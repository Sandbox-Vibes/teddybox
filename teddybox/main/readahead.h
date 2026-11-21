#pragma once
/* Read-ahead of whole file blocks in an own task, like the original
   firmware's separate SD task: the SD card sometimes stops answering for
   1-2 s, the decoder then still gets data from the blocks read before.

   Only complete blocks are kept (while a download runs, the block at its end
   is read by the caller itself). The reader announces each block it needs
   with readahead_get(); the task then fills the following blocks. */
#include <stdbool.h>
#include <stdint.h>

typedef struct readahead readahead_t;

/* reads block "block" into buf, may block for seconds; returns the number of
   bytes read (less than a block: not there (yet), retried later) */
typedef int (*readahead_read_fn)(void *ctx, uint32_t block, uint8_t *buf);

/* "blocks" buffers of "block_size" bytes, blocks >= "limit" are never read.
   ctx_free(ctx) is called by the task when it ends. NULL if out of memory
   (ctx is then freed as well). */
readahead_t *readahead_create(int blocks, int block_size, uint32_t limit, readahead_read_fn fn, void *ctx,
                              void (*ctx_free)(void *ctx), int prio, int core);

/* the reader needs "block" now: copies it to buf if it is buffered (true),
   and lets the task fill the blocks after it */
bool readahead_get(readahead_t *ra, uint32_t block, uint8_t *buf);

/* hits/misses of readahead_get */
void readahead_stats(readahead_t *ra, uint32_t *hits, uint32_t *misses);

/* does not wait: a task stuck in an SD read ends (and frees everything)
   when the read returns */
void readahead_destroy(readahead_t *ra);
