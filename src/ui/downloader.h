#ifndef ABS_DOWNLOADER_H
#define ABS_DOWNLOADER_H

#include "core/abs_api.h"
#include "core/config.h"
#include "core/manifest.h"   /* ABS_MAX_DIR */

typedef enum {
    DL_IDLE = 0,
    DL_RUNNING,
    DL_DONE,
    DL_FAILED,
    DL_CANCELLED
} abs_dl_state;

typedef struct {
    abs_dl_state state;
    int          track_index;      /* 0-based, currently transferring */
    int          track_count;
    long long    done_bytes;       /* across the whole book */
    long long    total_bytes;
    char         current_name[ABS_MAX_NAME];
    char         message[256];
} abs_dl_status;

/*
 * Begin downloading every track of `item` into `dir`.
 *
 * Returns 1 if the transfer started. The transfer itself makes no progress
 * until abs_dl_pump() is called: this is deliberately a state machine driven
 * from a timer, because a 300 MB download inside an event handler would
 * freeze the UI for minutes and leave the panel stale.
 */
int abs_dl_start(const abs_config *cfg, const abs_item_detail *item,
                 const char *dir);

/*
 * Advance the transfer. Call from a timer, roughly every 100ms.
 * Returns 1 while there is still work to do.
 */
int abs_dl_pump(void);

/* Stop and leave the partial file in place, so it can be resumed later. */
void abs_dl_cancel(void);

const abs_dl_status *abs_dl_status_get(void);

/* Free bytes on the volume holding the audiobook directory, 0 if unknown. */
long long abs_dl_free_space(void);

#endif /* ABS_DOWNLOADER_H */
