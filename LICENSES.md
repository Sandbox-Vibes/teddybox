# Licenses

This repository mixes code from several sources. Each part keeps its own
license.

| Part | License |
|---|---|
| Changes of this fork: every commit on branch `idf5` after `bff6d3d` (upstream `master`), incl. `main/readahead.c`, `main/taf_verify.c`, `main/maint.c`, `main/ogg_demux.c`, `main/tb_opus_dec.c`, the speaker EQ, docs, tests and tools | **MIT or CC0-1.0**, at your choice (`LICENSE`) |
| Upstream TeddyBox by g3gg0 and contributors ([toniebox-reverse-engineering/teddybox](https://github.com/toniebox-reverse-engineering/teddybox)), the code as of `bff6d3d` | no license file upstream; ask the upstream authors before reusing their parts outside this fork |
| `teddybox/components/opus_celt/opus/` (libopus 1.5.2, Xiph.Org) | BSD 3-clause, `opus/COPYING` |
| `teddybox/main/proto/protobuf-c.*` | BSD 2-clause (protobuf-c) |
| `teddybox/main/www/opusenc.js` (opus-recorder 8.0.5 `encoderWorker.min.js`: libopus 1.3.1 and the Speex resampler compiled to WebAssembly) | MIT (opus-recorder, `www/opusenc.LICENSE.md`); libopus BSD 3-clause, Speex resampler BSD 3-clause |
| `teddybox/components/audio_stream/`, board files in `components/toniebox/` based on ESP-ADF | ESPRESSIF MIT License (file headers; use on Espressif products) |
| `teddybox/components/lis3dh/` | parts after electricimp's LIS3DH library (MIT), see the file header |
| ESP-IDF, ESP-ADF (fetched by `setup_libs.sh`, not in this repository) | Apache 2.0 / ESPRESSIF MIT |

Where a file of the fork changes an upstream or third-party file, the
original license of that file stays in force for the original parts; the
changes are MIT or CC0-1.0.

The firmware binaries in the releases contain all of the above plus ESP-IDF
and ESP-ADF.
