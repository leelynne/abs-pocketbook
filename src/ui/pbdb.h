#ifndef ABS_PBDB_H
#define ABS_PBDB_H

#include "core/abs_api.h"

#define ABS_MAX_PB_BOOKS 128

/* One row of the firmware's audiobook state. */
typedef struct {
    char   path[512];      /* absolute file path, from read_position */
    double raw_loc;        /* the number inside #loc(...) */
    double duration;       /* seconds, from the audiobooks table */
    char   title[ABS_MAX_NAME];
    long long last_read;   /* unix timestamp */
} abs_pb_state;

/*
 * Read listening positions out of the firmware's audiobook database.
 *
 * Returns the number of rows written, or -1 if the database could not be
 * read. Opens strictly read-only: the firmware owns this file.
 */
int abs_pbdb_read_states(abs_pb_state *out, int max);

#endif /* ABS_PBDB_H */
