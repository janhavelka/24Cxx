/**
 * @file      eeprom_24c08_types.h
 * 
 * @ingroup   eeprom_24c08
 * 
 * @brief     This is the data type definition file for 24C08 EEPROM
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

#ifndef EEPROM_24C08_TYPES_H
#define	EEPROM_24C08_TYPES_H

#include "eeprom_24c08_config.h"

/**
 * @ingroup     eeprom_24c08
 * @enum        EEPROM_WRITE_STATUS
 * @brief       This enum can be used to know the EEPROM Write Status. 
*/
enum I2C_EEPROM_WRITE_STATUS
{
    EEPROM_WRITE_SUCCESS,              /**< No Error */
    EEPROM_WRITE_FAIL,                 /**< EEPROM address write failed */
    EEPROM_WRITE_CONNECTION_FAIL       /**< Failed to connect EEPROM */
};

/**
 * @ingroup     eeprom_24c08
 * @enum        EEPROM_WRITE_STATUS
 * @brief       This enum can be used to know the EEPROM read Status. 
*/
enum I2C_EEPROM_READ_STATUS
{
    EEPROM_READ_SUCCESS,              /**< No Error */
    EEPROM_READ_FAIL,                 /**< Failed to connect EEPROM */
    EEPROM_READ_CONNECTION_FAIL       /**< Failed to connect EEPROM */
};

/**
 * @ingroup     eeprom_24c08
 * @enum        EEPROM_WRITE_BUFFER
 * @brief       This union can be used to load the page data that has to be written to EEPROM 
*/
struct I2C_EEPROM_WRITE_BUFFER{
    uint8_t startAddress[EEPROM_ADDRESS_SIZE];
    uint8_t data[EEPROM_PAGE_SIZE];
};

#endif	/* EEPROM_24C08_TYPES_H */

