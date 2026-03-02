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

int sd_command(uint8_t cmd, uint32_t arg, uint8_t crc, uint8_t *res, size_t len = 1) {
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
    sd_command(CMD0, CMD0_ARG, CMD0_CRC, res);
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

//command 58 (read operations conditions register), returns type of response recieved
int read_ocr(uint8_t *res) {
    //R3 response format, R1 + four bytes
    //check for illegal command (response format will be R1 only)
    if(sd_command(CMD58, CMD58_ARG, CMD58_CRC, res, 5) == 0) {
        return 1;
    }
    //command recognized, response will be R3
    return 3;
}

//command 55
int app_cmd(uint8_t *res) {
    //R1 response format, single byte
    sd_command(CMD55, CMD55_ARG, CMD55_CRC, res);
    //check R1 for errors
    if(res[0] > 1) {
//        if(PARAM_ERROR(res[0])) {printf("Parameter Error\n");}
//        if(ADDR_ERROR(res[0])) {printf("Address Error\n");}
//        if(ERASE_SEQ_ERROR(res[0])) {printf("Erase Sequence Error\n");}
//        if(CRC_ERROR(res[0])) {printf("CRC Error\n");}
        if(ILLEGAL_CMD(res[0])) {
//            printf("Illegal Command\n");
            return ILL_CMD_ER;
        }
//        if(ERASE_RESET(res[0])) {printf("Erase Reset Error\n");}
        return SD_ERROR;
    }
    return 1;
}

//app command 41
int send_op_condition(uint8_t *res) {
    //app command, send command 55 first
    if(app_cmd(res) == 0) {
        //something went wrong
        return 0;
    }
    //R1 response format, single byte
    sd_command(ACMD41, ACMD41_ARG, ACMD41_CRC, res);
    return 1;
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

int _process_R1(uint8_t *res) {
//    if(res[0] & 0b10000000) {printf("Error: MSB = 1\n");            }
    if(res[0] == 0) {
//        printf("Card Ready\n");
        return SD_READY;
    }
//    if(PARAM_ERROR(res[0])) {printf("Parameter Error\n");           }
//    if(ADDR_ERROR(res[0])) {printf("Address Error\n");              }
//    if(ERASE_SEQ_ERROR(res[0])) {printf("Erase Sequence Error\n");  }
//    if(CRC_ERROR(res[0])) {printf("CRC Error\n");                   }
    if(ILLEGAL_CMD(res[0])) {
//        printf("Illegal Command\n");
        return ILL_CMD_ER;
    }
//    if(ERASE_RESET(res[0])) {printf("Erase Reset Error\n");         }
    if(IN_IDLE(res[0])) {
//        printf("In Idle State\n");
        return IDLE_STATE;
    }
    return SD_ERROR;
}

int _process_R3(uint8_t *res) {
/*  
    printf("VDD Window: ");
    if(VDD_2728(res[3])) printf("2.7-2.8, ");
    if(VDD_2829(res[2])) printf("2.8-2.9, ");
    if(VDD_2930(res[2])) printf("2.9-3.0, ");
    if(VDD_3031(res[2])) printf("3.0-3.1, ");
    if(VDD_3132(res[2])) printf("3.1-3.2, ");
    if(VDD_3233(res[2])) printf("3.2-3.3, ");
    if(VDD_3334(res[2])) printf("3.3-3.4, ");
    if(VDD_3435(res[2])) printf("3.4-3.5, ");
    if(VDD_3536(res[2])) printf("3.5-3.6");
*/
//    printf("\n");
//    printf("Card Power Up Status: ");
    if(POWER_UP_STATUS(res[1])) {
//        printf("READY\nCCS Status: ");
        if(CCS_VAL(res[1])) {
//            printf("%d\n", 1);
            return SD_HIGH_CAPACITY;
        } else {
//            printf("%d\n", 0);
            return SD_READY;
        }
    } else {
//        printf("BUSY\n");
        return IDLE_STATE;
    }
}

int _process_R7(uint8_t *res) {
    if (res[4] != ECHO) {
        //echo mismatch
//        printf("Echo: %x\n", res[4]);
//        printf("echo mismatch\n");
        return ECHO_MISMATCH;
    }
//    printf("Command Version: %x \n", CMD_VER(res[1]));
//    printf("Echo: %x\n", res[4]);
//    printf("Voltage Accepted: %x :", res[3]);
    switch (VOL_ACC(res[3])) {
        case VOLTAGE_ACC_27_33:
//            printf("2.7-3.6V\n");
            return VOLTAGE_SUPPORTED;
        case VOLTAGE_ACC_LOW:
//            printf("LOW VOLTAGE\n");
            break;
        case VOLTAGE_ACC_RES1:
//            printf("RESERVED\n");
            break;
        case VOLTAGE_ACC_RES2:
//            printf("RESERVED\n");
            break;
        default:
//            printf("NOT DEFINED\n");
            break;
    }
    return SD_ERROR;
}

int process_response(uint8_t *res, int res_type = 1) {
    int R1_status;
    switch (res_type)
    {
    case 0:
//        printf("Command 55 (app command) Error: \n");
    case 1:
//        printf("Recieved R1 Response: %x :\n", res[0]);
        return _process_R1(res);
    case 3:
//        printf("Recieved R3 Response: %x, %x, %x, %x, %x:\n", res[0], res[1], res[2], res[3], res[4]);
//        printf("R1 response:\n");
        R1_status = _process_R1(res);
        if (R1_status <= 0) {
            //error
            return R1_status;
        }
//        printf("rest of R3 response:\n");
        return _process_R3(res);
    case 7:
//        printf("Recieved R7 Response: %x, %x, %x, %x, %x:\n", res[0], res[1], res[2], res[3], res[4]);
//        printf("R1 response:\n");
        R1_status = _process_R1(res);
        if (R1_status <= 0) {
            //error
            return R1_status;
        }
//        printf("rest of R7 response:\n");
        return _process_R7(res);
    }
    return SD_ERROR;
}

volatile bool timed_out = false;
int64_t init_timeout_callback(alarm_id_t id, __unused void *user_data) {
    timed_out = true;
    return 0;
}

int sd_init(uint8_t *res) {
    bool version_2 = true;
    bool high_capacity = false;
    bool ready_status = false;
    //startup sequence for SD card into idle state (command 0)
    sd_boot(res);
    if (process_response(res) <= 0) {
        //failed power on
        printf("failed SD Card power on. double check connection\n");
        return 1;
    }

    //interface conditions (command 8)
    switch (process_response(res, send_if_condition(res))) {
        case ILL_CMD_ER:
//            printf("version 1.x card\n");
            version_2 = false;
            break;
        case ECHO_MISMATCH:
            printf("echo mismatch, unusable card.\n");
            return 1;
        case VOLTAGE_SUPPORTED:
//            printf("version 2.00 or later card\n");
            break;
        default:
            printf("non-compatible voltage range, unusable card.\n");
            return 1;
    }

    //operations conditions register (command 58)
    switch (process_response(res, read_ocr(res))) {
        case ILL_CMD_ER:
            printf("not SD Memory Card, unsupported.\n");
            return 1;
        case IDLE_STATE:
            break;
        case SD_READY:
            break;
        case SD_HIGH_CAPACITY:
            high_capacity = true;
            break;
        default:
            return 1;
    }

    //send operating condition (app command 41)
    add_alarm_in_ms(1000, init_timeout_callback, NULL, false);
    do {
        switch (process_response(res, send_op_condition(res))) {
            case IDLE_STATE:
                break;
            case SD_READY:
                ready_status = true;
                break;
            case ILL_CMD_ER:
                printf("not SD Memory Card, unsupported.\n");
                return 1;
            default:
                printf("SD Card Error. R1 Response: %x", res[0]);
                return 1;
        }
    } while(!timed_out && !ready_status);
    if (timed_out && !ready_status) {
        printf("initialisation sequence timed out.\n");
        return 1;
    }

    //resissue operations conditions register command to get the now valid CCS bit
    switch (process_response(res, read_ocr(res))) {
        case ILL_CMD_ER:
            printf("not SD Memory Card, unsupported.\n");
            return 1;
        case IDLE_STATE:
            break;
        case SD_READY:
            break;
        case SD_HIGH_CAPACITY:
            high_capacity = true;
            break;
        default:
            return 1;
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
    
    //reponse buffer
    uint8_t res[5];
    //version and capacity flags (may be useful to have idk)
    bool sd_v2;
    bool sd_hcxc;

    switch(sd_init(res)) {
        case 0:
            sd_v2 = false;
            sd_hcxc = false;
            printf("Successfully Initialized V1.X SD Memory Card\n");
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
