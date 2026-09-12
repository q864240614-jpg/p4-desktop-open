/* Two opposite highlights orbit with eased motion and directional soft tails.
 * Four narrow invalidation areas avoid repainting the screen interior. */
static lv_obj_t *alert_edges[4];
static uint32_t edge_color, edge_started, edge_frame;
static int edge_head, edge_energy;
static void edge_draw(lv_event_t *event)
{
    const int side=(intptr_t)lv_event_get_user_data(event);
    lv_area_t a;lv_obj_get_coords(lv_event_get_target(event),&a);
    lv_draw_ctx_t *ctx=lv_event_get_draw_ctx(event);
    const int length=side%2?480:800;
    const int start=side==0?0:side==1?800:side==2?1280:2080;
    for(int pos=0;pos<length;pos+=4){
        const int path=start+pos;
        int behind=(edge_head-path+2560)%2560;
        behind=LV_MIN(behind,(behind+1280)%2560);
        const int tail=LV_MAX(0,360-behind);
        const int nose=LV_MAX(0,60-(1280-behind))*6;
        const int light=LV_MAX(tail,nose);
        const int alpha=(35+light*light*220/(360*360))*edge_energy/255;
        const int x=side==0?a.x1+pos:side==1?a.x1:side==2?a.x2-pos-3:a.x1;
        const int y=side==0?a.y1:side==1?a.y1+pos:side==2?a.y1:a.y2-pos-3;
        lv_draw_rect_dsc_t d;lv_draw_rect_dsc_init(&d);
        d.bg_color=lv_color_hex(edge_color);d.bg_opa=alpha/4;
        lv_area_t glow={x,y,x+(side%2?6:3),y+(side%2?3:6)};
        lv_draw_rect(ctx,&d,&glow);
        d.bg_opa=alpha;
        lv_area_t core={x+(side==1?5:0),y+(side==2?5:0),
            x+(side%2?(side==1?6:1):3),y+(side%2?3:(side==2?6:1))};
        lv_draw_rect(ctx,&d,&core);
    }
}
static void edge_process(void)
{
    uint32_t color=0;
    for(int i=0;i<2;i++){
        const BambuState *s=&printer_slots[i];
        if(!s->online)continue;
        if(printer_effect(s)==FX_ERROR){color=0xEF4248;break;}
        if(printer_effect(s)==FX_PAUSE)color=0xF2B942;
    }
    if(color!=edge_color){
        edge_color=color;edge_started=lv_tick_get();
        if(color && !alert_edges[0]){
            for(int i=0;i<4;i++){
                alert_edges[i]=lv_obj_create(ui_root);set_draw_object(alert_edges[i]);
                lv_obj_set_pos(alert_edges[i],i==1?793:0,i==2?473:0);
                lv_obj_set_size(alert_edges[i],i%2?7:800,i%2?480:7);
                lv_obj_add_event_cb(alert_edges[i],edge_draw,LV_EVENT_DRAW_MAIN,(void *)(intptr_t)i);
            }
        }
        for(int i=0;i<4;i++)if(alert_edges[i]){
            if(color)lv_obj_clear_flag(alert_edges[i],LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(alert_edges[i],LV_OBJ_FLAG_HIDDEN);
        }
    }
    const uint32_t elapsed=lv_tick_elaps(edge_frame);
    if(elapsed<16)return;
    edge_frame=lv_tick_get();
    // The card animates only on its visible printer page, never behind video/clock/notifications.
    if(!video_active && !standby_active && !event_playing && selected_page<2){
        PrinterPage *p=&printer_pages[selected_page];
        update_printer(p,&p->view); /* Reconcile deferred MQTT state on reveal; equal labels are inert. */
        bool running=printer_running(&p->view);
        PrinterEffect effect=printer_effect(&p->view);
        bool animate=p->view.online && (effect!=FX_STILL || p->motion_ms-p->stage_changed<1200);
        if(animate)p->motion_ms+=elapsed;
        if((animate || p->particle_dirty) && (p->particle_dirty || lv_tick_elaps(p->water_tick)>=80)){
            p->water_tick=lv_tick_get();
            render_printer_water(p);p->particle_dirty=false;lv_obj_invalidate(p->fill);
            if(running||effect==FX_IDLE||effect==FX_FINISH)lv_obj_invalidate(p->state_icon);
        }
        /* Same 80 ms cadence, offset from the left card to avoid a wide merged dirty area. */
        if(running && lv_tick_elaps(p->tray_tick)>=80 && lv_tick_elaps(p->water_tick)>=32){
            p->tray_tick=lv_tick_get();
            for(int i=0;i<4;i++){
                const BambuTray *t=&p->view.trays[p->tray_page*4+i];
                if(t->present && p->view.active_tray==t->unit*4+t->slot)lv_obj_invalidate(p->tray_leds[i]);
            }
            if(p->ext_led && p->view.active_tray==254)lv_obj_invalidate(p->ext_led);
        }
    }
    if(!color)return;
    const uint32_t age=lv_tick_elaps(edge_started)%5200;
    // 0.9 s inhale, 3.4 s eased orbit, 0.9 s quiet tail; one complete lap.
    const int t=LV_CLAMP(0,(int)age-900,3400);
    const int q=t*65536/3400;
    const int eased=(int)((int64_t)q*q/65536*(3*65536-2*q)/65536);
    edge_head=(int)((int64_t)eased*2560/65536)%2560;
    if(age<900)edge_energy=90+(32767-printer_sine(age+450,1800))*165/65534;
    else if(age<4300)edge_energy=255;
    else edge_energy=90+(32767+printer_sine(age-4300+450,1800))*165/65534;
    for(int i=0;i<4;i++)lv_obj_invalidate(alert_edges[i]);
}

static void printer_visual_timer(lv_timer_t *timer)
{ (void)timer;edge_process(); }
