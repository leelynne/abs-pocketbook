#ifndef ABS_NET_H
#define ABS_NET_H

#include <stddef.h>

#include "core/config.h"

/* A response body accumulated in memory. Free with abs_http_free. */
typedef struct {
    char  *data;    /* NUL-terminated */
    size_t len;
    long   status;  /* HTTP status code, 0 if the request never completed */
    char   error[256];
} abs_http_response;

/*
 * Bring up networking if it is not already up.
 *
 * Returns 1 on success. On failure the reason is left in `err` (which may be
 * NULL). Safe to call repeatedly -- it checks first and only dials if needed.
 */
int abs_net_connect(char *err, size_t err_size);

/*
 * GET `url` with the config's bearer token.
 *
 * Returns 1 if the request completed (check `res->status` for the HTTP code),
 * 0 if it failed outright, with the reason in `res->error`.
 *
 * `max_bytes` caps the response so a wrong URL returning something enormous
 * cannot exhaust memory on a 512 MB device.
 */
int abs_http_get(const abs_config *cfg, const char *url,
                 abs_http_response *res, size_t max_bytes);

/*
 * As abs_http_get, but with an explicit bearer token instead of the config's
 * stored key. Used during sign-in, when we hold a login access token but do
 * not yet have an API key. Pass NULL for no Authorization header.
 */
int abs_http_get_auth(const abs_config *cfg, const char *url, const char *bearer,
                      abs_http_response *res, size_t max_bytes);

/*
 * POST `body` as application/json. `bearer` may be NULL (the login endpoint
 * sits above the auth middleware and takes no token).
 */
int abs_http_post_json(const abs_config *cfg, const char *url, const char *body,
                       const char *bearer, abs_http_response *res,
                       size_t max_bytes);

/* As abs_http_post_json, but PATCH -- what the progress endpoint expects. */
int abs_http_patch_json(const abs_config *cfg, const char *url, const char *body,
                        const char *bearer, abs_http_response *res,
                        size_t max_bytes);

void abs_http_free(abs_http_response *res);

/* Map an HTTP status to a message worth putting in front of a user. */
const char *abs_http_status_message(long status);

#endif /* ABS_NET_H */
