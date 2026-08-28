#include "core/manifest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/paths.h"

/*
 * One record per line, tab-separated:
 *   item_id \t dir \t title \t author \t duration \t size \t tracks \t synced
 *
 * Tab-separated rather than key=value because titles and authors are free
 * text and routinely contain '=' and ':'; tabs they never contain.
 */
#define FIELD_SEP '\t'

static void copy_field(char *dst, size_t dst_size, const char *src, size_t len)
{
    if (len >= dst_size) len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* Advance past one field, copying it out. Returns the next field start. */
static const char *take_field(const char *p, const char *end,
                              char *dst, size_t dst_size)
{
    const char *sep = memchr(p, FIELD_SEP, (size_t)(end - p));
    const char *stop = (sep != NULL) ? sep : end;

    if (dst != NULL) copy_field(dst, dst_size, p, (size_t)(stop - p));
    return (sep != NULL) ? sep + 1 : end;
}

void abs_manifest_parse(const char *text, abs_manifest *m)
{
    memset(m, 0, sizeof *m);
    if (text == NULL) return;

    const char *p = text;
    while (*p != '\0' && m->count < ABS_MAX_DOWNLOADS) {
        const char *line_end = strchr(p, '\n');
        if (line_end == NULL) line_end = p + strlen(p);

        if (line_end > p && *p != '#') {
            abs_download e;
            char num[32];
            memset(&e, 0, sizeof e);

            const char *q = p;
            q = take_field(q, line_end, e.item_id, sizeof e.item_id);
            q = take_field(q, line_end, e.dir, sizeof e.dir);
            q = take_field(q, line_end, e.title, sizeof e.title);
            q = take_field(q, line_end, e.author, sizeof e.author);

            q = take_field(q, line_end, num, sizeof num);
            e.duration = atof(num);
            q = take_field(q, line_end, num, sizeof num);
            e.size = atoll(num);
            q = take_field(q, line_end, num, sizeof num);
            e.track_count = atoi(num);
            /* Added after the first release; absent in older files, where
             * take_field yields "" and this reads as 0. */
            take_field(q, line_end, num, sizeof num);
            e.synced_pos = atof(num);

            /* A record with no id or directory is unusable for sync. */
            if (e.item_id[0] != '\0' && e.dir[0] != '\0') {
                m->items[m->count++] = e;
            }
        }

        if (*line_end == '\0') break;
        p = line_end + 1;
    }
}

size_t abs_manifest_serialize(const abs_manifest *m, char *out, size_t out_size)
{
    size_t used = 0;

    int n = snprintf(out, out_size,
                     "# item\tdir\ttitle\tauthor\tduration\tsize\ttracks\tsynced\n");
    if (n < 0 || (size_t)n >= out_size) return 0;
    used = (size_t)n;

    for (int i = 0; i < m->count; i++) {
        const abs_download *e = &m->items[i];
        n = snprintf(out + used, out_size - used,
                     "%s\t%s\t%s\t%s\t%.0f\t%lld\t%d\t%.0f\n",
                     e->item_id, e->dir, e->title, e->author,
                     e->duration, e->size, e->track_count, e->synced_pos);
        if (n < 0 || (size_t)n >= out_size - used) return 0;
        used += (size_t)n;
    }

    return used;
}

int abs_manifest_put(abs_manifest *m, const abs_download *entry)
{
    for (int i = 0; i < m->count; i++) {
        if (strcmp(m->items[i].item_id, entry->item_id) == 0) {
            m->items[i] = *entry;
            return 1;
        }
    }

    if (m->count >= ABS_MAX_DOWNLOADS) return 0;
    m->items[m->count++] = *entry;
    return 1;
}

const abs_download *abs_manifest_find(const abs_manifest *m, const char *item_id)
{
    if (item_id == NULL) return NULL;
    for (int i = 0; i < m->count; i++) {
        if (strcmp(m->items[i].item_id, item_id) == 0) return &m->items[i];
    }
    return NULL;
}

int abs_manifest_remove(abs_manifest *m, const char *item_id)
{
    for (int i = 0; i < m->count; i++) {
        if (strcmp(m->items[i].item_id, item_id) == 0) {
            memmove(&m->items[i], &m->items[i + 1],
                    sizeof m->items[0] * (size_t)(m->count - i - 1));
            m->count--;
            return 1;
        }
    }
    return 0;
}

const abs_download *abs_manifest_find_by_path(const abs_manifest *m,
                                              const char *path)
{
    if (path == NULL) return NULL;

    for (int i = 0; i < m->count; i++) {
        size_t dlen = strlen(m->items[i].dir);
        if (dlen == 0) continue;

        /* The path must sit inside the directory, not merely share a prefix:
         * ".../Dune" must not match ".../Dune Messiah/track.mp3". */
        if (strncmp(path, m->items[i].dir, dlen) == 0 && path[dlen] == '/') {
            return &m->items[i];
        }
    }
    return NULL;
}

size_t abs_manifest_dir_for(const char *author, const char *title,
                            char *out, size_t out_size)
{
    char safe_author[ABS_MAX_NAME];
    char safe_title[ABS_MAX_NAME];

    abs_sanitize_component(author, safe_author, sizeof safe_author);
    abs_sanitize_component(title, safe_title, sizeof safe_title);

    int n;
    if (author != NULL && author[0] != '\0') {
        n = snprintf(out, out_size, "%s/%s - %s", ABS_AUDIO_DIR,
                     safe_author, safe_title);
    } else {
        n = snprintf(out, out_size, "%s/%s", ABS_AUDIO_DIR, safe_title);
    }

    if (n < 0 || (size_t)n >= out_size) {
        if (out_size > 0) out[0] = '\0';
        return 0;
    }
    return (size_t)n;
}
