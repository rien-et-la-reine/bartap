#include <stdio.h>
#include <stdint.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"

#include "ff16/source/ff.h" //to get basic FatFs data types
#include "ff16/source/diskio.h" // FatFs MAI function declarations and defines

// SPI defines
// Pins can be changed, see the GPIO function select table in the datasheet for information on GPIO assignments
#define SPI_PORT spi1
#define PIN_MISO 12
#define PIN_CS   13
#define PIN_SCK  14
#define PIN_MOSI 11

// Command Macros
#define CMD0        0			// GO_IDLE_STATE
#define CMD0_ARG    0x00000000
#define CMD0_CRC    0x94
#define CMD1	    1			// SEND_OP_COND (MMC)
#define CMD8	    8			// SEND_IF_COND
#define CMD8_ARG    0x0000001AA
#define CMD8_CRC    0x86
#define CMD9	    9			// SEND_CSD
#define CMD10	    10		    // SEND_CID
#define CMD12	    12		    // STOP_TRANSMISSION
#define ACMD13	    0x80+13	    // SD_STATUS (SDC)
#define CMD16	    16		    // SET_BLOCKLEN
#define CMD17	    17		    // READ_SINGLE_BLOCK
#define CMD18	    18		    // READ_MULTIPLE_BLOCK
#define CMD23	    23		    // SET_BLOCK_COUNT (MMC)
#define	ACMD23	    0x80+23	    // SET_WR_BLK_ERASE_COUNT (SDC)
#define CMD24	    24		    // WRITE_BLOCK
#define CMD25	    25		    // WRITE_MULTIPLE_BLOCK
#define CMD32	    32		    // ERASE_ER_BLK_START
#define CMD33	    33		    // ERASE_ER_BLK_END
#define CMD38	    38		    // ERASE
#define	ACMD41	    0x80+41	    // SEND_OP_COND (SDC)
#define ACMD41_ARG  0x40000000
#define CMD55	    55		    // APP_CMD
#define CMD58	    58		    // READ_OCR

// internal driver functions
/*!
    \brief enable card select
    \details pulls the chip select pin of the card low

    \return 0 if card ready, 1 if timed out
*/
static inline uint8_t cs_enable();

/*!
    \brief disable card select
    \details releases the chip select pin of the card high
*/
static inline void cs_disable();

/*!
    \brief initialise spi peripheral
    \note must be called before anything else can be done
*/
static void sd_spi_init();

/*!
    \brief exchange data with sd card
    \details sends a single byte of data and returns the single byte of data received in response

    \param data a byte of data to be sent to the card
    \return a byte of data recieved from the card
*/
static BYTE spi_transfer(BYTE data);

/*!
    \brief recieve multiple bytes
    \details read multiple bytes from the card into the buffer in SPI 16-bit mode

    \param buffer data buffer to read into
    \param length number of bytes to read
*/
static void spi_rcv_16(BYTE *buffer, unsigned int length);

/*!
    \brief send multiple bytes
    \details send multiple bytes to the card from the buffer in SPI 16-bit mode

    \param buffer data buffer containing data to be sent
    \param length number of bytes to send
*/
static void spi_snd_16(BYTE *buffer, unsigned int length);

/*!
    \brief recieve a block of data from the card

    \param buffer data buffer to read into
    \param length length of data buffer in bytes
    \return 0 if successful, 1 if invalid data token or timeout
*/
static uint8_t datablock_rcv(BYTE *buffer, unsigned int length);

/*!
    \brief send a block of data from the card

    \param buffer data buffer containing data to be sent
    \param length length of data buffer in bytes
    \return data response token from card
*/
static BYTE datablock_snd(BYTE *buffer, unsigned int length);

/*!
    \brief wait for card ready
    \details wait for card to output constant high with specified ms timeout
    
    \param wait_timeout timeout in ms
    \returns 0 if card ready, 1 if timed out
*/
static uint8_t wait_sd_ready(uint32_t wait_timeout);

/*!
    \brief sends command to the card

    \param cmd command index
    \param arg command argument
    \note CRC for CMD0 and CMD8 are handled automatically, SPI mode does not otherwise use CRC
    
    \return R1 response
    \note for longer response formats the non-R1 part of the response should be handled outside this function where it is called
*/
static BYTE command_snd(uint8_t cmd, uint32_t arg);
