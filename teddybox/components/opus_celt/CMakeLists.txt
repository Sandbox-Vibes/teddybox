# libopus 1.5.2 (xiph.org, BSD license, see opus/COPYING), only the CELT
# decoder like the original toniebox firmware: tonie files are CELT only.
# Fixed point, SILK replaced by stubs (silk_stubs.c); only the libopus files
# the build uses are kept (no ARM/x86/MIPS code, no float tables). All symbols get a
# tb_ prefix (opus_rename.h): ESP-ADF links its own libopus.
set(OPUS opus)
idf_component_register(
    SRCS
        ${OPUS}/celt/bands.c ${OPUS}/celt/celt.c ${OPUS}/celt/celt_decoder.c ${OPUS}/celt/celt_lpc.c
        ${OPUS}/celt/cwrs.c ${OPUS}/celt/entcode.c ${OPUS}/celt/entdec.c ${OPUS}/celt/entenc.c
        ${OPUS}/celt/kiss_fft.c ${OPUS}/celt/laplace.c ${OPUS}/celt/mathops.c ${OPUS}/celt/mdct.c
        ${OPUS}/celt/modes.c ${OPUS}/celt/pitch.c ${OPUS}/celt/quant_bands.c ${OPUS}/celt/rate.c
        ${OPUS}/celt/vq.c
        ${OPUS}/src/opus_decoder.c ${OPUS}/src/opus.c ${OPUS}/src/extensions.c
        silk_stubs.c
    INCLUDE_DIRS ${OPUS}/include .
    PRIV_INCLUDE_DIRS ${OPUS} ${OPUS}/celt ${OPUS}/silk ${OPUS}/src)
target_compile_definitions(${COMPONENT_LIB} PRIVATE FIXED_POINT=1 OPUS_BUILD=1 VAR_ARRAYS=1 DISABLE_FLOAT_API=1 HAVE_LRINT=1 HAVE_LRINTF=1)
target_compile_options(${COMPONENT_LIB} PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/opus_rename.h -O2 -Wno-unused-function -Wno-maybe-uninitialized -Wno-stringop-overflow)
