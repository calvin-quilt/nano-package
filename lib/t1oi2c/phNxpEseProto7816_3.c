/*
 * Copyright 2012-2014,2018-2020,2022,2024 NXP
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

#include <stdbool.h>
#include <phNxpEseProto7816_3.h>
#include <phNxpEsePal_i2c.h>
#include <phEseTypes.h>
#include "sm_timer.h"
#include "se05x_tlv.h"
#include "sm_port.h"
#include <limits.h>

/**
 * \addtogroup ISO7816-3_protocol_lib
 *
 * @{ */

phNxpEseProto7816_t phNxpEseProto7816_3_Var;

/******************************************************************************
\section Introduction Introduction

 * This module provide the 7816-3 protocol level implementation for ESE
 *
 ******************************************************************************/
static bool_t phNxpEseProto7816_SendRawFrame(void *conn_ctx, uint32_t data_len, uint8_t *p_data);
static bool_t phNxpEseProto7816_GetRawFrame(void *conn_ctx, uint32_t *data_len, uint8_t **pp_data);
static uint16_t phNxpEseProto7816_ComputeCRC(unsigned char *p_buff, uint32_t offset, uint32_t length);
static bool_t phNxpEseProto7816_CheckCRC(uint32_t data_len, uint8_t *p_data);
static bool_t phNxpEseProto7816_SendSFrame(void *conn_ctx, sFrameInfo_t sFrameData);
static bool_t phNxpEseProto7816_SendIframe(void *conn_ctx, iFrameInfo_t iFrameData);
static bool_t phNxpEseProto7816_sendRframe(void *conn_ctx, rFrameTypes_t rFrameType);
static bool_t phNxpEseProto7816_SetFirstIframeContxt(void);
static bool_t phNxpEseProto7816_SetNextIframeContxt(void);
static bool_t phNxpEseProro7816_SaveRxframeData(uint8_t *p_data, uint32_t data_len);
static bool_t phNxpEseProto7816_ResetRecovery(void);
static bool_t phNxpEseProto7816_RecoverySteps(void);
static bool_t phNxpEseProto7816_DecodeFrame(uint8_t *p_data, uint32_t data_len);
static bool_t phNxpEseProto7816_ProcessResponse(void *conn_ctx);
static bool_t TransceiveProcess(void *conn_ctx);
static bool_t phNxpEseProto7816_RSync(void *conn_ctx);

/******************************************************************************
 * Function         phNxpEseProto7816_SendRawFrame
 *
 * Description      This internal function is called send the data to ESE
 *
 * param[in]        uint32_t: number of bytes to be written
 * param[in]        uint8_t : data buffer
 *
 * Returns          On success return TRUE or else FALSE.
 *
 ******************************************************************************/
static bool_t phNxpEseProto7816_SendRawFrame(void *conn_ctx, uint32_t data_len, uint8_t *p_data)
{
    ESESTATUS status = ESESTATUS_FAILED;
    status           = phNxpEse_WriteFrame(conn_ctx, data_len, p_data);
    if (ESESTATUS_SUCCESS != status) {
        T_SMLOG_E("%s Error phNxpEse_WriteFrame ", __FUNCTION__);
    }

    return (status == ESESTATUS_SUCCESS) ? TRUE : FALSE;
}

/******************************************************************************
 * Function         phNxpEseProto7816_GetRawFrame
 *
 * Description      This internal function is called read the data from the ESE
 *
 * param[out]        uint32_t: number of bytes read
 * param[out]        uint8_t : Read data from ESE
 *
 * Returns          On success return TRUE or else FALSE.
 *
 ******************************************************************************/
static bool_t phNxpEseProto7816_GetRawFrame(void *conn_ctx, uint32_t *data_len, uint8_t **pp_data)
{
    bool_t bStatus   = FALSE;
    ESESTATUS status = ESESTATUS_FAILED;

    status = phNxpEse_read(conn_ctx, data_len, pp_data);
    if (ESESTATUS_SUCCESS != status) {
        T_SMLOG_E("%s phNxpEse_read failed , status : 0x%x ", __FUNCTION__, status);
    }
    else {
        bStatus = TRUE;
    }
    return bStatus;
}

/******************************************************************************
 * Function         phNxpEseProto7816_ComputeCRC
 *
 * Description      This internal function is called compute the CRC
 *
 * param[in]        unsigned char: data buffer
 * param[in]        uint32_t : offset from which CRC to be calculated
 * param[in]        uint32_t : total length of frame
 *
 * Returns          On success return TRUE or else FALSE.
 *
 ******************************************************************************/
static uint16_t phNxpEseProto7816_ComputeCRC(unsigned char *p_buff, uint32_t offset, uint32_t length)
{
    uint16_t CAL_CRC = 0xFFFF, CRC = 0x0000;
    uint32_t i = 0;

    ENSURE_OR_GO_EXIT(p_buff != NULL);

    for (i = offset; i < length; i++) {
        CAL_CRC ^= p_buff[i];
        for (int bit = 8; bit > 0; --bit) {
            if ((CAL_CRC & 0x0001) == 0x0001) {
                CAL_CRC = (unsigned short)((CAL_CRC >> 1) ^ 0x8408);
            }
            else {
                CAL_CRC >>= 1;
            }
        }
    }
    CAL_CRC ^= 0xFFFF;
#if defined(T1oI2C_UM11225)
    CRC = ((CAL_CRC & 0xFF) << 8) | ((CAL_CRC >> 8) & 0xFF);
#elif defined(T1oI2C_GP1_0)
    CRC = CAL_CRC;
#endif
exit:
    return (uint16_t)CRC;
}

/******************************************************************************
 * Function         phNxpEseProto7816_CheckCRC
 *
 * Description      This internal function is called compute and compare the
 *                  received CRC of the received data
 *
 * param[in]        uint32_t : frame length
 * param[in]        uint8_t: data buffer
 *
 * Returns          On success return TRUE or else FALSE.
 *
 ******************************************************************************/
static bool_t phNxpEseProto7816_CheckCRC(uint32_t data_len, uint8_t *p_data)
{
    bool_t status     = FALSE;
    uint16_t calc_crc = 0;
    uint16_t recv_crc = 0;

    ENSURE_OR_GO_EXIT(p_data != NULL);
    ENSURE_OR_GO_EXIT(data_len < MAX_APDU_BUFFER);
    ENSURE_OR_GO_EXIT(data_len >= 2);

    status = TRUE;

    recv_crc = p_data[data_len - 2] << 8 | p_data[data_len - 1]; //combine 2 byte CRC

    /* calculate the CRC after excluding Recieved CRC  */
    /* CRC calculation includes NAD byte, so offset is set to 0 */
    calc_crc = phNxpEseProto7816_ComputeCRC(p_data, 0, (data_len - 2));
    T_SMLOG_D("Received CRC:0x%x Calculated CRC:0x%x ", recv_crc, calc_crc);
    if (recv_crc != calc_crc) {
        status = FALSE;
        T_SMLOG_E("%s CRC failed ", __FUNCTION__);
    }
exit:
    return status;
}

/******************************************************************************
 * Function         getMaxSupportedSendIFrameSize
 *
 * Description      This internal function is called to get the max supported
 *                  I-frame size
 *
 * param[in]        void
 *
 * Returns          IFSC_SIZE_SEND
 *
 ******************************************************************************/
uint8_t getMaxSupportedSendIFrameSize(void)
{
    return IFSC_SIZE_SEND;
}

/******************************************************************************
 * Function         phNxpEseProto7816_SendSFrame
 *
 * Description      This internal function is called to send S-frame with all
 *                   updated 7816-3 headers
 *
 * param[in]        sFrameInfo_t: Info about S frame
 *
 * Returns          On success return TRUE or else FALSE.
 *
 ******************************************************************************/
static bool_t phNxpEseProto7816_SendSFrame(void *conn_ctx, sFrameInfo_t sFrameData)
{
    bool_t status           = ESESTATUS_FAILED;
    uint32_t frame_len      = 0;
    uint8_t p_framebuff[7]  = {0};
    uint8_t pcb_byte        = 0;
    sFrameInfo_t sframeData = sFrameData;
    uint16_t calc_crc       = 0;
    /* This update is helpful in-case a R-NACK is transmitted from the MW */
    phNxpEseProto7816_3_Var.lastSentNonErrorframeType = SFRAME;
    switch (sframeData.sFrameType) {
    case RESYNCH_REQ:
        frame_len                                    = (PH_PROTO_7816_HEADER_LEN + PH_PROTO_7816_CRC_LEN);
        p_framebuff[PH_PROPTO_7816_LEN_UPPER_OFFSET] = 0;
#if defined(T1oI2C_GP1_0)
        /* T =1 GP block format LEN field is of 2 byte*/
        p_framebuff[PH_PROPTO_7816_LEN_LOWER_OFFSET] = 0;
#endif
        p_framebuff[PH_PROPTO_7816_INF_BYTE_OFFSET] = 0x00;

        pcb_byte |= PH_PROTO_7816_S_BLOCK_REQ; /* PCB */
        pcb_byte |= PH_PROTO_7816_S_RESYNCH;
        break;
#if defined(T1oI2C_UM11225)
    case INTF_RESET_REQ:
        frame_len                                    = (PH_PROTO_7816_HEADER_LEN + PH_PROTO_7816_CRC_LEN);
        p_framebuff[PH_PROPTO_7816_LEN_UPPER_OFFSET] = 0;
        p_framebuff[PH_PROPTO_7816_INF_BYTE_OFFSET]  = 0x00;

        pcb_byte |= PH_PROTO_7816_S_BLOCK_REQ; /* PCB */
        pcb_byte |= PH_PROTO_7816_S_RESET;
        break;
    case PROP_END_APDU_REQ:
        frame_len                                    = (PH_PROTO_7816_HEADER_LEN + PH_PROTO_7816_CRC_LEN);
        p_framebuff[PH_PROPTO_7816_LEN_UPPER_OFFSET] = 0;
        p_framebuff[PH_PROPTO_7816_INF_BYTE_OFFSET]  = 0x00;

        pcb_byte |= PH_PROTO_7816_S_BLOCK_REQ; /* PCB */
        pcb_byte |= PH_PROTO_7816_S_END_OF_APDU;
        break;
    case ATR_REQ:
        frame_len                                    = (PH_PROTO_7816_HEADER_LEN + PH_PROTO_7816_CRC_LEN);
        p_framebuff[PH_PROPTO_7816_LEN_UPPER_OFFSET] = 0;
        p_framebuff[PH_PROPTO_7816_INF_BYTE_OFFSET]  = 0x00;

        pcb_byte |= PH_PROTO_7816_S_BLOCK_REQ; /* PCB */
        pcb_byte |= PH_PROTO_7816_S_GET_ATR;
        break;
#endif
    case DEEP_PWR_DOWN_REQ:
        frame_len                                    = (PH_PROTO_7816_HEADER_LEN + PH_PROTO_7816_CRC_LEN);
        p_framebuff[PH_PROPTO_7816_LEN_UPPER_OFFSET] = 0;
        p_framebuff[PH_PROPTO_7816_INF_BYTE_OFFSET]  = 0x00;

        pcb_byte |= PH_PROTO_7816_S_BLOCK_REQ; /* PCB */
        pcb_byte |= PH_PROTO_7816_S_DEEP_PWR_DOWN;
        break;
    case WTX_RSP:
        frame_len = (PH_PROTO_7816_HEADER_LEN + 1 + PH_PROTO_7816_CRC_LEN);
#if defined(T1oI2C_UM11225)
        /* T =1 UM11225 SE050 block format LEN field is of 2 byte*/
        p_framebuff[PH_PROPTO_7816_LEN_UPPER_OFFSET] = 0x01;
#elif defined(T1oI2C_GP1_0)
        /* T =1 GP block format LEN field is of 2 byte*/
        p_framebuff[PH_PROPTO_7816_LEN_UPPER_OFFSET] = 0x00;
        p_framebuff[PH_PROPTO_7816_LEN_LOWER_OFFSET] = 0x01;
#endif
        p_framebuff[PH_PROPTO_7816_INF_BYTE_OFFSET] = 0x01;

        pcb_byte |= PH_PROTO_7816_S_BLOCK_RSP;
        pcb_byte |= PH_PROTO_7816_S_WTX;
        break;
#if defined(T1oI2C_UM11225)
    case CHIP_RESET_REQ:
        frame_len                                    = (PH_PROTO_7816_HEADER_LEN + PH_PROTO_7816_CRC_LEN);
        p_framebuff[PH_PROPTO_7816_LEN_UPPER_OFFSET] = 0;
        p_framebuff[PH_PROPTO_7816_INF_BYTE_OFFSET]  = 0x00;

        pcb_byte |= PH_PROTO_7816_S_BLOCK_REQ; /* PCB */
        pcb_byte |= PH_PROTO_7816_S_CHIP_RST;
        break;
#endif
#if defined(T1oI2C_GP1_0)
    case SWR_REQ:
        frame_len                                    = (PH_PROTO_7816_HEADER_LEN + PH_PROTO_7816_CRC_LEN);
        p_framebuff[PH_PROPTO_7816_LEN_UPPER_OFFSET] = 0;
        p_framebuff[PH_PROPTO_7816_LEN_LOWER_OFFSET] = 0;
        p_framebuff[PH_PROPTO_7816_INF_BYTE_OFFSET]  = 0x00;

        pcb_byte |= PH_PROTO_7816_S_BLOCK_REQ; /* PCB */
        pcb_byte |= PH_PROTO_7816_S_SWR;
        break;
    case RELEASE_REQ:
        frame_len                                    = (PH_PROTO_7816_HEADER_LEN + PH_PROTO_7816_CRC_LEN);
        p_framebuff[PH_PROPTO_7816_LEN_UPPER_OFFSET] = 0;
        p_framebuff[PH_PROPTO_7816_LEN_LOWER_OFFSET] = 0;
        p_framebuff[PH_PROPTO_7816_INF_BYTE_OFFSET]  = 0x00;

        pcb_byte |= PH_PROTO_7816_S_BLOCK_REQ; /* PCB */
        pcb_byte |= PH_PROTO_7816_S_RELEASE;
        break;
    case CIP_REQ:
        frame_len                                    = (PH_PROTO_7816_HEADER_LEN + PH_PROTO_7816_CRC_LEN);
        p_framebuff[PH_PROPTO_7816_LEN_UPPER_OFFSET] = 0;
        p_framebuff[PH_PROPTO_7816_LEN_LOWER_OFFSET] = 0;
        p_framebuff[PH_PROPTO_7816_INF_BYTE_OFFSET]  = 0x00;

        pcb_byte |= PH_PROTO_7816_S_BLOCK_REQ; /* PCB */
        pcb_byte |= PH_PROTO_7816_S_GET_CIP;
        break;
    case COLD_RESET_REQ:
        frame_len                                    = (PH_PROTO_7816_HEADER_LEN + PH_PROTO_7816_CRC_LEN);
        p_framebuff[PH_PROPTO_7816_LEN_UPPER_OFFSET] = 0;
        p_framebuff[PH_PROPTO_7816_LEN_LOWER_OFFSET] = 0;
        p_framebuff[PH_PROPTO_7816_INF_BYTE_OFFSET]  = 0x00;

        pcb_byte |= PH_PROTO_7816_S_BLOCK_REQ; /* PCB */
        pcb_byte |= PH_PROTO_7816_S_COLD_RST;
        break;
#endif
    default:
        T_SMLOG_E(" %s :Invalid S-block", __FUNCTION__);
        return status;
    }

    /* frame the packet */
    p_framebuff[PH_PROPTO_7816_NAD_OFFSET] = 0x5A;     /* NAD Byte */
    p_framebuff[PH_PROPTO_7816_PCB_OFFSET] = pcb_byte; /* PCB */

    calc_crc                   = phNxpEseProto7816_ComputeCRC(p_framebuff, 0, (frame_len - 2));
    p_framebuff[frame_len - 2] = (calc_crc >> 8) & 0xFF;
    p_framebuff[frame_len - 1] = calc_crc & 0xFF;
    T_SMLOG_D("S-Frame PCB: %x ", p_framebuff[PH_PROPTO_7816_PCB_OFFSET]);
    status = phNxpEseProto7816_SendRawFrame(conn_ctx, frame_len, p_framebuff);
    return status;
}

/******************************************************************************
 * Function         phNxpEseProto7816_sendRframe
 *
 * Description      This internal function is called to send R-frame with all
 *                   updated 7816-3 headers
 *
 * param[in]        sFrameInfo_t: Info about R frame
 *
 * Returns          On success return TRUE or else FALSE.
 *
 ******************************************************************************/
static bool_t phNxpEseProto7816_sendRframe(void *conn_ctx, rFrameTypes_t rFrameType)
{
    bool_t status = FALSE;
#if defined(T1oI2C_UM11225)
    uint8_t recv_ack[5] = {0x5A, 0x80, 0x00, 0x00, 0x00};
#elif defined(T1oI2C_GP1_0)
    uint8_t recv_ack[6] = {0x5A, 0x80, 0x00, 0x00, 0x00, 0x00};
#endif
    uint16_t calc_crc                    = 0;
    iFrameInfo_t *pRx_lastRcvdIframeInfo = &phNxpEseProto7816_3_Var.phNxpEseRx_Cntx.lastRcvdIframeInfo;
    rFrameInfo_t *pNextTx_RframeInfo     = &phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.RframeInfo;
    if (RNACK == rFrameType) /* R-NACK */
    {
        switch (pNextTx_RframeInfo->errCode) {
        case PARITY_ERROR:
            recv_ack[PH_PROPTO_7816_PCB_OFFSET] |= (0x01 & 0xFF);
            break;

        case OTHER_ERROR:
            recv_ack[PH_PROPTO_7816_PCB_OFFSET] |= (0x02 & 0xFF);
            break;

        case SOF_MISSED_ERROR:
        case UNDEFINED_ERROR:
            recv_ack[PH_PROPTO_7816_PCB_OFFSET] |= (0x03 & 0xFF);
            break;

        default:
            break;
        }
    }
    else /* R-ACK*/
    {
        /* This update is helpful in-case a R-NACK is transmitted from the MW */
        phNxpEseProto7816_3_Var.lastSentNonErrorframeType = RFRAME;
    }

    recv_ack[PH_PROPTO_7816_PCB_OFFSET] |= ((pRx_lastRcvdIframeInfo->seqNo ^ 1) << 4);
    T_SMLOG_D("%s recv_ack[PH_PROPTO_7816_PCB_OFFSET]:0x%x ", __FUNCTION__, recv_ack[PH_PROPTO_7816_PCB_OFFSET]);
    calc_crc = phNxpEseProto7816_ComputeCRC(recv_ack, 0x00, (sizeof(recv_ack) - 2));

    recv_ack[(sizeof(recv_ack) - 2)] = (calc_crc >> 8) & 0xFF;
    recv_ack[(sizeof(recv_ack) - 1)] = calc_crc & 0xFF;
    status                           = phNxpEseProto7816_SendRawFrame(conn_ctx, sizeof(recv_ack), recv_ack);
    return status;
}

/******************************************************************************
 * Function         phNxpEseProto7816_SendIframe
 *
 * Description      This internal function is called to send I-frame with all
 *                   updated 7816-3 headers
 *
 * param[in]        sFrameInfo_t: Info about I frame
 *
 * Returns          On success return TRUE or else FALSE.
 *
 ******************************************************************************/
static bool_t phNxpEseProto7816_SendIframe(void *conn_ctx, iFrameInfo_t iFrameData)
{
    bool_t status      = FALSE;
    uint32_t frame_len = 0;
    uint8_t p_framebuff[MAX_APDU_BUFFER];
    uint8_t pcb_byte                 = 0;
    uint16_t calc_crc                = 0;
    iFrameInfo_t *pNextTx_IframeInfo = &phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.IframeInfo;

    if (0 == iFrameData.sendDataLen) {
        T_SMLOG_E("%s Line: [%d] I frame Len is 0, INVALID ", __FUNCTION__, __LINE__);
        return FALSE;
    }
    /* This update is helpful in-case a R-NACK is transmitted from the MW */
    phNxpEseProto7816_3_Var.lastSentNonErrorframeType = IFRAME;
    ENSURE_OR_GO_EXIT(iFrameData.sendDataLen <= (UINT_MAX - (PH_PROTO_7816_HEADER_LEN + PH_PROTO_7816_CRC_LEN)))
    frame_len = (iFrameData.sendDataLen + PH_PROTO_7816_HEADER_LEN + PH_PROTO_7816_CRC_LEN);

    /* frame the packet */
    p_framebuff[PH_PROPTO_7816_NAD_OFFSET] = SEND_PACKET_SOF; /* NAD Byte */

    if (iFrameData.isChained) {
        /* make B6 (M) bit high */
        pcb_byte |= PH_PROTO_7816_CHAINING;
    }

    /* Update the send seq no */
    pcb_byte |= (pNextTx_IframeInfo->seqNo << 6);

    /* store the pcb byte */
    p_framebuff[PH_PROPTO_7816_PCB_OFFSET] = pcb_byte;
#if defined(T1oI2C_UM11225)
    /* store I frame length */
    /* for T1oI2C_UM11225 LEN field is of 1 byte*/
    p_framebuff[PH_PROPTO_7816_LEN_UPPER_OFFSET] = iFrameData.sendDataLen;
#elif defined(T1oI2C_GP1_0)
    /* store I frame length */
    /* for T1oI2C_GP1_0 LEN field is of 2 byte*/
    p_framebuff[PH_PROPTO_7816_LEN_UPPER_OFFSET] = (((uint16_t)iFrameData.sendDataLen) >> 8 & 0xff);
    p_framebuff[PH_PROPTO_7816_LEN_LOWER_OFFSET] = (((uint16_t)iFrameData.sendDataLen) & 0xff);
#endif
    /* store I frame */
    ENSURE_OR_GO_EXIT(iFrameData.sendDataLen <= (MAX_APDU_BUFFER - PH_PROPTO_7816_INF_BYTE_OFFSET))
    ENSURE_OR_GO_EXIT(frame_len - 1 < MAX_APDU_BUFFER)

    phNxpEse_memcpy(&(p_framebuff[PH_PROPTO_7816_INF_BYTE_OFFSET]),
        iFrameData.p_data + iFrameData.dataOffset,
        iFrameData.sendDataLen);
    calc_crc = phNxpEseProto7816_ComputeCRC(p_framebuff, 0, (frame_len - 2));

    p_framebuff[frame_len - 2] = (calc_crc >> 8) & 0xff;
    p_framebuff[frame_len - 1] = calc_crc & 0xff;
    status                     = phNxpEseProto7816_SendRawFrame(conn_ctx, frame_len, p_framebuff);

exit:
    return status;
}

/******************************************************************************
 * Function         phNxpEseProto7816_SetFirstIframeContxt
 *
 * Description      This internal function is called to set the context for next I-frame.
 *                  Not applicable for the first I-frame of the transceive
 *
 * param[in]        void
 *
 * Returns          Always return TRUE.
 *
 ******************************************************************************/
static bool_t phNxpEseProto7816_SetFirstIframeContxt(void)
{
    phNxpEseRx_Cntx_t *pRx_EseCntx   = &phNxpEseProto7816_3_Var.phNxpEseRx_Cntx;
    iFrameInfo_t *pNextTx_IframeInfo = &phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.IframeInfo;
    iFrameInfo_t *pLastTx_IframeInfo = &phNxpEseProto7816_3_Var.phNxpEseLastTx_Cntx.IframeInfo;

    pNextTx_IframeInfo->dataOffset                                = 0;
    phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = IFRAME;
    pNextTx_IframeInfo->seqNo                                     = (uint8_t)(pLastTx_IframeInfo->seqNo ^ 1);
    phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = SEND_IFRAME;
    pRx_EseCntx->responseBytesRcvd                                = 0;
    if (pNextTx_IframeInfo->totalDataLen > pNextTx_IframeInfo->maxDataLen) {
        pNextTx_IframeInfo->isChained    = TRUE;
        pNextTx_IframeInfo->sendDataLen  = pNextTx_IframeInfo->maxDataLen;
        pNextTx_IframeInfo->totalDataLen = pNextTx_IframeInfo->totalDataLen - pNextTx_IframeInfo->maxDataLen;
    }
    else {
        pNextTx_IframeInfo->sendDataLen = pNextTx_IframeInfo->totalDataLen;
        pNextTx_IframeInfo->isChained   = FALSE;
    }
    T_SMLOG_D("I-Frame Data Len: %d Seq. no:%d ", pNextTx_IframeInfo->sendDataLen, pNextTx_IframeInfo->seqNo);
    return TRUE;
}

/******************************************************************************
 * Function         phNxpEseProto7816_SetNextIframeContxt
 *
 * Description      This internal function is called to set the context for next I-frame.
 *                  Not applicable for the first I-frame of the transceive
 *
 * param[in]        void
 *
 * Returns          Always return TRUE.
 *
 ******************************************************************************/
static bool_t phNxpEseProto7816_SetNextIframeContxt(void)
{
    iFrameInfo_t *pNextTx_IframeInfo = &phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.IframeInfo;
    iFrameInfo_t *pLastTx_IframeInfo = &phNxpEseProto7816_3_Var.phNxpEseLastTx_Cntx.IframeInfo;

    /* Expecting to reach here only after first of chained I-frame is sent and before the last chained is sent */
    phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = IFRAME;
    phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = SEND_IFRAME;

    pNextTx_IframeInfo->seqNo = (uint8_t)(pLastTx_IframeInfo->seqNo ^ 1);
    if ((UINT_MAX - pLastTx_IframeInfo->dataOffset) < pLastTx_IframeInfo->maxDataLen) {
        return FALSE;
    }
    pNextTx_IframeInfo->dataOffset = pLastTx_IframeInfo->dataOffset + pLastTx_IframeInfo->maxDataLen;
    pNextTx_IframeInfo->p_data     = pLastTx_IframeInfo->p_data;
    pNextTx_IframeInfo->maxDataLen = pLastTx_IframeInfo->maxDataLen;

    //if  chained
    if (pLastTx_IframeInfo->totalDataLen > pLastTx_IframeInfo->maxDataLen) {
        T_SMLOG_D("%s Process Chained Frame ", __FUNCTION__);
        pNextTx_IframeInfo->isChained    = TRUE;
        pNextTx_IframeInfo->sendDataLen  = pLastTx_IframeInfo->maxDataLen;
        pNextTx_IframeInfo->totalDataLen = pLastTx_IframeInfo->totalDataLen - pLastTx_IframeInfo->maxDataLen;
    }
    else {
        pNextTx_IframeInfo->isChained   = FALSE;
        pNextTx_IframeInfo->sendDataLen = pLastTx_IframeInfo->totalDataLen;
    }
    T_SMLOG_D("I-Frame Data Len: %d ", pNextTx_IframeInfo->sendDataLen);
    return TRUE;
}

/******************************************************************************
 * Function         phNxpEseProro7816_SaveRxframeData
 *
 * Description      This internal function is called to save recv frame data
 *
 * param[in]        uint8_t: data buffer
 * param[in]        uint32_t: buffer length
 *
 * Returns          Always return TRUE.
 *
 ******************************************************************************/
static bool_t phNxpEseProro7816_SaveRxframeData(uint8_t *p_data, uint32_t data_len)
{
    phNxpEseRx_Cntx_t *pRx_EseCntx = &phNxpEseProto7816_3_Var.phNxpEseRx_Cntx;

    if (p_data == NULL) {
        return FALSE;
    }
    T_SMLOG_D("Data[0]=0x%x len=%ld Data[%ld]=0x%x Data[%ld]=0x%x",
        p_data[0],
        data_len,
        data_len - 1,
        p_data[data_len - 2],
        data_len,
        p_data[data_len - 1]);
    if (pRx_EseCntx->pRsp != NULL) {
        if (pRx_EseCntx->pRsp->p_data == NULL) {
            return FALSE;
        }
        if (pRx_EseCntx->responseBytesRcvd > (UINT_MAX - data_len)) {
            return FALSE;
        }
        if ((pRx_EseCntx->responseBytesRcvd + data_len) > pRx_EseCntx->pRsp->len) {
            // LOG_W("Need '%ld' bytes. Got '%ld' to copy.", (size_t)(pRx_EseCntx->responseBytesRcvd + data_len), pRx_EseCntx->pRsp->len);
            return FALSE;
        }
        memmove((pRx_EseCntx->pRsp->p_data + pRx_EseCntx->responseBytesRcvd), p_data, data_len);
        pRx_EseCntx->responseBytesRcvd += data_len;
        return TRUE;
    }
    else {
        T_SMLOG_E("Unsolicited response");
        return FALSE;
    }
}

/******************************************************************************
 * Function         phNxpEseProto7816_ResetRecovery
 *
 * Description      This internal function is called to do reset the recovery pareameters
 *
 * param[in]        void
 *
 * Returns          Always return TRUE.
 *
 ******************************************************************************/
static bool_t phNxpEseProto7816_ResetRecovery(void)
{
    phNxpEseProto7816_3_Var.recoveryCounter = 0;
    return TRUE;
}

/******************************************************************************
 * Function         phNxpEseProto7816_RecoverySteps
 *
 * Description      This internal function is called when 7816-3 stack failed to recover
 *                  after PH_PROTO_7816_FRAME_RETRY_COUNT, and the interface has to be
 *                  recovered
 *
 * param[in]        void
 *
 * Returns          Always return TRUE.
 *
 ******************************************************************************/
static bool_t phNxpEseProto7816_RecoverySteps(void)
{
    sFrameInfo_t *pRx_lastRcvdSframeInfo = &phNxpEseProto7816_3_Var.phNxpEseRx_Cntx.lastRcvdSframeInfo;
    sFrameInfo_t *pNextTx_SframeInfo     = &phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.SframeInfo;

    if (phNxpEseProto7816_3_Var.recoveryCounter <= PH_PROTO_7816_FRAME_RETRY_COUNT) {
#if defined(T1oI2C_UM11225)
        pRx_lastRcvdSframeInfo->sFrameType                            = INTF_RESET_REQ;
        phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = SFRAME;
        pNextTx_SframeInfo->sFrameType                                = INTF_RESET_REQ;
        phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = SEND_S_INTF_RST;
#elif defined(T1oI2C_GP1_0)
        pRx_lastRcvdSframeInfo->sFrameType                            = SWR_REQ;
        phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = SFRAME;
        pNextTx_SframeInfo->sFrameType                                = SWR_REQ;
        phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = SEND_S_SWR;
#endif
    }
    else { /* If recovery fails */
        phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = IDLE_STATE;
    }
    return TRUE;
}

/******************************************************************************
 * Function         phNxpEseProto7816_DecodeSFrameData
 *
 * Description      This internal function is to decode S-frame payload.
 *
 * param[in]        uint8_t; data buffer
 *
 * Returns          void
 *
 ******************************************************************************/
static void phNxpEseProto7816_DecodeSFrameData(uint8_t *p_data)
{
    uint8_t maxSframeLen = 0, frameOffset = 0;

    ENSURE_OR_GO_EXIT(p_data != NULL);
#if defined(T1oI2C_UM11225)
    frameOffset = PH_PROPTO_7816_LEN_UPPER_OFFSET;
#elif defined(T1oI2C_GP1_0)
    /* current GP implementation support max payload of 0x00FE, so considering lower offset */
    frameOffset = PH_PROPTO_7816_LEN_LOWER_OFFSET;
#endif
    maxSframeLen = p_data[frameOffset] + frameOffset; /* to be in sync with offset which starts from index 0 */
    while (maxSframeLen > frameOffset) {
        frameOffset += 1; /* To get the Type (TLV) */
        T_SMLOG_D("%s frameoffset=%d value=0x%x ", __FUNCTION__, frameOffset, p_data[frameOffset]);
        frameOffset += p_data[frameOffset + 1]; /* Goto the end of current marker */
    }
exit:
    return;
}

/******************************************************************************
 * Function         phNxpEseProto7816_DecodeFrame
 *
 * Description      This internal function is used to
 *                  1. Identify the received frame
 *                  2. If the received frame is I-frame with expected sequence number, store it or else send R-NACK
                    3. If the received frame is R-frame,
                       3.1 R-ACK with expected seq. number: Send the next chained I-frame
                       3.2 R-ACK with different sequence number: Sebd the R-Nack
                       3.3 R-NACK: Re-send the last frame
                    4. If the received frame is S-frame, send back the correct S-frame response.
 *
 * param[in]        uint8_t : data buffer
 * param[in]        uint32_t : buffer length
 *
 * Returns          On success return TRUE or else FALSE.
 *
 ******************************************************************************/
static bool_t phNxpEseProto7816_DecodeFrame(uint8_t *p_data, uint32_t data_len)
{
    bool_t status = TRUE;
    uint8_t pcb;
    iFrameInfo_t *pRx_lastRcvdIframeInfo = &phNxpEseProto7816_3_Var.phNxpEseRx_Cntx.lastRcvdIframeInfo;
    rFrameInfo_t *pNextTx_RframeInfo     = &phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.RframeInfo;
    sFrameInfo_t *pNextTx_SframeInfo     = &phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.SframeInfo;
    iFrameInfo_t *pLastTx_IframeInfo     = &phNxpEseProto7816_3_Var.phNxpEseLastTx_Cntx.IframeInfo;
    sFrameInfo_t *pLastTx_SframeInfo     = &phNxpEseProto7816_3_Var.phNxpEseLastTx_Cntx.SframeInfo;
    rFrameInfo_t *pRx_lastRcvdRframeInfo = &phNxpEseProto7816_3_Var.phNxpEseRx_Cntx.lastRcvdRframeInfo;
    sFrameInfo_t *pRx_lastRcvdSframeInfo = &phNxpEseProto7816_3_Var.phNxpEseRx_Cntx.lastRcvdSframeInfo;

    T_SMLOG_D("Retry Counter = %d ", phNxpEseProto7816_3_Var.recoveryCounter);

    ENSURE_OR_GO_EXIT(p_data != NULL);
    ENSURE_OR_GO_EXIT(data_len < MAX_APDU_BUFFER);
    ENSURE_OR_GO_EXIT(data_len >= PH_PROTO_7816_HEADER_LEN);

    pcb = p_data[PH_PROPTO_7816_PCB_OFFSET];
    if (data_len < PH_PROTO_7816_INF_FILED) {
        return FALSE;
    }

    if (!(pcb & 0x80)) /* I-FRAME decoded should come here */
    {
        T_SMLOG_D("%s I-Frame Received ", __FUNCTION__);
        phNxpEseProto7816_3_Var.wtx_counter                       = 0;
        phNxpEseProto7816_3_Var.phNxpEseRx_Cntx.lastRcvdFrameType = IFRAME;
        if (pRx_lastRcvdIframeInfo->seqNo != ((pcb & 0x40) >> 6)) {
            T_SMLOG_D("%s I-Frame lastRcvdIframeInfo.seqNo:0x%x ", __FUNCTION__, ((pcb & 0x40) >> 6));
            phNxpEseProto7816_ResetRecovery();
            pRx_lastRcvdIframeInfo->seqNo = 0x00;
            pRx_lastRcvdIframeInfo->seqNo |= ((pcb & 0x40) >> 6);

            if (pcb & 0x20) {
                pRx_lastRcvdIframeInfo->isChained                     = TRUE;
                phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType = RFRAME;
                pNextTx_RframeInfo->errCode                           = NO_ERROR;
                if (FALSE == phNxpEseProro7816_SaveRxframeData(
                                 &p_data[PH_PROPTO_7816_INF_BYTE_OFFSET], data_len - PH_PROTO_7816_INF_FILED)) {
                    phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = IDLE_STATE;
                    T_SMLOG_E("phNxpEseProro7816_SaveRxframeData Failed");
                    return FALSE;
                }
                phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = SEND_R_ACK;
            }
            else {
                pRx_lastRcvdIframeInfo->isChained                             = FALSE;
                phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = IDLE_STATE;
                if (FALSE == phNxpEseProro7816_SaveRxframeData(
                                 &p_data[PH_PROPTO_7816_INF_BYTE_OFFSET], data_len - PH_PROTO_7816_INF_FILED)) {
                    T_SMLOG_E("phNxpEseProro7816_SaveRxframeData Failed");
                    return FALSE;
                }
            }
        }
        else {
            sm_sleep(DELAY_ERROR_RECOVERY / 1000);
            if (phNxpEseProto7816_3_Var.recoveryCounter < PH_PROTO_7816_FRAME_RETRY_COUNT) {
                phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = RFRAME;
                pNextTx_RframeInfo->errCode                                   = OTHER_ERROR;
                phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = SEND_R_NACK;
                phNxpEseProto7816_3_Var.recoveryCounter++;
            }
            else {
                phNxpEseProto7816_RecoverySteps();
                phNxpEseProto7816_3_Var.recoveryCounter++;
            }
        }
    }
    else if ((pcb & 0x80) && (!(0x40 & pcb))) /* R-FRAME decoded should come here */
    {
        T_SMLOG_D("%s R-Frame Received", __FUNCTION__);
        phNxpEseProto7816_3_Var.wtx_counter                       = 0;
        phNxpEseProto7816_3_Var.phNxpEseRx_Cntx.lastRcvdFrameType = RFRAME;
        pRx_lastRcvdRframeInfo->seqNo                             = 0; // = 0;
        pRx_lastRcvdRframeInfo->seqNo |= ((pcb & 0x10) >> 4);

        if ((!(pcb & 0x01)) && (!(pcb & 0x02))) {
            pRx_lastRcvdRframeInfo->errCode = NO_ERROR;
            phNxpEseProto7816_ResetRecovery();
            if (pRx_lastRcvdRframeInfo->seqNo != pLastTx_IframeInfo->seqNo) {
                phNxpEseProto7816_SetNextIframeContxt();
                phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = SEND_IFRAME;
            }

        } /* Error handling 1 : Parity error */
        else if (((pcb & 0x01) && (!(pcb & 0x02))) ||
                 /* Error handling 2: Other indicated error */
                 ((!(pcb & 0x01)) && (pcb & 0x02))) {
            sm_sleep(DELAY_ERROR_RECOVERY / 1000);
            if ((!(pcb & 0x01)) && (pcb & 0x02)) {
                pRx_lastRcvdRframeInfo->errCode = OTHER_ERROR;
            }
            else {
                pRx_lastRcvdRframeInfo->errCode = PARITY_ERROR;
            }
            if (phNxpEseProto7816_3_Var.recoveryCounter < PH_PROTO_7816_FRAME_RETRY_COUNT) {
                if (phNxpEseProto7816_3_Var.phNxpEseLastTx_Cntx.FrameType == IFRAME) {
                    phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx = phNxpEseProto7816_3_Var.phNxpEseLastTx_Cntx;
                    phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = SEND_IFRAME;
                    phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = IFRAME;
                }
                else if (phNxpEseProto7816_3_Var.phNxpEseLastTx_Cntx.FrameType == RFRAME) {
                    /* Usecase to reach the below case:
                    I-frame sent first, followed by R-NACK and we receive a R-NACK with
                    last sent I-frame sequence number*/
                    if ((pRx_lastRcvdRframeInfo->seqNo == pLastTx_IframeInfo->seqNo) &&
                        (phNxpEseProto7816_3_Var.lastSentNonErrorframeType == IFRAME)) {
                        phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx = phNxpEseProto7816_3_Var.phNxpEseLastTx_Cntx;
                        phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = SEND_IFRAME;
                        phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = IFRAME;
                    }
                    /* Usecase to reach the below case:
                    R-frame sent first, followed by R-NACK and we receive a R-NACK with
                    next expected I-frame sequence number*/
                    else if ((pRx_lastRcvdRframeInfo->seqNo != pLastTx_IframeInfo->seqNo) &&
                             (phNxpEseProto7816_3_Var.lastSentNonErrorframeType == RFRAME)) {
                        phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = RFRAME;
                        pNextTx_RframeInfo->errCode                                   = NO_ERROR;
                        phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = SEND_R_ACK;
                    }
                    /* Usecase to reach the below case:
                    I-frame sent first, followed by R-NACK and we receive a R-NACK with
                    next expected I-frame sequence number + all the other unexpected scenarios */
                    else {
                        phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = RFRAME;
                        pNextTx_RframeInfo->errCode                                   = OTHER_ERROR;
                        phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = SEND_R_NACK;
                    }
                }
                else if (phNxpEseProto7816_3_Var.phNxpEseLastTx_Cntx.FrameType == SFRAME) {
                    /* Copy the last S frame sent */
                    phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx = phNxpEseProto7816_3_Var.phNxpEseLastTx_Cntx;
                }
                phNxpEseProto7816_3_Var.recoveryCounter++;
            }
            else {
                phNxpEseProto7816_RecoverySteps();
                phNxpEseProto7816_3_Var.recoveryCounter++;
            }
            //resend previously send I frame
        }
        /* Error handling 3 */
        else if ((pcb & 0x01) && (pcb & 0x02)) {
            sm_sleep(DELAY_ERROR_RECOVERY / 1000);
            if (phNxpEseProto7816_3_Var.recoveryCounter < PH_PROTO_7816_FRAME_RETRY_COUNT) {
                pRx_lastRcvdRframeInfo->errCode             = SOF_MISSED_ERROR;
                phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx = phNxpEseProto7816_3_Var.phNxpEseLastTx_Cntx;
                phNxpEseProto7816_3_Var.recoveryCounter++;
            }
            else {
                phNxpEseProto7816_RecoverySteps();
                phNxpEseProto7816_3_Var.recoveryCounter++;
            }
        }
    }
    else if ((0x80 & pcb) && (0x40 & pcb)) /* S-FRAME decoded should come here */
    {
        T_SMLOG_D("%s S-Frame Received ", __FUNCTION__);
        int32_t frameType                                         = (int32_t)(pcb & 0x3F); /*discard upper 2 bits */
        phNxpEseProto7816_3_Var.phNxpEseRx_Cntx.lastRcvdFrameType = SFRAME;
        if (frameType != WTX_REQ) {
            phNxpEseProto7816_3_Var.wtx_counter = 0;
        }
        switch (frameType) {
        case RESYNCH_RSP:
            pRx_lastRcvdSframeInfo->sFrameType                            = RESYNCH_RSP;
            phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = UNKNOWN;
            phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = IDLE_STATE;
            break;
        case IFSC_RES:
            pRx_lastRcvdSframeInfo->sFrameType                            = IFSC_RES;
            phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = UNKNOWN;
            phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = IDLE_STATE;
            break;
        case ABORT_RES:
            pRx_lastRcvdSframeInfo->sFrameType                            = ABORT_RES;
            phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = UNKNOWN;
            phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = IDLE_STATE;
            break;
        case WTX_REQ:
            phNxpEseProto7816_3_Var.wtx_counter++;
            T_SMLOG_D("%s Wtx_counter value - %lu ", __FUNCTION__, phNxpEseProto7816_3_Var.wtx_counter);
            T_SMLOG_D(
                "%s Wtx_counter wtx_counter_limit - %lu ", __FUNCTION__, phNxpEseProto7816_3_Var.wtx_counter_limit);
            /* Previous sent frame is some S-frame but not WTX response S-frame */
            if (pLastTx_SframeInfo->sFrameType != WTX_RSP &&
                phNxpEseProto7816_3_Var.phNxpEseLastTx_Cntx.FrameType ==
                    SFRAME) { /* Goto recovery if it keep coming here for more than recovery counter max. value */
                if (phNxpEseProto7816_3_Var.recoveryCounter <
                    PH_PROTO_7816_FRAME_RETRY_COUNT) { /* Re-transmitting the previous sent S-frame */
                    phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx = phNxpEseProto7816_3_Var.phNxpEseLastTx_Cntx;
                    phNxpEseProto7816_3_Var.recoveryCounter++;
                }
                else {
                    phNxpEseProto7816_RecoverySteps();
                    phNxpEseProto7816_3_Var.recoveryCounter++;
                }
            }
            else { /* Checking for WTX counter with max. allowed WTX count */
                if (phNxpEseProto7816_3_Var.wtx_counter == phNxpEseProto7816_3_Var.wtx_counter_limit) {
#if defined(T1oI2C_UM11225)
                    phNxpEseProto7816_3_Var.wtx_counter                           = 0;
                    pRx_lastRcvdSframeInfo->sFrameType                            = INTF_RESET_REQ;
                    phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = SFRAME;
                    pNextTx_SframeInfo->sFrameType                                = INTF_RESET_REQ;
                    phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = SEND_S_INTF_RST;
                    T_SMLOG_E("%s Interface Reset to eSE wtx count reached!!! ", __FUNCTION__);
#elif defined(T1oI2C_GP1_0)
                    phNxpEseProto7816_3_Var.wtx_counter                           = 0;
                    pRx_lastRcvdSframeInfo->sFrameType                            = SWR_REQ;
                    phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = SFRAME;
                    pNextTx_SframeInfo->sFrameType                                = SWR_REQ;
                    phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = SEND_S_SWR;
                    T_SMLOG_E("%s Software Reset to eSE wtx count reached!!! ", __FUNCTION__);
#endif
                }
                else {
                    sm_sleep(DELAY_ERROR_RECOVERY / 1000);
                    pRx_lastRcvdSframeInfo->sFrameType                            = WTX_REQ;
                    phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = SFRAME;
                    pNextTx_SframeInfo->sFrameType                                = WTX_RSP;
                    phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = SEND_S_WTX_RSP;
                }
            }
            break;
#if defined(T1oI2C_UM11225)
        case INTF_RESET_RSP:
            if (p_data[PH_PROPTO_7816_FRAME_LENGTH_OFFSET] > 0) {
                phNxpEseProto7816_DecodeSFrameData(p_data);
            }
            if (data_len < PH_PROTO_7816_INF_FILED) {
                return FALSE;
            }
            if (FALSE == phNxpEseProro7816_SaveRxframeData(
                             &p_data[PH_PROPTO_7816_INF_BYTE_OFFSET], data_len - PH_PROTO_7816_INF_FILED)) {
                phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = IDLE_STATE;
                T_SMLOG_E("phNxpEseProro7816_SaveRxframeData Failed");
                return FALSE;
            }
            if (phNxpEseProto7816_3_Var.recoveryCounter > PH_PROTO_7816_FRAME_RETRY_COUNT) {
                /*Max recovery counter reached, send failure to APDU layer  */
                T_SMLOG_E("%s Max retry count reached!!! ", __FUNCTION__);
                phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = IDLE_STATE;
                status                                                        = FALSE;
            }
            else {
                phNxpEseProto7816_ResetProtoParams();
                pRx_lastRcvdSframeInfo->sFrameType                            = INTF_RESET_RSP;
                phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = UNKNOWN;
                phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = IDLE_STATE;
            }
            break;
        case PROP_END_APDU_RSP:
            pRx_lastRcvdSframeInfo->sFrameType = PROP_END_APDU_RSP;
            if (p_data[PH_PROPTO_7816_FRAME_LENGTH_OFFSET] > 0) {
                phNxpEseProto7816_DecodeSFrameData(p_data);
            }
            phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = UNKNOWN;
            phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = IDLE_STATE;
            break;
        case ATR_RES:
            pRx_lastRcvdSframeInfo->sFrameType = ATR_RES;
            if (p_data[PH_PROPTO_7816_FRAME_LENGTH_OFFSET] > 0) {
                phNxpEseProto7816_DecodeSFrameData(p_data);
            }
            if (FALSE == phNxpEseProro7816_SaveRxframeData(
                             &p_data[PH_PROPTO_7816_INF_BYTE_OFFSET], data_len - PH_PROTO_7816_INF_FILED)) {
                phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = IDLE_STATE;
                T_SMLOG_E("phNxpEseProro7816_SaveRxframeData Failed");
                return FALSE;
            }
            phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = UNKNOWN;
            phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = IDLE_STATE;
            break;
        case CHIP_RESET_RES:
            pRx_lastRcvdSframeInfo->sFrameType = CHIP_RESET_RES;
            if (p_data[PH_PROPTO_7816_FRAME_LENGTH_OFFSET] > 0) {
                phNxpEseProto7816_DecodeSFrameData(p_data);
            }
            phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = UNKNOWN;
            phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = IDLE_STATE;
            break;
#endif
#if defined(T1oI2C_GP1_0)
        case SWR_RSP:
            if (p_data[PH_PROPTO_7816_FRAME_LENGTH_OFFSET] > 0) {
                phNxpEseProto7816_DecodeSFrameData(p_data);
            }
            if (phNxpEseProto7816_3_Var.recoveryCounter > PH_PROTO_7816_FRAME_RETRY_COUNT) {
                /*Max recovery counter reached, send failure to APDU layer  */
                T_SMLOG_E("%s Max retry count reached!!! ", __FUNCTION__);
                phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = IDLE_STATE;
                status                                                        = FALSE;
            }
            else {
                phNxpEseProto7816_ResetProtoParams();
                pRx_lastRcvdSframeInfo->sFrameType                            = SWR_RSP;
                phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = UNKNOWN;
                phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = IDLE_STATE;
            }
            break;
        case RELEASE_RES:
            pRx_lastRcvdSframeInfo->sFrameType = RELEASE_RES;
            if (p_data[PH_PROPTO_7816_FRAME_LENGTH_OFFSET] > 0) {
                phNxpEseProto7816_DecodeSFrameData(p_data);
            }
            phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = UNKNOWN;
            phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = IDLE_STATE;
            break;
        case CIP_RES:
            pRx_lastRcvdSframeInfo->sFrameType = CIP_RES;
            if (p_data[PH_PROPTO_7816_FRAME_LENGTH_OFFSET] > 0) {
                phNxpEseProto7816_DecodeSFrameData(p_data);
            }
            if (FALSE == phNxpEseProro7816_SaveRxframeData(
                             &p_data[PH_PROPTO_7816_INF_BYTE_OFFSET], data_len - PH_PROTO_7816_INF_FILED)) {
                phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = IDLE_STATE;
                T_SMLOG_E("phNxpEseProro7816_SaveRxframeData Failed");
                return FALSE;
            }
            phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = UNKNOWN;
            phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = IDLE_STATE;
            break;
        case COLD_RESET_RES:
            pRx_lastRcvdSframeInfo->sFrameType = COLD_RESET_RES;
            if (p_data[PH_PROPTO_7816_FRAME_LENGTH_OFFSET] > 0) {
                phNxpEseProto7816_DecodeSFrameData(p_data);
            }
            phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = UNKNOWN;
            phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = IDLE_STATE;
            break;
#endif
        case DEEP_PWR_DOWN_RES:
            pRx_lastRcvdSframeInfo->sFrameType = DEEP_PWR_DOWN_RES;
            if (p_data[PH_PROPTO_7816_FRAME_LENGTH_OFFSET] > 0) {
                phNxpEseProto7816_DecodeSFrameData(p_data);
            }
            phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = UNKNOWN;
            phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = IDLE_STATE;
            break;
        default:
            T_SMLOG_E("%s Wrong S-Frame Received ", __FUNCTION__);
            break;
        }
    }
    else {
        T_SMLOG_E("%s Wrong-Frame Received ", __FUNCTION__);
    }
exit:
    return status;
}

/******************************************************************************
 * Function         phNxpEseProto7816_ProcessResponse
 *
 * Description      This internal function is used to
 *                  1. Check the CRC
 *                  2. Initiate decoding of received frame of data.
 *
 * param[in]        void
 *
 * Returns          On success return TRUE or else FALSE.
 *
 ******************************************************************************/
static bool_t phNxpEseProto7816_ProcessResponse(void *conn_ctx)
{
    uint32_t data_len                    = 0;
    uint8_t *p_data                      = NULL;
    bool_t status                        = FALSE;
    bool_t checkCrcPass                  = TRUE;
    iFrameInfo_t *pRx_lastRcvdIframeInfo = &phNxpEseProto7816_3_Var.phNxpEseRx_Cntx.lastRcvdIframeInfo;
    rFrameInfo_t *pNextTx_RframeInfo     = &phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.RframeInfo;
    sFrameInfo_t *pLastTx_SframeInfo     = &phNxpEseProto7816_3_Var.phNxpEseLastTx_Cntx.SframeInfo;

    status = phNxpEseProto7816_GetRawFrame(conn_ctx, &data_len, &p_data);
    if (TRUE == status) {
        /* Resetting the timeout counter */
        phNxpEseProto7816_3_Var.timeoutCounter = PH_PROTO_7816_VALUE_ZERO;
        /* CRC check followed */
        checkCrcPass = phNxpEseProto7816_CheckCRC(data_len, p_data);
        if (checkCrcPass == TRUE) {
            /* Resetting the RNACK retry counter */
            phNxpEseProto7816_3_Var.rnack_retry_counter = PH_PROTO_7816_VALUE_ZERO;
            status                                      = phNxpEseProto7816_DecodeFrame(p_data, data_len);
        }
        else {
            T_SMLOG_E("%s CRC Check failed ", __FUNCTION__);
            if (phNxpEseProto7816_3_Var.rnack_retry_counter < phNxpEseProto7816_3_Var.rnack_retry_limit) {
                phNxpEseProto7816_3_Var.phNxpEseRx_Cntx.lastRcvdFrameType     = INVALID;
                phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = RFRAME;
                pNextTx_RframeInfo->errCode                                   = PARITY_ERROR;
                pNextTx_RframeInfo->seqNo                                     = (!pRx_lastRcvdIframeInfo->seqNo) << 4;
                phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = SEND_R_NACK;
                phNxpEseProto7816_3_Var.rnack_retry_counter++;
            }
            else {
                phNxpEseProto7816_3_Var.rnack_retry_counter = PH_PROTO_7816_VALUE_ZERO;
                /* Re-transmission failed completely, Going to exit */
                phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = IDLE_STATE;
                phNxpEseProto7816_3_Var.timeoutCounter                        = PH_PROTO_7816_VALUE_ZERO;
                status                                                        = FALSE;
            }
        }
    }
    else {
        T_SMLOG_E("%s phNxpEseProto7816_GetRawFrame failed starting recovery", __FUNCTION__);
        if ((SFRAME == phNxpEseProto7816_3_Var.phNxpEseLastTx_Cntx.FrameType) &&
            ((WTX_RSP == pLastTx_SframeInfo->sFrameType) || (RESYNCH_RSP == pLastTx_SframeInfo->sFrameType))) {
            if (phNxpEseProto7816_3_Var.rnack_retry_counter < phNxpEseProto7816_3_Var.rnack_retry_limit) {
                phNxpEse_clearReadBuffer(conn_ctx);
                phNxpEseProto7816_3_Var.phNxpEseRx_Cntx.lastRcvdFrameType     = INVALID;
                phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = RFRAME;
                pNextTx_RframeInfo->errCode                                   = OTHER_ERROR;
                pNextTx_RframeInfo->seqNo                                     = (!pRx_lastRcvdIframeInfo->seqNo) << 4;
                phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = SEND_R_NACK;
                phNxpEseProto7816_3_Var.rnack_retry_counter++;
            }
            else {
                T_SMLOG_E("%s Recovery failed completely, Going to exit ", __FUNCTION__);
                phNxpEseProto7816_3_Var.rnack_retry_counter = PH_PROTO_7816_VALUE_ZERO;
                /* Recovery failed completely, Going to exit */
                phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = IDLE_STATE;
                phNxpEseProto7816_3_Var.timeoutCounter                        = PH_PROTO_7816_VALUE_ZERO;
            }
        }
        /*ISO7816-3 Rule 7.1 Implementation*/
        else if (IFRAME == phNxpEseProto7816_3_Var.phNxpEseLastTx_Cntx.FrameType) {
            if (phNxpEseProto7816_3_Var.rnack_retry_counter < phNxpEseProto7816_3_Var.rnack_retry_limit) {
                phNxpEse_clearReadBuffer(conn_ctx);
                phNxpEseProto7816_3_Var.phNxpEseRx_Cntx.lastRcvdFrameType     = INVALID;
                phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = RFRAME;
                pNextTx_RframeInfo->errCode                                   = PARITY_ERROR;
                pNextTx_RframeInfo->seqNo                                     = (!pRx_lastRcvdIframeInfo->seqNo) << 4;
                phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = SEND_R_NACK;
                phNxpEseProto7816_3_Var.rnack_retry_counter++;
            }
            else {
                T_SMLOG_E("%s Recovery failed completely, Going to exit ", __FUNCTION__);
                phNxpEseProto7816_3_Var.rnack_retry_counter = PH_PROTO_7816_VALUE_ZERO;
                /* Recovery failed completely, Going to exit */
                phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = IDLE_STATE;
                phNxpEseProto7816_3_Var.timeoutCounter                        = PH_PROTO_7816_VALUE_ZERO;
            }
        }
        else {
            sm_sleep(DELAY_ERROR_RECOVERY / 1000);
            /* re transmit the frame */
            if (phNxpEseProto7816_3_Var.timeoutCounter < PH_PROTO_7816_TIMEOUT_RETRY_COUNT) {
                phNxpEseProto7816_3_Var.timeoutCounter++;
                T_SMLOG_E("%s re-transmitting the previous frame ", __FUNCTION__);
                phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx = phNxpEseProto7816_3_Var.phNxpEseLastTx_Cntx;
            }
            else {
                /* Recovery failed completely, Going to exit */
                T_SMLOG_E("%s Recovery failed completely, Going to exit ", __FUNCTION__);
                phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = IDLE_STATE;
                phNxpEseProto7816_3_Var.timeoutCounter                        = PH_PROTO_7816_VALUE_ZERO;
            }
        }
    }
    return status;
}

/******************************************************************************
 * Function         TransceiveProcess
 *
 * Description      This internal function is used to
 *                  1. Send the raw data received from application after computing CRC
 *                  2. Receive the the response data from ESE, decode, process and
 *                     store the data.
 *
 * param[in]        void
 *
 * Returns          On success return TRUE or else FALSE.
 *
 ******************************************************************************/
static bool_t TransceiveProcess(void *conn_ctx)
{
    bool_t status = FALSE;
    sFrameInfo_t sFrameInfo;
    sFrameInfo.sFrameType = INVALID_REQ_RES;

    sFrameInfo.sFrameType = INVALID_REQ_RES;

    while (phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState != IDLE_STATE) {
        T_SMLOG_D(
            "%s nextTransceiveState %x ", __FUNCTION__, phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState);
        switch (phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState) {
        case SEND_IFRAME:
            status = phNxpEseProto7816_SendIframe(conn_ctx, phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.IframeInfo);
            break;
        case SEND_R_ACK:
            status = phNxpEseProto7816_sendRframe(conn_ctx, RACK);
            break;
        case SEND_R_NACK:
            status = phNxpEseProto7816_sendRframe(conn_ctx, RNACK);
            break;
        case SEND_S_RSYNC:
            sFrameInfo.sFrameType = RESYNCH_REQ;
            status                = phNxpEseProto7816_SendSFrame(conn_ctx, sFrameInfo);
            break;
        case SEND_S_WTX_RSP:
            sFrameInfo.sFrameType = WTX_RSP;
            status                = phNxpEseProto7816_SendSFrame(conn_ctx, sFrameInfo);
            break;
        case SEND_DEEP_PWR_DOWN:
            sFrameInfo.sFrameType = DEEP_PWR_DOWN_REQ;
            status                = phNxpEseProto7816_SendSFrame(conn_ctx, sFrameInfo);
            break;
#if defined(T1oI2C_UM11225)
        case SEND_S_CHIP_RST:
            sFrameInfo.sFrameType = CHIP_RESET_REQ;
            status                = phNxpEseProto7816_SendSFrame(conn_ctx, sFrameInfo);
            break;
        case SEND_S_INTF_RST:
            sFrameInfo.sFrameType = INTF_RESET_REQ;
            status                = phNxpEseProto7816_SendSFrame(conn_ctx, sFrameInfo);
            break;
        case SEND_S_EOS:
            sFrameInfo.sFrameType = PROP_END_APDU_REQ;
            status                = phNxpEseProto7816_SendSFrame(conn_ctx, sFrameInfo);
            break;
        case SEND_S_ATR:
            sFrameInfo.sFrameType = ATR_REQ;
            status                = phNxpEseProto7816_SendSFrame(conn_ctx, sFrameInfo);
            break;
#elif defined(T1oI2C_GP1_0)
        case SEND_S_CIP:
            sFrameInfo.sFrameType = CIP_REQ;
            status                = phNxpEseProto7816_SendSFrame(conn_ctx, sFrameInfo);
            break;
        case SEND_S_SWR:
            sFrameInfo.sFrameType = SWR_REQ;
            status                = phNxpEseProto7816_SendSFrame(conn_ctx, sFrameInfo);
            break;
        case SEND_S_RELEASE:
            sFrameInfo.sFrameType = RELEASE_REQ;
            status                = phNxpEseProto7816_SendSFrame(conn_ctx, sFrameInfo);
            break;
        case SEND_S_COLD_RST:
            sFrameInfo.sFrameType = COLD_RESET_REQ;
            status                = phNxpEseProto7816_SendSFrame(conn_ctx, sFrameInfo);
            break;
#else
#error Either T1oI2C_UM11225 or T1oI2C_GP1_0 must be defined.
#endif
        default:
            phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = IDLE_STATE;
            break;
        }
        if (TRUE == status) {
            phNxpEseProto7816_3_Var.phNxpEseLastTx_Cntx = phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx;
            status                                      = phNxpEseProto7816_ProcessResponse(conn_ctx);
        }
        else {
            T_SMLOG_E("%s Transceive send failed, going to recovery! ", __FUNCTION__);
            phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = IDLE_STATE;
        }
    };
    return status;
}

/******************************************************************************
 * Function         phNxpEseProto7816_Transceive
 *
 * Description      This function is used to
 *                  1. Send the raw data received from application after computing CRC
 *                  2. Receive the the response data from ESE, decode, process and
 *                     store the data.
 *                  3. Get the final complete data and sent back to application
 *
 * param[in]        phNxpEse_data: Command to ESE C-APDU
 * param[out]       phNxpEse_data: Response from ESE R-APDU
 *
 * Returns          On success return TRUE or else FALSE.
 *
 ******************************************************************************/
bool_t phNxpEseProto7816_Transceive(void *conn_ctx, phNxpEse_data *pCmd, phNxpEse_data *pRsp)
{
    bool_t status                    = FALSE;
    phNxpEseRx_Cntx_t *pRx_EseCntx   = &phNxpEseProto7816_3_Var.phNxpEseRx_Cntx;
    iFrameInfo_t *pNextTx_IframeInfo = &phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.IframeInfo;

    T_SMLOG_D("Enter %s  ", __FUNCTION__);
    if ((NULL == pCmd) || (NULL == pRsp) ||
        (phNxpEseProto7816_3_Var.phNxpEseProto7816_CurrentState != PH_NXP_ESE_PROTO_7816_IDLE))
        return status;
    /* Updating the transceive information to the protocol stack */
    phNxpEseProto7816_3_Var.phNxpEseProto7816_CurrentState = PH_NXP_ESE_PROTO_7816_TRANSCEIVE;
    pNextTx_IframeInfo->p_data                             = pCmd->p_data;
    pNextTx_IframeInfo->totalDataLen                       = pCmd->len;
    pRx_EseCntx->pRsp                                      = pRsp;
    T_SMLOG_D("Transceive data ptr 0x%p len:%d ", pCmd->p_data, pCmd->len);
    phNxpEseProto7816_SetFirstIframeContxt();
    status = TransceiveProcess(conn_ctx);
    if (FALSE == status) {
        /* ESE hard reset to be done */
        T_SMLOG_E("%s Transceive failed, hard reset to proceed ", __FUNCTION__);
    }
    if (pRx_EseCntx->responseBytesRcvd > UINT32_MAX) {
        return FALSE;
    }
    pRsp->len                                              = pRx_EseCntx->responseBytesRcvd;
    phNxpEseProto7816_3_Var.phNxpEseProto7816_CurrentState = PH_NXP_ESE_PROTO_7816_IDLE;
    return status;
}

/******************************************************************************
 * Function         phNxpEseProto7816_RSync
 *
 * Description      This function is used to send the RSync command
 *
 * param[in]        void
 *
 * Returns          On success return TRUE or else FALSE.
 *
 ******************************************************************************/
static bool_t phNxpEseProto7816_RSync(void *conn_ctx)
{
    bool_t status                    = FALSE;
    sFrameInfo_t *pNextTx_SframeInfo = &phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.SframeInfo;

    phNxpEseProto7816_3_Var.phNxpEseProto7816_CurrentState = PH_NXP_ESE_PROTO_7816_TRANSCEIVE;
    /* send the end of session s-frame */
    phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = SFRAME;
    pNextTx_SframeInfo->sFrameType                                = RESYNCH_REQ;
    phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = SEND_S_RSYNC;
    status                                                        = TransceiveProcess(conn_ctx);
    phNxpEseProto7816_3_Var.phNxpEseProto7816_CurrentState        = PH_NXP_ESE_PROTO_7816_IDLE;
    return status;
}

/******************************************************************************
 * Function         phNxpEseProto7816_ResetProtoParams
 *
 * Description      This function is used to reset the 7816 protocol stack instance
 *
 * param[in]        void
 *
 * Returns          Always return TRUE.
 *
 ******************************************************************************/
bool_t phNxpEseProto7816_ResetProtoParams(void)
{
    unsigned long int tmpWTXCountlimit   = PH_PROTO_7816_VALUE_ZERO;
    unsigned long int tmpRNACKCountlimit = PH_PROTO_7816_VALUE_ZERO;
    phNxpEseRx_Cntx_t *pRx_EseCntx       = &phNxpEseProto7816_3_Var.phNxpEseRx_Cntx;
    iFrameInfo_t *pNextTx_IframeInfo     = &phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.IframeInfo;
    iFrameInfo_t *pLastTx_IframeInfo     = &phNxpEseProto7816_3_Var.phNxpEseLastTx_Cntx.IframeInfo;

    tmpWTXCountlimit   = phNxpEseProto7816_3_Var.wtx_counter_limit;
    tmpRNACKCountlimit = phNxpEseProto7816_3_Var.rnack_retry_limit;
    phNxpEse_memset(&phNxpEseProto7816_3_Var, PH_PROTO_7816_VALUE_ZERO, sizeof(phNxpEseProto7816_t));
    phNxpEseProto7816_3_Var.wtx_counter_limit                     = tmpWTXCountlimit;
    phNxpEseProto7816_3_Var.rnack_retry_limit                     = tmpRNACKCountlimit;
    phNxpEseProto7816_3_Var.phNxpEseProto7816_CurrentState        = PH_NXP_ESE_PROTO_7816_IDLE;
    phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = IDLE_STATE;
    pRx_EseCntx->lastRcvdFrameType                                = INVALID;
    phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = INVALID;
    pNextTx_IframeInfo->maxDataLen                                = IFSC_SIZE_SEND;
    pNextTx_IframeInfo->p_data                                    = NULL;
    phNxpEseProto7816_3_Var.phNxpEseLastTx_Cntx.FrameType         = INVALID;
    pLastTx_IframeInfo->maxDataLen                                = IFSC_SIZE_SEND;
    pLastTx_IframeInfo->p_data                                    = NULL;
    /* Initialized with sequence number of the last I-frame sent */
    pNextTx_IframeInfo->seqNo = PH_PROTO_7816_VALUE_ONE;
    /* Initialized with sequence number of the last I-frame received */
    pRx_EseCntx->lastRcvdIframeInfo.seqNo = PH_PROTO_7816_VALUE_ONE;
    /* Initialized with sequence number of the last I-frame received */
    pLastTx_IframeInfo->seqNo               = PH_PROTO_7816_VALUE_ONE;
    phNxpEseProto7816_3_Var.recoveryCounter = PH_PROTO_7816_VALUE_ZERO;
    phNxpEseProto7816_3_Var.timeoutCounter  = PH_PROTO_7816_VALUE_ZERO;
    phNxpEseProto7816_3_Var.wtx_counter     = PH_PROTO_7816_VALUE_ZERO;
    /* This update is helpful in-case a R-NACK is transmitted from the MW */
    phNxpEseProto7816_3_Var.lastSentNonErrorframeType = UNKNOWN;
    phNxpEseProto7816_3_Var.rnack_retry_counter       = PH_PROTO_7816_VALUE_ZERO;
    pRx_EseCntx->pRsp                                 = NULL;
    return TRUE;
}

/******************************************************************************
 * Function         phNxpEseProto7816_Reset
 *
 * Description      This function is used to reset the 7816 protocol stack instance
 *
 * param[in]        void
 *
 * Returns          On success return TRUE or else FALSE.
 *
 ******************************************************************************/
bool_t phNxpEseProto7816_Reset(void)
{
    bool_t status = FALSE;
    /* Resetting host protocol instance */
    status = phNxpEseProto7816_ResetProtoParams();
    /* Resynchronising ESE protocol instance */
    //status = phNxpEseProto7816_RSync();
    return status;
}

/******************************************************************************
 * Function         phNxpEseProto7816_Open
 *
 * Description      This function is used to open the 7816 protocol stack instance
 *
 * param[in]        phNxpEseProto7816InitParam_t: ESE communication mode
 * param[out]       phNxpEse_data: ATR Response from ESE
 *
 * Returns          On success return TRUE or else FALSE.
 *
 ******************************************************************************/
bool_t phNxpEseProto7816_Open(void *conn_ctx, phNxpEseProto7816InitParam_t initParam, phNxpEse_data *AtrRsp)
{
    bool_t status                  = FALSE;
    phNxpEseRx_Cntx_t *pRx_EseCntx = &phNxpEseProto7816_3_Var.phNxpEseRx_Cntx;
    status                         = phNxpEseProto7816_ResetProtoParams();
    T_SMLOG_D("%s: First open completed", __FUNCTION__);
    /* Update WTX max. limit */
    phNxpEseProto7816_3_Var.wtx_counter_limit = initParam.wtx_counter_limit;
    phNxpEseProto7816_3_Var.rnack_retry_limit = initParam.rnack_retry_limit;
    /*Intialise the buffers before hand so that we are able to receive data
    if RSync goes to recovery handling*/
    pRx_EseCntx->pRsp              = AtrRsp;
    pRx_EseCntx->pRsp->len         = AtrRsp->len;
    pRx_EseCntx->responseBytesRcvd = 0;
    if (initParam.interfaceReset) /* Do interface reset */
    {
        /*After power ON , initialization state takes 5ms after which slave enters active
        state where slave can exchange data with the master */
        sm_sleep(WAKE_UP_DELAY_MS);
        phNxpEse_waitForWTX(conn_ctx);
        phNxpEse_clearReadBuffer(conn_ctx);
#if defined(T1oI2C_UM11225)
        /* Interface Reset respond with ATR*/
        status = phNxpEseProto7816_RSync(conn_ctx);
        if (status == TRUE) {
            status = phNxpEseProto7816_GetAtr(conn_ctx, AtrRsp);
        }

#elif defined(T1oI2C_GP1_0)
        /* For GP soft reset does not respond with CIP so master should send CIP req. seperatly  */
        status = phNxpEseProto7816_RSync(conn_ctx);
        if (status == TRUE) {
            status = phNxpEseProto7816_GetCip(conn_ctx, AtrRsp);
        }
#endif
    }
    else /* Do R-Sync */
    {
        status = phNxpEseProto7816_RSync(conn_ctx);
    }
    AtrRsp->len = pRx_EseCntx->responseBytesRcvd;
    return status;
}

/******************************************************************************
 * Function         phNxpEseProto7816_Close
 *
 * Description      This function is used to close the 7816 protocol stack instance
 *
 * param[in]        void
 *
 * Returns          On success return TRUE or else FALSE.
 *
 ******************************************************************************/
bool_t phNxpEseProto7816_Close(void *conn_ctx)
{
    sFrameInfo_t *pNextTx_SframeInfo = &phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.SframeInfo;
    bool_t status                    = FALSE;
    /*Explicitly Initilising to NULL as the Application layer does not intend to receive a response*/
    phNxpEseRx_Cntx_t *pRx_EseCntx = &phNxpEseProto7816_3_Var.phNxpEseRx_Cntx;
    pRx_EseCntx->pRsp              = NULL;

    if (phNxpEseProto7816_3_Var.phNxpEseProto7816_CurrentState != PH_NXP_ESE_PROTO_7816_IDLE) {
        return status;
    }
    phNxpEseProto7816_3_Var.phNxpEseProto7816_CurrentState = PH_NXP_ESE_PROTO_7816_DEINIT;
    phNxpEseProto7816_3_Var.recoveryCounter                = 0;
    phNxpEseProto7816_3_Var.wtx_counter                    = 0;
#if defined(T1oI2C_UM11225)
    /* send the end of session s-frame */
    phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = SFRAME;
    pNextTx_SframeInfo->sFrameType                                = PROP_END_APDU_REQ;
    phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = SEND_S_EOS;
#elif defined(T1oI2C_GP1_0)
    /* send the release request s-frame */
    phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = SFRAME;
    pNextTx_SframeInfo->sFrameType                                = RELEASE_REQ;
    phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = SEND_S_RELEASE;
#endif
    status = TransceiveProcess(conn_ctx);
    if (FALSE == status) {
        /* reset all the structures */
        T_SMLOG_E("%s TransceiveProcess failed  ", __FUNCTION__);
    }
    phNxpEseProto7816_3_Var.phNxpEseProto7816_CurrentState = PH_NXP_ESE_PROTO_7816_IDLE;
    return status;
}

#if defined(T1oI2C_UM11225)
/******************************************************************************
 * Function         phNxpEseProto7816_IntfReset
 *
 * Description      This function is used to reset just the current interface
                    and get the ATR response on successful reset
 *
 * param[in]        phNxpEse_data: ATR response from ESE
 *
 * Returns          On success return TRUE or else FALSE.
 *
 ******************************************************************************/
bool_t phNxpEseProto7816_IntfReset(void *conn_ctx, phNxpEse_data *AtrRsp)
{
    bool_t status                    = FALSE;
    sFrameInfo_t *pNextTx_SframeInfo = &phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.SframeInfo;
    phNxpEseRx_Cntx_t *pRx_EseCntx   = &phNxpEseProto7816_3_Var.phNxpEseRx_Cntx;

    ENSURE_OR_GO_EXIT(AtrRsp != NULL);
    phNxpEseProto7816_3_Var.phNxpEseProto7816_CurrentState        = PH_NXP_ESE_PROTO_7816_TRANSCEIVE;
    phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = SFRAME;
    pNextTx_SframeInfo->sFrameType                                = INTF_RESET_REQ;
    phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = SEND_S_INTF_RST;
    pRx_EseCntx->pRsp                                             = AtrRsp;
    pRx_EseCntx->pRsp->len                                        = AtrRsp->len;
    pRx_EseCntx->responseBytesRcvd                                = 0;
    phNxpEse_clearReadBuffer(conn_ctx);
    status      = TransceiveProcess(conn_ctx);
    AtrRsp->len = pRx_EseCntx->responseBytesRcvd;
    if (FALSE == status) {
        /* reset all the structures */
        T_SMLOG_E("%s TransceiveProcess failed  ", __FUNCTION__);
    }

    phNxpEseProto7816_3_Var.phNxpEseProto7816_CurrentState = PH_NXP_ESE_PROTO_7816_IDLE;
exit:
    return status;
}

/******************************************************************************
 * Function         phNxpEseProto7816_ChipReset
 *
 * Description      This function is used to reset just the current interface
 *
 * param[in]        void
 *
 * Returns          On success return TRUE or else FALSE.
 *
 ******************************************************************************/
bool_t phNxpEseProto7816_ChipReset(void *conn_ctx)
{
    bool_t status                    = FALSE;
    sFrameInfo_t *pNextTx_SframeInfo = &phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.SframeInfo;
    phNxpEseRx_Cntx_t *pRx_EseCntx   = &phNxpEseProto7816_3_Var.phNxpEseRx_Cntx;

    phNxpEseProto7816_3_Var.phNxpEseProto7816_CurrentState        = PH_NXP_ESE_PROTO_7816_TRANSCEIVE;
    phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = SFRAME;
    pNextTx_SframeInfo->sFrameType                                = CHIP_RESET_REQ;
    phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = SEND_S_CHIP_RST;
    pRx_EseCntx->pRsp                                             = NULL;
    status                                                        = TransceiveProcess(conn_ctx);
    if (FALSE == status) {
        /* reset all the structures */
        T_SMLOG_E("%s TransceiveProcess failed  ", __FUNCTION__);
    }
    phNxpEseProto7816_3_Var.phNxpEseProto7816_CurrentState = PH_NXP_ESE_PROTO_7816_IDLE;
    return status;
}
#endif

#if defined(T1oI2C_GP1_0)
/******************************************************************************
 * Function         phNxpEseProto7816_SoftReset
 *
 * Description      This function is used only for T1oI2C GP to reset just the current interface
 *
 * param[in]        void
 *
 * Returns          On success return TRUE or else FALSE.
 *
 ******************************************************************************/
bool_t phNxpEseProto7816_SoftReset(void *conn_ctx)
{
    bool_t status                    = FALSE;
    sFrameInfo_t *pNextTx_SframeInfo = &phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.SframeInfo;
    phNxpEseRx_Cntx_t *pRx_EseCntx   = &phNxpEseProto7816_3_Var.phNxpEseRx_Cntx;

    phNxpEseProto7816_3_Var.phNxpEseProto7816_CurrentState        = PH_NXP_ESE_PROTO_7816_TRANSCEIVE;
    phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = SFRAME;
    pNextTx_SframeInfo->sFrameType                                = SWR_REQ;
    phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = SEND_S_SWR;
    pRx_EseCntx->pRsp                                             = NULL;
    phNxpEse_clearReadBuffer(conn_ctx);
    status = TransceiveProcess(conn_ctx);
    if (FALSE == status) {
        /* reset all the structures */
        T_SMLOG_E("%s TransceiveProcess failed  ", __FUNCTION__);
    }

    phNxpEseProto7816_3_Var.phNxpEseProto7816_CurrentState = PH_NXP_ESE_PROTO_7816_IDLE;
    return status;
}

/******************************************************************************
 * Function         phNxpEseProto7816_ColdReset
 *
 * Description      This function is used to reset just the current interface
 *
 * param[in]        void
 *
 * Returns          On success return TRUE or else FALSE.
 *
 ******************************************************************************/
bool_t phNxpEseProto7816_ColdReset(void *conn_ctx)
{
    bool_t status                    = FALSE;
    sFrameInfo_t *pNextTx_SframeInfo = &phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.SframeInfo;
    phNxpEseRx_Cntx_t *pRx_EseCntx   = &phNxpEseProto7816_3_Var.phNxpEseRx_Cntx;

    phNxpEseProto7816_3_Var.phNxpEseProto7816_CurrentState        = PH_NXP_ESE_PROTO_7816_TRANSCEIVE;
    phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = SFRAME;
    pNextTx_SframeInfo->sFrameType                                = COLD_RESET_REQ;
    phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = SEND_S_COLD_RST;
    pRx_EseCntx->pRsp                                             = NULL;
    status                                                        = TransceiveProcess(conn_ctx);
    if (FALSE == status) {
        /* reset all the structures */
        T_SMLOG_E("%s TransceiveProcess failed  ", __FUNCTION__);
    }
    phNxpEseProto7816_3_Var.phNxpEseProto7816_CurrentState = PH_NXP_ESE_PROTO_7816_IDLE;
    return status;
}
#endif
/******************************************************************************
 * Function         phNxpEseProto7816_SetIfscSize
 *
 * Description      This function is used to set the max T=1 data send size
 *
 * param[in]        uint16_t IFSC_Size
 *
 * Returns          Always return TRUE (1).
 *
 ******************************************************************************/
bool_t phNxpEseProto7816_SetIfscSize(uint16_t IFSC_Size)
{
    iFrameInfo_t *pNextTx_IframeInfo = &phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.IframeInfo;
    pNextTx_IframeInfo->maxDataLen   = IFSC_Size;
    return TRUE;
}

/******************************************************************************
 * Function         phNxpEseProto7816_WTXRsp
 *
 * Description      This function is used to send WTX response
 *
 * param[in]        void* conn_ctx
 *
 * Returns          On success return TRUE or else FALSE.
 *
 ******************************************************************************/
bool_t phNxpEseProto7816_WTXRsp(void *conn_ctx)
{
    sFrameInfo_t sFrameInfo;
    sFrameInfo.sFrameType = WTX_RSP;
    T_SMLOG_D(" %s - Sending WTX Response", __FUNCTION__);
    return phNxpEseProto7816_SendSFrame(conn_ctx, sFrameInfo);
}

/******************************************************************************
 * Function         phNxpEseProto7816_SendRSync
 *
 * Description      This function is used to send Rsync
 *
 * param[in]        void* conn_ctx
 *
 * Returns          On success return TRUE or else FALSE.
 *
 ******************************************************************************/
bool_t phNxpEseProto7816_SendRSync(void *conn_ctx)
{
    sFrameInfo_t sFrameInfo;
    sFrameInfo.sFrameType = RESYNCH_REQ;
    T_SMLOG_D(" %s - Sending Rsync", __FUNCTION__);
    return phNxpEseProto7816_SendSFrame(conn_ctx, sFrameInfo);
}

#if defined(T1oI2C_UM11225)
/******************************************************************************
 * Function         phNxpEseProto7816_GetAtr
 *
 * Description      This function is used to reset just the current interface
 *
 * param[in]        phNxpEse_data : ATR response from ESE
 *
 * Returns          On success return TRUE or else FALSE.
 *
 ******************************************************************************/
bool_t phNxpEseProto7816_GetAtr(void *conn_ctx, phNxpEse_data *pRsp)
{
    bool_t status                    = FALSE;
    sFrameInfo_t *pNextTx_SframeInfo = &phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.SframeInfo;
    phNxpEseRx_Cntx_t *pRx_EseCntx   = &phNxpEseProto7816_3_Var.phNxpEseRx_Cntx;

    ENSURE_OR_GO_EXIT(pRsp != NULL);
    phNxpEseProto7816_3_Var.phNxpEseProto7816_CurrentState        = PH_NXP_ESE_PROTO_7816_TRANSCEIVE;
    phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = SFRAME;
    pNextTx_SframeInfo->sFrameType                                = ATR_REQ;
    phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = SEND_S_ATR;
    pRx_EseCntx->pRsp                                             = pRsp;
    pRx_EseCntx->pRsp->len                                        = pRsp->len;
    pRx_EseCntx->responseBytesRcvd                                = 0;
    status                                                        = TransceiveProcess(conn_ctx);
    pRsp->len                                                     = pRx_EseCntx->responseBytesRcvd;
    if (FALSE == status) {
        /* reset all the structures */
        T_SMLOG_E("%s TransceiveProcess failed  ", __FUNCTION__);
    }
    phNxpEseProto7816_3_Var.phNxpEseProto7816_CurrentState = PH_NXP_ESE_PROTO_7816_IDLE;
exit:
    return status;
}
#endif

#if defined(T1oI2C_GP1_0)
/******************************************************************************
 * Function         phNxpEseProto7816_GetCip
 *
 * Description      This function is used only by T1oI2c GP to get CIP response
 *
 * param[in]        phNxpEse_data : CIP response from ESE
 *
 * Returns          On success return TRUE or else FALSE.
 *
 ******************************************************************************/
bool_t phNxpEseProto7816_GetCip(void *conn_ctx, phNxpEse_data *pRsp)
{
    bool_t status                    = FALSE;
    phNxpEseRx_Cntx_t *pRx_EseCntx   = &phNxpEseProto7816_3_Var.phNxpEseRx_Cntx;
    sFrameInfo_t *pNextTx_SframeInfo = &phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.SframeInfo;

    ENSURE_OR_GO_EXIT(pRsp != NULL);
    phNxpEseProto7816_3_Var.phNxpEseProto7816_CurrentState        = PH_NXP_ESE_PROTO_7816_TRANSCEIVE;
    phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = SFRAME;
    pNextTx_SframeInfo->sFrameType                                = CIP_REQ;
    phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = SEND_S_CIP;
    pRx_EseCntx->pRsp                                             = pRsp;
    pRx_EseCntx->pRsp->len                                        = pRsp->len;
    pRx_EseCntx->responseBytesRcvd                                = 0;
    status                                                        = TransceiveProcess(conn_ctx);
    pRsp->len                                                     = pRx_EseCntx->responseBytesRcvd;
    if (FALSE == status) {
        /* reset all the structures */
        T_SMLOG_E("%s TransceiveProcess failed  ", __FUNCTION__);
    }

    phNxpEseProto7816_3_Var.phNxpEseProto7816_CurrentState = PH_NXP_ESE_PROTO_7816_IDLE;
exit:
    return status;
}
#endif

/******************************************************************************
 * Function         phNxpEseProto7816_Deep_Pwr_Down
 *
 * Description      This function is used to send deep power down command
 *
 * param[in]        void
 *
 * Returns          On success return TRUE or else FALSE.
 *
 ******************************************************************************/
bool_t phNxpEseProto7816_Deep_Pwr_Down(void *conn_ctx)
{
    bool_t status                    = FALSE;
    sFrameInfo_t *pNextTx_SframeInfo = &phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.SframeInfo;

    phNxpEseProto7816_3_Var.phNxpEseProto7816_CurrentState = PH_NXP_ESE_PROTO_7816_TRANSCEIVE;
    /* send the end of session s-frame */
    phNxpEseProto7816_3_Var.phNxpEseNextTx_Cntx.FrameType         = SFRAME;
    pNextTx_SframeInfo->sFrameType                                = DEEP_PWR_DOWN_REQ;
    phNxpEseProto7816_3_Var.phNxpEseProto7816_nextTransceiveState = SEND_DEEP_PWR_DOWN;
    status                                                        = TransceiveProcess(conn_ctx);
    phNxpEseProto7816_3_Var.phNxpEseProto7816_CurrentState        = PH_NXP_ESE_PROTO_7816_IDLE;
    return status;
}

/** @} */
