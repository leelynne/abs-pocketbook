#ifndef ABS_CONFIG_H
#define ABS_CONFIG_H

#include <stddef.h>

#define ABS_MAX_URL   512
#define ABS_MAX_TOKEN 1024   /* ABS API keys are JWTs -- they get long. */

typedef struct {
    char server[ABS_MAX_URL];   /* e.g. "https://abs.example.com" (no trailing /) */
    char token[ABS_MAX_TOKEN];  /* ABS API key */
    int  insecure;              /* 1 = skip TLS verification (self-signed certs) */
} abs_config;

/* Result of validating a config before use. */
typedef enum {
    ABS_CONFIG_OK = 0,
    ABS_CONFIG_NO_SERVER,
    ABS_CONFIG_NO_TOKEN,
    ABS_CONFIG_BAD_SCHEME
} abs_config_status;

/*
 * Parse `text` (the contents of abs_client.cfg) into `cfg`.
 *
 * Format is one `key=value` per line, matching the plain-text convention the
 * PocketBook OPDS client settled on after hitting write-protection trouble with
 * binary saves. Unknown keys, blank lines, and `#` comments are ignored so a
 * config written by a newer build stays readable by an older one.
 *
 * Always leaves `cfg` fully initialised.
 */
void abs_config_parse(const char *text, abs_config *cfg);

/*
 * Serialize `cfg` into `out`. Returns the length written (excluding NUL), or 0
 * if it would not fit.
 */
size_t abs_config_serialize(const abs_config *cfg, char *out, size_t out_size);

/* Check a config is usable before we try to talk to a server with it. */
abs_config_status abs_config_validate(const abs_config *cfg);

/* Human-readable message for a validation failure (NULL when OK). */
const char *abs_config_status_message(abs_config_status status);

/*
 * Normalise a server URL in place: trim surrounding whitespace and strip any
 * trailing '/' so endpoint construction can always just append "/api/...".
 */
void abs_config_normalize_server(char *server);

#endif /* ABS_CONFIG_H */
