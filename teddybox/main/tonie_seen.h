#pragma once
/* The last tags placed on the box (UID and time), kept in NVS, so the setup
   page can offer them when a tonie file is uploaded for a new tonie. */
#include <stdint.h>

#define TONIE_SEEN_MAX 8

typedef struct
{
    uint64_t uid;
    uint32_t time; /* unix time, 0 if the clock was not set */
} tonie_seen_t;

void tonie_seen_add(uint64_t uid);
/* newest first; returns the number of entries */
int tonie_seen_get(tonie_seen_t out[TONIE_SEEN_MAX]);
