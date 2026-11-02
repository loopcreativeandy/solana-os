// ILI9341 on the shared SPI bus, driven by LVGL through esp_lvgl_port.

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_lvgl_port.h"
#include "esp_check.h"
#include "esp_log.h"
#include "bsp.h"
#include "bsp_bus.h"

static const char *TAG = "bsp_display";
static SemaphoreHandle_t s_lcd_idle;    // see bsp_bus.h

void bsp_lcd_hold(void)
{
    if (s_lcd_idle) {
        xSemaphoreTake(s_lcd_idle, portMAX_DELAY);
    }
}

void bsp_lcd_release(void)
{
    if (s_lcd_idle) {
        xSemaphoreGive(s_lcd_idle);
    }
}

static void flush_start(lv_event_t *e)
{
    bsp_lcd_hold();
}

// Replaces esp_lvgl_port's own callback, which only calls lv_display_flush_ready()
// (esp_lcd logs a warning that it was overwritten).
static bool flush_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    BaseType_t woken = pdFALSE;
    lv_display_flush_ready(ctx);
    xSemaphoreGiveFromISR(s_lcd_idle, &woken);
    return woken == pdTRUE;
}

// Backlight: LEDC PWM on the BL pin. 25 kHz is far above anything a camera's
// shutter or rolling readout can beat against, so dimmed video doesn't band.
#define BL_FREQ_HZ  25000
#define BL_RES      LEDC_TIMER_10_BIT
#define BL_MAX      ((1 << 10) - 1)

static bool s_bl_ready;
static bool s_bl_on;
static uint8_t s_bl_level = 100;

static void bl_apply(void)
{
    if (!s_bl_ready) {
        return;
    }
    // Perceived brightness is roughly the square of the duty: 50% looks half as bright.
    uint32_t p = s_bl_on ? s_bl_level : 0;
    uint32_t duty = (p * p * BL_MAX + 5000) / 10000;
    if (p && !duty) {
        duty = 1;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void bl_init(void)
{
    const ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BL_RES,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = BL_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t c = {
        .gpio_num = BOARD_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    if (ledc_timer_config(&t) != ESP_OK || ledc_channel_config(&c) != ESP_OK) {
        // Fall back to plain on/off so the screen still works.
        ESP_LOGW(TAG, "backlight PWM unavailable, on/off only");
        gpio_config_t bl = { .pin_bit_mask = BIT64(BOARD_LCD_BL), .mode = GPIO_MODE_OUTPUT };
        gpio_config(&bl);
        return;
    }
    s_bl_ready = true;
}

void bsp_backlight(bool on)
{
    s_bl_on = on;
    if (s_bl_ready) {
        bl_apply();
    } else {
        gpio_set_level(BOARD_LCD_BL, on);
    }
}

void bsp_backlight_level(int percent)
{
    s_bl_level = percent < BSP_BL_MIN ? BSP_BL_MIN : percent > 100 ? 100 : percent;
    bl_apply();
}

int bsp_backlight_get_level(void)
{
    return s_bl_level;
}

lv_display_t *bsp_display_start(void)
{
    bl_init();
    bsp_backlight(false);
    ESP_ERROR_CHECK(bsp_spi_bus_init());

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BOARD_LCD_CS,
        .dc_gpio_num = BOARD_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = BOARD_LCD_PCLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_panel_io_handle_t io;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_SPI_HOST, &io_cfg, &io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BOARD_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    esp_lcd_panel_handle_t panel;
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    // Production panels show true colours without inversion; with it, RED reads on
    // cyan and GREEN on magenta. (The prototype's panel was the other way round.)
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_stack = 8192;     // deep object trees (the Guide's pack reveal) left ~600 B of 7 KB
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = BOARD_LCD_H_RES * 24,     // 2 x 15 KB DMA; RAM is tight
        .double_buffer = true,
        .hres = BOARD_LCD_H_RES,
        .vres = BOARD_LCD_V_RES,
        .rotation = { .swap_xy = true, .mirror_x = true, .mirror_y = true },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = { .buff_dma = true, .swap_bytes = true },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(disp, NULL, TAG, "lvgl display");
    s_lcd_idle = xSemaphoreCreateBinary();
    xSemaphoreGive(s_lcd_idle);
    const esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = flush_done };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io, &cbs, disp));
    lv_display_add_event_cb(disp, flush_start, LV_EVENT_FLUSH_START, NULL);
    bsp_backlight(true);
    ESP_LOGI(TAG, "LCD %dx%d ready", BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    return disp;
}
