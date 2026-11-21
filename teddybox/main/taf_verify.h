#pragma once
/* Checks a tonie file against the SHA-1 in its TAF header (over the audio
   data after the 4 KB header), in a low priority background task. A file
   that does not match is renamed to <file>.bad (at once, or when its playback
   ends), so the box downloads it again the next time the tonie is placed. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* starts the check unless one is running or this audio ID was already found
   good since the start; sha1 must have 20 bytes */
void taf_verify_start(const char *filename, const uint8_t *sha1, size_t sha1_len, uint64_t num_bytes,
                      uint32_t audio_id);

/* playback calls this when it closes "filename": a pending rename of a bad
   file happens now */
void taf_verify_file_closed(const char *filename);

/* is "filename" open for playback right now (set by the playback) */
void taf_verify_set_playing(const char *filename);
