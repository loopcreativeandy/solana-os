// Screen mirror over the console (optional: CONFIG_SOLANA_OS_SCREEN_MIRROR).
// "mirror on [baud]" streams every area LVGL flushes, run-length encoded,
// until "mirror off"; tools/mirror.py rebuilds the screen in a window (or
// PNGs) on the computer. Off at boot: until it's switched on, the two event
// callbacks return straight away.
//
// Stream, one text line each, so logs and the prompt can sit in between:
//   MIR ON <w> <h>          mirroring started; a full frame follows
//   MIR A <x> <y> <w> <h>   an area starts
//   MIR D <base64>          its data: runs of (count-1: u8, RGB565 little-endian: u16), row by row
//   MIR E                   the area is complete
//   MIR F                   a refresh is complete: the screen is consistent now
//   MIR OFF
// Only changed areas are sent, so a menu costs little; a full-screen
// animation is limited by the console (115200 baud ~ 11 KB/s, or the baud
// given to "mirror on"). Printing happens on the LVGL task: a slow link
// slows the screen down, never the other way around.

#include "sdkconfig.h"

#if CONFIG_SOLANA_OS_SCREEN_MIRROR

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "mbedtls/base64.h"
#include "lvgl_private.h"
#include "bsp.h"
#include "console.h"
#include "os.h"
#include "os_cmd.h"

static volatile bool s_on;
static bool s_dirty;                // an area went out since the last MIR F
static uint8_t s_raw[384];          // whole runs (3 bytes each) waiting for a MIR D line
static size_t s_len;

static void put_line(void)
{
    if (!s_len) {
        return;
    }
    static unsigned char b64[sizeof(s_raw) * 4 / 3 + 4];
    size_t n = 0;
    mbedtls_base64_encode(b64, sizeof(b64), &n, s_raw, s_len);
    b64[n] = 0;
    printf("MIR D %s\n", b64);
    s_len = 0;
}

static void put_run(int count, uint16_t c)
{
    if (s_len + 3 > sizeof(s_raw)) {
        put_line();
    }
    s_raw[s_len++] = (uint8_t)(count - 1);
    s_raw[s_len++] = (uint8_t)c;
    s_raw[s_len++] = (uint8_t)(c >> 8);
}

// LVGL task, before the area goes to the panel (still in LVGL's byte order).
static void on_flush(lv_event_t *e)
{
    if (!s_on) {
        return;
    }
    const lv_area_t *a = lv_event_get_param(e);
    lv_display_t *disp = lv_event_get_current_target(e);
    const uint16_t *px = (const uint16_t *)disp->buf_act->data;
    int w = lv_area_get_width(a), h = lv_area_get_height(a);
    printf("MIR A %d %d %d %d\n", (int)a->x1, (int)a->y1, w, h);
    int n = w * h;
    for (int i = 0; i < n;) {
        uint16_t c = px[i];
        int run = 1;
        while (i + run < n && run < 256 && px[i + run] == c) {
            run++;
        }
        put_run(run, c);
        i += run;
    }
    put_line();
    printf("MIR E\n");
    s_dirty = true;
}

static void on_refr_ready(lv_event_t *e)
{
    if (s_on && s_dirty) {
        s_dirty = false;
        printf("MIR F\n");
        fflush(stdout);
    }
}

// OS task (LVGL locked).
static void start(void *arg)
{
    if (s_on) {
        return;
    }
    esp_log_level_set("*", ESP_LOG_WARN);   // the link is busy enough
    printf("MIR ON %d %d\n", BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    s_dirty = false;
    s_on = true;
    lv_obj_invalidate(lv_screen_active());  // the first frame is the whole screen
    lv_obj_invalidate(lv_layer_top());
}

static void stop(void *arg)
{
    s_on = false;
    printf("MIR OFF\n");
    fflush(stdout);
    esp_log_level_set("*", ESP_LOG_INFO);
}

static int cmd_mirror(int argc, char **argv)
{
    bool on = argc > 1 && !strcmp(argv[1], "on");
    if (argc < 2 || (!on && strcmp(argv[1], "off"))) {
        printf("mirror: %s\n", s_on ? "on" : "off");
        return 0;
    }
    if (!on) {
        os_post(stop, NULL);
        vTaskDelay(pdMS_TO_TICKS(100));
        console_set_baud(0);                // back to the configured speed
        return 0;
    }
    int baud = argc > 2 ? atoi(argv[2]) : 0;
    if (baud) {
        printf("MIR BAUD %d\n", baud);
        if (console_set_baud(baud)) {
            printf("mirror: can't change the speed here, staying as it is\n");
        } else {
            vTaskDelay(pdMS_TO_TICKS(300));     // the computer switches when it reads MIR BAUD
        }
    }
    os_post(start, NULL);
    return 0;
}

void os_mirror_init(void)
{
    lv_display_t *disp = lv_display_get_default();
    lv_display_add_event_cb(disp, on_flush, LV_EVENT_FLUSH_START, NULL);
    lv_display_add_event_cb(disp, on_refr_ready, LV_EVENT_REFR_READY, NULL);
    os_cmd_register("mirror", "mirror [on [baud]|off] - stream the screen (tools/mirror.py)", cmd_mirror);
}

#else

void os_mirror_init(void)
{
}

#endif
