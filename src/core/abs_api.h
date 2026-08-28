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

/* -------------------------------------------------------------- browsing -- */

#define ABS_MAX_DESC 4096

/* A row in the library list. Kept small: hundreds of these may be resident. */
typedef struct {
    char   id[ABS_MAX_ID];
    char   title[ABS_MAX_NAME];
    char   author[ABS_MAX_NAME];
    double duration;        /* seconds */
    int    num_tracks;
} abs_item;

#define ABS_MAX_TRACKS 64

/* One downloadable audio file of a book. */
typedef struct {
    char      ino[ABS_MAX_ID];        /* files are addressed by inode, not index */
    char      filename[ABS_MAX_NAME];
    long long size;                   /* bytes */
    double    duration;               /* seconds */
    int       index;                  /* 1-based track order from the server */
} abs_track;

/* Everything the detail screen shows. Fetched one at a time. */
typedef struct {
    char      id[ABS_MAX_ID];
    char      title[ABS_MAX_NAME];
    char      subtitle[ABS_MAX_NAME];
    char      author[ABS_MAX_NAME];
    char      narrator[ABS_MAX_NAME];
    char      series[ABS_MAX_NAME];
    char      published_year[16];
    double    duration;
    long long size;         /* bytes */
    int       num_tracks;
    int       num_chapters;
    char      description[ABS_MAX_DESC];

    abs_track tracks[ABS_MAX_TRACKS];
    int       track_count;
    long long tracks_total_size;      /* sum, for "download 312 MB?" */
} abs_item_detail;

/*
 * GET /api/libraries/:id/items -- one page of a library.
 *
 * `sort` may be NULL for the server default. Pagination is zero-based.
 */
size_t abs_url_library_items(const abs_config *cfg, const char *library_id,
                             int page, int limit, char *out, size_t out_size);

/* GET /api/items/:id?expanded=1 */
size_t abs_url_item(const abs_config *cfg, const char *item_id,
                    char *out, size_t out_size);

/*
 * Download URL for one audio file.
 *
 * The token goes in the query string: this URL is handed to a plain transfer
 * with no header plumbing, and ABS accepts ?token= as readily as a bearer
 * header (server/Auth.js builds its JWT strategy with both extractors).
 */
size_t abs_url_track_download(const abs_config *cfg, const char *item_id,
                              const char *ino, char *out, size_t out_size);

/*
 * Parse a page of library items. Returns the count written, or -1 on a body
 * we do not recognise. `total_out` (optional) receives the server's total item
 * count, which is what drives paging.
 */
int abs_parse_items(const char *json, abs_item *out, int max, int *total_out);

/* Parse a single expanded library item. Returns 1 on success. */
int abs_parse_item_detail(const char *json, abs_item_detail *out);

/* --------------------------------------------------------------- display -- */

/*
 * Render a duration as "3h 24m" / "47m" / "38s".
 *
 * Audiobook lengths are the one number a reader actually scans for, so this
 * never prints raw seconds.
 */
void abs_format_duration(double seconds, char *out, size_t out_size);

/* Render a byte count as "1.4 GB" / "312 MB". */
void abs_format_size(long long bytes, char *out, size_t out_size);

/*
 * Flatten HTML into plain text.
 *
 * Descriptions scraped from Audible routinely contain <p>, <br> and named
 * entities. inkview's DrawTextRect has no notion of markup, so tags would be
 * rendered literally. Drops tags, decodes the handful of entities that
 * actually show up, and collapses whitespace runs.
 */
void abs_strip_html(const char *in, char *out, size_t out_size);

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
