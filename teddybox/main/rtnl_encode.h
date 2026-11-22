#pragma once
/*
 * RTNL frames as the original box sends them to rtnl.bxcl.de and teddyCloud
 * reads them on its API port (teddyCloud: proto/toniebox.pb.rtnl.proto,
 * src/handler_rtnl.c): 4 byte big endian length, then a TonieRtnlRPC with
 * either log2 (function group / function / payload) or log3 (event type).
 */
#include <stddef.h>
#include <stdint.h>

/* log2 function groups and functions (ESP32 box) */
#define RTNL2_FUGR_FIRMWARE 8
#define RTNL2_FUNC_FIRMWARE_FULL_VERSION 703
#define RTNL2_FUNC_FIRMWARE_VERSION 704
#define RTNL2_FUGR_TAG 15
#define RTNL2_FUNC_TAG_INVALID 15452
#define RTNL2_FUNC_TAG_VALID 16065
#define RTNL2_FUNC_TAG_VALID_REMOVED 16067
#define RTNL2_FUGR_AUDIO_A 20
#define RTNL2_FUNC_AUDIO_ID 16096
#define RTNL2_FUGR_VOLUME 27
#define RTNL2_FUNC_VOLUME_CHANGE 15524

/* log3 event types */
#define RTNL3_EAR_BIG 1
#define RTNL3_EAR_SMALL 2
#define RTNL3_KNOCK_FORWARD 3
#define RTNL3_KNOCK_BACKWARD 4
#define RTNL3_TILT_FORWARD 5
#define RTNL3_TILT_BACKWARD 6
#define RTNL3_CHARGER_ON 7
#define RTNL3_CHARGER_OFF 8
#define RTNL3_PLAYBACK_STARTING 11
#define RTNL3_PLAYBACK_STARTED 12
#define RTNL3_PLAYBACK_STOPPED 13

#define RTNL_PAYLOAD_MAX 48
#define RTNL_FRAME_MAX (4 + 8 + 32 + RTNL_PAYLOAD_MAX)

/* frame into out (RTNL_FRAME_MAX bytes); returns its length */
size_t rtnl_encode_log2(uint8_t *out, uint64_t uptime_ms, uint32_t sequence, uint32_t group, uint32_t function,
                        const uint8_t *payload, size_t len);
size_t rtnl_encode_log3(uint8_t *out, uint32_t datetime, uint32_t type);

/* payloads */
/* as teddyCloud's read_big_endian64() reads it: low half first, each half
   big endian. teddyCloud sign-extends the low half, so a UID whose low half
   is 0x80000000 or more shows up with FFFFFFFF in front (teddyCloud bug). */
void rtnl_payload_uid(uint8_t out[8], uint64_t uid);
void rtnl_payload_u32le(uint8_t out[4], uint32_t v);
/* volume: 4 bytes unknown (0), DAC volume in 0.5 dB, level index; little endian */
void rtnl_payload_volume(uint8_t out[12], int32_t dac_half_db, int32_t level);
