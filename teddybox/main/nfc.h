#pragma once

#define NFC_TASK_PRIO 5
#define NFC_UID_INVALID 0

#define NFC_RETRIES 5
/* RF field duty cycle, see nfc.c */
#define NFC_FIELD_SETTLE_MS 5
#define NFC_SEARCH_INTERVAL_MS 250
#define NFC_TAG_CHECK_INTERVAL_MS 500
#define NFC_RETRY_INTERVAL_MS 50
#define NFC_SEARCH_RESET_EVERY 20

void nfc_init();
uint64_t nfc_get_current_uid();
