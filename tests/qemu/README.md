# QEMU tests

Firmware variants that replace `app_main()` with a test (`qemu_*_test.c`) and
run in Espressif's QEMU (`qemu-system-xtensa -machine esp32s3`). Each
`mk*.sh` copies `teddybox/` to `$WORK_BASE/teddybox-qemu-<name>`, applies
`qemu_variant.py` (test entry point, Open Ethernet for networking, embedded
media), builds it, patches out the ADC calibration QEMU does not have
(`qemu_patch.py`) and writes a flash image with the test app at 0x3a0000.

| script | test | needs |
|---|---|---|
| `mkqcodec.sh` | CELT and ESP-ADF decoder, seek; CELT bit exact against `celt_host_reference.c` (host libopus build with `OPUS_FAST_INT64=0`) | |
| `mkqstream.sh` | teddyCloud streams: detection, no `Range` for a left-over stream, no resume, `.tmp` deleted | `fake_stream_server.py 4444 <server cert> <server key>` running (the local teddyCloud's server certificate, so the box trusts it) |
| `mkqresume.sh` | automatic resume after lost connections, abort while waiting | as `mkqdl.sh`, plus `cut_proxy.py 4443 127.0.0.1 443 50000` running (cuts every connection after 50 KB) |
| `mkqdl.sh` | downloads: abort, resume, `.tmp` handling, migration | a local teddyCloud with cloud access off, client certificate in the assets FAT under `CERT/` |
| `mkqra.sh` | read-ahead: SD hangs, seek, growing file, ending during a hang | |
| `mkqtcset.sh` | box settings from teddyCloud: baseline, change taken over, box change stays (`run` drives teddyCloud's web API) | as `mkqdl.sh`, teddyCloud web API on 127.0.0.1:80 |
| `mkqcrash.sh` | crash dump: crash on purpose, restart, summary logged and written once to `tb_crash.txt` | |
| `mkqv.sh` | SHA-1 check of TAF files, rename to `.bad` | `TAF=` a small tonie file (< 300 KB) |
| `mkqrtnl.sh` | RTNL events reach teddyCloud (`run` reads teddyCloud's `/api/sse`) | as `mkqdl.sh`, teddyCloud web API on 127.0.0.1:80 |
| `mkqtw.sh` | task watchdog: a task that stops checking in restarts the box, the reason names it | |

Environment:

- `QEMU_BASE_IMAGE`: an 8 MB flash image with bootloader, partition table,
  NVS and the assets FAT (the app slot is overwritten). Build it from your own
  box or from a fresh `idf.py build` plus an assets image; it is only copied.
  Do not commit or share an image made from a real box: NVS and the assets
  hold WiFi credentials and the box certificate with its private key.
- `QEMU_MEDIA`: a directory with `jingle.ogg`, `music.ogg`, `speech.ogg`
  (Ogg Opus, e.g. encoded by teddyCloud) and `tc_ca.der` (CA of the test
  teddyCloud); they are embedded into every variant.
- `IDF_EXPORT` (default `$IDF_PATH/export.sh`), `WORK_BASE` (default `/tmp`).

Example:

```sh
export QEMU_BASE_IMAGE=~/qemu/base.bin QEMU_MEDIA=~/qemu/media
tests/qemu/mkqra.sh
qemu-system-xtensa -nographic -machine esp32s3 -drive file=/tmp/teddybox-qemu-ra.bin,if=mtd,format=raw
```

Every test ends with a line `RESULT OK` or `RESULT FAIL (n failed)`.
