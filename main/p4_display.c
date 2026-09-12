#include "p4_display.h"
#include <assert.h>
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Three strips per full screen instead of six; smaller digit caches fund the extra buffers.
#define DRAW_PIXELS (800 * 160)
#define DRAW_BYTES (DRAW_PIXELS * sizeof(lv_color_t))
static esp_lcd_panel_handle_t lcd;
static ppa_client_handle_t rotation;
static lv_disp_draw_buf_t draw_buffer;
static lv_disp_drv_t display_driver;
static void *rotated_buffer;
static TaskHandle_t transfer_task;
static lv_area_t transfer_area;
static SemaphoreHandle_t dma_gate;
static bool panel_copy_failed;

void p4_display_dma_take(void)
{ if(dma_gate)xSemaphoreTake(dma_gate,portMAX_DELAY); }
void p4_display_dma_give(void)
{
    if(!dma_gate)return;
    if(xPortInIsrContext()){
        BaseType_t wake=pdFALSE;
        xSemaphoreGiveFromISR(dma_gate,&wake);
        portYIELD_FROM_ISR(wake);
    }else xSemaphoreGive(dma_gate);
}
static bool rotated(ppa_client_handle_t client,ppa_event_data_t *event,void *context)
{
    (void)client;(void)event;(void)context;
    BaseType_t wake=pdFALSE;vTaskNotifyGiveFromISR(transfer_task,&wake);return wake==pdTRUE;
}
static void transfer_worker(void *arg)
{
    (void)arg;
    for(;;){
        ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        esp_err_t err=esp_lcd_panel_draw_bitmap(lcd,transfer_area.y1,799-transfer_area.x2,
            transfer_area.y2+1,800-transfer_area.x1,rotated_buffer);
        if(err!=ESP_OK){
            ESP_LOGE("p4_display","panel copy failed %s",esp_err_to_name(err));
            panel_copy_failed=true;
            p4_display_dma_give();
            lv_disp_flush_ready(&display_driver);
        }
    }
}

static bool copied(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *event, void *context)
{
    (void)panel;(void)event;
    if(panel_copy_failed){panel_copy_failed=false;return false;}
    p4_display_dma_give();
    lv_disp_flush_ready(context);
    return false;
}
static void flush(lv_disp_drv_t *driver,const lv_area_t *area,lv_color_t *pixels)
{
    (void)driver;
    transfer_area=*area;
    const unsigned width=lv_area_get_width(area),height=lv_area_get_height(area);
    /* Logical (x,y) -> native (y,799-x), matching LVGL ROT_90 and GT911 input.
     * PPA synchronizes the caches and rotates; the panel's DMA2D copies the result.
     * The output stays owned by this flush until the panel copy callback fires. */
    const ppa_srm_oper_config_t operation={
        .in={.buffer=pixels,.pic_w=width,.pic_h=height,.block_w=width,.block_h=height,
             .srm_cm=PPA_SRM_COLOR_MODE_RGB565},
        .out={.buffer=rotated_buffer,.buffer_size=DRAW_BYTES,.pic_w=height,.pic_h=width,
              .srm_cm=PPA_SRM_COLOR_MODE_RGB565},
        .rotation_angle=PPA_SRM_ROTATION_ANGLE_90,.scale_x=1,.scale_y=1,
        .mode=PPA_TRANS_MODE_NON_BLOCKING,
    };
    p4_display_dma_take();
    esp_err_t err=ppa_do_scale_rotate_mirror(rotation,&operation);
    if(err!=ESP_OK){
        ESP_LOGE("p4_display","PPA rotate failed %s",esp_err_to_name(err));
        p4_display_dma_give();
        lv_disp_flush_ready(driver);
    }
}
static void monitor(lv_disp_drv_t *driver,uint32_t time_ms,uint32_t pixels)
{
    (void)driver;(void)pixels;
    static unsigned frames,total_ms,max_ms,started;
    const unsigned now=lv_tick_get();
    if(!frames)started=now;
    total_ms+=time_ms;
    if(time_ms>max_ms)max_ms=time_ms;
    if(++frames==120){
        ESP_LOGI("p4_display","120 redraws, render+flush mean/max %u/%u ms, mean interval %u ms",
                 total_ms/frames,max_ms,(now-started)/(frames-1));
        frames=total_ms=max_ms=0;
    }
}
lv_disp_t *p4_display_create(esp_lcd_panel_handle_t panel)
{
    lcd=panel;
    dma_gate=xSemaphoreCreateBinary();
    configASSERT(dma_gate);xSemaphoreGive(dma_gate);
    const ppa_client_config_t cfg={.oper_type=PPA_OPERATION_SRM};
    ESP_ERROR_CHECK(ppa_register_client(&cfg,&rotation));
    configASSERT(xTaskCreate(transfer_worker,"display_dma",4096,NULL,5,&transfer_task)==pdPASS);
    const ppa_event_callbacks_t ppa_callbacks={.on_trans_done=rotated};
    ESP_ERROR_CHECK(ppa_client_register_event_callbacks(rotation,&ppa_callbacks));
    const size_t alignment=CONFIG_CACHE_L2_CACHE_LINE_SIZE;
    const uint32_t caps=MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT|MALLOC_CAP_DMA;
    void *first=heap_caps_aligned_alloc(alignment,DRAW_BYTES,caps);
    void *second=heap_caps_aligned_alloc(alignment,DRAW_BYTES,caps);
    rotated_buffer=heap_caps_aligned_alloc(alignment,DRAW_BYTES,caps);
    assert(first && second && rotated_buffer);
    lv_disp_draw_buf_init(&draw_buffer,first,second,DRAW_PIXELS);
    lv_disp_drv_init(&display_driver);
    display_driver.hor_res=480;display_driver.ver_res=800;
    display_driver.rotated=LV_DISP_ROT_90;
    display_driver.sw_rotate=0;
    display_driver.draw_buf=&draw_buffer;display_driver.flush_cb=flush;display_driver.monitor_cb=monitor;
    const esp_lcd_dpi_panel_event_callbacks_t callbacks={.on_color_trans_done=copied};
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_register_event_callbacks(panel,&callbacks,&display_driver));
    lv_disp_t *display=lv_disp_drv_register(&display_driver);
    ESP_LOGI("p4_display","Async PPA rotation + DMA2D panel copy, 160-line double buffer");
    return display;
}
