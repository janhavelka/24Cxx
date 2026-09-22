/*
© [2022] Microchip Technology Inc. and its subsidiaries.

    Subject to your compliance with these terms, you may use Microchip 
    software and any derivatives exclusively with Microchip products. 
    You are responsible for complying with 3rd party license terms  
    applicable to your use of 3rd party software (including open source  
    software) that may accompany Microchip software. SOFTWARE IS ?AS IS.? 
    NO WARRANTIES, WHETHER EXPRESS, IMPLIED OR STATUTORY, APPLY TO THIS 
    SOFTWARE, INCLUDING ANY IMPLIED WARRANTIES OF NON-INFRINGEMENT,  
    MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE. IN NO EVENT 
    WILL MICROCHIP BE LIABLE FOR ANY INDIRECT, SPECIAL, PUNITIVE, 
    INCIDENTAL OR CONSEQUENTIAL LOSS, DAMAGE, COST OR EXPENSE OF ANY 
    KIND WHATSOEVER RELATED TO THE SOFTWARE, HOWEVER CAUSED, EVEN IF 
    MICROCHIP HAS BEEN ADVISED OF THE POSSIBILITY OR THE DAMAGES ARE 
    FORESEEABLE. TO THE FULLEST EXTENT ALLOWED BY LAW, MICROCHIP?S 
    TOTAL LIABILITY ON ALL CLAIMS RELATED TO THE SOFTWARE WILL NOT 
    EXCEED AMOUNT OF FEES, IF ANY, YOU PAID DIRECTLY TO MICROCHIP FOR 
    THIS SOFTWARE.
*/

#include "./../mcc_generated_files/system/clock.h"
#include "./../mcc_generated_files/i2c_host/i2c1.h"
#include "./../mcc_generated_files/i2c_host/i2c_host_types.h"
#include "./../mcc_generated_files/uart/uart1.h"

#define FCY             CLOCK_SystemFrequencyGet()

#include <libpic30.h>
#include <stdbool.h>
#include <stdio.h>

#include "eeprom_24c08.h"
#include "eeprom_24c08_config.h"
#include "eeprom_24c08_types.h"


//Status flags
static bool i2cHostErrorNoneFlag = false;
static bool i2cHostNackFlag = false;
static bool i2cHostBusCollisionFlag = false;

void I2C_BusErrorCallback(void);
static bool EEPROM_ErrorHandler(void);
static void I2C_BusErrorClear(void);

void I2C_EEPROM_Initialize(void)
{
    I2C_Host.HostCallbackRegister(&I2C_BusErrorCallback);
}

enum I2C_EEPROM_WRITE_STATUS I2C_EEPROM_PageWrite(struct I2C_EEPROM_WRITE_BUFFER *writeBuffer)
{ 
    enum I2C_EEPROM_WRITE_STATUS writeStatus = EEPROM_WRITE_SUCCESS;
    
#if EEPROM_DEBUG_MODE == 1U
    uint8_t addressOffset = 0;
    
    printf("* Address: 0x%x\tData: ",writeBuffer->startAddress[0]);
    
    while(addressOffset < EEPROM_PAGE_SIZE)
    {
        printf("0x%x ", writeBuffer->data[addressOffset]);
        addressOffset++;
    }
#endif    
    //Write page to EEPROM
    if(I2C_Host.Write(EEPROM_CLIENT_BLOCK_ADDRESS, (uint8_t*)writeBuffer, (EEPROM_ADDRESS_SIZE + EEPROM_PAGE_SIZE)) == false)
    {
        writeStatus = EEPROM_WRITE_FAIL;
    }
    //EEPROM internal page write delay 
    __delay_ms(1);
    if(EEPROM_ErrorHandler() == true)
    {
        writeStatus = EEPROM_WRITE_CONNECTION_FAIL;
    }
#if EEPROM_DEBUG_MODE == 1U
    if(writeStatus != EEPROM_WRITE_SUCCESS)
    {
        printf("\r\n***ERROR WRITING EEPROM***\r\n");
    }
#endif
    return writeStatus;
}

enum I2C_EEPROM_READ_STATUS I2C_EEPROM_MultiPageRead(uint8_t address, uint8_t *readBuffer, uint8_t numPages)
{
    uint8_t eepromAddress;
    uint8_t *readBufferOffset;
    
    //for iteration purposes
    uint8_t pageOffset;
    uint8_t addressOffset;
    
    enum I2C_EEPROM_READ_STATUS readStatus = EEPROM_READ_SUCCESS;
    for(pageOffset=0;pageOffset<numPages;pageOffset++)
    {
        eepromAddress = address + (EEPROM_PAGE_SIZE * pageOffset);
        readBufferOffset = readBuffer + (EEPROM_PAGE_SIZE * pageOffset);
        //Read page from EEPROM
        if(I2C_Host.WriteRead(EEPROM_CLIENT_BLOCK_ADDRESS, &eepromAddress, EEPROM_ADDRESS_SIZE, readBufferOffset, EEPROM_PAGE_SIZE) == false)
        {
#if EEPROM_DEBUG_MODE == 1U
            printf("\r\n***ERROR READING EEPROM***\r\n");
#endif
            readStatus = EEPROM_READ_FAIL;
            break;
        }

#if EEPROM_DEBUG_MODE == 1U        
        printf("* Address: 0x%x\tData: ",eepromAddress);
        
        for(addressOffset=0;addressOffset<EEPROM_PAGE_SIZE;addressOffset++)
        {
             printf("0x%x ", readBufferOffset[addressOffset]);

        }
        printf("\r\n");
#endif
    }
    if(EEPROM_ErrorHandler() == true)
    {
        readStatus = EEPROM_READ_CONNECTION_FAIL;
    }
#if EEPROM_DEBUG_MODE == 1U
    if(readStatus != EEPROM_READ_SUCCESS)
    {
        printf("\r\n***ERROR WRITING EEPROM***\r\n");
    }
#endif
    return readStatus;
}

static bool EEPROM_ErrorHandler(void)
{
    if((i2cHostErrorNoneFlag != true) && (i2cHostNackFlag == false) && (i2cHostBusCollisionFlag == false))
    {
        I2C_BusErrorClear();
        return true;
    }
    
    return false;
}

void I2C_BusErrorCallback(void)
{
    enum I2C_HOST_ERROR status = I2C1_ErrorGet();
    
    if(status == I2C_HOST_ERROR_NONE)
    {
        i2cHostErrorNoneFlag = true;
    }
    else if(status == I2C_HOST_ERROR_NACK)
    {
        i2cHostNackFlag = true;
    }
    else if(status == I2C_HOST_ERROR_BUS_COLLISION)
    {
        i2cHostBusCollisionFlag = true;
    }
}

static void I2C_BusErrorClear(void)
{
    i2cHostErrorNoneFlag = false;
    i2cHostNackFlag = false;
    i2cHostBusCollisionFlag = false;
}