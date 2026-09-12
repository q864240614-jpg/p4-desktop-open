#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef void *p4_h264_decoder;
typedef void *QueueHandle_t;
typedef void *SemaphoreHandle_t;
typedef bool atomic_bool;
#define atomic_load(p) (*(p))
#define pdTRUE 1
#define MALLOC_CAP_SPIRAM 0
#define configASSERT assert
#define ESP_LOGW(...) ((void)0)
typedef struct H264 H264;
typedef struct {uint8_t *data;size_t size;} H264Job;
#include "rtp_state.inc"
static H264Job queued;
static bool present,allocation_failed;
static int releases;
static void *heap_caps_malloc(size_t n,int caps){(void)caps;return allocation_failed?NULL:malloc(n);}
static bool current(unsigned id){return id==1;}
static int xQueueReceive(void *q,H264Job *job,int ticks)
{(void)q;(void)ticks;if(!present)return 0;*job=queued;present=false;return 1;}
static int xQueueSend(void *q,H264Job *job,int ticks)
{(void)q;(void)ticks;assert(!present);queued=*job;present=true;return 1;}
static void release(void *data){releases++;free(data);}
#define free release
#include "latest_job.inc"
int main(void)
{
    uint8_t payload[3]={1,2,3};H264 h={.id=1,.au=payload,.au_size=3,.au_has_idr=true,.parameter_size=2};
    h.parameter_sets[0]=7;h.parameter_sets[1]=8;
    assert(decode_access_unit(&h) && queued.size==5);
    assert(queued.data[0]==7 && queued.data[1]==8 && queued.data[4]==3);
    h.au_size=3;h.au_has_idr=true;payload[2]=9;
    assert(decode_access_unit(&h) && releases==1 && h.skipped==1);
    assert(queued.data[4]==9); // Newest complete picture replaces old queued work.
    H264Job active;assert(xQueueReceive(NULL,&active,0));
    h.au_size=3;h.au_has_idr=true;h.au_prefix=3;
    assert(decode_access_unit(&h) && queued.size==3); // Already-prefixed AU is not prefixed twice.
    assert(active.data[4]==9);free(active.data); // Active decoder storage remains independent.
    allocation_failed=true;h.au_size=3;h.au_has_idr=true;
    assert(decode_access_unit(&h) && !present && h.skipped==3);
    assert(h.au_size==0 && !h.au_has_idr);
    puts("PASS: latest IDR replacement, independent active buffer, parameter preservation, allocation-pressure dropping");
}
