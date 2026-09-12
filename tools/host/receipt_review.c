#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cJSON.h"
#define ESP_OK 0
#define ESP_ERR_NVS_NOT_FOUND 1
#define NVS_TYPE_I32 1
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
typedef int esp_err_t;
typedef int nvs_handle_t;
typedef struct { int id,date;char title[321]; } TodoReminder;
typedef struct { char key[16]; } nvs_entry_info_t;
static struct {char key[16];int32_t date;} saved[3];
static size_t entries;
static int erased,commits,iteration;
typedef int *nvs_iterator_t;
static const char *esp_err_to_name(int err){(void)err;return "error";}
static int nvs_get_used_entry_count(int h,size_t *n){(void)h;*n=entries;return 0;}
static int nvs_entry_find(const char *p,const char *n,int type,nvs_iterator_t *it)
{(void)p;(void)n;(void)type;iteration=0;*it=&iteration;return entries?0:1;}
static void nvs_entry_info(nvs_iterator_t it,nvs_entry_info_t *info){strcpy(info->key,saved[*it].key);}
static int nvs_entry_next(nvs_iterator_t *it){return (size_t)++**it<entries?0:1;}
static void nvs_release_iterator(nvs_iterator_t it){(void)it;}
static int nvs_get_i32(int h,const char *key,int32_t *date)
{(void)h;for(size_t i=0;i<entries;i++)if(!strcmp(key,saved[i].key)){*date=saved[i].date;return 0;}return 1;}
static int nvs_erase_key(int h,const char *key)
{(void)h;for(size_t i=0;i<entries;i++)if(!strcmp(key,saved[i].key)){
    memmove(saved+i,saved+i+1,(--entries-i)*sizeof(saved[0]));erased++;return 0;}return 1;}
static int nvs_commit(int h){(void)h;commits++;return 0;}
static void fixed_time(const time_t *t,struct tm *out)
{(void)t;*out=(struct tm){.tm_year=126,.tm_mon=8,.tm_mday=10,.tm_hour=10};}
#define localtime_r fixed_time
static bool fail_page,invalid_page;
static bool http(const char *path,bool post,char *body,size_t capacity)
{
    (void)post;int page;assert(sscanf(path,"/api/todos?page=%d",&page)==1);
    if(page==1 && fail_page)return false;
    if(invalid_page){strcpy(body,"{}");return true;}
    snprintf(body,capacity,"{\"pages\":2,\"items\":[{\"id\":%d,\"title\":\"alarm\",\"daily\":0,\"remind_minute\":540}]}",page?200:100);
    return true;
}
#include "receipt_functions.inc"
int main(void)
{
    strcpy(saved[0].key,"t100");saved[0].date=20260910;
    strcpy(saved[1].key,"t999");saved[1].date=20260909;entries=2;
    char body[8192];TodoReminder out;
    fail_page=true;assert(!next_reminder(0,body,&out) && erased==0 && commits==0);
    fail_page=false;invalid_page=true;
    assert(!next_reminder(0,body,&out) && erased==0 && commits==0);
    invalid_page=false;assert(next_reminder(0,body,&out));
    assert(out.id==200 && out.date==20260910 && erased==1 && commits==1);
    assert(entries==1 && !strcmp(saved[0].key,"t100"));
    entries=0;assert(next_reminder(0,body,&out) && out.id==100);
    puts("PASS: failed/incomplete scans retain receipts; completed scan prunes obsolete receipt and suppresses shown reminders");
}
