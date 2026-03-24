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

//wait for card busy TODO: implement timeout parameter
static void wait_busy() {
    BYTE status;
    while((status = spi_transfer(0xFF)) == 0x00);
}

//send a command to the sd card and return the R1 response
static BYTE sd_command(BYTE cmd, DWORD arg) {
    uint8_t i = 0;
    //response byte
    BYTE R1;
    //ensure card is ready
    wait_busy();
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

//single block read, returns token
static BYTE sd_single_read(DWORD addr, BYTE *buf) {
    //token
    BYTE token = 0xFF;

    //assert chip select
    spi_transfer(0xFF);
    CS_ENABLE();
    spi_transfer(0xFF);

    //ensure no errors in R1 response
    BYTE R1 = sd_command(CMD17, addr);
    if(R1 == 0x00) {
        //proceed
        while((token = spi_transfer(0xFF)) == 0xFF) {
            //TODO implement 100ms timeout
        }
        //recieved token
        if (token == 0xFE) {
            //read in data into buffer
            for(uint16_t i = 0; i < 512; i++) {
                *buf++ = spi_transfer(0xFF);
            }
            //read in crc
            spi_transfer(0xFF);
            spi_transfer(0xFF);
        }
    }

    //deassert chip select
    spi_transfer(0xFF);
    CS_DISABLE();
    spi_transfer(0xFF);

    return token;
}

//single block write
static BYTE sd_single_write(DWORD addr, BYTE *buf) {
    //token
    BYTE token = 0xFF;

    //assert chip select
    spi_transfer(0xFF);
    CS_ENABLE();
    spi_transfer(0xFF);

    //ensure no errors in R1 response
    BYTE R1 = sd_command(CMD24, addr);
    if(R1 == 0x00) {
        //proceed
        uint16_t i = 0;
        //send start token
        spi_transfer(DATA_START_TOKEN);
        //send datablock
        while(i < 512) { spi_transfer(buf[i++]); }

        //get response token
        while((token = spi_transfer(0xFF)) == 0xFF) {
            //TODO: implement 250ms timeout
        }

        //recieved token
        if ((token & 0x1F) == 0x05) {
            //data accepted
            token = 0x05;
            //send status command
            sd_command(CMD13, EMPTY_ARG);
            if (spi_transfer(0xFF) != 0x00) {
                //R2 status response indicates programming error
                token = 0xFF;
            }
        }
    }

    //deassert chip select
    spi_transfer(0xFF);
    CS_DISABLE();
    spi_transfer(0xFF);

    return token;
}

//multiblock read TODO: use DMA to transfer the data?
static BYTE sd_multiple_read(DWORD addr, BYTE *buf, uint32_t cnt) {
    //token
    BYTE token = 0xFF;

    //assert chip select
    spi_transfer(0xFF);
    CS_ENABLE();
    spi_transfer(0xFF);

    //ensure no errors in R1 response
    BYTE R1 = sd_command(CMD18, addr);
    if(R1 == 0x00) {
        do {
            //proceed
            while((token = spi_transfer(0xFF)) == 0xFF) {
                //TODO implement 100ms timeout
            }
            //recieved token
            if (token == 0xFE) {
                //read in data into buffer
                for(uint16_t i = 0; i < 512; i++) {
                    *buf++ = spi_transfer(0xFF);
                }
                //read in crc
                spi_transfer(0xFF);
                spi_transfer(0xFF);
            } else { break; }
        } while (--cnt);
        //send stop transmission command
        sd_command(CMD12, EMPTY_ARG);
    }

    //deassert chip select
    spi_transfer(0xFF);
    CS_DISABLE();
    spi_transfer(0xFF);

    return token;
}

//multiblock write
static BYTE sd_multiple_write(DWORD addr, BYTE *buf, uint32_t cnt) {
    //token
    BYTE token = 0xFF, R1;

    //assert chip select
    spi_transfer(0xFF);
    CS_ENABLE();
    spi_transfer(0xFF);

    //preerase blocks
    if(!(sd_command(CMD55, EMPTY_ARG))) {
        R1 = sd_command(ACMD23, EMPTY_ARG + cnt);
    }

    if(R1 == 0x00) {
        //send write multiple block command
        R1 = sd_command(CMD25, addr);
        if(R1 == 0x00) {
            uint16_t i = 0;
             while(cnt--) {
                //send start token
                spi_transfer(MBW_START_TOKEN);
                //send datablock
                while(i < 512) { spi_transfer(buf[i++]); }
                //get response token
                while((token = spi_transfer(0xFF)) == 0xFF) {
                    //TODO: implement 250ms timeout
                }
                //recieved token
                if ((token & 0x1F) != 0x05) {
                    //data error
                    break;
                } else { token = 0x05; }
                wait_busy();
            }
            //send stop transmission token
            spi_transfer(MBW_STOP_TOKEN);
            //send status command
            sd_command(CMD13, EMPTY_ARG);
            if (spi_transfer(0xFF) != 0x00) {
                //R2 status response indicates programming error
                token = 0xFF;
            }
        }
    }

    //deassert chip select
    spi_transfer(0xFF);
    CS_DISABLE();
    spi_transfer(0xFF);

    return token;
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
    //typeconvert start address
    DWORD addr = (DWORD) sector;

    //check count is valid
    if(!count) { return RES_PARERR; }

    //check card initialization status
    if(card_status & STA_NOINIT) { return RES_NOTRDY; }

    //read in data using appropriate command
    if(count > 1) {
        //multiple block read
        if(sd_multiple_read(addr, buff, (uint32_t) count) != 0xFE) { return RES_ERROR; }
    } else {
        //single block read
        if(sd_single_read(addr, buff) != 0xFE) { return RES_ERROR; }
    }

    //no errors occured, read successful
    return RES_OK;
}

DRESULT disk_write (BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count){
    //typeconvert start address
    DWORD addr = (DWORD) sector;

    //check count is valid
    if(!count) { return RES_PARERR; }

    //check card initialization status
    if(card_status & STA_NOINIT) { return RES_NOTRDY; }

    if(count == 1) {
        if(sd_single_write(addr, buff) != 0x05) { return RES_ERROR; }
    } else {
        if(sd_multiple_write(addr, buff, (uint32_t) count) != 0x05) { return RES_ERROR; }
    }

    return RES_OK;
}

DRESULT disk_ioctl (BYTE pdrv, BYTE cmd, void* buff){

}