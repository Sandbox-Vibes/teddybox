#pragma once

/* idle power off time is configured in settings (idle_timeout) */
#define BOOT_OFW_HOLD_TIME 5000000 /* µs both ears held to boot the original firmware */
#define SETUP_EAR_HOLD_MS 3000 /* big ear held while switching on opens the setup page */
#define OTA_CONFIRM_AFTER_US 10000000 /* a new firmware confirms itself after running this long without a crash */
