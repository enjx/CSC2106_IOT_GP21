/* hw_config.c
 * Hardware configuration for Cytron Maker Pi Pico SD card slot
 */

#include "hw_config.h"

/* ========== SPI Configuration ========== */
static spi_t spis[] = {
    {
        .hw_inst = spi1,
        .miso_gpio = 12,
        .mosi_gpio = 11,
        .sck_gpio = 10,
        .baud_rate = 12500 * 1000
    }
};

/* ========== SD Card Configuration ========== */
static sd_card_t sd_cards[] = {
    {
        .pcName = "0:",
        .spi = &spis[0],
        .ss_gpio = 15,
        .use_card_detect = false,
        .card_detect_gpio = -1,
        .card_detected_true = -1
    }
};

/* ========== Getter Functions ========== */

size_t sd_get_num() {
    return count_of(sd_cards);
}

sd_card_t *sd_get_by_num(size_t num) {
    if (num < sd_get_num()) {
        return &sd_cards[num];
    }
    return NULL;
}

size_t spi_get_num() {
    return count_of(spis);
}

spi_t *spi_get_by_num(size_t num) {
    if (num < spi_get_num()) {
        return &spis[num];
    }
    return NULL;
}
