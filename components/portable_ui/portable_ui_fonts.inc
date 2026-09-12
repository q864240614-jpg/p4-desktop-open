/* Reuse the installed glyph coverage at smaller sizes; no duplicate CJK font blob. */
typedef struct { const lv_font_t *source; int numerator,denominator; } PrinterFontScale;
static const PrinterFontScale printer_scale18={&lv_font_cjk_24,3,4};
static const PrinterFontScale printer_scale20={&lv_font_cjk_24,5,6};
static const PrinterFontScale printer_scale88={&lv_font_inter_108,22,27};
static uint8_t printer_scaled_glyph[128*128];
static bool printer_font_descriptor(const lv_font_t *font,lv_font_glyph_dsc_t *g,uint32_t code,uint32_t next)
{
    const PrinterFontScale *scale=font->dsc;
    bool found=lv_font_get_glyph_dsc(scale->source,g,code,next);
    if(!found)return false; /* Font API's explicit missing-glyph result. */
    g->adv_w=(g->adv_w*scale->numerator+scale->denominator/2)/scale->denominator;
    g->box_w=(g->box_w*scale->numerator+scale->denominator-1)/scale->denominator;
    g->box_h=(g->box_h*scale->numerator+scale->denominator-1)/scale->denominator;
    g->ofs_x=g->ofs_x*scale->numerator/scale->denominator;
    g->ofs_y=g->ofs_y*scale->numerator/scale->denominator;g->bpp=8;
    return true;
}
static const uint8_t *printer_font_bitmap(const lv_font_t *font,uint32_t code)
{
    const PrinterFontScale *scale=font->dsc;lv_font_glyph_dsc_t src,dst;
    lv_font_get_glyph_dsc(scale->source,&src,code,0);
    printer_font_descriptor(font,&dst,code,0);
    const uint8_t *bitmap=lv_font_get_glyph_bitmap(scale->source,code);
    assert(src.bpp==4 && dst.box_w*dst.box_h<=sizeof(printer_scaled_glyph));
    for(int y=0;y<dst.box_h;y++)for(int x=0;x<dst.box_w;x++){
        int sx=x*scale->denominator*256/scale->numerator,sy=y*scale->denominator*256/scale->numerator;
        int ix=sx/256,iy=sy/256,fx=sx%256,fy=sy%256,value=0;
        for(int yy=0;yy<2;yy++)for(int xx=0;xx<2;xx++){
            int px=LV_MIN(src.box_w-1,ix+xx),py=LV_MIN(src.box_h-1,iy+yy),bit=(py*src.box_w+px)*4;
            int sample=((bitmap[bit/8]>>(4-bit%8))&15)*17;
            value+=sample*(xx?fx:256-fx)*(yy?fy:256-fy);
        }
        printer_scaled_glyph[y*dst.box_w+x]=value/65536;
    }
    return printer_scaled_glyph;
}
static const lv_font_t printer_font18={.get_glyph_dsc=printer_font_descriptor,.get_glyph_bitmap=printer_font_bitmap,.line_height=24,.base_line=4,.dsc=&printer_scale18};
static const lv_font_t printer_font20={.get_glyph_dsc=printer_font_descriptor,.get_glyph_bitmap=printer_font_bitmap,.line_height=27,.base_line=5,.dsc=&printer_scale20};
static const lv_font_t printer_font88={.get_glyph_dsc=printer_font_descriptor,.get_glyph_bitmap=printer_font_bitmap,.line_height=106,.base_line=22,.dsc=&printer_scale88};
