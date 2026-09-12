#include "portable_ui.h"
#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "esp_random.h"
#if LVGL_VERSION_MAJOR != 8 || LVGL_VERSION_MINOR != 3
#error "This renderer is exported for the bundled LVGL 8.3.11"
#endif
#if LV_COLOR_DEPTH != 16 || !LV_DRAW_COMPLEX || !LV_USE_CANVAS || !LV_USE_IMG || !LV_USE_FLEX
#error "Enable RGB565, complex drawing, canvas, image and flex in LVGL configuration"
#endif
LV_FONT_DECLARE(lv_font_inter_108);
LV_FONT_DECLARE(lv_font_inter_120);
LV_FONT_DECLARE(lv_font_inter_164);
LV_FONT_DECLARE(lv_font_inter_24);
LV_FONT_DECLARE(lv_font_cjk_24);
LV_FONT_DECLARE(lv_font_inter_28);
LV_FONT_DECLARE(lv_font_inter_32);
LV_FONT_DECLARE(lv_font_inter_40);
LV_FONT_DECLARE(lv_font_inter_48);

#include "portable_ui_fonts.inc"

#define COLOR_BACKGROUND 0x030303
#define COLOR_SURFACE 0x121315
#define COLOR_SURFACE_ACTIVE 0x1B1C1F
#define COLOR_MUTED 0x949493
#define COLOR_ACTIVE 0xE64A43
#define COLOR_WHITE 0xF5F5F2
#define STANDBY_TIMEOUT_MS 180000
#define CLOCK_ROTATED_SIDE 256
#define DOT_TIMER_MS 16
#define DOT_FIELD_Y 64
#define DOT_FIELD_HEIGHT 392
#define DOT_FRAME_MS 40
#define DOT_CLOUD_COUNT 6
#define DOT_MAP_WIDTH 69
#define DOT_MAP_HEIGHT 35

static lv_obj_t *codex_tile, *codex_plan_label, *codex_state_label;
static lv_obj_t *codex_values[2], *codex_resets[2], *codex_bars[2], *codex_short_label;
static float codex_remaining[2] = {NAN, NAN};
static bool codex_connected;
static bool codex_seen, codex_was_limited;
static uint32_t codex_previous_reset[2];
static void notify_codex_restored(void);
static uint8_t printer_led_kernel[26][26];
static BambuState printer_slots[2];
static lv_obj_t *standby_printer_labels[2];
static lv_obj_t *control_layer;
static lv_obj_t *ui_root;
static void stop_video(void);
static lv_obj_t *header_time_label;
static lv_obj_t *header_date_label;
static lv_obj_t *standby_layer;
static lv_obj_t *dot_field;
static lv_img_dsc_t dot_scene_image;
static uint8_t dot_kernel[24][24];
static lv_color_t dot_colors[256];
static uint8_t *dot_readability;
static struct { uint8_t light, fog; } dot_map[DOT_MAP_HEIGHT][DOT_MAP_WIDTH];
static struct {
    int16_t x, y;
    uint8_t radius, power;
    uint32_t start, duration;
} dot_clouds[DOT_CLOUD_COUNT];
static uint32_t dot_scene_ms, dot_frame_tick;
static lv_obj_t *hour_dial;
static lv_obj_t *minute_dial;
static lv_img_dsc_t clock_images[24 + 60];
static struct {
    lv_img_dsc_t image;
    int16_t angle;
    int16_t value;
    int16_t fraction_x, fraction_y;
    uint8_t *unshifted;
} rotated_digits[2][5];
static lv_color_t *rotation_colors;
static uint8_t *clock_source_rgba;
static uint64_t clock_draw_keys[2][5];
static lv_area_t clock_dirty[2][5];
static uint32_t clock_frame_progress[2];
static lv_img_dsc_t clock_tracks[2];
static const struct {
    int16_t center_x, center_y, radius, pitch, track_radius, marker_x;
} clock_geometry[2] = {
    {.center_x = -190, .center_y = 236, .radius = 360,
     .pitch = 210, .track_radius = 480, .marker_x = 312},
    {.center_x = 990, .center_y = 236, .radius = 360,
     .pitch = 210, .track_radius = 480, .marker_x = 488},
};
static lv_obj_t *date_label;
static lv_obj_t *time_pending_label;
static lv_obj_t *date_weekday_label;
static lv_timer_t *motion_timer;
static bool standby_active;
static bool clock_valid;
static uint8_t clock_hour;
static uint8_t clock_minute;
static uint8_t clock_second;
static uint32_t clock_second_tick;

static lv_obj_t *create_label(lv_obj_t *parent, const char *text,
                              const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static void set_plain_container(lv_obj_t *object, uint32_t color)
{
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(object, 0, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
}

static void set_draw_object(lv_obj_t *object)
{
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
}

/* Camera coordinates use quarter pixels; a shared periodic kernel keeps every
 * LED on the same moving plane. The RGB565 frame needs only a native image blit. */
static int32_t dot_sine(uint32_t now, uint32_t period)
{
    const uint32_t phase = (uint64_t)(now % period) * (360 * 256) / period;
    const int angle = phase >> 8;
    const int fraction = phase & 255;
    return ((int32_t)lv_trigo_sin(angle) * (256 - fraction) +
            (int32_t)lv_trigo_sin(angle + 1) * fraction) / 256;
}

static int dot_cell(int quarter_pixel)
{
    return quarter_pixel >= 0 ? quarter_pixel / 24 : -((-quarter_pixel + 23) / 24);
}

static void restart_dot_cloud(int index, int camera_x, int camera_y)
{
    const uint32_t random = esp_random();
    dot_clouds[index].x = camera_x + 4 + random % 60;
    dot_clouds[index].y = camera_y + 3 + (random >> 8) % 30;
    dot_clouds[index].radius = 8 + (random >> 16) % 6;
    dot_clouds[index].power = 130 + (random >> 20) % 61;
    dot_clouds[index].duration = 10000 + (random >> 4) % 6000;
    dot_clouds[index].start = dot_scene_ms;
}

/* The bars share the background image and its LED halo, without panels. */
static bool standby_print_visible(const BambuState *state)
{
    return state->has_data && (!strcmp(state->state, "RUNNING") ||
        !strcmp(state->state, "PREPARE") || !strcmp(state->state, "PAUSE"));
}

static void render_printer_leds(lv_color_t *pixels)
{
    for (int slot = 0; slot < 2; ++slot) {
        const BambuState *state = &printer_slots[slot];
        if (!standby_print_visible(state)) continue;
        const bool paused = !strcmp(state->state, "PAUSE");
        const lv_color_t color = lv_color_hex(!state->online ? 0x647777 :
                                              paused ? 0xE9AA42 : 0x00AE42);
        const int progress = LV_MAX(0, state->progress);
        const int pulse = 190 + (dot_sine(dot_scene_ms + slot * 1200, 3200) + 32767) * 65 / 65534;
        for (int col = 0; col < 36; ++col) {
            const int fill = LV_CLAMP(0, progress * 36 - col * 100, 100);
            const int brightness = 28 + fill * (state->online ? 227 : 80) / 100;
            const int tip = fill > 0 && fill < 100 ? pulse : 255;
            for (int row = 0; row < 2; ++row) {
                for (int y = 0; y < 26; ++y) {
                    for (int x = 0; x < 26; ++x) {
                        const int pixel = (340 + row * 8 + y) * 800 + 76 + slot * 352 + col * 8 + x;
                        const int opacity = printer_led_kernel[y][x] * brightness / 255 * tip / 255;
                        if(opacity) pixels[pixel] = lv_color_mix(color, pixels[pixel], opacity);
                    }
                }
            }
        }
    }
}

static void render_dot_scene(void)
{
    const int camera_x = (4 * (40 * dot_sine(dot_scene_ms, 60000) +
                               14 * dot_sine(dot_scene_ms, 23000))) >> LV_TRIGO_SHIFT;
    const int camera_y = (4 * (32 * dot_sine(dot_scene_ms, 71000) +
                               10 * dot_sine(dot_scene_ms + 9250, 37000))) >> LV_TRIGO_SHIFT;
    const int first_x = dot_cell(camera_x), first_y = dot_cell(camera_y);
    int cloud_power[DOT_CLOUD_COUNT];
    for (int i = 0; i < DOT_CLOUD_COUNT; ++i) {
        uint32_t age = dot_scene_ms - dot_clouds[i].start;
        if (age >= dot_clouds[i].duration) {
            restart_dot_cloud(i, first_x, first_y);
            age = 0;
        }
        /* Raised cosine: both the value and its slope are zero at birth/death. */
        const int envelope = 32767 - dot_sine(age + dot_clouds[i].duration / 4,
                                             dot_clouds[i].duration);
        cloud_power[i] = dot_clouds[i].power * envelope / 65534;
    }
    for (int row = 0; row < DOT_MAP_HEIGHT; ++row) {
        for (int col = 0; col < DOT_MAP_WIDTH; ++col) {
            const int wx = first_x + col, wy = first_y + row;
            int glow = 0;
            for (int i = 0; i < DOT_CLOUD_COUNT; ++i) {
                const int dx = wx - dot_clouds[i].x, dy = wy - dot_clouds[i].y;
                const int radius2 = dot_clouds[i].radius * dot_clouds[i].radius;
                /* Offset elliptical lobes avoid perfectly round spots. */
                const int distance = LV_MIN(dx * dx + 2 * dy * dy,
                                             2 * (dx - 3) * (dx - 3) + (dy + 2) * (dy + 2));
                const int falloff = LV_MAX(0, 256 - distance * 256 / radius2);
                glow += cloud_power[i] * falloff * falloff / 65536;
            }
            glow = LV_MIN(glow, 210);
            /* Stable world-space variation: adjacent LEDs differ without flicker. */
            uint32_t noise = (uint32_t)wx * 0x45d9f3bU ^ (uint32_t)wy * 0x119de1f3U;
            noise ^= noise >> 16;
            dot_map[row][col].light = 38 + glow * (160 + (noise & 95)) / 255;
            dot_map[row][col].fog = glow / 20;
        }
    }
    uint8_t columns[800], phases[800];
    for (int x = 0; x < 800; ++x) {
        const int world = x * 2 + camera_x;
        const int cell = dot_cell(world);
        columns[x] = cell - first_x;
        phases[x] = world - cell * 24;
    }
    lv_color_t *pixels = (lv_color_t *)dot_scene_image.data;
    for (int y = 0; y < DOT_FIELD_HEIGHT; ++y) {
        const int world = y * 2 + camera_y;
        const int cell = dot_cell(world);
        const uint8_t *kernel = dot_kernel[world - cell * 24];
        const int row = cell - first_y;
        for (int x = 0; x < 800; ++x) {
            const int col = columns[x];
            const int level = dot_map[row][col].fog +
                dot_map[row][col].light * kernel[phases[x]] / 255;
            const int pixel = y * 800 + x;
            pixels[pixel] = dot_colors[(level * dot_readability[pixel]) >> 8];
        }
    }
    render_printer_leds(pixels);
    lv_obj_invalidate(dot_field);
}

static void create_dot_field(void)
{
    for (int y = 0; y < 26; ++y) {
        for (int x = 0; x < 26; ++x) {
            const float dx = (x - 12.5f) / 2, dy = (y - 12.5f) / 2, r2 = dx * dx + dy * dy;
            printer_led_kernel[y][x] = fabsf(dx) <= 1 && fabsf(dy) <= 1
                ? 255 : (uint8_t)(52 * expf(-r2 / 10.0f));
        }
    }
    for (int y = 0; y < 24; ++y) {
        for (int x = 0; x < 24; ++x) {
            float dx = fabsf(x / 4.0f - 2.5f), dy = fabsf(y / 4.0f - 2.5f);
            dx = fmaxf(0, fminf(dx, 6.0f - dx) - 0.5f);
            dy = fmaxf(0, fminf(dy, 6.0f - dy) - 0.5f);
            const float radius2 = dx * dx + dy * dy;
            dot_kernel[y][x] = (uint8_t)(205 * expf(-radius2 * 4) + 40 * expf(-radius2 / 3));
        }
    }
    for (int i = 0; i < 256; ++i) {
        dot_colors[i] = lv_color_make(3 + i * 182 / 255, 3 + i * 229 / 255,
                                     3 + i * 252 / 255);
    }
    /* Fixed soft dimming behind both digit paths, plus a lower global ceiling.
     * Bake it once so stronger haze adds no per-frame blur or mask calculation. */
    dot_readability = heap_caps_malloc(800 * DOT_FIELD_HEIGHT, MALLOC_CAP_SPIRAM);
    assert(dot_readability != NULL);
    for (int y = 0; y < DOT_FIELD_HEIGHT; ++y) {
        for (int x = 0; x < 800; ++x) {
            const int screen_y = y + DOT_FIELD_Y;
            const int hx = x - 170, hy = screen_y - 236;
            const int mx = x - 630, my = screen_y - 236;
            const int hour = LV_MAX(0, 256 - hx * hx * 256 / (152 * 152) - hy * hy * 256 / (176 * 176));
            const int minute = LV_MAX(0, 256 - mx * mx * 256 / (152 * 152) - my * my * 256 / (176 * 176));
            const int shade = LV_MAX(hour, minute);
            const int gain = 224 - shade * shade * 144 / 65536;
            const int edge = LV_MIN(16, LV_MIN(LV_MIN(x, 799 - x), LV_MIN(y, DOT_FIELD_HEIGHT - 1 - y)));
            dot_readability[y * 800 + x] = gain * edge / 16;
        }
    }
    dot_scene_image.header.cf = LV_IMG_CF_TRUE_COLOR;
    dot_scene_image.header.w = 800;
    dot_scene_image.header.h = DOT_FIELD_HEIGHT;
    dot_scene_image.data_size = 800 * DOT_FIELD_HEIGHT * sizeof(lv_color_t);
    dot_scene_image.data = heap_caps_calloc(800 * DOT_FIELD_HEIGHT, sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    assert(dot_scene_image.data != NULL);
    dot_field = lv_img_create(standby_layer);
    lv_img_set_src(dot_field, &dot_scene_image);
    lv_obj_set_pos(dot_field, 0, DOT_FIELD_Y);
    lv_obj_clear_flag(dot_field, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < DOT_CLOUD_COUNT; ++i) {
        restart_dot_cloud(i, 0, 0);
        dot_clouds[i].start -= dot_clouds[i].duration * (i + 1) / (DOT_CLOUD_COUNT + 1);
    }
    render_dot_scene();
}

/* Render once into PSRAM so rotating digits do not need per-frame text layers. */
static void create_clock_images(lv_obj_t *parent)
{
    const size_t cache_pixels = CLOCK_ROTATED_SIDE * CLOCK_ROTATED_SIDE;
    rotation_colors = heap_caps_malloc(cache_pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    assert(rotation_colors != NULL);
    for (int dial = 0; dial < 2; ++dial) {
        for (int slot = 0; slot < 5; ++slot) {
            rotated_digits[dial][slot].image.header.cf = LV_IMG_CF_ALPHA_8BIT;
            rotated_digits[dial][slot].image.data = heap_caps_malloc(cache_pixels, MALLOC_CAP_SPIRAM);
            rotated_digits[dial][slot].unshifted = heap_caps_malloc(cache_pixels, MALLOC_CAP_SPIRAM);
            assert(rotated_digits[dial][slot].image.data != NULL &&
                   rotated_digits[dial][slot].unshifted != NULL);
            rotated_digits[dial][slot].value = -1;
        }
    }
    lv_obj_t *canvas = lv_canvas_create(parent);
    const int16_t width = 166;
    const int16_t height = lv_font_inter_120.line_height + 8;
    const uint32_t pixels_count = width * height;
    const uint32_t size = lv_img_buf_get_img_size(width, height, LV_IMG_CF_TRUE_COLOR_ALPHA);
    clock_source_rgba = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    assert(clock_source_rgba != NULL);
    lv_canvas_set_buffer(canvas, clock_source_rgba, width, height, LV_IMG_CF_TRUE_COLOR_ALPHA);
    for (int value = 0; value < 24 + 60; ++value) {
        const bool is_hour = value < 24;
        const lv_font_t *font = &lv_font_inter_120;
        lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_TRANSP);
        lv_draw_label_dsc_t label;
        lv_draw_label_dsc_init(&label);
        label.font = font;
        label.color = lv_color_hex(COLOR_WHITE);
        label.align = LV_TEXT_ALIGN_CENTER;
        char text[4];
        snprintf(text, sizeof(text), "%02d", is_hour ? value : value - 24);
        lv_canvas_draw_text(canvas, 0, 4, width, &label, text);
        // Digits are white: cache their exact 8-bit alpha, not redundant RGB.
        uint8_t *alpha = heap_caps_malloc(pixels_count, MALLOC_CAP_SPIRAM);
        assert(alpha != NULL);
        for (uint32_t i = 0; i < pixels_count; ++i)
            alpha[i] = clock_source_rgba[i * LV_IMG_PX_SIZE_ALPHA_BYTE + LV_IMG_PX_SIZE_ALPHA_BYTE - 1];
        clock_images[value] = (lv_img_dsc_t){
            .header = {.cf = LV_IMG_CF_ALPHA_8BIT, .w = width, .h = height},
            .data_size = pixels_count, .data = alpha};
    }
    lv_obj_del(canvas);
    memset(clock_source_rgba, 255, size);
}

/* The stationary arc, ticks and marker are rasterized once, too. */
static void create_clock_tracks(void)
{
    lv_obj_update_layout(standby_layer);
    for (int dial = 0; dial < 2; ++dial) {
        lv_obj_t *object = dial == 0 ? hour_dial : minute_dial;
        const int16_t width = lv_obj_get_width(object);
        const int16_t height = lv_obj_get_height(object);
        const int16_t cx = clock_geometry[dial].center_x - lv_obj_get_x(object);
        const int16_t cy = clock_geometry[dial].center_y - lv_obj_get_y(object);
        const int16_t radius = clock_geometry[dial].track_radius;
        const int direction = dial == 0 ? 1 : -1;
        const uint32_t pixels = width * height;
        uint8_t *rgba = heap_caps_calloc(pixels, LV_IMG_PX_SIZE_ALPHA_BYTE, MALLOC_CAP_SPIRAM);
        uint8_t *packed = heap_caps_malloc(pixels * 3, MALLOC_CAP_SPIRAM);
        assert(rgba != NULL && packed != NULL);
        lv_obj_t *canvas = lv_canvas_create(standby_layer);
        lv_canvas_set_buffer(canvas, rgba, width, height, LV_IMG_CF_TRUE_COLOR_ALPHA);
        lv_draw_arc_dsc_t arc;
        lv_draw_arc_dsc_init(&arc);
        arc.color = lv_color_hex(0x454545);
        arc.opa = LV_OPA_30;
        arc.width = 2;
        lv_canvas_draw_arc(canvas, cx, cy, radius, dial == 0 ? 335 : 155, dial == 0 ? 25 : 205, &arc);
        lv_draw_rect_dsc_t dot;
        lv_draw_rect_dsc_init(&dot);
        dot.bg_color = lv_color_hex(0x777777);
        dot.bg_opa = LV_OPA_40;
        dot.radius = LV_RADIUS_CIRCLE;
        for (int16_t degree = -24; degree <= 24; degree += 3) {
            if (degree == 0) continue;
            const int16_t x = cx + direction * ((radius + 8) * (int32_t)lv_trigo_cos(degree + 360) >> LV_TRIGO_SHIFT);
            const int16_t y = cy + ((radius + 8) * (int32_t)lv_trigo_sin(degree + 360) >> LV_TRIGO_SHIFT);
            lv_canvas_draw_rect(canvas, x - 2, y - 2, 5, 5, &dot);
        }
        const int16_t marker_x = clock_geometry[dial].marker_x - lv_obj_get_x(object);
        const lv_point_t points[] = {{marker_x, cy}, {marker_x + direction * 20, cy}};
        lv_draw_line_dsc_t line;
        lv_draw_line_dsc_init(&line);
        line.color = lv_color_hex(COLOR_ACTIVE);
        line.width = 5;
        line.round_start = line.round_end = true;
        lv_canvas_draw_line(canvas, points, 2, &line);
        /* Planar RGB565 + alpha uses LVGL's direct blend path on this 16-bit panel. */
        for (uint32_t i = 0; i < pixels; ++i) {
            packed[2 * i] = rgba[3 * i];
            packed[2 * i + 1] = rgba[3 * i + 1];
            packed[2 * pixels + i] = rgba[3 * i + 2];
        }
        clock_tracks[dial] = *lv_canvas_get_img(canvas);
        clock_tracks[dial].header.cf = LV_IMG_CF_RGB565A8;
        clock_tracks[dial].data = packed;
        lv_obj_del(canvas);
        free(rgba);
    }
}

static uint32_t clock_progress(bool is_hour)
{
    const uint32_t minute_ms = (uint32_t)clock_second * 1000 +
                               LV_MIN(lv_tick_elaps(clock_second_tick), 999);
    return is_hour ? (uint32_t)clock_minute * 60000 + minute_ms : minute_ms;
}

/* Keep position fractional even though LVGL's rotation angle uses 0.1 degrees. */
static int16_t clock_pose(bool is_hour, int32_t phase, int8_t offset,
                          int32_t *x_q8, int32_t *y_q8)
{
    const int dial = is_hour ? 0 : 1;
    const int direction = is_hour ? 1 : -1;
    const int32_t pitch = clock_geometry[dial].pitch;
    const int32_t angle_q8 = (phase - 524288) * pitch / 4096 - offset * pitch * 256;
    const int32_t rotation = (angle_q8 + 3600 * 256) % (3600 * 256);
    const int16_t degree = rotation / 2560;
    const int16_t fraction = rotation % 2560;
    const int32_t cosine = (lv_trigo_cos(degree) * (2560 - fraction) +
                            lv_trigo_cos(degree + 1) * fraction) / 2560;
    const int32_t sine = (lv_trigo_sin(degree) * (2560 - fraction) +
                          lv_trigo_sin(degree + 1) * fraction) / 2560;
    *x_q8 = clock_geometry[dial].center_x * 256 +
        direction * (clock_geometry[dial].radius * cosine >> (LV_TRIGO_SHIFT - 8));
    *y_q8 = clock_geometry[dial].center_y * 256 +
        (clock_geometry[dial].radius * sine >> (LV_TRIGO_SHIFT - 8));
    /* Reflect the orbit and tilt; glyph pixels themselves stay readable. */
    return direction * (angle_q8 / 256);
}

/* Allow clipped neighbors. Enter/exit near the top/bottom of the dial,
 * independent of glyph width, with the same 800 ms envelope on both wheels. */
static int32_t clock_visible_phase[2][2];
static int32_t clock_safe_margin(int dial,int32_t phase)
{
    int32_t x,y;clock_pose(dial==0,phase,0,&x,&y);
    return LV_MIN(y-108*256,420*256-y);
}
static void init_clock_visibility(void)
{
    for(int dial=0;dial<2;dial++)for(int end=0;end<2;end++){
        int32_t low=end?524288:-1572864,high=end?2621440:524288;
        while(high-low>1){
            int32_t mid=low+(high-low)/2;
            if((clock_safe_margin(dial,mid)>=0)==(end==0))high=mid;
            else low=mid;
        }
        clock_visible_phase[dial][end]=end?low:high;
    }
}
static int32_t clock_ease(int64_t elapsed)
{
    const int64_t t=LV_CLAMP(0,elapsed,800)*65536/800;
    const int64_t t2=t*t/65536;
    return t2*(3*65536-2*t)/65536;
}
static uint16_t clock_opacity(int dial, int offset, uint32_t progress,
                              int32_t x_q8, int32_t y_q8, int16_t angle)
{
    (void)x_q8;(void)y_q8;(void)angle;
    const int64_t period=dial==0?3600000:60000;
    const int64_t age=(int64_t)progress-offset*period;
    // Old reaches dim at rollover. Only then does the new value start brightening.
    const int32_t highlight=clock_ease(LV_MIN(age,period-age));
    const int64_t enter=(int64_t)clock_visible_phase[dial][0]*period/1048576;
    const int64_t leave=(int64_t)clock_visible_phase[dial][1]*period/1048576;
    const int32_t visibility=clock_ease(LV_MIN(age-enter,leave-age));
    const int32_t brightness=40*256+(int64_t)215*256*highlight/65536;
    return (int64_t)brightness*visibility/65536;
}

/* Invalidate only when a pixel fraction, angle, value or brightness changes. */
static void refresh_clock_dials(void)
{
    for (int dial = 0; dial < 2; ++dial) {
        const uint32_t period = dial == 0 ? 3600000 : 60000;
        const uint32_t progress = clock_frame_progress[dial] = clock_progress(dial == 0);
        const int32_t phase = (int64_t)progress * 1048576 / period;
        const uint8_t value = dial == 0 ? clock_hour : clock_minute;
        lv_obj_t *object=dial==0?hour_dial:minute_dial;
        lv_area_t bounds;lv_obj_get_coords(object,&bounds);
        for (int8_t offset = -2; offset <= 2; ++offset) {
            int32_t x_q8, y_q8;
            const int16_t angle = clock_pose(dial == 0, phase, offset, &x_q8, &y_q8);
            const uint16_t opacity = clock_opacity(dial, offset, progress, x_q8, y_q8, angle);
            uint64_t key = 0;
            if (clock_valid && opacity && abs(angle) < 420) {
                key = (uint16_t)(x_q8 >> 5) | ((uint64_t)(uint16_t)(y_q8 >> 5) << 16) |
                      ((uint64_t)(angle + 420) << 32) | ((uint64_t)value << 42) |
                      ((uint64_t)opacity << 48);
            }
            if (key != clock_draw_keys[dial][offset + 2]) {
                lv_area_t *dirty=&clock_dirty[dial][offset+2];
                if(clock_draw_keys[dial][offset+2])lv_obj_invalidate_area(object,dirty);
                if(key){
                    const lv_img_dsc_t *img=&clock_images[dial?24:0];
                    lv_point_t pivot={img->header.w/2,img->header.h/2};
                    _lv_img_buf_get_transformed_area(dirty,img->header.w,img->header.h,
                        (angle+3600)%3600,LV_IMG_ZOOM_NONE,&pivot);
                    lv_area_move(dirty,bounds.x1-dial*400+(x_q8>>8)-pivot.x,
                        bounds.y1-40+(y_q8>>8)-pivot.y);
                    dirty->x2++;dirty->y2++; // Fractional mask adds one AA pixel.
                    lv_obj_invalidate_area(object,dirty);
                }
                clock_draw_keys[dial][offset + 2] = key;
            }
        }

    }
}

/* Stable per-pixel threshold. 8x8 Bayer filled in as a marching grid on large
 * glyphs; a hash keeps the mix looking like grain instead of a staircase. */
static uint8_t clock_dither(int x, int y)
{
    unsigned n = (unsigned)x * 374761393u + (unsigned)y * 668265263u;
    n = (n ^ (n >> 13)) * 1274126177u;
    return (uint8_t)(n >> 24);
}
static lv_color_t clock_blend_pixel(lv_color_t background, uint8_t coverage, uint16_t opacity, int x, int y)
{
    const int alpha = coverage * (int)opacity / 255; /* Q8, 0..65280 */
    const int inv = 65280 - alpha;
    const int r8 = (((LV_COLOR_GET_R(background) * 255 + 15) / 31) * inv + 255 * alpha) / 65280;
    const int g8 = (((LV_COLOR_GET_G(background) * 255 + 31) / 63) * inv + 255 * alpha) / 65280;
    const int b8 = (((LV_COLOR_GET_B(background) * 255 + 15) / 31) * inv + 255 * alpha) / 65280;
    const int d = clock_dither(x, y);
    lv_color_t result = {0};
    LV_COLOR_SET_R(result, LV_MIN(31, (r8 * 32 + d) / 256));
    LV_COLOR_SET_G(result, LV_MIN(63, (g8 * 64 + d) / 256));
    LV_COLOR_SET_B(result, LV_MIN(31, (b8 * 32 + d) / 256));
    return result;
}
static void clock_draw_mask(lv_draw_ctx_t *ctx, const lv_area_t *area, const lv_img_dsc_t *mask, uint16_t opacity)
{
    lv_area_t clip;
    if (!_lv_area_intersect(&clip, area, ctx->clip_area)) return;
    lv_color_t *pixels = ctx->buf;
    const int stride = lv_area_get_width(ctx->buf_area);
    for (int y = clip.y1; y <= clip.y2; y++) for (int x = clip.x1; x <= clip.x2; x++) {
        uint8_t coverage = mask->data[(y - area->y1) * mask->header.w + x - area->x1];
        if (!coverage) continue;
        lv_color_t *pixel = &pixels[(y - ctx->buf_area->y1) * stride + x - ctx->buf_area->x1];
        *pixel = clock_blend_pixel(*pixel, coverage, opacity, x, y);
    }
}
/* Bilinear alpha translation makes the slow disc motion advance in 1/8 pixels. */
static void shift_clock_mask(uint8_t *out, const uint8_t *in, int width, int height,
                              int fx, int fy)
{
    for (int y = 0; y <= height; ++y) {
        for (int x = 0; x <= width; ++x) {
            const int a = x < width && y < height ? in[y * width + x] : 0;
            const int b = x > 0 && y < height ? in[y * width + x - 1] : 0;
            const int c = x < width && y > 0 ? in[(y - 1) * width + x] : 0;
            const int d = x > 0 && y > 0 ? in[(y - 1) * width + x - 1] : 0;
            const int coverage =
                (a * (8 - fx) * (8 - fy) + b * fx * (8 - fy) +
                 c * (8 - fx) * fy + d * fx * fy + 32) / 64;
            out[y * (width + 1) + x] = coverage;
        }
    }
}

static void clock_dial_draw(lv_event_t *event)
{
    if (!clock_valid) return;
    lv_obj_t *object = lv_event_get_target(event);
    const bool is_hour = object == hour_dial;
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(event);
    lv_area_t bounds;
    lv_obj_get_coords(object, &bounds);
    const int16_t screen_x = bounds.x1 - (is_hour ? 0 : 400);
    const int16_t screen_y = bounds.y1 - 40;
    const uint32_t period = is_hour ? 3600000 : 60000;
    /* Freeze the pose for every DMA strip in this frame so mask caches remain valid. */
    const uint32_t progress = clock_frame_progress[is_hour ? 0 : 1];
    const int32_t phase = (int64_t)progress * 1048576 / period;
    const int16_t value = is_hour ? clock_hour : clock_minute;
    const int16_t modulus = is_hour ? 24 : 60;

    lv_draw_img_dsc_t track;
    lv_draw_img_dsc_init(&track);
    lv_draw_img(draw_ctx, &track, &bounds, &clock_tracks[is_hour ? 0 : 1]);
    lv_area_t digit_clip=*draw_ctx->clip_area;
    digit_clip.y1=LV_MAX(digit_clip.y1,screen_y+64); // Keep the date row clear.
    if(digit_clip.y1>digit_clip.y2)return;
    for (int8_t offset = -2; offset <= 2; ++offset) {
        int32_t x_q8, y_q8;
        const int16_t angle = clock_pose(is_hour, phase, offset, &x_q8, &y_q8);
        const int32_t edge = LV_CLAMP(0, 420 - abs(angle), 120);
        if (edge == 0) continue;
        const int16_t rotation = (angle + 3600) % 3600;
        const int16_t x = screen_x + (x_q8 >> 8);
        const int16_t y = screen_y + (y_q8 >> 8);
        const int16_t fx = (x_q8 & 255) >> 5;
        const int16_t fy = (y_q8 & 255) >> 5;
        const int16_t index = (value + offset + modulus) % modulus + (is_hour ? 0 : 24);
        const lv_img_dsc_t *image = &clock_images[index];
        lv_draw_img_dsc_t descriptor;
        lv_draw_img_dsc_init(&descriptor);
        descriptor.angle = rotation;
        descriptor.pivot = (lv_point_t){image->header.w / 2, image->header.h / 2};
        descriptor.antialias = true;
        const lv_area_t area = {
            .x1 = x - descriptor.pivot.x, .y1 = y - descriptor.pivot.y,
            .x2 = x - descriptor.pivot.x + image->header.w - 1,
            .y2 = y - descriptor.pivot.y + image->header.h - 1,
        };
        lv_area_t rotated, visible;
        _lv_img_buf_get_transformed_area(&rotated, image->header.w, image->header.h,
                                         rotation, LV_IMG_ZOOM_NONE, &descriptor.pivot);
        lv_area_move(&rotated, area.x1, area.y1);
        if (!_lv_area_intersect(&visible, &rotated, &bounds) ||
            !_lv_area_intersect(&visible, &rotated, draw_ctx->clip_area)) continue;
        /* Neighbors stay dim; current brightness eases through the rollover.
         * The same printed value keeps its angle and position across 23/59 -> 00. */
        const uint16_t opacity = clock_opacity(is_hour ? 0 : 1, offset, progress, x_q8, y_q8, angle);
        if (opacity == 0) continue;
        /* Background motion must not repeat the same rotation on every frame
         * (or every DMA strip). Cache a white alpha mask until value/angle changes. */
        const int dial = is_hour ? 0 : 1;
        // One slot per visible position; hours wrap at 24, not a multiple of five.
        const int slot = offset + 2;
        lv_img_dsc_t *cached = &rotated_digits[dial][slot].image;
        const int16_t width = lv_area_get_width(&rotated);
        const int16_t height = lv_area_get_height(&rotated);
        const bool rotate = rotated_digits[dial][slot].value != index ||
                            rotated_digits[dial][slot].angle != rotation;
        if (rotate) {
            lv_area_t local = rotated;
            lv_area_move(&local, -area.x1, -area.y1);
            cached->header.w = width + 1;
            cached->header.h = height + 1;
            cached->data_size = cached->header.w * cached->header.h;
            assert(cached->data_size <= CLOCK_ROTATED_SIDE * CLOCK_ROTATED_SIDE);
            for (uint32_t i = 0; i < image->data_size; ++i)
                clock_source_rgba[i * LV_IMG_PX_SIZE_ALPHA_BYTE + LV_IMG_PX_SIZE_ALPHA_BYTE - 1] = image->data[i];
            lv_draw_transform(draw_ctx, &local, clock_source_rgba, image->header.w,
                              image->header.h, image->header.w, &descriptor,
                              LV_IMG_CF_TRUE_COLOR_ALPHA, rotation_colors,
                              rotated_digits[dial][slot].unshifted);
            rotated_digits[dial][slot].value = index;
            rotated_digits[dial][slot].angle = rotation;
        }
        if (rotate || rotated_digits[dial][slot].fraction_x != fx ||
                      rotated_digits[dial][slot].fraction_y != fy) {
            shift_clock_mask((uint8_t *)cached->data,rotated_digits[dial][slot].unshifted,
                             width,height,fx,fy);
            rotated_digits[dial][slot].fraction_x=fx;
            rotated_digits[dial][slot].fraction_y=fy;
        }
        ++rotated.x2;
        ++rotated.y2;
        descriptor.angle = 0;
        descriptor.recolor = lv_color_hex(COLOR_WHITE);
        descriptor.recolor_opa = LV_OPA_COVER;
        const lv_area_t *saved_clip=draw_ctx->clip_area;
        draw_ctx->clip_area=&digit_clip;
        clock_draw_mask(draw_ctx, &rotated, cached, opacity);
        draw_ctx->clip_area=saved_clip;
    }
}

static void motion_timer_callback(lv_timer_t *timer)
{
    (void)timer;
    const uint32_t elapsed = lv_tick_elaps(dot_frame_tick);
    if (elapsed >= DOT_FRAME_MS) {
        dot_frame_tick = lv_tick_get();
        dot_scene_ms += LV_MIN(elapsed, 100);
        render_dot_scene();
    }
    refresh_clock_dials();
}

static void refresh_clock(void)
{
    static time_t previous_second;
    const time_t now = time(NULL);
    if (now == previous_second) return;
    previous_second = now;

    struct tm local_time;
    localtime_r(&now, &local_time);
    clock_valid = local_time.tm_year + 1900 >= 2024;
    if (!clock_valid) {
        lv_obj_clear_flag(time_pending_label, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(header_time_label, "--:--");
        lv_label_set_text(date_label, "--.--");
        lv_label_set_text(date_weekday_label, "---");
        lv_label_set_text(header_date_label, "--.-- ---");
        refresh_clock_dials();
        return;
    }

    lv_obj_add_flag(time_pending_label, LV_OBJ_FLAG_HIDDEN);
    clock_hour = (uint8_t)local_time.tm_hour;
    clock_minute = (uint8_t)local_time.tm_min;
    clock_second = (uint8_t)local_time.tm_sec;
    clock_second_tick = lv_tick_get();
    char text[24];
    strftime(text, sizeof(text), "%H:%M", &local_time);
    if (strcmp(lv_label_get_text(header_time_label), text) != 0) lv_label_set_text(header_time_label, text);
    static const char *weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    snprintf(text, sizeof(text), "%02d.%02d", local_time.tm_mon + 1, local_time.tm_mday);
    if (strcmp(lv_label_get_text(date_label), text) != 0) lv_label_set_text(date_label, text);
    const char *weekday = weekdays[local_time.tm_wday];
    if (strcmp(lv_label_get_text(date_weekday_label), weekday) != 0) lv_label_set_text(date_weekday_label, weekday);
    snprintf(text, sizeof(text), "%02d.%02d %s", local_time.tm_mon + 1, local_time.tm_mday, weekday);
    if (strcmp(lv_label_get_text(header_date_label), text) != 0) lv_label_set_text(header_date_label, text);
    refresh_clock_dials();
}

static void set_standby_position(void *object, int32_t x)
{
    lv_obj_set_x(object, x);
    lv_obj_set_x(control_layer, x - 800);
}

static void standby_exit_ready(lv_anim_t *animation)
{
    (void)animation;
    lv_obj_add_flag(standby_layer, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(motion_timer);
}

static void standby_enter_ready(lv_anim_t *animation)
{
    (void)animation;
    dot_frame_tick = lv_tick_get();
    lv_timer_resume(motion_timer);
}

static void hide_standby(void)
{
    if (!standby_active) return;
    standby_active = false;
    lv_timer_pause(motion_timer);
    lv_disp_trig_activity(lv_obj_get_disp(ui_root));

    lv_anim_del(standby_layer, set_standby_position);
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, standby_layer);
    lv_anim_set_exec_cb(&animation, set_standby_position);
    lv_anim_set_values(&animation,
                       lv_obj_get_x(standby_layer), 800);
    lv_anim_set_time(&animation, 280);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_in_out);
    lv_anim_set_ready_cb(&animation, standby_exit_ready);
    lv_anim_start(&animation);
}

static void standby_event(lv_event_t *event)
{
    (void)event;
    hide_standby();
}

static void server_number(char *out, size_t size, float value, const char *format)
{
    if (isfinite(value)) snprintf(out, size, format, value);
    else snprintf(out, size, "--");
}

static void codex_bar_draw(lv_event_t *event)
{
    const int index = (int)(uintptr_t)lv_event_get_user_data(event);
    lv_area_t bounds;
    lv_obj_get_coords(lv_event_get_target(event), &bounds);
    const float remaining = codex_remaining[index];
    const int lit = isfinite(remaining) ? (int)(remaining * 40 / 100 + 0.5f) : 0;
    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.radius = 0;
    for (int col = 0; col < 40; ++col) {
        dot.bg_color = lv_color_hex(col < lit ? COLOR_WHITE : 0x303237);
        dot.bg_opa = codex_connected || col >= lit ? LV_OPA_COVER : LV_OPA_40;
        for (int row = 0; row < 2; ++row) {
            const lv_area_t area = {bounds.x1 + col * 8, bounds.y1 + row * 8,
                                   bounds.x1 + col * 8 + 3, bounds.y1 + row * 8 + 3};
            lv_draw_rect(lv_event_get_draw_ctx(event), &dot, &area);
        }
    }
}

static void create_codex_card(void)
{
    lv_obj_t *card = lv_obj_create(codex_tile);
    lv_obj_set_size(card, 752, 312);
    lv_obj_center(card);
    set_plain_container(card, COLOR_SURFACE);
    lv_obj_set_style_radius(card, 24, 0);
    lv_obj_t *title = create_label(card, "CODEX", &lv_font_inter_28, COLOR_WHITE);
    lv_obj_set_pos(title, 24, 24);
    codex_plan_label = create_label(card, "--", &lv_font_cjk_24, COLOR_MUTED);
    lv_obj_align(codex_plan_label, LV_ALIGN_TOP_RIGHT, -24, 27);
    lv_obj_set_width(codex_plan_label, 180);
    lv_label_set_long_mode(codex_plan_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(codex_plan_label, LV_TEXT_ALIGN_RIGHT, 0);
    for (int i = 0; i < 2; ++i) {
        const int x = 24 + i * 376;
        const int y = 80;
        lv_obj_t *label = create_label(card, i == 0 ? "WEEKLY LEFT" : "SESSION LEFT",
                                       &lv_font_cjk_24, COLOR_MUTED);
        lv_obj_set_pos(label, x, y);
        if (i == 1) codex_short_label = label;
        codex_values[i] = create_label(card, "--%", &lv_font_inter_48, COLOR_WHITE);
        lv_obj_set_pos(codex_values[i], x, y + 36);
        codex_bars[i] = lv_obj_create(card);
        lv_obj_set_size(codex_bars[i], 320, 12);
        lv_obj_set_pos(codex_bars[i], x, y + 108);
        set_draw_object(codex_bars[i]);
        lv_obj_add_event_cb(codex_bars[i], codex_bar_draw, LV_EVENT_DRAW_MAIN, (void *)(uintptr_t)i);
        codex_resets[i] = create_label(card, "RESET --", &lv_font_cjk_24, COLOR_MUTED);
        lv_obj_set_pos(codex_resets[i], x, y + 138);
    }
    codex_state_label = create_label(card, "WAITING", &lv_font_cjk_24, COLOR_MUTED);
    lv_obj_set_pos(codex_state_label, 24, 264);
}

static void update_codex(const PortableUI_Status *snapshot)
{
    const PortableUI_Codex *usage = &snapshot->codex;
    codex_connected = snapshot->nas_online && usage->online;
    if(codex_connected) {
        bool restored=codex_seen && codex_was_limited && !usage->limited;
        for(int i=0;i<2;i++) {
            if(codex_seen && isfinite(codex_remaining[i]) && isfinite(usage->remaining[i]) &&
               usage->remaining[i]>codex_remaining[i] &&
               (codex_remaining[i]==0 || (codex_previous_reset[i] && usage->reset_at[i]>codex_previous_reset[i])))
                restored=true;
            codex_previous_reset[i]=usage->reset_at[i];
        }
        codex_seen=true;codex_was_limited=usage->limited;
        if(restored)notify_codex_restored();
    }
    const char *state = !codex_connected ? (usage->has_data ? "STALE" : "UNAVAILABLE") :
                        usage->limited ? "LIMIT REACHED" : "LIVE";
    lv_label_set_text(codex_state_label, state);
    lv_obj_set_style_text_color(codex_state_label, lv_color_hex(codex_connected && !usage->limited ?
        COLOR_WHITE : COLOR_MUTED), 0);
    if (usage->has_data) {
        char plan[sizeof(usage->plan)];
        for (size_t i = 0; i < sizeof(plan); ++i) plan[i] = toupper((unsigned char)usage->plan[i]);
        lv_label_set_text(codex_plan_label, plan);
        char text[32];
        if (usage->short_seconds && usage->short_seconds % 3600 == 0)
            snprintf(text, sizeof(text), "%luH LEFT", (unsigned long)usage->short_seconds / 3600);
        else snprintf(text, sizeof(text), "SESSION LEFT");
        lv_label_set_text(codex_short_label, text);
        for (int i = 0; i < 2; ++i) {
            codex_remaining[i] = usage->remaining[i];
            server_number(text, sizeof(text), usage->remaining[i], "%.0f%%");
            lv_label_set_text(codex_values[i], text);
            lv_obj_set_style_text_color(codex_values[i], lv_color_hex(codex_connected ? COLOR_WHITE : COLOR_MUTED), 0);
            if (usage->reset_at[i]) {
                const time_t reset = usage->reset_at[i];
                struct tm local;
                localtime_r(&reset, &local);
                strftime(text, sizeof(text), "RESET %m.%d %H:%M", &local);
            } else snprintf(text, sizeof(text), "RESET --");
            lv_label_set_text(codex_resets[i], text);
        }
    }
    for (int i = 0; i < 2; ++i) lv_obj_invalidate(codex_bars[i]);
}

static void create_standby(lv_obj_t *screen)
{
    standby_layer = lv_obj_create(screen);
    lv_obj_set_size(standby_layer, 800, 480);
    lv_obj_set_pos(standby_layer, 800, 0);
    set_plain_container(standby_layer, COLOR_BACKGROUND);
    lv_obj_add_flag(standby_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(standby_layer, standby_event, LV_EVENT_RELEASED, NULL);

    create_clock_images(standby_layer);
    create_dot_field();

    lv_obj_t *standby_header = lv_obj_create(standby_layer);
    lv_obj_set_size(standby_header, 744, 64);
    lv_obj_set_pos(standby_header, 28, 8);
    set_draw_object(standby_header);
    lv_obj_set_flex_flow(standby_header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(standby_header, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(standby_header, 16, 0);
    date_label = create_label(standby_header, "--.--", &lv_font_inter_28, COLOR_MUTED);
    date_weekday_label = create_label(standby_header, "---", &lv_font_inter_28, COLOR_MUTED);
    lv_obj_set_flex_grow(date_weekday_label, 1);

    hour_dial = lv_obj_create(standby_layer);
    lv_obj_set_size(hour_dial, 400, 400);
    lv_obj_set_pos(hour_dial, 0, 40);
    set_draw_object(hour_dial);
    lv_obj_add_event_cb(hour_dial, clock_dial_draw, LV_EVENT_DRAW_MAIN, NULL);
    minute_dial = lv_obj_create(standby_layer);
    lv_obj_set_size(minute_dial, 400, 400);
    lv_obj_set_pos(minute_dial, 400, 40);
    set_draw_object(minute_dial);
    lv_obj_add_event_cb(minute_dial, clock_dial_draw, LV_EVENT_DRAW_MAIN,
                        NULL);

    create_clock_tracks();
    init_clock_visibility();
    for (int i = 0; i < 2; ++i) {
        standby_printer_labels[i] = create_label(standby_layer, "", &lv_font_cjk_24, COLOR_MUTED);
        lv_obj_set_pos(standby_printer_labels[i], 88 + i * 352, 376);
        lv_obj_add_flag(standby_printer_labels[i], LV_OBJ_FLAG_HIDDEN);
    }
    time_pending_label = create_label(standby_layer, "WAITING FOR TIME", &lv_font_cjk_24, COLOR_MUTED);
    lv_obj_align(time_pending_label, LV_ALIGN_CENTER, 0, 0);
    motion_timer = lv_timer_create(motion_timer_callback, DOT_TIMER_MS, NULL);
    lv_timer_pause(motion_timer);
    lv_obj_add_flag(standby_layer, LV_OBJ_FLAG_HIDDEN);
}


typedef struct { lv_obj_t *label; lv_img_dsc_t image; char text[64]; } PrinterTextCache;
typedef struct {
    lv_obj_t *tile, *card, *fill, *state_label, *state_icon, *name_label, *percent;
    lv_obj_t *bar, *metrics[4], *trays[4], *tray_slots[4], *tray_labels[4], *tray_remaining[4], *ams, *fan, *play;
    lv_obj_t *h2d_metrics[6], *door, *tray_leds[4], *percent_sign, *text_layer;
    lv_obj_t *ext, *ext_led, *ext_slot, *ext_label, *ext_remain, *lane;
    PrinterTextCache text_cache[4];
    BambuState painted;
    bool paint_ready, particle_dirty, ui_pending, fog_ready;
    int painted_tray_page;
    uint32_t tray_tick, particle_renders, text_rasters;
    BambuState view;
    int tray_page;
    bool h2d_layout;
    uint32_t motion_ms,water_tick,stage_changed,previous_motion,particle_seed;
    int previous_stage,previous_effect;
    lv_img_dsc_t water;
    uint8_t *particle_blur,*fog_weights;
} PrinterPage;
static PrinterPage printer_pages[2];
static lv_obj_t *network_label, *network_panel, *nav_line, *todo_tile;
static lv_obj_t *nav_buttons[4];
static int selected_page, printer_slot;
static void activate_page(int page, bool animate);
static void update_printer(PrinterPage *p, const BambuState *s);
static bool printer_page_visible(const PrinterPage *p);
#define COLOR_BAMBU 0x00AE42
/* Native LVGL LEDs: no raster scaling, one draw object per icon/bar. */
static void printer_led(lv_draw_ctx_t *ctx,int x,int y,int size,uint32_t color,lv_opa_t opacity)
{
    static const uint8_t dot2[]={210,210,210,210};
    static const uint8_t dot3[]={100,220,100,220,255,220,100,220,100};
    static const uint8_t dot4[]={72,210,210,72,210,255,255,210,210,255,255,210,72,210,210,72};
    static const uint8_t dot6[]={0,80,210,210,80,0,80,245,255,255,245,80,210,255,255,255,255,210,210,255,255,255,255,210,80,245,255,255,245,80,0,80,210,210,80,0};
    static const lv_img_dsc_t images[]={
        {.header={.cf=LV_IMG_CF_ALPHA_8BIT,.w=2,.h=2},.data_size=4,.data=dot2},
        {.header={.cf=LV_IMG_CF_ALPHA_8BIT,.w=3,.h=3},.data_size=9,.data=dot3},
        {.header={.cf=LV_IMG_CF_ALPHA_8BIT,.w=4,.h=4},.data_size=16,.data=dot4},
        {.header={.cf=LV_IMG_CF_ALPHA_8BIT,.w=6,.h=6},.data_size=36,.data=dot6}};
    lv_draw_img_dsc_t d;lv_draw_img_dsc_init(&d);d.recolor=lv_color_hex(color);
    d.recolor_opa=LV_OPA_COVER;d.opa=opacity;
    lv_area_t a={x,y,x+size-1,y+size-1};lv_draw_img(ctx,&d,&a,&images[size==6?3:size-2]);
}
#include "portable_ui_stages.inc"
static int printer_sine(uint32_t elapsed,uint32_t period)
{
    uint32_t phase=(elapsed%period)*360,angle=phase/period,fraction=phase%period;
    return (lv_trigo_sin(angle)*(int32_t)(period-fraction)+lv_trigo_sin((angle+1)%360)*(int32_t)fraction)/(int32_t)period;
}
static uint16_t printer_breath(const PrinterPage *p)
{ return 222*256+(printer_sine(p->motion_ms,4200)+32767)*30*256/65534; }
static void printer_soft_led(lv_draw_ctx_t *ctx,int x,int y,int size,uint32_t color,uint16_t alpha)
{ printer_led(ctx,x,y,size,color,alpha/256); }
static void status_led_draw(lv_event_t *event)
{
    PrinterPage *p=lv_event_get_user_data(event);
    const BambuState *s=&p->view;
    static const uint16_t run[9]={0x010,0x038,0x07C,0x0FE,0x1FF,0x0FE,0x07C,0x038,0x010};
    static const uint16_t pause[9]={0x0C6,0x0C6,0x0C6,0x0C6,0x0C6,0x0C6,0x0C6,0x0C6,0x0C6};
    static const uint16_t warning[9]={0x010,0x038,0x028,0x06C,0x054,0x0C6,0x082,0x110,0x1FF};
    static const uint16_t done[9]={0,0x100,0x180,0x0C1,0x063,0x036,0x01C,0x008,0};
    static const uint16_t offline[9]={0x101,0x082,0x044,0x028,0x010,0x028,0x044,0x082,0x101};
    static const uint16_t heat[9]={0x044,0x088,0x044,0x022,0x044,0,0x101,0x101,0x1FF};
    static const uint16_t level[9]={0x010,0x038,0x07C,0x010,0,0x101,0x111,0x101,0x1FF};
    static const uint16_t feed[9]={0x07C,0x082,0x092,0x082,0x07C,0x010,0x054,0x038,0x010};
    static const uint16_t scan[9]={0x1C7,0x101,0x101,0,0x1FF,0,0x101,0x101,0x1C7};
    const PrinterEffect effect=printer_effect(s);
    const uint16_t *pattern=!s->online?offline:effect==FX_ERROR?warning:
        effect==FX_PAUSE?pause:effect==FX_FINISH?done:effect==FX_HEAT?heat:
        effect==FX_LEVEL||effect==FX_HOME?level:effect==FX_FEED||effect==FX_VENT?feed:
        effect==FX_FLOW?feed:effect==FX_SCAN||effect==FX_CALIBRATE||effect==FX_LIDAR?scan:run;
    lv_area_t a;lv_obj_get_coords(lv_event_get_target(event),&a);
    for(int y=0;y<9;y++)for(int x=0;x<9;x++)if(pattern[y]&(1<<x))
        printer_soft_led(lv_event_get_draw_ctx(event),a.x1+x*3,a.y1+y*3,2,printer_color(s),
            printer_running(s)||effect==FX_IDLE||effect==FX_FINISH?printer_breath(p):255*256);
}
#include "portable_ui_water.inc"
#include "portable_ui_text.inc"

static uint32_t printer_bar_dim(uint32_t on)
{
    uint32_t r=(on>>16)&255,g=(on>>8)&255,b=on&255;
    return ((20+r*52/255)<<16)|((22+g*58/255)<<8)|(20+b*48/255);
}
static void printer_led_draw(lv_event_t *event)
{
    PrinterPage *p=lv_event_get_user_data(event);
    lv_area_t a;lv_obj_get_coords(lv_event_get_target(event),&a);
    lv_draw_ctx_t *ctx=lv_event_get_draw_ctx(event);
    int lit=p->view.progress<0 ? 0 : (p->view.progress*39+50)/100;
    uint32_t on=printer_color(&p->view),dim=printer_bar_dim(on);
    for(int y=0;y<3;y++)for(int x=0;x<39;x++)
        printer_led(ctx,a.x1+x*6,a.y1+y*6,4,x<lit?on:dim,
            p->view.online || x>=lit ? LV_OPA_COVER:LV_OPA_40);
}
static const BambuTray *printer_tray(const BambuState *s,int unit,int slot)
{
    for(int i=0;i<BAMBU_TRAYS;i++)
        if(s->trays[i].present && s->trays[i].unit==unit && s->trays[i].slot==slot)return &s->trays[i];
    return NULL;
}
static void filament_led_draw(lv_event_t *event)
{
    PrinterPage *p=lv_event_get_user_data(event);
    lv_obj_t *target=lv_event_get_target(event);
    static const BambuTray empty={.remaining=-1};
    const BambuTray *t=&empty;
    if(p->ext_led && target==p->ext_led) {
        const BambuTray *ext=printer_tray(&p->view,254,0);
        if(ext)t=ext;
    } else {
        int index=0;while(index<4 && p->tray_leds[index]!=target)index++;
        if(index<4)t=&p->view.trays[p->tray_page*4+index];
    }
    lv_area_t a;lv_obj_get_coords(target,&a);
    int width=a.x2-a.x1+1,height=a.y2-a.y1+1;
    const int led=4,rows=2,pitch=6;
    int cells=LV_MAX(1,(width-led)/pitch+1);
    const int lit=t->present ? (t->remaining < 0 ? cells : (t->remaining*cells+99)/100) : 0;
    bool active=t->present && (p->view.active_tray==t->unit*4+t->slot || p->view.active_tray==t->unit);
    lv_opa_t on=t->remaining<0 ? LV_OPA_50 : printer_running(&p->view) && active ? printer_breath(p)/256:LV_OPA_COVER;
    int cluster_h=rows*led+(rows-1)*(pitch-led);
    int y0=a.y1+(height-cluster_h)/2;
    for(int y=0;y<rows;y++)for(int x=0;x<cells;x++)
        printer_led(lv_event_get_draw_ctx(event),a.x1+x*pitch,y0+y*pitch,led,
            t->present?t->color:0x26342B,x<lit?on:LV_OPA_20);
}

static void tray_next(lv_event_t *event)
{
    PrinterPage *p=lv_event_get_user_data(event);
    p->tray_page=(p->tray_page+1)%4; update_printer(p,&p->view);
    PortableUI_Activity();
}

 #include "portable_ui_events.inc"
 #include "portable_ui_video.inc"
 #include "portable_ui_edges.inc"

static bool printer_page_visible(const PrinterPage *p)
{ return !video_active && !standby_active && !event_playing && selected_page<2 && p==&printer_pages[selected_page] && !lv_obj_has_flag(p->tile,LV_OBJ_FLAG_HIDDEN); }

void PortableUI_SetPrinterSlot(int slot,const BambuState *state)
{
    if(slot<0 || slot>1 || !state)return;
    bool wake=state->online && strcmp(printer_slots[slot].state,state->state) &&
        (!strcmp(state->state,"RUNNING") || !strcmp(state->state,"FINISH") || !strcmp(state->state,"FAILED"));
    BambuState previous=printer_slots[slot];
    printer_slots[slot]=*state;
    if (standby_print_visible(state)) {
        char percent[12];
        if (state->progress < 0) strcpy(percent, "--");
        else snprintf(percent, sizeof(percent), "%d%%", state->progress);
        printer_text_fmt(standby_printer_labels[slot], "%s  /  %s%s",
            slot == 0 ? "H2D" : "A1 MINI", percent,
            !state->online ? "  OFFLINE" : !strcmp(state->state, "PAUSE") ? "  PAUSED" : "");
        lv_obj_clear_flag(standby_printer_labels[slot], LV_OBJ_FLAG_HIDDEN);
    } else lv_obj_add_flag(standby_printer_labels[slot], LV_OBJ_FLAG_HIDDEN);
    update_printer(&printer_pages[slot],state);
    if(wake)PortableUI_Activity();
    event_transition(slot,&previous,state);
}
static lv_obj_t *printer_label(lv_obj_t *parent, const char *text, int x, int y, int w, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l=create_label(parent,text,font,color);
    lv_obj_set_pos(l,x,y);lv_obj_set_width(l,w);lv_label_set_long_mode(l,LV_LABEL_LONG_DOT);return l;
}
#include "portable_ui_todo.inc"

static void page_x(void *object,int32_t x) { lv_obj_set_x(object,x); }
static void slide_to(lv_obj_t *object,int x)
{
    lv_anim_t animation;lv_anim_init(&animation);
    lv_anim_set_var(&animation,object);lv_anim_set_exec_cb(&animation,page_x);
    lv_anim_set_values(&animation,lv_obj_get_x(object),x);lv_anim_set_time(&animation,280);
    lv_anim_set_path_cb(&animation,lv_anim_path_ease_in_out);lv_anim_start(&animation);
}
static void activate_page(int page,bool animate)
{
    stop_video();
    lv_obj_t *tiles[]={printer_pages[0].tile,printer_pages[1].tile,codex_tile,todo_tile};
    const int old=selected_page;
    if(page==old)animate=false;
    selected_page=page;
    if(page<2)printer_slot=page;
    for(int i=0;i<4;i++) {
        lv_anim_del(tiles[i],page_x);
        lv_obj_set_x(tiles[i],0);
        if(i==page)lv_obj_clear_flag(tiles[i],LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(tiles[i],LV_OBJ_FLAG_HIDDEN);
        if(i==page)lv_obj_add_state(nav_buttons[i],LV_STATE_CHECKED);
        else lv_obj_clear_state(nav_buttons[i],LV_STATE_CHECKED);
    }
    if(page<2)update_printer(&printer_pages[page],&printer_pages[page].view);
    lv_anim_del(nav_line,page_x);
    if(animate)slide_to(nav_line,28+page*156);else lv_obj_set_x(nav_line,28+page*156);
    lv_obj_set_style_bg_color(nav_line,lv_color_hex(page==2?COLOR_ACTIVE:COLOR_BAMBU),0);
}
static void select_page(lv_event_t *event)
{
    int page=(intptr_t)lv_event_get_user_data(event);
    activate_page(page,true);
    if(page==3 && todo_handler && !todo_pending)todo_handler(todo_view.page,todo_filter,0);
    PortableUI_Activity();
}
static void create_printer(int slot)
{
    PrinterPage *p=&printer_pages[slot];
    BambuState_Init(&p->view);
    p->h2d_layout=slot==0;
    p->tile=lv_obj_create(control_layer);lv_obj_set_pos(p->tile,0,136);lv_obj_set_size(p->tile,800,344);
    set_plain_container(p->tile,COLOR_BACKGROUND);
    lv_obj_t *left=lv_obj_create(p->tile);lv_obj_set_pos(left,24,12);lv_obj_set_size(left,272,316);
    p->card=left;
    set_plain_container(left,COLOR_SURFACE);lv_obj_set_style_radius(left,CARD_RADIUS,0);
    p->particle_seed=esp_random();
    p->water=(lv_img_dsc_t){.header={.cf=LV_IMG_CF_ALPHA_8BIT,.w=PARTICLE_WIDTH,.h=PARTICLE_HEIGHT},.data_size=PARTICLE_WIDTH*PARTICLE_HEIGHT};
    p->water.data=heap_caps_calloc(PARTICLE_WIDTH*PARTICLE_HEIGHT,1,MALLOC_CAP_SPIRAM);
    p->particle_blur=heap_caps_calloc(PARTICLE_WIDTH*PARTICLE_HEIGHT,1,MALLOC_CAP_SPIRAM);
    p->fog_weights=heap_caps_calloc(PARTICLE_WIDTH*PARTICLE_HEIGHT,1,MALLOC_CAP_SPIRAM);
    assert(p->water.data && p->particle_blur && p->fog_weights);
    p->particle_dirty=true;
    p->fill=lv_obj_create(left);set_draw_object(p->fill);
    lv_obj_set_pos(p->fill,0,0);lv_obj_set_size(p->fill,PARTICLE_WIDTH,PARTICLE_HEIGHT);
    lv_obj_add_event_cb(p->fill,progress_fill_draw,LV_EVENT_DRAW_MAIN,p);
    p->state_icon=lv_obj_create(left);set_draw_object(p->state_icon);
    lv_obj_set_pos(p->state_icon,20,22);lv_obj_set_size(p->state_icon,28,28);
    lv_obj_add_event_cb(p->state_icon,status_led_draw,LV_EVENT_DRAW_MAIN,p);
    p->state_label=printer_label(left,"OFFLINE",54,20,198,&printer_font20,COLOR_BAMBU);
    p->percent=printer_label(left,"--",20,88,198,&printer_font88,COLOR_WHITE);
    p->percent_sign=printer_label(left,"%",220,0,40,&lv_font_inter_32,COLOR_MUTED);
    lv_obj_set_y(p->percent_sign,88+printer_font88.line_height-printer_font88.base_line
        -lv_font_inter_32.line_height+lv_font_inter_32.base_line);
    p->bar=lv_obj_create(left);set_draw_object(p->bar);
    lv_obj_set_pos(p->bar,20,200);lv_obj_set_size(p->bar,232,16);
    lv_obj_add_event_cb(p->bar,printer_led_draw,LV_EVENT_DRAW_MAIN,p);
    p->metrics[0]=printer_label(left,"--H --M",20,222,248,&lv_font_inter_28,COLOR_MUTED);
    lv_obj_set_style_text_letter_space(p->metrics[0],3,0);
    lv_label_set_long_mode(p->metrics[0],LV_LABEL_LONG_WRAP);lv_obj_set_height(p->metrics[0],68);
    p->name_label=printer_label(p->tile,"Waiting for printer",316,12,390,&printer_font20,COLOR_WHITE);
    lv_obj_set_height(p->name_label,27);
    if(p->h2d_layout) {
        const char *names[]={"NOZZLE L","NOZZLE R","BED","CHAMBER","LAYER","SPEED"};
        for(int i=0;i<6;i++) {
            int x=316+(i%3)*158, y=i<3?50:111;
            printer_label(p->tile,names[i],x,y,144,&printer_font20,COLOR_MUTED);
            p->h2d_metrics[i]=printer_label(p->tile,"--",x,y+27,144,
                i==5?&printer_font20:&printer_font20,COLOR_WHITE);
            lv_obj_set_height(p->h2d_metrics[i],27);
            lv_label_set_long_mode(p->h2d_metrics[i],LV_LABEL_LONG_DOT);
            if(i<3)lv_label_set_recolor(p->h2d_metrics[i],true);
        }
        p->door=printer_label(p->tile,"DOOR --",632,170,144,&printer_font20,COLOR_MUTED);
    } else {
        const char *names[]={"NOZZLE","BED","LAYER"};
        for(int i=0;i<3;i++){
            int x=316+i*158;
            printer_label(p->tile,names[i],x,72,148,&printer_font20,COLOR_MUTED);
            p->metrics[i+1]=printer_label(p->tile,"-- / --",x,104,148,&printer_font20,COLOR_WHITE);
            if(i<2)lv_label_set_recolor(p->metrics[i+1],true);
        }
    }
    p->ams=printer_label(p->tile,"AMS 1  >",p->h2d_layout?416:316,p->h2d_layout?201:166,p->h2d_layout?360:200,&printer_font20,COLOR_MUTED);
    lv_obj_add_flag(p->ams,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(p->ams,tray_next,LV_EVENT_CLICKED,p);
    p->fan=printer_label(p->tile,p->h2d_layout?"PART FAN --":"FAN --%",p->h2d_layout?316:590,p->h2d_layout?170:166,p->h2d_layout?240:186,&printer_font20,COLOR_MUTED);
    if(p->h2d_layout) {
        p->lane=printer_label(p->tile,"L",316,201,84,&printer_font20,COLOR_MUTED);
        lv_obj_set_height(p->lane,27);lv_obj_set_height(p->ams,27);
        p->ext=lv_obj_create(p->tile);
        lv_obj_set_pos(p->ext,316,232);lv_obj_set_size(p->ext,84,103);
        set_plain_container(p->ext,0x101813);lv_obj_set_style_radius(p->ext,8,0);
        lv_obj_set_style_border_width(p->ext,2,0);
        lv_obj_set_style_border_color(p->ext,lv_color_hex(COLOR_BAMBU),0);
        lv_obj_set_style_border_opa(p->ext,LV_OPA_TRANSP,0);
        p->ext_led=lv_obj_create(p->ext);set_draw_object(p->ext_led);
        lv_obj_set_pos(p->ext_led,6,10);lv_obj_set_size(p->ext_led,71,16);
        lv_obj_add_event_cb(p->ext_led,filament_led_draw,LV_EVENT_DRAW_MAIN,p);
        p->ext_slot=printer_label(p->ext,"EXT",6,25,72,&printer_font18,COLOR_MUTED);
        p->ext_label=printer_label(p->ext,"--",6,49,72,&printer_font18,COLOR_WHITE);
        lv_label_set_long_mode(p->ext_label,LV_LABEL_LONG_DOT);lv_obj_set_height(p->ext_label,24);
        p->ext_remain=printer_label(p->ext,"--",6,73,72,&printer_font18,COLOR_MUTED);
    }
    for(int i=0;i<4;i++){
        p->trays[i]=lv_obj_create(p->tile);
        lv_obj_set_pos(p->trays[i],p->h2d_layout?416+i*92:316+i*118,p->h2d_layout?232:198);
        lv_obj_set_size(p->trays[i],p->h2d_layout?84:106,p->h2d_layout?103:130);
        set_plain_container(p->trays[i],0x101813);
        lv_obj_set_style_border_width(p->trays[i],2,0);
        lv_obj_set_style_border_color(p->trays[i],lv_color_hex(COLOR_BAMBU),0);
        lv_obj_set_style_border_opa(p->trays[i],LV_OPA_TRANSP,0);
        p->tray_leds[i]=lv_obj_create(p->trays[i]);set_draw_object(p->tray_leds[i]);
        lv_obj_set_pos(p->tray_leds[i],p->h2d_layout?6:8,10);
        lv_obj_set_size(p->tray_leds[i],p->h2d_layout?71:89,16);
        lv_obj_add_event_cb(p->tray_leds[i],filament_led_draw,LV_EVENT_DRAW_MAIN,p);
        lv_obj_set_style_radius(p->trays[i],8,0);
        p->tray_slots[i]=printer_label(p->trays[i],"--",p->h2d_layout?6:8,p->h2d_layout?25:32,p->h2d_layout?72:90,&printer_font18,COLOR_MUTED);
        p->tray_labels[i]=printer_label(p->trays[i],"--",p->h2d_layout?6:8,p->h2d_layout?49:61,p->h2d_layout?72:90,&printer_font18,COLOR_WHITE);
        lv_label_set_long_mode(p->tray_labels[i],LV_LABEL_LONG_DOT);
        lv_obj_set_height(p->tray_labels[i],24);
        p->tray_remaining[i]=printer_label(p->trays[i],"--",p->h2d_layout?6:8,p->h2d_layout?73:96,p->h2d_layout?72:90,&printer_font18,COLOR_MUTED);
        lv_obj_set_style_text_align(p->tray_remaining[i],LV_TEXT_ALIGN_LEFT,0);
    }
    lv_obj_t *cached_labels[]={p->state_label,p->percent,p->percent_sign,p->metrics[0]};
    for(int i=0;i<4;i++){
        PrinterTextCache *cache=&p->text_cache[i];cache->label=cached_labels[i];
        int width=lv_obj_get_style_width(cache->label,0),height=lv_obj_get_style_text_font(cache->label,0)->line_height;
        if(cache->label==p->metrics[0])height*=2;
        cache->image=(lv_img_dsc_t){.header={.cf=LV_IMG_CF_ALPHA_8BIT,.w=width,.h=height},.data_size=width*height};
        cache->image.data=heap_caps_calloc(width*height,1,MALLOC_CAP_SPIRAM);assert(cache->image.data);
        lv_obj_set_style_text_opa(cache->label,LV_OPA_TRANSP,0);
    }
    p->text_layer=lv_obj_create(left);set_draw_object(p->text_layer);
    lv_obj_set_size(p->text_layer,272,316);
    lv_obj_add_event_cb(p->text_layer,printer_text_draw,LV_EVENT_DRAW_MAIN,p);
    /* H2D uses equal-aspect 1 + 4 cards; A1 MINI keeps its four-slot layout. */
    p->play=lv_btn_create(p->tile);set_plain_container(p->play,COLOR_SURFACE);
    lv_obj_set_pos(p->play,724,12);lv_obj_set_size(p->play,52,p->h2d_layout?32:56);
    lv_obj_set_style_radius(p->play,10,0);lv_obj_set_style_shadow_width(p->play,0,0);
    lv_obj_add_state(p->play,LV_STATE_DISABLED);
    lv_obj_add_event_cb(p->play,play_video,LV_EVENT_CLICKED,(void *)(intptr_t)slot);
    lv_obj_t *play_label=create_label(p->play,LV_SYMBOL_PLAY,LV_FONT_DEFAULT,COLOR_MUTED);lv_obj_center(play_label);
    lv_obj_add_flag(p->tile,LV_OBJ_FLAG_HIDDEN);
    printer_cache_text(p);
    render_printer_water(p);
    p->particle_dirty=false;
}
static void network_close(lv_event_t *event)
{
    (void)event;lv_obj_add_flag(network_panel,LV_OBJ_FLAG_HIDDEN);
}
static void header_action(lv_event_t *event)
{
    static uint32_t date_pressed;
    if(lv_event_get_target(event)==header_time_label)PortableUI_SetStandby(true);
    else if(lv_event_get_code(event)==LV_EVENT_PRESSED)date_pressed=lv_tick_get();
    else if(lv_tick_elaps(date_pressed)>=2000 && lv_obj_has_flag(network_panel,LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(network_panel,LV_OBJ_FLAG_HIDDEN);lv_obj_move_foreground(network_panel);
    }
}
void PortableUI_SetNetwork(const char *message)
{ if(network_label && message) lv_label_set_text(network_label,message); }
void PortableUI_SetPrinter(const BambuState *s)
{
    update_printer(&printer_pages[printer_slot],s);
}
static void printer_temperature(char *out,size_t size,int current,int target,bool online)
{
    if(current<0)snprintf(out,size,"--");
    else if(online && target>current)snprintf(out,size,"#EF4248 %d#",current);
    else snprintf(out,size,"%d",current);
}
static void update_printer(PrinterPage *p,const BambuState *s)
{
    bool effect_changed=s->stage!=p->view.stage || printer_effect(s)!=printer_effect(&p->view);
    bool background_changed=effect_changed || s->progress!=p->view.progress || s->speed_level!=p->view.speed_level || printer_color(s)!=printer_color(&p->view);
    if(effect_changed){
        p->previous_effect=printer_effect(&p->view);p->previous_stage=p->view.stage;
        p->previous_motion=p->motion_ms-p->stage_changed;p->stage_changed=p->motion_ms;
    }
    p->particle_dirty|=background_changed;
    if(s!=&p->view){p->view=*s;p->ui_pending=true;}
    if(!printer_page_visible(p))return;
    if(!p->ui_pending && p->paint_ready && p->painted_tray_page==p->tray_page)return;
    const BambuState *old=&p->painted;
    bool first=!p->paint_ready;
    char stage_text[48];const char *state=printer_status(s,stage_text,sizeof(stage_text));
    printer_text(p->state_label,state);
    if(first || printer_effect(old)!=printer_effect(s))lv_obj_invalidate(p->state_icon);
    printer_text_color(p->state_label,printer_color(s));
    printer_text(p->name_label,s->name[0] ? s->name : "Waiting for printer");

    if(s->progress>=0) printer_text_fmt(p->percent,"%d",s->progress);else printer_text(p->percent,"--");
    printer_text_color(p->percent,s->online?COLOR_WHITE:COLOR_MUTED);
    if(first || old->progress!=s->progress || old->online!=s->online || printer_color(old)!=printer_color(s))lv_obj_invalidate(p->bar);
    bool finished=!strcmp(s->state,"FINISH");
    if(finished)printer_text(p->metrics[0],"AWAIT AN\nOPPORTUNITY");
    else if(s->remaining>=0) printer_text_fmt(p->metrics[0],"%dH %02dM",s->remaining/60,s->remaining%60);
    else printer_text(p->metrics[0],"--H --M");
    printer_text_color(p->metrics[0],COLOR_MUTED);
    if(p->h2d_layout) {
        /* Protocol nozzle IDs are right=0, left=1; screen order is left then right. */
        const int current[]={s->nozzle_current[1],s->nozzle_current[0],s->bed};
        const int target[]={s->nozzle_targets[1],s->nozzle_targets[0],s->bed_target};
        for(int i=0;i<3;i++) {
            char value[32], goal[16]="--";
            printer_temperature(value,sizeof(value),current[i],target[i],s->online);
            if(target[i]>=0)snprintf(goal,sizeof(goal),"%d",target[i]);
            printer_text_fmt(p->h2d_metrics[i],"%s/%s",value,goal);
        }
        if(s->chamber>=0)printer_text_fmt(p->h2d_metrics[3],"%d°C",s->chamber);
        else printer_text(p->h2d_metrics[3],"--");
        char layer[16]="--",layers[16]="--";
        if(s->layer>=0)snprintf(layer,sizeof(layer),"%d",s->layer);
        if(s->layers>=0)snprintf(layers,sizeof(layers),"%d",s->layers);
        printer_text_fmt(p->h2d_metrics[4],"%s/%s",layer,layers);
        const char *speeds[]={"SILENT","STANDARD","SPORT","LUDICROUS"};
        printer_text(p->h2d_metrics[5],s->speed_level<0?"--":speeds[s->speed_level-1]);
        printer_text(p->door,s->door_open<0?"DOOR --":s->door_open?"DOOR OPEN":"DOOR SHUT");
        printer_text_color(p->door,s->online && s->door_open==1?0xE9AA42:COLOR_MUTED);
        if(s->fan>=0)printer_text_fmt(p->fan,"PART FAN %d%%",s->fan);
        else printer_text(p->fan,"PART FAN --");
    } else {
        const int current[]={s->nozzle,s->bed,s->layer}, target[]={s->nozzle_target,s->bed_target,s->layers};
        for(int i=0;i<3;i++) {
            char value[32]="--",goal[16]="--";
            if(i<2){
                printer_temperature(value,sizeof(value),current[i],target[i],s->online);
            }else if(current[i]>=0)snprintf(value,sizeof(value),"%d",current[i]);
            if(target[i]>=0)snprintf(goal,sizeof(goal),"%d",target[i]);
            printer_text_fmt(p->metrics[i+1],"%s / %s",value,goal);
        }
        if(s->fan>=0)printer_text_fmt(p->fan,"FAN %d%%",s->fan);
        else printer_text(p->fan,"FAN --%");
    }
    printer_text_fmt(p->ams,p->h2d_layout?"R / AMS %d  >":"AMS %d  >",p->tray_page+1);
    for(int i=0;i<4;i++){
        const BambuTray *t=&s->trays[p->tray_page*4+i];
        const BambuTray *prior=&old->trays[p->tray_page*4+i];
        bool active=t->present && s->active_tray==t->unit*4+t->slot;
        bool was_active=prior->present && old->active_tray==prior->unit*4+prior->slot;
        if(first || p->painted_tray_page!=p->tray_page || t->present!=prior->present || t->color!=prior->color || t->remaining!=prior->remaining || active!=was_active)
            lv_obj_invalidate(p->tray_leds[i]);
        lv_opa_t opacity=active?LV_OPA_COVER:LV_OPA_TRANSP;
        if(lv_obj_get_style_border_opa(p->trays[i],0)!=opacity)lv_obj_set_style_border_opa(p->trays[i],opacity,0);
        if(t->present) {
            char remain[16]="--";if(t->remaining>=0)snprintf(remain,sizeof(remain),"%d%%",t->remaining);
            printer_text_fmt(p->tray_slots[i],"%d.%d",t->unit+1,t->slot+1);
            printer_text(p->tray_labels[i],t->material);
            printer_text(p->tray_remaining[i],remain);
        }
        else {printer_text(p->tray_slots[i],"--");printer_text(p->tray_labels[i],"--");printer_text(p->tray_remaining[i],"--");}
    }
    if(p->h2d_layout && p->ext){
        const BambuTray *ext=printer_tray(s,254,0);
        const BambuTray *was=printer_tray(old,254,0);
        bool active=ext && s->active_tray==254, was_active=was && old->active_tray==254;
        if(first || !ext!=!was || (ext && was && (ext->color!=was->color || ext->remaining!=was->remaining)) || active!=was_active)
            lv_obj_invalidate(p->ext_led);
        lv_opa_t opacity=active?LV_OPA_COVER:LV_OPA_TRANSP;
        if(lv_obj_get_style_border_opa(p->ext,0)!=opacity)lv_obj_set_style_border_opa(p->ext,opacity,0);
        if(ext){
            printer_text(p->ext_label,ext->material[0]?ext->material:"--");
            if(ext->remaining>=0)printer_text_fmt(p->ext_remain,"%d%%",ext->remaining);
            else printer_text(p->ext_remain,"N/A");
        }else{
            printer_text(p->ext_label,"--");printer_text(p->ext_remain,"--");
        }
    }
    printer_cache_text(p);
    if(p->particle_dirty)lv_obj_invalidate(p->fill);
    p->painted=*s;p->painted_tray_page=p->tray_page;p->paint_ready=true;p->ui_pending=false;
}

/* Single retained instance per boot. Own it for the lifetime of the LVGL display;
 * hide/switch views instead of deleting the returned root or its parent. */
lv_obj_t *PortableUI_Create(lv_obj_t *parent)
{
    if (ui_root || !parent) return NULL;
    lv_obj_update_layout(parent);
    if (lv_obj_get_content_width(parent) < 800 || lv_obj_get_content_height(parent) < 480) return NULL;
    ui_root = lv_obj_create(parent);
    lv_obj_set_size(ui_root, 800, 480);
    set_plain_container(ui_root, COLOR_BACKGROUND);
    lv_obj_center(ui_root);
    lv_obj_set_style_text_font(ui_root, &lv_font_inter_28, 0);
    control_layer = lv_obj_create(ui_root);
    lv_obj_set_size(control_layer, 800, 480);
    set_plain_container(control_layer, COLOR_BACKGROUND);
    lv_obj_t *header = lv_obj_create(control_layer);
    lv_obj_set_size(header, 800, 80);
    set_plain_container(header, COLOR_BACKGROUND);
    header_date_label = create_label(header, "--.-- ---", &lv_font_inter_28, COLOR_MUTED);
    lv_obj_align(header_date_label, LV_ALIGN_LEFT_MID, 28, 0);
    header_time_label = create_label(header, "--:--", &lv_font_inter_40, COLOR_WHITE);
    lv_obj_align(header_time_label, LV_ALIGN_RIGHT_MID, -28, 0);
    lv_obj_t *navigation = lv_obj_create(control_layer);
    lv_obj_set_size(navigation, 800, 56);
    lv_obj_set_pos(navigation, 0, 80);
    set_plain_container(navigation, 0x0B0C0E);
    const char *tabs[]={"H2D","A1 MINI","CODEX","TODO"};
    for(int i=0;i<4;i++) {
        lv_obj_t *button=lv_btn_create(navigation);lv_obj_set_pos(button,24+i*156,0);lv_obj_set_size(button,152,48);
        nav_buttons[i]=button;
        set_plain_container(button,0x0B0C0E);lv_obj_set_style_shadow_width(button,0,0);
        lv_obj_set_style_bg_color(button,lv_color_hex(0x17241B),LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(button,lv_color_hex(0x244631),LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(button,lv_color_hex(0x244631),LV_STATE_CHECKED|LV_STATE_PRESSED);
        if(i==0)lv_obj_add_state(button,LV_STATE_CHECKED);
        lv_obj_t *label=create_label(button,tabs[i],&lv_font_cjk_24,COLOR_WHITE);lv_obj_center(label);
        lv_obj_add_event_cb(button,select_page,LV_EVENT_CLICKED,(void *)(intptr_t)i);
    }
    network_panel=lv_obj_create(control_layer);set_plain_container(network_panel,COLOR_SURFACE);
    lv_obj_set_pos(network_panel,24,14);lv_obj_set_size(network_panel,600,62);
    network_label=printer_label(network_panel,"Wi-Fi starting",12,14,576,&lv_font_cjk_24,COLOR_WHITE);
    lv_obj_add_flag(network_panel,LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(network_panel,network_close,LV_EVENT_CLICKED,NULL);
    lv_obj_add_flag(header_date_label,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(header_date_label,header_action,LV_EVENT_PRESSED,NULL);
    lv_obj_add_event_cb(header_date_label,header_action,LV_EVENT_LONG_PRESSED_REPEAT,NULL);
    lv_obj_add_flag(header_time_label,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(header_time_label,header_action,LV_EVENT_CLICKED,NULL);
    nav_line=lv_obj_create(navigation);lv_obj_set_pos(nav_line,28,50);lv_obj_set_size(nav_line,144,3);
    set_plain_container(nav_line,COLOR_BAMBU);
    codex_tile = lv_obj_create(control_layer);
    lv_obj_set_size(codex_tile, 800, 344);
    lv_obj_set_pos(codex_tile, 0, 136);
    set_plain_container(codex_tile, COLOR_BACKGROUND);
    create_codex_card();
    for(int i=0;i<2;i++) {BambuState_Init(&printer_slots[i]);create_printer(i);}
    create_todo();
    activate_page(0,false);
    create_standby(ui_root);
    create_events();
    create_video();
    refresh_clock();
    lv_disp_trig_activity(lv_obj_get_disp(ui_root));
    lv_timer_create(printer_visual_timer,16,NULL);
    return ui_root;
}

void PortableUI_SetStandby(bool show)
{
    if (!ui_root) return;
    if (!show) { hide_standby(); return; }
    if (standby_active) return;
    stop_video();
    lv_obj_add_flag(network_panel,LV_OBJ_FLAG_HIDDEN);
    standby_active = true;
    dot_frame_tick = lv_tick_get();
    lv_timer_pause(motion_timer);
    refresh_clock_dials();
    lv_obj_clear_flag(standby_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(standby_layer);
    lv_anim_del(standby_layer, set_standby_position);
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, standby_layer);
    lv_anim_set_exec_cb(&animation, set_standby_position);
    lv_anim_set_values(&animation, lv_obj_get_x(standby_layer), 0);
    lv_anim_set_time(&animation, 280);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_in_out);
    lv_anim_set_ready_cb(&animation, standby_enter_ready);
    lv_anim_start(&animation);
}

void PortableUI_Activity(void)
{
    if (!ui_root) return;
    lv_disp_trig_activity(lv_obj_get_disp(ui_root));
    PortableUI_SetStandby(false);
}

void PortableUI_Process(void)
{
    if (!ui_root) return;
    if(!video_active)refresh_clock();
    event_process();
    todo_process();
    if (event_playing) return;
    if (!standby_active && !video_active && lv_disp_get_inactive_time(lv_obj_get_disp(ui_root)) >= STANDBY_TIMEOUT_MS)
        PortableUI_SetStandby(true);
}

void PortableUI_SetStatus(const PortableUI_Status *status)
{
    if (!ui_root || !status) return;
    PortableUI_Status checked = *status;
    checked.codex.plan[sizeof(checked.codex.plan)-1] = '\0';
    for (int i = 0; i < 2; ++i) {
        if (!isfinite(checked.codex.remaining[i]) || checked.codex.remaining[i] < 0 || checked.codex.remaining[i] > 100)
            checked.codex.remaining[i] = NAN;
    }
    checked.codex.online = checked.codex.online && checked.codex.has_data;
    update_codex(&checked);
}

void PortableUI_SetCodex(const PortableUI_Codex *state)
{
    PortableUI_Status status={.nas_online=true,.codex=*state};
    PortableUI_SetStatus(&status);
}
