#ifndef ABS_SYNC_H
#define ABS_SYNC_H

#include <stddef.h>

#include "core/abs_api.h"

/*
 * Listening position as the firmware records it.
 *
 * book_state.read_position looks like
 *   /mnt/ext1/Audio Books/Author - Title/Book.m4b:#loc(87)
 *
 * Returns 1 on success. `loc_out` receives the raw number; deciding what unit
 * that is belongs to abs_sync_position_seconds().
 */
int abs_parse_read_position(const char *value, char *path_out, size_t path_size,
                            double *loc_out);

/*
 * Convert a raw #loc() value to seconds.
 *
 * Two observations on hardware both read as seconds (39 after ~40s, 87 after
 * ~90s), and the chapters table in the same database is in milliseconds -- so
 * the units are genuinely mixed and worth guarding. If the raw value cannot be
 * seconds because it exceeds the book's duration, but works as milliseconds,
 * it is treated as milliseconds. Getting this wrong scales every synced
 * position by 1000.
 */
double abs_sync_position_seconds(double raw_loc, double duration);

/* PATCH /api/me/progress/:libraryItemId */
size_t abs_url_progress(const abs_config *cfg, const char *item_id,
                        char *out, size_t out_size);

/*
 * Body for a progress update.
 *
 * isFinished is only asserted very near the end: marking a book finished is
 * destructive on the server side (it drops out of Continue Listening), so the
 * threshold is deliberately conservative.
 */
size_t abs_build_progress_body(double current_time, double duration,
                               char *out, size_t out_size);

/*
 * Is this new position worth sending?
 *
 * Avoids pushing a position we already sent, and avoids churn from the
 * firmware rewriting the same value.
 */
int abs_sync_should_push(double new_pos, double last_pushed);

#endif /* ABS_SYNC_H */
