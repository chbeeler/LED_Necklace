#include "nfc_platform.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "nrf.h"
#include "nrf_sdm.h"
#include "nrf_soc.h"
#include "nrf_nvic.h"
#include "nfc_diag.h"

static nfc_lib_cb_resolve_t m_nfc_cb_resolve;

/* Allocate enough for Type 4T (2 x 259 bytes); Type 2T needs only 16 bytes. */
static uint8_t m_nfc_platform_buffer[NFC_PLATFORM_T4T_BUFFER_SIZE * 2U];
static bool m_hfclk_requested;
volatile nfc_diag_t g_nfc_diag;

#define HFCLK_WAIT_LOOPS 200000UL

static bool softdevice_enabled(void)
{
    uint8_t enabled = 0;
    uint32_t err = sd_softdevice_is_enabled(&enabled);

    return (err == NRF_SUCCESS) && (enabled != 0U);
}

static bool hfclk_running(void)
{
    if (softdevice_enabled()) {
        uint32_t running = 0;
        if (sd_clock_hfclk_is_running(&running) == NRF_SUCCESS) {
            return (running != 0U);
        }
    }

    uint32_t state_masked = (NRF_CLOCK->HFCLKSTAT & CLOCK_HFCLKSTAT_STATE_Msk);
    uint32_t src_masked = (NRF_CLOCK->HFCLKSTAT & CLOCK_HFCLKSTAT_SRC_Msk);

    return (state_masked == (CLOCK_HFCLKSTAT_STATE_Running << CLOCK_HFCLKSTAT_STATE_Pos)) &&
           (src_masked == (CLOCK_HFCLKSTAT_SRC_Xtal << CLOCK_HFCLKSTAT_SRC_Pos));
}

static void hfclk_request(void)
{
    if (m_hfclk_requested) {
        return;
    }

    if (softdevice_enabled()) {
        (void)sd_clock_hfclk_request();
    } else {
        NRF_CLOCK->EVENTS_HFCLKSTARTED = 0;
        NRF_CLOCK->TASKS_HFCLKSTART = 1;
    }

    m_hfclk_requested = true;
}

static void hfclk_release(void)
{
    if (!m_hfclk_requested) {
        return;
    }

    if (softdevice_enabled()) {
        (void)sd_clock_hfclk_release();
    } else {
        NRF_CLOCK->TASKS_HFCLKSTOP = 1;
    }

    m_hfclk_requested = false;
}

static bool hfclk_wait_ready(void)
{
    uint32_t loops = HFCLK_WAIT_LOOPS;

    while (loops-- > 0U) {
        if (hfclk_running()) {
            return true;
        }
        __NOP();
    }

    return false;
}

void nfc_diag_reset(void)
{
    (void)memset((void *)&g_nfc_diag, 0, sizeof(g_nfc_diag));
}

int nfc_platform_setup(nfc_lib_cb_resolve_t nfc_lib_cb_resolve, uint8_t * p_irq_priority)
{
    if ((nfc_lib_cb_resolve == NULL) || (p_irq_priority == NULL)) {
        return -EINVAL;
    }

    m_nfc_cb_resolve = nfc_lib_cb_resolve;
    *p_irq_priority = 7;
    m_hfclk_requested = false;
    nfc_diag_reset();
    g_nfc_diag.platform_setup_count++;

    // Platform integration requirement: hook and enable NFCT IRQ in this runtime.
    if (softdevice_enabled()) {
        g_nfc_diag.platform_setup_used_sd = 1;
        g_nfc_diag.platform_setup_clear_rc = sd_nvic_ClearPendingIRQ(NFCT_IRQn);
        g_nfc_diag.platform_setup_prio_rc = sd_nvic_SetPriority(NFCT_IRQn, *p_irq_priority);
        g_nfc_diag.platform_setup_enable_rc = sd_nvic_EnableIRQ(NFCT_IRQn);
    } else {
        g_nfc_diag.platform_setup_used_sd = 0;
        NVIC_ClearPendingIRQ(NFCT_IRQn);
        NVIC_SetPriority(NFCT_IRQn, *p_irq_priority);
        NVIC_EnableIRQ(NFCT_IRQn);
        g_nfc_diag.platform_setup_clear_rc = NRF_SUCCESS;
        g_nfc_diag.platform_setup_prio_rc = NRF_SUCCESS;
        g_nfc_diag.platform_setup_enable_rc = NRF_SUCCESS;
    }

    return 0;
}

int nfc_platform_nfcid1_default_bytes_get(uint8_t * const p_nfcid1_buff,
                                          uint32_t        nfcid1_buff_len)
{
    if (p_nfcid1_buff == NULL) {
        return -EINVAL;
    }

    if ((nfcid1_buff_len != NRFX_NFCT_NFCID1_SINGLE_SIZE) &&
        (nfcid1_buff_len != NRFX_NFCT_NFCID1_DOUBLE_SIZE) &&
        (nfcid1_buff_len != NRFX_NFCT_NFCID1_TRIPLE_SIZE)) {
        return -E2BIG;
    }

    uint32_t tag_header0 = NRF_FICR->NFC.TAGHEADER0;
    uint32_t tag_header1 = NRF_FICR->NFC.TAGHEADER1;
    uint32_t tag_header2 = NRF_FICR->NFC.TAGHEADER2;

    p_nfcid1_buff[0] = (uint8_t)(tag_header0 >> 0);
    p_nfcid1_buff[1] = (uint8_t)(tag_header0 >> 8);
    p_nfcid1_buff[2] = (uint8_t)(tag_header0 >> 16);
    p_nfcid1_buff[3] = (uint8_t)(tag_header1 >> 0);

    if (nfcid1_buff_len != NRFX_NFCT_NFCID1_SINGLE_SIZE) {
        p_nfcid1_buff[4] = (uint8_t)(tag_header1 >> 8);
        p_nfcid1_buff[5] = (uint8_t)(tag_header1 >> 16);
        p_nfcid1_buff[6] = (uint8_t)(tag_header1 >> 24);

        if (nfcid1_buff_len == NRFX_NFCT_NFCID1_TRIPLE_SIZE) {
            p_nfcid1_buff[7] = (uint8_t)(tag_header2 >> 0);
            p_nfcid1_buff[8] = (uint8_t)(tag_header2 >> 8);
            p_nfcid1_buff[9] = (uint8_t)(tag_header2 >> 16);
        } else if (p_nfcid1_buff[3] == 0x88) {
            /* nRF52 errata 181 workaround for 7-byte NFCID1. */
            p_nfcid1_buff[3] |= 0x11;
        }
    }

    return 0;
}

void nfc_platform_event_handler(nrfx_nfct_evt_t const * p_event)
{
    if (p_event == NULL) {
        return;
    }

    switch (p_event->evt_id) {
    case NRFX_NFCT_EVT_FIELD_DETECTED:
        g_nfc_diag.nfct_field_detected++;
        g_nfc_diag.nfct_last_event = NRFX_NFCT_EVT_FIELD_DETECTED;
        hfclk_request();
        g_nfc_diag.hfclk_request_count++;
        if (hfclk_wait_ready()) {
            g_nfc_diag.hfclk_wait_ready_count++;
            nrfx_nfct_state_force(NRFX_NFCT_STATE_ACTIVATED);
            g_nfc_diag.nfct_activate_count++;
        } else {
            g_nfc_diag.hfclk_wait_timeout_count++;
        }
        break;

    case NRFX_NFCT_EVT_FIELD_LOST:
        g_nfc_diag.nfct_field_lost++;
        g_nfc_diag.nfct_last_event = NRFX_NFCT_EVT_FIELD_LOST;
        hfclk_release();
        g_nfc_diag.hfclk_release_count++;
        break;

    case NRFX_NFCT_EVT_SELECTED:
        g_nfc_diag.nfct_selected++;
        g_nfc_diag.nfct_last_event = NRFX_NFCT_EVT_SELECTED;
        break;

    case NRFX_NFCT_EVT_RX_FRAMESTART:
        g_nfc_diag.nfct_rx_framestart++;
        g_nfc_diag.nfct_last_event = NRFX_NFCT_EVT_RX_FRAMESTART;
        break;

    case NRFX_NFCT_EVT_RX_FRAMEEND:
        g_nfc_diag.nfct_rx_frameend++;
        g_nfc_diag.nfct_last_event = NRFX_NFCT_EVT_RX_FRAMEEND;
        break;

    case NRFX_NFCT_EVT_TX_FRAMESTART:
        g_nfc_diag.nfct_tx_framestart++;
        g_nfc_diag.nfct_last_event = NRFX_NFCT_EVT_TX_FRAMESTART;
        break;

    case NRFX_NFCT_EVT_TX_FRAMEEND:
        g_nfc_diag.nfct_tx_frameend++;
        g_nfc_diag.nfct_last_event = NRFX_NFCT_EVT_TX_FRAMEEND;
        break;

    case NRFX_NFCT_EVT_ERROR:
        g_nfc_diag.nfct_error++;
        g_nfc_diag.nfct_last_event = NRFX_NFCT_EVT_ERROR;
        break;

    default:
        g_nfc_diag.nfct_other++;
        g_nfc_diag.nfct_last_event = (uint32_t)p_event->evt_id;
        break;
    }
}

void nfc_platform_cb_request(const void    * p_ctx,
                             size_t          ctx_len,
                             const uint8_t * p_data,
                             size_t          data_len,
                             bool            copy_data)
{
    g_nfc_diag.cb_request_count++;
    g_nfc_diag.cb_last_ctx_len = (uint32_t)ctx_len;
    g_nfc_diag.cb_last_data_len = (uint32_t)data_len;
    if (copy_data) {
        g_nfc_diag.cb_copy_data_count++;
    }

    (void)ctx_len;
    (void)data_len;
    (void)copy_data;

    if (m_nfc_cb_resolve != NULL) {
        m_nfc_cb_resolve(p_ctx, p_data);
    }
}

uint8_t * nfc_platform_buffer_alloc(size_t size)
{
    if (size > sizeof(m_nfc_platform_buffer)) {
        return NULL;
    }

    return m_nfc_platform_buffer;
}

void nfc_platform_buffer_free(uint8_t * p_buffer)
{
    (void)p_buffer;
}
