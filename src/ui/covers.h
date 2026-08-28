#ifndef ABS_COVERS_H
#define ABS_COVERS_H

#include <inkview.h>

#include "core/config.h"

/* Create the cache directory. Safe to call more than once. */
void abs_covers_init(void);

/*
 * Get a cover scaled to fit within (w, h), preserving aspect ratio.
 *
 * Returns a bitmap owned by the cache -- do not free it. NULL if there is no
 * cover, or if `allow_fetch` is 0 and it is not already cached.
 *
 * Two caches sit behind this. Downloaded bytes are kept on disk so revisiting
 * a page costs no network; decoded thumbnails are kept in memory so paging
 * back and forth costs no JPEG decode, which is the expensive half on a
 * 1 GHz device.
 */
ibitmap *abs_cover_get(const abs_config *cfg, const char *item_id,
                       int w, int h, int allow_fetch);

/* Drop the in-memory thumbnails (the disk cache is untouched). */
void abs_covers_free_memory(void);

#endif /* ABS_COVERS_H */
