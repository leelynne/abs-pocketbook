#ifndef ABS_MANIFEST_H
#define ABS_MANIFEST_H

#include <stddef.h>

#include "core/abs_api.h"

#define ABS_MAX_DOWNLOADS 128
#define ABS_MAX_DIR       320

/*
 * Record of one downloaded book.
 *
 * `dir` is the load-bearing field: the firmware reports listening position as
 * an absolute file path (`book_state.read_position`), so this is what lets us
 * turn "the user is 39 seconds into /mnt/ext1/Audio Books/X/Y.m4b" back into
 * an Audiobookshelf item id at sync time.
 */
typedef struct {
    char      item_id[ABS_MAX_ID];
    char      dir[ABS_MAX_DIR];       /* absolute, no trailing slash */
    char      title[ABS_MAX_NAME];
    char      author[ABS_MAX_NAME];
    double    duration;               /* seconds, from ABS */
    long long size;                   /* bytes on disk */
    int       track_count;
    double    synced_pos;   /* last position pushed to ABS, seconds */
} abs_download;

typedef struct {
    abs_download items[ABS_MAX_DOWNLOADS];
    int count;
} abs_manifest;

void abs_manifest_parse(const char *text, abs_manifest *m);

/*
 * Serialize. Returns bytes written, or 0 if it would not fit -- callers must
 * treat 0 as failure and leave the existing file alone rather than truncating
 * it.
 */
size_t abs_manifest_serialize(const abs_manifest *m, char *out, size_t out_size);

/* Insert or replace by item id. Returns 1 on success, 0 if full. */
int abs_manifest_put(abs_manifest *m, const abs_download *entry);

/* NULL if absent. */
const abs_download *abs_manifest_find(const abs_manifest *m, const char *item_id);

/* Remove by item id. Returns 1 if something was removed. */
int abs_manifest_remove(abs_manifest *m, const char *item_id);

/*
 * Find the entry whose directory contains `path`.
 *
 * This is the lookup that makes sync work: given a path out of the firmware's
 * audiobook database, which ABS item is it?
 */
const abs_download *abs_manifest_find_by_path(const abs_manifest *m,
                                              const char *path);

/*
 * Build the download directory for a book: "<author> - <title>", sanitized,
 * under ABS_AUDIO_DIR. Returns length written, or 0 if it would not fit.
 */
size_t abs_manifest_dir_for(const char *author, const char *title,
                            char *out, size_t out_size);

#endif /* ABS_MANIFEST_H */
