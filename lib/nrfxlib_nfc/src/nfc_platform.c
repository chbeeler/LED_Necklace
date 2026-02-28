#include "nfc_platform.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include "nrf.h"

static nfc_lib_cb_resolve_t m_nfc_cb_resolve;

/* Allocate enough for Type 4T (2 x 259 bytes); Type 2T needs only 16 bytes. */
static uint8_t m_nfc_platform_buffer[NFC_PLATFORM_T4T_BUFFER_SIZE * 2U];

int nfc_platform_setup(nfc_lib_cb_resolve_t nfc_lib_cb_resolve, uint8_t * p_irq_priority)
{
    if ((nfc_lib_cb_resolve == NULL) || (p_irq_priority == NULL)) {
        return -EINVAL;
    }

    m_nfc_cb_resolve = nfc_lib_cb_resolve;
    *p_irq_priority = 7;

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

    if (p_event->evt_id == NRFX_NFCT_EVT_FIELD_DETECTED) {
        /* HFCLK is typically already managed by SoftDevice in BLE builds. */
        nrfx_nfct_state_force(NRFX_NFCT_STATE_ACTIVATED);
    }
}

void nfc_platform_cb_request(const void    * p_ctx,
                             size_t          ctx_len,
                             const uint8_t * p_data,
                             size_t          data_len,
                             bool            copy_data)
{
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
