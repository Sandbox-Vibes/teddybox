#pragma once
/*
 * Minimal mDNS responder (plain C, host tested): answers A (or ANY) queries
 * for "<host>.local" with one IPv4 address. Nothing else: no services, no
 * probing, no IPv6. Used in setup mode in the home network.
 */
#include <stdbool.h>
#include <stdint.h>

#define MDNS_PORT 5353
#define MDNS_GROUP "224.0.0.251"

/* answer to a received packet, or 0 if it does not ask for <host>.local.
   legacy: the query came from a port other than 5353 (a plain DNS resolver):
   the answer repeats the question and the id and goes back by unicast.
   ip in network byte order. */
int mdns_min_answer(const uint8_t *in, int in_len, const char *host, uint32_t ip, bool legacy, uint8_t *out, int out_size);

/* unsolicited announcement (multicast) */
int mdns_min_announce(const char *host, uint32_t ip, uint8_t *out, int out_size);
