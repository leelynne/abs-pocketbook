#include "core/sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vendor/cJSON.h"

/* Below this many seconds of movement, a push is not worth the radio. */
#define SYNC_MIN_DELTA 5.0

/* Only claim a book is finished within this much of the end. */
#define FINISHED_TAIL  30.0

/*
 * How far past the server's duration a position may sit and still be read as
 * seconds. Covers the routine disagreement between the firmware's file length
 * and the server's summed track durations.
 */
#define SECONDS_SLACK  60.0

int abs_parse_read_position(const char *value, char *path_out, size_t path_size,
                            double *loc_out)
{
    if (value == NULL || path_out == NULL || loc_out == NULL) return 0;
    if (path_size == 0) return 0;

    path_out[0] = '\0';
    *loc_out = 0;

    /* Find the ":#loc(" separator. Search from the right: paths themselves
     * may contain ':' (PocketBook uses ':#zip(...)' for archive members). */
    const char *marker = NULL;
    for (const char *p = value; *p; p++) {
        if (strncmp(p, ":#loc(", 6) == 0) marker = p;
    }
    if (marker == NULL) return 0;

    const char *open_paren = marker + 6;
    const char *close_paren = strchr(open_paren, ')');
    if (close_paren == NULL || close_paren == open_paren) return 0;

    size_t path_len = (size_t)(marker - value);
    if (path_len == 0 || path_len >= path_size) return 0;
    memcpy(path_out, value, path_len);
    path_out[path_len] = '\0';

    char num[32];
    size_t num_len = (size_t)(close_paren - open_paren);
    if (num_len >= sizeof num) return 0;
    memcpy(num, open_paren, num_len);
    num[num_len] = '\0';

    *loc_out = atof(num);
    return 1;
}

double abs_sync_position_seconds(double raw_loc, double duration)
{
    if (raw_loc <= 0) return 0;
    if (duration <= 0) return raw_loc;

    /*
     * The firmware measures against the audio file; the server sums its
     * tracks. The two differ by a second or so, so finishing a book routinely
     * produces a position slightly PAST the server's duration. Clamp that --
     * concluding "must be milliseconds" there would turn a finished book into
     * 19 seconds and overwrite real progress with nothing.
     */
    if (raw_loc <= duration + SECONDS_SLACK) {
        return (raw_loc > duration) ? duration : raw_loc;
    }

    /*
     * Only reinterpret as milliseconds when seconds is wrong by orders of
     * magnitude, not merely by a rounding error at the end of a book.
     */
    double as_ms = raw_loc / 1000.0;
    if (raw_loc > duration * 10.0 && as_ms <= duration) return as_ms;

    /* Neither reading fits: clamp rather than send nonsense to the server. */
    return duration;
}

size_t abs_url_progress(const abs_config *cfg, const char *item_id,
                        char *out, size_t out_size)
{
    int n = snprintf(out, out_size, "%s/api/me/progress/%s", cfg->server, item_id);
    if (n < 0 || (size_t)n >= out_size) {
        if (out_size > 0) out[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

size_t abs_build_progress_body(double current_time, double duration,
                               char *out, size_t out_size)
{
    if (current_time < 0) current_time = 0;
    if (duration > 0 && current_time > duration) current_time = duration;

    double progress = (duration > 0) ? current_time / duration : 0.0;

    int finished = (duration > 0 && current_time >= duration - FINISHED_TAIL);

    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) return 0;

    cJSON_AddNumberToObject(obj, "currentTime", current_time);
    cJSON_AddNumberToObject(obj, "duration", duration);
    cJSON_AddNumberToObject(obj, "progress", progress);
    cJSON_AddBoolToObject(obj, "isFinished", finished);

    char *text = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    size_t written = 0;
    if (text != NULL) {
        size_t len = strlen(text);
        if (len < out_size) {
            memcpy(out, text, len + 1);
            written = len;
        } else if (out_size > 0) {
            out[0] = '\0';
        }
        cJSON_free(text);
    }
    return written;
}

int abs_sync_should_push(double new_pos, double last_pushed)
{
    if (new_pos <= 0) return 0;

    double delta = new_pos - last_pushed;
    if (delta < 0) delta = -delta;

    return delta >= SYNC_MIN_DELTA;
}
