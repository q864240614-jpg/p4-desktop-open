#pragma once
#include "lvgl.h"
#include "esp_lcd_panel_ops.h"
/* Caller holds the LVGL port lock. The panel keeps its native 480x800 timing. */
lv_disp_t *p4_display_create(esp_lcd_panel_handle_t panel);
/* JPEG decode and PPA/panel copy share DMA2D; take this around either. */
void p4_display_dma_take(void);
void p4_display_dma_give(void);
