#pragma once
/*
 * Software watchdog for TeddyBox's own tasks: each task feeds it in its
 * loop; if one does not for its timeout (main 30 s, NFC 30 s, playback
 * 90 s, cloud 120 s), the box aborts with the task name, which restarts it
 * (with a crash dump where enabled) instead of hanging with a dead player.
 * The ESP-IDF task watchdog only watches the idle tasks (5 s); a per-task
 * timeout is not possible there.
 */
#include <stdint.h>

#define TASKWATCH_MAX 8

/* returns an id for taskwatch_feed(), -1 when full; call from the task */
int taskwatch_add(const char *name, uint32_t timeout_ms);
void taskwatch_feed(int id);
/* reason of the last abort by the taskwatch ("" if none), kept over the restart */
const char *taskwatch_last_reason(void);
