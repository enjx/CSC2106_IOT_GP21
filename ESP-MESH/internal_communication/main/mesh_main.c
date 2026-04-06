/*
 * ESP-MESH — Fixed Root, No External Router
 * With ST7789 LCD display (M5StickC Plus)
 *
 * Root node  : flash with CONFIG_MESH_IS_ROOT=y
 * Other nodes: flash with CONFIG_MESH_IS_ROOT=n
 *
 * LED:  Blinking = searching | Solid ON = connected
 * LCD:  Shows MAC, Layer, Role, Node count, Connection status
 */

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_mesh.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/uart.h"

#include "mbedtls/md.h"      /* HMAC-SHA256 for application-layer auth */
#include "esp_random.h"      /* esp_random() for nonce generation       */

static const char *TAG = "esp_mesh";

/* ================================================================
 *  ST7789 LCD DRIVER  (M5StickC Plus — 135 x 240)
 * ================================================================ */

#define LCD_MOSI    15
#define LCD_SCLK    13
#define LCD_CS       5
#define LCD_DC      23
#define LCD_RST     18
#define LCD_W      135
#define LCD_H      240
#define LCD_X_OFF   52    /* column offset for 135px ST7789 portrait */
#define LCD_Y_OFF   40    /* row offset for 135px ST7789 portrait */

static spi_device_handle_t s_lcd_spi;

/* ----------------------------------------------------------------
 *  AXP192 PMIC — bit-bang I2C (bypasses driver GPIO conflict)
 * ---------------------------------------------------------------- */
#define AXP192_ADDR  0x34
#define AXP_SDA      21
#define AXP_SCL      22
#define I2C_DELAY_US 5

#define SDA_HIGH() gpio_set_level(AXP_SDA, 1)
#define SDA_LOW()  gpio_set_level(AXP_SDA, 0)
#define SCL_HIGH() gpio_set_level(AXP_SCL, 1)
#define SCL_LOW()  gpio_set_level(AXP_SCL, 0)
#define I2C_DLY()  esp_rom_delay_us(I2C_DELAY_US)

static void bb_i2c_start(void)
{
    SDA_HIGH(); I2C_DLY();
    SCL_HIGH(); I2C_DLY();
    SDA_LOW();  I2C_DLY();
    SCL_LOW();  I2C_DLY();
}

static void bb_i2c_stop(void)
{
    SDA_LOW();  I2C_DLY();
    SCL_HIGH(); I2C_DLY();
    SDA_HIGH(); I2C_DLY();
}

static bool bb_i2c_write_byte(uint8_t byte)
{
    for (int i = 7; i >= 0; i--) {
        if ((byte >> i) & 1) { SDA_HIGH(); } else { SDA_LOW(); }
        I2C_DLY();
        SCL_HIGH(); I2C_DLY();
        SCL_LOW();  I2C_DLY();
    }
    /* read ACK */
    SDA_HIGH();
    gpio_set_direction(AXP_SDA, GPIO_MODE_INPUT);
    I2C_DLY();
    SCL_HIGH(); I2C_DLY();
    bool ack = (gpio_get_level(AXP_SDA) == 0);
    SCL_LOW();
    gpio_set_direction(AXP_SDA, GPIO_MODE_OUTPUT);
    I2C_DLY();
    return ack;
}

static void axp192_write(uint8_t reg, uint8_t val)
{
    bb_i2c_start();
    bb_i2c_write_byte((AXP192_ADDR << 1) | 0); /* write */
    bb_i2c_write_byte(reg);
    bb_i2c_write_byte(val);
    bb_i2c_stop();
}

static void axp192_init(void)
{
    /* Configure pins as open-drain outputs with pull-ups */
    gpio_reset_pin(AXP_SDA);
    gpio_reset_pin(AXP_SCL);
    gpio_set_direction(AXP_SDA, GPIO_MODE_OUTPUT);
    gpio_set_direction(AXP_SCL, GPIO_MODE_OUTPUT);
    gpio_set_pull_mode(AXP_SDA, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(AXP_SCL, GPIO_PULLUP_ONLY);
    SDA_HIGH(); SCL_HIGH();
    vTaskDelay(pdMS_TO_TICKS(10));

    /* LDO2 = 3.0V (LCD logic), LDO3 = 2.8V (backlight) */
    axp192_write(0x28, 0xCC);
    /* Enable LDO2 + LDO3 + DC-DC1 */
    axp192_write(0x12, 0x4D);
    ESP_LOGI(TAG, "AXP192 init done — backlight ON");
}

static void lcd_cmd(uint8_t cmd)
{
    gpio_set_level(LCD_DC, 0);
    spi_transaction_t t = { .length = 8, .tx_buffer = &cmd };
    spi_device_polling_transmit(s_lcd_spi, &t);
}

static void lcd_data(const uint8_t *data, size_t len)
{
    if (!len) return;
    gpio_set_level(LCD_DC, 1);
    spi_transaction_t t = { .length = len * 8, .tx_buffer = data };
    spi_device_polling_transmit(s_lcd_spi, &t);
}

static void lcd_data_byte(uint8_t b) { lcd_data(&b, 1); }

static void lcd_init(void)
{
    /* Power on LCD via AXP192 PMIC (backlight + logic supply) */
    axp192_init();

    gpio_config_t io = {
        .pin_bit_mask = (1ULL<<LCD_DC)|(1ULL<<LCD_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);   /* <-- this was missing, DC and RST were never set as outputs */

    spi_bus_config_t buscfg = {
        .mosi_io_num   = LCD_MOSI,
        .miso_io_num   = -1,
        .sclk_io_num   = LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * LCD_H * 2 + 8,
    };
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 20 * 1000 * 1000,
        .mode           = 0,
        .spics_io_num   = LCD_CS,
        .queue_size     = 7,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &s_lcd_spi));

    gpio_set_level(LCD_RST, 0); vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(LCD_RST, 1); vTaskDelay(pdMS_TO_TICKS(120));

    lcd_cmd(0x11); vTaskDelay(pdMS_TO_TICKS(120));
    lcd_cmd(0x3A); lcd_data_byte(0x55);  /* 16-bit colour */
    lcd_cmd(0x36); lcd_data_byte(0x00);  /* MADCTL: portrait, no rotation */
    lcd_cmd(0xB2);
    lcd_data_byte(0x0C); lcd_data_byte(0x0C);
    lcd_data_byte(0x00); lcd_data_byte(0x33); lcd_data_byte(0x33);
    lcd_cmd(0xB7); lcd_data_byte(0x35);
    lcd_cmd(0xBB); lcd_data_byte(0x19);
    lcd_cmd(0xC0); lcd_data_byte(0x2C);
    lcd_cmd(0xC2); lcd_data_byte(0x01);
    lcd_cmd(0xC3); lcd_data_byte(0x12);
    lcd_cmd(0xC4); lcd_data_byte(0x20);
    lcd_cmd(0xC6); lcd_data_byte(0x0F);
    lcd_cmd(0xD0); lcd_data_byte(0xA4); lcd_data_byte(0xA1);
    lcd_cmd(0x29);  /* Display ON */
}

static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t d[4];
    x0 += LCD_X_OFF; x1 += LCD_X_OFF;
    y0 += LCD_Y_OFF; y1 += LCD_Y_OFF;
    lcd_cmd(0x2A);
    d[0]=x0>>8; d[1]=x0&0xFF; d[2]=x1>>8; d[3]=x1&0xFF; lcd_data(d,4);
    lcd_cmd(0x2B);
    d[0]=y0>>8; d[1]=y0&0xFF; d[2]=y1>>8; d[3]=y1&0xFF; lcd_data(d,4);
    lcd_cmd(0x2C);
}

/* Row buffer — reused for all fills, avoids per-pixel SPI overhead */
static uint8_t s_row_buf[LCD_W * 2];

/* Fill a rectangle with a colour (RGB565) — sends one full row per transaction */
static void lcd_fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t colour)
{
    if (w == 0 || h == 0) return;
    lcd_set_window(x, y, x+w-1, y+h-1);
    gpio_set_level(LCD_DC, 1);
    uint8_t hi = colour >> 8, lo = colour & 0xFF;
    /* Fill the row buffer once */
    for (int i = 0; i < w; i++) {
        s_row_buf[i*2]   = hi;
        s_row_buf[i*2+1] = lo;
    }
    spi_transaction_t t = {
        .length    = w * 16,
        .tx_buffer = s_row_buf,
    };
    /* Send the same row h times */
    for (int row = 0; row < h; row++) {
        spi_device_polling_transmit(s_lcd_spi, &t);
    }
}

/* ---- Minimal 5x7 font ---------------------------------------- */
static const uint8_t FONT5x7[95][5] = {
 {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00}, {0x00,0x07,0x00,0x07,0x00},
 {0x14,0x7F,0x14,0x7F,0x14}, {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62},
 {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00}, {0x00,0x1C,0x22,0x41,0x00},
 {0x00,0x41,0x22,0x1C,0x00}, {0x0A,0x04,0x1F,0x04,0x0A}, {0x08,0x08,0x3E,0x08,0x08},
 {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x60,0x60,0x00,0x00},
 {0x20,0x10,0x08,0x04,0x02}, {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
 {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31}, {0x18,0x14,0x12,0x7F,0x10},
 {0x27,0x45,0x45,0x45,0x39}, {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
 {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}, {0x00,0x36,0x36,0x00,0x00},
 {0x00,0x56,0x36,0x00,0x00}, {0x00,0x08,0x14,0x22,0x41}, {0x14,0x14,0x14,0x14,0x14},
 {0x41,0x22,0x14,0x08,0x00}, {0x02,0x01,0x51,0x09,0x06}, {0x32,0x49,0x79,0x41,0x3E},
 {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
 {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01},
 {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00},
 {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
 {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
 {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
 {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F},
 {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F}, {0x63,0x14,0x08,0x14,0x63},
 {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43}, {0x00,0x7F,0x41,0x41,0x00},
 {0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x7F,0x00}, {0x04,0x02,0x01,0x02,0x04},
 {0x40,0x40,0x40,0x40,0x40}, {0x00,0x01,0x02,0x04,0x00}, {0x20,0x54,0x54,0x54,0x78},
 {0x7F,0x48,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20}, {0x38,0x44,0x44,0x48,0x7F},
 {0x38,0x54,0x54,0x54,0x18}, {0x08,0x7E,0x09,0x01,0x02}, {0x0C,0x52,0x52,0x52,0x3E},
 {0x7F,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7D,0x40,0x00}, {0x20,0x40,0x44,0x3D,0x00},
 {0x7F,0x10,0x28,0x44,0x00}, {0x00,0x41,0x7F,0x40,0x00}, {0x7C,0x04,0x18,0x04,0x78},
 {0x7C,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38}, {0x7C,0x14,0x14,0x14,0x08},
 {0x08,0x14,0x14,0x18,0x7C}, {0x7C,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x20},
 {0x04,0x3F,0x44,0x40,0x20}, {0x3C,0x40,0x40,0x40,0x7C}, {0x1C,0x20,0x40,0x20,0x1C},
 {0x3C,0x40,0x30,0x40,0x3C}, {0x44,0x28,0x10,0x28,0x44}, {0x0C,0x50,0x50,0x50,0x3C},
 {0x44,0x64,0x54,0x4C,0x44}, {0x00,0x08,0x36,0x41,0x00}, {0x00,0x00,0x7F,0x00,0x00},
 {0x00,0x41,0x36,0x08,0x00}, {0x0C,0x02,0x0C,0x10,0x0C},
};

/* Draw a single character, scale=1(5x7) scale=2(10x14) scale=3(15x21) */
static void lcd_char(uint16_t x, uint16_t y, char c,
                     uint16_t fg, uint16_t bg, uint8_t scale)
{
    if (c < 32 || c > 126) c = '?';
    const uint8_t *g = FONT5x7[(uint8_t)(c - 32)];
    for (int col = 0; col < 5; col++) {
        for (int row = 0; row < 7; row++) {
            uint16_t colour = (g[col] >> row) & 1 ? fg : bg;
            lcd_fill(x + col*scale, y + row*scale, scale, scale, colour);
        }
    }
    lcd_fill(x + 5*scale, y, scale, 7*scale, bg);
}

static void lcd_str(uint16_t x, uint16_t y, const char *s,
                    uint16_t fg, uint16_t bg, uint8_t scale)
{
    while (*s && x + 6*scale <= LCD_W) {
        lcd_char(x, y, *s++, fg, bg, scale);
        x += 6 * scale;
    }
}

/* Native M5StickC Plus RGB565 colour values — use these directly,
 * do NOT use the RGB() macro which produces incorrect values for
 * this display's bit packing. */
#define COL_BLACK   0x0000
#define COL_WHITE   0xFFFF
#define COL_GREEN   0x07E0
#define COL_YELLOW  0xFFE0
#define COL_RED     0xF800
#define COL_CYAN    0x07FF
#define COL_BLUE    0x001F
#define COL_GRAY    0x7BEF
#define COL_ORANGE  0xFDA0
#define COL_DKGREEN 0x0300  /* dark green — CONNECTED background */
#define COL_DKRED   0x4000  /* dark red   — SEARCHING background  */
#define COL_MAGENTA 0xF81F  /* magenta    — BtnA notification     */

/* ---- Message type prefixes ---- */
#define BTN_MSG_PREFIX  "BTN_MSG:"   /* BtnA physical button press */
#define TXT_MSG_PREFIX  "TXT_MSG:"   /* Python script text message */
#define PING_MSG_PREFIX "PING_MSG:"  /* RTT ping forwarded to root */
#define PONG_MSG_PREFIX "PONG_MSG:"  /* RTT pong replied by root   */

/* ================================================================
 *  CONFIG  (menuconfig → "Mesh Configuration")
 * ================================================================ */

#ifdef CONFIG_MESH_IS_ROOT
#define MESH_IS_ROOT 1
#else
#define MESH_IS_ROOT 0
#endif
#define MESH_CHANNEL          CONFIG_MESH_CHANNEL
#define MESH_MAX_LAYER        CONFIG_MESH_MAX_LAYER
#define MESH_AP_MAX_CONN      CONFIG_MESH_AP_MAX_CONN
#define LED_GPIO              CONFIG_LED_GPIO

static const uint8_t MESH_ID[6] = {
    CONFIG_MESH_ID[0], CONFIG_MESH_ID[1], CONFIG_MESH_ID[2],
    CONFIG_MESH_ID[3], CONFIG_MESH_ID[4], CONFIG_MESH_ID[5]
};

/* ================================================================
 *  GLOBAL STATE
 * ================================================================ */

static volatile bool      s_mesh_connected = false;
static EventGroupHandle_t s_mesh_eg;
#define MESH_CONNECTED_BIT  BIT0

/* ================================================================
 *  NOTIFICATION QUEUE
 *  Any task calls show_notification() — display_task dequeues one
 *  entry per 2 s refresh cycle, showing each for exactly 2 s.
 *  Multiple simultaneous arrivals are queued and displayed in order.
 * ================================================================ */
#define NOTIF_Y         220   /* bottom strip — below all existing rows */
#define NOTIF_MAX_LEN    24
#define NOTIF_QUEUE_LEN   8   /* max pending notifications            */

typedef struct {
    char     text[NOTIF_MAX_LEN];
    uint16_t colour;
} notif_entry_t;

static notif_entry_t s_notif_queue[NOTIF_QUEUE_LEN];
static int           s_notif_head  = 0;   /* next entry to display */
static int           s_notif_tail  = 0;   /* next slot to write    */
static int           s_notif_count = 0;   /* entries pending       */
static portMUX_TYPE  s_notif_mux   = portMUX_INITIALIZER_UNLOCKED;

/* Enqueue a notification — if queue is full the oldest entry is
 * kept and the new one is silently dropped.
 * duration_ms is unused (display_task drives timing via its 2 s loop). */
static void show_notification(const char *text, uint16_t colour, uint32_t duration_ms)
{
    (void)duration_ms;
    portENTER_CRITICAL(&s_notif_mux);
    if (s_notif_count < NOTIF_QUEUE_LEN) {
        strlcpy(s_notif_queue[s_notif_tail].text, text, NOTIF_MAX_LEN);
        s_notif_queue[s_notif_tail].colour = colour;
        s_notif_tail  = (s_notif_tail + 1) % NOTIF_QUEUE_LEN;
        s_notif_count++;
    }
    portEXIT_CRITICAL(&s_notif_mux);
}

/* ================================================================
 *  NODE LIST OVERLAY  (root BtnA — shows all node IDs for 5 s)
 * ================================================================ */
#define NODE_OVERLAY_MAX  12   /* max nodes renderable on 240px display */

typedef struct {
    bool    active;
    uint32_t expire_ms;
    uint8_t  macs[NODE_OVERLAY_MAX][6];
    int      count;
} node_overlay_t;

static node_overlay_t s_node_overlay = {0};
static portMUX_TYPE   s_overlay_mux  = portMUX_INITIALIZER_UNLOCKED;

/* ================================================================
 *  LED
 * ================================================================ */

static void led_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(LED_GPIO, 0);
}

static void led_task(void *arg)
{
    (void)arg;
    while (1) {
        if (s_mesh_connected) {
            gpio_set_level(LED_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(2000)); /* connected — LED stays on, check every 2s */
        } else {
            gpio_set_level(LED_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(500));
            gpio_set_level(LED_GPIO, 0);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

/* ================================================================
 *  LCD DISPLAY TASK
 *  Refreshes every 2 s — shows mesh status on the screen
 * ================================================================ */

/* Draw all static elements (title bar, row labels, MAC, role).
 * Called once at boot and again whenever the node-list overlay clears. */
static void lcd_draw_static_layout(void)
{
    uint8_t mac[6];
    char    buf[32];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    lcd_fill(0, 0, LCD_W, 18, COL_BLUE);
    lcd_str(4, 4, "ESP-MESH NODE", COL_WHITE, COL_BLUE, 1);

    lcd_str(4, 30,  "MAC:",    COL_GRAY, COL_BLACK, 1);
    lcd_str(4, 60,  "ROLE:",   COL_GRAY, COL_BLACK, 1);
    lcd_str(4, 90,  "LAYER:",  COL_GRAY, COL_BLACK, 1);
    lcd_str(4, 120, "CHLD:",   COL_GRAY, COL_BLACK, 1);
    lcd_str(4, 150, "STATUS:", COL_GRAY, COL_BLACK, 1);
#if !MESH_IS_ROOT
    lcd_str(4, 196, "RSSI:",   COL_GRAY, COL_BLACK, 1);
#endif

    snprintf(buf, sizeof(buf), "%02X:%02X:%02X", mac[0], mac[1], mac[2]);
    lcd_str(4, 44, buf, COL_CYAN, COL_BLACK, 1);
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X", mac[3], mac[4], mac[5]);
    lcd_str(4, 54, buf, COL_CYAN, COL_BLACK, 1);

    lcd_fill(4, 72, 90, 12, COL_BLACK);
    lcd_str(4, 72, MESH_IS_ROOT ? "ROOT" : "NODE",
            MESH_IS_ROOT ? COL_YELLOW : COL_WHITE, COL_BLACK, 2);
}

static void display_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "display_task started");
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGI(TAG, "Clearing screen...");
    lcd_fill(0, 0, LCD_W, LCD_H, COL_BLACK);
    ESP_LOGI(TAG, "Screen cleared");
    lcd_draw_static_layout();

    char buf[32];

    while (1) {
        /* ---- Node list overlay (root BtnA) ---- */
#if MESH_IS_ROOT
        {
            portENTER_CRITICAL(&s_overlay_mux);
            node_overlay_t snap = s_node_overlay;
            portEXIT_CRITICAL(&s_overlay_mux);
            uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

            if (snap.active && now_ms < snap.expire_ms) {
                /* Draw the overlay window */
                lcd_fill(0, 19, LCD_W, LCD_H - 19, COL_BLACK);
                lcd_fill(0, 19, LCD_W, 16, COL_BLUE);
                lcd_str(4, 22, "NODES IN MESH", COL_WHITE, COL_BLUE, 1);
                if (snap.count == 0) {
                    lcd_str(4, 44, "No nodes", COL_GRAY, COL_BLACK, 1);
                } else {
                    for (int i = 0; i < snap.count; i++) {
                        snprintf(buf, sizeof(buf), "%02X%02X",
                                 snap.macs[i][4], snap.macs[i][5]);
                        int y = 40 + i * 18;
                        if (y + 14 > NOTIF_Y) break;  /* stop before notif strip */
                        lcd_str(4, y, buf, COL_CYAN, COL_BLACK, 2);
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;   /* skip normal drawing while overlay is live */
            } else if (snap.active) {
                /* Overlay just expired — clear it and restore normal layout */
                portENTER_CRITICAL(&s_overlay_mux);
                s_node_overlay.active = false;
                portEXIT_CRITICAL(&s_overlay_mux);
                lcd_fill(0, 0, LCD_W, LCD_H, COL_BLACK);
                lcd_draw_static_layout();
            }
        }
#endif
        int   layer  = esp_mesh_get_layer();
        int   nodes  = esp_mesh_get_routing_table_size();

        /* Read volatile flag at the last possible moment — avoids a stale
         * snapshot taken at the top of the loop being used 2 s later. */
#if MESH_IS_ROOT
        /* Root is always connected once mesh has started — use the API
         * directly as the ground truth rather than the shared flag. */
        bool  conn   = esp_mesh_is_root();
#else
        bool  conn   = s_mesh_connected;
#endif

        /* LAYER */
        lcd_fill(4, 104, 80, 14, COL_BLACK);
        snprintf(buf, sizeof(buf), "%d", layer);
        uint16_t layer_col = (layer == 1) ? COL_YELLOW :
                             (layer == 2) ? COL_GREEN   : COL_ORANGE;
        lcd_str(4, 104, buf, layer_col, COL_BLACK, 2);

        /* NODES / CHLD
         * Root:     full routing table includes itself → subtract 1 for "other nodes"
         * Non-root: subtree count includes itself     → subtract 1 for "children only" */
        lcd_fill(4, 134, 80, 14, COL_BLACK);
        int display_nodes = (nodes > 0) ? nodes - 1 : 0;
        snprintf(buf, sizeof(buf), "%d", display_nodes);
        lcd_str(4, 134, buf, COL_WHITE, COL_BLACK, 2);

        /* STATUS */
        lcd_fill(0, 164, LCD_W, 20, COL_BLACK);
        if (conn) {
            lcd_fill(0, 164, LCD_W, 20, COL_DKGREEN);
#if MESH_IS_ROOT
            lcd_str(4, 168, "ONLINE", COL_GREEN, COL_DKGREEN, 2);
#else
            lcd_str(4, 168, "CONNECTED", COL_GREEN, COL_DKGREEN, 2);
#endif
        } else {
            lcd_fill(0, 164, LCD_W, 20, COL_DKRED);
#if MESH_IS_ROOT
            lcd_str(4, 168, "OFFLINE", COL_RED, COL_DKRED, 2);
#else
            lcd_str(4, 168, "SEARCHING..", COL_RED, COL_DKRED, 1);
#endif
        }

#if !MESH_IS_ROOT
        /* RSSI — signal strength to current parent
         * Green: > -60 dBm (strong)
         * Yellow: -60 to -75 dBm (moderate)
         * Red:  < -75 dBm (weak) */
        {
            int rssi = 0;
            lcd_fill(40, 208, 90, 14, COL_BLACK);
            if (conn && esp_wifi_sta_get_rssi(&rssi) == ESP_OK) {
                /* Use M5StickC Plus native RGB565 values directly —
                 * avoids byte-order artifacts from the RGB() macro at scale=1
                 * Green  0x07E0 = strong  (> -60 dBm)
                 * Yellow 0xFFE0 = moderate (-60 to -75 dBm)
                 * Red    0xF800 = weak    (< -75 dBm)          */
                uint16_t rssi_col = (rssi > -60) ? 0x07E0 :
                                    (rssi > -75) ? 0xFFE0 : 0xF800;
                snprintf(buf, sizeof(buf), "%d dBm", rssi);
                lcd_str(40, 208, buf, rssi_col, COL_BLACK, 1);
            } else {
                lcd_str(40, 208, "N/A", COL_GRAY, COL_BLACK, 1);
            }
        }
#endif

        /* ---- Notification queue — one entry per 2 s loop cycle ---- */
        {
            portENTER_CRITICAL(&s_notif_mux);
            bool has_notif = (s_notif_count > 0);
            notif_entry_t cur = {0};
            if (has_notif) {
                cur           = s_notif_queue[s_notif_head];
                s_notif_head  = (s_notif_head + 1) % NOTIF_QUEUE_LEN;
                s_notif_count--;
            }
            portEXIT_CRITICAL(&s_notif_mux);

            if (has_notif) {
                lcd_fill(0, NOTIF_Y, LCD_W, 20, cur.colour);
                lcd_str(4, NOTIF_Y + 2, cur.text, COL_WHITE, cur.colour, 1);
            } else {
                lcd_fill(0, NOTIF_Y, LCD_W, 20, COL_BLACK);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/* ================================================================
 *  MESH EVENT HANDLER
 * ================================================================ */

static void mesh_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    switch (event_id) {

    case MESH_EVENT_STARTED:
        ESP_LOGI(TAG, "MESH_EVENT_STARTED");
        s_mesh_connected = false;
        if (MESH_IS_ROOT) {
            ESP_ERROR_CHECK(esp_mesh_set_self_organized(false, false));
            ESP_ERROR_CHECK(esp_mesh_fix_root(true));
            ESP_ERROR_CHECK(esp_mesh_set_type(MESH_ROOT));
            s_mesh_connected = true;
            xEventGroupSetBits(s_mesh_eg, MESH_CONNECTED_BIT);
            ESP_LOGW(TAG, "*** THIS NODE IS ROOT (layer 1) ***");
        } else {
            ESP_ERROR_CHECK(esp_mesh_set_self_organized(true, false));
        }
        break;

    case MESH_EVENT_STOPPED:
        s_mesh_connected = false;
        xEventGroupClearBits(s_mesh_eg, MESH_CONNECTED_BIT);
        break;

    case MESH_EVENT_PARENT_CONNECTED: {
        mesh_event_connected_t *e = (mesh_event_connected_t *)event_data;
        ESP_LOGI(TAG, "PARENT_CONNECTED — layer %d, parent: " MACSTR,
                 esp_mesh_get_layer(), MAC2STR(e->connected.bssid));
        s_mesh_connected = true;
        xEventGroupSetBits(s_mesh_eg, MESH_CONNECTED_BIT);
        break;
    }

    case MESH_EVENT_PARENT_DISCONNECTED: {
        if (esp_mesh_is_root()) break;
        mesh_event_disconnected_t *e = (mesh_event_disconnected_t *)event_data;
        ESP_LOGW(TAG, "PARENT_DISCONNECTED reason=%d", e->reason);
        s_mesh_connected = false;
        xEventGroupClearBits(s_mesh_eg, MESH_CONNECTED_BIT);
        break;
    }

    case MESH_EVENT_NO_PARENT_FOUND: {
        mesh_event_no_parent_found_t *e = (mesh_event_no_parent_found_t *)event_data;
        ESP_LOGW(TAG, "NO_PARENT_FOUND scan_times=%d", e->scan_times);
        break;
    }

    case MESH_EVENT_LAYER_CHANGE: {
        mesh_event_layer_change_t *e = (mesh_event_layer_change_t *)event_data;
        ESP_LOGI(TAG, "LAYER_CHANGE → layer %d", e->new_layer);
        break;
    }

    case MESH_EVENT_CHILD_CONNECTED: {
        mesh_event_child_connected_t *e = (mesh_event_child_connected_t *)event_data;
        ESP_LOGI(TAG, "CHILD_CONNECTED aid=%d mac=" MACSTR, e->aid, MAC2STR(e->mac));
        break;
    }

    case MESH_EVENT_CHILD_DISCONNECTED: {
        mesh_event_child_disconnected_t *e = (mesh_event_child_disconnected_t *)event_data;
        ESP_LOGI(TAG, "CHILD_DISCONNECTED aid=%d mac=" MACSTR, e->aid, MAC2STR(e->mac));
        break;
    }

    case MESH_EVENT_ROUTING_TABLE_ADD: {
        mesh_event_routing_table_change_t *e = (mesh_event_routing_table_change_t *)event_data;
        ESP_LOGI(TAG, "ROUTING_TABLE_ADD +%d total=%d", e->rt_size_change, e->rt_size_new);
        break;
    }

    case MESH_EVENT_ROUTING_TABLE_REMOVE: {
        mesh_event_routing_table_change_t *e = (mesh_event_routing_table_change_t *)event_data;
        ESP_LOGI(TAG, "ROUTING_TABLE_REMOVE -%d total=%d", e->rt_size_change, e->rt_size_new);
        break;
    }

    case MESH_EVENT_ROOT_ADDRESS: {
        mesh_event_root_address_t *e = (mesh_event_root_address_t *)event_data;
        ESP_LOGI(TAG, "ROOT_ADDRESS: " MACSTR, MAC2STR(e->addr));
        break;
    }

    default:
        break;
    }
}

/* ================================================================
 *  WIFI INIT
 * ================================================================ */

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

/* ================================================================
 *  MESH INIT
 * ================================================================ */

static void mesh_init(void)
{
    ESP_ERROR_CHECK(esp_mesh_init());
    ESP_ERROR_CHECK(esp_event_handler_register(
        MESH_EVENT, ESP_EVENT_ANY_ID, mesh_event_handler, NULL));

    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();
    memcpy((uint8_t *)&cfg.mesh_id, MESH_ID, 6);
    cfg.channel = MESH_CHANNEL;

    const char *dummy_ssid = "MESH_NO_ROUTER";
    memcpy(cfg.router.ssid, dummy_ssid, strlen(dummy_ssid));
    cfg.router.ssid_len = strlen(dummy_ssid);
    cfg.router.allow_router_switch = false;

    cfg.mesh_ap.max_connection         = MESH_IS_ROOT ? 10 : MESH_AP_MAX_CONN;
    cfg.mesh_ap.nonmesh_max_connection = 0;
    memset(cfg.mesh_ap.password, 0, sizeof(cfg.mesh_ap.password));

    ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));
    ESP_ERROR_CHECK(esp_mesh_set_topology(MESH_TOPO_TREE));
    ESP_ERROR_CHECK(esp_mesh_set_max_layer(MESH_MAX_LAYER));
    ESP_ERROR_CHECK(esp_mesh_set_ap_connections(MESH_AP_MAX_CONN));
    ESP_ERROR_CHECK(esp_mesh_set_ap_authmode(WIFI_AUTH_OPEN));
    ESP_ERROR_CHECK(esp_mesh_start());

    ESP_LOGI(TAG, "Mesh started channel=%d max_layer=3 is_root=%s",
             MESH_CHANNEL, MESH_IS_ROOT ? "YES" : "NO");
}

/* ================================================================
 *  MESH SEND / RECEIVE
 * ================================================================ */

/* Send a string to a specific node by MAC address */
static void mesh_send_to_addr(const mesh_addr_t *dest, const char *msg)
{
    mesh_data_t data;
    data.data  = (uint8_t *)msg;
    data.size  = strlen(msg) + 1;
    data.proto = MESH_PROTO_BIN;
    data.tos   = MESH_TOS_P2P;
    esp_err_t err = esp_mesh_send(dest, &data, MESH_DATA_P2P, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Send to node failed: %s", esp_err_to_name(err));
    }
}

/* Send a string to the root node from any non-root node.
 * Returns true on success, false if disconnected or send failed. */
static bool mesh_send_to_root(const char *msg)
{
    /* If disconnected, skip immediately — do not wait.
     * The RETRY round owns recovery: root will list this chunk as missing
     * in IMG_ACK:RETRY and the node resends it once reconnected.
     * Waiting here adds up to 5s per chunk with no benefit since the
     * chunk ends up in the RETRY list regardless. */
    if (!s_mesh_connected) {
        ESP_LOGW(TAG, "Mesh disconnected — chunk deferred to RETRY round");
        return false;
    }

    mesh_data_t data;
    data.data  = (uint8_t *)msg;
    data.size  = strlen(msg) + 1;
    data.proto = MESH_PROTO_BIN;
    data.tos   = MESH_TOS_P2P;

    /* Passing NULL as destination = always send to root */
    esp_err_t err = esp_mesh_send(NULL, &data, MESH_DATA_P2P, NULL, 0);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Sent to root: %s", msg);
        vTaskDelay(pdMS_TO_TICKS(500)); /* rate limit — prevents WND-RX timeout */
        return true;
    } else {
        ESP_LOGW(TAG, "Send failed: %s", esp_err_to_name(err));
        return false;
    }
}

/* ----------------------------------------------------------------
 *  Simple CRC32 for chunk verification (IEEE 802.3 polynomial)
 * ---------------------------------------------------------------- */
static uint32_t crc32_calc(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
    return ~crc;
}

/* ----------------------------------------------------------------
 *  Base64 decode table
 * ---------------------------------------------------------------- */
static const int8_t B64_TAB[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1, 0,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
};

static int b64_decode(const char *src, uint8_t *dst, int max_len)
{
    int out = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (; *src && *src != '='; src++) {
        int v = B64_TAB[(uint8_t)*src];
        if (v < 0) continue;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out < max_len) dst[out++] = (acc >> bits) & 0xFF;
        }
    }
    return out;
}

/* ROOT: receives messages and verifies image chunk delivery */
#define MAX_CHUNKS    64    /* supports up to 64 * 800 = 51200 byte images */
#define ROOT_IMG_BUF  (MAX_CHUNKS * 800)

/* Reassembly buffer — static to avoid stack overflow */
static uint8_t  s_root_img_buf[ROOT_IMG_BUF];
static int      s_root_img_total  = 0;
static char     s_root_img_fname[64];

static void mesh_recv_task(void *arg)
{
    (void)arg;
    static uint8_t  buf[1500];
    static uint8_t  chunk_buf[1100]; /* decoded chunk buffer */
    static bool     received[MAX_CHUNKS];
    mesh_addr_t     from;
    mesh_data_t     data;
    int             flag         = 0;
    int             img_chunks   = 0;
    int             img_received = 0;
    uint32_t        img_file_crc = 0;
    uint32_t        img_session  = 0;  /* session ID of the active transfer */
    bool            img_saved    = false; /* guard — only save to UART once per session */

    /* Exclusive send lock — only one node may transfer at a time */
    bool       lock_active       = false;
    uint8_t    lock_mac[6]       = {0};
    TickType_t lock_grant_tick   = 0;
    uint32_t   lock_duration_ms  = 240000; /* default fallback (layer 3 budget) */

    ESP_LOGI(TAG, "Root receive task started");

    while (1) {
        data.data = buf;
        data.size = sizeof(buf) - 1;
        esp_err_t err = esp_mesh_recv(&from, &data, portMAX_DELAY, &flag, NULL, 0);
        if (err != ESP_OK) continue;
        buf[data.size] = '\0';
        const char *msg = (const char *)buf;

        /* ---- IMG_REQ:<session_id>:<lock_duration_ms> — node requests exclusive send lock ---- */
        if (strncmp(msg, "IMG_REQ:", 8) == 0) {
            /* Auto-expire stale lock if previous holder vanished */
            if (lock_active &&
                (xTaskGetTickCount() - lock_grant_tick) > pdMS_TO_TICKS(lock_duration_ms)) {
                ESP_LOGW(TAG, "Lock held by " MACSTR " expired — dumping %d/%d partial chunks "
                         "for session %"PRIu32, MAC2STR(lock_mac),
                         img_received, img_chunks, img_session);
                lock_active  = false;
                memset(received, 0, sizeof(received));
                img_chunks   = 0;
                img_received = 0;
                img_session  = 0;
                img_file_crc = 0;
                s_root_img_total    = 0;
                s_root_img_fname[0] = '\0';
            }
            uint32_t req_session = 0;
            uint32_t req_lock_ms = 0;
            sscanf(msg + 8, "%"SCNu32":%"SCNu32, &req_session, &req_lock_ms);
            char reply[32];
        if (!lock_active) {
            /* No current holder — grant normally */
            lock_active      = true;
            lock_grant_tick  = xTaskGetTickCount();
            lock_duration_ms = (req_lock_ms > 0) ? req_lock_ms : 240000;
            memcpy(lock_mac, from.addr, 6);
            snprintf(reply, sizeof(reply), "IMG_GRANT:%"PRIu32, req_session);
            ESP_LOGI(TAG, "IMG_GRANT → " MACSTR " session:%"PRIu32" lock:%"PRIu32"ms",
                    MAC2STR(from.addr), req_session, lock_duration_ms);
        } else if (memcmp(lock_mac, from.addr, 6) == 0) {
            /* SAME node requesting again — it restarted or IMG_RELEASE was lost.
            * Re-grant: reset the expiry timer and accept the new session. */
            lock_grant_tick  = xTaskGetTickCount();
            lock_duration_ms = (req_lock_ms > 0) ? req_lock_ms : 240000;
            /* Reset any partial reassembly state from the abandoned session */
            memset(received, 0, sizeof(received));
            img_chunks   = 0;
            img_received = 0;
            img_session  = 0;
            img_file_crc = 0;
            s_root_img_total    = 0;
            s_root_img_fname[0] = '\0';
            snprintf(reply, sizeof(reply), "IMG_GRANT:%"PRIu32, req_session);
            ESP_LOGW(TAG, "IMG_GRANT (re-grant — stale lock cleared) → " MACSTR
                    " new_session:%"PRIu32, MAC2STR(from.addr), req_session);
        } else {
            /* Different node — genuinely busy */
            snprintf(reply, sizeof(reply), "IMG_BUSY:%"PRIu32, req_session);
            ESP_LOGI(TAG, "IMG_BUSY  → " MACSTR " (lock held by " MACSTR ")",
                    MAC2STR(from.addr), MAC2STR(lock_mac));
        }
            mesh_send_to_addr(&from, reply);
            continue;
        }

        /* ---- IMG_RELEASE:<OK|FAIL>:<session_id> — node signals transfer complete ---- */
        if (strncmp(msg, "IMG_RELEASE:", 12) == 0) {
            if (lock_active && memcmp(lock_mac, from.addr, 6) == 0) {
                bool rel_ok = (strncmp(msg + 12, "OK:", 3) == 0);
                lock_active = false;
                if (!rel_ok) {
                    /* Transfer failed after all retries — dump any partial reassembly */
                    ESP_LOGW(TAG, "IMG_RELEASE:FAIL from " MACSTR
                             " — dumping %d/%d partial chunks for session %"PRIu32,
                             MAC2STR(from.addr), img_received, img_chunks, img_session);
                    memset(received, 0, sizeof(received));
                    img_chunks   = 0;
                    img_received = 0;
                    img_session  = 0;
                    img_file_crc = 0;
                    s_root_img_total = 0;
                    s_root_img_fname[0] = '\0';
                } else {
                    ESP_LOGI(TAG, "IMG_RELEASE:OK from " MACSTR " — lock cleared",
                             MAC2STR(from.addr));
                }
            }
            continue;
        }

        /* ---- IMG_START:<session_id>:<filename>:<total_bytes>:<num_chunks>:<file_crc> ---- */
        if (strncmp(msg, "IMG_START:", 10) == 0) {
            int  total_bytes = 0;
            char filename[64] = {0};
            uint32_t new_session = 0;
            sscanf(msg + 10, "%"SCNu32":%63[^:]:%d:%d:%"SCNu32,
                   &new_session, filename, &total_bytes, &img_chunks, &img_file_crc);
            img_session  = new_session;
            img_received = 0;
            img_saved    = false;
            if (img_chunks > MAX_CHUNKS) img_chunks = MAX_CHUNKS;
            memset(received, 0, sizeof(received));
            s_root_img_total = total_bytes;
            strncpy(s_root_img_fname, filename, sizeof(s_root_img_fname) - 1);
            ESP_LOGI(TAG, "IMG_START: session=%"PRIu32" node=" MACSTR " %s  %d bytes  %d chunks  CRC:%08"PRIx32,
                     img_session, MAC2STR(from.addr), filename, total_bytes, img_chunks, img_file_crc);

        /* ---- IMG_DATA:<session_id>:<idx>:<num_chunks>:<crc32>:<base64> ---- */
        } else if (strncmp(msg, "IMG_DATA:", 9) == 0) {
            int      idx       = 0;
            int      total     = 0;
            uint32_t exp_crc   = 0;
            uint32_t chunk_session = 0;
            /* Find the base64 payload (5th colon: session:idx:total:crc:b64) */
            const char *p = msg + 9;
            int colons = 0;
            const char *b64_start = NULL;
            for (const char *c = p; *c; c++) {
                if (*c == ':') {
                    colons++;
                    if (colons == 4) { b64_start = c + 1; break; }
                }
            }
            sscanf(p, "%"SCNu32":%d:%d:%"SCNu32, &chunk_session, &idx, &total, &exp_crc);

            /* Discard chunks from a stale/previous transfer session */
            if (chunk_session != img_session) {
                ESP_LOGW(TAG, "IMG_DATA: stale session %"PRIu32" (active: %"PRIu32") — discarded",
                         chunk_session, img_session);
                continue;
            }

            if (b64_start && idx < MAX_CHUNKS) {
                int decoded_len = b64_decode(b64_start, chunk_buf, sizeof(chunk_buf));
                uint32_t got_crc = crc32_calc(chunk_buf, decoded_len);

                if (got_crc == exp_crc) {
                    bool duplicate = received[idx];
                    if (!duplicate) {
                        received[idx] = true;
                        img_received++;
                    }
                    /* Always write decoded bytes — covers the case where
                     * first delivery had a silent buffer corruption */
                    int offset = idx * 800;
                    if (offset + decoded_len <= ROOT_IMG_BUF)
                        memcpy(s_root_img_buf + offset, chunk_buf, decoded_len);
                    int pct = img_chunks > 0 ? (img_received * 100 / img_chunks) : 0;
                    ESP_LOGI(TAG, "IMG chunk %02d/%d OK  CRC:%08"PRIx32"  (%d%%)%s",
                             idx + 1, img_chunks, got_crc, pct,
                             duplicate ? " [duplicate — ignored]" : "");
                } else {
                    ESP_LOGE(TAG, "IMG chunk %02d/%d CRC FAIL  got:%08"PRIx32" exp:%08"PRIx32,
                             idx + 1, img_chunks, got_crc, exp_crc);
                }
            }

        /* ---- IMG_END:<session_id>:<filename>:<num_chunks> ---- */
        } else if (strncmp(msg, "IMG_END:", 8) == 0) {
            uint32_t end_session = 0;
            sscanf(msg + 8, "%"SCNu32, &end_session);
            if (end_session != img_session) {
                ESP_LOGW(TAG, "IMG_END: stale session %"PRIu32" — discarded", end_session);
                continue;
            }
            /* Build ACK — list any missing/failed chunks */
            char ack[256];
            int missing = 0;
            int pos = 0;
            pos += snprintf(ack + pos, sizeof(ack) - pos,
                            "IMG_ACK:%08"PRIx32":", img_file_crc);
            for (int i = 0; i < img_chunks; i++) {
                if (!received[i]) {
                    if (missing == 0)
                        pos += snprintf(ack + pos, sizeof(ack) - pos, "RETRY:");
                    else
                        pos += snprintf(ack + pos, sizeof(ack) - pos, ",");
                    pos += snprintf(ack + pos, sizeof(ack) - pos, "%d", i + 1);
                    missing++;
                }
            }
            if (missing == 0) {
                snprintf(ack + pos, sizeof(ack) - pos, "OK");
                ESP_LOGI(TAG, "IMG_COMPLETE — all %d chunks OK — sending ACK",
                         img_chunks);
            } else {
                ESP_LOGW(TAG, "IMG_INCOMPLETE — %d missing — requesting retransmit",
                         missing);
            }
            /* Send ACK back to the node that sent the image */
            mesh_send_to_addr(&from, ack);
            ESP_LOGI(TAG, "ACK sent: %s", ack);

            if (missing == 0) {
                if (img_saved) {
                    /* ACK:OK already sent above — root already streamed this image
                     * once. The node timed out waiting for that ACK and sent IMG_END
                     * again (Layer 3 latency). Do not save a second copy. */
                    ESP_LOGW(TAG, "IMG_END duplicate — already saved session %"PRIu32
                             " — ACK resent, skipping save", img_session);
                } else {
                    img_saved = true;
                    show_notification("IMG RECVD", COL_GREEN, 3000);
                /* Stream reassembled image to PC over UART so it can be saved.
                 * Format:  IMG_SAVE:<filename>:<size>\n<raw bytes>
                 * The Python receive_image.py script waits for this header. */
                char hdr[96];
                int hdr_len = snprintf(hdr, sizeof(hdr),
                                       "IMG_SAVE:%s:%d\n",
                                       s_root_img_fname, s_root_img_total);
                uart_write_bytes(UART_NUM_0, hdr, hdr_len);
                /* Send in 512-byte chunks to avoid overwhelming the UART TX FIFO */
                const int UART_CHUNK = 512;
                int sent = 0;
                while (sent < s_root_img_total) {
                    int n = s_root_img_total - sent;
                    if (n > UART_CHUNK) n = UART_CHUNK;
                    uart_write_bytes(UART_NUM_0, (const char *)(s_root_img_buf + sent), n);
                    sent += n;
                    vTaskDelay(pdMS_TO_TICKS(5)); /* let TX FIFO drain */
                }
                ESP_LOGI(TAG, "IMG_SAVE: %d bytes sent to PC", s_root_img_total);
                img_chunks   = 0;
                img_received = 0;
                } /* end else (!img_saved) */
            }
            /* If retrying, keep received[] state so re-sent chunks fill gaps */

        /* ---- Plain text / button messages ---- */
        /* ---- PING_MSG:<seq> — RTT ping from a node, reply PONG_MSG immediately ---- */
        } else if (strncmp(msg, PING_MSG_PREFIX, strlen(PING_MSG_PREFIX)) == 0) {
            int seq_num = atoi(msg + strlen(PING_MSG_PREFIX));
            char pong[32];
            snprintf(pong, sizeof(pong), "PONG_MSG:%d", seq_num);
            mesh_send_to_addr(&from, pong);
            ESP_LOGI(TAG, "PING from " MACSTR " seq=%d → PONG sent",
                     MAC2STR(from.addr), seq_num);

        } else if (strncmp(msg, BTN_MSG_PREFIX, strlen(BTN_MSG_PREFIX)) == 0) {
            /* BtnA physical button press from a node.
             * Payload format: BTN_MSG:XXYY says hello.
             * Extract the first 4 chars after the prefix as the sender ID. */
            const char *payload = msg + strlen(BTN_MSG_PREFIX);
            char sender_id[5]   = {0};
            strncpy(sender_id, payload, 4);   /* grab "XXYY" */
            ESP_LOGI(TAG, "BTN_MSG from " MACSTR ": %s", MAC2STR(from.addr), payload);
            char notif_text[NOTIF_MAX_LEN];
            snprintf(notif_text, sizeof(notif_text), "Recvd from %s", sender_id);
            show_notification(notif_text, COL_MAGENTA, 0);
        } else if (strncmp(msg, TXT_MSG_PREFIX, strlen(TXT_MSG_PREFIX)) == 0) {
            /* Python script text message forwarded from a node */
            const char *payload = msg + strlen(TXT_MSG_PREFIX);
            ESP_LOGI(TAG, "TXT_MSG from " MACSTR ": %s", MAC2STR(from.addr), payload);
            show_notification("TXT RECVD", COL_GREEN, 3000);
        } else {
            ESP_LOGI(TAG, "MSG from " MACSTR ": %s", MAC2STR(from.addr), msg);
        }
    }
}

/* ================================================================
 *  NON-ROOT IMAGE TRANSFER
 *  uart_recv_task    -- buffers all chunks from PC UART into heap
 *  img_transfer_task -- sends to root, handles ACK/retry loop
 * ================================================================ */

/* ================================================================
 *  RAW IMAGE BUFFER  (shared between uart_recv_task and img_transfer_task)
 * ================================================================ */
#define IMG_CHUNK_SIZE    800
#define IMG_MAX_CHUNKS     64
#define IMG_MAX_BYTES    (IMG_CHUNK_SIZE * IMG_MAX_CHUNKS)  /* 51200 bytes */

static uint8_t  *s_img_buf       = NULL;   /* heap: raw image bytes */
static int       s_img_total     = 0;      /* total bytes received  */
static char      s_img_filename[64];       /* original filename     */
static SemaphoreHandle_t s_img_sem = NULL; /* signals img_transfer_task */

/* ----------------------------------------------------------------
 *  Base64 encode — needed here since chunking happens in firmware
 * ---------------------------------------------------------------- */
static const char B64_ENC[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_encode(const uint8_t *src, int src_len, char *dst, int dst_max)
{
    int out = 0;
    for (int i = 0; i < src_len; i += 3) {
        int rem  = src_len - i;
        uint32_t w = ((uint32_t)src[i] << 16)
                   | (rem > 1 ? (uint32_t)src[i+1] << 8 : 0)
                   | (rem > 2 ? (uint32_t)src[i+2]      : 0);
        if (out + 4 >= dst_max) break;
        dst[out++] = B64_ENC[(w >> 18) & 0x3F];
        dst[out++] = B64_ENC[(w >> 12) & 0x3F];
        dst[out++] = rem > 1 ? B64_ENC[(w >>  6) & 0x3F] : '=';
        dst[out++] = rem > 2 ? B64_ENC[(w      ) & 0x3F] : '=';
    }
    dst[out] = '\0';
    return out;
}

/* ----------------------------------------------------------------
 *  Build and send a single chunk line to root
 *  Format: IMG_DATA:<session_id>:<idx>:<num_chunks>:<crc32>:<base64>
 *  Returns true on success, false if mesh was disconnected.
 * ---------------------------------------------------------------- */
static bool send_chunk_to_root(uint32_t session_id, int idx, int num_chunks)
{
    const uint8_t *chunk_data = s_img_buf + idx * IMG_CHUNK_SIZE;
    int chunk_len = s_img_total - idx * IMG_CHUNK_SIZE;
    if (chunk_len > IMG_CHUNK_SIZE) chunk_len = IMG_CHUNK_SIZE;

    uint32_t crc = crc32_calc(chunk_data, chunk_len);

    /* b64 output: ceil(chunk_len/3)*4 + null */
    static char b64_buf[((IMG_CHUNK_SIZE + 2) / 3) * 4 + 4];
    b64_encode(chunk_data, chunk_len, b64_buf, sizeof(b64_buf));

    /* Line: IMG_DATA:<session_id>:<idx>:<total>:<crc>:<b64> */
    static char line[1200];
    snprintf(line, sizeof(line), "IMG_DATA:%"PRIu32":%d:%d:%"PRIu32":%s",
             session_id, idx, num_chunks, crc, b64_buf);
    return mesh_send_to_root(line);
}

/* Base ACK timeout tuned for Layer 3 (one relay hop to root).
 * Each additional layer adds one more base interval:
 *   Layer 3 → 1 × 30s = 30s
 *   Layer 4 → 2 × 30s = 60s
 *   Layer 5 → 3 × 30s = 90s
 * Formula:  ack_timeout_ms = BASE × max(1, layer - 2)             */
#define IMG_ACK_TIMEOUT_BASE_MS  30000
#define IMG_MAX_RETRIES          5

/* img_transfer_task: chunks raw buffer, sends to root, handles ACK/retry,
 * then writes IMG_ACK:OK or IMG_ACK:FAIL back to UART for the PC. */
static void img_transfer_task(void *arg)
{
    (void)arg;
    static uint8_t ack_buf[256];
    mesh_addr_t    from;
    mesh_data_t    data;
    int            flag = 0;
    bool           success = false;
    static uint32_t s_session_counter = 0;  /* increments every transfer */

    while (1) {
        xSemaphoreTake(s_img_sem, portMAX_DELAY);
        success = false;

        /* Unique session ID for this transfer — root uses this to discard
         * stale chunks from a previous disconnected attempt. */
        uint32_t session_id = ++s_session_counter;

        /* Scale ACK timeout with mesh depth.  Layer 3 is the baseline (×1).
         * Every additional layer multiplies by one more base interval.
         * Lock duration covers all retry attempts plus a 10s safety buffer
         * so the root never auto-expires a lock while the node is still
         * legitimately retrying.                                          */
        int layer     = esp_mesh_get_layer();
        int layer_mult = (layer > 2) ? (layer - 2) : 1;
        uint32_t ack_timeout_ms  = (uint32_t)IMG_ACK_TIMEOUT_BASE_MS * layer_mult;
        uint32_t lock_duration_ms = ack_timeout_ms * (IMG_MAX_RETRIES + 1) + 10000;
        ESP_LOGI(TAG, "Layer %d — ACK timeout %"PRIu32"ms × %d attempts — lock %"PRIu32"ms",
                 layer, ack_timeout_ms, IMG_MAX_RETRIES + 1, lock_duration_ms);

        int num_chunks = (s_img_total + IMG_CHUNK_SIZE - 1) / IMG_CHUNK_SIZE;
        uint32_t file_crc = crc32_calc(s_img_buf, s_img_total);

        ESP_LOGI(TAG, "Starting mesh transfer: %s  %d bytes  %d chunks  CRC:%08"PRIx32"  session:%"PRIu32,
                 s_img_filename, s_img_total, num_chunks, file_crc, session_id);

        /* --- Request exclusive lock from root before transmitting --- */
#define IMG_REQ_RETRY_MS   5000    /* pause between BUSY responses     */
#define IMG_REQ_TIMEOUT_MS 300000  /* give up after 5 minutes waiting  */
        {
            bool granted = false;
            char req[64];
            /* Include lock_duration_ms so root uses the correct expiry
             * for this node's layer depth.
             * Format: IMG_REQ:<session_id>:<lock_duration_ms>          */
            snprintf(req, sizeof(req), "IMG_REQ:%"PRIu32":%"PRIu32,
                     session_id, lock_duration_ms);
            TickType_t req_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(IMG_REQ_TIMEOUT_MS);

            while (!granted && xTaskGetTickCount() < req_deadline) {
                mesh_send_to_root(req);

                /* Wait up to 3 s for GRANT or BUSY reply */
                TickType_t resp_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
                while (xTaskGetTickCount() < resp_deadline) {
                    TickType_t remaining = resp_deadline - xTaskGetTickCount();
                    data.data = ack_buf;
                    data.size = sizeof(ack_buf) - 1;
                    if (esp_mesh_recv(&from, &data, remaining, &flag, NULL, 0) != ESP_OK)
                        continue;
                    ack_buf[data.size] = '\0';
                    const char *resp = (const char *)ack_buf;
                    if (strncmp(resp, "IMG_GRANT:", 10) == 0) { granted = true; break; }
                    if (strncmp(resp, "IMG_BUSY:",   9) == 0) { break; }
                }

                if (!granted) {
                    ESP_LOGI(TAG, "Mesh busy — retrying in %d s", IMG_REQ_RETRY_MS / 1000);
                    vTaskDelay(pdMS_TO_TICKS(IMG_REQ_RETRY_MS));
                }
            }

            if (!granted) {
                ESP_LOGE(TAG, "Failed to acquire mesh lock — aborting");
                uart_write_bytes(UART_NUM_0, "IMG_ACK:FAIL\n", 13);
                free(s_img_buf);
                s_img_buf   = NULL;
                s_img_total = 0;
                continue; /* back to xSemaphoreTake */
            }
            ESP_LOGI(TAG, "Mesh lock acquired — starting transfer session:%"PRIu32, session_id);
        }

        /* IMG_START — includes session_id so root can track which transfer this is */
        char start_line[128];
        snprintf(start_line, sizeof(start_line),
                 "IMG_START:%"PRIu32":%s:%d:%d:%"PRIu32,
                 session_id, s_img_filename, s_img_total, num_chunks, file_crc);
        mesh_send_to_root(start_line);

        /* Send all chunks — mesh_send_to_root waits for reconnection if disconnected */
        int sent_count = 0;
        for (int i = 0; i < num_chunks; i++) {
            if (send_chunk_to_root(session_id, i, num_chunks))
                sent_count++;
            else
                ESP_LOGE(TAG, "Chunk %d failed (disconnected) — will appear in RETRY list", i);
        }
        ESP_LOGI(TAG, "Initial send: %d/%d chunks delivered", sent_count, num_chunks);

        /* ACK / retry loop */
        char end_line[96];
        snprintf(end_line, sizeof(end_line),
                 "IMG_END:%"PRIu32":%s:%d", session_id, s_img_filename, num_chunks);

        for (int attempt = 0; attempt <= IMG_MAX_RETRIES; attempt++) {
            mesh_send_to_root(end_line);
            ESP_LOGI(TAG, "Waiting for ACK (attempt %d/%d)",
                     attempt + 1, IMG_MAX_RETRIES + 1);

            bool got_ack = false;
            TickType_t deadline = xTaskGetTickCount() +
                                  pdMS_TO_TICKS(ack_timeout_ms);

            while (xTaskGetTickCount() < deadline) {
                TickType_t remaining = deadline - xTaskGetTickCount();
                data.data = ack_buf;
                data.size = sizeof(ack_buf) - 1;
                if (esp_mesh_recv(&from, &data, remaining, &flag, NULL, 0) != ESP_OK)
                    continue;
                ack_buf[data.size] = '\0';
                const char *ack = (const char *)ack_buf;
                if (strncmp(ack, "IMG_ACK:", 8) != 0) continue;
                got_ack = true;

                /* Skip past <crc>: to reach status token
                 * Format: IMG_ACK:<crc_hex>:OK  or  IMG_ACK:<crc_hex>:RETRY:...
                 * There is exactly ONE colon between the prefix and the status. */
                const char *p = strchr(ack + 8, ':');
                if (!p) break;
                p++;   /* p now points directly at "OK" or "RETRY:..." */

                if (strncmp(p, "OK", 2) == 0) {
                    ESP_LOGI(TAG, "IMG_COMPLETE -- root confirmed all chunks OK");
                    success = true;
                    show_notification("IMG SENT", COL_GREEN, 3000);
                    goto transfer_done;
                } else if (strncmp(p, "RETRY:", 6) == 0) {
                    /* Parse missing chunk indices and resend only those */
                    char tmp[64];
                    strncpy(tmp, p + 6, sizeof(tmp) - 1);
                    tmp[sizeof(tmp) - 1] = '\0';
                    int mcount = 0;
                    char *tok = strtok(tmp, ",");
                    while (tok) {
                        int idx = atoi(tok) - 1;  /* root sends 1-based, convert to 0-based */
                        if (idx >= 0 && idx < num_chunks) {
                            if (send_chunk_to_root(session_id, idx, num_chunks))
                                mcount++;
                            else
                                ESP_LOGW(TAG, "Retry chunk %d skipped — disconnected", idx);
                        }
                        tok = strtok(NULL, ",");
                    }
                    ESP_LOGW(TAG, "Retransmitted %d missing chunks", mcount);
                    break; /* send IMG_END again */
                }
                break;
            }
            if (!got_ack)
                ESP_LOGE(TAG, "ACK timeout (attempt %d)", attempt + 1);
            if (attempt >= IMG_MAX_RETRIES)
                ESP_LOGE(TAG, "Max retries reached -- transfer failed");
        }

transfer_done:
        /* Release the mesh lock — carries OK/FAIL so root can dump partial state */
        {
            char rel[48];
            snprintf(rel, sizeof(rel), "IMG_RELEASE:%s:%"PRIu32,
                     success ? "OK" : "FAIL", session_id);
            mesh_send_to_root(rel);
            ESP_LOGI(TAG, "Mesh lock released — %s (session:%"PRIu32")",
                     success ? "OK" : "FAIL", session_id);
        }

        /* Write result back to UART so PC sender knows the outcome */
        if (success) {
            uart_write_bytes(UART_NUM_0, "IMG_ACK:OK\n", 11);
        } else {
            uart_write_bytes(UART_NUM_0, "IMG_ACK:FAIL\n", 13);
        }

        free(s_img_buf);
        s_img_buf   = NULL;
        s_img_total = 0;
    }
}

/* uart_recv_task: reads IMG_FILE header then raw bytes, buffers into heap,
 * signals img_transfer_task. Plain text lines are forwarded to root. */
#define UART_HDR_BUF 128

static void uart_recv_task(void *arg)
{
    (void)arg;
    uart_config_t uart_cfg = {
        .baud_rate  = 115200,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    /* UART driver RX buffer must be >= IMG_MAX_BYTES because send_image.py
     * writes the entire image in one ser.write() call — the full burst
     * arrives in ~1 second at 115200 baud and must be buffered before
     * uart_read_bytes drains it into the malloc buffer. Any smaller size
     * overflows and silently drops bytes. */
    uart_driver_install(UART_NUM_0, IMG_MAX_BYTES + 512, 0, 0, NULL, 0);
    uart_param_config(UART_NUM_0, &uart_cfg);

    static uint8_t hdr_buf[UART_HDR_BUF];
    int pos = 0;

    ESP_LOGI(TAG, "UART ready -- send IMG_FILE:<name>:<bytes> then raw data");

    while (1) {
        uint8_t c;
        if (uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(20)) <= 0) continue;

        if (c == '\n' || c == '\r') {
            if (pos == 0) continue;
            hdr_buf[pos] = '\0'; pos = 0;
            const char *line = (const char *)hdr_buf;

            xEventGroupWaitBits(s_mesh_eg, MESH_CONNECTED_BIT,
                                pdFALSE, pdTRUE, portMAX_DELAY);

            if (strncmp(line, "IMG_FILE:", 9) == 0) {
                /* Parse: IMG_FILE:<filename>:<total_bytes> */
                int total = 0;
                char fname[64] = {0};
                sscanf(line + 9, "%63[^:]:%d", fname, &total);

                if (total <= 0 || total > IMG_MAX_BYTES) {
                    ESP_LOGE(TAG, "Invalid image size: %d", total);
                    continue;
                }

                /* Allocate buffer and read raw bytes */
                uint8_t *img = malloc(total);
                if (!img) {
                    ESP_LOGE(TAG, "malloc failed for %d bytes", total);
                    uart_write_bytes(UART_NUM_0, "IMG_ACK:FAIL\n", 13);
                    continue;
                }

                ESP_LOGI(TAG, "Receiving %d raw bytes for %s...", total, fname);
                int received = 0;
                while (received < total) {
                    /* Read in 512-byte chunks — drains the 2048-byte driver
                     * buffer faster than the PC can fill it at 115200 baud */
                    int n = uart_read_bytes(UART_NUM_0,
                                           img + received,
                                           MIN(512, total - received),
                                           pdMS_TO_TICKS(5000));
                    if (n <= 0) {
                        ESP_LOGE(TAG, "UART read timeout after %d bytes", received);
                        break;
                    }
                    received += n;
                }

                if (received != total) {
                    ESP_LOGE(TAG, "Expected %d bytes, got %d -- aborting",
                             total, received);
                    free(img);
                    /* Notify PC immediately so it doesn't wait ACK_TIMEOUT */
                    uart_write_bytes(UART_NUM_0, "IMG_ACK:FAIL\n", 13);
                    continue;
                }

                ESP_LOGI(TAG, "All %d bytes received -- signalling transfer task",
                         total);
                strncpy(s_img_filename, fname, sizeof(s_img_filename) - 1);
                s_img_buf   = img;
                s_img_total = total;
                xSemaphoreGive(s_img_sem);

            } else if (strncmp(line, "PING:", 5) == 0) {
                /* RTT measurement — forward to root and echo PONG back to PC.
                 * Format in : PING:<seq>
                 * Format out: PONG:<seq>:<mesh_rtt_ms>   on success
                 *             PONG_TIMEOUT:<seq>          if root did not reply */
                int seq = atoi(line + 5);
                char ping_mesh[32];
                snprintf(ping_mesh, sizeof(ping_mesh), "PING_MSG:%d", seq);

                TickType_t t_send = xTaskGetTickCount();
                mesh_send_to_root(ping_mesh);

                /* Wait up to 8s for PONG_MSG reply from root */
                static uint8_t pong_buf[64];
                mesh_addr_t    pong_from;
                mesh_data_t    pong_data;
                int            pong_flag = 0;
                pong_data.data = pong_buf;
                pong_data.size = sizeof(pong_buf) - 1;

                TickType_t pong_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(8000);
                bool got_pong = false;
                while (xTaskGetTickCount() < pong_deadline) {
                    TickType_t remaining = pong_deadline - xTaskGetTickCount();
                    if (esp_mesh_recv(&pong_from, &pong_data, remaining,
                                      &pong_flag, NULL, 0) != ESP_OK) continue;
                    pong_buf[pong_data.size] = '\0';
                    char expected[32];
                    snprintf(expected, sizeof(expected), "PONG_MSG:%d", seq);
                    if (strncmp((char *)pong_buf, expected, strlen(expected)) == 0) {
                        uint32_t mesh_rtt_ms =
                            (xTaskGetTickCount() - t_send) * portTICK_PERIOD_MS;
                        char uart_reply[48];
                        int rlen = snprintf(uart_reply, sizeof(uart_reply),
                                            "PONG:%d:%"PRIu32"\n", seq, mesh_rtt_ms);
                        uart_write_bytes(UART_NUM_0, uart_reply, rlen);
                        ESP_LOGI(TAG, "RTT ping seq=%d mesh_rtt=%"PRIu32"ms",
                                 seq, mesh_rtt_ms);
                        got_pong = true;
                        break;
                    }
                }
                if (!got_pong) {
                    char timeout_reply[32];
                    int rlen = snprintf(timeout_reply, sizeof(timeout_reply),
                                        "PONG_TIMEOUT:%d\n", seq);
                    uart_write_bytes(UART_NUM_0, timeout_reply, rlen);
                    ESP_LOGW(TAG, "RTT ping seq=%d — no reply from root (timeout)", seq);
                }

            } else {
                /* Plain text message — prepend TXT prefix and forward to root */
                char txt_msg[256];
                snprintf(txt_msg, sizeof(txt_msg), "%s%s", TXT_MSG_PREFIX, line);
                if (mesh_send_to_root(txt_msg)) {
                    show_notification("TXT SENT", COL_GREEN, 3000);
                }
            }
        } else if (pos < UART_HDR_BUF - 1) {
            hdr_buf[pos++] = c;
        }
    }
}


#define BTN_A  37
#define BTN_B  39

static void button_task(void *arg)
{
    (void)arg;

    gpio_config_t io = {
        .pin_bit_mask = (1ULL<<BTN_A) | (1ULL<<BTN_B),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,  /* GPIO 37 & 39 are input-only pads —
                                                * no internal PU hardware exists.
                                                * M5StickC Plus has external pull-ups
                                                * on the PCB so none needed here. */
    };
    gpio_config(&io);

    int btn_a_last = 1, btn_b_last = 1;

    ESP_LOGI(TAG, "BtnA = send hello | BtnB = rejoin mesh");

    while (1) {
        if (!s_mesh_connected) {
            vTaskDelay(pdMS_TO_TICKS(100));
            btn_a_last = gpio_get_level(BTN_A);
            btn_b_last = gpio_get_level(BTN_B);
            continue;
        }

        int btn_a = gpio_get_level(BTN_A);
        int btn_b = gpio_get_level(BTN_B);

        /* BtnA falling edge:
         *   Root → snapshot routing table and show node-list overlay for 5 s
         *   Node → send "last4mac says hello." to root                       */
        if (btn_a == 0 && btn_a_last == 1) {
#if MESH_IS_ROOT
            /* Gather routing table — filter out root's own MAC */
            mesh_addr_t table[NODE_OVERLAY_MAX + 1];
            int table_size = 0;
            esp_mesh_get_routing_table(table,
                                       sizeof(table),
                                       &table_size);
            uint8_t my_mac[6];
            esp_read_mac(my_mac, ESP_MAC_WIFI_STA);

            portENTER_CRITICAL(&s_overlay_mux);
            s_node_overlay.count = 0;
            s_node_overlay.expire_ms =
                xTaskGetTickCount() * portTICK_PERIOD_MS + 5000;
            for (int i = 0; i < table_size &&
                            s_node_overlay.count < NODE_OVERLAY_MAX; i++) {
                if (memcmp(table[i].addr, my_mac, 6) != 0) {
                    memcpy(s_node_overlay.macs[s_node_overlay.count++],
                           table[i].addr, 6);
                }
            }
            s_node_overlay.active = true;
            portEXIT_CRITICAL(&s_overlay_mux);
            ESP_LOGI(TAG, "Node overlay: showing %d nodes", s_node_overlay.count);
#else
            char msg[64];
            uint8_t mac[6];
            esp_read_mac(mac, ESP_MAC_WIFI_STA);
            snprintf(msg, sizeof(msg), "%s%02X%02X says hello.",
                     BTN_MSG_PREFIX, mac[4], mac[5]);
            if (mesh_send_to_root(msg)) {
                show_notification("Message Sent", COL_MAGENTA, 0);
            }
#endif
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        /* BtnB falling edge:
         *   Root   → stop mesh entirely (clears routing table) then restart;
         *            all child nodes lose their parent and gradually reconnect,
         *            rebuilding the routing table from scratch.
         *   Node   → drop current parent and immediately scan for a new one. */
        if (btn_b == 0 && btn_b_last == 1) {
#if MESH_IS_ROOT
            /* Kick all directly connected children at the WiFi layer.
             * Root stays online — only the child links are torn down.
             * Each kicked child cascades a disconnect through its own subtree.
             * All nodes then re-scan and reconnect, rebuilding the routing table. */
            ESP_LOGW(TAG, "Button B (root) — deauthing all children to rebuild routing table...");
            wifi_sta_list_t sta_list = {0};
            if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK && sta_list.num > 0) {
                for (int i = 0; i < sta_list.num; i++) {
                    uint16_t aid = (uint16_t)(i + 1); /* softAP AIDs are 1-based sequential */
                    esp_wifi_deauth_sta(aid);
                    ESP_LOGI(TAG, "  deauthed child " MACSTR " (aid=%d)",
                             MAC2STR(sta_list.sta[i].mac), aid);
                }
                ESP_LOGI(TAG, "Kicked %d direct children — routing table will rebuild",
                         sta_list.num);
            } else {
                ESP_LOGW(TAG, "No children currently connected");
            }
#else
            ESP_LOGW(TAG, "Button B (node) — rejoining mesh...");
            s_mesh_connected = false;
            xEventGroupClearBits(s_mesh_eg, MESH_CONNECTED_BIT);
            esp_mesh_disconnect();            /* drop current parent                 */
            esp_mesh_connect();               /* scan and connect to strongest parent */
#endif
            vTaskDelay(pdMS_TO_TICKS(200));   /* debounce */
        }

        btn_a_last = btn_a;
        btn_b_last = btn_b;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ================================================================
 *  STATUS TASK  (serial log every 30s)
 * ================================================================ */

static void status_task(void *arg)
{
    (void)arg;
    while (1) {
        xEventGroupWaitBits(s_mesh_eg, MESH_CONNECTED_BIT,
                            pdFALSE, pdTRUE, portMAX_DELAY);
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        ESP_LOGI(TAG, "── Status ──────────────────────────");
        ESP_LOGI(TAG, "  MAC   : " MACSTR, MAC2STR(mac));
        ESP_LOGI(TAG, "  Layer : %d", esp_mesh_get_layer());
        ESP_LOGI(TAG, "  Root  : %s", esp_mesh_is_root() ? "YES" : "no");
        ESP_LOGI(TAG, "  Nodes : %d", esp_mesh_get_routing_table_size());
        ESP_LOGI(TAG, "────────────────────────────────────");
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

/* ================================================================
 *  APP MAIN
 * ================================================================ */

void app_main(void)
{
    esp_log_level_set("wifi",               ESP_LOG_WARN);
    esp_log_level_set("mesh",               ESP_LOG_WARN);
    esp_log_level_set("esp_netif_lwip",     ESP_LOG_WARN);
    esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);
    esp_log_level_set("i2c.common",         ESP_LOG_ERROR);
    esp_log_level_set("i2c.master",         ESP_LOG_ERROR);
    esp_log_level_set("mesh_schedule",      ESP_LOG_ERROR); /* suppress WND-RX flow control noise */

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    ESP_LOGI(TAG, "STA MAC: " MACSTR, MAC2STR(mac));
    ESP_LOGI(TAG, "Role   : %s", MESH_IS_ROOT ? "ROOT" : "NODE");

    s_mesh_eg = xEventGroupCreate();
    s_img_sem = xSemaphoreCreateBinary();

    led_init();
    lcd_init();   /* must be before wifi_init — AXP192 needs I2C before WiFi claims GPIO21/22 */

    wifi_init();
    mesh_init();

    xTaskCreate(led_task,     "led",     2048, NULL, 4, NULL);
    xTaskCreate(status_task,  "status",  4096, NULL, 3, NULL);
    xTaskCreate(display_task, "display", 16384, NULL, 3, NULL);

    xTaskCreate(button_task, "buttons", 4096, NULL, 5, NULL);  /* both root and node */

    if (MESH_IS_ROOT) {
        /* Install UART driver so mesh_recv_task can stream images to PC */
        uart_config_t uart_cfg = {
            .baud_rate  = 115200,
            .data_bits  = UART_DATA_8_BITS,
            .parity     = UART_PARITY_DISABLE,
            .stop_bits  = UART_STOP_BITS_1,
            .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        };
        uart_driver_install(UART_NUM_0, 4096, 0, 0, NULL, 0);
        uart_param_config(UART_NUM_0, &uart_cfg);
        xTaskCreate(mesh_recv_task, "recv", 12288, NULL, 5, NULL);
    } else {
        xTaskCreate(uart_recv_task,    "uart_recv",  4096, NULL, 5, NULL);
        xTaskCreate(img_transfer_task, "img_xfer",   8192, NULL, 5, NULL);
    }

    ESP_LOGI(TAG, "app_main() done — mesh running");
}