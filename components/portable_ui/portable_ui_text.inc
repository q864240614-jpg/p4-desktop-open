/* Label mutations are gated before entering LVGL, including formatted text. */
static uint32_t printer_label_updates;
static bool printer_text(lv_obj_t *label,const char *text)
{
    if(!strcmp(lv_label_get_text(label),text))return false;
    printer_label_updates++;lv_label_set_text(label,text);return true;
}
static bool printer_text_fmt(lv_obj_t *label,const char *format,...)
{
    char text[192];va_list args;va_start(args,format);vsnprintf(text,sizeof(text),format,args);va_end(args);
    return printer_text(label,text);
}
static void printer_text_color(lv_obj_t *label,uint32_t color)
{
    lv_color_t next=lv_color_hex(color);
    if(lv_obj_get_style_text_color(label,0).full!=next.full)lv_obj_set_style_text_color(label,next,0);
}
static void printer_text_draw(lv_event_t *event)
{
    PrinterPage *p=lv_event_get_user_data(event);lv_area_t card;lv_obj_get_coords(p->card,&card);
    for(int i=0;i<4;i++){
        PrinterTextCache *cache=&p->text_cache[i];
        lv_area_t a={card.x1+lv_obj_get_x(cache->label),card.y1+lv_obj_get_y(cache->label),0,0};
        a.x2=a.x1+cache->image.header.w-1;a.y2=a.y1+cache->image.header.h-1;
        lv_draw_img_dsc_t d;lv_draw_img_dsc_init(&d);
        d.recolor=lv_obj_get_style_text_color(cache->label,0);d.recolor_opa=LV_OPA_COVER;
        lv_draw_img(lv_event_get_draw_ctx(event),&d,&a,&cache->image);
    }
}
static void printer_fog_stamp(PrinterPage *p,int x0,int y0,int x1,int y1,int strength)
{
    if(x1<x0 || y1<y0)return;
    x0=LV_MAX(0,x0);y0=LV_MAX(0,y0);x1=LV_MIN(PARTICLE_WIDTH-1,x1);y1=LV_MIN(PARTICLE_HEIGHT-1,y1);
    for(int y=y0;y<=y1;y++){
        int yedge=LV_MIN(y-y0,y1-y);
        int yfade=yedge>=5?256:(yedge+1)*256/6;
        for(int x=x0;x<=x1;x++){
            int xedge=LV_MIN(x-x0,x1-x);
            int weight=strength*LV_MIN(12,xedge+1)/12*yfade/256;
            if(p->fog_weights[y*PARTICLE_WIDTH+x]<weight)p->fog_weights[y*PARTICLE_WIDTH+x]=weight;
        }
    }
}
static void printer_cache_text(PrinterPage *p)
{
    bool changed=false;
    for(int i=0;i<4;i++){
        PrinterTextCache *cache=&p->text_cache[i];const char *text=lv_label_get_text(cache->label);
        if(!strcmp(cache->text,text))continue;
        changed=true;p->text_rasters++;snprintf(cache->text,sizeof(cache->text),"%s",text);
        uint8_t *pixels=(uint8_t *)cache->image.data;memset(pixels,0,cache->image.data_size);
        const lv_font_t *font=lv_obj_get_style_text_font(cache->label,0);
        int pen=0,row=0,width=cache->image.header.w,height=cache->image.header.h,tracking=lv_obj_get_style_text_letter_space(cache->label,0);
        int line_left=-1,line_right=-1;
        uint32_t pos=0;
        while(text[pos]){
            uint32_t code=_lv_txt_encoded_next(text,&pos);
            if(code=='\n'){pen=0;row++;continue;}
            uint32_t look=pos,next=_lv_txt_encoded_next(text,&look);
            lv_font_glyph_dsc_t g;bool found=lv_font_get_glyph_dsc(font,&g,code,next);assert(found);
            if(g.box_w && g.box_h){
                const uint8_t *bitmap=lv_font_get_glyph_bitmap(font,code);
                int left=pen+g.ofs_x,top=row*font->line_height+font->line_height-font->base_line-g.box_h-g.ofs_y;
                if(row==1){
                    if(line_left<0)line_left=left;
                    line_right=left+g.box_w-1;
                }
                for(int y=0;y<g.box_h;y++)for(int x=0;x<g.box_w;x++){
                    int px=left+x,py=top+y;
                    if(px<0 || py<0 || px>=width || py>=height)continue;
                    int bit=(y*g.box_w+x)*g.bpp,maximum=(1<<g.bpp)-1;
                    int value=((bitmap[bit/8]>>(8-g.bpp-bit%8))&maximum)*255/maximum;
                    pixels[py*width+px]=value;
                }
            }
            pen+=g.adv_w+tracking;
        }
        if(line_left>=0){
            int uy=font->line_height+font->line_height-font->base_line-font->underline_position;
            int uh=font->underline_thickness?font->underline_thickness:1;
            for(int y=uy;y<uy+uh && y<height;y++)
                for(int x=line_left;x<=line_right && x<width;x++)
                    if(x>=0 && y>=0)pixels[y*width+x]=255;
        }
    }
    if(!changed && p->fog_ready)return;
    /* Detailed fog: glyph ink + icon + bar. Width follows "0" vs "100". */
    memset(p->fog_weights,0,PARTICLE_WIDTH*PARTICLE_HEIGHT);
    printer_fog_stamp(p,10,12,66,68,230);
    for(int i=0;i<4;i++){
        PrinterTextCache *cache=&p->text_cache[i];
        int left=cache->image.header.w,right=-1,top=cache->image.header.h,bottom=-1;
        const uint8_t *pixels=cache->image.data;
        int width=cache->image.header.w,height=cache->image.header.h;
        for(int y=0;y<height;y++)for(int x=0;x<width;x++){
            int value=pixels[y*width+x];if(value<24)continue;
            if(x<left)left=x;
            if(x>right)right=x;
            if(y<top)top=y;
            if(y>bottom)bottom=y;
            int ox=lv_obj_get_x(cache->label)+x,oy=lv_obj_get_y(cache->label)+y;
            for(int dy=-12;dy<=12;dy++)for(int dx=-12;dx<=12;dx++){
                int px=ox+dx,py=oy+dy;
                if(px<0 || py<0 || px>=PARTICLE_WIDTH || py>=PARTICLE_HEIGHT)continue;
                int dist=abs(dx)+abs(dy);if(dist>18)continue;
                int weight=value*(19-dist)/19;
                if(p->fog_weights[py*PARTICLE_WIDTH+px]<weight)p->fog_weights[py*PARTICLE_WIDTH+px]=weight;
            }
        }
        if(right<0)continue;
        int ox=lv_obj_get_x(cache->label),oy=lv_obj_get_y(cache->label);
        printer_fog_stamp(p,ox+left-20,oy+top-14,ox+right+21,oy+bottom+14,210);
    }
    lv_coord_t bx=lv_obj_get_x(p->bar),by=lv_obj_get_y(p->bar);
    printer_fog_stamp(p,bx-25,by-20,bx+lv_obj_get_width(p->bar)+25,by+lv_obj_get_height(p->bar)+20,230);
    uint8_t *fog=p->fog_weights;
    for(int pass=0;pass<2;pass++)for(int y=1;y<PARTICLE_HEIGHT-1;y++)for(int x=1;x<PARTICLE_WIDTH-1;x++){
        int i=y*PARTICLE_WIDTH+x;
        int v=(fog[i]*2+fog[i-1]+fog[i+1]+fog[i-PARTICLE_WIDTH]+fog[i+PARTICLE_WIDTH])/6;
        if(v>fog[i])fog[i]=v>255?255:v;
    }
    p->fog_ready=true;
}
