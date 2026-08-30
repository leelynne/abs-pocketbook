/*
 * libFuzzer harness for the parsers in src/core.
 *
 * These are the app's whole attack surface: pure functions over bytes that
 * arrive from the server or from files on removable storage, with no I/O of
 * their own. That makes them cheap to fuzz and worth fuzzing -- a malicious or
 * compromised Audiobookshelf server controls every byte they see.
 *
 * The first input byte selects a target, so one binary covers them all.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/abs_api.h"
#include "core/config.h"
#include "core/manifest.h"
#include "core/paths.h"
#include "core/state.h"
#include "core/sync.h"

/* Parsers take C strings; fuzz input is a byte range. */
static char *dup_as_string(const uint8_t *data, size_t size)
{
    char *s = malloc(size + 1);
    if (s == NULL) return NULL;
    memcpy(s, data, size);
    s[size] = '\0';
    return s;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 2) return 0;

    uint8_t which = data[0];
    char *text = dup_as_string(data + 1, size - 1);
    if (text == NULL) return 0;

    switch (which % 12) {
    case 0: {
        abs_item items[8];
        int total = 0;
        abs_parse_items(text, items, 8, &total);
        break;
    }
    case 1: {
        abs_item_detail d;
        abs_parse_item_detail(text, &d);
        break;
    }
    case 2: {
        abs_item items[8];
        abs_parse_search_items(text, items, 8);
        break;
    }
    case 3: {
        abs_library libs[8];
        abs_parse_libraries(text, libs, 8);
        break;
    }
    case 4: {
        char token[ABS_MAX_TOKEN], id[ABS_MAX_ID];
        abs_parse_login(text, token, sizeof token, id, sizeof id);
        break;
    }
    case 5: {
        abs_user users[8];
        abs_parse_users(text, users, 8);
        break;
    }
    case 6: {
        char key[ABS_MAX_TOKEN];
        abs_parse_api_key(text, key, sizeof key);
        break;
    }
    case 7: {
        char out[512];
        abs_strip_html(text, out, sizeof out);
        break;
    }
    case 8: {
        /* Read from a file the firmware writes -- untrusted in its own right. */
        char path[512];
        double loc = 0;
        abs_parse_read_position(text, path, sizeof path, &loc);
        break;
    }
    case 9: {
        abs_manifest m;
        abs_manifest_parse(text, &m);
        /* Round-trip: serializing what we parsed must not overflow either. */
        char buf[8192];
        abs_manifest_serialize(&m, buf, sizeof buf);
        abs_manifest_find_by_path(&m, "/mnt/ext1/Audio Books/x/y.m4b");
        break;
    }
    case 10: {
        abs_config cfg;
        abs_config_parse(text, &cfg);
        char buf[4096];
        abs_config_serialize(&cfg, buf, sizeof buf);
        break;
    }
    case 11: {
        abs_state st;
        abs_state_parse(text, &st);
        char buf[2048];
        abs_state_serialize(&st, buf, sizeof buf);
        /* The sanitizer and the redactor see the same untrusted text. */
        char safe[128], red[512];
        abs_sanitize_component(text, safe, sizeof safe);
        abs_redact_token(text, red, sizeof red);
        break;
    }
    }

    free(text);
    return 0;
}
