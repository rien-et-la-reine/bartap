#include "bartap.h"

// internal driver functions
static inline uint8_t cs_enable() {

}

static inline void cs_disable() {

}

static void sd_spi_init() {
    spi_reset(SPI_PORT);
    spi_unreset(SPI_PORT);
}

static BYTE spi_transfer(BYTE data) {

}

static void spi_rcv_16(BYTE *buffer, unsigned int length) {

}

static void spi_snd_16(BYTE *buffer, unsigned int length) {

}

static uint8_t datablock_rcv(BYTE *buffer, unsigned int length) {

}

static BYTE datablock_snd(BYTE *buffer, unsigned int length) {

}

static uint8_t wait_sd_ready(uint32_t wait_timeout) {

}

static BYTE command_snd(uint8_t cmd, uint32_t arg) {

}

// FatFs MAI functions
//TODO

int main()
{
    stdio_init_all();

    while (true) {
        printf("Hello, world!\n");
        sleep_ms(1000);
    }
}
