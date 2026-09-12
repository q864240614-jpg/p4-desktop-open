#include "codex_app.h"
#include "codex_credentials.h"
#include "portable_ui.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#define RESPONSE_SIZE 16384
static QueueHandle_t snapshots;
typedef struct { char *data; size_t used; bool overflow; } Response;
static esp_err_t receive(esp_http_client_event_t *event)
{
    Response *r=event->user_data;
    if(event->event_id==HTTP_EVENT_ON_DATA && event->data_len>0) {
        if((size_t)event->data_len>RESPONSE_SIZE-1-r->used){r->overflow=true;return ESP_FAIL;}
        memcpy(r->data+r->used,event->data,event->data_len);r->used+=event->data_len;
    }
    return ESP_OK;
}
static void worker(void *arg)
{
    (void)arg;
    PortableUI_Codex state={.remaining={NAN,NAN}};
    Response response={.data=malloc(RESPONSE_SIZE)};configASSERT(response.data);
    vTaskDelay(pdMS_TO_TICKS(1000));
    for(;;) {
        response.used=0;response.overflow=false;
        esp_http_client_config_t cfg={.url=CODEX_STATUS_URL,.timeout_ms=5000,.event_handler=receive,.user_data=&response};
        esp_http_client_handle_t client=esp_http_client_init(&cfg);configASSERT(client);
        esp_http_client_set_header(client,"Authorization",CODEX_STATUS_AUTH);
        bool ok=esp_http_client_perform(client)==ESP_OK && esp_http_client_get_status_code(client)==200 && !response.overflow;
        response.data[response.used]=0;
        esp_http_client_cleanup(client);
        PortableUI_ParseCodexJSON(ok?response.data:NULL,&state);
        xQueueOverwrite(snapshots,&state);
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}
void codex_app_start(void)
{
    snapshots=xQueueCreate(1,sizeof(PortableUI_Codex));configASSERT(snapshots);
    configASSERT(xTaskCreate(worker,"codex_quota",6144,NULL,3,NULL)==pdPASS);
}
void codex_app_poll(void)
{
    PortableUI_Codex state;
    if(snapshots && xQueueReceive(snapshots,&state,0)==pdTRUE)PortableUI_SetCodex(&state);
}
