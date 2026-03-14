#include "bartap.h"

// internal driver functions
static inline uint8_t cs_enable() {

}

static inline void cs_disable() {

}

static void sd_spi_init() {
    // SPI initialisation
    spi_init(SPI_PORT, 1000*1000);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PIN_CS,   GPIO_FUNC_SIO);
    gpio_set_function(PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    
    // Chip select is active-low, so we'll initialise it to a driven-high state
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_put(PIN_CS, 1);

    // sd card spi mode initialization process
}

static BYTE spi_transfer(BYTE data) {
    // write data to SSPDR register (right justified)
    hw_set_bits(SPI_HW->dr, data);
    // wait while SSPSR register == 0x10 (busy flag is set)
    while (SPI_HW->sr == 0x10);
    // return last 8 bits of SSPDR register
    return (uint8_t) SPI_HW->dr;
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
    // send command index
    spi_transfer(cmd|0x40);
    // send argument
    spi_transfer((uint8_t)(arg >> 24));
    spi_transfer((uint8_t)(arg >> 16));
    spi_transfer((uint8_t)(arg >> 8));
    spi_transfer((uint8_t)(arg));
    // if needed send crc
    if (cmd == 0) { spi_transfer(CMD0_CRC); }
    else if (cmd == 8) { spi_transfer(CMD8_CRC); }
    // wait for response
    BYTE R1;
    uint8_t i = 0;
    while((R1 = spi_transfer(0xFF)) == 0xFF) {
        i++;
        if (i > 8) break;
    }
    // return R1 response byte
    return R1;
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
