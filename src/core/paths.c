#include "core/paths.h"

#include <ctype.h>
#include <string.h>

/* Illegal on FAT32, plus a few that confuse shell-ish consumers on device. */
static int is_forbidden(unsigned char c)
{
    if (c < 0x20) return 1;
    return strchr("<>:\"/\\|?*", c) != NULL;
}

void abs_str_copy_n(char *dst, size_t dst_size, const char *src, size_t len)
{
    if (dst == NULL || dst_size == 0) return;

    if (src == NULL) { dst[0] = '\0'; return; }
    if (len >= dst_size) len = dst_size - 1;

    memcpy(dst, src, len);
    dst[len] = '\0';
}

size_t abs_sanitize_component(const char *in, char *out, size_t out_size)
{
    size_t w = 0;
    int pending_space = 0;

    if (out == NULL || out_size == 0) return 0;

    if (in != NULL) {
        for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
            unsigned char c = *p;

            if (isspace(c) || is_forbidden(c)) {
                /* Collapse any run of separators into a single space, and
                 * never emit one before real content. */
                if (w > 0) pending_space = 1;
                continue;
            }

            if (pending_space) {
                if (w + 1 >= out_size) break;
                out[w++] = ' ';
                pending_space = 0;
            }

            if (w + 1 >= out_size) break;
            out[w++] = (char)c;
        }
    }

    /* Trailing dots and spaces are silently dropped by FAT32. */
    while (w > 0 && (out[w - 1] == '.' || out[w - 1] == ' ')) w--;

    if (w == 0) {
        const char *fallback = "untitled";
        for (const char *p = fallback; *p && w + 1 < out_size; p++) out[w++] = *p;
    }

    out[w] = '\0';
    return w;
}
