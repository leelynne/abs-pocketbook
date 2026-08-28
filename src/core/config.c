#include "core/config.h"

#include <stdio.h>
#include <string.h>

static void copy_field(char *dst, size_t dst_size, const char *src, size_t len)
{
    if (len >= dst_size) len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static const char *skip_spaces(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    return p;
}

static const char *trim_end(const char *start, const char *end)
{
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
    return end;
}

void abs_config_parse(const char *text, abs_config *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    if (text == NULL) return;

    const char *p = text;
    while (*p != '\0') {
        const char *line_end = strchr(p, '\n');
        if (line_end == NULL) line_end = p + strlen(p);

        const char *key = skip_spaces(p, line_end);

        /* Comments and blank lines. */
        if (key < line_end && *key != '#') {
            const char *eq = memchr(key, '=', (size_t)(line_end - key));
            if (eq != NULL) {
                const char *key_end = trim_end(key, eq);
                const char *val = skip_spaces(eq + 1, line_end);
                const char *val_end = trim_end(val, line_end);

                size_t key_len = (size_t)(key_end - key);
                size_t val_len = (size_t)(val_end - val);

                if (key_len == 6 && memcmp(key, "server", 6) == 0) {
                    copy_field(cfg->server, sizeof cfg->server, val, val_len);
                } else if (key_len == 5 && memcmp(key, "token", 5) == 0) {
                    copy_field(cfg->token, sizeof cfg->token, val, val_len);
                } else if (key_len == 8 && memcmp(key, "insecure", 8) == 0) {
                    cfg->insecure = (val_len > 0 && val[0] == '1') ? 1 : 0;
                }
                /* Unknown keys are ignored on purpose -- forward compatibility. */
            }
        }

        if (*line_end == '\0') break;
        p = line_end + 1;
    }

    abs_config_normalize_server(cfg->server);
}

size_t abs_config_serialize(const abs_config *cfg, char *out, size_t out_size)
{
    int n = snprintf(out, out_size,
                     "# Audiobookshelf client configuration\n"
                     "server=%s\n"
                     "token=%s\n"
                     "# insecure=1 skips TLS certificate verification.\n"
                     "# Only for a self-signed certificate on a server you control.\n"
                     "insecure=%d\n",
                     cfg->server, cfg->token, cfg->insecure ? 1 : 0);

    if (n < 0 || (size_t)n >= out_size) return 0;
    return (size_t)n;
}

void abs_config_normalize_server(char *server)
{
    if (server == NULL) return;

    /* Trim leading whitespace by shifting left. */
    char *start = server;
    while (*start == ' ' || *start == '\t') start++;
    if (start != server) memmove(server, start, strlen(start) + 1);

    size_t len = strlen(server);
    while (len > 0 && (server[len - 1] == ' ' || server[len - 1] == '\t' ||
                       server[len - 1] == '\r' || server[len - 1] == '/')) {
        server[--len] = '\0';
    }
}

abs_config_status abs_config_validate(const abs_config *cfg)
{
    if (cfg->server[0] == '\0') return ABS_CONFIG_NO_SERVER;

    if (strncmp(cfg->server, "http://", 7) != 0 &&
        strncmp(cfg->server, "https://", 8) != 0) {
        return ABS_CONFIG_BAD_SCHEME;
    }

    if (cfg->token[0] == '\0') return ABS_CONFIG_NO_TOKEN;

    return ABS_CONFIG_OK;
}

const char *abs_config_status_message(abs_config_status status)
{
    switch (status) {
    case ABS_CONFIG_OK:          return NULL;
    case ABS_CONFIG_NO_SERVER:   return "No server address set.";
    case ABS_CONFIG_NO_TOKEN:    return "No API key set.";
    case ABS_CONFIG_BAD_SCHEME:  return "Server must start with http:// or https://";
    }
    return "Unknown configuration error.";
}
