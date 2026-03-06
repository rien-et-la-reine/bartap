//command 0
#define CMD0        0
#define CMD0_ARG    0x00000000
#define CMD0_CRC    0x94

//command 8
#define CMD8        8
#define CMD8_ARG    0x0000001AA
#define ECHO        0xAA //last byte or CMD8_ARG
#define CMD8_CRC    0x86 //(1000011 << 1)

//command 12
#define CMD12       12
#define CMD12_ARG   0x00000000
#define CMD12_CRC   0x00

//command 16
#define BLOCK_LEN   512

//command 17
#define CMD17       17
#define CMD17_CRC   0x00

//command 18
#define CMD18       18
#define CMD18_CRC   0x00

//command 24
#define CMD24       24
#define CMD24_CRC   0x00

//command 25
#define CMD25       25
#define CMD25_CRC   0x00

//command 58
#define CMD58        58
#define CMD58_ARG    0x00000000
#define CMD58_CRC    0x00

//command 55
#define CMD55       55
#define CMD55_ARG   0x00000000
#define CMD55_CRC   0x00

//app command 22
#define ACMD22      22
#define ACMD22_ARG  0x00000000
#define ACMD22_CRC  0x00

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

//data error token
#define SD_TOKEN_OOR(X)     X & 0b00001000
#define SD_TOKEN_CECC(X)    X & 0b00000100
#define SD_TOKEN_CC(X)      X & 0b00000010
#define SD_TOKEN_ERROR(X)   X & 0b00000001