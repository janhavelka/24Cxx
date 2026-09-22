/**
 * @file      eeprom_24c08_config.h
 * 
 * @defgroup  eeprom_24c08
 * 
 * @brief     This is the configuration file for 24C08 EEPROM
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
 
#ifndef EEPROM_24C08_CONFIG_H
#define	EEPROM_24C08_CONFIG_H

//24C08 EEPROM config
#define EEPROM_NODE_ADDRESS     0x50U
#define EEPROM_BLOCK_ADDRESS    0x1U // 0 - 3 for 24C08 and 0 - 7 for 24C016
#define EEPROM_ADDRESS_SIZE     1U
#define EEPROM_PAGE_SIZE        16U
#define EEPROM_BLOCK_SIZE       256U
#define EEPROM_DEBUG_MODE       1U // 1U : to enable printing debug messages 

#endif	/* EEPROM_24C08_CONFIG_H */

