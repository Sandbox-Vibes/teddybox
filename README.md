# TeddyBox

Custom firmware for the ESP32-S3 based Toniebox. It runs from a free OTA
slot next to the original Boxine firmware, which stays untouched and can be
booted again at any time.

## What works

- Playback of Tonie content (TAF files) from the SD card, seeking, chapters
- NFC (TRF7962A), ears for volume, headphone detection, LEDs, accelerometer
- **WiFi from the original firmware**: networks stored by the Tonies app
  (NVS `TB_WIFI`, `SSID0..6`/`PW0..6`) are used directly, newest first.
  WPS push button as fallback, credentials are stored in the same format.
- **System sounds from the box itself**: `fw/sfx.bin` of the assets partition
  holds all 25 voice lines (startup jingle, low battery, ...). On boot they
  are copied once to `/CONTENT/<lang>/<id>` on the SD card. Files that
  already exist are never overwritten, so custom sounds can be placed there.
- **Language and battery thresholds from the original settings**
  (NVS `TB_SETTINGS`: `LANG_ID`/`FAC_LANG_ID`, `BAT_CRITICAL`, `BAT_LOW_LEVEL_D/U`)
- **Battery monitoring** (GPIO9, 100k/33k divider) and charger detection
  (GPIO8, 100k/100k): warning sound when low (never interrupts a story),
  announcement and power off when critical
- **Back to the original firmware without a cable**: hold both ears for
  5 seconds
- Content download from teddyCloud (`/v2/content`, resume via `Range`),
  tag password read from the tag and sent as `Authorization: BD <hex>`
- **Freshness check** like the original firmware: changed content (new
  episodes, creative tonies, live content) is detected and downloaded again
- **Home Assistant** via MQTT discovery (see below)
- Firmware updates through the setup page with rollback, never touching the original firmware

## Setup page

On the first boot (no `teddybox.ini` on the SD card yet), or when the big
ear is held for 3 seconds while switching on, the box says the installation
announcement, blinks blue/green and opens the WiFi hotspot `TeddyBox-XXXX`
(no password, only while setup runs). Phones show the setup page
automatically, otherwise open `http://192.168.4.1/`. It configures WiFi
(with network scan), teddyCloud (address, port, CA upload as DER or PEM),
Home Assistant (MQTT) and volume, language and power off time. Saving
writes `teddybox.ini`, stores the WiFi in the original firmware's format and
restarts. Unused for 15 minutes, the box switches off. Needs about 18 KB RAM;
cloud, MQTT and NFC do not run in setup mode.

## Configuration: `teddybox.ini` on the SD card

All keys are optional.

```ini
# teddyCloud host or IP; the cloud stays off while this is empty
cloud_host = 192.168.1.20
cloud_port = 443
# CA of your teddyCloud (certs/server/ca.der). The CA in flash only works
# if it was already replaced by the teddyCloud patch
ca_path = /sdcard/teddybox/ca.der
# cloud_enabled = 0        # force off even with a host set

# Home Assistant / MQTT; off while mqtt_host is empty
mqtt_host = 192.168.1.10
mqtt_port = 1883
mqtt_user = teddybox
mqtt_pass = secret
# mqtt_topic = teddybox/<mac>          # default
# ha_discovery_prefix = homeassistant  # default

lang_id = 1                # system sounds: 0=EN-GB 1=DE 2=EN-US 3=FR
volume_max = 80            # percent
volume_default = 50
idle_timeout = 60          # seconds until power off
```

The cloud is off by default on purpose: the box certificate would otherwise
be used against the Boxine cloud by a non-original firmware.

### Optional: Boxine fallback and original user agent

Both are off by default and can be switched on in the setup page
("Erweitert") or in `teddybox.ini`:

```ini
boxine_fallback = 1          # use the Boxine cloud when teddyCloud is missing or down
boxine_host = prod.de.tbs.toys
user_agent_original = 1      # send "toniebox-esp32-eu/v5.x" (read from the original app)
```

- With `boxine_fallback`, the box tries teddyCloud first and uses the Boxine
  cloud only if teddyCloud does not answer; every 5 minutes it checks whether
  teddyCloud is back. Boxine connections use the original CA from the assets
  partition, send SNI and verify the server name, like the original firmware.
- teddyCloud passes the box's user agent on to Boxine. Without
  `user_agent_original`, Boxine therefore sees `toniebox-esp32-teddybox/...`
  whenever teddyCloud fetches content for TeddyBox.
- The user agent is not the only difference: TLS handshake (mbedTLS 2.28 from
  IDF 4.4 instead of the original's IDF 5.0) and behaviour (no log upload, no
  firmware checks) differ as well. If Boxine blocks the box certificate, the
  Boxine cloud stops working for the original firmware and teddyCloud too.

## teddyCloud

TeddyBox uses the same protocol as the original firmware: `GET /v1/time`,
`GET /v2/content/<rUID>` with `Authorization: BD <tag memory>` and `Range`
for resuming. teddyCloud identifies it as an ESP32 box by the user agent
`toniebox-esp32-teddybox/<version>`. Requirements:

- `cloud_host` set to teddyCloud in `teddybox.ini`
- teddyCloud's CA (`certs/server/ca.der`) on the SD card, `ca_path` pointing to it
- the box certificates known to teddyCloud (`certs/client/<mac>/`), as for the original firmware

### Freshness check

After boot (as soon as teddyCloud is reachable) and every 6 hours the box
sends UID and `audio_id` of every tonie on the SD card to
`POST /v1/freshness-check`. teddyCloud answers with the tonies that have new
content: a newer version in its library, a newer version in the Boxine
cloud (if cloud access is enabled in teddyCloud) or live content. Those
files are renamed to `<file>.old`; the next time the tonie is placed, the new
version is downloaded. If that fails (no WiFi, teddyCloud down), the `.old`
version is played instead. `.old` files are removed once the new version is
complete. A tonie that is playing during the check is handled next time.
Up to 128 tonies per check.

## Home Assistant

With `mqtt_host` set, the box registers itself via MQTT discovery as device
"TeddyBox" with these entities (`<id>` = `teddybox_<mac>`):

| Entity | |
|---|---|
| `sensor.<id>_status` | playing / idle |
| `sensor.<id>_tonie` | UID of the current tonie |
| `sensor.<id>_chapter`, `_chapters` | current chapter, number of chapters |
| `number.<id>_volume` | volume, settable, limited by `volume_max` |
| `sensor.<id>_battery` | charge, rough estimate from the voltage |
| `sensor.<id>_battery_voltage` | battery voltage |
| `binary_sensor.<id>_charging`, `_battery_low` | on the charger, battery low |
| `binary_sensor.<id>_headphones` | headphones connected |
| `sensor.<id>_rssi` | WiFi signal |
| `button.<id>_stop`, `_next`, `_prev`, `_poweroff` | control |

Topics: `teddybox/<mac>/state` (JSON), `teddybox/<mac>/availability`
(`online`/`offline`, also as last will) and `teddybox/<mac>/cmd/<command>`.
The box powers off when idle (`idle_timeout`), Home Assistant then shows it
as unavailable. Needs about 16 KB of RAM.

## Audio

Output and volume work as in the original firmware (driver `CO3100.c`,
read from the flash dump):

- speaker: both channels mixed to mono (the class-D amplifier only gets
  the left DAC, so stereo content used to lose its right channel), driver
  gain 6 dB
- headphones: stereo, the speaker is powered down while they are plugged in
  (and the other way round)
- 16 volume steps per output, speaker -20 dB to +2.5 dB, headphones -20 dB
  to -2 dB; each ear press moves one step. The percent values in Home
  Assistant and in the settings map onto these steps.
- speaker EQ in the DAC's own biquads (no CPU load), headphones stay flat.
  `speaker_eq` in teddybox.ini, on the setup page and as a select in Home
  Assistant (applied immediately, saved to the INI):
  - `full` (default): 4th order Butterworth high-pass at 150 Hz, -2.5 dB at
    400 Hz (Q 1.0), -1.5 dB at 3.2 kHz (Q 1.4)
  - `lowcut`: only the high-pass
  - `off`: flat, as the original firmware
  The speaker is specified for 180 Hz - 15 kHz; the high-pass removes what
  it cannot play anyway and what makes it distort at high volume. The two
  cuts are a best guess for a small closed enclosure, not measured. The EQ
  only cuts, so it cannot clip. `tests/host/test_speaker_eq` checks the
  response computed from the rounded 16 bit coefficients.
- checked against a register model: `tests/host/dac_sim`

## Home Assistant: progress and LED

- `Fortschritt` (% of the tonie), `Kapitel-Fortschritt`, `Gespielt` and
  `Gesamtdauer` (minutes, from the Ogg granule positions; every TAF block
  starts with an Ogg page, the duration is the last page of the file) and
  `Download` (% of a running download). Unknown values are sent as null and
  show as "unknown". Progress alone goes out at most every 10 s.
- `LED-Helligkeit` 0 (off) to 3, also `led_brightness` in teddybox.ini and on
  the setup page. The setup mode lights up at level 1 at least.

## SD card errors

The SD card occasionally times out (`0x107`), with or without a download.
FATFS then marks the open file as broken, so every later access fails.

- Downloads are written in 4 KiB blocks aligned to the file offset. A failed
  write reopens the file and retries up to 3 times. `received` only counts
  data that reached the card, so a resume continues at the right place.
- Playback reopens and retries failed reads. While the tonie is still
  downloading it waits up to 3 s for data instead of treating it as the end.
- Opus PCM ring buffer is 8 KiB.
- Log lines: `SD write … ms/s` (download line), `SD write/read at … failed,
  reopening`, `Waited … for the download's file lock`,
  `Playback caught up with the download`.
- `tests/host` does not cover this; the download path was tested on a PC
  against a local teddyCloud with injected write errors.

## Power

- The NFC reader's RF field (70-130 mA while on) is only switched on for the
  moments a tag is queried: every 250 ms without a tonie, every 500 ms with
  one. A Tonie returns to privacy mode without field, so each check unlocks
  it again with the password that worked before. Simulated with a virtual
  Tonie (`tests/host/nfc_sim`): field on 3-4 % of the time instead of
  65-100 %, tonie removal noticed after about 0.7 s.
- Peripheral power and SD card power are held off during deep sleep
  (digital pads would lose their level otherwise).
- Idle power off after 60 s by default (`idle_timeout`).

## Network traffic

Without `boxine_fallback` the box only talks to what is configured:
teddyCloud (`cloud_host`) and the MQTT broker (`mqtt_host`), plus WiFi,
DHCP, ARP and DNS lookups for those host names in the local network. No
time servers (the time comes from teddyCloud), no mDNS, no telemetry.
It shows up in the router as `teddybox-xxxx`. Inbound, nothing listens
in normal mode. Checked with a
packet capture in QEMU (Ethernet instead of WiFi). Note that teddyCloud
itself contacts the Boxine cloud if its `cloud.enabled` is on.

## Flash layout

Identical to the original firmware, the partition table is not changed:

| Slot   | Address  | Original firmware            | With TeddyBox |
|--------|----------|------------------------------|---------------|
| ota_0  | 0x180000 | `toniebox-esp32-pd` (factory/fallback) | unchanged |
| ota_1  | 0x3a0000 | free (`TB_OTA/FREE`)         | **TeddyBox**  |
| ota_2  | 0x5c0000 | `toniebox-esp32-eu`          | unchanged     |

Check your own layout first (`tools/make_otadata.py` prints the current
slots, NVS `TB_OTA` lists `EU`, `PD`, `FREE`). The PD app takes over when the
EU app is missing or invalid and then demands an update ("Codewort
Murmeltier"), so neither of them may be overwritten.

## Build

Tested on Linux with ESP-ADF v2.6 and its bundled ESP-IDF v4.4.4.

```sh
./setup_libs.sh            # clones esp-adf v2.6, applies patches/malloc.diff
. ./env_vars.sh
cd teddybox && idf.py build
```

## Install (into the free slot)

Make a full backup first and keep it safe (it contains the box certificate):

```sh
esptool.py -b 921600 read_flash 0x0 0x800000 backup.bin
python3 tools/make_otadata.py backup.bin 1 otadata_teddybox.bin
esptool.py -b 921600 write_flash 0x3a0000 teddybox/build/teddybox.bin 0x16f000 otadata_teddybox.bin
```

Only the free slot and the 8 KiB boot selection are written. Bootloader,
partition table, NVS, certificates and both original apps stay as they are.

Back to the original firmware: hold both ears for 5 seconds, or

```sh
dd if=backup.bin of=otadata_original.bin bs=4096 skip=367 count=2
esptool.py write_flash 0x16f000 otadata_original.bin
```

## Tests

```sh
make -C tests/host     # host unit tests, no ESP-IDF needed
```

## Firmware updates

Updates of TeddyBox itself are uploaded on the setup page (hold the big ear
while switching on, section "Firmware aktualisieren"). There is no network
port for updates, so physical access to the box is required.

- The upload is checked before anything is written: ESP32-S3 image with
  project name `teddybox` (original Boxine apps are refused), fits the slot,
  SHA-256 verified at the end.
- The first update needs the slot of the PD app (stock layout: PD,
  TeddyBox, EU app); the page asks before using it. After that TeddyBox
  alternates between the two slots, the EU app is never touched.
- Rollback: the original bootloader supports it. A new version boots "on
  probation" and confirms itself after running 10 seconds (or once the setup
  page is up). If it crashes or restarts before that, the bootloader starts
  the previous version again.
- Switching to the original firmware (both ears) marks it valid right away,
  so it does not fall back to TeddyBox after its first restart.

Tested in QEMU with the original bootloader from a flash dump: refused
images, PD consent, update, rollback of an unconfirmed version, alternating
updates, switch to the original firmware.
