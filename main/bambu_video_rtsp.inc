/* H2D: RTSPS with interleaved RTP (RFC 2326), H.264 packetization mode 0/1
 * (RFC 6184). Only parameter sets and IDR slices reach the decoder. */
#include "p4_h264.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "mbedtls/md5.h"
#include "esp_random.h"
#include <strings.h>
#include <ctype.h>

#define RTSP_TEXT 8192
#define NAL_LIMIT (1024*1024)
#define AU_LIMIT (2*1024*1024)
typedef struct H264 H264;
static bool rtp_packet(H264 *h,uint8_t *data,size_t length,unsigned payload);
typedef struct {
    CameraTLS *tls;
    H264 *video;
    uint8_t *packet;
    unsigned id,cseq,nc;
    char url[192],track[384],session[128],realm[128],nonce[256],opaque[256];
    const char *code;
    bool digest,basic,qop;
    uint8_t channel,payload;
    char *response;
    char *body;
} RTSP;
static bool header_value(const char *text,const char *key,char *out,size_t size)
{
    size_t keylen=strlen(key);
    const char *line=strstr(text,"\r\n");
    while(line && line[2]) {
        line+=2;const char *end=strstr(line,"\r\n");
        if(!end)return false;
        if(!strncasecmp(line,key,keylen) && line[keylen]==':') {
            const char *value=line+keylen+1;while(value<end && *value==' ')value++;
            size_t n=end-value;if(n>=size)return false;
            memcpy(out,value,n);out[n]=0;return true;
        }
        line=end;
    }
    return false;
}
/* Returns HTTP-style response code; zero denotes malformed/closed transport.
 * first is an already consumed byte when reading a streaming RTSP response. */
static int rtsp_response(RTSP *s,int first)
{
    // Interleaved RTCP/RTP can precede an RTSP reply, including PLAY's 200.
    uint8_t prefix[4];
    if(first<0) {
        if(!camera_transfer(s->tls,prefix,1,false,s->id))return 0;
    } else prefix[0]=first;
    while(prefix[0]=='$') {
        if(!camera_transfer(s->tls,prefix+1,3,false,s->id))return 0;
        size_t length=((unsigned)prefix[2]<<8)|prefix[3];
        if(!length || !camera_transfer(s->tls,s->packet,length,false,s->id))return 0;
        if(prefix[1]==s->channel && !rtp_packet(s->video,s->packet,length,s->payload))return 0;
        if(!camera_transfer(s->tls,prefix,1,false,s->id))return 0;
    }
    size_t used=1;s->response[0]=prefix[0];
    while(used<RTSP_TEXT-1) {
        if(!camera_transfer(s->tls,(uint8_t *)s->response+used,1,false,s->id))return 0;
        used++;
        if(used>=4 && !memcmp(s->response+used-4,"\r\n\r\n",4))break;
    }
    if(used==RTSP_TEXT-1)return 0;
    s->response[used]=0;
    int status;unsigned seq;char value[64];
    if(sscanf(s->response,"RTSP/1.0 %d",&status)!=1 ||
       !header_value(s->response,"CSeq",value,sizeof(value)) || sscanf(value,"%u",&seq)!=1 || seq!=s->cseq)return 0;
    size_t length=0;
    if(header_value(s->response,"Content-Length",value,sizeof(value))) {
        char *end;unsigned long n=strtoul(value,&end,10);
        if(*end || n>=RTSP_TEXT)return 0;
        length=n;
    }
    if(length && !camera_transfer(s->tls,(uint8_t *)s->body,length,false,s->id))return 0;
    s->body[length]=0;return status;
}
static void md5_hex(const char *text,char hex[33])
{
    unsigned char digest[16];
    int rc=mbedtls_md5((const unsigned char *)text,strlen(text),digest);configASSERT(rc==0);
    for(int i=0;i<16;i++)sprintf(hex+i*2,"%02x",digest[i]);
}
static bool auth_field(const char *text,const char *name,char *out,size_t size)
{
    const char *p=text;
    while(*p) {
        while(*p==' ' || *p==',')p++;
        const char *key=p;while(*p && *p!='=' && *p!=',')p++;
        if(*p!='=')return false;
        size_t keylen=p-key;p++;
        bool quoted=*p=='"';if(quoted)p++;
        const char *value=p;
        while(*p && (quoted?*p!='"':*p!=','))p++;
        if(keylen==strlen(name) && !memcmp(key,name,keylen)) {
            if(quoted && *p!='"')return false;
            size_t n=p-value;if(n>=size)return false;
            for(size_t i=0;i<n;i++)if((unsigned char)value[i]<32 || value[i]=='\\')return false;
            memcpy(out,value,n);out[n]=0;return true;
        }
        if(quoted && *p)p++;
    }
    return false;
}
static bool challenge(RTSP *s)
{
    char value[1024],qop[64],algorithm[32];
    if(!header_value(s->response,"WWW-Authenticate",value,sizeof(value)))return false;
    if(!strncasecmp(value,"Basic ",6)){s->basic=true;return true;}
    if(strncasecmp(value,"Digest ",7))return false;
    if(!auth_field(value+7,"realm",s->realm,sizeof(s->realm)) ||
       !auth_field(value+7,"nonce",s->nonce,sizeof(s->nonce)))return false;
    if(auth_field(value+7,"algorithm",algorithm,sizeof(algorithm)) && strcasecmp(algorithm,"MD5"))return false;
    s->qop=auth_field(value+7,"qop",qop,sizeof(qop));
    if(s->qop) {
        bool auth=false;char *save;
        for(char *token=strtok_r(qop,",",&save);token;token=strtok_r(NULL,",",&save)) {
            while(*token==' ')token++;
            size_t length=strlen(token);while(length && token[length-1]==' ')length--;
            if(length==4 && !memcmp(token,"auth",4))auth=true;
        }
        if(!auth)return false;
    }
    auth_field(value+7,"opaque",s->opaque,sizeof(s->opaque));
    s->digest=true;return true;
}
static bool rtsp_send(RTSP *s,const char *method,const char *url,const char *headers)
{
    char auth[1536]="",session[160]="";
    if(s->basic) {
        char plain[80],encoded[112];size_t used;
        snprintf(plain,sizeof(plain),"bblp:%s",s->code);
        if(mbedtls_base64_encode((uint8_t *)encoded,sizeof(encoded)-1,&used,(const uint8_t *)plain,strlen(plain)))return false;
        encoded[used]=0;snprintf(auth,sizeof(auth),"Authorization: Basic %s\r\n",encoded);
    } else if(s->digest) {
        char text[1024],ha1[33],ha2[33],response[33],cnonce[17],qop[128]="",opaque[288]="";
        snprintf(text,sizeof(text),"bblp:%s:%s",s->realm,s->code);md5_hex(text,ha1);
        snprintf(text,sizeof(text),"%s:%s",method,url);md5_hex(text,ha2);
        snprintf(cnonce,sizeof(cnonce),"%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random());
        s->nc++;
        if(s->qop) {
            snprintf(text,sizeof(text),"%s:%s:%08x:%s:auth:%s",ha1,s->nonce,s->nc,cnonce,ha2);
            snprintf(qop,sizeof(qop),", qop=auth, nc=%08x, cnonce=\"%s\"",s->nc,cnonce);
        } else snprintf(text,sizeof(text),"%s:%s:%s",ha1,s->nonce,ha2);
        md5_hex(text,response);
        if(s->opaque[0])snprintf(opaque,sizeof(opaque),", opaque=\"%s\"",s->opaque);
        int n=snprintf(auth,sizeof(auth),"Authorization: Digest username=\"bblp\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\"%s%s\r\n",
            s->realm,s->nonce,url,response,qop,opaque);
        if(n<0 || (size_t)n>=sizeof(auth))return false;
    }
    if(s->session[0])snprintf(session,sizeof(session),"Session: %s\r\n",s->session);
    char *text=malloc(3072);configASSERT(text);
    int n=snprintf(text,3072,"%s %s RTSP/1.0\r\nCSeq: %u\r\nUser-Agent: P4-Camera\r\n%s%s%s\r\n",
        method,url,++s->cseq,auth,session,headers);
    bool ok=n>=0 && n<3072 && camera_transfer(s->tls,(uint8_t *)text,n,true,s->id);
    free(text);return ok;
}
static bool rtsp_request(RTSP *s,const char *method,const char *url,const char *headers)
{
    if(!rtsp_send(s,method,url,headers))return false;
    int status=rtsp_response(s,-1);
    /* The single 401 challenge exchange is part of RTSP authentication. */
    if(status==401 && !s->digest && !s->basic) {
        if(!challenge(s) || !rtsp_send(s,method,url,headers))return false;
        status=rtsp_response(s,-1);
    }
    ESP_LOGI("bambu_video","H2D %s status %d",method,status);
    return status==200;
}

typedef struct { uint8_t *data;size_t size; } H264Job;
struct H264 {
    p4_h264_decoder decoder;
    QueueHandle_t decode_queue;
    SemaphoreHandle_t decode_stopped;
    atomic_bool decode_failed;
    unsigned skipped, decoded_frames;
    int64_t metrics_start, decode_us, convert_us;
    uint8_t parameter_sets[4096];
    size_t parameter_size;
    uint8_t *au;
    size_t au_size, au_prefix;
    bool au_has_idr;
    char detail[96];
    unsigned id;
    uint8_t *nal;
    size_t used;
    uint16_t *rgb;
    bool fragment,fragment_keep,have_sequence,have_sps,discard_timestamp;
    uint32_t lost_timestamp;
    uint16_t sequence;
    uint32_t timestamp;
    const char *error;
};
static int clamp_rgb(int value) { return value<0?0:value>255?255:value; }
typedef struct { const uint8_t *data; size_t bits,pos; bool ok; } Bits;
static uint32_t bits(Bits *b,unsigned count)
{
    if(count>b->bits-b->pos){b->ok=false;return 0;}
    uint32_t value=0;
    for(unsigned i=0;i<count;i++,b->pos++)value=(value<<1)|((b->data[b->pos/8]>>(7-b->pos%8))&1);
    return value;
}
static uint32_t golomb(Bits *b)
{
    unsigned zeros=0;
    while(b->ok && !bits(b,1)) {
        if(++zeros==31){b->ok=false;return 0;}
    }
    uint32_t tail=bits(b,zeros);
    return ((1U<<zeros)-1)+tail;
}
/* Bound frame allocation at the external SPS boundary. */
static bool supported_sps(const uint8_t *nal,size_t length)
{
    uint8_t rbsp[512];size_t size=0;unsigned zeros=0;
    if(length<5 || length>sizeof(rbsp) || (nal[1]!=66 && nal[1]!=77 && nal[1]!=100))return false;
    for(size_t i=1;i<length;i++) {
        if(zeros==2 && nal[i]==3){zeros=0;continue;}
        rbsp[size++]=nal[i];zeros=nal[i]==0?zeros+1:0;
    }
    Bits b={.data=rbsp,.bits=size*8,.pos=24,.ok=true};
    golomb(&b); /* seq_parameter_set_id */
    if(nal[1]==100) {
        if(golomb(&b)!=1 || golomb(&b)!=0 || golomb(&b)!=0)return false; /* 8-bit 4:2:0 */
        bits(&b,1);
        if(bits(&b,1))for(unsigned list=0;list<8;list++) {
            if(!bits(&b,1))continue;
            int last=8,next=8;
            for(unsigned j=0;j<(list<6?16:64);j++) {
                if(next) {
                    uint32_t code=golomb(&b);
                    int delta=(code&1)?(int)((code+1)/2):-(int)(code/2);
                    next=(last+delta+256)%256;
                }
                if(next)last=next;
            }
        }
    }
    uint32_t frame_num=golomb(&b),poc=golomb(&b);
    if(frame_num>12 || poc>2)return false;
    if(poc==0){if(golomb(&b)>12)return false;}
    if(poc==1) {
        bits(&b,1);golomb(&b);golomb(&b);
        uint32_t cycle=golomb(&b);if(cycle>255)return false;
        for(unsigned i=0;i<cycle;i++)golomb(&b);
    }
    uint32_t references=golomb(&b);bits(&b,1);
    uint32_t width=golomb(&b),height=golomb(&b);
    bool progressive=bits(&b,1);
    return b.ok && references<=4 && width<120 && height<68 && progressive;
}
static bool decode_nal(H264 *h,uint8_t *data,size_t length)
{
    if(!length || data[0]&0x80)return false;
    unsigned type=data[0]&31;
    if(type!=5 && type!=7 && type!=8)return true;
    if(type==7) {
        if(!supported_sps(data,length)) {
            h->error="H2D requires 8-bit 4:2:0, max 1920x1088 / 4 refs";return false;
        }
        h->have_sps=true;
    }
    if(!h->have_sps)return true; /* Join at the first parameter set. */
    if(length+4>AU_LIMIT-h->au_size){h->error="H2D keyframe exceeds 2 MiB";return false;}
    memcpy(h->au+h->au_size,"\0\0\0\1",4);
    memcpy(h->au+h->au_size+4,data,length);
    h->au_size+=length+4;
    if(type==7 || type==8){
        if(type==7)h->parameter_size=0;
        if(length+4>sizeof(h->parameter_sets)-h->parameter_size){h->error="H2D parameter sets too large";return false;}
        memcpy(h->parameter_sets+h->parameter_size,"\0\0\0\1",4);
        memcpy(h->parameter_sets+h->parameter_size+4,data,length);h->parameter_size+=length+4;
    }
    if(type==5)h->au_has_idr=true;
    else if(!h->au_has_idr)h->au_prefix=h->au_size;
    return true;
}
static bool decode_picture(H264 *h)
{
    // Retain SDP parameter sets until the first complete IDR picture arrives.
    if(!h->au_has_idr)return true;
    p4_h264_frame frame={0};
    int64_t started=esp_timer_get_time();

    int status=p4_h264_decode(h->decoder,h->au,h->au_size,&frame);
    h->au_size=0;h->au_prefix=0;h->au_has_idr=false;
    if(status) {
        if(status&0x4000)snprintf(h->detail,sizeof(h->detail),"H2D decoder: not enough PSRAM");
        else snprintf(h->detail,sizeof(h->detail),"H2D H.264 decode error: 0x%x",status);
        h->error=h->detail;return false;
    }
    if(!frame.ready){h->error="H2D IDR produced no picture";return false;}
    int64_t decoded=esp_timer_get_time();
    h->decode_us+=decoded-started;
    if(!h->metrics_start)h->metrics_start=started;
    h->rgb=acquire_frame(h->id);
    if(!h->rgb)return false;
    unsigned crop_width=frame.width,crop_height=frame.height;
    if(frame.width*VIDEO_HEIGHT>frame.height*VIDEO_WIDTH)crop_width=frame.height*VIDEO_WIDTH/VIDEO_HEIGHT;
    else crop_height=frame.width*VIDEO_HEIGHT/VIDEO_WIDTH;
    unsigned left=(frame.width-crop_width)/2,top=(frame.height-crop_height)/2;
    unsigned source_x[VIDEO_WIDTH];
    for(unsigned x=0;x<VIDEO_WIDTH;x++)source_x[x]=left+x*crop_width/VIDEO_WIDTH;
    for(unsigned y=0;y<VIDEO_HEIGHT;y++) {
        unsigned sy=top+y*crop_height/VIDEO_HEIGHT;
        for(unsigned x=0;x<VIDEO_WIDTH;x++) {
            unsigned sx=source_x[x];
            unsigned chroma=(sy/2)*frame.stride_uv+sx/2;
            int c=frame.plane[0][sy*frame.stride_y+sx]-16;
            int d=frame.plane[1][chroma]-128,e=frame.plane[2][chroma]-128;
            int r=clamp_rgb((298*c+409*e+128)>>8);
            int g=clamp_rgb((298*c-100*d-208*e+128)>>8);
            int b=clamp_rgb((298*c+516*d+128)>>8);
            h->rgb[y*VIDEO_WIDTH+x]=((r>>3)<<11)|((g>>2)<<5)|(b>>3);
        }
        if((y&31)==0)vTaskDelay(1);
    }
    h->convert_us+=esp_timer_get_time()-decoded;
    report(h->id,h->rgb,NULL);
    h->decoded_frames++;
    int64_t elapsed=esp_timer_get_time()-h->metrics_start;
    if(elapsed>=5000000){
        ESP_LOGI("bambu_video","H2D %.2f fps, mean decode %lld ms, output/wait %lld ms, PSRAM free %u",
            h->decoded_frames*1000000.0/elapsed,(long long)(h->decode_us/h->decoded_frames/1000),
            (long long)(h->convert_us/h->decoded_frames/1000),(unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        h->metrics_start=0;h->decoded_frames=0;h->decode_us=0;h->convert_us=0;
    }
    return current(h->id);
}
static void decode_worker(void *arg)
{
    H264 *owner=arg;
    H264 picture={.id=owner->id,.decoder=owner->decoder};
    H264Job job;
    for(;;){
        xQueueReceive(owner->decode_queue,&job,portMAX_DELAY);
        if(!job.data)break;
        if(current(owner->id) && !atomic_load(&owner->decode_failed)){
            picture.au=job.data;picture.au_size=job.size;picture.au_has_idr=true;
            if(!decode_picture(&picture) && current(owner->id)){
                report(owner->id,NULL,picture.error);
                atomic_store(&owner->decode_failed,true);
            }
        }
        free(job.data);
    }
    ESP_LOGI("bambu_video","H2D decoder stack minimum free %u bytes",
        (unsigned)uxTaskGetStackHighWaterMark(NULL));
    xSemaphoreGive(owner->decode_stopped);
    // The reader joins us and releases the WithCaps stack/TCB without a helper task.
    vTaskSuspend(NULL);
}
static bool decode_access_unit(H264 *h)
{
    if(!h->au_has_idr)return true;
    // Latest complete IDR wins. Never make the socket reader wait for decoding or rendering.
    H264Job old;
    if(xQueueReceive(h->decode_queue,&old,0)==pdTRUE){free(old.data);h->skipped++;}
    size_t prefix=h->au_prefix?0:h->parameter_size;
    uint8_t *data=heap_caps_malloc(prefix+h->au_size,MALLOC_CAP_SPIRAM);
    if(!data){
        ESP_LOGW("bambu_video","Dropping IDR: compressed frame allocation failed");
        h->skipped++;
    }else{
        memcpy(data,h->parameter_sets,prefix);memcpy(data+prefix,h->au,h->au_size);
        H264Job job={data,prefix+h->au_size};configASSERT(xQueueSend(h->decode_queue,&job,0)==pdTRUE);
    }
    h->au_size=0;h->au_prefix=0;h->au_has_idr=false;
    return !atomic_load(&h->decode_failed) && current(h->id);
}
static bool standalone_nal(H264 *h,const uint8_t *data,size_t length)
{
    if(!length || length>NAL_LIMIT || data[0]&0x80)return false;
    unsigned type=data[0]&31;
    if(type<1 || type>23)return false;
    if(type!=5 && type!=7 && type!=8)return true;
    memcpy(h->nal+4,data,length);return decode_nal(h,h->nal+4,length);
}
static bool rtp_payload(H264 *h,uint8_t *data,size_t length,unsigned payload)
{
    if(length<12 || data[0]>>6!=2)return false;
    if((data[1]&127)!=payload)return true;
    uint16_t seq=((uint16_t)data[2]<<8)|data[3];
    uint32_t timestamp=(uint32_t)data[4]<<24|(uint32_t)data[5]<<16|(uint32_t)data[6]<<8|data[7];
    if(h->have_sequence && seq!=(uint16_t)(h->sequence+1)){
        // A missing packet invalidates the entire picture, including later IDR slices.
        h->fragment=false;h->used=0;h->au_size=h->au_prefix;h->au_has_idr=false;
        h->discard_timestamp=true;h->lost_timestamp=timestamp;
        ESP_LOGW("bambu_video","RTP gap: discarding damaged picture");
    }
    h->have_sequence=true;h->sequence=seq;
    if(h->discard_timestamp){
        if(timestamp==h->lost_timestamp)return true;
        h->discard_timestamp=false;
    }
    size_t start=12+4*(data[0]&15);
    if(start>length)return false;
    if(data[0]&16) {
        if(start+4>length)return false;
        size_t extension=((unsigned)data[start+2]<<8)|data[start+3];start+=4+4*extension;
    }
    if(start>length)return false;
    if(data[0]&32) {
        unsigned padding=data[length-1];if(!padding || padding>length-start)return false;
        length-=padding;
    }
    if(start>=length)return false;
    uint8_t *p=data+start;size_t n=length-start;unsigned type=p[0]&31;
    if(p[0]&0x80)return false;
    if(type>=1 && type<=23) {
        if(h->fragment)return false;
        return standalone_nal(h,p,n);
    }
    if(type==24) {
        if(h->fragment)return false;
        size_t offset=1;
        while(offset+2<=n) {
            size_t size=((unsigned)p[offset]<<8)|p[offset+1];offset+=2;
            if(!size || size>n-offset || !standalone_nal(h,p+offset,size))return false;
            offset+=size;
        }
        return offset==n;
    }
    if(type!=28 || n<3 || (p[1]&32) || (p[1]&0xc0)==0xc0 || (p[1]&31)==0 || (p[1]&31)>23)return false;
    if(p[1]&0x80) {
        if(h->fragment)return false;
        h->fragment=true;h->timestamp=timestamp;h->used=1;
        h->nal[4]=(p[0]&0xe0)|(p[1]&31);
        unsigned nal_type=p[1]&31;
        h->fragment_keep=nal_type==7 || nal_type==8 || nal_type==5;
    } else if(!h->fragment || h->timestamp!=timestamp ||
              h->nal[4]!=((p[0]&0xe0)|(p[1]&31)))return false;
    if(n-2>NAL_LIMIT-h->used)return false;
    if(h->fragment_keep)memcpy(h->nal+4+h->used,p+2,n-2);
    h->used+=n-2;
    if(p[1]&0x40) {
        h->fragment=false;return !h->fragment_keep || decode_nal(h,h->nal+4,h->used);
    }
    return true;
}
static bool rtp_packet(H264 *h,uint8_t *data,size_t length,unsigned payload)
{
    if(!rtp_payload(h,data,length,payload))return false;
    if(h->discard_timestamp)return true;
    if((data[1]&127)==payload && (data[1]&128)) {
        if(h->fragment)return false;
        return decode_access_unit(h);
    }
    return true;
}
static bool sdp_video(RTSP *s,H264 *h)
{
    char base[384],control[192]="",sets[1536]="";
    if(!header_value(s->response,"Content-Base",base,sizeof(base)))strcpy(base,s->url);
    if(!base[0])return false;
    bool video=false,codec=false;int pt=-1;
    char *save;
    for(char *line=strtok_r(s->body,"\r\n",&save);line;line=strtok_r(NULL,"\r\n",&save)) {
        if(!strncmp(line,"m=",2)) {
            if(video)break;
            video=!strncmp(line,"m=video ",8);
        }
        if(!video)continue;
        int n;char name[32];
        if(sscanf(line,"a=rtpmap:%d %31s",&n,name)==2 && !strcasecmp(name,"H264/90000")) {pt=n;codec=true;}
        if(!strncmp(line,"a=control:",10)) {
            if(strlen(line+10)>=sizeof(control))return false;
            strcpy(control,line+10);
        }
        const char *params=strstr(line,"sprop-parameter-sets=");
        if(params) {
            params+=21;size_t size=strcspn(params,"; ");if(size>=sizeof(sets))return false;
            memcpy(sets,params,size);sets[size]=0;
        }
        const char *mode=strstr(line,"packetization-mode=");
        if(mode && mode[19]!='0' && mode[19]!='1')return false;
    }
    if(!codec || pt<0 || pt>127 || !control[0])return false;
    s->payload=pt;
    /* Resolve absolute-path controls against the printer origin. */
    const char *authority_end=strchr(s->url+8,'/');size_t authority=authority_end-s->url;
    int n;
    if(!strncmp(control,"rtsps://",8))n=snprintf(s->track,sizeof(s->track),"%s",control);
    else if(control[0]=='/')n=snprintf(s->track,sizeof(s->track),"%.*s%s",(int)authority,s->url,control);
    else n=snprintf(s->track,sizeof(s->track),"%s%s%s",base,base[strlen(base)-1]=='/'?"":"/",control);
    /* LIVE555 omits :322 from Content-Base. Keep the existing TLS endpoint. */
    if(n<0 || (size_t)n>=sizeof(s->track) || strncmp(s->track,"rtsps://",8))return false;
    const char *track_path=strchr(s->track+8,'/');
    if(!track_path)return false;
    size_t track_authority=track_path-s->track;
    size_t host_end=strchr(s->url+8,':')-s->url;
    if(track_authority==host_end && !strncmp(s->track,s->url,host_end)) {
        if((size_t)n+4>=sizeof(s->track))return false;
        memmove(s->track+host_end+4,s->track+host_end,strlen(s->track+host_end)+1);
        memcpy(s->track+host_end,":322",4);
    } else if(track_authority!=authority || strncmp(s->track,s->url,authority))return false;
    for(const char *p=s->track;*p;p++)if((unsigned char)*p<=32 || *p=='"' || *p=='\\')return false;
    if(sets[0]) {
        char *save_set;
        for(char *set=strtok_r(sets,",",&save_set);set;set=strtok_r(NULL,",",&save_set)) {
            size_t used;
            if(mbedtls_base64_decode(h->nal+4,NAL_LIMIT,&used,(const uint8_t *)set,strlen(set)) ||
               !decode_nal(h,h->nal+4,used))return false;
        }
    }
    return true;
}
static void play_rtsp(CameraTLS *camera,const BambuPrinterConfig *printer,unsigned id)
{
    TaskHandle_t decode_task=NULL;
    RTSP *s=calloc(1,sizeof(*s));H264 h={.id=id,.error="Invalid H2D RTSP / RTP stream"};
    configASSERT(s);s->tls=camera;s->id=id;s->code=printer->code;
    s->response=malloc(RTSP_TEXT);s->body=malloc(RTSP_TEXT);
    h.nal=heap_caps_malloc(NAL_LIMIT+4,MALLOC_CAP_SPIRAM);
    h.au=heap_caps_malloc(AU_LIMIT,MALLOC_CAP_SPIRAM);

    uint8_t *packet=malloc(65536);
    s->packet=packet;s->video=&h;
    const char *error="Not enough memory for H2D camera";
    if(!s->response || !s->body || !h.nal || !h.au || !packet)goto done;
    int status=p4_h264_create(&h.decoder);
    if(status) {
        snprintf(h.detail,sizeof(h.detail),"H2D decoder init error: 0x%x",status);
        error=h.detail;goto done;
    }
    h.decode_queue=xQueueCreate(1,sizeof(H264Job));h.decode_stopped=xSemaphoreCreateBinary();
    if(!h.decode_queue || !h.decode_stopped){error="Not enough memory for H2D pipeline";goto done;}
    if(xTaskCreatePinnedToCoreWithCaps(decode_worker,"h2d_decode",32768,&h,2,&decode_task,1,
                                     MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS){
        ESP_LOGE("bambu_video","H2D task allocation failed: internal free %u largest %u; PSRAM free %u largest %u",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
        error="Cannot start H2D decoder task";goto done;
    }
    snprintf(s->url,sizeof(s->url),"rtsps://%s:322/streaming/live/1",printer->ip);
    error="RTSPS failed: check access code / LAN liveview";
    if(!rtsp_request(s,"DESCRIBE",s->url,"Accept: application/sdp\r\n"))goto done;
    if(!sdp_video(s,&h)){error=h.error;goto done;}
    if(!rtsp_request(s,"SETUP",s->track,"Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"))goto done;
    char session[256],transport[512];unsigned video_channel,rtcp_channel;
    if(!header_value(s->response,"Session",session,sizeof(session)))goto done;
    size_t size=strcspn(session,"; ");if(!size || size>=sizeof(s->session))goto done;
    memcpy(s->session,session,size);s->session[size]=0;
    if(!header_value(s->response,"Transport",transport,sizeof(transport)))goto done;
    ESP_LOGI("bambu_video","H2D negotiated Transport: %s",transport);
    const char *channels=strstr(transport,"interleaved=");
    if(!channels || sscanf(channels,"interleaved=%u-%u",&video_channel,&rtcp_channel)!=2 ||
       video_channel>255 || rtcp_channel>255 || video_channel==rtcp_channel)goto done;
    s->channel=video_channel;
    ESP_LOGI("bambu_video","H2D interleaved RTP/RTCP channels %u/%u",video_channel,rtcp_channel);
    if(!rtsp_request(s,"PLAY",s->url,"Range: npt=0.000-\r\n"))goto done;
    report(id,NULL,"WAITING FOR KEYFRAME...");
    int64_t keepalive=esp_timer_get_time();bool pending=false;
    while(current(id) && !atomic_load(&h.decode_failed)) {
        uint8_t prefix[4];
        if(!camera_transfer(camera,prefix,1,false,id)){error="H2D video timed out / disconnected";goto done;}
        if(prefix[0]!='$') {
            if(!pending || rtsp_response(s,prefix[0])!=200)goto done;
            pending=false;continue;
        }
        if(!camera_transfer(camera,prefix+1,3,false,id))goto done;
        size_t length=((unsigned)prefix[2]<<8)|prefix[3];
        if(!length || !camera_transfer(camera,packet,length,false,id))goto done;
        if(prefix[1]==s->channel && !rtp_packet(&h,packet,length,s->payload)){error=h.error;goto done;}
        if(esp_timer_get_time()-keepalive>=20000000) {
            // A slow decoder may leave the OPTIONS reply behind queued RTP.
            // Keep draining the live connection instead of treating that as a disconnect.
            if(!pending){
                if(!rtsp_send(s,"OPTIONS",s->url,""))goto done;
                pending=true;keepalive=esp_timer_get_time();
            }
        }
        /* Yield to networking/UI even while dropping predicted frames. */
        if((h.sequence&31)==0)vTaskDelay(1);
    }
done:
    if(decode_task){
        H264Job pending;
        if(xQueueReceive(h.decode_queue,&pending,0)==pdTRUE)free(pending.data);
        H264Job stop={0};xQueueSend(h.decode_queue,&stop,portMAX_DELAY);
        xSemaphoreTake(h.decode_stopped,portMAX_DELAY);
        vTaskDeleteWithCaps(decode_task);
        ESP_LOGI("bambu_video","H2D reader skipped %u queued IDRs",h.skipped);
    }
    if(h.decode_queue)vQueueDelete(h.decode_queue);
    if(h.decode_stopped)vSemaphoreDelete(h.decode_stopped);
    if(current(id) && !atomic_load(&h.decode_failed))
        report(id,NULL,camera->error[0]?camera->error:error);
    if(h.decoder)p4_h264_destroy(h.decoder);
    free(packet);free(h.au);free(h.nal);free(s->response);free(s->body);free(s);
}
