/*
 * Copyright 2010-2014,2018-2020,2022 NXP
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * DAL i2c port implementation for linux
 *
 * Project: Trusted ESE Linux
 *
 */
#include <phNxpEsePal_i2c.h>
#include <phEseStatus.h>
#include "sm_i2c.h"
#include "sm_timer.h"
#include "sm_port.h"

#define MAX_RETRY_CNT 10

/*******************************************************************************
**
** Function         phPalEse_i2c_close
**
** Description      Closes PN547 device
**
** param[in]        pDevHandle - device handle
**
** Returns          None
**
*******************************************************************************/
void phPalEse_i2c_close(void *pDevHandle)
{
#ifdef Android
    if (NULL != pDevHandle) {
        close((intptr_t)pDevHandle);
    }
#endif
    axI2CTerm(pDevHandle, 0);
    pDevHandle = NULL;

    return;
}

/*******************************************************************************
**
** Function         phPalEse_i2c_open_and_configure
**
** Description      Open and configure pn547 device
**
** param[in]        pConfig     - hardware information
**
** Returns          ESE status:
**                  ESESTATUS_SUCCESS            - open_and_configure operation success
**                  ESESTATUS_INVALID_DEVICE     - device open operation failure
**
*******************************************************************************/
ESESTATUS phPalEse_i2c_open_and_configure(pphPalEse_Config_t pConfig)
{
    void *conn_ctx       = NULL;
    int retryCnt         = 0;
    unsigned int i2c_ret = 0;

    T_SMLOG_D("%s Opening port", __FUNCTION__);
    /* open port */
    /*Disable as interface reset happens on every session open*/
    //se05x_ic_reset();
retry:
    i2c_ret = axI2CInit(&conn_ctx, (const char *)pConfig->pDevName);
    if (i2c_ret != I2C_OK) {
        T_SMLOG_E("%s Failed retry ", __FUNCTION__);
        if (i2c_ret == I2C_BUSY) {
            retryCnt++;
            T_SMLOG_E("Retry open eSE driver, retry cnt : %d ", retryCnt);
            if (retryCnt < MAX_RETRY_CNT) {
                sm_sleep(ESE_POLL_DELAY_MS);
                goto retry;
            }
        }
        T_SMLOG_E("I2C init Failed: retval %x ", i2c_ret);
        pConfig->pDevHandle = NULL;
        return ESESTATUS_INVALID_DEVICE;
    }
    T_SMLOG_D("I2C driver Initialized :: fd = [%d] ", i2c_ret);
    pConfig->pDevHandle = conn_ctx;
    return ESESTATUS_SUCCESS;
}

/*******************************************************************************
**
** Function         phPalEse_i2c_read
**
** Description      Reads requested number of bytes from pn547 device into given buffer
**
** param[in]       pDevHandle       - valid device handle
** param[in]       pBuffer          - buffer for read data
** param[in]       nNbBytesToRead   - number of bytes requested to be read
**
** Returns          numRead   - number of successfully read bytes
**                  -1        - read operation failure
**
*******************************************************************************/
int phPalEse_i2c_read(void *pDevHandle, uint8_t *pBuffer, int nNbBytesToRead)
{
    unsigned int ret = 0;
    int retryCount   = 0;
    int numRead      = 0;
    T_SMLOG_D("%s Read Requested %d bytes ", __FUNCTION__, nNbBytesToRead);
    //sm_sleep(ESE_POLL_DELAY_MS);
    while (numRead != nNbBytesToRead) {
        ret = axI2CRead(pDevHandle, I2C_BUS_0, SMCOM_I2C_ADDRESS, pBuffer, nNbBytesToRead);
        if (ret != I2C_OK) {
            T_SMLOG_D("_i2c_read() error : %d ", ret);
            /* if platform returns different error codes, modify the check below.*/
            /* Also adjust the retry count based on the platform */
#ifdef T1OI2C_RETRY_ON_I2C_FAILED
            if (((ret == I2C_FAILED) || (ret == I2C_NACK_ON_ADDRESS)) && (retryCount < MAX_RETRY_COUNT)) {
#else
            if ((ret == I2C_NACK_ON_ADDRESS) && (retryCount < MAX_RETRY_COUNT)) {
#endif
                retryCount++;
                /* 1ms delay to give ESE polling delay */
                /*i2c driver back off delay is providing 1ms wait time so ignoring waiting time at this level*/
#ifdef T1OI2C_RETRY_ON_I2C_FAILED
                sm_sleep(ESE_POLL_DELAY_MS);
#endif
                T_SMLOG_D("_i2c_read() failed. Going to retry, counter:%d  !", retryCount);
                continue;
            }
            return -1;
        }
        else {
            numRead = nNbBytesToRead;
            break;
        }
    }
    return numRead;
}

/*******************************************************************************
**
** Function         phPalEse_i2c_write
**
** Description      Writes requested number of bytes from given buffer into pn547 device
**
** param[in]       pDevHandle       - valid device handle
** param[in]       pBuffer          - buffer for read data
** param[in]       nNbBytesToWrite  - number of bytes requested to be written
**
** Returns          numWrote   - number of successfully written bytes
**                  -1         - write operation failure
**
*******************************************************************************/
int phPalEse_i2c_write(void *pDevHandle, uint8_t *pBuffer, int nNbBytesToWrite)
{
    unsigned int ret = I2C_OK, retryCount = 0;
    int numWrote = 0;
    pBuffer[0]   = 0x5A; //Recovery if stack forgot to add NAD byte.
    do {
        /* 1ms delay to give ESE polling delay */
        sm_sleep(ESE_POLL_DELAY_MS);
        ret = axI2CWrite(pDevHandle, I2C_BUS_0, SMCOM_I2C_ADDRESS, pBuffer, nNbBytesToWrite);
        if (ret != I2C_OK) {
            T_SMLOG_D("_i2c_write() error : %d ", ret);
            if ((ret == I2C_NACK_ON_ADDRESS) && (retryCount < MAX_RETRY_COUNT)) {
                retryCount++;
                /* 1ms delay to give ESE polling delay */
                /*i2c driver back off delay is providing 1ms wait time so ignoring waiting time at this level*/
                //sm_sleep(ESE_POLL_DELAY_MS);
                T_SMLOG_D("_i2c_write() failed. Going to retry, counter:%d  !", retryCount);
                continue;
            }
            return -1;
        }
        else {
            numWrote = nNbBytesToWrite;
            //sm_sleep(ESE_POLL_DELAY_MS);
            break;
        }
    } while (ret != I2C_OK);
    return numWrote;
}
