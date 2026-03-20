// basic SD card driver, implements FatFs MAI

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/dma.h"
#include "hardware/pio.h"

//get datatypes
#include "ff.h"
//get MAI declarations
#include "diskio.h"

#include "bartap.h"

//card status
static volatile DSTATUS card_status = STA_NOINIT;

//card type, 0x00 SDv1, 0x02 SDv2, 0x03 SDv2 High Capacity, 0x1X invalid card
static BYTE card_type;

/* Internal Driver Functions */

//send a byte to the sd card and read one back
static BYTE spi_transfer( BYTE data) {
    // write data to sspdr
    REG(SPI_BASE + SPI_SSPDR_OFFSET + REG_ALIAS_RW_BITS) = RJUST(data);
    // wait until sspsr bit 4 is no longer set
    while(REG(SPI_BASE + SPI_SSPSR_OFFSET) & 0x10);
    // return contents of sspdr
    return REG(SPI_BASE + SPI_SSPDR_OFFSET);
}

//send a command to the sd card and return the R1 response
static BYTE sd_command(BYTE cmd, DWORD arg) {
    uint8_t i = 0;
    //response byte
    BYTE R1;
    //send command index
    spi_transfer(cmd|0x40);
    //send argument
    spi_transfer((uint8_t) (arg >> 24));
    spi_transfer((uint8_t) (arg >> 16));
    spi_transfer((uint8_t) (arg >> 8));
    spi_transfer((uint8_t) (arg));
    //send CRC
    if (cmd == 0) {
        spi_transfer(0x94|0x01);
    } else if (cmd == 8) {
        spi_transfer(0x86|0x01);
    } else {
        spi_transfer(0x00|0x01);
    }
    //look for R1 response
    while ((R1 = spi_transfer(0xFF)) == 0xFF) {
        i++;
        if (i > 8) { break; }
    }
    return R1;
}

//sd card initialization procedure
static void sd_init() {
    //set card status uninitialized flag
    card_status = STA_NOINIT;
    //response buffer
    BYTE res[5];
    //insert sd card initialization sequence here
    //assert chip select
    spi_transfer(0xFF);
    CS_ENABLE();
    spi_transfer(0xFF);
    
    //CMD0 (go idle state)
    if (sd_command(CMD0, EMPTY_ARG) != IDLE_STATE) { return; }
    //CMD8 (send if cond)
    res[0] = sd_command(CMD8, (EMPTY_ARG + 0x1AA));
    if (res[0] <= IDLE_STATE) {
        //version 2+ card, read in rest of R7 response
        spi_transfer(0xFF);
        spi_transfer(0xFF);
        res[3] = spi_transfer(0xFF);
        res[4] = spi_transfer(0xFF);
        //set card type v2 flag
        card_type |= 0x02;
        //confirm check patern
        if (res[4] != 0xAA) { card_type |= 0x10; return; }
        //don't need to confirm voltage range technically since all cards can operate at 3v3 by default but doing it once here anyway
        if ((res[3] & 0x0F) != 0x01) { card_type |= 0x10; return; }
        //CMD58
        res[0] = sd_command(CMD58, EMPTY_ARG);
        if (res[0] > IDLE_STATE) {
            //error
            card_type |= 0x10;
            return;
        } else {
            //read in OCR, CCS bit is not valid and all cards by default support 3v3 so no need to check anything here
            spi_transfer(0xFF);
            spi_transfer(0xFF);
            spi_transfer(0xFF);
            spi_transfer(0xFF);            
        }

        //ACMD41
        do {
            res[0] = sd_command(CMD55, EMPTY_ARG);
            if (res[0] > IDLE_STATE) { card_type |= 0x10; return; }
            res[0] = sd_command(ACMD41, 0x40000000);
            if (res[0] > IDLE_STATE) { card_type |= 0x10; return; }
        //repeat ACMD41 until R1 response no longer reads IDLE_STATE
        } while (res[0] != 0x00);

        //CMD58, to get CCS bit
        res[0] = sd_command(CMD58, EMPTY_ARG);
        if (res[0] > IDLE_STATE) {
            //unusable card or error
            card_type |= 0x10;
            return;
        } else {
            //read in OCR, once again ignoring voltage range, we only care about the most significant byte here
            res[1] = spi_transfer(0xFF);
            spi_transfer(0xFF);
            spi_transfer(0xFF);
            spi_transfer(0xFF);
            //check OCR bits 31, 30
            if (res[1] == 0xC0) {
                //SDHC or SDXC card, set high capacity flag
                card_type |= 0x01;
            }
            //card initialized, v2+ SD memory card          
        }
    } else if (res[0] == ILLEGAL_CMD) {
        //version 1.xx card or not SD
        //CMD58, if illegal, unusable card
        res[0] = sd_command(CMD58, EMPTY_ARG);
        if (res[0] > IDLE_STATE) {
            //unusable card or error
            card_type |= 0x10;
            return;
        } else {
            //read in OCR, not using it for anything in this case
            spi_transfer(0xFF);
            spi_transfer(0xFF);
            spi_transfer(0xFF);
            spi_transfer(0xFF);            
        }
        //ACMD41, if illegal, unusable card
        do {
            res[0] = sd_command(CMD55, EMPTY_ARG);
            if (res[0] > IDLE_STATE) { card_type |= 0x10; return; }
            res[0] = sd_command(ACMD41, 0x40000000);
            if (res[0] > IDLE_STATE) { card_type |= 0x10; return; }
        //repeat ACMD41 until R1 response no longer reads IDLE_STATE
        } while (res[0] != 0x00);
        //card initialized, v1.xx SD memory card
    }

    //unset card status flag
    card_status &= (~STA_NOINIT);
}

/* FatFs MAI functions */
DSTATUS disk_initialize (BYTE pdrv) {
    sd_init();
    return card_status;
}

DSTATUS disk_status (BYTE pdrv) {
    return card_status;
}

DRESULT disk_read (BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {

}

DRESULT disk_write (BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count){

}

DRESULT disk_ioctl (BYTE pdrv, BYTE cmd, void* buff){

}