#ifndef NFC_DIAG_H__
#define NFC_DIAG_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    volatile uint32_t platform_setup_count;
    volatile uint32_t platform_setup_used_sd;
    volatile uint32_t platform_setup_clear_rc;
    volatile uint32_t platform_setup_prio_rc;
    volatile uint32_t platform_setup_enable_rc;

    volatile uint32_t nfct_irq_count;

    volatile uint32_t t2t_event_none;
    volatile uint32_t t2t_field_on;
    volatile uint32_t t2t_field_off;
    volatile uint32_t t2t_data_read;
    volatile uint32_t t2t_stopped;
    volatile uint32_t t2t_other;
    volatile uint32_t t2t_last_event;
    volatile uint32_t t2t_last_len;

    volatile uint32_t nfct_field_detected;
    volatile uint32_t nfct_field_lost;
    volatile uint32_t nfct_selected;
    volatile uint32_t nfct_rx_framestart;
    volatile uint32_t nfct_rx_frameend;
    volatile uint32_t nfct_tx_framestart;
    volatile uint32_t nfct_tx_frameend;
    volatile uint32_t nfct_error;
    volatile uint32_t nfct_other;
    volatile uint32_t nfct_last_event;

    volatile uint32_t hfclk_request_count;
    volatile uint32_t hfclk_release_count;
    volatile uint32_t hfclk_wait_ready_count;
    volatile uint32_t hfclk_wait_timeout_count;
    volatile uint32_t nfct_activate_count;

    volatile uint32_t cb_request_count;
    volatile uint32_t cb_copy_data_count;
    volatile uint32_t cb_last_ctx_len;
    volatile uint32_t cb_last_data_len;
} nfc_diag_t;

extern volatile nfc_diag_t g_nfc_diag;

void nfc_diag_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* NFC_DIAG_H__ */
