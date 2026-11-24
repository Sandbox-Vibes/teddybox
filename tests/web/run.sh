#!/bin/bash
# Browser TAF encoder test: encodes test signals in Chrome (taf_run.py),
# checks the layout (taf_check.py), decodes with the box's own CELT decoder
# and Ogg demuxer (taf_decode.c) and checks the signal (taf_signal.py).
# Needs python3 with playwright and numpy, Chrome, cc.
set -e
cd "$(dirname "$0")"
W=${TMPDIR:-/tmp}/teddybox-web; mkdir -p "$W"
O=../../teddybox/components/opus_celt/opus
cc -O2 -w -DFIXED_POINT=1 -DOPUS_BUILD=1 -DVAR_ARRAYS=1 -DDISABLE_FLOAT_API=1 -DHAVE_LRINT=1 -DHAVE_LRINTF=1 \
    -I$O/include -I$O -I$O/celt -I$O/silk -I$O/silk/fixed -I$O/src -I../../teddybox/components/opus_celt -I../../teddybox/main \
    taf_decode.c $O/celt/{bands,celt,celt_decoder,celt_lpc,cwrs,entcode,entdec,entenc,kiss_fft,laplace,mathops,mdct,modes,pitch,quant_bands,rate,vq}.c \
    $O/src/{opus_decoder,opus,extensions}.c ../../teddybox/components/opus_celt/silk_stubs.c ../../teddybox/main/ogg_demux.c -lm -o "$W/taf_decode"
python3 taf_run.py "$W/a.taf"
python3 taf_check.py "$W/a.taf" 3
"$W/taf_decode" "$W/a.taf" "$W/a.wav"
python3 taf_signal.py "$W/a.wav"
# short chapters (padding corner cases) and a long one
python3 taf_run.py "$W/b.taf" 0.01 0.06 0.061 0.2 0.12 1.9 > /dev/null
python3 taf_check.py "$W/b.taf" 6
"$W/taf_decode" "$W/b.taf" "$W/b.wav"
python3 taf_run.py "$W/c.taf" 300 > /dev/null
python3 taf_check.py "$W/c.taf" 1
"$W/taf_decode" "$W/c.taf" "$W/c.wav"
python3 taf_check.py sample_browser.taf 3
echo "ALL OK"
