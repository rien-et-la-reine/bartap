#define ILL_CMD_ER           0
#define MSB_ERROR           -1
#define SD_ERROR            -2
#define ECHO_MISMATCH       -3
#define TIMEOUT             -4

#define IDLE_STATE           1
#define SD_READY             2
#define VOLTAGE_SUPPORTED    3
#define SD_HIGH_CAPACITY     4

//command 0
#define CMD0        0
#define CMD0_ARG    0x00000000
#define CMD0_CRC    0x94

//command 8
#define CMD8        8
#define CMD8_ARG    0x0000001AA
#define ECHO        0xAA //last byte or CMD8_ARG
#define CMD8_CRC    0x86 //(1000011 << 1)

//command 58
#define CMD58        58
#define CMD58_ARG    0x00000000
#define CMD58_CRC    0x00

//command 55
#define CMD55       55
#define CMD55_ARG   0x00000000
#define CMD55_CRC   0x00

//app command 41
#define ACMD41      41
#define ACMD41_ARG  0x40000000  //indicates High Capacity cards are supported (HCS bit is 1)
#define ACMD41_CRC  0x00

//response 1
#define PARAM_ERROR(X)      X & 0b01000000
#define ADDR_ERROR(X)       X & 0b00100000
#define ERASE_SEQ_ERROR(X)  X & 0b00010000
#define CRC_ERROR(X)        X & 0b00001000
#define ILLEGAL_CMD(X)      X & 0b00000100
#define ERASE_RESET(X)      X & 0b00000010
#define IN_IDLE(X)          X & 0b00000001

//response 3
#define POWER_UP_STATUS(X)  X & 0x40
#define CCS_VAL(X)          X & 0x40
#define VDD_2728(X)         X & 0b10000000
#define VDD_2829(X)         X & 0b00000001
#define VDD_2930(X)         X & 0b00000010
#define VDD_3031(X)         X & 0b00000100
#define VDD_3132(X)         X & 0b00001000
#define VDD_3233(X)         X & 0b00010000
#define VDD_3334(X)         X & 0b00100000
#define VDD_3435(X)         X & 0b01000000
#define VDD_3536(X)         X & 0b10000000

//response 7
#define CMD_VER(X)          ((X >> 4) & 0xF0)
#define VOL_ACC(X)          (X & 0x1F)
#define VOLTAGE_ACC_27_33   0b00000001
#define VOLTAGE_ACC_LOW     0b00000010
#define VOLTAGE_ACC_RES1    0b00000100
#define VOLTAGE_ACC_RES2    0b00001000