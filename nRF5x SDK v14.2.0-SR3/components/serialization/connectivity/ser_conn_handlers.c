/**
 * Copyright (c) 2014 - 2018, Nordic Semiconductor ASA
 * 
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form, except as embedded into a Nordic
 *    Semiconductor ASA integrated circuit in a product or a software update for
 *    such product, must reproduce the above copyright notice, this list of
 *    conditions and the following disclaimer in the documentation and/or other
 *    materials provided with the distribution.
 * 
 * 3. Neither the name of Nordic Semiconductor ASA nor the names of its
 *    contributors may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 * 
 * 4. This software, with or without modification, must only be used with a
 *    Nordic Semiconductor ASA integrated circuit.
 * 
 * 5. Any software provided in binary form under this license must not be reverse
 *    engineered, decompiled, modified and/or disassembled.
 * 
 * THIS SOFTWARE IS PROVIDED BY NORDIC SEMICONDUCTOR ASA "AS IS" AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY, NONINFRINGEMENT, AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL NORDIC SEMICONDUCTOR ASA OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 */
#include <string.h>
#include "app_error.h"
#include "app_scheduler.h"
#include "ser_config.h"
#include "ser_conn_handlers.h"
#include "ser_conn_event_encoder.h"
#include "ser_conn_pkt_decoder.h"
#include "ser_conn_dtm_cmd_decoder.h"
#include "nrf_sdh.h"


/** @file
 *
 * @defgroup ser_conn_handlers Events handlers for the Connectivity Chip.
 * @{
 * @ingroup sdk_lib_serialization
 *
 * @brief   A module to handle the Connectivity application events.
 *
 * @details There are two types of events in the Connectivity application: BLE events generated by
 *          the SoftDevice and events generated by the HAL Transport layer.
 */

/** Parameters of a received packet. */
static ser_hal_transport_evt_rx_pkt_received_params_t m_rx_pkt_received_params;

/** Indicator of received packet that should be process. */
static bool m_rx_pkt_to_process = false;


void ser_conn_hal_transport_event_handle(ser_hal_transport_evt_t event)
{
    switch (event.evt_type)
    {
        case SER_HAL_TRANSP_EVT_TX_PKT_SENT:
        {
            /* SoftDevice event or response to received packet was sent, so unblock the application
             * scheduler to process a next event. */
            app_sched_resume();

            /* Check if chip is ready to enter DTM mode. */
            ser_conn_is_ready_to_enter_dtm();

            break;
        }

        case SER_HAL_TRANSP_EVT_RX_PKT_RECEIVING:
        {
            /* The connectivity side has started receiving a packet. Temporary block processing
             * SoftDevice events. It is going to be unblocked when a response for the packet will
             * be sent. This prevents communication block. */
            app_sched_pause();
            break;
        }

        case SER_HAL_TRANSP_EVT_RX_PKT_RECEIVED:
        {
            /* We can NOT add received packets as events to the application scheduler queue because
             * received packets have to be processed before SoftDevice events but the scheduler
             * queue do not have priorities. */
            memcpy(&m_rx_pkt_received_params, &event.evt_params.rx_pkt_received,
                   sizeof (ser_hal_transport_evt_rx_pkt_received_params_t));
            m_rx_pkt_to_process = true;
            break;
        }

        case SER_HAL_TRANSP_EVT_RX_PKT_DROPPED:
        {
            APP_ERROR_CHECK(SER_WARNING_CODE);
            break;
        }

        case SER_HAL_TRANSP_EVT_PHY_ERROR:
        {
            APP_ERROR_CHECK(NRF_ERROR_FORBIDDEN);
            break;
        }

        default:
        {
            /* do nothing */
            break;
        }
    }
}


uint32_t ser_conn_rx_process(void)
{
    uint32_t err_code = NRF_SUCCESS;

    if (m_rx_pkt_to_process)
    {
        /* No critical section needed on m_rx_pkt_to_process parameter because it is not possible
         * to get next packet before sending a response. */
        m_rx_pkt_to_process = false;
        err_code            = ser_conn_received_pkt_process(&m_rx_pkt_received_params);
    }

    return err_code;
}

#ifdef BLE_STACK_SUPPORT_REQD

NRF_SDH_BLE_OBSERVER(m_ble_observer, 0, ser_conn_ble_event_handle, NULL);

void ser_conn_ble_event_handle(ble_evt_t const * p_ble_evt, void * p_context)
{
    uint32_t err_code = NRF_SUCCESS;

    /* We can NOT encode and send BLE events here. SoftDevice handler implemented in
     * softdevice_handler.c pull all available BLE events at once but we need to reschedule between
     * encoding and sending every BLE event because sending a response on received packet has higher
     * priority than sending a BLE event. Solution for that is to put BLE events into application
     * scheduler queue to be processed at a later time. */
    err_code = app_sched_event_put(p_ble_evt, p_ble_evt->header.evt_len,
                                   ser_conn_ble_event_encoder);
    APP_ERROR_CHECK(err_code);
    uint16_t free_space = app_sched_queue_space_get();
    if (!free_space)
    {
        // Queue is full. Do not pull new events.
        nrf_sdh_suspend();
    }
}
#endif // BLE_STACK_SUPPORT_REQD

#ifdef ANT_STACK_SUPPORT_REQD

NRF_SDH_ANT_OBSERVER(m_ant_observer, 0, ser_conn_ant_event_handle, NULL);

void ser_conn_ant_event_handle(ant_evt_t * p_ant_evt, void * p_context)
{
     uint32_t err_code = NRF_SUCCESS;

    /* We can NOT encode and send ANT events here. SoftDevice handler implemented in
     * softdevice_handler.c pull all available ANT events at once but we need to reschedule between
     * encoding and sending every ANT event because sending a response on received packet has higher
     * priority than sending an ANT event. Solution for that is to put ANT events into application
     * scheduler queue to be processed at a later time. */
    err_code = app_sched_event_put(p_ant_evt, sizeof (ant_evt_t),
                                   ser_conn_ant_event_encoder);
    APP_ERROR_CHECK(err_code);
    uint16_t free_space = app_sched_queue_space_get();
    if (!free_space)
    {
        // Queue is full. Do not pull new events.
        nrf_sdh_suspend();
    }
}
#endif // ANT_STACK_SUPPORT_REQD

/** @} */
