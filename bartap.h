// useful macros and defines

//register dereference
#define REG(x)  (*((uint32_t *)(x)))

//right justify to 16 bits
#define RJUST(x)    (0x0000 + x)

//use spi0 by default
#ifndef SPI_BASE
    #define SPI_BASE    SPI0_BASE
    #define PIN_MISO    16
    #define PIN_CS      17
    #define PIN_SCK     18
    #define PIN_MOSI    19
#endif

#ifndef CS_ENABLE()
    //chip select macros
    #define CS_ENABLE()     REG(SIO_BASE + SIO_GPIO_OUT_CLR_OFFSET) = 1 << PIN_CS
#endif
#ifndef CS_DISABLE()
    #define CS_DISABLE()    REG(SIO_BASE + SIO_GPIO_OUT_SET_OFFSET) = 1 << PIN_CS
#endif

//command macros
#define EMPTY_ARG           0x00000000
#define CMD0	            0			// GO_IDLE_STATE
#define CMD8	            8			// SEND_IF_COND
#define CMD9	            9			// SEND_CSD
#define CMD10	            10		    // SEND_CID 
#define CMD12	            12		    // STOP_TRANSMISSION
#define CMD13	            13	        // SD_STATUS
#define CMD16	            16		    // SET_BLOCKLEN
#define CMD17	            17		    // READ_SINGLE_BLOCK
#define CMD18	            18		    // READ_MULTIPLE_BLOCK
#define	ACMD23	            23	        // SET_WR_BLK_ERASE_COUNT
#define CMD24	            24		    // WRITE_BLOCK
#define CMD25	            25		    // WRITE_MULTIPLE_BLOCK
#define CMD32	            32		    // ERASE_ER_BLK_START
#define CMD33	            33		    // ERASE_ER_BLK_END
#define CMD38	            38		    // ERASE
#define	ACMD41	            41	        // SEND_OP_COND
#define	CMD48	            48		    // READ_EXTR_SINGLE
#define	CMD49	            49		    // WRITE_EXTR_SINGLE
#define CMD55	            55		    // APP_CMD
#define CMD58	            58		    // READ_OCR

//sd write tokens
#define DATA_START_TOKEN    0xFE
#define MBW_START_TOKEN     0xFC
#define MBW_STOP_TOKEN      0xFD

//R1 error conditions
#define IDLE_STATE          0b00000001
#define ERASE_RESET         0b00000010
#define ILLEGAL_CMD         0b00000100
#define CRC_ERROR           0b00001000
#define ERASE_SEQ_ERR       0b00010000
#define ADDR_ERROR          0b00100000
#define PARAM_ERROR         0b01000000

//time divisors
#define YEAR_DIV            31540000000
#define MONTH_DIV           2628000000
#define DAY_DIV             86400000
#define HR_DIV              3600000
#define MIN_DIV             60000

//TODO replace defines from sdk with ones here, want to remove dependance on sdk
#define OFFSET_TIME_UPPER   0x70
#define OFFSET_TIME_LOWER   0x74