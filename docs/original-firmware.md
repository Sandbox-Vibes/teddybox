# What the original firmware tells us

Findings from the flash dump of one box (ESP32-S3, 8 MB flash), original
firmware `toniebox-esp32-eu` v5.235.0 built with ESP-IDF v5.0.1. No keys,
passwords, network names or device identifiers are reproduced here.

The original firmware has almost no readable log texts (its own logger), so
constants like buffer sizes, timings or the CPU clock cannot be read from
strings; that would need disassembly.

## Flash layout

| Partition | Offset | Size | Content in the dump |
|---|---|---|---|
| phy_init | 0xa000 | 4K | RF calibration |
| nvs | 0xb000 | 16K | settings, WiFi (see below) |
| assets | 0xf000 | 1408K | FAT: `CERT/` (ca, client, private .der), `FW/sfx.bin` (system sounds, 882 KB), `WWW/` (setup page, the directory entry is damaged in this dump) |
| otadata | 0x16f000 | 8K | |
| ota_0 | 0x180000 | 2176K | `toniebox-esp32-pd` v5.235.0 (production firmware, see below) |
| ota_1 | 0x3a0000 | 2176K | `toniebox-esp32-eu` v5.235.0 |
| ota_2 | 0x5c0000 | 2176K | `toniebox-esp32-eu` v5.235.0 (identical to ota_1) |
| coredump | 0x7e0000 | 128K | empty: the original never crashed on this box |

The bootloader is Boxine's own (IDF 5.0.1); settings of our bootloader build
have no effect.

## NVS (namespaces and keys)

- `TB_META`: `VERSION`, `FACTORY_RESET`, `FIRST_BOOT_EU`, `SLEEP_ACTIVE`,
  `HW_REVISION` (6 byte blob, all zero on this box), `CHECKRIDE_PD`
- `TB_SETTINGS`: `BAT_CRITICAL` 3600, `BAT_LOW_LEVEL_D` 3670,
  `BAT_LOW_LEVEL_U` 3700 (mV), `LANG_ID`, `FAC_LANG_ID`, `REGION` "EU"
- `TB_OTA`: slot roles `EU` = ota_2, `PD` = ota_0, `HWTEST` = ota_0,
  `FREE` = ota_1, plus SHA1s and timestamps (`PD_TIME`, `WEB_HTML_TIME`,
  `SFX_TIME`)
- `TB_FILES`: `KEY_TIME`, `CERT_TIME`
- `TB_WIFI`: `INDEX`, `SSID0/1`, `PW0/1` (two stored networks)
- `nvs.net80211`: IDF WiFi driver data

The original writes `SLEEP_ACTIVE` to NVS on every sleep/wake (dozens of
erased entries); TeddyBox avoids NVS writes.

## The production firmware (`toniebox-esp32-pd`, ota_0)

Factory firmware: registers the box and writes tonies (`/v1/pd/register`,
`/v1/pd/tagging`, `/v1/pd/tagging/secret`, `/v1/pd/content/`,
`/v1/pd/pass`, `/v1/certificate`, host `fcs.tbs.toys`), hardware test mode
(`HWTEST`, `MSG_TEST_MODE`), uses `ISO15693_WriteSingleBlock`. Not useful
for TeddyBox and never to be contacted. On a box running TeddyBox in ota_0
it is overwritten; the dump is then the only copy.

Internal message names (only readable in the pd build):
`MSG_INIT`, `MSG_BATTERY_LOW`, `MSG_POWER_NEAR_EMPTY`,
`MSG_ENTER/EXIT_LOW_BATTERY_MODE`, `MSG_ENTER/EXIT_OFFLINE_MODE`,
`MSG_CHARGER_REMOVED_POWER_EMPTY/OK`, `MSG_DOWNLOAD_FINISHED`,
`MSG_AP_MODE_EXIT`, `MSG_PULSE_BRIGHTNESS`, `MSG_LONG_ERROR`,
`LEDSTATE_MAT_RECONNECTING`, `MAT_BATTERY_LOW/OK`; "SD task has been stuck
for more than %lu ms".

## Structure of the original (`toniebox-esp32-eu`)

Source files named in the binary:

- tasks: `main`, `tb_task`, `tb_task_coop`, `tb_supervisor`, `hmi_task`
  (ears), `led_task`, `nfc_task`, `sd_task`, `dl_task`, `rtnl_task`
- buffers: `dl2sd_double_buffer` (download → SD), `sd2pl_double_buffer`
  (SD → player), `pl2iis_double_buffer` (player → I2S). SD access runs in its
  own task, decoupled from decoding by double buffers.
- audio: `CO3100.c` (DAC3100), `pcm_handler.c`, `sfx.c`, and a bundled
  libopus with **only the CELT part** (`celt/*.c` + `src/opus_decoder.c`, no
  SILK): tonie files are CELT only.
- sensors: `mems.c` with **`LIS3DH.c` and `MMA8451.c`**: there are box
  variants with an MMA8451 accelerometer (TeddyBox supports LIS3DH only).
- NFC: `trf7962a.c`, `iso15693.c`, `nxp_icode_slix.c`
- network: `net.c`, `netman.c`, `wifi.c`, `tbs.c`, `ota_tbs.c`,
  `http_client.c`, `tcpclient.c`; setup hotspot name `Toniebox-%02X%02X`,
  setup page `/assets/www/index.html` with `/toniebox.js`

## Cloud requests of the original

`/v1/time`, `/v1/freshness-check`, `/v1/content/`, `/v2/content/`,
`/v1/claim/`, `/v1/log`, `/v1/ota/%d?cv=%lu`, header `X-Toniebox: %s`;
hosts `prod.de.tbs.toys`, `rtnl.bxcl.de` (RTNL live log). teddyCloud
implements all of them (`handleCloudClaim`, `handleCloudLog`, RTNL decoding).

`/v1/claim/<ruid>` is sent when a tonie is placed; teddyCloud marks the tonie
"claimed" and keeps its cloud auth. TeddyBox did not send it before.

## SD card layout of the original

- `content/<dir>/<file>.tmp`: downloads go to a `.tmp` file that is renamed
  when complete, so a partial download never looks like a tonie
- `trashcan/`: removed content
- `badblock`: list of bad SD blocks
- `update/sfx.bin`: system sounds update from the SD card
- `PM1.LOG`, `PM2.LOG`: logs of the original firmware

## Used in TeddyBox

- battery thresholds and language from NVS (read only)
- LIS3DH click configuration (slap) and the system sounds from the assets
- certificates from the assets
- `.tmp` downloads (since this analysis)
- `/v1/claim` implemented but off by default (`claim = 1`): teddyCloud
  forwards claims to the Boxine cloud when its cloud access is on, which
  would report every placed tonie; Home Assistant shows the tonie anyway
- CELT-only decoder (`decoder = celt`, default since 2026-11 after a listening test on the box; ESP-ADF stays as `decoder = adf` fallback)
