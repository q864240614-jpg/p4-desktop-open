#include "bambu_app.h"
#include "bambu_config.h"
#include "esp_crt_bundle.h"
#include <time.h>
#include "wifi_credentials.h"
#include "portable_ui.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "lwip/sockets.h"
#include "mbedtls/base64.h"

typedef struct {
    QueueHandle_t queue;
    BambuState state;
    esp_mqtt_client_handle_t mqtt;
    const BambuPrinterConfig *config;
    char report_topic[80], request_topic[80];
    int connection_error;
    BambuFrame frame;
    int64_t last_report, next_start, offline_since;
} PrinterConnection;
static PrinterConnection printers[2];
static BambuConfig config;
static QueueHandle_t network_queue;
static SemaphoreHandle_t state_lock;
static char authorization[80], pin[12];
static int64_t restart_at;
static bool wifi_up;
static void network(const char *text)
{ char message[96];snprintf(message,sizeof(message),"%s",text);xQueueOverwrite(network_queue,message); }
static void publish_state(PrinterConnection *c) { xQueueOverwrite(c->queue,&c->state); }
static void request_status(PrinterConnection *c)
{
    if(c->mqtt) esp_mqtt_client_publish(c->mqtt,c->request_topic,
        "{\"pushing\":{\"sequence_id\":\"0\",\"command\":\"pushall\",\"version\":1,\"push_target\":1}}",0,0,0);
}
static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    PrinterConnection *c=arg;(void)base;esp_mqtt_event_handle_t e=data;
    xSemaphoreTake(state_lock,portMAX_DELAY);
    if(id==MQTT_EVENT_CONNECTED) {
        c->connection_error=0;c->offline_since=0;
        esp_mqtt_client_subscribe(c->mqtt,c->report_topic,0);c->last_report=esp_timer_get_time();
    } else if(id==MQTT_EVENT_SUBSCRIBED) request_status(c);
    else if(id==MQTT_EVENT_DISCONNECTED || id==MQTT_EVENT_ERROR) {
        c->connection_error=-1;
        if(id==MQTT_EVENT_ERROR && e->error_handle && e->error_handle->error_type==MQTT_ERROR_TYPE_CONNECTION_REFUSED)
            c->connection_error=e->error_handle->connect_return_code;
        BambuFrame_Reset(&c->frame);
        /* Camera start can briefly drop LAN MQTT. Wait before CONNECTION LOST. */
        if(c->connection_error==4 || c->connection_error==5) {
            c->state.online=false;c->offline_since=0;publish_state(c);
        } else if(!c->offline_since)c->offline_since=esp_timer_get_time();
    } else if(id==MQTT_EVENT_DATA) {
        if(e->current_data_offset==0 && (e->topic_len!=(int)strlen(c->report_topic) || memcmp(e->topic,c->report_topic,e->topic_len))) {
            BambuFrame_Reset(&c->frame);
        } else if(e->total_data_len>0 && e->current_data_offset>=0 && e->data_len>0 &&
                  BambuFrame_Append(&c->frame,e->total_data_len,e->current_data_offset,e->data,e->data_len)) {
            BambuState parsed=c->state;
            xSemaphoreGive(state_lock);
            bool valid=BambuState_Parse(&parsed,c->frame.data,c->frame.size);
            xSemaphoreTake(state_lock,portMAX_DELAY);
            if(valid){parsed.online=wifi_up;c->offline_since=0;c->state=parsed;c->last_report=esp_timer_get_time();publish_state(c);}
            BambuFrame_Reset(&c->frame);
        }
    }
    xSemaphoreGive(state_lock);
}
static void mqtt_start(PrinterConnection *c)
{
    const BambuPrinterConfig *p=c->config;
    if(c->mqtt || p->mode==BAMBU_OFF || esp_timer_get_time()<c->next_start)return;
    const bool cloud=p->mode==BAMBU_CLOUD;
    /* Cloud certificate validity requires SNTP time, unlike the local self-signed broker. */
    if(cloud && time(NULL)<1704067200)return;
    snprintf(c->report_topic,sizeof(c->report_topic),"device/%s/report",p->serial);
    snprintf(c->request_topic,sizeof(c->request_topic),"device/%s/request",p->serial);
    char client_id[40];snprintf(client_id,sizeof(client_id),"p4_%08lx_%d",(unsigned long)esp_random(),(int)(c-printers));
    esp_mqtt_client_config_t cfg={
        .broker.address.hostname=cloud?BambuConfig_Broker(&config):p->ip,
        .broker.address.port=8883,.broker.address.transport=MQTT_TRANSPORT_OVER_SSL,
        .credentials.client_id=client_id,.credentials.username=cloud?config.username:"bblp",
        .credentials.authentication.password=cloud?config.token:p->code,
        .broker.verification.crt_bundle_attach=cloud?esp_crt_bundle_attach:NULL,
        .broker.verification.skip_cert_common_name_check=!cloud,
        .buffer.size=4096,.task.stack_size=8192,.session.keepalive=45,
        .network.timeout_ms=15000,.network.reconnect_timeout_ms=3000,
    };
    c->mqtt=esp_mqtt_client_init(&cfg);
    esp_err_t err=c->mqtt?esp_mqtt_client_register_event(c->mqtt,ESP_EVENT_ANY_ID,mqtt_event,c):ESP_ERR_NO_MEM;
    if(err==ESP_OK)err=esp_mqtt_client_start(c->mqtt);
    if(err!=ESP_OK){
        if(c->mqtt){esp_mqtt_client_destroy(c->mqtt);c->mqtt=NULL;}
        c->next_start=esp_timer_get_time()+10000000;
        ESP_LOGE("bambu_mqtt","Start failed: %s; next attempt in 10 seconds",esp_err_to_name(err));
    }
}
static bool authorized(httpd_req_t *r)
{
    char header[80];
    if(httpd_req_get_hdr_value_str(r,"Authorization",header,sizeof(header))==ESP_OK && !strcmp(header,authorization)) return true;
    httpd_resp_set_status(r,"401 Unauthorized");httpd_resp_set_hdr(r,"WWW-Authenticate","Basic realm=\"P4 - user admin, PIN on display\"");
    httpd_resp_sendstr(r,"Use admin and the PIN on the P4 display.");return false;
}
extern const char settings_html[] asm("_binary_printer_settings_html_start");
static esp_err_t settings_get(httpd_req_t *r)
{
    if(!authorized(r))return ESP_OK;
    httpd_resp_set_type(r,"text/html; charset=utf-8");httpd_resp_set_hdr(r,"Cache-Control","no-store");
    return httpd_resp_sendstr(r,settings_html);
}
static esp_err_t settings_read(httpd_req_t *r)
{
    if(!authorized(r))return ESP_OK;
    char *json=BambuConfig_JSON(&config,false);
    if(!json)return httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"Not enough memory");
    cJSON *j=cJSON_Parse(json);free(json);
    if(!j)return httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"Not enough memory");
    xSemaphoreTake(state_lock,portMAX_DELAY);
    for(int i=0;i<2;i++) {
        PrinterConnection *c=&printers[i];char key[16];snprintf(key,sizeof(key),"status%d",i);
        const char *status=c->config->mode==BAMBU_OFF?"已停用":c->state.online?"在线 · 正在接收打印状态":
            c->connection_error==4 || c->connection_error==5?"认证失败 · 检查区域和令牌 / 访问码":
            !wifi_up?"等待 Wi-Fi":c->config->mode==BAMBU_CLOUD && time(NULL)<1704067200?"等待网络校时":
            c->connection_error?"连接失败 · 正在重连":"等待打印机状态";
        if(!cJSON_AddStringToObject(j,key,status)){
            xSemaphoreGive(state_lock);cJSON_Delete(j);
            return httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"Not enough memory");
        }
    }
    xSemaphoreGive(state_lock);
    json=cJSON_PrintUnformatted(j);cJSON_Delete(j);
    if(!json)return httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"Not enough memory");
    httpd_resp_set_type(r,"application/json");httpd_resp_set_hdr(r,"Cache-Control","no-store");
    esp_err_t result=httpd_resp_sendstr(r,json);free(json);return result;
}
static esp_err_t settings_post(httpd_req_t *r)
{
    if(!authorized(r))return ESP_OK;
    char type[40];
    if(httpd_req_get_hdr_value_str(r,"Content-Type",type,sizeof(type))!=ESP_OK || strcmp(type,"application/json") || r->content_len<=0 || r->content_len>=BAMBU_CONFIG_SIZE)
        return httpd_resp_send_err(r,HTTPD_400_BAD_REQUEST,"Invalid request");
    char *body=malloc(r->content_len+1);
    if(!body)return httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"Not enough memory");
    int n=0;
    while(n<r->content_len){int count=httpd_req_recv(r,body+n,r->content_len-n);if(count<=0){free(body);return ESP_FAIL;}n+=count;}body[n]=0;
    BambuConfig *next=malloc(sizeof(*next));
    if(!next){free(body);return httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"Not enough memory");}
    char error[128];
    bool valid=BambuConfig_Parse(body,&config,next,error,sizeof(error));free(body);
    if(!valid){free(next);return httpd_resp_send_err(r,HTTPD_400_BAD_REQUEST,error);}
    char *json=BambuConfig_JSON(next,true);free(next);
    if(!json)return httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"Not enough memory");
    nvs_handle_t nvs;esp_err_t err=nvs_open("bambu",NVS_READWRITE,&nvs);
    if(err==ESP_OK){err=nvs_set_str(nvs,"config",json);if(err==ESP_OK)err=nvs_commit(nvs);nvs_close(nvs);}free(json);
    if(err!=ESP_OK)return httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"Could not save settings");
    xSemaphoreTake(state_lock,portMAX_DELAY);restart_at=esp_timer_get_time()+2000000;xSemaphoreGive(state_lock);
    return httpd_resp_sendstr(r,"Saved. P4 restarting; the display will show a new settings PIN.");
}
static void wifi_event(void *arg,esp_event_base_t base,int32_t id,void *data)
{
    (void)arg;
    if(base==WIFI_EVENT && id==WIFI_EVENT_STA_START) esp_wifi_connect();
    else if(base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED){
        ESP_LOGW("p4_wifi", "Disconnected: reason=%u", ((wifi_event_sta_disconnected_t *)data)->reason);
        xSemaphoreTake(state_lock,portMAX_DELAY);wifi_up=false;
        for(int i=0;i<2;i++){printers[i].state.online=false;publish_state(&printers[i]);}
        xSemaphoreGive(state_lock);
        network("Wi-Fi reconnecting");
    } else if(base==IP_EVENT && id==IP_EVENT_STA_GOT_IP){
        ip_event_got_ip_t *e=data;ESP_LOGI("p4_wifi", "Connected: " IPSTR, IP2STR(&e->ip_info.ip));char text[96];snprintf(text,sizeof(text),IPSTR " PIN %s",IP2STR(&e->ip_info.ip),pin);network(text);
        xSemaphoreTake(state_lock,portMAX_DELAY);wifi_up=true;xSemaphoreGive(state_lock);
    }
}
static void network_task(void *arg)
{
    (void)arg;
    nvs_handle_t nvs;
    if(nvs_open("bambu",NVS_READONLY,&nvs)==ESP_OK){
        size_t len=0;
        if(nvs_get_str(nvs,"config",NULL,&len)==ESP_OK && len<=BAMBU_CONFIG_SIZE){
            char *body=malloc(len);configASSERT(body);char error[128];
            if(nvs_get_str(nvs,"config",body,&len)==ESP_OK && !BambuConfig_Parse(body,NULL,&config,error,sizeof(error)))
                ESP_LOGW("p4_wifi","Saved printer settings: %s",error);
            free(body);
        }nvs_close(nvs);
    }
    snprintf(pin,sizeof(pin),"%06u",(unsigned)(esp_random()%1000000));
    char plain[32];snprintf(plain,sizeof(plain),"admin:%s",pin);size_t written;
    strcpy(authorization,"Basic ");mbedtls_base64_encode((unsigned char *)authorization+6,sizeof(authorization)-6,&written,(unsigned char *)plain,strlen(plain));
    ESP_ERROR_CHECK(esp_netif_init());ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_event,NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,wifi_event,NULL));
    wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_config_t config={.sta={.ssid=P4_WIFI_SSID,.password=P4_WIFI_PASSWORD}};
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA,&config));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);esp_sntp_setservername(0,"pool.ntp.org");esp_sntp_init();
    httpd_handle_t server;httpd_config_t http=HTTPD_DEFAULT_CONFIG();http.stack_size=16384;
    if(httpd_start(&server,&http)==ESP_OK){
        httpd_uri_t read={.uri="/settings",.method=HTTP_GET,.handler=settings_read};
        httpd_register_uri_handler(server,&read);
        httpd_uri_t get={.uri="/",.method=HTTP_GET,.handler=settings_get},post={.uri="/settings",.method=HTTP_POST,.handler=settings_post};
        httpd_register_uri_handler(server,&get);httpd_register_uri_handler(server,&post);
    }
    unsigned seconds=0;
    for(;;){
        vTaskDelay(pdMS_TO_TICKS(1000));seconds++;
        xSemaphoreTake(state_lock,portMAX_DELAY);
        bool up=wifi_up;int64_t now=esp_timer_get_time();bool restart=restart_at && now>=restart_at;
        for(int i=0;i<2;i++) {
            PrinterConnection *c=&printers[i];
            if(c->offline_since && now-c->offline_since>15000000 && c->state.online) {
                c->state.online=false;c->offline_since=0;publish_state(c);
            }
            if(c->state.online && now-c->last_report>90000000){c->state.online=false;publish_state(c);}
        }
        xSemaphoreGive(state_lock);
        if(restart)esp_restart();
        if(!up && seconds%10==0)esp_wifi_connect();
        if(up) for(int i=0;i<2;i++) {
            mqtt_start(&printers[i]);
            if(seconds%60==0)request_status(&printers[i]);
        }
    }
}
void bambu_app_start(void)
{
    network_queue=xQueueCreate(1,96);state_lock=xSemaphoreCreateMutex();
    configASSERT(network_queue && state_lock);
    for(int i=0;i<2;i++) {
        printers[i].config=&config.printers[i];
        printers[i].queue=xQueueCreate(1,sizeof(BambuState));configASSERT(printers[i].queue);
        BambuState_Init(&printers[i].state);publish_state(&printers[i]);
    }
    configASSERT(xTaskCreate(network_task,"bambu_net",12288,NULL,4,NULL)==pdPASS);
}
void bambu_app_poll(void)
{
    if(!network_queue) return;
    BambuState s;char message[96];
    for(int i=0;i<2;i++) if(xQueueReceive(printers[i].queue,&s,0)==pdTRUE)PortableUI_SetPrinterSlot(i,&s);
    if(xQueueReceive(network_queue,message,0)==pdTRUE)PortableUI_SetNetwork(message);
}
