#pragma once
/*
 * Crash dumps: on a panic (exception, watchdog) ESP-IDF writes a core dump
 * to the "coredump" partition (0x7e0000, 128 KB). At the next start the
 * summary (task, program counter, backtrace) is logged and, once per
 * crash, appended to /sdcard/tb_crash.txt. Decode with the .elf of
 * the same version, see README "Crash dumps".
 */
void crashlog_check(void);
