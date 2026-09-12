#include "todo_app.h"
#include "todo_credentials.h"
#include "portable_ui.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "cJSON.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static QueueHandle_t requests, snapshots, reminders, reminder_shown;
typedef struct { int page,done,id; } TodoRequest;
typedef struct { int id,date; char title[321]; } TodoReminder;
static bool request(int page,int done,int id)
{ TodoRequest r={page,done,id};return xQueueSend(requests,&r,0)==pdTRUE; }
static bool http(const char *path,bool post,char *body,size_t capacity)
{
    char url[160];snprintf(url,sizeof(url),"%s%s",TODO_URL,path);
    esp_http_client_config_t cfg={.url=url,.timeout_ms=4000,.method=post?HTTP_METHOD_POST:HTTP_METHOD_GET};
    esp_http_client_handle_t client=esp_http_client_init(&cfg);
    if(!client){ESP_LOGE("todo_http","Client allocation failed");return false;}
    esp_http_client_set_header(client,"Authorization",TODO_AUTH);
    if(post)esp_http_client_set_header(client,"Content-Type","application/json");
    bool ok=esp_http_client_open(client,post?2:0)==ESP_OK;
    if(ok && post)ok=esp_http_client_write(client,"{}",2)==2;
    if(ok)ok=esp_http_client_fetch_headers(client)>=0;
    int used=0,n=0;
    if(ok)while(used<(int)capacity-1 && (n=esp_http_client_read(client,body+used,capacity-1-used))>0)used+=n;
    body[used]=0;
    ok=ok && n>=0 && esp_http_client_is_complete_data_received(client) && esp_http_client_get_status_code(client)==200;
    esp_http_client_cleanup(client);return ok;
}
/* Scan OPEN independently of the visible page/filter. Web reminded_date belongs
 * to the browser; a separate NVS receipt records actual device presentation. */
typedef struct { char key[16]; bool open; } ReceiptKey;
static bool scan_reminders(nvs_handle_t receipts,char *body,TodoReminder *out,ReceiptKey *keys,size_t key_count)
{
    time_t now=time(NULL);
    struct tm local;localtime_r(&now,&local);
    if(local.tm_year<124)return false;
    int date=(local.tm_year+1900)*10000+(local.tm_mon+1)*100+local.tm_mday;
    int minute=local.tm_hour*60+local.tm_min;
    int pages=1;
    for(int page=0;page<pages;page++) {
        char path[80];snprintf(path,sizeof(path),"/api/todos?page=%d&done=0",page);
        if(!http(path,false,body,8192)) {
            ESP_LOGW("todo_reminder","Cannot read OPEN page %d",page);
            return false;
        }
        cJSON *root=cJSON_Parse(body);
        cJSON *items=cJSON_GetObjectItemCaseSensitive(root,"items");
        cJSON *page_count=cJSON_GetObjectItemCaseSensitive(root,"pages");
        if(!cJSON_IsArray(items) || !cJSON_IsNumber(page_count) ||
           page_count->valuedouble!=page_count->valueint || page_count->valueint<1) {
            ESP_LOGW("todo_reminder","Invalid OPEN page response");
            cJSON_Delete(root);return false;
        }
        pages=page_count->valueint;
        cJSON *item;
        cJSON_ArrayForEach(item,items) {
            cJSON *at=cJSON_GetObjectItemCaseSensitive(item,"remind_minute");
            cJSON *id=cJSON_GetObjectItemCaseSensitive(item,"id");
            if(!cJSON_IsNumber(id) || id->valuedouble!=id->valueint || id->valueint<=0){
                cJSON_Delete(root);return false;
            }
            char key[16];snprintf(key,sizeof(key),"t%d",id->valueint);
            for(size_t i=0;i<key_count;i++)if(!strcmp(keys[i].key,key))keys[i].open=true;
            if(cJSON_IsNull(at))continue; /* Keep receipts for OPEN tasks whose reminder is temporarily hidden. */
            cJSON *title=cJSON_GetObjectItemCaseSensitive(item,"title");
            cJSON *daily=cJSON_GetObjectItemCaseSensitive(item,"daily");
            if(!cJSON_IsNumber(at) || at->valuedouble!=at->valueint || at->valueint<0 || at->valueint>=1440 ||
               !cJSON_IsString(title) || strlen(title->valuestring)>=sizeof(out->title) ||
               !cJSON_IsNumber(daily) || (daily->valuedouble!=0 && daily->valuedouble!=1)) {
                ESP_LOGW("todo_reminder","Invalid reminder fields on page %d",page);
                cJSON_Delete(root);return false;
            }
            if(minute<at->valueint || out->id)continue;
            int32_t shown_date=0;
            esp_err_t err=nvs_get_i32(receipts,key,&shown_date);
            if(err!=ESP_OK && err!=ESP_ERR_NVS_NOT_FOUND){ESP_LOGE("todo_reminder","Read receipt: %s",esp_err_to_name(err));cJSON_Delete(root);return false;}
            if(shown_date==date || (shown_date!=0 && !daily->valueint))continue;
            out->id=id->valueint;out->date=date;
            strcpy(out->title,title->valuestring);
        }
        cJSON_Delete(root);
    }
    return true;
}
/* Only prune receipts after every OPEN page was read successfully. A failed
 * request must never erase deduplication state and replay old notifications. */
static bool next_reminder(nvs_handle_t receipts,char *body,TodoReminder *out)
{
    size_t count=0;esp_err_t err=nvs_get_used_entry_count(receipts,&count);
    if(err!=ESP_OK){ESP_LOGE("todo_reminder","Count receipts: %s",esp_err_to_name(err));return false;}
    ReceiptKey *keys=count?calloc(count,sizeof(*keys)):NULL;
    if(count && !keys){ESP_LOGE("todo_reminder","Receipt scan allocation failed");return false;}
    nvs_iterator_t it=NULL;size_t used=0;
    err=nvs_entry_find("nvs","todo_reminders",NVS_TYPE_I32,&it);
    while(err==ESP_OK){
        nvs_entry_info_t info;nvs_entry_info(it,&info);
        strcpy(keys[used++].key,info.key);
        err=nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    if(err!=ESP_ERR_NVS_NOT_FOUND){free(keys);ESP_LOGE("todo_reminder","Enumerate receipts: %s",esp_err_to_name(err));return false;}
    out->id=0;
    bool ok=scan_reminders(receipts,body,out,keys,used),changed=false;
    if(ok)for(size_t i=0;i<used;i++)if(!keys[i].open){
        err=nvs_erase_key(receipts,keys[i].key);
        if(err!=ESP_OK){ok=false;break;}
        changed=true;
    }
    if(changed){err=nvs_commit(receipts);if(err!=ESP_OK)ok=false;}
    if(!ok && err!=ESP_ERR_NVS_NOT_FOUND && err!=ESP_OK)ESP_LOGE("todo_reminder","Prune receipts: %s",esp_err_to_name(err));
    free(keys);return ok && out->id!=0;
}
static void worker(void *arg)
{
    (void)arg;TodoRequest r={0};TodoState state={0};char *body=malloc(8192);configASSERT(body);
    nvs_handle_t receipts;esp_err_t receipt_status=nvs_open("todo_reminders",NVS_READWRITE,&receipts);
    if(receipt_status!=ESP_OK)ESP_LOGE("todo_reminder","Cannot open receipts: %s; reminders disabled",esp_err_to_name(receipt_status));
    bool awaiting_reminder=false, receipt_pending=false;
    TodoReminder shown={0};int acknowledged=0;
    for(;;){
        xQueueReceive(requests,&r,pdMS_TO_TICKS(5000));
        if(xQueueReceive(reminder_shown,&shown,0)==pdTRUE)receipt_pending=true;
        if(receipt_pending) {
            char key[16];snprintf(key,sizeof(key),"t%d",shown.id);
            esp_err_t err=nvs_set_i32(receipts,key,shown.date);
            if(err==ESP_OK)err=nvs_commit(receipts);
            if(err==ESP_OK){awaiting_reminder=false;receipt_pending=false;}
            else ESP_LOGE("todo_reminder","Save receipt failed: %s; reminder delivery paused",esp_err_to_name(err));
        }
        char path[100];bool ok=true;state.completed_id=acknowledged;state.failed_id=0;
        if(r.id){snprintf(path,sizeof(path),"/api/todos/%d/complete",r.id);ok=http(path,true,body,8192);if(ok)state.completed_id=acknowledged=r.id;else state.failed_id=r.id;r.id=0;}
        snprintf(path,sizeof(path),"/api/device/todos?page=%d&done=%d",r.page,r.done);
        ok=http(path,false,body,8192) && ok;
        if(ok){
            cJSON *root=cJSON_Parse(body),*items=cJSON_GetObjectItemCaseSensitive(root,"items");
            cJSON *pages=cJSON_GetObjectItemCaseSensitive(root,"pages"),*total=cJSON_GetObjectItemCaseSensitive(root,"total");
            ok=cJSON_IsArray(items) && cJSON_IsNumber(pages) && pages->valuedouble==pages->valueint && pages->valueint>=1 &&
                cJSON_IsNumber(total) && total->valuedouble==total->valueint && total->valueint>=0;
            if(ok){
                state.page=r.page;state.done=r.done;state.count=0;
                state.pages=pages->valueint;
                state.total=total->valueint;
                cJSON *item;
                cJSON_ArrayForEach(item,items){
                    if(state.count==8)break;
                    cJSON *title=cJSON_GetObjectItemCaseSensitive(item,"title"),*id=cJSON_GetObjectItemCaseSensitive(item,"id");
                    if(!cJSON_IsString(title)||!cJSON_IsNumber(id)){ok=false;break;}
                    TodoItem *t=&state.items[state.count++];t->id=id->valueint;t->done=r.done;
                    snprintf(t->title,sizeof(t->title),"%s",title->valuestring);
                }
            }
            if(ok){
                cJSON *next=cJSON_GetObjectItemCaseSensitive(root,"next");
                if(cJSON_IsNull(next)){state.next=(TodoItem){0};state.next_at=0;}
                else {
                    cJSON *id=cJSON_GetObjectItemCaseSensitive(next,"id"),*title=cJSON_GetObjectItemCaseSensitive(next,"title");
                    cJSON *at=cJSON_GetObjectItemCaseSensitive(next,"at");
                    ok=cJSON_IsNumber(id) && id->valueint>0 && id->valuedouble==id->valueint &&
                        cJSON_IsString(title) && strlen(title->valuestring)<sizeof(state.next.title) &&
                        cJSON_IsNumber(at) && at->valuedouble>0 && at->valuedouble<=UINT32_MAX;
                    if(ok){state.next.id=id->valueint;state.next_at=(uint32_t)at->valuedouble;
                        snprintf(state.next.title,sizeof(state.next.title),"%s",title->valuestring);}
                }
            }
            cJSON_Delete(root);
            if(ok && state.page>=state.pages && state.page>0){r.page=state.pages-1;continue;}
        }
        state.online=ok;xQueueOverwrite(snapshots,&state);acknowledged=0;
        TodoReminder candidate;
        // Pruning also runs while a full-partition receipt waits to be committed.
        if(receipt_status==ESP_OK && next_reminder(receipts,body,&candidate) && !awaiting_reminder) {
            shown=candidate;
            xQueueOverwrite(reminders,&shown);
            awaiting_reminder=true;
        }
    }
}
void todo_app_start(void)
{
    requests=xQueueCreate(4,sizeof(TodoRequest));snapshots=xQueueCreate(1,sizeof(TodoState));configASSERT(requests&&snapshots);
    reminders=xQueueCreate(1,sizeof(TodoReminder));reminder_shown=xQueueCreate(1,sizeof(TodoReminder));
    configASSERT(reminders&&reminder_shown);
    PortableUI_SetTodoHandler(request);
    configASSERT(xTaskCreate(worker,"todo_http",8192,NULL,3,NULL)==pdPASS);
}
void todo_app_poll(void)
{
    TodoState state;if(snapshots && xQueueReceive(snapshots,&state,0)==pdTRUE)PortableUI_SetTodo(&state);
    TodoReminder reminder;
    if(reminders && xQueuePeek(reminders,&reminder,0)==pdTRUE && PortableUI_ShowReminder(reminder.title)) {
        xQueueReceive(reminders,&reminder,0);
        xQueueOverwrite(reminder_shown,&reminder);
    }
}
