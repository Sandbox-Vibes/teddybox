#pragma once

/* idle power off time is configured in settings (idle_timeout) */
#define BOOT_OFW_HOLD_TIME 5000000 /* µs both ears held (box upside down) to boot the original firmware */
#define BOOT_OFW_RELEASE_TIME 10000000 /* µs to let go of the ears afterwards, else nothing happens */
#define SETUP_EAR_HOLD_MS 3000 /* big ear held while switching on opens the setup page */
/* heap and stack report in the serial log */
#define MEMDIAG_INTERVAL_US (30 * 1000000LL)
#define OTA_CONFIRM_AFTER_US 10000000 /* a new firmware confirms itself after running this long without a crash */
