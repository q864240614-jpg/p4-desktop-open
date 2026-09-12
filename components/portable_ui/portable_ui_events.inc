/* Private renderer, included by portable_ui.c. All state belongs to LVGL task. */
#include "reminder_bell.h"
#include "codex_restore.h"
typedef enum { EVENT_START, EVENT_RESUME, EVENT_WARNING, EVENT_PAUSE, EVENT_DONE, EVENT_OFFLINE, EVENT_TODO, EVENT_CODEX } PrinterEventKind;
typedef struct { int slot; PrinterEventKind kind; uint32_t code; } PrinterEvent;
static lv_obj_t *event_layer, *event_machine, *event_title, *event_detail;
static PrinterEvent event_current, event_queue[8];
static unsigned event_count;
static bool event_playing;
static bool reminder_acknowledged;
static uint32_t event_started, event_frame_age;
#define EVENT_ENTER_MS 900
#define EVENT_DURATION 4100
static unsigned event_duration(void) { return event_current.kind==EVENT_TODO ? 10000 : EVENT_DURATION; }
static uint32_t event_color(PrinterEventKind kind)
{
    return kind==EVENT_START || kind==EVENT_CODEX ? 0x247BDF : kind==EVENT_WARNING ? 0xE63936 : kind==EVENT_PAUSE ? 0xE9AF26 :
           kind==EVENT_OFFLINE ? 0x658196 : COLOR_BAMBU;
}
static bool event_pixel(PrinterEventKind kind,int x,int y,unsigned age)
{
    if(kind==EVENT_START || kind==EVENT_RESUME)
        return x>=4 && x<=12 && abs(y-7)*5 <= (12-x)*4 && (unsigned)(x-4)*55<age;
    if(kind==EVENT_CODEX)return (codex_restore[y] & (1U << x)) != 0;
    if(kind==EVENT_TODO) {
        return (reminder_bell[y] & (1U << x)) != 0;
    }
    if(kind==EVENT_PAUSE) return ((x>=3 && x<=5)||(x>=9 && x<=11)) && y>=3 && y<=11;
    if(kind==EVENT_WARNING) {
        int width=(y-1)/2;
        return (y>=2 && y<=13 && (abs(x-7)==width || y==13)) ||
               (x==7 && ((y>=6 && y<=9)||y==11));
    }
    if(kind==EVENT_DONE) return ((x>=2 && x<=6 && abs(y-(x+4))<=1) ||
        (x>=6 && x<=13 && abs(y-(16-x))<=1)) && (unsigned)x*55<age;
    /* Two broken chain links move away from each other. */
    return ((x>=2 && x<=6 && y>=3 && y<=7 && (x==2||x==6||y==3||y==7)) ||
            (x>=8 && x<=12 && y>=7 && y<=11 && (x==8||x==12||y==7||y==11)));
}
/* Two short holds per ribbon make the upward wipe feel deliberately stepped. */
static int event_ribbon_progress(unsigned elapsed)
{
    if(elapsed>=520)return 1000;
    if(elapsed<100)return elapsed*260/100;
    if(elapsed<160)return 260;
    if(elapsed<330)return 260+(elapsed-160)*500/170;
    if(elapsed<390)return 760;
    return 760+(elapsed-390)*240/130;
}
static struct { int top,height,gap,offset; unsigned delay,duration; } event_ribbons[8];
static void event_ribbons_reset(void)
{
    int weights[8],sum=0,top=0;
    for(int i=0;i<8;i++){weights[i]=25+esp_random()%70;sum+=weights[i];}
    for(int i=0;i<8;i++) {
        uint32_t r=esp_random();
        event_ribbons[i].top=top;
        event_ribbons[i].height=i==7?480-top:weights[i]*480/sum;
        event_ribbons[i].gap=event_ribbons[i].height*(25+r%26)/100;
        event_ribbons[i].offset=(int)((r>>8)%181)-90;
        event_ribbons[i].delay=(7-i)*18+(r>>16)%115;
        event_ribbons[i].duration=340+(r>>4)%250;
        if(event_ribbons[i].delay+event_ribbons[i].duration>835)
            event_ribbons[i].duration=835-event_ribbons[i].delay;
        top+=event_ribbons[i].height;
    }
}
static int event_ribbon_phase(int index,unsigned age)
{
    unsigned delay=event_ribbons[index].delay;
    return age>delay?event_ribbon_progress((age-delay)*520/event_ribbons[index].duration):0;
}
static int event_ribbon_y(int index,unsigned age)
{
    return 480-(480-event_ribbons[index].top)*event_ribbon_phase(index,age)/1000;
}
static int event_gap_height(unsigned age)
{
    if(age<=720)return 24;
    return age>=EVENT_ENTER_MS ? 0 : (EVENT_ENTER_MS-age)*24/(EVENT_ENTER_MS-720);
}
static bool event_gap_row(int y,unsigned age)
{
    for(int i=0;i<8;i++) {
        int end=event_ribbons[i].top+event_ribbons[i].height;
        int gap=event_ribbons[i].gap*event_gap_height(age)/24;
        if(y>=end-gap && y<end)return true;
    }
    return false;
}
static void event_entrance(lv_draw_ctx_t *ctx,unsigned age)
{
    lv_draw_rect_dsc_t d;lv_draw_rect_dsc_init(&d);
    d.bg_color=lv_color_hex(event_color(event_current.kind));
    for(int i=0;i<8;i++) {
        int y=event_ribbon_y(i,age);
        if(y>=480)continue;
        int offset=(1000-event_ribbon_phase(i,age))*event_ribbons[i].offset/1000;
        int end=y+event_ribbons[i].height-1;
        if(end>479)end=479;
        /* Random geometry is fixed for the event, never re-rolled per frame.
         * A shared irregular gap mask preserves real background between strips. */
        while(y<=end) {
            bool gap=event_gap_row(y,age);int last=y;
            while(last<end && event_gap_row(last+1,age)==gap)last++;
            if(!gap) {
                lv_area_t a={offset>0?offset:0,y,offset<0?799+offset:799,last};
                lv_draw_rect(ctx,&d,&a);
            }
            y=last+1;
        }
    }
}
static void event_draw(lv_event_t *event)
{
    lv_draw_ctx_t *ctx=lv_event_get_draw_ctx(event);
    unsigned elapsed=event_frame_age;
    if(elapsed<EVENT_ENTER_MS){event_entrance(ctx,elapsed);return;}
    unsigned age=elapsed-EVENT_ENTER_MS;
    lv_draw_rect_dsc_t d;lv_draw_rect_dsc_init(&d);d.bg_color=lv_color_hex(0x031109);
    d.bg_color=lv_color_hex(event_color(event_current.kind));
    lv_area_t screen={0,0,799,479};lv_draw_rect(ctx,&d,&screen);
    d.bg_color=lv_color_hex(0x031109);
    /* Dark LED field remains visible against a solid, full-screen event color. */
    for(int y=16;y<480;y+=16)for(int x=8;x<800;x+=16)
        printer_led(ctx,x,y,2,0x031109,LV_OPA_20);
    int scan=(age%1000)*480/1000;
    d.bg_opa=LV_OPA_20;lv_area_t band={0,scan,799,scan+2>479?479:scan+2};lv_draw_rect(ctx,&d,&band);
    for(int y=0;y<15;y++)for(int x=0;x<15;x++) {
        int dx=0,dy=0;
        if(age<320 && y%4==0)dx=((age/80)%2?1:-1)*(320-age)/18;
        if(event_current.kind==EVENT_PAUSE && age<650)dy=(x<7?-1:1)*(650-age)/35;
        if(event_current.kind==EVENT_OFFLINE)dx=(x<7?-1:1)*(age<900?age:900)/160;
        bool on=event_pixel(event_current.kind,x,y,age);
        unsigned pulse=180+(lv_trigo_sin((age%1200)*360/1200)+32767)*60/65534;
        printer_led(ctx,333+x*9+dx,130+y*9+dy,6,COLOR_WHITE,on ?
            (event_current.kind==EVENT_WARNING||event_current.kind==EVENT_PAUSE?pulse:255):32);
    }
    /* Expanding dot perimeter gives start/completion an outward-moving finish. */
    if(event_current.kind==EVENT_DONE || event_current.kind==EVENT_START || event_current.kind==EVENT_RESUME) {
        int radius=84+(age%1100)*38/1100;
        for(int angle=0;angle<360;angle+=12) {
            int x=399+(int32_t)lv_trigo_sin(angle)*radius/32767;
            int y=193+(int32_t)lv_trigo_sin(angle+90)*radius/32767;
            printer_led(ctx,x,y,3,COLOR_WHITE,120-(age%1100)*90/1100);
        }
    }
    unsigned duration=event_duration();
    int lit=(duration-(elapsed>duration?duration:elapsed))*60/(duration-EVENT_ENTER_MS);
    for(int x=0;x<60;x++)printer_led(ctx,162+x*8,434,4,COLOR_WHITE,x<lit?220:40);
}
static void event_begin(PrinterEvent e)
{
    static const char *titles[]={"PRINT STARTED","PRINT RESUMED","ATTENTION REQUIRED","PRINT PAUSED","PRINT COMPLETE","CONNECTION LOST","TIME TO DO","QUOTA RESTORED"};
    static const char *details[]={"BUILD IN PROGRESS","BACK IN MOTION","CHECK YOUR PRINTER","WAITING TO CONTINUE","READY TO COLLECT","WAITING FOR RECONNECTION","","CODEX IS READY"};
    event_current=e;event_playing=true;event_started=lv_tick_get();event_frame_age=0;
    reminder_acknowledged=false;
    event_ribbons_reset();
    stop_video(); // Cancel the decoder before the notification wakes or switches the page.
    PortableUI_Activity();
    activate_page(e.kind==EVENT_CODEX?2:e.kind==EVENT_TODO?3:e.slot,false);
    lv_label_set_text(event_machine,e.kind==EVENT_CODEX ? "CODEX / USAGE" : e.kind==EVENT_TODO ? "TODO / REMINDER" : e.slot ? "A1 MINI / SYSTEM EVENT" : "H2D / SYSTEM EVENT");
    lv_label_set_text(event_title,titles[e.kind]);
    lv_obj_set_y(event_title,e.kind==EVENT_TODO?270:316);
    lv_obj_set_y(event_detail,e.kind==EVENT_TODO?334:380);
    lv_obj_set_height(event_detail,e.kind==EVENT_TODO?96:32);
    if(e.kind==EVENT_WARNING && e.code)lv_label_set_text_fmt(event_detail,"ERROR %08lX / CHECK PRINTER",(unsigned long)e.code);
    else lv_label_set_text(event_detail,details[e.kind]);
    lv_obj_set_style_opa(event_machine,0,0);lv_obj_set_style_opa(event_title,0,0);lv_obj_set_style_opa(event_detail,0,0);
    lv_obj_set_style_opa(event_layer,LV_OPA_COVER,0);
    lv_obj_clear_flag(event_layer,LV_OBJ_FLAG_HIDDEN);lv_obj_move_foreground(event_layer);
}
static void event_enqueue(PrinterEvent e)
{
    if(!event_layer)return;
    for(unsigned i=0;i<event_count;i++)if(event_queue[i].slot==e.slot && event_queue[i].kind==e.kind && event_queue[i].code==e.code)return;
    if(!event_playing || (e.kind==EVENT_WARNING && event_current.kind!=EVENT_WARNING)) {event_begin(e);return;}
    /* Bounded queue: preserve warnings before ordinary transitions. */
    if(event_count==8) {
        int drop=-1;for(int i=7;i>=0;i--)if(event_queue[i].kind!=EVENT_WARNING){drop=i;break;}
        if(drop<0)return;
        memmove(event_queue+drop,event_queue+drop+1,(event_count-drop-1)*sizeof(*event_queue));event_count--;
    }
    unsigned pos=event_count;
    if(e.kind==EVENT_WARNING){pos=0;while(pos<event_count && event_queue[pos].kind==EVENT_WARNING)pos++;}
    memmove(event_queue+pos+1,event_queue+pos,(event_count-pos)*sizeof(*event_queue));event_queue[pos]=e;event_count++;
}
static void event_transition(int slot,const BambuState *old,const BambuState *next)
{
    if(!next->online) {if(old->online)event_enqueue((PrinterEvent){slot,EVENT_OFFLINE,0});return;}
    bool changed=strcmp(old->state,next->state)!=0;
    if((next->error && next->error!=old->error) || (changed && !strcmp(next->state,"FAILED")))
        event_enqueue((PrinterEvent){slot,EVENT_WARNING,next->error});
    else if(changed && !strcmp(next->state,"PAUSE"))event_enqueue((PrinterEvent){slot,EVENT_PAUSE,0});
    else if(old->has_data && changed && !strcmp(next->state,"FINISH"))event_enqueue((PrinterEvent){slot,EVENT_DONE,0});
    else if(old->has_data && changed && (!strcmp(next->state,"PREPARE") || !strcmp(next->state,"RUNNING"))) {
        if(!strcmp(old->state,"PAUSE"))event_enqueue((PrinterEvent){slot,EVENT_RESUME,0});
        else if(strcmp(old->state,"PREPARE") && strcmp(old->state,"RUNNING"))event_enqueue((PrinterEvent){slot,EVENT_START,0});
    }
}
static void event_process(void)
{
    if(!event_playing)return;
    unsigned previous_age=event_frame_age;
    unsigned age=event_frame_age=lv_tick_elaps(event_started);
    unsigned duration=event_duration();
    if(age>=duration) {
        event_playing=false;lv_obj_add_flag(event_layer,LV_OBJ_FLAG_HIDDEN);
        lv_disp_trig_activity(lv_obj_get_disp(ui_root));
        if(event_count){PrinterEvent next=event_queue[0];memmove(event_queue,event_queue+1,--event_count*sizeof(*event_queue));event_begin(next);}
        return;
    }
    unsigned reveal=age>EVENT_ENTER_MS?age-EVENT_ENTER_MS:0;
    lv_opa_t opacity=reveal>=180?255:reveal*255/180;
    lv_obj_set_style_opa(event_machine,opacity,0);lv_obj_set_style_opa(event_title,opacity,0);lv_obj_set_style_opa(event_detail,opacity,0);
    if(age>duration-350)lv_obj_set_style_opa(event_layer,(duration-age)*255/350,0);
    if(previous_age<EVENT_ENTER_MS+900 || age>duration-350){
        lv_obj_invalidate(event_layer);
    }else{
        // The hold background is static. Refresh only the icon, countdown and both scan positions.
        lv_area_t icon={270,60,530,322},countdown={162,434,639,438};
        lv_obj_invalidate_area(event_layer,&icon);lv_obj_invalidate_area(event_layer,&countdown);
        int old_scan=((previous_age-EVENT_ENTER_MS)%1000)*480/1000;
        int new_scan=((age-EVENT_ENTER_MS)%1000)*480/1000;
        lv_area_t old_band={0,old_scan,799,LV_MIN(old_scan+2,479)};
        lv_area_t new_band={0,new_scan,799,LV_MIN(new_scan+2,479)};
        lv_obj_invalidate_area(event_layer,&old_band);lv_obj_invalidate_area(event_layer,&new_band);
    }
}
static void create_events(void)
{
    event_layer=lv_obj_create(ui_root);set_plain_container(event_layer,COLOR_BAMBU);lv_obj_set_style_bg_opa(event_layer,LV_OPA_TRANSP,0);lv_obj_set_size(event_layer,800,480);
    lv_obj_add_flag(event_layer,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(event_layer,event_draw,LV_EVENT_DRAW_MAIN,NULL);
    event_machine=create_label(event_layer,"",&lv_font_inter_28,COLOR_WHITE);
    event_title=create_label(event_layer,"",&lv_font_inter_48,COLOR_WHITE);
    event_detail=create_label(event_layer,"",&lv_font_cjk_24,COLOR_WHITE);
    lv_obj_t *labels[]={event_machine,event_title,event_detail};int ys[]={54,316,380};
    for(int i=0;i<3;i++){lv_obj_set_pos(labels[i],24,ys[i]);lv_obj_set_width(labels[i],752);lv_obj_set_style_text_align(labels[i],LV_TEXT_ALIGN_CENTER,0);lv_label_set_long_mode(labels[i],LV_LABEL_LONG_DOT);}
}

/* Poller hands over a reminder only when the full-screen event queue is idle. */
bool PortableUI_ShowReminder(const char *title)
{
    if(event_playing && event_current.kind==EVENT_TODO && !reminder_acknowledged &&
       event_frame_age>=EVENT_ENTER_MS+180) {
        reminder_acknowledged=true;
        return true;
    }
    if(event_playing || event_count)return false;
    event_begin((PrinterEvent){.kind=EVENT_TODO});
    lv_label_set_text(event_detail,title);
    return false;
}

static void notify_codex_restored(void)
{
    event_enqueue((PrinterEvent){.slot=2,.kind=EVENT_CODEX});
}
