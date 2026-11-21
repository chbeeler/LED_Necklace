/*
 * Copyright (c) 2024, Texas Instruments Incorporated
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * *  Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * *  Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * *  Neither the name of Texas Instruments Incorporated nor the names of
 *    its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 *  ============ boot_config.c =============
 *  Configured MSPM0 NONMAIN options
 */

#include "boot_config.h"
#include <flashBSL_defines.h>

/* Ordinary constants in main flash; never program NONMAIN. */
const BCR_Config BCRConfig = {.bcrConfigID = 3};
const BSL_Config BSLConfig = {
#ifdef AEQ_LIGHT
    /* UART Get Identity: "AQL1" identifies this light protocol and layout. */
    .configID = 0x314c5141U,
#else
    .configID = 0x3,
#endif

    .interfacePins.UART_RXD_pad_num    = DEF_UART_RXD_PAD,
    .interfacePins.UART_RXD_PF_mux_sel = DEF_UART_RXD_MUX,
    .interfacePins.UART_TXD_pad_num    = DEF_UART_TXD_PAD,
    .interfacePins.UART_TXD_PF_mux_sel = DEF_UART_TXD_MUX,

    .interfacePins.I2C_SDA_pad_num    = DEF_I2C_SDA_PAD,
    .interfacePins.I2C_SDA_PF_mux_sel = DEF_I2C_SDA_MUX,
    .interfacePins.I2C_SCL_pad_num    = DEF_I2C_SCL_PAD,
    .interfacePins.I2C_SCL_PF_mux_sel = DEF_I2C_SCL_MUX,
    .pin.pinData0                     = 17U /* PINCM17, active low */,
    .pin.pinData1                     = 16U /* PA16, LaunchPad S2 */,
    .memoryRead                       = BSL_CFG_MEMORY_READOUT_ENABLE,
    .password = {CFG_DEFAULT_VALUE, CFG_DEFAULT_VALUE, CFG_DEFAULT_VALUE,
        CFG_DEFAULT_VALUE, CFG_DEFAULT_VALUE, CFG_DEFAULT_VALUE,
        CFG_DEFAULT_VALUE, CFG_DEFAULT_VALUE},

    .appRev          = (uint32_t *) CFG_DEFAULT_VALUE,
    .bootloaderMode  = BSL_CFG_BOOTLOADER_MODE_EN,
    .uartBaudRate    = BSL_CFG_UART_BAUDRATE_9600,
    .i2cSlaveAddress = BSL_CFG_I2C_SLAVE_ADDRESS,
    .disableNRST     = BSL_CFG_NRST_ENABLE,
};
