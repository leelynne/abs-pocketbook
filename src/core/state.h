#ifndef ABS_STATE_H
#define ABS_STATE_H

#include <stddef.h>

#include "core/abs_api.h"

/*
 * Where the user was, so the next launch can put them back.
 *
 * The firmware's Home key backgrounds this app and then terminates it, and
 * there is no way to intercept that (the key never reaches the app at all).
 * Persisting the position is what stops that costing the user their place.
 */
typedef struct {
    int  screen;                          /* screen_id, as an int */
    char library_id[ABS_MAX_ID];
    char library_name[ABS_MAX_NAME];
    int  page;
    char item_id[ABS_MAX_ID];
} abs_state;

/* Same plain key=value format as the config file. */
void   abs_state_parse(const char *text, abs_state *st);
size_t abs_state_serialize(const abs_state *st, char *out, size_t out_size);

#endif /* ABS_STATE_H */
