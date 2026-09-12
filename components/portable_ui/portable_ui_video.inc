/* Retain the producer buffer until replacement/exit. No full-frame CPU copy. */
static lv_obj_t *video_layer,*video_image,*video_status,*video_exit;
static lv_img_dsc_t video_frame;
static void (*video_release)(const uint16_t *);
static void (*video_handler)(int);
static bool video_active;

static void release_held_video(void)
{
    if(!video_frame.data)return;
    lv_img_cache_invalidate_src(&video_frame);
    if(video_image)lv_obj_add_flag(video_image,LV_OBJ_FLAG_HIDDEN);
    video_release((const uint16_t *)video_frame.data);
    video_frame.data=NULL;
}
static void stop_video(void)
{
    if(!video_active)return;
    video_active=false;
    video_handler(-1);
    lv_obj_add_flag(video_layer,LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(control_layer,LV_OBJ_FLAG_HIDDEN);
    release_held_video();
}
static void close_video(lv_event_t *event)
{
    (void)event;stop_video();PortableUI_Activity();
}
static void play_video(lv_event_t *event)
{
    int slot=(intptr_t)lv_event_get_user_data(event);
    PortableUI_Activity();
    release_held_video();
    video_active=true;
    lv_obj_add_flag(control_layer,LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(video_status,slot==0?"HOST CONNECTING...":"CONNECTING...");
    lv_obj_clear_flag(video_status,LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(video_layer,LV_OBJ_FLAG_HIDDEN);lv_obj_move_foreground(video_layer);
    video_handler(slot);
}
void PortableUI_SetVideoHandler(void (*handler)(int))
{
    video_handler=handler;
    for(int i=0;i<2;i++)lv_obj_clear_state(printer_pages[i].play,LV_STATE_DISABLED);
}
void PortableUI_SetVideoStatus(const char *message)
{
    if(!video_active)return;
    lv_label_set_text(video_status,message);
    lv_obj_clear_flag(video_status,LV_OBJ_FLAG_HIDDEN);
    /* On failure remove the last frame; a frozen image must not look live. */
    if(video_image)lv_obj_add_flag(video_image,LV_OBJ_FLAG_HIDDEN);
}
void PortableUI_SetVideoFrame(const uint16_t *rgb565, void (*release)(const uint16_t *))
{
    if(!video_active){release(rgb565);return;}
    const uint16_t *previous=(const uint16_t *)video_frame.data;
    void (*previous_release)(const uint16_t *)=video_release;
    if(!video_image){
        video_image=lv_img_create(video_layer);lv_obj_set_pos(video_image,0,0);
        lv_obj_move_foreground(video_exit);lv_obj_clear_flag(video_image,LV_OBJ_FLAG_CLICKABLE);
    }
    lv_img_cache_invalidate_src(&video_frame);
    video_frame=(lv_img_dsc_t){.header={.cf=LV_IMG_CF_TRUE_COLOR,.w=PORTABLE_UI_VIDEO_WIDTH,.h=PORTABLE_UI_VIDEO_HEIGHT},
        .data_size=PORTABLE_UI_VIDEO_WIDTH*PORTABLE_UI_VIDEO_HEIGHT*2,.data=(const uint8_t *)rgb565};
    video_release=release;
    lv_img_set_src(video_image,&video_frame);
    lv_obj_clear_flag(video_image,LV_OBJ_FLAG_HIDDEN);lv_obj_invalidate(video_image);
    lv_obj_add_flag(video_status,LV_OBJ_FLAG_HIDDEN);
    // Previous image has already been rasterized into the display's own DMA buffers.
    if(previous)previous_release(previous);
}
static void create_video(void)
{
    video_layer=lv_obj_create(ui_root);set_plain_container(video_layer,COLOR_BACKGROUND);
    lv_obj_set_size(video_layer,800,480);lv_obj_add_flag(video_layer,LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *close=video_exit=lv_btn_create(video_layer);set_plain_container(close,COLOR_SURFACE);
    lv_obj_set_pos(close,684,430);lv_obj_set_size(close,92,44);lv_obj_set_style_radius(close,10,0);
    lv_obj_set_style_shadow_width(close,0,0);
    lv_obj_t *label=create_label(close,"EXIT",&lv_font_cjk_24,COLOR_WHITE);lv_obj_center(label);
    lv_obj_add_event_cb(close,close_video,LV_EVENT_CLICKED,NULL);
    video_status=create_label(video_layer,"",&lv_font_cjk_24,COLOR_MUTED);
    lv_obj_set_pos(video_status,24,224);lv_obj_set_size(video_status,752,64);
    lv_obj_set_style_text_align(video_status,LV_TEXT_ALIGN_CENTER,0);lv_label_set_long_mode(video_status,LV_LABEL_LONG_DOT);
}
