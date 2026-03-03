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

volatile bool timed_out;
int64_t init_timeout_callback(alarm_id_t id, __unused void *user_data) {
    timed_out = true;
    return 0;
}

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

void _sd_command(uint8_t cmd, uint32_t arg, uint8_t crc, uint8_t *res, size_t len = 1) {
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
        return;
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
    return;
}

//command 0 (go to idle state), returns type of response recieved
void go_idle_state(uint8_t *res) {
    //R1 response format, single byte
    _sd_command(CMD0, CMD0_ARG, CMD0_CRC, res);
}

//command 8 (send interface condition), returns type of response recieved
void send_if_condition(uint8_t *res) {
    //R7 response format, R1 + four bytes
    _sd_command(CMD8, CMD8_ARG, CMD8_CRC, res, 5);
}

//command 58 (read operations conditions register), returns type of response recieved
void read_ocr(uint8_t *res) {
    //R3 response format, R1 + four bytes
    _sd_command(CMD58, CMD58_ARG, CMD58_CRC, res, 5);
}

//command 17 (read single block) timeout 100ms
void read_single_block(uint32_t addr, uint8_t *buf, uint8_t *token) {
    uint8_t res1[1], read[1];
    *token = 0xFF;

    _sd_command(CMD17, addr, CMD17_CRC, res1);
    if(res1[0] != 0xFF) {
        //wait for response token, timeout 100ms
        timed_out = false;
        add_alarm_in_ms(100, init_timeout_callback, NULL, false);
        do {

        } while(!timed_out);
    }
}

// (write single block) timeout 250ms


//command 55
uint8_t _app_cmd(uint8_t *res) {
    //R1 response format, single byte
    _sd_command(CMD55, CMD55_ARG, CMD55_CRC, res);
    //check R1 for errors
    if(res[0] > 1) {
        return 1;
    }
    return 0;
}

//app command 41
void send_op_condition(uint8_t *res) {
    //app command, send command 55 first
    if(_app_cmd(res) > 0) {
        //something went wrong, do not issue the ACMD
        return;
    }
    //R1 response format, single byte
    _sd_command(ACMD41, ACMD41_ARG, ACMD41_CRC, res);
}


void sd_boot(uint8_t *res) {
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

/*
initialize sd card (in spi mode)
returns 0bXY 
where:
version 2.00+ (X) | high capacity (Y)
------------------|------------------
        0         |        0
        1         |        0
        1         |        1
and unused 0b01 (since v1.X high capacity cards don't exist) signifies an error
*/
uint8_t sd_init_spi(uint8_t *res) {
    bool version_2 = true;
    bool high_capacity = false;
    bool ready_status = false;
    printf("initializing SD card in SPI mode\n");
    //startup sequence for SD card into idle state (command 0)
    sd_boot(res);
    if (res[0] > 1) {
        //failed power on
        printf("failed SD Card power on. double check connection\n");
        return 1;
    }
    
    //interface conditions (command 8)
    send_if_condition(res);
    if(ILLEGAL_CMD(res[0])) {
//        printf("version 1.x card\n");
        version_2 = false;
    } else if (res[4] != ECHO) {
        printf("echo mismatch, unusable card.\n");
        return 1;
    } else if (VOL_ACC(res[3]) == VOLTAGE_ACC_27_33) {
//        printf("version 2.00 or later card\n");
    } else {
        printf("non-compatible voltage range, unusable card.\n");
        return 1;
    }

    //operations conditions register (command 58)
    read_ocr(res);
    if(ILLEGAL_CMD(res[0])) {
        printf("not SD Memory Card, unsupported.\n");
        return 1;
    } else if (res[0] > 1) {
        printf("error reading OCR.\n");
        return 1;
    }

    //send operating condition (app command 41)
    timed_out = false;
    add_alarm_in_ms(1000, init_timeout_callback, NULL, false);
    send_op_condition(res);
    if(ILLEGAL_CMD(res[0])) {
        printf("not SD Memory Card, unsupported.\n");
        return 1;
    } else if(res[0] > 1) {
        printf("error sending op condition.\n");
        return 1;
    }
    if(!IN_IDLE(res[0])) {
        ready_status = true;
    }
    while(!ready_status) {
        if(timed_out) {
            printf("initialisation sequence timed out.\n");
            return 1;
        }
        send_op_condition(res);
        if(res[0] > 1) {
            printf("error sending op condition.\n");
            return 1;
        }
        if(!IN_IDLE(res[0])) {
            ready_status = true;
            break;
        }
    }

    //resissue operations conditions register command to get the now valid CCS bit
    read_ocr(res);
    if (res[0] > 0) {
        printf("error reading OCR.\n");
        return 1;
    }
    if(CCS_VAL(res[1])) {
        high_capacity = true;
    }

    if(!version_2) { return 0; }
    if(!high_capacity) { return 2; }
    return 3;
}

int main() {
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
    

    //SD Initialisation
    //reponse buffer
    uint8_t res[5];
    //version and capacity flags (may be useful to have idk)
    bool sd_v2;
    bool sd_hcxc;
    //read buffer (single sdhc/xc block, 512 bytes)
    uint8_t buf_single_block[512];

    switch(sd_init_spi(res)) {
        case 0:
            sd_v2 = false;
            sd_hcxc = false;
            printf("Successfully Initialized V1.X SD Memory Card\n");
            //TODO: implement CMD16 (SET_BLOCKLEN) to set blocklength for V1.X cards
            break;
        case 2:
            sd_v2 = true;
            sd_hcxc = false;
            printf("Successfully Initialized V2.00+ Standard Capacity SD Memory Card\n");
            break;
        case 3:
            sd_v2 = true;
            sd_hcxc = true;
            printf("Successfully Initialized V2.00+ High Capacity SD Memory Card\n");
            break;
        default:
            printf("SD Card Initialiasation Failed\n");
            return 1;
    }

    while (true) {

    }
}
