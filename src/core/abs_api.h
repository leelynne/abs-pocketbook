#ifndef ABS_API_H
#define ABS_API_H

#include <stddef.h>

#include "core/config.h"

#define ABS_MAX_ID    64
#define ABS_MAX_NAME  256

/*
 * Endpoint URL builders.
 *
 * Each writes a full URL into `out` and returns the length written, or 0 if it
 * would not fit (callers must treat 0 as failure and not send the request).
 * `cfg->server` is assumed already normalised -- no trailing slash.
 */
size_t abs_url_libraries(const abs_config *cfg, char *out, size_t out_size);

/*
 * Cover art. The token goes in the query string rather than a header because
 * ABS accepts either (its JWT strategy extracts from Authorization OR ?token=),
 * and image fetches are simpler to issue without custom headers.
 */
size_t abs_url_item_cover(const abs_config *cfg, const char *item_id,
                          char *out, size_t out_size);

/* Value for the Authorization header, e.g. "Authorization: Bearer eyJ...". */
size_t abs_auth_header(const abs_config *cfg, char *out, size_t out_size);

/* Auth header from an explicit token (used mid-sign-in, before we have a key). */
size_t abs_auth_header_token(const char *token, char *out, size_t out_size);

/* Note: login lives at /login, NOT /api/login -- it is mounted above the
 * /api router, before the auth middleware. */
size_t abs_url_login(const abs_config *cfg, char *out, size_t out_size);
size_t abs_url_users(const abs_config *cfg, char *out, size_t out_size);
size_t abs_url_api_keys(const abs_config *cfg, char *out, size_t out_size);

/* A library as we care about it. */
typedef struct {
    char id[ABS_MAX_ID];
    char name[ABS_MAX_NAME];
    char media_type[32];   /* "book" or "podcast" */
} abs_library;

/*
 * Parse the response body of GET /api/libraries.
 *
 * Writes up to `max` entries into `out` and returns how many were written, or
 * -1 if the body was not the shape we expect. Libraries whose mediaType is not
 * "book" are skipped: podcast libraries have no audiobooks for us to play.
 */
int abs_parse_libraries(const char *json, abs_library *out, int max);

/* ------------------------------------------------------------ sign-in --- */

#define ABS_MAX_USERNAME 128
#define ABS_MAX_PASSWORD 256

typedef struct {
    char id[ABS_MAX_ID];
    char username[ABS_MAX_NAME];
    char type[32];        /* "root", "admin", "user", "guest" */
} abs_user;

/*
 * Build request bodies. These go through a real JSON encoder rather than
 * snprintf: passwords routinely contain quotes and backslashes, and a
 * hand-rolled body would corrupt them.
 *
 * Return the length written, or 0 if it would not fit.
 */
size_t abs_build_login_body(const char *username, const char *password,
                            char *out, size_t out_size);

/*
 * Body for POST /api/api-keys.
 *
 * isActive is always sent true: the server reads it as `!!req.body.isActive`,
 * so omitting it silently creates a key that exists but authenticates nothing.
 */
size_t abs_build_api_key_body(const char *name, const char *user_id,
                              char *out, size_t out_size);

/*
 * Parse POST /login. Extracts the access token and the id of the user who
 * logged in. Returns 1 on success, 0 if the body was not the expected shape.
 */
int abs_parse_login(const char *json, char *token_out, size_t token_size,
                    char *user_id_out, size_t user_id_size);

/* Parse GET /api/users. Returns the count written, or -1 on a bad body. */
int abs_parse_users(const char *json, abs_user *out, int max);

/*
 * Parse POST /api/api-keys. The raw key is only ever returned here, at
 * creation -- there is no way to read it back later.
 */
int abs_parse_api_key(const char *json, char *key_out, size_t key_size);

#endif /* ABS_API_H */
