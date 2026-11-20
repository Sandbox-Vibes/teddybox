#pragma once
#include "board.h"

#define ACCEL_TASK_PRIO 5

#define ACCEL_SEEK_BLOCKS 5 /* blocks to seek when tilted */
#define ACCEL_SEEK_REPEAT 10 /* number of ACCEL_LOOP_MS until to seek again */

#define ACCEL_DATA_RATE 20 /* 20 samples/s -> 50 ms */
#define ACCEL_LOOP_MS 50 /* loop cycle time in ms */

#define ACCEL_STABLE_DELAY 5 /* number of ACCEL_LOOP_MS until angle is considered stable */

/* angle definitions for stable, tilted and flipped */
#define ACCEL_ANGLE_STABLE 0
/* sensitivity levels for slap and tilt: 0 off, 1..5 */
#define ACCEL_LEVELS 6
/* after a slap, ignore further ones (the box swings back) */
#define ACCEL_SLAP_PAUSE_MS 700
#define ACCEL_ANGLE_FLIP 180

void accel_init(audio_board_handle_t board);
