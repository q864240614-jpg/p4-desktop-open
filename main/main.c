#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_lvgl_port_touch.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "portable_ui.h"
#include "p4_display.h"
#include "bambu_app.h"
#include "bambu_video.h"
#include "todo_app.h"
#include "codex_app.h"

static void brightness_timer(lv_timer_t *timer)
{
    (void)timer;
    static int applied = 100;
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    if (local.tm_year < 124) return; /* Wait for RTC/SNTP before scheduling. */
    int brightness = local.tm_hour >= 22 || local.tm_hour < 8 ? 15 : 100;
    if (brightness == applied) return;
    ESP_ERROR_CHECK(bsp_display_brightness_set(brightness));
    applied = brightness;
}

static void ui_timer(lv_timer_t *timer)
{
    (void)timer;
    bambu_app_poll();
    todo_app_poll();
    codex_app_poll();
    bambu_video_poll();
    PortableUI_Process();
}

void app_main(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();
    esp_err_t storage=nvs_flash_init();
    if(storage==ESP_ERR_NVS_NO_FREE_PAGES || storage==ESP_ERR_NVS_NEW_VERSION_FOUND){
        ESP_LOGE("storage","NVS requires recovery (%s); retained settings, startup stopped",esp_err_to_name(storage));
        return; // Do not repeatedly reboot or silently erase printer credentials.
    }
    ESP_ERROR_CHECK(storage);
    /* Keep native panel timing; PPA handles the landscape pixel rotation. */
    lvgl_port_cfg_t port = ESP_LVGL_PORT_INIT_CONFIG();
    port.task_affinity=0; // Core 1 is reserved for H2D software decode while viewing.
    ESP_ERROR_CHECK(lvgl_port_init(&port));
    bsp_lcd_handles_t panel;
    ESP_ERROR_CHECK(bsp_display_new_with_handles(NULL, &panel));
    bool locked = lvgl_port_lock(0);
    assert(locked);
    lv_disp_t *display = p4_display_create(panel.panel);
    assert(display);
    esp_lcd_touch_handle_t touch;
    ESP_ERROR_CHECK(bsp_touch_new(NULL, &touch));
    const lvgl_port_touch_cfg_t touch_cfg = {.disp = display, .handle = touch};
    lv_indev_t *input = lvgl_port_add_touch(&touch_cfg);
    assert(input);

    /* The display rotation also supplies LVGL's native-to-logical touch mapping. */
    assert(lv_disp_get_hor_res(display) == 800 && lv_disp_get_ver_res(display) == 480);
    lv_obj_t *screen = lv_disp_get_scr_act(display);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x030303), 0);
    lv_obj_t *ui = PortableUI_Create(screen);
    assert(ui);
    PortableUI_Status status = {.codex.remaining = {NAN, NAN}};
    PortableUI_SetStatus(&status);
    PortableUI_SetStandby(false);
    /* esp_lvgl_port owns tick and lv_timer_handler; UI work shares its task. */
    lv_timer_t *timer = lv_timer_create(ui_timer, 25, NULL);
    assert(timer);
    ESP_ERROR_CHECK(bsp_display_backlight_on());
    lv_timer_t *brightness = lv_timer_create(brightness_timer, 1000, NULL);
    assert(brightness);
    brightness_timer(brightness);
    todo_app_start();
    bambu_video_start();
    lvgl_port_unlock();
    bambu_app_start();
    codex_app_start();
    ESP_LOGI("p4_ui", "800x480 landscape UI ready; waiting for RTC/SNTP and status data");
}
