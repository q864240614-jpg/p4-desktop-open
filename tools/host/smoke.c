/* Actual LVGL renderer + actual UI; only hardware allocation/RNG/time are stubbed. */
#include <time.h>
#ifdef _WIN32
#include <stdlib.h>
#include <windows.h>
#include <crtdbg.h>
#undef COLOR_BACKGROUND
static struct tm *host_localtime_r(const time_t *t, struct tm *out)
{ localtime_s(out,t);return out; }
#define localtime_r host_localtime_r
#define setenv(name,value,overwrite) _putenv_s(name,value)
#define tzset _tzset
#endif
#include "portable_icons.h"
static time_t wall_time;
static time_t fixture_time(time_t *out) { if (out) *out = wall_time; return wall_time; }
#define time fixture_time
#include "../../components/portable_ui/portable_ui.c"
#undef time
static void host_assert_fail(const char *expression,int line)
{ fprintf(stderr,"Host assertion at line %d: %s\n",line,expression);fflush(stderr);exit(1); }
#undef assert
#define assert(expression) ((expression)?(void)0:host_assert_fail(#expression,__LINE__))

static lv_color_t frame[480 * 800];
static bool pressed;
static lv_point_t touch_position={300,399};
static void flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *pixels)
{
    assert(area->x1 >= 0 && area->y1 >= 0 && area->x2 < 480 && area->y2 < 800);
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x)
            frame[y * 480 + x] = *pixels++;
    lv_disp_flush_ready(drv);
}
static void read_touch(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    data->point = touch_position; /* native -> logical (400,300) */
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
static void advance(unsigned ms)
{
    if(ms>60000){
        while(event_playing && ms>5000){advance(5000);ms-=5000;}
        lv_tick_inc(ms-1000);ms=1000;
    }
    for (unsigned i = 0; i < ms; i += 25) {
        lv_tick_inc(25);
        PortableUI_Process();
        lv_timer_handler();
    }
}
static void snapshot(const char *name)
{
    char path[128];
    snprintf(path, sizeof(path), "%s.ppm", name);
    FILE *out = fopen(path, "wb");
    assert(out);
    fprintf(out, "P6\n800 480\n255\n");
    for (unsigned i = 0; i < 800 * 480; ++i) {
        lv_color32_t c = {.full = lv_color_to32(frame[(799 - i % 800) * 480 + i / 800])};
        unsigned char rgb[] = {c.ch.red, c.ch.green, c.ch.blue};
        fwrite(rgb, 1, 3, out);
    }
    fclose(out);
}
static void sample_rgb(int x,int y,int *r,int *g,int *b)
{
    lv_color32_t c={.full=lv_color_to32(frame[(799-x)*480+y])};
    *r=c.ch.red;*g=c.ch.green;*b=c.ch.blue;
}
static int luma(int r,int g,int b){return (r*30+g*59+b*11)/100;}
static void write_card_rgb(FILE *out)
{
    for(int y=148;y<148+316;y++)for(int x=24;x<24+272;x++){
        lv_color32_t c={.full=lv_color_to32(frame[(799-x)*480+y])};
        unsigned char rgb[]={c.ch.red,c.ch.green,c.ch.blue};
        fwrite(rgb,1,3,out);
    }
}
static void check_labels(lv_obj_t *obj)
{
    if (lv_obj_check_type(obj, &lv_label_class)) {
        lv_area_t a, b;
        lv_obj_get_coords(obj, &a);
        lv_obj_get_coords(lv_obj_get_parent(obj), &b);
        if(!(a.x1 >= b.x1 && a.x2 <= b.x2 && a.y1 >= b.y1 && a.y2 <= b.y2))
            fprintf(stderr,"Label %s: (%d,%d,%d,%d) parent (%d,%d,%d,%d)\n",lv_label_get_text(obj),a.x1,a.y1,a.x2,a.y2,b.x1,b.y1,b.x2,b.y2);
        assert(a.x1 >= b.x1 && a.x2 <= b.x2 && a.y1 >= b.y1 && a.y2 <= b.y2);
    }
    for (unsigned i = 0; i < lv_obj_get_child_cnt(obj); ++i)
        check_labels(lv_obj_get_child(obj, i));
}
static int video_released;
static void video_test_release(const uint16_t *p){(void)p;video_released++;}
static int video_requested_slot;
static void video_test_request(int slot){video_requested_slot=slot;}
static int todo_requested_id;
static bool todo_test_request(int page,int done,int id)
{ (void)page;(void)done;todo_requested_id=id;return true; }
int main(void)
{
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
#endif
    setenv("TZ", "CST-8", 1); tzset();
    /* Every phase has mirrored positions and opposite tilt; time units differ. */
    assert(clock_geometry[0].center_x < 0 && clock_geometry[1].center_x > 800);
    for (int phase = 0; phase <= 1048576; phase += 256) {
        for (int offset = -2; offset <= 2; ++offset) {
            int32_t hx, hy, mx, my;
            int16_t ha = clock_pose(true, phase, offset, &hx, &hy);
            int16_t ma = clock_pose(false, phase, offset, &mx, &my);
            assert(hx + mx == 800 * 256 && hy == my && ha == -ma);
        }
    }
    lv_init();
    static lv_color_t buffer[480 * 40];
    static lv_disp_draw_buf_t draw;
    lv_disp_draw_buf_init(&draw, buffer, NULL, 480 * 40);
    static lv_disp_drv_t driver;
    lv_disp_drv_init(&driver);
    driver.hor_res = 480; driver.ver_res = 800;
    driver.draw_buf = &draw; driver.flush_cb = flush; driver.sw_rotate = true;
    lv_disp_t *display = lv_disp_drv_register(&driver);
    assert(display);
    lv_disp_set_rotation(display, LV_DISP_ROT_90);
    assert(lv_disp_get_hor_res(display) == 800 && lv_disp_get_ver_res(display) == 480);
    static lv_indev_drv_t input;
    lv_indev_drv_init(&input);
    input.type = LV_INDEV_TYPE_POINTER; input.read_cb = read_touch;
    lv_indev_t *touch = lv_indev_drv_register(&input);
    assert(touch);
    assert(PortableUI_Create(lv_scr_act()));
    assert(!PortableUI_Create(lv_scr_act()));
    /* Sample fixed spatial thresholds; brightness must rise monotonically. */
    for(int k=0;k<64;k++){
        int last=-1;
        for(int alpha=0;alpha<=65280;alpha+=128){
            lv_color_t pixel=clock_blend_pixel(lv_color_black(),255,alpha,k%8,k/8);
            assert(LV_COLOR_GET_R(pixel)>=last);last=LV_COLOR_GET_R(pixel);
        }
    }
    assert(clock_blend_pixel(lv_color_black(),255,65280,0,0).full==lv_color_white().full);
    BambuState stage_probe;BambuState_Init(&stage_probe);
    const char *stage_json="{\"print\":{\"stg_cur\":7,\"gcode_state\":\"RUNNING\"}}";
    assert(BambuState_Parse(&stage_probe,stage_json,strlen(stage_json)));
    assert(stage_probe.stage==7 && printer_effect(&stage_probe)==FX_HEAT);
    assert(BambuState_Parse(&stage_probe,"{\"print\":{\"mc_percent\":12}}",strlen("{\"print\":{\"mc_percent\":12}}")));
    assert(stage_probe.stage==7);
    stage_probe.stage=66;char stage_name[48];
    assert(!strcmp(printer_status(&stage_probe,stage_name,sizeof(stage_name)),"AIR PURIFY"));
    assert(printer_effect(&stage_probe)==FX_VENT);
    stage_probe.stage=660;
    assert(!strcmp(printer_status(&stage_probe,stage_name,sizeof(stage_name)),"RUNNING"));
    assert(printer_effect(&stage_probe)==FX_WATER);


    assert(340+8+25 < DOT_FIELD_HEIGHT); // Standby bars retain their screen position after the shift.
    for(int dial=0;dial<2;dial++){
        uint16_t last=0;
        for(int ms=0;ms<=800;ms+=8){
            uint16_t next=clock_opacity(dial,0,ms,0,0,0);
            uint16_t old=clock_opacity(dial,-1,ms,0,0,0);
            assert(next>=last && old==40*256);last=next;
        }
        assert(last==255*256);
    }

    PortableUI_Status status = {.codex.remaining = {NAN, NAN}};
    PortableUI_SetStatus(&status);
    advance(50);
    assert(!clock_valid && !lv_obj_has_flag(time_pending_label, LV_OBJ_FLAG_HIDDEN));
    snapshot("printer-offline");
    lv_obj_add_flag(printer_pages[printer_slot].tile,LV_OBJ_FLAG_HIDDEN);lv_obj_clear_flag(codex_tile,LV_OBJ_FLAG_HIDDEN);
    snapshot("codex-offline");
    PortableUI_SetStandby(true); advance(650);
    snapshot("standby-offline");
    PortableUI_Activity(); advance(650);
    const char *fixture = "{\"host\":\"demo\",\"codex_usage\":{\"ok\":true,\"plan_type\":\"plus\",\"allowed\":true,\"primary_window\":{\"limit_window_seconds\":18000,\"used_percent\":26,\"reset_at\":1788788400},\"secondary_window\":{\"limit_window_seconds\":604800,\"used_percent\":49,\"reset_at\":1789220422}}}";
    assert(PortableUI_ParseStatusJSON(fixture, &status));
    assert(status.codex.remaining[0] == 51 && status.codex.remaining[1] == 74);
    PortableUI_SetStatus(&status);
    wall_time = 1788770430;
    advance(50);
    assert(clock_valid && lv_obj_has_flag(time_pending_label, LV_OBJ_FLAG_HIDDEN));
    check_labels(ui_root);
    snapshot("codex");
    PortableUI_SetStandby(true);
    advance(650);
    assert(standby_active && lv_obj_get_x(standby_layer) == 0);
    check_labels(ui_root);
    snapshot("standby");
    const uint8_t saved_second=clock_second;
    for(int second=0;second<60;second+=10){
        clock_second=second;clock_second_tick=lv_tick_get();
        refresh_clock_dials();lv_refr_now(display);
        char name[48];snprintf(name,sizeof(name),"standby-phase-%02d",second);snapshot(name);
    }
    clock_second=saved_second;clock_second_tick=lv_tick_get();refresh_clock_dials();
    /* Touch through LVGL's real hit testing, not a direct UI API call. */
    pressed = true; advance(100);
    lv_point_t point; lv_indev_get_point(touch, &point);
    assert(point.x == 400 && point.y == 300);
    pressed = false; advance(650);
    assert(!standby_active && lv_obj_has_flag(standby_layer, LV_OBJ_FLAG_HIDDEN));
    lv_tick_inc(181000); advance(650);
    assert(standby_active && lv_obj_get_x(standby_layer) == 0);
    assert(!PortableUI_ParseStatusJSON("{}", &status));
    assert(!status.nas_online && !status.codex.online && status.codex.remaining[0] == 51);
    PortableUI_SetStatus(&status);
    PortableUI_Activity(); advance(650);
    snapshot("codex-stale");
    /* Long server plan is ellipsized, remaining 100% still fits. */
    snprintf(status.codex.plan, sizeof(status.codex.plan), "enterprise-plan");
    status.nas_online = status.codex.online = true;
    status.codex.remaining[0] = 100;
    PortableUI_SetStatus(&status); advance(50); check_labels(ui_root);
    for (int i = 0; i < PORTABLE_ICON_COUNT; ++i) {
        const lv_img_dsc_t *im = PortableIcons_Get(i);
        assert(im && im->data_size == im->header.w * im->header.h);
    }
    assert(!PortableIcons_Get(PORTABLE_ICON_COUNT));
    BambuFrame reassembly={0};
    assert(!BambuFrame_Append(&reassembly,6,0,"abc",3));
    assert(BambuFrame_Append(&reassembly,6,3,"def",3));
    assert(!strcmp(reassembly.data,"abcdef"));BambuFrame_Reset(&reassembly);
    assert(!BambuFrame_Append(&reassembly,70000,0,"abc",3) && !reassembly.data);
    assert(!BambuFrame_Append(&reassembly,6,0,"abc",3));
    assert(!BambuFrame_Append(&reassembly,6,4,"def",3) && !reassembly.data);
    assert(!BambuFrame_Append(&reassembly,2,0,"abc",3) && !reassembly.data);
    /* H2D telemetry regressions: packed temperatures, 64-bit stat and deltas. */
    BambuState telemetry;BambuState_Init(&telemetry);
    assert(telemetry.chamber==-1 && telemetry.speed_level==-1 && telemetry.door_open==-1);
    const char *h2d_metrics_report="{\"print\":{\"spd_lvl\":2,\"stat\":\"100800000\",\"device\":{\"ctc\":{\"info\":{\"temp\":3932205}},\"extruder\":{\"info\":[{\"id\":0},{\"id\":1}]}}}}";
    assert(BambuState_Parse(&telemetry,h2d_metrics_report,strlen(h2d_metrics_report)));
    assert(telemetry.chamber==45 && telemetry.speed_level==2 && telemetry.door_open==1);
    const char *metrics_delta="{\"print\":{\"mc_percent\":12}}";
    assert(BambuState_Parse(&telemetry,metrics_delta,strlen(metrics_delta)));
    assert(telemetry.chamber==45 && telemetry.speed_level==2 && telemetry.door_open==1);
    const char *metrics_closed="{\"print\":{\"spd_lvl\":4,\"stat\":\"100000000\",\"device\":{\"ctc\":{\"info\":{\"temp\":\"3932160\"}}}}}";
    assert(BambuState_Parse(&telemetry,metrics_closed,strlen(metrics_closed)));
    assert(telemetry.chamber==0 && telemetry.speed_level==4 && telemetry.door_open==0);
    const char *metrics_invalid="{\"print\":{\"spd_lvl\":5,\"stat\":\"xyz\",\"device\":{\"ctc\":{\"info\":{\"temp\":3932461}}}}}";
    assert(BambuState_Parse(&telemetry,metrics_invalid,strlen(metrics_invalid)));
    assert(telemetry.chamber==0 && telemetry.speed_level==4 && telemetry.door_open==0);
    BambuState single_nozzle;BambuState_Init(&single_nozzle);
    assert(BambuState_Parse(&single_nozzle,metrics_closed,strlen(metrics_closed)));
    assert(single_nozzle.door_open==-1);
    BambuState printer; BambuState_Init(&printer);
    const char *report="{\"print\":{\"gcode_state\":\"RUNNING\",\"subtask_name\":\"Desk organizer / PETG\",\"mc_percent\":64,\"mc_remaining_time\":83,\"layer_num\":192,\"total_layer_num\":300,\"nozzle_temper\":245,\"nozzle_target_temper\":245,\"bed_temper\":70,\"bed_target_temper\":70,\"ams\":{\"tray_now\":\"1\",\"ams\":[{\"id\":\"0\",\"tray\":[{\"id\":\"0\",\"tray_type\":\"PLA\",\"tray_color\":\"E64A43FF\",\"remain\":80},{\"id\":\"1\",\"tray_type\":\"PETG\",\"tray_color\":\"E4DCCFFF\",\"remain\":64},{\"id\":\"2\",\"tray_type\":\"ABS\",\"tray_color\":\"58867EFF\"},{\"id\":\"3\",\"tray_type\":\"PLA\",\"tray_color\":\"424D6BFF\"}]}]}}}";
    assert(BambuState_Parse(&printer,report,strlen(report)));
    assert(printer.progress==64 && printer.trays[1].remaining==64 && printer.active_tray==1);
    const char *delta="{\"print\":{\"mc_percent\":65}}";
    assert(BambuState_Parse(&printer,delta,strlen(delta)) && printer.progress==65 && printer.nozzle==245);
    BambuState saved=printer;
    assert(!BambuState_Parse(&printer,"{",1) && !memcmp(&saved,&printer,sizeof(printer)));
    assert(!BambuState_Parse(&printer,"{}garbage",9));
    const char *bad="{\"print\":{\"mc_percent\":200,\"nozzle_temper\":null}}";
    assert(BambuState_Parse(&printer,bad,strlen(bad)) && printer.progress==65 && printer.nozzle==245);
    PortableUI_SetNetwork("192.168.1.88 PIN 123456");
    lv_obj_clear_flag(printer_pages[printer_slot].tile,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(codex_tile,LV_OBJ_FLAG_HIDDEN);
    activate_page(printer_slot,false);PortableUI_SetPrinter(&printer);PortableUI_Activity();advance(650);check_labels(ui_root);snapshot("printer");
    {
        uint32_t dim=printer_bar_dim(COLOR_BAMBU);
        assert(((dim>>8)&255)>40 && (dim&255)>20);
        int lr,lg,lb,dr,dg,db;
        sample_rgb(45+3*6,355,&lr,&lg,&lb);
        sample_rgb(45+32*6,355,&dr,&dg,&db);
        assert(lg>80 && lg>lr+30 && lg>lb);
        assert(luma(dr,dg,db)>22 && luma(dr,dg,db)<luma(lr,lg,lb) && dg>20);
    }
    if(getenv("P4_MOTION_REVIEW")){
        const int stages[]={1,2,3,4,8,9,12,13,14,19,22,24,25,29,36,39,46,62,70};
        for(unsigned i=0;i<sizeof(stages)/sizeof(stages[0]);i++){
            printer.stage=stages[i];PortableUI_SetPrinter(&printer);advance(900);
            PrinterPage *p=&printer_pages[printer_slot];p->previous_effect=FX_STILL;p->stage_changed=0;
            char name[80];snprintf(name,sizeof(name),"motion-%02d.rgb",printer.stage);
            FILE *out=fopen(name,"wb");assert(out);
            for(int ms=0;ms<=STAGE_PERIOD;ms+=50){
                p->motion_ms=ms;render_printer_water(p);lv_obj_invalidate(p->card);lv_refr_now(display);
                write_card_rgb(out);
                if(ms%1500==0){snprintf(name,sizeof(name),"motion-card-%02d-%04d",printer.stage,ms);snapshot(name);}
            }
            fclose(out);
        }
        puts("PASS: exported actual RGB565 LVGL stage sequences at 50 ms intervals");return 0;
    }
    if(getenv("P4_FX_REVIEW")){
        static const struct { int stage,progress,error,online; const char *state,*file; } fx[]={
            {0,45,0,1,"RUNNING","fx-water"},{0,0,0,1,"RUNNING","fx-prep"},
            {2,10,0,1,"RUNNING","fx-heat"},{1,5,0,1,"RUNNING","fx-level"},
            {4,20,0,1,"RUNNING","fx-feed"},{22,20,0,1,"RUNNING","fx-unload"},
            {24,20,0,1,"RUNNING","fx-load"},{9,15,0,1,"RUNNING","fx-scan"},
            {14,8,0,1,"RUNNING","fx-clean"},{13,5,0,1,"RUNNING","fx-home"},
            {8,12,0,1,"RUNNING","fx-flow"},{19,12,0,1,"RUNNING","fx-calibrate"},
            {29,80,0,1,"RUNNING","fx-cool"},{62,10,0,1,"RUNNING","fx-tool"},
            {70,10,0,1,"RUNNING","fx-center"},{46,10,0,1,"RUNNING","fx-camera"},
            {12,10,0,1,"RUNNING","fx-lidar"},{25,10,0,1,"RUNNING","fx-motor"},
            {36,10,0,1,"RUNNING","fx-motion"},{39,10,0,1,"RUNNING","fx-offset"},
            {3,10,0,1,"RUNNING","fx-vibration"},{66,30,0,1,"RUNNING","fx-vent"},
            {0,45,0,1,"PAUSE","fx-pause"},
            {0,100,0,1,"FINISH","fx-finish"},{0,45,1,1,"FAILED","fx-error"},
        };
        BambuState catalog=printer;catalog.has_data=true;catalog.remaining=83;
        catalog.layer=192;catalog.layers=300;
        for(unsigned i=0;i<sizeof(fx)/sizeof(fx[0]);i++){
            catalog.stage=fx[i].stage;catalog.progress=fx[i].progress;
            catalog.error=fx[i].error;catalog.online=fx[i].online;
            snprintf(catalog.state,sizeof(catalog.state),"%s",fx[i].state);
            PortableUI_SetPrinter(&catalog);advance(200);
            PrinterPage *p=&printer_pages[printer_slot];
            p->previous_effect=FX_STILL;p->stage_changed=0;p->motion_ms=0;
            char name[80];snprintf(name,sizeof(name),"fx-anim-%s.rgb",fx[i].file);
            FILE *out=fopen(name,"wb");assert(out);
            for(int ms=0;ms<4000;ms+=80){
                p->motion_ms=ms;render_printer_water(p);
                lv_obj_invalidate(p->fill);lv_obj_invalidate(p->card);lv_obj_invalidate(p->state_icon);
                lv_refr_now(display);write_card_rgb(out);
            }
            fclose(out);
        }
        puts("PASS: exported 4s LVGL card loops at 80 ms for every effect");return 0;
    }

    advance(181000);assert(standby_active);PortableUI_Activity();advance(650);
    for(int stage=1;stage<=14;stage++){
        printer.stage=stage;PortableUI_SetPrinter(&printer);advance(1000);
        char file[40];snprintf(file,sizeof(file),"printer-stage-%02d",stage);snapshot(file);
    }
    const int detail_stages[]={18,22,24,25,36,39,46,62,70};
    for(unsigned i=0;i<sizeof(detail_stages)/sizeof(detail_stages[0]);i++){
        printer.stage=detail_stages[i];PortableUI_SetPrinter(&printer);advance(1200);
        char name[48];snprintf(name,sizeof(name),"printer-detail-%d",printer.stage);snapshot(name);
    }
    BambuState catalog=printer;
    catalog.has_data=true;catalog.remaining=83;catalog.layer=192;catalog.layers=300;
    static const struct { int stage,progress,error,online; const char *state,*file; } fx_catalog[]={
        {0,0,0,0,"IDLE","fx-offline"},{0,0,0,1,"IDLE","fx-idle"},
        {0,0,0,1,"RUNNING","fx-prep"},{0,45,0,1,"RUNNING","fx-water"},
        {2,10,0,1,"RUNNING","fx-heat"},{1,5,0,1,"RUNNING","fx-level"},
        {4,20,0,1,"RUNNING","fx-feed"},{22,20,0,1,"RUNNING","fx-unload"},
        {24,20,0,1,"RUNNING","fx-load"},{9,15,0,1,"RUNNING","fx-scan"},
        {14,8,0,1,"RUNNING","fx-clean"},{13,5,0,1,"RUNNING","fx-home"},
        {8,12,0,1,"RUNNING","fx-flow"},{19,12,0,1,"RUNNING","fx-calibrate"},
        {29,80,0,1,"RUNNING","fx-cool"},{62,10,0,1,"RUNNING","fx-tool"},
        {70,10,0,1,"RUNNING","fx-center"},{46,10,0,1,"RUNNING","fx-camera"},
        {12,10,0,1,"RUNNING","fx-lidar"},{25,10,0,1,"RUNNING","fx-motor"},
        {36,10,0,1,"RUNNING","fx-motion"},{39,10,0,1,"RUNNING","fx-offset"},
        {3,10,0,1,"RUNNING","fx-vibration"},{66,30,0,1,"RUNNING","fx-vent"},
        {0,45,0,1,"PAUSE","fx-pause"},
        {0,100,0,1,"FINISH","fx-finish"},{0,45,1,1,"FAILED","fx-error"},
        {0,45,0,1,"FAILED","fx-stopped"},{0,45,0,0,"RUNNING","fx-disconnected"},
    };
    for(unsigned i=0;i<sizeof(fx_catalog)/sizeof(fx_catalog[0]);i++){
        catalog.stage=fx_catalog[i].stage;catalog.progress=fx_catalog[i].progress;
        catalog.error=fx_catalog[i].error;catalog.online=fx_catalog[i].online;
        catalog.has_data=fx_catalog[i].online || !strcmp(fx_catalog[i].file,"fx-disconnected");
        snprintf(catalog.state,sizeof(catalog.state),"%s",fx_catalog[i].state);
        PortableUI_SetPrinter(&catalog);advance(700);snapshot(fx_catalog[i].file);
        if(!strcmp(fx_catalog[i].file,"fx-idle")){
            int r,g,b;sample_rgb(45+10*6,355,&r,&g,&b);
            assert(b>30 && luma(r,g,b)>22);
        }
    }
    PortableUI_SetPrinter(&printer);advance(50);
    printer.stage=7;printer.nozzle=180;printer.nozzle_target=220;printer.bed=40;printer.bed_target=70;
    PortableUI_SetPrinter(&printer);advance(1000);snapshot("printer-heating-red");
    printer.stage=-1;
    char temperature[32];
    printer_temperature(temperature,sizeof(temperature),180,220,true);assert(!strcmp(temperature,"#EF4248 180#"));
    printer_temperature(temperature,sizeof(temperature),220,220,true);assert(!strcmp(temperature,"220"));
    printer_temperature(temperature,sizeof(temperature),220,180,true);assert(!strcmp(temperature,"220"));
    const int animated_stages[]={1,2,3,4,7,8,9,10,11,12,18,13,14,15,19,22,24,25,31,29,36,37,38,39,40,41,42,44,52,55,43,46,53,56,45,47,48,49,50,54,51,60,62,66,70};
    for(unsigned i=0;i<sizeof(animated_stages)/sizeof(animated_stages[0]);i++){
        stage_probe.stage=animated_stages[i];assert(printer_running(&stage_probe));
        StagePose a=stage_pose(stage_probe.stage,printer_effect(&stage_probe),800);
        StagePose b=stage_pose(stage_probe.stage,printer_effect(&stage_probe),2100);
        int difference=0;
        for(int y=4;y<PARTICLE_HEIGHT-4;y+=5)for(int x=4;x<PARTICLE_WIDTH-4;x+=5){
            StageParticle pa=stage_particle(&a,x,y),pb=stage_particle(&b,x,y);
            difference+=abs(pa.x-pb.x)+abs(pa.y-pb.y)+abs(pa.light-pb.light);
        }
        assert(difference>0);
    }
    /* Filament swap / flow cal: particle positions must not jump between nearby frames. */
    const int smooth_stages[]={4,8,19,22,24};
    for(unsigned s=0;s<sizeof(smooth_stages)/sizeof(smooth_stages[0]);s++){
        stage_probe.stage=smooth_stages[s];
        PrinterEffect fx=printer_effect(&stage_probe);
        for(int t=0;t<STAGE_PERIOD-50;t+=200){
            StagePose a=stage_pose(stage_probe.stage,fx,t),b=stage_pose(stage_probe.stage,fx,t+50);
            int jump=0;
            for(int y=4;y<PARTICLE_HEIGHT-4;y+=20)for(int x=4;x<PARTICLE_WIDTH-4;x+=20){
                StageParticle pa=stage_particle(&a,x,y),pb=stage_particle(&b,x,y);
                jump=LV_MAX(jump,abs(pa.x-pb.x)+abs(pa.y-pb.y));
            }
            assert(jump<10);
        }
    }
    {
        int was_progress=stage_probe.progress;stage_probe.progress=0;stage_probe.stage=0;
        strcpy(stage_probe.state,"PREPARE");assert(printer_effect(&stage_probe)==FX_PREP);
        StagePose a=stage_pose(0,FX_PREP,800),b=stage_pose(0,FX_PREP,2100);
        int difference=0,jump=0;
        for(int y=4;y<PARTICLE_HEIGHT-4;y+=5)for(int x=4;x<PARTICLE_WIDTH-4;x+=5){
            StageParticle pa=stage_particle(&a,x,y),pb=stage_particle(&b,x,y);
            difference+=abs(pa.x-pb.x)+abs(pa.y-pb.y)+abs(pa.light-pb.light);
        }
        assert(difference>0);
        for(int t=0;t<STAGE_PERIOD-50;t+=200){
            StagePose s0=stage_pose(0,FX_PREP,t),s1=stage_pose(0,FX_PREP,t+50);
            for(int y=4;y<PARTICLE_HEIGHT-4;y+=20)for(int x=4;x<PARTICLE_WIDTH-4;x+=20){
                StageParticle pa=stage_particle(&s0,x,y),pb=stage_particle(&s1,x,y);
                jump=LV_MAX(jump,abs(pa.x-pb.x)+abs(pa.y-pb.y));
            }
        }
        assert(jump<10);
        stage_probe.progress=was_progress;strcpy(stage_probe.state,"RUNNING");
    }
    stage_probe.stage=8;assert(printer_effect(&stage_probe)==FX_FLOW);
    BambuState stopped;BambuState_Init(&stopped);
    const char *failure="{\"print\":{\"gcode_state\":\"FAILED\",\"print_error\":123}}";
    assert(BambuState_Parse(&stopped,failure,strlen(failure)));
    assert(printer_effect(&stopped)==FX_ERROR);
    const char *cleared="{\"print\":{\"print_error\":0}}";
    assert(BambuState_Parse(&stopped,cleared,strlen(cleared)));
    assert(!strcmp(stopped.state,"FAILED") && printer_effect(&stopped)==FX_STILL);
    assert(!strcmp(printer_status(&stopped,stage_name,sizeof(stage_name)),"STOPPED"));
    BambuState saved_slots[2]={printer_slots[0],printer_slots[1]};
    printer_slots[0]=stopped;printer_slots[1].online=false;
    edge_process();assert(edge_color==0);
    printer_slots[0].error=123;edge_process();assert(edge_color==0xEF4248);
    printer_slots[0].error=0;edge_process();assert(edge_color==0);
    printer_slots[0]=saved_slots[0];printer_slots[1]=saved_slots[1];

    /* Full-card coverage, repeatable frames, and actual spatial blur under text. */
    PrinterPage *water_page=&printer_pages[printer_slot];
    uint8_t water_expected[PARTICLE_WIDTH*PARTICLE_HEIGHT];
    assert(lv_obj_get_x(water_page->fill)==0 && lv_obj_get_y(water_page->fill)==0);
    assert(lv_obj_get_width(water_page->fill)==PARTICLE_WIDTH && lv_obj_get_height(water_page->fill)==PARTICLE_HEIGHT);
    assert(water_page->water.header.w==PARTICLE_WIDTH && water_page->water.header.h==PARTICLE_HEIGHT);
    water_page->stage_changed=0;water_page->previous_effect=FX_STILL;
    water_page->view.stage=9;water_page->motion_ms=2800;
    render_printer_water(water_page);
    memcpy(water_expected,water_page->water.data,sizeof(water_expected));
    for(int band=0;band<3;band++){
        int energy=0,y0=band*PARTICLE_HEIGHT/3,y1=(band+1)*PARTICLE_HEIGHT/3;
        for(int y=y0;y<y1;y++)for(int x=12;x<PARTICLE_WIDTH-12;x++)energy+=water_expected[y*PARTICLE_WIDTH+x];
        assert(energy>0);
    }
    render_printer_water(water_page);
    assert(!memcmp(water_expected,water_page->water.data,sizeof(water_expected)));
    uint8_t *alpha=(uint8_t *)water_page->water.data;
    assert(alpha[0]==0 && alpha[PARTICLE_WIDTH-1]==0);
    assert(alpha[(PARTICLE_HEIGHT-1)*PARTICLE_WIDTH]==0 && alpha[PARTICLE_HEIGHT*PARTICLE_WIDTH-1]==0);
    int gutter=0,gutter_r=0,hot=0,rim=0;
    for(int y=20;y<80;y++)for(int x=0;x<8;x++)gutter+=alpha[y*PARTICLE_WIDTH+x];
    for(int y=20;y<80;y++)for(int x=PARTICLE_WIDTH-8;x<PARTICLE_WIDTH;x++)gutter_r+=alpha[y*PARTICLE_WIDTH+x];
    assert(gutter>0 && gutter_r>0);
    for(int y=0;y<PARTICLE_HEIGHT;y++)for(int x=0;x<PARTICLE_WIDTH;x++){
        int e=LV_MIN(LV_MIN(x,PARTICLE_WIDTH-1-x),LV_MIN(y,PARTICLE_HEIGHT-1-y));
        if(e>=2 && e<8){rim++;if(alpha[y*PARTICLE_WIDTH+x]>200)hot++;}
    }
    assert(hot*5<rim);
    assert(water_page->fog_weights[78*PARTICLE_WIDTH+238]==0); /* Empty space has no rectangular fog. */
    assert(lv_obj_get_height(water_page->bar)==16);
    assert(lv_obj_get_style_text_font(water_page->metrics[0],0)==&lv_font_inter_28);
    assert(lv_obj_get_y(water_page->metrics[0])==222);
    assert(water_page->fog_weights[30*PARTICLE_WIDTH+34]>80); /* Status icon. */
    assert(water_page->fog_weights[208*PARTICLE_WIDTH+56]>80); /* Progress bar. */
    assert(water_page->fog_weights[184*PARTICLE_WIDTH+56]>40); /* Bar fog extends ~2.5x. */
    assert(water_page->fog_weights[236*PARTICLE_WIDTH+44]>40); /* Remaining time. */
    printer_text(water_page->percent,"0");water_page->text_cache[1].text[0]=0;printer_cache_text(water_page);
    int fog0=0;for(int x=0;x<PARTICLE_WIDTH;x++)if(water_page->fog_weights[100*PARTICLE_WIDTH+x]>80)fog0=x;
    printer_text(water_page->percent,"100");water_page->text_cache[1].text[0]=0;printer_cache_text(water_page);
    int fog100=0;for(int x=0;x<PARTICLE_WIDTH;x++)if(water_page->fog_weights[100*PARTICLE_WIDTH+x]>80)fog100=x;
    assert(fog100>fog0+10);
    printer_text(water_page->percent,"64");water_page->text_cache[1].text[0]=0;printer_cache_text(water_page);
    assert(water_page->fog_weights[100*PARTICLE_WIDTH+48]>80);
    memset(alpha,90,water_page->water.data_size);
    particle_soften(water_page);
    assert(alpha[100*PARTICLE_WIDTH+48]<alpha[78*PARTICLE_WIDTH+238]*2/3);
    memset(water_page->fog_weights,255,PARTICLE_WIDTH*PARTICLE_HEIGHT);
    memset(alpha,0,water_page->water.data_size);alpha[120*PARTICLE_WIDTH+120]=255;
    particle_soften(water_page);
    assert(alpha[120*PARTICLE_WIDTH+120]>0 && alpha[120*PARTICLE_WIDTH+120]<100);
    assert(alpha[120*PARTICLE_WIDTH+122]>0); /* Blur spreads energy, unlike a brightness mask. */
    water_page->text_cache[0].text[0]=0;printer_cache_text(water_page);
    WaterCluster drop_a=water_cluster(1234,2,0,9000,160,2,4300u),drop_b=water_cluster(5678,2,0,9000,160,2,4300u);
    assert(drop_a.x!=drop_b.x || drop_a.age!=drop_b.age);
    WaterCluster quiet=water_cluster(999,3,0,9000,160,1,5200u);
    WaterCluster ludicrous=water_cluster(999,3,0,9000,160,4,2500u);
    assert(quiet.fall>ludicrous.fall);
    assert(ludicrous.count>quiet.count);
    water_page->view.progress=45;water_page->view.stage=-1;strcpy(water_page->view.state,"RUNNING");
    water_page->motion_ms=4000;render_printer_water(water_page);
    assert(alpha[0]==0 && alpha[PARTICLE_WIDTH-1]==0 && alpha[PARTICLE_HEIGHT*PARTICLE_WIDTH-1]==0);
    int bottom=0;for(int x=40;x<PARTICLE_WIDTH-40;x++)bottom+=alpha[(PARTICLE_HEIGHT-12)*PARTICLE_WIDTH+x];
    assert(bottom>0);
    for(int percent=5;percent<=95;percent+=45){
        water_page->view.progress=percent;water_page->view.stage=-1;
        strcpy(water_page->view.state,"RUNNING");
        for(int ms=1200;ms<18000;ms+=613){
            water_page->motion_ms=ms;render_printer_water(water_page);
            memcpy(water_expected,water_page->water.data,sizeof(water_expected));
            render_printer_water(water_page);
            assert(!memcmp(water_expected,water_page->water.data,sizeof(water_expected)));
        }
    }
    PortableUI_SetPrinter(&printer);
    water_page->motion_ms=0;
    for(int frame=0;frame<16;frame++){
        advance(160);char name[40];snprintf(name,sizeof(name),"led-cluster-%02d",frame);snapshot(name);
    }
    strcpy(printer.state,"PAUSE");PortableUI_SetPrinter(&printer);advance(50);snapshot("printer-paused");
    uint32_t paused_phase=printer_pages[printer_slot].motion_ms;advance(200);
    assert(printer_pages[printer_slot].motion_ms>paused_phase);
    strcpy(printer.state,"FINISH");printer.progress=100;printer.remaining=0;
    PortableUI_SetPrinter(&printer);advance(50);check_labels(ui_root);snapshot("printer-finished");
    assert(!strcmp(lv_label_get_text(printer_pages[printer_slot].metrics[0]),"AWAIT AN\nOPPORTUNITY"));
    {
        PrinterTextCache *finish=&printer_pages[printer_slot].text_cache[3];
        const lv_font_t *font=&lv_font_inter_28;
        int uy=font->line_height+font->line_height-font->base_line-font->underline_position;
        const uint8_t *pix=finish->image.data;int w=finish->image.header.w;
        int ink=0;for(int x=0;x<w;x++)if(pix[uy*w+x]>200)ink++;
        assert(ink>40);
    }
    assert(lv_obj_get_style_text_font(printer_pages[printer_slot].metrics[0],0)==&lv_font_inter_28);
    assert(lv_obj_get_style_text_color(printer_pages[printer_slot].metrics[0],0).full==lv_color_hex(COLOR_MUTED).full);
    assert(lv_obj_get_x(printer_pages[printer_slot].percent)==20);
    strcpy(printer.state,"RUNNING");printer.remaining=650;PortableUI_SetPrinter(&printer);advance(50);
    assert(!strcmp(lv_label_get_text(printer_pages[printer_slot].metrics[0]),"10H 50M"));
    assert(lv_obj_get_style_text_font(printer_pages[printer_slot].metrics[0],0)==&lv_font_inter_28);
    printer.remaining=1263;PortableUI_SetPrinter(&printer);advance(50);
    assert(!strcmp(lv_label_get_text(printer_pages[printer_slot].metrics[0]),"21H 03M"));
    printer.remaining=6000;PortableUI_SetPrinter(&printer);advance(50);
    assert(!strcmp(lv_label_get_text(printer_pages[printer_slot].metrics[0]),"100H 00M"));
    check_labels(ui_root);
    advance(1000);uint32_t finish_phase=printer_pages[printer_slot].motion_ms;advance(200);
    assert(printer_pages[printer_slot].motion_ms>finish_phase);
    printer.progress=0;strcpy(printer.state,"PREPARE");PortableUI_SetPrinter(&printer);advance(100);
    uint32_t prepare_phase=printer_pages[printer_slot].motion_ms;advance(200);
    assert(printer_pages[printer_slot].motion_ms>prepare_phase);snapshot("printer-prepare");
    printer.progress=45;printer.error=1;strcpy(printer.state,"FAILED");PortableUI_SetPrinter(&printer);advance(100);
    uint32_t error_phase=printer_pages[printer_slot].motion_ms;advance(200);
    assert(printer_pages[printer_slot].motion_ms>error_phase);snapshot("printer-failed-still");
    printer.error=0;
    printer.online=false;PortableUI_SetPrinter(&printer);advance(50);snapshot("printer-disconnected");
    BambuState h2d, mini;BambuState_Init(&h2d);BambuState_Init(&mini);
    const char *dual="{\"print\":{\"gcode_state\":\"RUNNING\",\"mc_percent\":45,\"device\":{\"extruder\":{\"state\":16,\"info\":[{\"id\":0,\"temp\":16056565},{\"id\":1,\"temp\":14418140,\"snow\":1}]}}}}";
    assert(BambuState_Parse(&h2d,dual,strlen(dual)));
    assert(h2d.dual_nozzle && h2d.active_nozzle==1 && h2d.nozzle_current[0]==245 && h2d.nozzle_current[1]==220);
    assert(h2d.nozzle==220 && h2d.active_tray==1);
    const char *ext_spool="{\"print\":{\"ams\":{\"tray_now\":\"254\"},\"vir_slot\":[{\"id\":254,\"tray_type\":\"PLA\",\"tray_info_idx\":\"GFS05\",\"tray_sub_brands\":\"Support for PLA/PETG\",\"tray_color\":\"111111FF\",\"remain\":80},{\"id\":255,\"tray_type\":\"PLA\",\"tray_color\":\"E64A43FF\",\"remain\":40}]}}";
    BambuState ext;BambuState_Init(&ext);
    assert(BambuState_Parse(&ext,ext_spool,strlen(ext_spool)));
    assert(ext.active_tray==254);
    int left=-1,right=-1;for(int i=0;i<BAMBU_TRAYS;i++){
        if(ext.trays[i].present && ext.trays[i].unit==254)left=i;
        if(ext.trays[i].present && ext.trays[i].unit==255)right=i;
    }
    assert(left>=0 && !strcmp(ext.trays[left].material,"SUP P/G") && ext.trays[left].remaining==80);
    assert(right>=0 && !strcmp(ext.trays[right].material,"PLA") && ext.trays[right].remaining==40);
    const char *vt_only="{\"print\":{\"vt_tray\":{\"id\":254,\"tray_type\":\"PETG\",\"tray_color\":\"FFFF00FF\",\"remain\":-1}}}";
    BambuState vt;BambuState_Init(&vt);
    assert(BambuState_Parse(&vt,vt_only,strlen(vt_only)));
    int ext_i=-1;for(int i=0;i<BAMBU_TRAYS;i++)if(vt.trays[i].present && vt.trays[i].unit==254)ext_i=i;
    assert(ext_i>=0 && !strcmp(vt.trays[ext_i].material,"PETG") && vt.trays[ext_i].remaining==-1);
    strcpy(h2d.name,"H2D / 双材料支架打印文件");h2d.bed=70;h2d.bed_target=70;h2d.layer=192;h2d.layers=300;h2d.remaining=83;h2d.fan=80;
    h2d.chamber=45;h2d.speed_level=2;h2d.door_open=0;
    memcpy(h2d.trays,printer.trays,sizeof(h2d.trays));
    assert(BambuState_Parse(&mini,report,strlen(report)));mini.progress=73;mini.nozzle=220;mini.nozzle_target=220;
    strcpy(mini.name,"A1 mini / 桌面收纳盒");
    PortableUI_SetPrinterSlot(0,&h2d);PortableUI_SetPrinterSlot(1,&mini);advance(650);
    assert(printer_pages[printer_slot].view.progress==45 && printer_slots[1].progress==73);
    assert(!strcmp(lv_label_get_text(printer_pages[printer_slot].h2d_metrics[0]),"220/220"));
    assert(!strcmp(lv_label_get_text(printer_pages[printer_slot].h2d_metrics[1]),"245/245"));
    assert(!strcmp(lv_label_get_text(printer_pages[printer_slot].h2d_metrics[5]),"STANDARD"));
    assert(lv_obj_get_style_text_color(printer_pages[printer_slot].metrics[0],0).full==lv_color_hex(COLOR_MUTED).full);
    h2d.active_tray=254;
    memcpy(h2d.trays[15].material,"SUP P/G",8);h2d.trays[15].present=true;h2d.trays[15].unit=254;h2d.trays[15].slot=0;h2d.trays[15].remaining=80;h2d.trays[15].color=0x111111;
    PortableUI_SetPrinterSlot(0,&h2d);advance(50);
    assert(!strcmp(lv_label_get_text(printer_pages[0].ext_label),"SUP P/G"));
    assert(!strcmp(lv_label_get_text(printer_pages[0].ext_remain),"80%"));
    assert(lv_obj_get_width(printer_pages[0].tray_leds[0])==71);
    assert(lv_obj_get_width(printer_pages[1].tray_leds[0])==89);
    assert(lv_obj_get_x(printer_pages[0].tray_leds[0])==6);
    assert(lv_obj_get_x(printer_pages[1].tray_leds[0])==8);
    assert(lv_obj_get_x(printer_pages[0].bar)==20);
    assert(lv_obj_get_x(printer_pages[0].door)==632);
    assert(printer_pages[0].lane && lv_obj_get_y(printer_pages[0].lane)==lv_obj_get_y(printer_pages[0].ams));
    assert(lv_obj_get_style_text_font(printer_pages[0].lane,0)==lv_obj_get_style_text_font(printer_pages[0].ams,0));
    assert(lv_obj_get_height(printer_pages[0].lane)==lv_obj_get_height(printer_pages[0].ams));
    check_labels(ui_root);snapshot("printer-h2d");
    static lv_color_t partial_frame[480*800];memcpy(partial_frame,frame,sizeof(frame));
    lv_obj_invalidate(ui_root);lv_refr_now(display);snapshot("printer-h2d-full");
    /* The left animation can advance between captures; unchanged right-side text must match. */
    for(int y=136;y<480;y++)for(int x=300;x<800;x++)
        assert(partial_frame[(799-x)*480+y].full==frame[(799-x)*480+y].full);

    /* MQTT must not trigger particle calculation or label mutation for unchanged data. */
    uint32_t renders=printer_pages[0].particle_renders,rasters=printer_pages[0].text_rasters,labels=printer_label_updates;
    PortableUI_SetPrinterSlot(0,&h2d);
    assert(printer_pages[0].particle_renders==renders && printer_pages[0].text_rasters==rasters && printer_label_updates==labels);
    h2d.bed++;PortableUI_SetPrinterSlot(0,&h2d);
    assert(printer_pages[0].particle_renders==renders && printer_pages[0].text_rasters==rasters);
    advance(400);assert(printer_pages[0].text_rasters==rasters);
    assert(printer_pages[0].particle_renders-renders<=6);
    uint32_t a1_renders=printer_pages[1].particle_renders;
    lv_event_send(nav_buttons[1],LV_EVENT_CLICKED,NULL);
    assert(printer_pages[1].particle_renders==a1_renders); /* Water is not rebuilt on the click. */
    advance(650);
    assert(printer_slot==1 && printer_pages[printer_slot].view.progress==73);check_labels(ui_root);snapshot("printer-a1mini");
    renders=printer_pages[0].particle_renders;rasters=printer_pages[0].text_rasters;
    h2d.progress=46;h2d.nozzle_current[1]=221;PortableUI_SetPrinterSlot(0,&h2d);advance(200);
    assert(printer_pages[0].particle_renders==renders && printer_pages[0].text_rasters==rasters);
    assert(!strcmp(lv_label_get_text(printer_pages[0].h2d_metrics[0]),"220/220"));
    activate_page(0,true);
    assert(lv_obj_get_x(printer_pages[0].tile)==0 && lv_obj_has_flag(printer_pages[1].tile,LV_OBJ_FLAG_HIDDEN));
    assert(!strcmp(lv_label_get_text(printer_pages[0].h2d_metrics[0]),"221/220"));
    activate_page(1,false);
    mini.online=false;PortableUI_SetPrinterSlot(1,&mini);advance(50);
    assert(printer_slots[0].online && !printer_slots[1].online);snapshot("printer-a1mini-offline");
    advance(186000);assert(standby_active);snapshot("standby-printer-offline");
    mini.online=true;PortableUI_SetPrinterSlot(1,&mini);advance(181000);
    assert(standby_active && standby_print_visible(&h2d) && standby_print_visible(&mini));
    check_labels(ui_root);snapshot("standby-printers");
    mini.progress=0;PortableUI_SetPrinterSlot(1,&mini);advance(50);snapshot("standby-printer-zero");
    mini.progress=100;PortableUI_SetPrinterSlot(1,&mini);advance(50);snapshot("standby-printer-full");
    strcpy(mini.state,"IDLE");PortableUI_SetPrinterSlot(1,&mini);advance(50);
    assert(!standby_print_visible(&mini) && lv_obj_has_flag(standby_printer_labels[1],LV_OBJ_FLAG_HIDDEN));
    PortableUI_Activity();advance(650);
    for(int sample=0;sample<100;sample++) {
    event_ribbons_reset();
    int covered=0;
    for(int i=0;i<8;i++) {
        assert(event_ribbons[i].top==covered && event_ribbons[i].height>0);
        covered+=event_ribbons[i].height;
        assert(event_ribbons[i].gap>0 && event_ribbons[i].gap<event_ribbons[i].height);
        assert(event_ribbon_y(i,0)==480 && event_ribbon_y(i,900)==event_ribbons[i].top);
        for(unsigned age=1;age<=900;age++)assert(event_ribbon_y(i,age)<=event_ribbon_y(i,age-1));
    }
    assert(covered==480);
    for(int y=0;y<480;y++)assert(!event_gap_row(y,900));
    }
    assert(event_gap_height(0)==24 && event_gap_height(600)==24);
    assert(event_gap_height(810)==12 && event_gap_height(900)==0);
    assert(event_ribbon_progress(110)==event_ribbon_progress(150));
    assert(event_ribbon_progress(340)==event_ribbon_progress(380));
    /* Event edges, warning priority and real rendered animation frames. */
    assert(!event_playing);
    h2d.error=0;strcpy(h2d.state,"IDLE");PortableUI_SetPrinterSlot(0,&h2d);
    strcpy(h2d.state,"RUNNING");PortableUI_SetPrinterSlot(0,&h2d);
    assert(event_playing && event_current.kind==EVENT_START);
    uint32_t first_started=event_started;
    PortableUI_SetPrinterSlot(0,&h2d);assert(event_started==first_started && event_count==0);
    const char *event_names[]={"start","pause","resume","warning","complete","offline"};
    for(int scenario=0;scenario<6;scenario++) {
        if(scenario==1){strcpy(h2d.state,"PAUSE");PortableUI_SetPrinterSlot(0,&h2d);}
        if(scenario==2){strcpy(h2d.state,"RUNNING");PortableUI_SetPrinterSlot(0,&h2d);}
        if(scenario==3){h2d.error=0x07008011;PortableUI_SetPrinterSlot(0,&h2d);}
        if(scenario==4){h2d.error=0;strcpy(h2d.state,"FINISH");PortableUI_SetPrinterSlot(0,&h2d);}
        if(scenario==5){h2d.online=false;PortableUI_SetPrinterSlot(0,&h2d);}
        assert(event_playing);
        for(int f=0;f<40;f++) {
            advance(100);check_labels(ui_root);
            char filename[96];snprintf(filename,sizeof(filename),"event-%s-%02d",event_names[scenario],f);snapshot(filename);
        }
        advance(300);assert(!event_playing && lv_obj_has_flag(event_layer,LV_OBJ_FLAG_HIDDEN));
    }
    h2d.online=true;strcpy(h2d.state,"IDLE");PortableUI_SetPrinterSlot(0,&h2d);
    mini.online=true;strcpy(mini.state,"IDLE");PortableUI_SetPrinterSlot(1,&mini);
    strcpy(h2d.state,"RUNNING");PortableUI_SetPrinterSlot(0,&h2d);
    strcpy(mini.state,"PAUSE");PortableUI_SetPrinterSlot(1,&mini);assert(event_count==1);
    h2d.error=23;PortableUI_SetPrinterSlot(0,&h2d);
    assert(event_current.kind==EVENT_WARNING && event_count==1);
    advance(EVENT_DURATION+50);assert(event_current.kind==EVENT_PAUSE && event_current.slot==1);
    advance(EVENT_DURATION+50);assert(!event_playing);
    for(int i=0;i<20;i++)event_enqueue((PrinterEvent){i%2,EVENT_START,(uint32_t)i});
    assert(event_count<=8);
    event_enqueue((PrinterEvent){0,EVENT_WARNING,77});assert(event_current.kind==EVENT_WARNING);
    event_playing=false;event_count=0;lv_obj_add_flag(event_layer,LV_OBJ_FLAG_HIDDEN);
    assert(lv_font_cjk_24.get_glyph_bitmap(&lv_font_cjk_24,0x68C0)!=NULL);
    TodoState todos={.online=true,.next={.id=31,.title="整理明天的打印材料与工具"},.next_at=1788774000,.pages=1,.total=3,.count=3,.items={
        {.id=11,.title="检查 H2D 打印任务"},
        {.id=12,.title="整理桌面，备份今天的项目"},
        {.id=13,.title="测试中文与 English 混合显示，以及较长标题的自动换行和省略显示"}}};
    PortableUI_SetTodoHandler(todo_test_request);PortableUI_SetTodo(&todos);
    lv_event_send(nav_buttons[3],LV_EVENT_CLICKED,NULL);
    PortableUI_Activity();advance(100);
    assert(selected_page==3 && lv_obj_get_x(todo_tile)==0);
    snapshot("tab-to-todo");advance(550);check_labels(ui_root);snapshot("todo");
    assert(lv_obj_has_flag(todo_pager,LV_OBJ_FLAG_HIDDEN));
    TodoState paged=todos;paged.pages=3;paged.total=19;
    PortableUI_SetTodo(&paged);advance(50);
    assert(!lv_obj_has_flag(todo_pager,LV_OBJ_FLAG_HIDDEN));
    assert(!strcmp(lv_label_get_text(todo_page_label),"1 / 3"));
    advance(300);snapshot("todo-pagination");PortableUI_SetTodo(&todos);advance(50);

    assert(lv_obj_get_x(todo_tile)==0 && lv_obj_get_x(nav_line)==496);
    /* Fast tab reversals finish with exactly one visible page and a matching indicator. */
    lv_event_send(nav_buttons[2],LV_EVENT_CLICKED,NULL);advance(75);
    lv_event_send(nav_buttons[0],LV_EVENT_CLICKED,NULL);advance(350);
    assert(selected_page==0 && lv_obj_get_x(printer_pages[printer_slot].tile)==0);
    assert(lv_obj_has_flag(codex_tile,LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(todo_tile,LV_OBJ_FLAG_HIDDEN));
    lv_event_send(nav_buttons[3],LV_EVENT_CLICKED,NULL);advance(350);
    lv_event_send(lv_obj_get_child(todo_list,0),LV_EVENT_CLICKED,NULL);
    assert(todo_requested_id==0 && !todo_pending); // Whole row is never completion.
    lv_obj_t *complete_button=lv_obj_get_child(lv_obj_get_child(todo_list,0),1);
    touch_position=(lv_point_t){238,62};pressed=true;advance(75);
    touch_position.x=174;advance(75);pressed=false;advance(600);
    assert(todo_requested_id==0 && !todo_pending); // Actual drag beginning on the check.
    lv_obj_scroll_to_y(todo_list,0,LV_ANIM_OFF);advance(100);
    touch_position=(lv_point_t){238,62};pressed=true;advance(75);pressed=false;advance(100);
    touch_position=(lv_point_t){300,399};
    assert(todo_requested_id==11 && todo_pending);
    PortableUI_SetTodo(&todos);assert(todo_pending); // Old poll cannot acknowledge the click.
    todos.total=2;todos.count=2;todos.completed_id=11;PortableUI_SetTodo(&todos);assert(!todo_pending && todo_success_started);
    advance(150);snapshot("todo-completed-effect");
    todos.done=true;todos.completed_id=0;for(int i=0;i<todos.count;i++)todos.items[i].done=true;
    PortableUI_SetTodo(&todos);advance(350);snapshot("todo-today-done");
    todos.online=false;PortableUI_SetTodo(&todos);advance(50);
    assert(lv_obj_has_state(lv_obj_get_child(lv_obj_get_child(todo_list,0),1),LV_STATE_DISABLED));snapshot("todo-offline");
    PortableUI_Codex quota={.remaining={NAN,NAN}};
    const char *quota_json="{\"codex_usage\":{\"ok\":true,\"plan_type\":\"plus\",\"allowed\":true,\"primary_window\":{\"used_percent\":16,\"limit_window_seconds\":18000,\"reset_at\":1788839297},\"secondary_window\":{\"used_percent\":83,\"limit_window_seconds\":604800,\"reset_at\":1789220422}}}";
    assert(PortableUI_ParseCodexJSON(quota_json,&quota));
    assert(quota.remaining[0]==17 && quota.remaining[1]==84 && quota.short_seconds==18000);
    status.nas_online=false;PortableUI_SetStatus(&status);
    PortableUI_SetCodex(&quota);assert(codex_connected);
    assert(!strcmp(lv_label_get_text(codex_state_label),"LIVE"));
    assert(!strcmp(lv_label_get_text(codex_plan_label),"PLUS"));
    lv_event_send(nav_buttons[2],LV_EVENT_CLICKED,NULL);advance(50);check_labels(ui_root);snapshot("codex-quota");
    assert(!PortableUI_ParseCodexJSON(NULL,&quota) && quota.remaining[0]==17 && !quota.online);
    PortableUI_SetCodex(&quota);advance(50);snapshot("codex-quota-offline");
    assert(!codex_connected && !strcmp(lv_label_get_text(codex_state_label),"STALE"));
    advance(EVENT_DURATION+50);
    PortableUI_SetStandby(true);advance(650);
    assert(!PortableUI_ShowReminder("点晚餐：检查今天的待办提醒"));
    assert(!standby_active && event_current.kind==EVENT_TODO);
    advance(EVENT_ENTER_MS+200);
    assert(PortableUI_ShowReminder("点晚餐：检查今天的待办提醒"));
    check_labels(ui_root);snapshot("todo-reminder");
    assert(!PortableUI_ShowReminder("第二条提醒"));
    advance(10000);
    assert(!PortableUI_ShowReminder("第二条提醒"));
    advance(EVENT_ENTER_MS+200);
    assert(PortableUI_ShowReminder("第二条提醒"));
    assert(!strcmp(lv_label_get_text(event_detail),"第二条提醒"));
    advance(11000);
    PortableUI_SetVideoHandler(video_test_request);
    static uint16_t camera_pixels[800*480], second_pixels[800*480];
    for(int slot=0;slot<2;slot++){
        lv_event_send(printer_pages[slot].play,LV_EVENT_CLICKED,NULL);
        assert(video_active && video_requested_slot==slot);
        int releases_before=video_released;
        PortableUI_SetVideoFrame(camera_pixels,video_test_release);
        assert(video_released==releases_before);
        PortableUI_SetVideoFrame(second_pixels,video_test_release);
        assert(video_released==releases_before+1 && video_frame.data==(const uint8_t *)second_pixels);
        assert(video_frame.data && !lv_obj_has_flag(video_layer,LV_OBJ_FLAG_HIDDEN));
        event_begin((PrinterEvent){.slot=slot,.kind=slot?EVENT_WARNING:EVENT_TODO,.code=23});
        assert(!video_active && video_requested_slot==-1 && !video_frame.data);
        assert(video_released==releases_before+2);
        assert(lv_obj_has_flag(video_layer,LV_OBJ_FLAG_HIDDEN));
        assert(!lv_obj_has_flag(event_layer,LV_OBJ_FLAG_HIDDEN));
        PortableUI_SetVideoFrame(camera_pixels,video_test_release);assert(!video_frame.data);
        assert(video_released==releases_before+3);
        advance(1200);snapshot(slot?"video-to-warning":"video-to-reminder");
        advance(11000);
    }
    strcpy(h2d.state,"PAUSE");h2d.error=0;PortableUI_SetPrinterSlot(0,&h2d);
    advance(EVENT_DURATION+50);advance(1500);snapshot("edge-paused");
    h2d.error=23;PortableUI_SetPrinterSlot(0,&h2d);
    advance(EVENT_DURATION+50);advance(1500);snapshot("edge-error");
    puts("PASS: actual LVGL render, labels, touch, idle, parser, notifications and both video interruption paths");
}
