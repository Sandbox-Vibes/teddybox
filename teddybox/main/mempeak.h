#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Keeps the two big short-lived memory peaks apart: a TLS handshake needs
 * about 40 KB, starting the Opus decoder about 67 KB (measured in QEMU). Both
 * at once plus WiFi did not fit into the internal RAM of the box.
 *
 * mempeak_enter() waits until the other peak is over, at most wait_ms; it
 * returns whether the lock was taken, pass that to mempeak_leave(). If the
 * wait times out the caller goes on anyway (slow is better than stuck).
 */
bool mempeak_enter(const char *who, uint32_t wait_ms);
void mempeak_leave(bool taken);
