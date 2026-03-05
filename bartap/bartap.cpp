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

typedef struct {
    uint8_t block_data[BLOCK_LEN];
} block_buffer;

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

void _sd_command(uint8_t cmd, uint32_t arg, uint8_t crc, uint8_t *res, size_t len = 1, bool disable_cs = true) {
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
    if (disable_cs) { CS_DISABLE(); }
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

//command 12 (stop transmission)
void _stop_transmission() {
    alarm_id_t timeout_alarm;
    uint8_t *res, *token;
    uint8_t cmdbuf[6] = {
        12|0x40,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0|0x01
    };
    spi_write_blocking(SPI_PORT, cmdbuf, 6);
    //get R1 portion of R1b response
    spi_read_blocking(SPI_PORT, 0xFF, res, 1);
    //wait for busy part of R1b to end
    timed_out = false;
    timeout_alarm = add_alarm_in_ms(250, init_timeout_callback, NULL, false);
    do {
        //busy timeout
        if (timed_out) { break; }
        spi_read_blocking(SPI_PORT, 0xFF, token, 1);
    } while(*token == 0x00);
    cancel_alarm(timeout_alarm);
}

//command 17 (read single block) timeout 100ms for response token
/*
 token = 0xFE - Successful read
 token = 0x0X - Data error
 token = 0xFF - Timeout
*/
void read_single_block(uint8_t *res, uint32_t addr, uint8_t *buf, uint8_t *token) {
    alarm_id_t timeout_alarm;
    //reset result buffer and token
    res[0] = 0xFF;
    *token = 0xFF;
    //CRC buffer, discarded in SPI mode but still needs to be read in
    uint8_t crc[2];
    //send read single block command, DO NOT disable chip select afterward
    _sd_command(CMD17, addr, CMD17_CRC, res, 1, false);
    //check for R1 response recieved
    if(res[0] != 0xFF) {
        //wait for response token, timeout 100ms
        timed_out = false;
        timeout_alarm = add_alarm_in_ms(100, init_timeout_callback, NULL, false);
        do {
            if (timed_out) { break; }
            spi_read_blocking(SPI_PORT, 0xFF, token, 1);
        } while(*token == 0xFF);
        cancel_alarm(timeout_alarm);

        if (*token == 0xFE) {
            //successful read, read data into buffer
            spi_read_blocking(SPI_PORT, 0xFF, buf, BLOCK_LEN);
            spi_read_blocking(SPI_PORT, 0xFF, crc, 2);
        }
    }
    //disable chip select at end of read operation
    CS_DISABLE();
}

//command 18 (read multiple block)
/*
 token = 0xFE - Successful read
 token = 0x0X - Data error
 token = 0xFF - Timeout
*/
uint8_t read_multiple_block(uint8_t *res, uint32_t addr, block_buffer *buf, uint8_t *token, uint32_t count) {
    alarm_id_t timeout_alarm;
    //reset result buffer and token
    res[0] = 0xFF;
    *token = 0xFF;
    //CRC buffer, discarded in SPI mode but still needs to be read in
    uint8_t crc[2];
    //send read single block command, DO NOT disable chip select afterward
    _sd_command(CMD18, addr, CMD18_CRC, res, 1, false);
    //check for R1 response recieved
    if(res[0] != 0xFF) {
        //wait for response token, timeout 100ms
        timed_out = false;
        timeout_alarm = add_alarm_in_ms(100, init_timeout_callback, NULL, false);
        do {
            if (timed_out) { break; }
            spi_read_blocking(SPI_PORT, 0xFF, token, 1);
        } while(*token == 0xFF);
        cancel_alarm(timeout_alarm);

        if (*token == 0xFE) {
            uint32_t i = 0;
            do {
                //successful read, read data into buffer
                spi_read_blocking(SPI_PORT, 0xFF, buf[i++].block_data, BLOCK_LEN);
                spi_read_blocking(SPI_PORT, 0xFF, crc, 2);
                //wait for next data block
                timed_out = false;
                timeout_alarm = add_alarm_in_ms(100, init_timeout_callback, NULL, false);
                do {
                    if (timed_out) { _stop_transmission(); CS_DISABLE(); return 1; }
                    spi_read_blocking(SPI_PORT, 0xFF, token, 1);
                } while(*token != 0xFE);
                cancel_alarm(timeout_alarm);
            } while (i < count);
            _stop_transmission();
        }
    }
    //disable chip select at end of read operation
    CS_DISABLE();
    return 0;
}

//command 24 (write block) timeout 250ms after data accepted token
/*
 token = 0x00 - Busy timeout
 token = 0x05 - Data accepted
 token = 0xFF - Timeout
*/
void write_block(uint8_t *res, uint32_t addr, uint8_t *buf, uint8_t *token) {
    alarm_id_t timeout_alarm;
    //reset result buffer and token
    res[0] = 0xFF;
    *token = 0xFE; //0b11111110 data block start token, single block write
    //CRC buffer, discarded in SPI mode but still needs to be read in
    uint8_t crc[2];
    //send write block command, DO NOT disable chip select afterward
    _sd_command(CMD24, addr, CMD24_CRC, res, 1, false);
    //check R1
    if(res[0] == 0) {
        //no errors, send start token and data to write
        spi_write_blocking(SPI_PORT, token, 1);
        spi_write_blocking(SPI_PORT, buf, BLOCK_LEN);
        //wait for response token, timeout 250ms
        *token = 0xFF;
        timed_out = false;
        timeout_alarm = add_alarm_in_ms(250, init_timeout_callback, NULL, false);
        do {
            if (timed_out) { break; }
            spi_read_blocking(SPI_PORT, 0xFF, token, 1);
        } while(*token == 0xFF);
        cancel_alarm(timeout_alarm);
        if ((*token & 0x1F) == 0x05) {
            //wait for write to finish, timeout 250ms
            *token = 0x00;
            timed_out = false;
            timeout_alarm = add_alarm_in_ms(250, init_timeout_callback, NULL, false);
            do {
                //busy timeout
                if (timed_out) { break; }
                spi_read_blocking(SPI_PORT, 0xFF, token, 1);
            } while(*token == 0x00);
            cancel_alarm(timeout_alarm);
            if (!timed_out) {
                *token = 0x05;
            }
        }
    }
    //disable chip select at end of write operation
    CS_DISABLE();
}

//command 25 (write multiple block) 250ms after data accepted token
/*
 token = 0x00 - Busy timeout
 token = 0x05 - Data accepted
 token = 0xFF - Timeout
 returns number of well written blocks
*/
uint32_t write_multiple_block(uint8_t *res, uint32_t addr, block_buffer *buf, uint8_t *token, uint32_t count) {
    alarm_id_t timeout_alarm;
    //reset result buffer and token
    res[0] = 0xFF;
    *token = 0xFC; //0b11111100 data block start token, multiple block write
    //CRC buffer, discarded in SPI mode but still needs to be read in
    uint8_t crc[2];
    //send write block command, DO NOT disable chip select afterward
    _sd_command(CMD25, addr, CMD25_CRC, res, 1, false);
    //check R1
    if(res[0] == 0) {
        //no errors, send start token and data to write
        uint8_t i = 0;
        do {
            *token = 0xFC;
            spi_write_blocking(SPI_PORT, token, 1);
            spi_write_blocking(SPI_PORT, buf[i++].block_data, BLOCK_LEN);

            //wait for response token, timeout 250ms
            *token = 0xFF;
            timed_out = false;
            timeout_alarm = add_alarm_in_ms(250, init_timeout_callback, NULL, false);
            do {
                if (timed_out) { break; }
                spi_read_blocking(SPI_PORT, 0xFF, token, 1);
            } while(*token == 0xFF);
            cancel_alarm(timeout_alarm);

            if ((*token & 0x1F) == 0x05) {
                //wait for write to finish, timeout 250ms
                *token = 0x00;
                timed_out = false;
                timeout_alarm = add_alarm_in_ms(250, init_timeout_callback, NULL, false);
                do {
                    //busy timeout
                    if (timed_out) { break; }
                    spi_read_blocking(SPI_PORT, 0xFF, token, 1);
                } while(*token == 0x00);
                cancel_alarm(timeout_alarm);
                if (!timed_out) {
                    *token = 0x05;
                }
            } else {
                //data error token recieved
                CS_DISABLE();
                //return result of ACMD22 (send_num_wr_blocks) to get number of well written blocks
                //TODO: implement ACMD22 and update return statement here
                return 1;
            }
        } while (i < count);
        //all data to be written sent, send stop transmission token
        *token = 0xFD;
        spi_write_blocking(SPI_PORT, token, 1);
        //wait for card busy
        *token = 0x00;
        timed_out = false;
        timeout_alarm = add_alarm_in_ms(250, init_timeout_callback, NULL, false);
        do {
            //busy timeout
            if (timed_out) { break; }
            spi_read_blocking(SPI_PORT, 0xFF, token, 1);
        } while(*token == 0x00);
        cancel_alarm(timeout_alarm);
        if (!timed_out) {
            *token = 0x05;
        }
    }
    //disable chip select at end of write operation
    CS_DISABLE();
    return count;
}

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
    alarm_id_t timeout_alarm;
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
    timeout_alarm = add_alarm_in_ms(1000, init_timeout_callback, NULL, false);
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
    cancel_alarm(timeout_alarm);

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
    //read buffer (5 blocks) and token
    block_buffer read_write_buffer[5];
    uint8_t token;

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


    //SD read block 0x100
    printf("reading block\n");
    read_single_block(res, 0x00000100, read_write_buffer[0].block_data, &token);
    //print out the block
    if ((res[0] == 0) && (token == 0xFE)) {
        for (uint16_t i = 0; i < 512; i++) {
            printf("%x", read_write_buffer[0].block_data[i]);
        }
        printf("\n");
    } else {
        if(SD_TOKEN_OOR(token))     { printf("block address out of range\n")    ;}
        if(SD_TOKEN_CECC(token))    { printf("card ECC failure\n")              ;}
        if(SD_TOKEN_CC(token))      { printf("CC error\n")                      ;}
        if(SD_TOKEN_ERROR(token))   { printf("single block read error\n")       ;}
    }

    //SD read 5 blocks starting with block 0x100
    printf("reading 5 blocks\n");
    if (read_multiple_block(res, 0x00000100, read_write_buffer, &token, 5) == 0) {
        //sucessful multiblock read
        for (uint8_t j = 0; j < 5; j++) {
            printf("block %d\n", j);
            for (uint16_t i = 0; i < 512; i++) {
                printf("%x", read_write_buffer[j].block_data[i]);
            }
            printf("\n");
        }
    } else {
        //timed out
        if(SD_TOKEN_OOR(token))     { printf("block address out of range\n")    ;}
        if(SD_TOKEN_CECC(token))    { printf("card ECC failure\n")              ;}
        if(SD_TOKEN_CC(token))      { printf("CC error\n")                      ;}
        if(SD_TOKEN_ERROR(token))   { printf("block read error\n")              ;}
    }

    //set data to be written
    printf("data to be written\n");
    for (uint8_t j = 0; j < 5; j++) {
        printf("block %d\n", j);
        for (uint16_t i = 0; i < 512; i++) {
            read_write_buffer[j].block_data[i]++;
            printf("%x", read_write_buffer[j].block_data[i]);
        }
        printf("\n");
    }
    //SD write 5 blocks starting with block 0x100
    printf("writing 5 blocks\n");
    if (write_multiple_block(res, 0x00000100, read_write_buffer, &token, 5) == 5) {
        //sucessful multiblock write
    } 
    if (token == 0x05) {
        printf("success\n");
    } else {
        //timed out
        if(SD_TOKEN_OOR(token))     { printf("block address out of range\n")    ;}
        if(SD_TOKEN_CECC(token))    { printf("card ECC failure\n")              ;}
        if(SD_TOKEN_CC(token))      { printf("CC error\n")                      ;}
        if(SD_TOKEN_ERROR(token))   { printf("block read error\n")              ;}
    }

    //SD read 5 blocks starting with block 0x100
    printf("reading same 5 blocks\n");
    if (read_multiple_block(res, 0x00000100, read_write_buffer, &token, 5) == 0) {
        //sucessful multiblock read
        for (uint8_t j = 0; j < 5; j++) {
            printf("block %d\n", j);
            for (uint16_t i = 0; i < 512; i++) {
                printf("%x", read_write_buffer[j].block_data[i]);
            }
            printf("\n");
        }
    } else {
        //timed out
        if(SD_TOKEN_OOR(token))     { printf("block address out of range\n")    ;}
        if(SD_TOKEN_CECC(token))    { printf("card ECC failure\n")              ;}
        if(SD_TOKEN_CC(token))      { printf("CC error\n")                      ;}
        if(SD_TOKEN_ERROR(token))   { printf("block read error\n")              ;}
    }

    while (true) {

    }
}
