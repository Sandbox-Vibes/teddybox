# TeddyBox

Custom firmware for the ESP32-S3 based Toniebox, for use with
[teddyCloud](https://github.com/toniebox-reverse-engineering/teddycloud).
It runs in the OTA slots next to the original Boxine firmware (`toniebox-esp32-eu`),
which is never touched and can be booted again at any time: box upside down,
hold both ears for 5 seconds, let go.

Built with ESP-IDF 5.5 and ESP-ADF 2.8. Tested on one box (ESP32-S3, 8 MB
flash, LIS3DH accelerometer) against teddyCloud.

## TeddyBox compared with the original firmware

| | Original firmware (`toniebox-esp32-eu` v5.235) | TeddyBox |
|---|---|---|
| Content source | Boxine cloud, or teddyCloud with a patched CA | teddyCloud only; Boxine cloud as optional fallback (`boxine_fallback`, off) |
| Talks to Boxine | yes: content, time, claim, log upload (`/v1/log`), firmware checks (`/v1/ota`), live log (`rtnl.bxcl.de`) | no (teddyCloud may, if its own cloud access is on) |
| Telemetry, logs to the cloud | yes | none |
| New tonie, never played before | loaded from the Boxine cloud | loaded through teddyCloud, which needs cloud access for it; a hint sound if that fails |
| Tonie placed is reported (claim) | always | off by default (`claim = 1`) |
| Freshness check (new episodes, changed content) | yes | yes, same protocol |
| Downloads | to `.tmp`, renamed when complete | the same; plus SHA-1 check of every file against its header, damaged files are downloaded again |
| Nightly content update | — | optional, at a set hour, on the charger or from 50 % battery |
| Opus decoder | libopus, CELT only | the same (libopus 1.5.2, CELT only, fixed point), ESP-ADF as fallback |
| SD card hangs during playback | own SD task with double buffers | own read-ahead task, 32 KB (about 2.7 s) |
| Setup | Tonies app, hotspot `Toniebox-XXXX` | own setup page, hotspot `TeddyBox-XXXX` (big ear while switching on); WiFi of the original is used as is |
| Settings | Tonies app / cloud | setup page, `teddybox.ini` on the SD card, Home Assistant; max volume (speaker/headphones), LED and slap also from teddyCloud |
| Live events (RTNL: tonie, playback, volume, ears, slap, tilt, charger) | to Boxine (`rtnl.bxcl.de`), or to teddyCloud with a patched CA | to teddyCloud only (`rtnl`, on) |
| Home Assistant | — (only what teddyCloud derives from the box) | MQTT discovery: playback, tonie, chapters, progress, volume, battery, settings, diagnostics, buttons |
| Speaker sound | flat | three profiles in the DAC's own filters: flat, low cut, tuned (measured, default) |
| Volume, ears, slap, tilt, headphones, LEDs | yes | yes (volume steps and slap configuration taken from the original) |
| System sounds | yes | the same sounds, read from the box's assets |
| Firmware updates | from Boxine, automatically | uploaded on the setup page, A/B with rollback |
| Back to the other firmware | — | upside down, both ears 5 s, let go |
| Certificate / flash backup | — | on the setup page (needs a press on the box) |
| MMA8451 accelerometer (some boxes) | yes | no, LIS3DH only |
| Tonies app pairing | yes | no |

## What works

- Playback of tonie content (TAF files) from the SD card, seeking, chapters
- NFC (TRF7962A), ears for volume, headphone detection, LEDs, accelerometer
  (slap and tilt for chapters)
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
- Content download from teddyCloud (`/v2/content`, resume via `Range`),
  tag password read from the tag and sent as `Authorization: BD <hex>`;
  playback starts after a few blocks while the rest downloads
- **Freshness check**, **nightly update**, **Home Assistant**, **firmware
  updates**: see below

## Setup page

On the first boot (no `teddybox.ini` on the SD card yet), or when the big
ear is held for 3 seconds while switching on, the box says the installation
announcement, blinks blue/green and opens the WiFi hotspot `TeddyBox-XXXX`
(no password, only while setup runs). Phones show the setup page
automatically, otherwise open `http://192.168.4.1/`. It configures WiFi
(with network scan), teddyCloud (address, port, CA upload as DER or PEM),
Home Assistant (MQTT), volume, language, power off time, speaker sound, LED,
slap and tilt, the nightly update hour and (section "Decoder und
Prozessortakt") decoder, CPU clock, download speed while playing and the
read-ahead. Saving writes `teddybox.ini`, stores the WiFi in the original
firmware's format and restarts. Unused for 15 minutes, the box switches off.
Cloud, MQTT and NFC do not run in setup mode.

### Backup (setup page)

The setup page offers a backup of what is unique to the box:

- `toniebox-<mac>-certs.zip`: `ca.der`, `client.der`, `private.der` (the
  names teddyCloud expects)
- `toniebox-<mac>-flash.bin`: the whole flash (bootloader, original
  firmware, NVS with WiFi data, certificates); restore with
  `esptool.py --chip esp32s3 write_flash 0x0 toniebox-<mac>-flash.bin`
- optionally both onto the SD card (`teddybox_backup/<mac>/`)

The setup hotspot is open, so the backup is only released after the small
ear is pressed on the box (60 s to press it, then 10 minutes valid). The
files contain the private key of the box.

## Configuration: `teddybox.ini` on the SD card

Written by the setup page; all keys are optional.

| Key | Default | |
|---|---|---|
| `cloud_host`, `cloud_port` | empty, 443 | teddyCloud; the cloud stays off while `cloud_host` is empty |
| `ca_path` | `/spiflash/cert/ca.der` | CA of teddyCloud (`certs/server/ca.der`); the CA in flash only works if it was replaced by the teddyCloud patch, otherwise put it on the SD card |
| `cloud_enabled` | on with a host | `0` forces the cloud off |
| `boxine_fallback`, `boxine_host` | 0, `prod.de.tbs.toys` | see below |
| `user_agent_original` | 0 | see below |
| `mqtt_host`, `mqtt_port`, `mqtt_user`, `mqtt_pass` | empty, 1883 | Home Assistant; off while `mqtt_host` is empty |
| `mqtt_topic`, `ha_discovery_prefix` | `teddybox/<mac>`, `homeassistant` | |
| `lang_id` | from NVS | system sounds: 0=EN-GB 1=DE 2=EN-US 3=FR |
| `volume_max`, `volume_default` | 100, 50 | percent |
| `volume_max_hdp` | 100 | percent, with headphones plugged in |
| `tc_box_settings` | 1 | take over box settings changed in teddyCloud, see below |
| `idle_timeout` | 60 | seconds until power off |
| `speaker_eq` | `full` | `off`, `lowcut`, `full`, see Audio |
| `led_brightness` | 3 | 0 (off) to 3 |
| `slap`, `slap_reverse` | 3, 0 | slap on the side for next/previous chapter: 0 off, 1 firm to 5 light |
| `tilt` | 3 | tilt for chapter/seek: 0 off, 1 far to 5 a little |
| `update_hour` | 3 | nightly update, -1 off |
| `timezone` | `CET-1CEST,M3.5.0,M10.5.0/3` | POSIX TZ |
| `claim` | 0 | report placed tonies to teddyCloud |
| `decoder` | `celt` | `celt` or `adf` |
| `cpu_mhz` | 160 | 160 or 240 (`decoder = adf` always uses 240) |
| `download_playing` | 0 | KiB/s while a tonie plays, 0 = unlimited |
| `readahead` | 1 | 0 only for diagnosis |
| `rtnl` | 1 | box events to teddyCloud, see below |
| `mem_log` | 1 | memory lines in the serial log |

The cloud is off by default on purpose: the box certificate would otherwise
be used against the Boxine cloud by a non-original firmware.

### Optional: Boxine fallback and original user agent

Both are off by default and can be switched on in the setup page
("Erweitert") or in `teddybox.ini`.

- With `boxine_fallback`, the box tries teddyCloud first and uses the Boxine
  cloud only if teddyCloud does not answer; every 5 minutes it checks whether
  teddyCloud is back. Boxine connections use the original CA from the assets
  partition, send SNI and verify the server name, like the original firmware.
- teddyCloud passes the box's user agent on to Boxine. Without
  `user_agent_original`, Boxine therefore sees `toniebox-esp32-teddybox/...`
  whenever teddyCloud fetches content for TeddyBox.
- The user agent is not the only difference: TLS handshake (mbedTLS 3.6 from
  IDF 5.5 instead of the original's IDF 5.0, TLS 1.2 only) and behaviour (no
  log upload, no firmware checks) differ as well. If Boxine blocks the box
  certificate, the Boxine cloud stops working for the original firmware and
  teddyCloud too.

## teddyCloud

TeddyBox uses the same protocol as the original firmware: `GET /v1/time`,
`GET /v2/content/<rUID>` with `Authorization: BD <tag memory>` and `Range`
for resuming. teddyCloud identifies it as an ESP32 box by the user agent
`toniebox-esp32-teddybox/<version>`. Requirements:

- `cloud_host` set to teddyCloud
- teddyCloud's CA (`certs/server/ca.der`) reachable via `ca_path`
- the box certificates known to teddyCloud (`certs/client/<mac>/`), as for
  the original firmware

A tonie that teddyCloud does not have yet can only be loaded if teddyCloud
may fetch it from the Boxine cloud (`cloud.enabled` and content v2 in
teddyCloud). Otherwise teddyCloud closes the connection without an answer;
the box logs that and plays the original's network error sound. Placing the
tonie again loads it once teddyCloud has it.

### Freshness check

After boot (as soon as teddyCloud is reachable) and every 6 hours the box
sends UID and `audio_id` of every tonie on the SD card to
`POST /v1/freshness-check`. teddyCloud answers with the tonies that have new
content: a newer version in its library, a newer version in the Boxine
cloud (if cloud access is enabled in teddyCloud) or live content. Those
files are renamed to `<file>.old`; the next time the tonie is placed (or in
the nightly update) the new version is downloaded. If that fails, the `.old`
version is played instead. `.old` files are removed once the new version is
complete. A tonie that is playing during the check is handled next time.
Up to 128 tonies per check.

The answer also carries the box settings from teddyCloud (*Settings →
Toniebox*, per box in the overlay; with "override cloud" off the values from
the Tonie app). TeddyBox takes over the ones that **changed there** since
the last check, so a setting changed on the box itself (setup page, INI,
Home Assistant) stays until it is changed in teddyCloud again. The first
check after the update only stores the values. `tc_box_settings = 0` ignores
them.

| teddyCloud | TeddyBox |
|---|---|
| max volume speaker / headphones 0–3 | `volume_max` / `volume_max_hdp` 25 / 50 / 75 / 100 % |
| LED on / off / dimmed | `led_brightness` 3 / 0 / 1 |
| slap off / on | `slap` 0 / the previous level (3 if it was off) |
| slap direction "back on the left" | `slap_reverse` |

Log: `TCSET: Applying <n> box setting(s) changed in teddyCloud`. QEMU test
`tests/qemu/mkqtcset.sh run`.

### Claim (tonie placed)

Like the original box, TeddyBox can report a placed tonie to teddyCloud with
`GET /v1/claim/<rUID>` and the tonie's token (5 s after placing). teddyCloud
shows it as the box's last tonie and marks it as claimed. It is never sent
to the Boxine fallback; but teddyCloud itself forwards claims to the Boxine
cloud if its cloud access and "Forward claim" are enabled, so every placed
tonie would reach Boxine. Therefore it is off by default; `claim = 1` turns
it on. Home Assistant shows the current tonie anyway.

### Live events (RTNL)

Like the original firmware, TeddyBox sends box events over RTNL (4 byte
length + protobuf `TonieRtnlRPC`, on teddyCloud's API port with the box
certificate). teddyCloud shows them live in its web interface and passes
them to its own MQTT / Home Assistant integration; it does not forward them
to Boxine. Sent: tonie placed and removed, audio id, playback starting /
started / stopped, volume (level and dB as the original), ears, slap
(knock), tilt chapter, charger on/off, and the firmware version
(`TeddyBox <version>`) after connecting. Only while teddyCloud is the
backend in use, never to the Boxine fallback; events while it is not
reachable are dropped. The connection is closed after a minute without
events (it costs RAM). `rtnl = 0` switches it off.

teddyCloud reads the tonie UID with a sign-extension bug: a UID whose lower
half is `0x80000000` or more shows up with `FFFFFFFF` in front in its event
view (the same happens with the original box).

QEMU test `tests/qemu/mkqrtnl.sh run` (checks teddyCloud's `/api/sse`).

## Downloads

- As in the original firmware, a download is written to `<file>.tmp` and only
  renamed to the tonie file when it is complete and valid, so a partial
  download never looks like a tonie. An interrupted download continues from
  the `.tmp` the next time. Tested in QEMU against teddyCloud
  (`tests/qemu/qemu_dl_test.c`).
- Full speed also while a tonie plays (about 300 KiB/s; the decoder then
  needs about 40 % of one core at 160 MHz). `download_playing = <KiB/s>`
  limits it. With `decoder = adf` at most 96 KiB/s.
- **Hash check** (`main/taf_verify.c`): after each download and once per start
  for every tonie played, a background task compares the SHA-1 of the audio
  data with the one in the TAF header (`Verify: Audio hash OK` / `MISMATCH`,
  about 30 s for 30 MB). A damaged file is renamed to `<file>.bad` (when its
  playback ends) and downloaded again next time
  (`tests/qemu/qemu_verify_test.c`).
- A failed download (teddyCloud unreachable or without the content) plays
  the original's network error sound.

## Nightly update

Once a day at `update_hour` (local time, `timezone`; default 3 o'clock plus
0-29 minutes) the box wakes from deep sleep by timer, without sound or LED
and without reading NFC. It waits for the freshness check against teddyCloud
and then downloads every tonie that has a new version (`<file>.old`) or an
unfinished download, one after another.

- On the charger always, on battery only from 50 % (resting voltage).
- A pressed ear stops it and starts the box normally; a download without
  progress for 2 minutes, low battery or 45 minutes in total stop it, the
  rest continues the next night or when the tonie is placed.
- Downloading without the tonie on the box needs its token. It is stored when
  a tonie is placed (`/sdcard/TOKENS/<dir>/<file>`, same names as in
  `CONTENT`, 32 bytes, only written when it changes). Tonies not placed since
  this version are skipped. **The tokens give access to the tonie's content in
  the cloud: whoever has the SD card has them.** Delete the `TOKENS` folder to
  remove them.
- Needs a valid clock (set from teddyCloud on every connection; kept in deep
  sleep, lost when the battery is removed: then the next normal start plans
  it again).
- The result (also "skipped (battery x %)") is kept in NVS, so it survives
  power loss and flashing. After a run the box waits up to 30 s for MQTT, so
  Home Assistant gets it right away (diagnostic sensor "Letzte nächtliche
  Aktualisierung"). Every start logs
  `Maint: Nightly update: last <date>: <result>, next planned <date>`.

## Home Assistant

With `mqtt_host` set, the box registers itself via MQTT discovery as device
"TeddyBox" (`<id>` = `teddybox_<mac>`, 31 entities):

| Entities | |
|---|---|
| Status, Tonie, Kapitel, Kapitel gesamt | playing / idle, UID of the current tonie, chapter |
| Fortschritt, Kapitel-Fortschritt, Gespielt, Gesamtdauer | progress in % and minutes (from the Ogg granule positions), at most every 10 s |
| Download | % of a running download |
| Lautstärke | number, limited by `volume_max` |
| Akku (geschätzt), Akkuspannung, Lädt, Akku schwach | charge estimated from the voltage (NiMH curve, amplifier load added back, unknown while charging) |
| Kopfhörer, WLAN-Signal | |
| LED-Helligkeit, Klapsen, Klapsen umgekehrt, Kippen, Lautsprecher-Klang | settings, applied at once and saved to the INI |
| Stopp, Nächstes/Vorheriges Kapitel, Ausschalten | buttons |
| Diagnostics | free heap (now and minimum), start time, reason of the last start, firmware slot, last nightly update |

Topics: `teddybox/<mac>/state` and `/diag` (JSON),
`teddybox/<mac>/availability` (`online`/`offline`, also as last will) and
`teddybox/<mac>/cmd/<command>`. Commands that come with the retain flag
(stored on the broker) are ignored and deleted there: they would be repeated
at every start and override the settings. The box powers off when idle
(`idle_timeout`), Home Assistant then shows it as unavailable.

## Audio

Output and volume work as in the original firmware (driver `CO3100.c`,
read from the flash dump):

- speaker: both channels mixed to mono (the class-D amplifier only gets the
  left DAC), driver gain 6 dB
- headphones: stereo, the speaker is powered down while they are plugged in
  (and the other way round)
- 16 volume steps per output, speaker -20 dB to +2.5 dB, headphones -20 dB
  to -2 dB; each ear press moves one step. The percent values in Home
  Assistant and in the settings map onto these steps.
- **DAC clocks within the data sheet**: PLL 98.3 MHz, DAC_CLK 49.152 MHz,
  DAC_MOD_CLK 6.144 MHz, DOSR 128. The values taken over earlier from
  RvX_TLV320DAC3100 ran the PLL at 295 MHz and the DAC far above its limits:
  high tones came out as strong spurs below them, nothing above 10 kHz
  reached the speaker. See `docs/original-firmware.md`.
- **Speaker EQ** in the DAC's own biquads (no CPU load), headphones stay
  flat. `speaker_eq` in the INI, on the setup page and in Home Assistant:
  - `full` (default, "Optimiert"): 4th order Butterworth high-pass at 150 Hz,
    -5 dB at 470 Hz (Q 1.4), -3 dB at 1.65 kHz (Q 1.8), -3 dB at 6.3 kHz (Q 3)
  - `lowcut` ("Nur Tiefenfilter"): only the high-pass
  - `off` ("Wie Original"): flat, as the original firmware

  The cuts come from phone measurements with third-octave test tones and pink
  noise (2026-11-21, see `speaker_eq.h`): flat, the box has +4 to +5 dB at
  400-500 Hz (closed foam cube), +3.5 dB at 1.6 kHz and +2.6 dB at 6.3 kHz.
  With `full` it is within about ±4 dB from 250 Hz to 16 kHz; measured with
  pink noise the EQ matches the computed curve within about 0.3 dB. The EQ
  only cuts, so it cannot clip. After writing, the coefficients are read back
  from the DAC (`DAC3100: EQ: full (coefficients checked)`).
- tests: `tests/host/test_speaker_eq` (response from the rounded 16 bit
  coefficients), `tests/host/dac_sim` (register model incl. clocks)

## Playback

### Opus decoder and CPU clock

`decoder = celt` (default): libopus 1.5.2, fixed point, only the CELT part,
like the original firmware's decoder (`components/opus_celt`, symbols
prefixed with `tb_` because ESP-ADF links its own libopus), with an own Ogg
demuxer (`main/ogg_demux.c`). Tonie files are CELT only (also teddyCloud's
encodes); a SILK/hybrid packet is skipped with a log hint to use
`decoder = adf` (ESP-ADF's esp_audio_codec, full Opus, kept as fallback).

- The decoder task runs on core 1. ESP-ADF's default, core 0, shares the
  core with WiFi, lwIP, esp_timer and the I2S output, and the decoder at the
  highest priority delayed the output there: small clicks, mostly at 160 MHz.
- `cpu_mhz = 160` (default) or `240`: fixed clock via power management (no
  light sleep). CELT at 160 MHz: about 32 % of one core, about 40 % while
  downloading. `decoder = adf` always runs at 240 MHz.
- RAM while decoding music: about 60 KB instead of 126 KB with ESP-ADF; on
  the box about 125 KB free while playing (85 KB with read-ahead and hash
  check running), about 60 KB during a download.
- Output is bit exact with the libopus reference (32 bit multiply path, as
  on every 32 bit CPU): `tests/qemu/qemu_codec_test.c` and
  `celt_host_reference.c`.

### SD card hangs and read-ahead

Some SD cards stop answering for about 1 s now and then (`sdmmc ... 0x107`),
often right after a long download.

- **Read-ahead** (`main/readahead.c`, like the original's separate SD task):
  an own task reads the next 8 blocks (32 KiB, about 2.7 s of audio) of a
  tonie, so a hanging card does not interrupt the sound. At the end of a
  tonie the log shows `Read-ahead: <n> blocks from the buffer, <m> read
  directly`. QEMU test `tests/qemu/qemu_ra_test.c` (simulated hangs, seek, a
  file that grows while it is read, ending while the task hangs).
  `readahead = 0` switches it off for diagnosis.
- Playback reopens and retries failed reads. While the tonie is still
  downloading it waits up to 3 s for data instead of treating it as the end.
- Downloads are written in 4 KiB blocks aligned to the file offset. A failed
  write reopens the file and retries up to 3 times. `received` only counts
  data that reached the card, so a resume continues at the right place.
- A download that loses its connection midway is resumed automatically
  with `Range` while the tonie stays on the box and keeps playing what is
  there: up to 6 attempts after 2, 4, 8, 15, 30 and 30 s, counted again
  after every attempt that brought data. Boxine's CDN sometimes drops
  connections that teddyCloud passes through slowly (teddycloud#585). If a
  server ignores the `Range`, the part already on the card is skipped
  instead of overwritten. Log: `[CDL] Connection lost at <x> of <y> bytes,
  resuming in <n> s (attempt <k> of 6)`. QEMU test `tests/qemu/mkqresume.sh`.
- **teddyCloud streams** (web radio and other live sources) are recognised by
  their length, which teddyCloud makes no multiple of 4096
  (`encode.stream_max_size`). A stream always plays from its start, is never
  resumed with `Range` (teddyCloud cannot continue a stream, teddycloud#606,
  #608), not resumed automatically after a lost connection, and its `.tmp`
  is deleted when it ends, so streams do not fill the card and the nightly
  update does not try to complete them (it also deletes left-over stream
  files). Log: `[CDL] teddyCloud stream (up to <n> bytes): not resumed, not
  kept`. QEMU test `tests/qemu/mkqstream.sh`.

### Diagnostics in the log

- `Decoder <adf|celt> at <MHz> MHz: <x> % CPU (of one core)` after 10 s of a
  tonie, then every 30 s
- `[PB]: Audio: ...`: the output did not get its 192000 B/s over 5 s
- `Audio gap near m:ss: decoder late n x (longest x ms), output late n x
  (longest x ms)`: a 5 ms monitor; decoder late = PCM buffer empty, output
  late = I2S standing still with data waiting (the first one in a window adds
  the state of the `i2s` and `dec` tasks and what ran on each core)
- `Ogg: ... dropped`: packets the decoder had to drop
- `SD write ... ms/s`, `SD read at ... failed, reopening`, `Waited ... for the
  download's file lock`, `Playback caught up with the download`

## Power

- The NFC reader's RF field (70-130 mA while on) is only switched on for the
  moments a tag is queried: every 250 ms without a tonie, every 500 ms with
  one. A tonie returns to privacy mode without field, so each check unlocks
  it again with the password that worked before. Simulated with a virtual
  tonie (`tests/host/nfc_sim`): field on 3-4 % of the time instead of
  65-100 %, tonie removal noticed after about 0.7 s.
- Peripheral power and SD card power are held off during deep sleep
  (digital pads would lose their level otherwise).
- On the charger the box sleeps with the ears as wake source and checks every
  5 minutes whether it was taken off.
- Idle power off after 60 s by default (`idle_timeout`).

## Network traffic

Without `boxine_fallback` the box only talks to what is configured:
teddyCloud (`cloud_host`) and the MQTT broker (`mqtt_host`), plus WiFi,
DHCP, ARP and DNS lookups for those host names in the local network. No
time servers (the time comes from teddyCloud), no mDNS, no telemetry.
It shows up in the router as `teddybox-xxxx`. Inbound, nothing listens
in normal mode. Checked with a packet capture in QEMU (Ethernet instead of
WiFi). Note that teddyCloud itself contacts the Boxine cloud if its
`cloud.enabled` is on.

## Flash layout

The partition table is not changed. TeddyBox goes into the free slot first;
the first firmware update (with consent on the setup page) takes the slot of
the PD app, after that TeddyBox alternates between ota_0 and ota_1:

| Slot   | Address  | Original firmware            | TeddyBox installed | after the first update |
|--------|----------|------------------------------|--------------------|------------------------|
| ota_0  | 0x180000 | `toniebox-esp32-pd` (factory/fallback) | unchanged | **TeddyBox** (A/B) |
| ota_1  | 0x3a0000 | free (`TB_OTA/FREE`, may hold an old copy of the EU app) | **TeddyBox** | **TeddyBox** (A/B) |
| ota_2  | 0x5c0000 | `toniebox-esp32-eu`          | unchanged          | unchanged              |

Check your own layout first (`tools/make_otadata.py` prints the current
slots, NVS `TB_OTA` lists `EU`, `PD`, `FREE`; the boot log shows them as
`BootSel:` lines). The EU app is never touched, so the box can always go
back to the original firmware (both ears). The PD app only takes over when
the EU app is missing or invalid and then demands an update ("Codewort
Murmeltier"); without it there is no such fallback, but TeddyBox stays
bootable. Keep the flash dump: it contains the PD app if you want it back.

## Install (into the free slot)

Make a full backup first and keep it safe (it contains the box certificate):

```sh
esptool.py -b 921600 read_flash 0x0 0x800000 backup.bin
python3 tools/make_otadata.py backup.bin 1 otadata_teddybox.bin
esptool.py -b 921600 write_flash 0x3a0000 teddybox/build/teddybox.bin 0x16f000 otadata_teddybox.bin
```

Only the free slot and the 8 KiB boot selection are written. Bootloader,
partition table, NVS, certificates and both original apps stay as they are.

Back to the original firmware: turn the box **upside down** (ears down),
hold both ears for 5 seconds until the LED blinks blue and it beeps, then
**let go**; the original firmware starts. Upside down, so that a child
pressing both ears on the upright box (in the original firmware that is
the WiFi setup) does not switch by accident. Letting go first matters: the
original firmware treats both ears held upside down as restart, and on the
charger as factory reset. If the ears are not let go within 10 s, the box
stays in TeddyBox. Without a working accelerometer the box does not need to
be upside down. Or with esptool:

```sh
dd if=backup.bin of=otadata_original.bin bs=4096 skip=367 count=2
esptool.py write_flash 0x16f000 otadata_original.bin
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
  probation", checks SD card, DAC, accelerometer and NFC chip and confirms
  itself after running 10 seconds (or once the setup page is up). If it
  crashes, restarts or a check fails before that, the bootloader starts the
  previous version again.
- Switching to the original firmware (both ears) marks it valid right away,
  so it does not fall back to TeddyBox after its first restart.

Tested in QEMU with the original bootloader from a flash dump: refused
images, PD consent, update, rollback of an unconfirmed version, alternating
updates, switch to the original firmware.

## Build

Built and tested on Linux with ESP-IDF v5.5.6 (supported by Espressif until
January 2028) and ESP-ADF v2.8 (only its components and esp-adf-libs; ADF's
own esp-idf copy is not used).

```sh
./setup_libs.sh            # clones esp-idf v5.5.6 and esp-adf v2.8
. ./env_vars.sh
cd teddybox && idf.py build
```

Without access to components.espressif.com (the IDF component manager),
`./setup_libs.sh offline` fetches the three registry components ESP-ADF needs
(jsmn, nghttp, zlib) from GitHub; `env_vars.sh` then sets
`IDF_EXTRA_COMPONENTS` and turns the component manager off.

Notes on the IDF 5 port:

- `components/audio_stream` is a slim copy of ADF's: only the I2S and raw
  streams (the original pulls in esp-sr on the ESP32-S3).
- The project only builds what `main` needs (`set(COMPONENTS main)`).
- ESP-ADF headers still use the old FreeRTOS type names, hence
  `CONFIG_FREERTOS_ENABLE_BACKWARD_COMPATIBILITY`.
- TRF7962A direct commands need an extra SPI clock; the ESP32-S3 cannot send
  a single extra bit, so it sends two (`trf7962a_command`). At start the
  driver checks that a direct command (software init) has an effect.
- Release build (`-O2`); TLS handshakes in QEMU about 20 % faster than -Og.
- TCP window and WiFi RX buffers as with IDF 4.4. A 23 KB window with 32
  dynamic RX buffers let the free heap fall to 8 KB on the box during a
  download.
- WiFi, lwIP, heap and FreeRTOS functions stay in flash, not IRAM: about
  70 KiB more heap at start (330 instead of 260 KiB).
- Task watchdog panics (reboot) if an idle task is starved for 5 s.

## Tests

```sh
make -C tests/host     # host unit tests, no ESP-IDF needed
```

QEMU tests (decoder, downloads, read-ahead, hash check): see
`tests/qemu/README.md`.

## Background

`docs/original-firmware.md` collects what the flash dump of the original
firmware shows (structure, cloud requests, NVS, SD layout) and what TeddyBox
took over from it.

## License

The work of this fork is MIT or CC0-1.0, at your choice; upstream TeddyBox
and the bundled third-party code keep their own licenses. See `LICENSE` and
`LICENSES.md`.
