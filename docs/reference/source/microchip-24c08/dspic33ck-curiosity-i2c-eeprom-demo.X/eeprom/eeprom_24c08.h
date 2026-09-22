/**
 * @file      eeprom_24c08.h
 * 
 * @ingroup   eeprom_24c08
 * 
 * @brief     This is the function header file for 24C08 EEPROM functions
*/

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

#ifndef EEPROM_24C08_H
#define	EEPROM_24C08_H

#include <stdint.h>
#include "eeprom_24c08_config.h"
#include "eeprom_24c08_types.h"

//EEPROM macros
#define EEPROM_CLIENT_BLOCK_ADDRESS (EEPROM_NODE_ADDRESS | ((EEPROM_BLOCK_ADDRESS)<<1))
#define EEPROM_MAX_PAGES ((EEPROM_BLOCK_SIZE)/(EEPROM_PAGE_SIZE))

/**
 * @ingroup     eeprom_24c08
 * @brief       This function initializes the I2C Host for communicating with EEPROM 
 * @param       none
 * @return      none
 */
void I2C_EEPROM_Initialize(void);

/**
 * @ingroup     eeprom_24c08
 * @brief       This function writes the page data to EEPROM 
 * @param[in]   writeBuffer - write buffer of size equal to \ref EEPROM_PAGE_SIZE + \ref EEPROM_ADDRESS_SIZE
 * @return      EEPROM_WRITE_SUCCESS - No Error 
 * @return      EEPROM_WRITE_FAIL - EEPROM address write failed 
 * @return      EEPROM_WRITE_CONNECTION_FAIL - Failed to connect EEPROM 
 */
enum I2C_EEPROM_WRITE_STATUS I2C_EEPROM_PageWrite(struct I2C_EEPROM_WRITE_BUFFER *writeBuffer);

/**
 * @ingroup     eeprom_24c08
 * @brief       This function reads multiple page from EEPROM 
 * @param[in]   startAddress - start address of the EEPROM memory 
 * @param[out]  readBuffer - pointer to read buffer of size \ref numPages times \ref EEPROM_PAGE_SIZE
 * @param[in]   numPages - number of pages to be read
 * @return      EEPROM_READ_SUCCESS - No Error
 * @return      EEPROM_READ_FAIL - Failed to connect EEPROM
 * @return      EEPROM_READ_CONNECTION_FAIL - Failed to connect EEPROM
 */
enum I2C_EEPROM_READ_STATUS I2C_EEPROM_MultiPageRead(uint8_t startAddress, uint8_t *readBuffer, uint8_t numPages);

#endif	/* EEPROM_24C08_H */

