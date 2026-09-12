#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define NAL_LIMIT 1024
#define ESP_LOGW(...) ((void)0)
typedef void *QueueHandle_t;
typedef void *SemaphoreHandle_t;
typedef bool atomic_bool;
typedef void *p4_h264_decoder;
typedef struct H264 H264;
#include "rtp_state.inc"
static int pictures;
static bool decode_nal(H264 *h,const uint8_t *data,size_t length)
{ (void)length;if((data[0]&31)==5)h->au_has_idr=true;return true; }
static bool standalone_nal(H264 *h,const uint8_t *data,size_t length)
{ return decode_nal(h,data,length); }
static bool decode_access_unit(H264 *h)
{ if(h->au_has_idr){pictures++;h->au_has_idr=false;}return true; }
#include "rtp_functions.inc"
static bool packet(H264 *h,uint16_t seq,uint32_t stamp,uint8_t fu,bool marker)
{
    uint8_t p[15]={0x80,(uint8_t)(96|(marker?128:0)),seq>>8,seq,
        stamp>>24,stamp>>16,stamp>>8,stamp,0,0,0,1,0x7c,fu,0xaa};
    return rtp_packet(h,p,sizeof(p),96);
}
int main(void)
{
    uint8_t nal[NAL_LIMIT+4];H264 h={.nal=nal,.au_prefix=12,.au_size=99};
    assert(packet(&h,100,1000,0x85,false) && h.fragment);
    assert(packet(&h,102,1000,0x05,false));
    assert(!h.fragment && h.discard_timestamp && h.au_size==12);
    assert(packet(&h,103,1000,0x45,true) && pictures==0);
    assert(packet(&h,104,2000,0x85,false));
    assert(packet(&h,105,2000,0x45,true) && pictures==1);
    h.sequence=65535;
    assert(packet(&h,0,3000,0x85,false) && !h.discard_timestamp);
    assert(packet(&h,1,3000,0x45,true) && pictures==2);
    uint8_t truncated[4]={0};assert(!rtp_packet(&h,truncated,4,96));
    puts("PASS: RTP loss discards whole picture, resumes at next timestamp, sequence wrap, short packet rejection");
}
