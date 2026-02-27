#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"

#include "command_macros.h"

// SPI Defines
// We are going to use SPI 0, and allocate it to the following GPIO pins
// Pins can be changed, see the GPIO function select table in the datasheet for information on GPIO assignments
#define SPI_PORT spi1
#define PIN_MISO 12
#define PIN_CS   13
#define PIN_SCK  14
#define PIN_MOSI 11

void CS_ENABLE() {
    uint8_t idle[1] = {
        0xFF
    };
    spi_write_blocking(SPI_PORT, idle, 1);
    gpio_put(PIN_CS, 0);
    spi_write_blocking(SPI_PORT, idle, 1);
}

void CS_DISABLE() {
    uint8_t idle[1] = {
        0xFF
    };
    spi_write_blocking(SPI_PORT, idle, 1);
    gpio_put(PIN_CS, 1);
    spi_write_blocking(SPI_PORT, idle, 1);
}

int sd_command(uint8_t cmd, uint32_t arg, uint8_t crc, uint8_t *res, size_t len) {
    CS_ENABLE();
    uint8_t cmdbuf[6] = {
        cmd|0x40,
        arg >> 24,
        arg >> 16,
        arg >> 8,
        arg,
        crc|0x01
    };
    spi_write_blocking(SPI_PORT, cmdbuf, 6);
    spi_read_blocking(SPI_PORT, 0xFF, res, 1);
    while (res[0] == 0xFF) {
        spi_read_blocking(SPI_PORT, 0xFF, res, 1);
    }
    if(ILLEGAL_CMD(res[0])) {
        CS_DISABLE();
        return 0;
    }
    if (len > 1) {
        uint8_t res_sub[len-1];
        spi_read_blocking(SPI_PORT, 0xFF, res_sub, len-1);
        CS_DISABLE();
        for(int i = 1; i < len; i++) {
            res[i] = res_sub[i-1];
        }
    }
    CS_DISABLE();
    return 1;
}

//command 0 (go to idle state), returns type of response recieved
int go_idle_state(uint8_t *res) {
    //R1 response format, single byte
    sd_command(CMD0, CMD0_ARG, CMD0_CRC, res, 1);
    return 1;
}

//command 8 (send interface condition), returns type of response recieved
int send_if_condition(uint8_t *res) {
    //R7 response format, R1 + four bytes
    //check for illegal command (response format will be R1 only)
    if(sd_command(CMD8, CMD8_ARG, CMD8_CRC, res, 5) == 0) {
        return 1;
    }
    //command recognized, response will be R7
    return 7;
}

void sd_init(uint8_t *res) {
    uint8_t idle[1] = {
        0xFF
    };
    gpio_put(PIN_CS, 1);
    sleep_ms(1);
    for(int i = 0; i < 10; i++) {
        spi_write_blocking(SPI_PORT, idle, 1);
    }
    gpio_put(PIN_CS, 1);
    spi_write_blocking(SPI_PORT, idle, 1);

    //command the card into an idle state
    go_idle_state(res);
}

void _print_R1(uint8_t *res) {
    if(res[0] & 0b10000000) {printf("Error: MSB = 1\n"); return;}
    if(res[0] == 0) {printf("Card Ready\n"); return;}
    if(PARAM_ERROR(res[0])) {printf("Parameter Error\n");}
    if(ADDR_ERROR(res[0])) {printf("Address Error\n");}
    if(ERASE_SEQ_ERROR(res[0])) {printf("Erase Sequence Error\n");}
    if(CRC_ERROR(res[0])) {printf("CRC Error\n");}
    if(ILLEGAL_CMD(res[0])) {printf("Illegal Command\n");}
    if(ERASE_RESET(res[0])) {printf("Erase Reset Error\n");}
    if(IN_IDLE(res[0])) {printf("In Idle State\n");}
}

void _print_R3(uint8_t *res) {

}

void _print_R7(uint8_t *res) {
    printf("Command Version: %x \n", CMD_VER(res[1]));
    printf("Voltage Accepted: %x :", res[3]);
    switch (VOL_ACC(res[3])) {
        case VOLTAGE_ACC_27_33:
            printf("2.7-3.6V\n");
            break;
        case VOLTAGE_ACC_LOW:
            printf("LOW VOLTAGE\n");
            break;
        case VOLTAGE_ACC_RES1:
            printf("RESERVED\n");
            break;
        case VOLTAGE_ACC_RES2:
            printf("RESERVED\n");
            break;
        default:
            printf("NOT DEFINED\n");
            break;
    }
    printf("Echo: %x\n", res[4]);
}

void print_response(uint8_t *res, int res_type = 1) {
    switch (res_type)
    {
    case 1:
        printf("Recieved R1 Response: %x :\n", res[0]);
        _print_R1(res);
        break;
    case 3:
        printf("Recieved R3 Response: %x, %x, %x, %x, %x:\n", res[0], res[1], res[2], res[3], res[4]);
        printf("R1 response:\n");
        _print_R1(res);
        printf("rest of R3 response:\n");
        _print_R3(res);
        break;
    case 7:
        printf("Recieved R7 Response: %x, %x, %x, %x, %x:\n", res[0], res[1], res[2], res[3], res[4]);
        printf("R1 response:\n");
        _print_R1(res);
        printf("rest of R7 response:\n");
        _print_R7(res);
        break;
    }
}

int main()
{
    stdio_init_all();

    // SPI initialisation. using SPI at 1MHz.
    spi_init(SPI_PORT, 1000*1000);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PIN_CS,   GPIO_FUNC_SIO);
    gpio_set_function(PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    
    // Chip select is active-low, so initialise it to a driven-high state
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_put(PIN_CS, 1);
    
    //reponse buffer
    uint8_t res[5];
    //startup sequence for SD card into idle state
    sd_init(res);
    print_response(res);
    //interface conditions
    print_response(res, send_if_condition(res));

    while (true) {

    }
}
