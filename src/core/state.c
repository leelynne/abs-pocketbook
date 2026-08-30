#include "core/state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/paths.h"

void abs_state_parse(const char *text, abs_state *st)
{
    memset(st, 0, sizeof *st);
    if (text == NULL) return;

    const char *p = text;
    while (*p != '\0') {
        const char *line_end = strchr(p, '\n');
        if (line_end == NULL) line_end = p + strlen(p);

        if (*p != '#' && line_end > p) {
            const char *eq = memchr(p, '=', (size_t)(line_end - p));
            if (eq != NULL) {
                size_t klen = (size_t)(eq - p);
                const char *val = eq + 1;
                size_t vlen = (size_t)(line_end - val);
                while (vlen > 0 && (val[vlen - 1] == '\r')) vlen--;

                if (klen == 6 && memcmp(p, "screen", 6) == 0) {
                    st->screen = atoi(val);
                } else if (klen == 4 && memcmp(p, "page", 4) == 0) {
                    st->page = atoi(val);
                } else if (klen == 10 && memcmp(p, "library_id", 10) == 0) {
                    abs_str_copy_n(st->library_id, sizeof st->library_id, val, vlen);
                } else if (klen == 12 && memcmp(p, "library_name", 12) == 0) {
                    abs_str_copy_n(st->library_name, sizeof st->library_name, val, vlen);
                } else if (klen == 7 && memcmp(p, "item_id", 7) == 0) {
                    abs_str_copy_n(st->item_id, sizeof st->item_id, val, vlen);
                }
            }
        }

        if (*line_end == '\0') break;
        p = line_end + 1;
    }

    if (st->page < 0) st->page = 0;
}

/* Strip newlines from server-supplied text: this is a key=value file, and a
 * newline in a library name would inject state keys. */
static void flatten(char *dst, size_t dst_size, const char *src)
{
    size_t w = 0;

    if (dst == NULL || dst_size == 0) return;

    for (const unsigned char *p = (const unsigned char *)src;
         src != NULL && *p && w + 1 < dst_size; p++) {
        dst[w++] = (*p == '\n' || *p == '\r') ? ' ' : (char)*p;
    }
    dst[w] = '\0';
}

size_t abs_state_serialize(const abs_state *st, char *out, size_t out_size)
{
    char lib_id[ABS_MAX_ID], lib_name[ABS_MAX_NAME], item[ABS_MAX_ID];

    flatten(lib_id, sizeof lib_id, st->library_id);
    flatten(lib_name, sizeof lib_name, st->library_name);
    flatten(item, sizeof item, st->item_id);

    int n = snprintf(out, out_size,
                     "# Last position -- safe to delete\n"
                     "screen=%d\n"
                     "library_id=%s\n"
                     "library_name=%s\n"
                     "page=%d\n"
                     "item_id=%s\n",
                     st->screen, lib_id, lib_name, st->page, item);

    if (n < 0 || (size_t)n >= out_size) return 0;
    return (size_t)n;
}
