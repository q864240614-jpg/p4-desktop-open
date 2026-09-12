#include "bambu_video.h"
#include "bambu_config.h"
#include "video_relay.h"
#include "portable_ui.h"
#include "p4_display.h"
#include "driver/jpeg_decode.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "mbedtls/ssl.h"
#include "mbedtls/error.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/net_sockets.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define JPEG_LIMIT (512*1024)
#define VIDEO_WIDTH PORTABLE_UI_VIDEO_WIDTH
#define VIDEO_HEIGHT PORTABLE_UI_VIDEO_HEIGHT
#define VIDEO_PIXELS (VIDEO_WIDTH*VIDEO_HEIGHT)
typedef struct { unsigned generation; int slot; } VideoRequest;
typedef struct { unsigned generation; const uint16_t *pixels; char error[96]; } VideoResult;
static QueueHandle_t requests,results;
static QueueHandle_t free_frames;
static atomic_uint generation;

static bool current(unsigned id) { return id==atomic_load(&generation); }
static void request(int slot)
{
    VideoRequest r={.generation=atomic_fetch_add(&generation,1)+1,.slot=slot};
    xQueueOverwrite(requests,&r);
}
static void report(unsigned id,const uint16_t *pixels,const char *error)
{
    VideoResult result={.generation=id,.pixels=pixels};
    if(error) {
        snprintf(result.error,sizeof(result.error),"%s",error);
        ESP_LOGI("bambu_video","%s",error);
    }
    if(xQueueSend(results,&result,pdMS_TO_TICKS(200))==pdTRUE)return;
    if(pixels) {
        uint16_t *frame=(uint16_t *)pixels;
        xQueueSend(free_frames,&frame,0);
    }
}
static void release_frame(const uint16_t *pixels)
{
    if(xQueueSend(free_frames,&pixels,0)!=pdTRUE)
        ESP_LOGW("bambu_video","frame pool full");
}
static uint16_t *acquire_frame(unsigned id)
{
    uint16_t *pixels;
    while(current(id))if(xQueueReceive(free_frames,&pixels,pdMS_TO_TICKS(25))==pdTRUE)return pixels;
    return NULL;
}
static bool load_config(BambuConfig *config)
{
    nvs_handle_t nvs;
    esp_err_t err=nvs_open("bambu",NVS_READONLY,&nvs);
    if(err==ESP_ERR_NVS_NOT_FOUND)return false;
    ESP_ERROR_CHECK(err);
    char *json=malloc(BAMBU_CONFIG_SIZE);configASSERT(json);
    size_t length=BAMBU_CONFIG_SIZE;err=nvs_get_str(nvs,"config",json,&length);
    nvs_close(nvs);
    if(err!=ESP_OK){free(json);return false;}
    char error[128];bool ok=BambuConfig_Parse(json,NULL,config,error,sizeof(error));
    free(json);return ok;
}

/* The local camera uses its printer-generated, self-signed TLS certificate.
 * Verification is disabled only in this private LAN session; no global ESP-TLS
 * setting is changed. Cloud MQTT and HTTPS continue to verify certificates. */
typedef struct {
    int fd;
    bool handshake,tls_ready;
    uint64_t rx_bytes,tx_bytes;
    int socket_errno;
    char error[96];
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config cfg;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context rng;
} CameraTLS;
static int camera_send(void *ctx,const unsigned char *data,size_t length)
{
    CameraTLS *c=ctx;
    int n=send(c->fd,data,length,0);
    if(n>0)c->tx_bytes+=n;
    if(n<0)c->socket_errno=errno;
    return n>=0?n:errno==EAGAIN || errno==EWOULDBLOCK?MBEDTLS_ERR_SSL_WANT_WRITE:MBEDTLS_ERR_NET_SEND_FAILED;
}
static int camera_recv(void *ctx,unsigned char *data,size_t length)
{
    CameraTLS *c=ctx;
    int n=recv(c->fd,data,length,0);
    if(n>0)c->rx_bytes+=n;
    if(n<0)c->socket_errno=errno;
    return n>=0?n:errno==EAGAIN || errno==EWOULDBLOCK?MBEDTLS_ERR_SSL_WANT_READ:MBEDTLS_ERR_NET_RECV_FAILED;
}
static bool tls_wait(CameraTLS *c,int result,unsigned id,int64_t deadline)
{
    if((result!=MBEDTLS_ERR_SSL_WANT_READ && result!=MBEDTLS_ERR_SSL_WANT_WRITE) ||
       !current(id) || esp_timer_get_time()>=deadline)return false;
    fd_set readable,writable;FD_ZERO(&readable);FD_ZERO(&writable);
    if(result==MBEDTLS_ERR_SSL_WANT_READ)FD_SET(c->fd,&readable);
    else FD_SET(c->fd,&writable);
    struct timeval timeout={.tv_usec=20000};
    int ready=select(c->fd+1,&readable,&writable,NULL,&timeout);
    if(ready<0)c->socket_errno=errno;
    return ready>=0 && current(id);
}
static bool camera_open(CameraTLS *c,const char *ip,int port,unsigned id)
{
    c->handshake=false;c->error[0]=0;c->rx_bytes=c->tx_bytes=0;c->socket_errno=0;
    mbedtls_ssl_init(&c->ssl);mbedtls_ssl_config_init(&c->cfg);
    mbedtls_entropy_init(&c->entropy);mbedtls_ctr_drbg_init(&c->rng);
    c->tls_ready=true;
    c->fd=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(c->fd<0)return false;
    int nodelay=1;setsockopt(c->fd,IPPROTO_TCP,TCP_NODELAY,&nodelay,sizeof(nodelay));
    if(fcntl(c->fd,F_SETFL,O_NONBLOCK)<0)return false;
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(port)};
    inet_pton(AF_INET,ip,&address.sin_addr);
    int result=connect(c->fd,(struct sockaddr *)&address,sizeof(address));
    if(result<0 && errno!=EINPROGRESS)return false;
    int64_t deadline=esp_timer_get_time()+5000000;
    while(result<0 && current(id) && esp_timer_get_time()<deadline) {
        fd_set writable;FD_ZERO(&writable);FD_SET(c->fd,&writable);
        struct timeval timeout={.tv_usec=200000};
        int ready=select(c->fd+1,NULL,&writable,NULL,&timeout);
        if(ready<0)return false;
        if(ready) {
            int error;socklen_t size=sizeof(error);
            if(getsockopt(c->fd,SOL_SOCKET,SO_ERROR,&error,&size)<0 || error)return false;
            result=0;
        }
    }
    if(result<0 || !current(id))return false;
    if(mbedtls_ctr_drbg_seed(&c->rng,mbedtls_entropy_func,&c->entropy,NULL,0) ||
       mbedtls_ssl_config_defaults(&c->cfg,MBEDTLS_SSL_IS_CLIENT,MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT))return false;
    mbedtls_ssl_conf_authmode(&c->cfg,MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&c->cfg,mbedtls_ctr_drbg_random,&c->rng);
    if(mbedtls_ssl_setup(&c->ssl,&c->cfg))return false;
    mbedtls_ssl_set_bio(&c->ssl,c,camera_send,camera_recv,NULL);
    deadline=esp_timer_get_time()+5000000;
    while((result=mbedtls_ssl_handshake(&c->ssl))!=0)
        if(!tls_wait(c,result,id,deadline))return false;
    if(!current(id))return false;
    c->handshake=true;
    return true;
}
static void camera_close(CameraTLS *c)
{
    if(c->fd>=0){
        uint8_t dump[256];
        for(int i=0;i<24;i++)if(recv(c->fd,dump,sizeof dump,0)<=0)break;
        close(c->fd);c->fd=-1;
    }
    if(c->tls_ready){
        mbedtls_ssl_free(&c->ssl);mbedtls_ssl_config_free(&c->cfg);
        mbedtls_ctr_drbg_free(&c->rng);mbedtls_entropy_free(&c->entropy);
        c->tls_ready=false;
    }
    c->handshake=false;
}
static bool camera_transfer(CameraTLS *c,uint8_t *data,size_t size,bool write,unsigned id)
{
    size_t used=0;const int64_t started=esp_timer_get_time(),deadline=started+15000000;
    const uint64_t start_rx=c->rx_bytes,start_tx=c->tx_bytes;
    int n=0,glitch=0;bool failed=false;
    while(used<size && current(id)) {
        n=write?mbedtls_ssl_write(&c->ssl,data+used,size-used):mbedtls_ssl_read(&c->ssl,data+used,size-used);
        if(n>0){used+=n;glitch=0;}
        else if(n==MBEDTLS_ERR_SSL_WANT_READ || n==MBEDTLS_ERR_SSL_WANT_WRITE) {
            if(!tls_wait(c,n,id,deadline)){failed=true;break;}
        } else if(n==MBEDTLS_ERR_ERROR_GENERIC_ERROR && ++glitch<8) {
            vTaskDelay(1);
        } else {failed=true;break;}
        if(esp_timer_get_time()>=deadline){failed=true;break;}
    }
    if(!current(id))return false; // User cancellation is not a transport fault.
    if(!failed && used==size)return true;
    const char *operation=write?"write":"read";
    if(esp_timer_get_time()>=deadline)
        snprintf(c->error,sizeof(c->error),"Camera %s timed out (15s)",operation);
    else if(n==0 || n==MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
        snprintf(c->error,sizeof(c->error),"Camera peer closed connection");
    else if(n==MBEDTLS_ERR_SSL_WANT_READ || n==MBEDTLS_ERR_SSL_WANT_WRITE)
        snprintf(c->error,sizeof(c->error),"Camera socket wait error: %d",c->socket_errno);
    else
        snprintf(c->error,sizeof(c->error),"Camera TLS %s error: -0x%x",operation,(unsigned)-n);
    ESP_LOGE("bambu_video","%s; payload %u/%u, elapsed %lld ms, TLS rc %d, last socket errno %d, wire RX/TX %llu/%llu bytes; internal free/largest %u/%u",
        c->error,(unsigned)used,(unsigned)size,(long long)((esp_timer_get_time()-started)/1000),n,c->socket_errno,
        (unsigned long long)(c->rx_bytes-start_rx),(unsigned long long)(c->tx_bytes-start_tx),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    return false;
}
static bool camera_auth(CameraTLS *c,const char *code,unsigned id)
{
    uint8_t packet[80]={0x40,0,0,0,0,0x30};
    memcpy(packet+16,"bblp",4);memcpy(packet+48,code,strlen(code));
    return camera_transfer(c,packet,sizeof(packet),true,id);
}
static size_t camera_frame(CameraTLS *c,uint8_t *jpeg,unsigned id)
{
    uint8_t header[16];
    if(!camera_transfer(c,header,sizeof(header),false,id))return 0;
    uint32_t size=(uint32_t)header[0]|(uint32_t)header[1]<<8|(uint32_t)header[2]<<16|(uint32_t)header[3]<<24;
    if(size<4 || size>JPEG_LIMIT) {
        snprintf(c->error,sizeof(c->error),"Camera frame size invalid");
        return 0;
    }
    return camera_transfer(c,jpeg,size,false,id)?size:0;
}
static bool decode(jpeg_decoder_handle_t decoder,const uint8_t *jpeg,size_t size,
                   uint8_t *raw,size_t capacity,uint16_t *scaled,char error[96])
{
    if(size<4 || jpeg[0]!=0xff || jpeg[1]!=0xd8) {
        snprintf(error,96,"JPEG missing SOI (%u bytes)",(unsigned)size);return false;
    }
    while(size>=4 && (jpeg[size-2]!=0xff || jpeg[size-1]!=0xd9))size--;
    if(size<4){snprintf(error,96,"JPEG missing EOI");return false;}
    jpeg_decode_picture_info_t info;
    esp_err_t status=jpeg_decoder_get_info(jpeg,size,&info);
    if(status!=ESP_OK) {snprintf(error,96,"JPEG header: %s",esp_err_to_name(status));return false;}
    if(!info.width || !info.height || info.width>1920 || info.height>1088) {
        snprintf(error,96,"JPEG size unsupported: %ux%u",(unsigned)info.width,(unsigned)info.height);return false;
    }
    jpeg_decode_cfg_t cfg={.output_format=JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order=JPEG_DEC_RGB_ELEMENT_ORDER_BGR,.conv_std=JPEG_YUV_RGB_CONV_STD_BT601};
    uint32_t used;
    p4_display_dma_take();
    status=jpeg_decoder_process(decoder,&cfg,jpeg,size,raw,capacity,&used);
    p4_display_dma_give();
    if(status!=ESP_OK) {
        snprintf(error,96,"JPEG decode: %s",esp_err_to_name(status));
        return false;
    }
    unsigned stride=(info.width+15)&~15U;
    const uint16_t *pixels=(const uint16_t *)raw;
    if(info.width==VIDEO_WIDTH && info.height==VIDEO_HEIGHT) {
        for(unsigned y=0;y<VIDEO_HEIGHT;y++)
            memcpy(scaled+y*VIDEO_WIDTH,pixels+y*stride,VIDEO_WIDTH*2);
        return true;
    }
    unsigned crop_width=info.width,crop_height=info.height;
    if(info.width*VIDEO_HEIGHT>info.height*VIDEO_WIDTH)crop_width=info.height*VIDEO_WIDTH/VIDEO_HEIGHT;
    else crop_height=info.width*VIDEO_HEIGHT/VIDEO_WIDTH;
    unsigned left=(info.width-crop_width)/2,top=(info.height-crop_height)/2;
    static unsigned source_x[VIDEO_WIDTH];
    for(unsigned x=0;x<VIDEO_WIDTH;x++)source_x[x]=left+x*crop_width/VIDEO_WIDTH;
    for(unsigned y=0;y<VIDEO_HEIGHT;y++){
        const uint16_t *row=pixels+(top+y*crop_height/VIDEO_HEIGHT)*stride;
        for(unsigned x=0;x<VIDEO_WIDTH;x++)scaled[y*VIDEO_WIDTH+x]=row[source_x[x]];
    }
    return true;
}
#include "bambu_video_rtsp.inc"

typedef struct { int fd; char error[96]; } CameraTCP;
static bool tcp_wait(CameraTCP *c,bool write,unsigned id,int64_t deadline)
{
    if(!current(id) || esp_timer_get_time()>=deadline)return false;
    fd_set set;FD_ZERO(&set);FD_SET(c->fd,&set);
    struct timeval timeout={.tv_usec=20000};
    int ready=write?select(c->fd+1,NULL,&set,NULL,&timeout):select(c->fd+1,&set,NULL,NULL,&timeout);
    if(ready<0)return false;
    return current(id);
}
static bool tcp_open(CameraTCP *c,const char *ip,int port,unsigned id)
{
    c->fd=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);c->error[0]=0;
    if(c->fd<0)return false;
    int nodelay=1;setsockopt(c->fd,IPPROTO_TCP,TCP_NODELAY,&nodelay,sizeof(nodelay));
    if(fcntl(c->fd,F_SETFL,O_NONBLOCK)<0)return false;
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(port)};
    inet_pton(AF_INET,ip,&address.sin_addr);
    int result=connect(c->fd,(struct sockaddr *)&address,sizeof(address));
    if(result<0 && errno!=EINPROGRESS)return false;
    int64_t deadline=esp_timer_get_time()+5000000;
    while(result<0 && current(id) && esp_timer_get_time()<deadline) {
        fd_set writable;FD_ZERO(&writable);FD_SET(c->fd,&writable);
        struct timeval timeout={.tv_usec=200000};
        int ready=select(c->fd+1,NULL,&writable,NULL,&timeout);
        if(ready<0)return false;
        if(ready) {
            int error;socklen_t size=sizeof(error);
            if(getsockopt(c->fd,SOL_SOCKET,SO_ERROR,&error,&size)<0 || error)return false;
            result=0;
        }
    }
    return result==0 && current(id);
}
static void tcp_close(CameraTCP *c)
{
    if(c->fd>=0)close(c->fd);
    c->fd=-1;
}
static bool tcp_xfer(CameraTCP *c,uint8_t *data,size_t size,bool write,unsigned id,int64_t idle_us)
{
    size_t used=0;int64_t deadline=esp_timer_get_time()+idle_us;
    while(used<size && current(id)) {
        int n=write?send(c->fd,data+used,size-used,0):recv(c->fd,data+used,size-used,0);
        if(n>0){used+=n;deadline=esp_timer_get_time()+idle_us;}
        else if(n==0){snprintf(c->error,sizeof(c->error),"Relay closed the stream");return false;}
        else if(errno!=EAGAIN && errno!=EWOULDBLOCK){snprintf(c->error,sizeof(c->error),"Relay socket error");return false;}
        else if(!tcp_wait(c,write,id,deadline))break;
        if(esp_timer_get_time()>=deadline)break;
    }
    if(!current(id))return false;
    if(used==size)return true;
    snprintf(c->error,sizeof(c->error),"Relay %s timed out",write?"write":"read");
    return false;
}
static bool relay_code_ok(const char *code)
{
    size_t n=strlen(code);if(n<4 || n>32)return false;
    for(;*code;code++)if(!isalnum((unsigned char)*code))return false;
    return true;
}
static jpeg_decoder_handle_t video_decoder;
static uint8_t *video_jpeg,*video_raw;
static size_t video_jpeg_cap,video_raw_cap;
/* Create the JPEG engine once, before any TLS session. jpeg_del_decoder_engine
 * on ESP32-P4 leaves dma2d held, so a later jpeg_new fails with
 * "JPEG decoder could not start". Internal DMA descriptors also fail if
 * mbedtls has already fragmented internal RAM. */
static bool video_codec_ready(char error[96])
{
    if(!video_decoder) {
        jpeg_decode_engine_cfg_t cfg={.timeout_ms=2000};
        esp_err_t status=jpeg_new_decoder_engine(&cfg,&video_decoder);
        if(status!=ESP_OK) {
            video_decoder=NULL;
            snprintf(error,96,"JPEG decoder could not start (%s)",esp_err_to_name(status));
            return false;
        }
        ESP_LOGI("bambu_video","JPEG decoder ready");
    }
    if(!video_jpeg) {
        jpeg_decode_memory_alloc_cfg_t in={.buffer_direction=JPEG_DEC_ALLOC_INPUT_BUFFER};
        video_jpeg=jpeg_alloc_decoder_mem(JPEG_LIMIT,&in,&video_jpeg_cap);
    }
    if(!video_raw) {
        jpeg_decode_memory_alloc_cfg_t out={.buffer_direction=JPEG_DEC_ALLOC_OUTPUT_BUFFER};
        video_raw=jpeg_alloc_decoder_mem(1920*1088*2,&out,&video_raw_cap);
    }
    if(!video_jpeg || !video_raw) {
        snprintf(error,96,"Not enough memory for camera");
        return false;
    }
    return true;
}
static void relay_error_line(CameraTCP *relay,unsigned id,const uint8_t header[16],char last_error[96])
{
    char msg[96];
    memcpy(msg,header,16);
    size_t n=16;
    while(n<sizeof(msg)-1 && !memchr(msg,'\n',n)) {
        if(!tcp_xfer(relay,(uint8_t *)msg+n,1,false,id,1000000))break;
        n++;
    }
    msg[n]=0;
    char *text=msg;
    if(!strncmp(text,"ERR ",4))text+=4;
    for(char *p=text;*p;p++)if(*p=='\r' || *p=='\n'){*p=0;break;}
    snprintf(last_error,96,"Host relay: %.80s",text[0]?text:"failed");
}
/* H2D only. Never opens the printer camera from this board. */
static void play_relay(const BambuPrinterConfig *p,unsigned id)
{
    CameraTCP relay={.fd=-1};
    if(!tcp_open(&relay,VIDEO_RELAY_IP,VIDEO_RELAY_PORT,id)) {
        tcp_close(&relay);
        if(current(id))report(id,NULL,"Host relay unreachable");
        return;
    }
    char line[192];
    int n=snprintf(line,sizeof(line),
        "{\"ip\":\"%s\",\"access_code\":\"%s\",\"model\":\"H2D\",\"width\":800,\"height\":480,\"fps\":8}\n",
        p->ip,p->code);
    if(n<0 || n>=(int)sizeof(line) || !tcp_xfer(&relay,(uint8_t *)line,(size_t)n,true,id,8000000)) {
        tcp_close(&relay);
        if(current(id))report(id,NULL,relay.error[0]?relay.error:"Host relay request failed");
        return;
    }
    if(current(id))report(id,NULL,"WAITING FOR CAMERA...");
    bool streamed=false;
    char last_error[96]="Host relay: no picture";
    int bad=0;
    while(current(id)) {
        uint8_t header[16];
        if(!tcp_xfer(&relay,header,sizeof(header),false,id,streamed?8000000:60000000)) {
            if(relay.error[0])snprintf(last_error,sizeof(last_error),"%s",relay.error);
            break;
        }
        if(header[0]=='E' && header[1]=='R') {
            relay_error_line(&relay,id,header,last_error);
            break;
        }
        uint32_t size=(uint32_t)header[0]|(uint32_t)header[1]<<8|(uint32_t)header[2]<<16|(uint32_t)header[3]<<24;
        if(!video_codec_ready(last_error))break;
        if(size<4 || size>JPEG_LIMIT || !tcp_xfer(&relay,video_jpeg,size,false,id,8000000)) {
            if(relay.error[0])snprintf(last_error,sizeof(last_error),"%s",relay.error);
            break;
        }
        uint16_t *scaled=acquire_frame(id);
        if(!scaled)break;
        char error[96];
        if(!decode(video_decoder,video_jpeg,size,video_raw,video_raw_cap,scaled,error)) {
            release_frame(scaled);
            snprintf(last_error,sizeof(last_error),"%s",error);
            if(++bad>=12)break;
            vTaskDelay(1);continue;
        }
        bad=0;
        if(!streamed){ESP_LOGI("bambu_video","Host relay JPEG ready");streamed=true;}
        report(id,scaled,NULL);
        vTaskDelay(1);
    }
    tcp_close(&relay);
    if(current(id) && !streamed)report(id,NULL,last_error);
}
static void play(VideoRequest request)
{
    unsigned id=request.generation;
    ESP_LOGI("bambu_video","Start %s camera",request.slot==0?"H2D":"A1 MINI");
    BambuConfig *config=malloc(sizeof(*config));configASSERT(config);
    if(!load_config(config) || config->printers[request.slot].mode==BAMBU_OFF) {
        report(id,NULL,"Configure this printer first");free(config);return;
    }
    const BambuPrinterConfig *p=&config->printers[request.slot];
    if(!p->ip[0] || !p->code[0] || (request.slot==1 && strlen(p->code)>32)) {
        report(id,NULL,"Set printer LAN IP / access code");free(config);return;
    }
    /* H2D never opens printer RTSPS from the board. A second camera session
     * fights the host relay and drops LAN MQTT (CONNECTION LOST). */
    if(request.slot==0) {
        if(!relay_code_ok(p->code)) {
            report(id,NULL,"LAN access code not usable on relay");free(config);return;
        }
        play_relay(p,id);free(config);return;
    }
    char codec_error[96];
    if(!video_codec_ready(codec_error)) {
        report(id,NULL,codec_error);free(config);return;
    }
    CameraTLS *camera=calloc(1,sizeof(*camera));
    if(!camera){report(id,NULL,"Not enough memory for camera");free(config);return;}
    camera->fd=-1;
    if(!camera_open(camera,p->ip,6000,id)) {
        report(id,NULL,"Camera connection failed / LAN liveview off");
        camera_close(camera);free(camera);free(config);return;
    }
    if(!camera_auth(camera,p->code,id)) {
        report(id,NULL,"Camera authentication failed");
        camera_close(camera);free(camera);free(config);return;
    }

    int64_t next_frame=0;
    bool first_frame=true,retried=false;
    while(current(id)) {
        size_t size=camera_frame(camera,video_jpeg,id);
        if(!size) {
            if(!retried && current(id) && strstr(camera->error,"-0x1")) {
                retried=true;
                camera_close(camera);
                vTaskDelay(pdMS_TO_TICKS(300));
                if(camera_open(camera,p->ip,6000,id) && camera_auth(camera,p->code,id))continue;
            }
            report(id,NULL,camera->error[0]?camera->error:"Camera stream ended / timed out");
            break;
        }
        if(!current(id))break;
        if(esp_timer_get_time()<next_frame){vTaskDelay(1);continue;}
        uint16_t *scaled=acquire_frame(id);
        if(!scaled)break;
        char error[96];
        if(!decode(video_decoder,video_jpeg,size,video_raw,video_raw_cap,scaled,error)) {
            release_frame(scaled);
            report(id,NULL,error);break;
        }
        if(first_frame){ESP_LOGI("bambu_video","A1 MINI JPEG decoded and ready");first_frame=false;}
        report(id,scaled,NULL);
        next_frame=esp_timer_get_time()+500000;
    }
    camera_close(camera);
    free(camera);
    vTaskDelay(pdMS_TO_TICKS(300));
    free(config);
}
static void worker(void *arg)
{
    (void)arg;
    char err[96];
    for(int i=0;i<5 && !video_decoder;i++) {
        if(video_codec_ready(err))break;
        ESP_LOGW("bambu_video","JPEG decoder start retry %d: %s",i+1,err);
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    VideoRequest r;
    for(;;) {
        xQueueReceive(requests,&r,portMAX_DELAY);
        if(r.slot>=0 && current(r.generation))play(r);
    }
}
void bambu_video_start(void)
{
    requests=xQueueCreate(1,sizeof(VideoRequest));results=xQueueCreate(1,sizeof(VideoResult));
    free_frames=xQueueCreate(2,sizeof(uint16_t *));configASSERT(requests && results && free_frames);
    for(int i=0;i<2;i++){
        uint16_t *pixels=heap_caps_aligned_alloc(128,VIDEO_PIXELS*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT|MALLOC_CAP_DMA);
        configASSERT(pixels);release_frame(pixels);
    }
    PortableUI_SetVideoHandler(request);
    configASSERT(xTaskCreatePinnedToCore(worker,"bambu_video",32768,NULL,2,NULL,1)==pdPASS);
}
void bambu_video_poll(void)
{
    VideoResult result;
    if(results && xQueueReceive(results,&result,0)==pdTRUE) {
        if(current(result.generation)) {
            if(result.pixels)PortableUI_SetVideoFrame(result.pixels,release_frame);
            else PortableUI_SetVideoStatus(result.error);
        }
        if(result.pixels && !current(result.generation))release_frame(result.pixels);
    }
}
