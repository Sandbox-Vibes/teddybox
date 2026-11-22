#pragma once

#define NFC_TASK_PRIO 5
#define NFC_UID_INVALID 0

/* missed presence checks before a tonie counts as removed; each takes
   ~80-100 ms, so a tonie that does not answer for about 1 s (seen on the
   box: ~0.6 s gap, playback and download restarted) still counts as there */
#define NFC_RETRIES 12
/* RF field duty cycle, see nfc.c */
#define NFC_FIELD_SETTLE_MS 5
#define NFC_SEARCH_INTERVAL_MS 250
#define NFC_TAG_CHECK_INTERVAL_MS 500
#define NFC_RETRY_INTERVAL_MS 50
#define NFC_SEARCH_RESET_EVERY 20

void nfc_init();
uint64_t nfc_get_current_uid();
/* token of the current tag (32 bytes), valid while it is on the box */
uint8_t *nfc_get_current_token();
