/* Two-column Todo view. Completion belongs only to the explicit check target. */
static lv_obj_t *todo_pager,*todo_page_label;
static lv_obj_t *todo_list,*todo_summary,*todo_prev,*todo_next,*todo_tabs[2];
static lv_obj_t *todo_preview,*todo_time,*todo_title,*todo_hint;
static TodoState todo_view;
static bool (*todo_handler)(int,int,int);
static int todo_filter,todo_pending_id;
static bool todo_pending,todo_dragged;
static lv_point_t todo_press;
static uint32_t todo_success_started;
static void todo_preview_draw(lv_event_t *e)
{
    lv_area_t a;lv_obj_get_coords(lv_event_get_target(e),&a);
    lv_draw_ctx_t *ctx=lv_event_get_draw_ctx(e);
    const unsigned age=lv_tick_elaps(todo_success_started);
    bool success=todo_success_started && age<900;
    for(int y=0;y<4;y++)for(int x=0;x<30;x++){
        int alpha=success ? LV_MAX(12,180-abs(x*24-(int)age)*2):18;
        printer_led(ctx,a.x1+18+x*8,a.y2-42+y*8,3,COLOR_BAMBU,alpha);
    }
}
static void todo_check_draw(lv_event_t *e)
{
    lv_area_t a;lv_obj_get_coords(lv_event_get_target(e),&a);
    bool done=(intptr_t)lv_event_get_user_data(e);
    static const uint16_t check[9]={0,0x100,0x180,0x0C1,0x063,0x036,0x01C,0x008,0};
    for(int y=0;y<9;y++)for(int x=0;x<9;x++){
        bool lit=done?(check[y]&(1<<x)):(x==0 || x==8 || y==0 || y==8);
        if(lit)printer_led(lv_event_get_draw_ctx(e),a.x1+10+x*3,a.y1+10+y*3,2,COLOR_BAMBU,done?150:230);
    }
}
void PortableUI_SetTodoHandler(bool (*handler)(int,int,int)) { todo_handler=handler; }
static void todo_request(lv_event_t *event)
{
    if(todo_pending)return;
    int action=(intptr_t)lv_event_get_user_data(event),page=todo_view.page,id=0,filter=todo_filter;
    if(action==-1)page--;else if(action==-2)page++;
    else if(action==-3 || action==-4){filter=action==-4;page=0;}
    else id=action;
    if(todo_handler && todo_handler(page,filter,id)){
        todo_filter=filter;todo_pending=true;todo_pending_id=id;
        lv_label_set_text(todo_summary,"同步中...");
        lv_obj_add_state(todo_prev,LV_STATE_DISABLED);lv_obj_add_state(todo_next,LV_STATE_DISABLED);
        for(int i=0;i<2;i++)lv_obj_add_state(todo_tabs[i],LV_STATE_DISABLED);
    }
    PortableUI_Activity();
}
static void todo_check_event(lv_event_t *e)
{
    lv_event_code_t code=lv_event_get_code(e);
    lv_indev_t *input=lv_indev_get_act();
    if(code==LV_EVENT_PRESSED){lv_indev_get_point(input,&todo_press);todo_dragged=lv_obj_is_scrolling(todo_list);}
    else if(code==LV_EVENT_PRESSING){
        lv_point_t point;lv_indev_get_point(input,&point);
        if(abs(point.x-todo_press.x)>8 || abs(point.y-todo_press.y)>8)todo_dragged=true;
    }else if(code==LV_EVENT_SHORT_CLICKED && !todo_dragged && !lv_obj_is_scrolling(todo_list))todo_request(e);
}
static void todo_scroll_event(lv_event_t *e)
{ (void)e;todo_dragged=true; }
static void todo_update_preview(void)
{
    if(todo_view.next.id){
        time_t at=todo_view.next_at;struct tm local;localtime_r(&at,&local);char text[32];
        strftime(text,sizeof(text),"%H:%M",&local);lv_label_set_text(todo_time,text);
        strftime(text,sizeof(text),"%m.%d",&local);lv_label_set_text(todo_hint,text);
        lv_label_set_text(todo_title,todo_view.next.title);
    }else{
        lv_label_set_text(todo_time,"--:--");lv_label_set_text(todo_hint,"可在网页设置提醒");
        lv_label_set_text(todo_title,todo_view.online?"暂无下一条提醒":"提醒服务离线");
    }
}
void PortableUI_SetTodo(const TodoState *state)
{
    if(!todo_pending && !memcmp(&todo_view,state,sizeof(*state)))return;
    if(todo_pending_id && state->completed_id!=todo_pending_id && state->failed_id!=todo_pending_id)return;
    bool completed=todo_pending_id && state->completed_id==todo_pending_id;
    todo_pending=false;todo_pending_id=0;todo_view=*state;
    if(!todo_tile)return;
    todo_filter=state->done;
    lv_obj_clean(todo_list);
    if(!state->online)lv_label_set_text(todo_summary,"等待连接");
    else lv_label_set_text_fmt(todo_summary,state->done?"今日完成 %d 项":"待办 %d 项",state->total);
    if(state->pages<=1 || !state->online)lv_obj_add_flag(todo_pager,LV_OBJ_FLAG_HIDDEN);
    else {
        lv_obj_clear_flag(todo_pager,LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(todo_page_label,"%d / %d",state->page+1,state->pages);
    }
    for(int i=0;i<2;i++){
        lv_obj_clear_state(todo_tabs[i],LV_STATE_DISABLED|LV_STATE_CHECKED);
        if(todo_filter==i)lv_obj_add_state(todo_tabs[i],LV_STATE_CHECKED);
    }
    lv_obj_clear_state(todo_prev,LV_STATE_DISABLED);lv_obj_clear_state(todo_next,LV_STATE_DISABLED);
    if(!state->online || state->page<=0)lv_obj_add_state(todo_prev,LV_STATE_DISABLED);
    if(!state->online || state->page+1>=state->pages)lv_obj_add_state(todo_next,LV_STATE_DISABLED);
    for(int i=0;i<state->count;i++){
        const TodoItem *item=&state->items[i];
        lv_obj_t *row=lv_obj_create(todo_list);set_plain_container(row,COLOR_SURFACE);
        lv_obj_set_size(row,460,76);lv_obj_set_style_radius(row,12,0);
        lv_obj_clear_flag(row,LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *title=printer_label(row,item->title,14,6,366,&lv_font_cjk_24,item->done?COLOR_MUTED:COLOR_WHITE);
        lv_obj_set_height(title,64);lv_label_set_long_mode(title,LV_LABEL_LONG_DOT);
        if(item->done)lv_obj_set_style_text_decor(title,LV_TEXT_DECOR_STRIKETHROUGH,0);
        lv_obj_t *check=lv_btn_create(row);set_plain_container(check,0x132019);
        lv_obj_set_pos(check,398,14);lv_obj_set_size(check,48,48);lv_obj_set_style_radius(check,12,0);lv_obj_set_style_shadow_width(check,0,0);
        lv_obj_add_event_cb(check,todo_check_draw,LV_EVENT_DRAW_MAIN,(void *)(intptr_t)item->done);
        lv_obj_add_event_cb(check,todo_check_event,LV_EVENT_ALL,(void *)(intptr_t)item->id);
        if(!state->online || item->done)lv_obj_add_state(check,LV_STATE_DISABLED);
    }
    if(!state->count)printer_label(todo_list,state->online?(state->done?"今天还没有完成的任务":"未完成任务已清空"):"等待服务恢复",12,18,424,&lv_font_cjk_24,COLOR_MUTED);
    todo_update_preview();
    if(completed){todo_success_started=lv_tick_get();lv_label_set_text(todo_hint,"已完成");lv_obj_invalidate(todo_preview);}
}
static lv_obj_t *todo_button(lv_obj_t *parent,const char *text,int x,int y,int width,int action)
{
    lv_obj_t *b=lv_btn_create(parent);set_plain_container(b,COLOR_SURFACE);
    lv_obj_set_pos(b,x,y);lv_obj_set_size(b,width,42);lv_obj_set_style_radius(b,10,0);lv_obj_set_style_shadow_width(b,0,0);
    lv_obj_set_style_bg_color(b,lv_color_hex(0x163520),LV_STATE_CHECKED);
    lv_obj_set_style_border_side(b,LV_BORDER_SIDE_BOTTOM,LV_STATE_CHECKED);
    lv_obj_set_style_border_width(b,3,LV_STATE_CHECKED);lv_obj_set_style_border_color(b,lv_color_hex(COLOR_BAMBU),LV_STATE_CHECKED);
    lv_obj_t *label=create_label(b,text,&lv_font_cjk_24,COLOR_WHITE);lv_obj_center(label);
    lv_obj_add_event_cb(b,todo_request,LV_EVENT_SHORT_CLICKED,(void *)(intptr_t)action);return b;
}
static void todo_process(void)
{
    if(!todo_success_started)return;
    if(lv_tick_elaps(todo_success_started)>=900){todo_success_started=0;todo_update_preview();}
    if(selected_page==3 && !standby_active && !video_active && !event_playing){
        lv_area_t a;lv_obj_get_coords(todo_preview,&a);a.y1=a.y2-48;lv_obj_invalidate_area(todo_preview,&a);
    }
}
static void create_todo(void)
{
    todo_tile=lv_obj_create(control_layer);set_plain_container(todo_tile,COLOR_BACKGROUND);
    lv_obj_set_pos(todo_tile,0,136);lv_obj_set_size(todo_tile,800,344);lv_obj_add_flag(todo_tile,LV_OBJ_FLAG_HIDDEN);
    todo_preview=lv_obj_create(todo_tile);set_plain_container(todo_preview,COLOR_SURFACE);
    lv_obj_set_pos(todo_preview,24,12);lv_obj_set_size(todo_preview,272,316);lv_obj_set_style_radius(todo_preview,16,0);
    lv_obj_add_event_cb(todo_preview,todo_preview_draw,LV_EVENT_DRAW_MAIN,NULL);
    printer_label(todo_preview,"下一条提醒",18,16,236,&lv_font_cjk_24,COLOR_MUTED);
    todo_time=printer_label(todo_preview,"--:--",18,60,236,&lv_font_inter_48,COLOR_WHITE);
    todo_title=printer_label(todo_preview,"等待提醒服务",18,116,236,&lv_font_cjk_24,COLOR_WHITE);
    lv_obj_set_height(todo_title,96);lv_label_set_long_mode(todo_title,LV_LABEL_LONG_DOT);
    todo_hint=printer_label(todo_preview,"",18,224,236,&lv_font_cjk_24,COLOR_MUTED);
    todo_tabs[0]=todo_button(todo_tile,"未完成",316,12,136,-3);
    todo_tabs[1]=todo_button(todo_tile,"今日完成",464,12,160,-4);
    lv_obj_add_state(todo_tabs[0],LV_STATE_CHECKED);
    lv_obj_t *footer=lv_obj_create(todo_tile);set_plain_container(footer,COLOR_SURFACE);
    lv_obj_set_pos(footer,316,290);lv_obj_set_size(footer,460,40);lv_obj_set_style_radius(footer,10,0);
    todo_summary=printer_label(footer,"等待连接",12,4,248,&lv_font_cjk_24,COLOR_MUTED);
    todo_pager=lv_obj_create(footer);set_plain_container(todo_pager,0x152219);
    lv_obj_set_pos(todo_pager,268,0);lv_obj_set_size(todo_pager,192,40);lv_obj_set_style_radius(todo_pager,10,0);
    todo_prev=todo_button(todo_pager,"<",0,0,44,-1);
    todo_next=todo_button(todo_pager,">",148,0,44,-2);
    lv_obj_set_height(todo_prev,40);lv_obj_set_height(todo_next,40);
    lv_obj_t *arrows[2]={todo_prev,todo_next};
    for(int i=0;i<2;i++){
        lv_obj_set_style_bg_color(arrows[i],lv_color_hex(0x193222),0);
        lv_obj_set_style_bg_color(arrows[i],lv_color_hex(0x245335),LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(arrows[i],lv_color_hex(0x152219),LV_STATE_DISABLED);
        lv_obj_set_style_opa(arrows[i],100,LV_STATE_DISABLED);
    }

    todo_page_label=printer_label(todo_pager,"",46,6,100,&lv_font_inter_24,COLOR_WHITE);
    lv_obj_set_style_text_align(todo_page_label,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_add_flag(todo_pager,LV_OBJ_FLAG_HIDDEN);
    todo_list=lv_obj_create(todo_tile);set_plain_container(todo_list,COLOR_BACKGROUND);
    lv_obj_set_pos(todo_list,316,64);lv_obj_set_size(todo_list,460,218);
    lv_obj_add_flag(todo_list,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_scroll_dir(todo_list,LV_DIR_VER);
    lv_obj_set_flex_flow(todo_list,LV_FLEX_FLOW_COLUMN);lv_obj_set_style_pad_row(todo_list,10,0);
    lv_obj_add_event_cb(todo_list,todo_scroll_event,LV_EVENT_SCROLL_BEGIN,NULL);
}
