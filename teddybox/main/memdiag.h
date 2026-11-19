#pragma once

/*
 * Memory diagnostics for the serial log:
 *   MEM <where>: free 61234 (min 40123) largest 31744
 *   STACK: [TB] Playback 812, dec 9000, ...   (bytes never used)
 * and a log line for every failed allocation instead of a silent NULL.
 */

void memdiag_init(void);
/* one line with the internal heap state */
void memdiag_log(const char *where);
/* heap plus the unused stack of every task */
void memdiag_log_tasks(void);
