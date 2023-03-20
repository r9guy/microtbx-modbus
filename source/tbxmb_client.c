/************************************************************************************//**
* \file         tbxmb_client.c
* \brief        Modbus client source file.
* \internal
*----------------------------------------------------------------------------------------
*                          C O P Y R I G H T
*----------------------------------------------------------------------------------------
*   Copyright (c) 2023 by Feaser     www.feaser.com     All rights reserved
*
*----------------------------------------------------------------------------------------
*                            L I C E N S E
*----------------------------------------------------------------------------------------
*
* SPDX-License-Identifier: GPL-3.0-or-later
*
* This file is part of MicroTBX-Modbus. MicroTBX-Modbus is free software: you can
* redistribute it and/or modify it under the terms of the GNU General Public License as
* published by the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* MicroTBX-Modbus is distributed in the hope that it will be useful, but WITHOUT ANY
* WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
* PARTICULAR PURPOSE. See the GNU General Public License for more details.
*
* You have received a copy of the GNU General Public License along with MicroTBX-Modbus.
* If not, see www.gnu.org/licenses/.
*
* \endinternal
****************************************************************************************/

/****************************************************************************************
* Include files
****************************************************************************************/
#include "microtbx.h"                            /* MicroTBX module                    */
#include "microtbxmodbus.h"                      /* MicroTBX-Modbus module             */
#include "tbxmb_checks.h"                        /* MicroTBX-Modbus config checks      */
#include "tbxmb_event_private.h"                 /* MicroTBX-Modbus event private      */
#include "tbxmb_osal_private.h"                  /* MicroTBX-Modbus OSAL private       */
#include "tbxmb_tp_private.h"                    /* MicroTBX-Modbus TP private         */
#include "tbxmb_client_private.h"                /* MicroTBX-Modbus client private     */


/****************************************************************************************
* Macro definitions
****************************************************************************************/
/** \brief Unique context type to identify a context as being a client channel. */
#define TBX_MB_CLIENT_CONTEXT_TYPE     (23U)


/****************************************************************************************
* Function prototypes
****************************************************************************************/
static void TbxMbClientProcessEvent(tTbxMbEvent * event);


/************************************************************************************//**
** \brief     Creates a Modbus client channel object and assigns the specified Modbus
**            transport layer to the channel for packet transmission and reception.
** \param     transport Handle to a previously created Modbus transport layer object to
**            assign to the channel.
** \param     responseTimeout Maximum time in milliseconds to wait for a response from
**            the Modbus server, after sending a PDU.
** \param     turnaroundDelay Delay time in milliseconds after sending a broadcast PDU
**            to give all recipients sufficient time to process the PDU.
** \return    Handle to the newly created Modbus client channel object if successful,
**            NULL otherwise.
**
****************************************************************************************/
tTbxMbClient TbxMbClientCreate(tTbxMbTp transport,
                               uint16_t responseTimeout,
                               uint16_t turnaroundDelay)
{
  tTbxMbClient result = NULL;

  /* Verify parameters. */
  TBX_ASSERT(transport != NULL);

  /* Only continue with valid parameters. */
  if (transport != NULL)
  {
    /* Allocate memory for the new channel context. */
    tTbxMbClientCtx * newClientCtx = TbxMemPoolAllocate(sizeof(tTbxMbClientCtx));
    /* Automatically increase the memory pool, if it was too small. */
    if (newClientCtx == NULL)
    {
      /* No need to check the return value, because if it failed, the following
       * allocation fails too, which is verified later on.
       */
      (void)TbxMemPoolCreate(1U, sizeof(tTbxMbClientCtx));
      newClientCtx = TbxMemPoolAllocate(sizeof(tTbxMbClientCtx));      
    }
    /* Verify memory allocation of the channel context. */
    TBX_ASSERT(newClientCtx != NULL);
    /* Only continue if the memory allocation succeeded. */
    if (newClientCtx != NULL)
    {
      /* Convert the TP channel pointer to the context structure. */
      tTbxMbTpCtx * tpCtx = (tTbxMbTpCtx *)transport;
      /* Sanity check on the transport layer's interface function. That way there is 
       * no need to do it later on, making it more run-time efficient. 
       */
      TBX_ASSERT((tpCtx->transmitFcn != NULL) && (tpCtx->receptionDoneFcn != NULL) &&
                 (tpCtx->getRxPacketFcn != NULL) && (tpCtx->getTxPacketFcn != NULL));
      /* Initialize the channel context. Start by crosslinking the transport layer. */
      newClientCtx->type = TBX_MB_CLIENT_CONTEXT_TYPE;
      newClientCtx->instancePtr = NULL;
      newClientCtx->pollFcn = NULL;
      newClientCtx->processFcn = TbxMbClientProcessEvent;
      newClientCtx->responseTimeout = responseTimeout;
      newClientCtx->turnaroundDelay = turnaroundDelay;
      newClientCtx->responseSem = TbxMbOsalSemCreate();
      newClientCtx->tpCtx = tpCtx;
      newClientCtx->tpCtx->channelCtx = newClientCtx;
      newClientCtx->tpCtx->isClient = TBX_TRUE;
      /* Update the result. */
      result = newClientCtx;
    }
  }
  /* Give the result back to the caller. */
  return result;
} /*** end of TbxMbClientCreate ****/


/************************************************************************************//**
** \brief     Releases a Modbus client channel object, previously created with
**            TbxMbClientCreate().
** \param     channel Handle to the Modbus client channel object to release.
**
****************************************************************************************/
void TbxMbClientFree(tTbxMbClient channel)
{
  /* Verify parameters. */
  TBX_ASSERT(channel != NULL);

  /* Only continue with valid parameters. */
  if (channel != NULL)
  {
    /* Convert the client channel pointer to the context structure. */
    tTbxMbClientCtx * clientCtx = (tTbxMbClientCtx *)channel;
    /* Sanity check on the context type. */
    TBX_ASSERT(clientCtx->type == TBX_MB_CLIENT_CONTEXT_TYPE);
    /* Release the semaphore used for syncing to PDU reception events. */
    TbxMbOsalSemFree(clientCtx->responseSem);
    /* Remove crosslink between the channel and the transport layer. */
    TbxCriticalSectionEnter();
    clientCtx->tpCtx->channelCtx = NULL;
    clientCtx->tpCtx = NULL;
    /* Invalidate the context to protect it from accidentally being used afterwards. */
    clientCtx->type = 0U;
    clientCtx->pollFcn = NULL;
    clientCtx->processFcn = NULL;
    clientCtx->responseSem = NULL;
    TbxCriticalSectionExit();
    /* Give the channel context back to the memory pool. */
    TbxMemPoolRelease(clientCtx);
  }
} /*** end of TbxMbClientFree ***/


/************************************************************************************//**
** \brief     Event processing function that is automatically called when an event for
**            this client channel object was received in TbxMbEventTask().
** \param     event Pointer to the event to process. Note that the event->context points
**            to the handle of the Modbus client channel object.
**
****************************************************************************************/
static void TbxMbClientProcessEvent(tTbxMbEvent * event)
{
  /* Verify parameters. */
  TBX_ASSERT(event != NULL);

  /* Only continue with valid parameters. */
  if (event != NULL)
  {
    /* Sanity check the context. */
    TBX_ASSERT(event->context != NULL);
    /* Convert the event context  to the client channel context structure. */
    tTbxMbClientCtx * clientCtx = (tTbxMbClientCtx *)event->context;
    /* Make sure the context is valid. */
    TBX_ASSERT(clientCtx != NULL);
    /* Only continue with a valid context. */
    if (clientCtx != NULL)
    {
      /* Sanity check on the context type. */
      TBX_ASSERT(clientCtx->type == TBX_MB_CLIENT_CONTEXT_TYPE);
      /* Filter on the event identifier. */
      switch (event->id)
      {
        case TBX_MB_EVENT_ID_PDU_RECEIVED:
        {
          /* Give the PDU received semaphore to synchronize whatever task is waiting
           * for this event.
           */
          TbxMbOsalSemGive(clientCtx->responseSem, TBX_FALSE);
        }
        break;

        case TBX_MB_EVENT_ID_PDU_TRANSMITTED:
        {
          /* At this point no additional event handling is needed on this channel upon
           * PDU transmission completion.
           */
        }
        break;

        default:
        {
          /* An unsupported event was dispatched to us. Should not happen. */
          TBX_ASSERT(TBX_FALSE);
        }
        break;
      }
    }
  }
} /*** end of TbxMbClientProcessEvent ***/


/************************************************************************************//**
** \brief     Helper function to extract an unsigned 16-bit value from the data of a 
**            Modbus packet. Note that unsigned 16-bit values are always store in the
**            big endian format, e.g. 0x1234 is stored as:
**            - data[0] = 0x12
**            - data[1] = 0x34
** \param     data Pointer to the byte array that holds the two bytes to extract, stored
**            in the big endian format.
** \return    The 16-bit unsigned integer value.
**
****************************************************************************************/
static inline uint16_t TbxMbClientExtractUInt16BE(uint8_t const * data)
{
  return ((uint16_t)data[0] << 8U) | data[1];
} /*** end of TbxMbServerExtractUInt16BE ***/


/************************************************************************************//**
** \brief     Helper function to store an unsigned 16-bit value in the data of a Modbus
**            packet. Note that unsigned 16-bit values are always stored in the
**            big endian format, e.g. 0x1234 is stored as:
**            - data[0] = 0x12
**            - data[1] = 0x34
** \param     value The unsigned 16-bit value to store.
** \param     data Pointer to the byte array where to store the value in the big endian
**            format.
**
****************************************************************************************/
static inline void TbxMbClientStoreUInt16BE(uint16_t   value,
                                            uint8_t  * data)
{
  data[0] = (uint8_t)(value >> 8U);
  data[1] = (uint8_t)value;
} /*** end of TbxMbServerExtractUInt16BE ***/



/************************************************************************************//**
** \brief     Helper function to both transmit a request packet and receive the reponse
**            packet, if applicable (unicast).
** \param     clientCtx Pointer to the Modbus client channel for the requested operation.
** \param     isBroadcast TBX_TRUE for sending a broadcast request, TBX_FALSE for
**            unicast.
** \return    TBX_OK if the request packet could be transmitted and (a) a response for
**            the unicast request was received or (b) the turnaround timeout passed after
**            sending the broadcast request. TBX_ERROR otherwise.
**
****************************************************************************************/
static uint8_t TbxMbClientTransceive(tTbxMbClientCtx * clientCtx,
                                     uint8_t           isBroadcast)
{
  uint8_t  result      = TBX_ERROR;
  uint16_t waitTimeout = clientCtx->responseTimeout;

  /* Update the wait time in case it is a broadcast request. */
  if (isBroadcast == TBX_TRUE)
  {
    waitTimeout = clientCtx->turnaroundDelay;
  }

  /* Request the transport layer to transmit the request packet and update the
   * result accordingly.
   */
  result = clientCtx->tpCtx->transmitFcn(clientCtx->tpCtx);
  /* Only continue if the request was successfully submitted for transmission. */
  if (result == TBX_OK)
  {
    /* Wait for the reception of the response from the server, with a timeout. */
    if (TbxMbOsalSemTake(clientCtx->responseSem, waitTimeout) == TBX_FALSE)
    {
      /* Semaphore timeout occured. Either because no response was received, which
       * is an error. Or because the turnaround time after the broadcast request
       * passed, which is okay.
       */
      if (isBroadcast == TBX_FALSE)
      {
        /* Flag the error. */
        result = TBX_ERROR;
      }
    }
  }
  /* Give the result back to the caller. */
  return result;
} /*** end of TbxMbClientTransceive ***/


/************************************************************************************//**
** \brief     Reads the coil(s) from the server with the specified node address.
** \param     channel Handle to the Modbus client channel for the requested operation.
** \param     node The address of the server. This parameter is transport layer
**            dependent. It is needed on RTU/ASCII, yet don't care for TCP unless it is
**            a gateway to an RTU network.
** \param     addr Starting element address (0..65535) in the Modbus data table for the
**            coil read operation.
** \param     num Number to elements to read from the coils data table. Range can be
**            1..2000
** \param     coils Pointer to array with TBX_ON / TBX_OFF values where the coil state
**            will be written to.
** \return    TBX_OK if successful, TBX_ERROR otherwise.
**
****************************************************************************************/
uint8_t TbxMbClientReadCoils(tTbxMbClient   channel,
                             uint8_t        node,
                             uint16_t       addr,
                             uint16_t       num,
                             uint8_t      * coils)
{
  uint8_t result = TBX_ERROR;

  TBX_UNUSED_ARG(addr);

  /* Verify the parameters. */
  TBX_ASSERT((channel != NULL) && (node <= TBX_MB_TP_NODE_ADDR_MAX) && (num >= 1U) &&
             (num <= 2000U) && (coils != NULL));

  /* Only continue with valid parameters. */
  if ((channel != NULL) && (node <= TBX_MB_TP_NODE_ADDR_MAX) && (num >= 1U) &&
      (num <= 2000U) && (coils != NULL))
  {
    /* Convert the client channel pointer to the context structure. */
    tTbxMbClientCtx * clientCtx = (tTbxMbClientCtx *)channel;
    /* Sanity check on the context type. */
    TBX_ASSERT(clientCtx->type == TBX_MB_CLIENT_CONTEXT_TYPE);
    /* TODO Implement TbxMbClientReadCoils(). */
    coils[0] = TBX_OFF; /* Dummy for now. */
  }      
  /* Give the result back to the caller. */
  return result;
} /*** end of TbxMbClientReadCoils ***/


/************************************************************************************//**
** \brief     Reads the discrete input(s) from the server with the specified node
**            address.
** \param     channel Handle to the Modbus client channel for the requested operation.
** \param     node The address of the server. This parameter is transport layer
**            dependent. It is needed on RTU/ASCII, yet don't care for TCP unless it is
**            a gateway to an RTU network.
** \param     addr Starting element address (0..65535) in the Modbus data table for the
**            discrete input read operation.
** \param     num Number to elements to read from the discrete inputs data table. Range
**            can be 1..2000
** \param     inputs Pointer to array with TBX_ON / TBX_OFF values where the discrete
**            input state will be written to.
** \return    TBX_OK if successful, TBX_ERROR otherwise.
**
****************************************************************************************/
uint8_t TbxMbClientReadInputs(tTbxMbClient   channel,
                              uint8_t        node,
                              uint16_t       addr,
                              uint16_t       num,
                              uint8_t      * inputs)
{
  uint8_t result = TBX_ERROR;

  TBX_UNUSED_ARG(addr);

  /* Verify the parameters. */
  TBX_ASSERT((channel != NULL) && (node <= TBX_MB_TP_NODE_ADDR_MAX) && (num >= 1U) &&
             (num <= 2000U) && (inputs != NULL));

  /* Only continue with valid parameters. */
  if ((channel != NULL) && (node <= TBX_MB_TP_NODE_ADDR_MAX) && (num >= 1U) &&
      (num <= 2000U) && (inputs != NULL))
  {
    /* Convert the client channel pointer to the context structure. */
    tTbxMbClientCtx * clientCtx = (tTbxMbClientCtx *)channel;
    /* Sanity check on the context type. */
    TBX_ASSERT(clientCtx->type == TBX_MB_CLIENT_CONTEXT_TYPE);
    /* TODO Implement TbxMbClientReadInputs(). */
    inputs[0] = TBX_OFF; /* Dummy for now. */
  }      
  /* Give the result back to the caller. */
  return result;
} /*** end of TbxMbClientReadInputs ***/


/************************************************************************************//**
** \brief     Reads the input register(s) from the server with the specified node
**            address.
** \param     channel Handle to the Modbus client channel for the requested operation.
** \param     node The address of the server. This parameter is transport layer
**            dependent. It is needed on RTU/ASCII, yet don't care for TCP unless it is
**            a gateway to an RTU network.
** \param     addr Starting element address (0..65535) in the Modbus data table for the
**            input register read operation.
** \param     num Number to elements to read from the input registers data table. Range
**            can be 1..125
** \param     inputRegs Pointer to array where the input register values will be written
**            to.
** \return    TBX_OK if successful, TBX_ERROR otherwise.
**
****************************************************************************************/
uint8_t TbxMbClientReadInputRegs(tTbxMbClient   channel,
                                 uint8_t        node,
                                 uint16_t       addr,
                                 uint8_t        num,
                                 uint16_t     * inputRegs)
{
  uint8_t result = TBX_ERROR;

  /* Verify the parameters. */
  TBX_ASSERT((channel != NULL) && (node <= TBX_MB_TP_NODE_ADDR_MAX) && (num >= 1U) &&
             (num <= 125U) && (inputRegs != NULL));

  /* Only continue with valid parameters. */
  if ((channel != NULL) && (node <= TBX_MB_TP_NODE_ADDR_MAX) && (num >= 1U) &&
      (num <= 125U) && (inputRegs != NULL))
  {
    /* Convert the client channel pointer to the context structure. */
    tTbxMbClientCtx * clientCtx = (tTbxMbClientCtx *)channel;
    /* Sanity check on the context type. */
    TBX_ASSERT(clientCtx->type == TBX_MB_CLIENT_CONTEXT_TYPE);

    /* Obtain write access to the request packet. */
    tTbxMbTpPacket * txPacket = clientCtx->tpCtx->getTxPacketFcn(clientCtx->tpCtx);
    /* Should always work, unless this function is being called recursively. Only
     * continue with access for preparing the request packet.
     */
    if (txPacket != NULL)
    {
      /* Prepare the request packet. */
      txPacket->node = node;
      txPacket->pdu.code = TBX_MB_FC04_READ_INPUT_REGISTERS;
      txPacket->dataLen = 4U;
      /* Starting address. */
      TbxMbClientStoreUInt16BE(addr, &txPacket->pdu.data[0]);
      /* Number of registers. */
      TbxMbClientStoreUInt16BE(num, &txPacket->pdu.data[2]);

      /* Determine the request type (broadcast / unicast). */
      uint8_t isBroadcast = TBX_FALSE;
      if (node == TBX_MB_TP_NODE_ADDR_BROADCAST)
      {
        isBroadcast = TBX_TRUE;
      }
      /* Transmit the request and wait for the response to a unicast request to come in
       * or the turnaround time to pass for a broadcast request.
       */
      result = TbxMbClientTransceive(clientCtx, isBroadcast);

      /* Only continue with processing the response if all is okay so far and the request
       * was unicast.
       */
      if ((result == TBX_OK) && (isBroadcast == TBX_FALSE))
      {
        /* Obtain read access to the response packet. */
        tTbxMbTpPacket * rxPacket = clientCtx->tpCtx->getRxPacketFcn(clientCtx->tpCtx);
        /* Since we just received a response packet, the packet access should always 
         * succeed. Sanity check anyways, just in case.
         */
        TBX_ASSERT(rxPacket != NULL);
        /* Only continue with packet access. */
        if (rxPacket != NULL)
        {
          /* Check that the response came from the expected node, that it's not an
           * exception response and that the data length and the byte count are as
           * expected.
           */
          uint8_t byteCount = rxPacket->pdu.data[0];
          if ((rxPacket->node != node) ||
              ((rxPacket->pdu.code & TBX_MB_FC_EXCEPTION_MASK) != 0U) ||
              (byteCount != (num * 2U)) ||
              (rxPacket->dataLen != (byteCount + 1U)) )
          {
            result = TBX_ERROR;
          }
          /* Response content valid. Process its data. */
          else
          {
            /* Set pointer to where the input registers start in the response. */
            uint8_t const * regValPtr = &rxPacket->pdu.data[1];
            /* Read out and store the input register values. */
            for (uint8_t idx = 0U; idx < num; idx++)
            {
              inputRegs[idx] = TbxMbClientExtractUInt16BE(&regValPtr[idx * 2U]);
            }
          }
        }
        /* Could not access the response packet. */
        else
        {
          result = TBX_ERROR;
        }
        /* Inform the transport layer that were done with the rx packet and no longer
         * need access to it.
         */
        clientCtx->tpCtx->receptionDoneFcn(clientCtx->tpCtx);
      }
    }
  }
  /* Give the result back to the caller. */
  return result;
} /*** end of TbxMbClientReadInputRegs ***/


/************************************************************************************//**
** \brief     Reads the holding register(s) from the server with the specified node
**            address.
** \param     channel Handle to the Modbus client channel for the requested operation.
** \param     node The address of the server. This parameter is transport layer
**            dependent. It is needed on RTU/ASCII, yet don't care for TCP unless it is
**            a gateway to an RTU network.
** \param     addr Starting element address (0..65535) in the Modbus data table for the
**            holding register read operation.
** \param     num Number to elements to read from the holding registers data table. Range
**            can be 1..125
** \param     holdingRegs Pointer to array where the holding register values will be
**            written to.
** \return    TBX_OK if successful, TBX_ERROR otherwise.
**
****************************************************************************************/
uint8_t TbxMbClientReadHoldingRegs(tTbxMbClient   channel,
                                   uint8_t        node,
                                   uint16_t       addr,
                                   uint8_t        num,
                                   uint16_t     * holdingRegs)
{
  uint8_t result = TBX_ERROR;

  TBX_UNUSED_ARG(addr);

  /* Verify the parameters. */
  TBX_ASSERT((channel != NULL) && (node <= TBX_MB_TP_NODE_ADDR_MAX) && (num >= 1U) &&
             (num <= 125U) && (holdingRegs != NULL));

  /* Only continue with valid parameters. */
  if ((channel != NULL) && (node <= TBX_MB_TP_NODE_ADDR_MAX) && (num >= 1U) &&
      (num <= 125U) && (holdingRegs != NULL))
  {
    /* Convert the client channel pointer to the context structure. */
    tTbxMbClientCtx * clientCtx = (tTbxMbClientCtx *)channel;
    /* Sanity check on the context type. */
    TBX_ASSERT(clientCtx->type == TBX_MB_CLIENT_CONTEXT_TYPE);
    /* TODO Implement TbxMbClientReadHoldingRegs(). */
    holdingRegs[0] = 0U; /* Dummy for now. */
  }      
  /* Give the result back to the caller. */
  return result;
} /*** end of TbxMbClientReadHoldingRegs ***/


/************************************************************************************//**
** \brief     Writes the coil(s) to the server with the specified node address.
** \param     channel Handle to the Modbus client channel for the requested operation.
** \param     node The address of the server. This parameter is transport layer
**            dependent. It is needed on RTU/ASCII, yet don't care for TCP unless it is
**            a gateway to an RTU network.
** \param     addr Starting element address (0..65535) in the Modbus data table for the
**            coil write operation.
** \param     num Number to elements to write to the coils data table. Range can be
**            1..1968
** \param     coils Pointer to array with the desired TBX_ON / TBX_OFF coils values.
** \return    TBX_OK if successful, TBX_ERROR otherwise.
**
****************************************************************************************/
uint8_t TbxMbClientWriteCoils(tTbxMbClient         channel,
                              uint8_t              node,
                              uint16_t             addr,
                              uint16_t             num,
                              uint8_t      const * coils)
{
  uint8_t result = TBX_ERROR;

  TBX_UNUSED_ARG(addr);

  /* Verify the parameters. */
  TBX_ASSERT((channel != NULL) && (node <= TBX_MB_TP_NODE_ADDR_MAX) && (num >= 1U) &&
             (num <= 1968U) && (coils != NULL));

  /* Only continue with valid parameters. */
  if ((channel != NULL) && (node <= TBX_MB_TP_NODE_ADDR_MAX) && (num >= 1U) &&
      (num <= 1968U) && (coils != NULL))
  {
    /* Convert the client channel pointer to the context structure. */
    tTbxMbClientCtx * clientCtx = (tTbxMbClientCtx *)channel;
    /* Sanity check on the context type. */
    TBX_ASSERT(clientCtx->type == TBX_MB_CLIENT_CONTEXT_TYPE);
    /* TODO Implement TbxMbClientWriteCoils(). */
  }      
  /* Give the result back to the caller. */
  return result;
} /*** end of TbxMbClientWriteCoils ***/


/************************************************************************************//**
** \brief     Writes the holding register(s) to the server with the specified node
**            address.
** \param     channel Handle to the Modbus client channel for the requested operation.
** \param     node The address of the server. This parameter is transport layer
**            dependent. It is needed on RTU/ASCII, yet don't care for TCP unless it is
**            a gateway to an RTU network.
** \param     addr Starting element address (0..65535) in the Modbus data table for the
**            holding register write operation.
** \param     num Number to elements to write to the holding registers data table. Range
**            can be 1..123
** \param     holdingRegs Pointer to array with the desired holding register values.
** \return    TBX_OK if successful, TBX_ERROR otherwise.
**
****************************************************************************************/
uint8_t TbxMbClientWriteHoldingRegs(tTbxMbClient         channel,
                                    uint8_t              node,
                                    uint16_t             addr,
                                    uint8_t              num,
                                    uint8_t      const * holdingRegs)
{
  uint8_t result = TBX_ERROR;

  TBX_UNUSED_ARG(addr);

  /* Verify the parameters. */
  TBX_ASSERT((channel != NULL) && (node <= TBX_MB_TP_NODE_ADDR_MAX) && (num >= 1U) &&
             (num <= 123U) && (holdingRegs != NULL));

  /* Only continue with valid parameters. */
  if ((channel != NULL) && (node <= TBX_MB_TP_NODE_ADDR_MAX) && (num >= 1U) &&
      (num <= 123U) && (holdingRegs != NULL))
  {
    /* Convert the client channel pointer to the context structure. */
    tTbxMbClientCtx * clientCtx = (tTbxMbClientCtx *)channel;
    /* Sanity check on the context type. */
    TBX_ASSERT(clientCtx->type == TBX_MB_CLIENT_CONTEXT_TYPE);
    /* TODO Implement TbxMbClientWriteHoldingRegs(). */
  }      
  /* Give the result back to the caller. */
  return result;
} /*** end of TbxMbClientWriteHoldingRegs ***/


/*********************************** end of tbxmb_client.c *****************************/
