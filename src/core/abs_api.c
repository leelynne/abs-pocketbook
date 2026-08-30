#include "core/abs_api.h"
#include "core/paths.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "vendor/cJSON.h"

static size_t write_url(char *out, size_t out_size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static size_t write_url(char *out, size_t out_size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, out_size, fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t)n >= out_size) {
        if (out_size > 0) out[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

size_t abs_url_libraries(const abs_config *cfg, char *out, size_t out_size)
{
    return write_url(out, out_size, "%s/api/libraries", cfg->server);
}

size_t abs_url_item_cover(const abs_config *cfg, const char *item_id,
                          char *out, size_t out_size)
{
    return write_url(out, out_size, "%s/api/items/%s/cover",
                     cfg->server, item_id);
}

size_t abs_auth_header_token(const char *token, char *out, size_t out_size)
{
    return write_url(out, out_size, "Authorization: Bearer %s", token);
}

size_t abs_url_login(const abs_config *cfg, char *out, size_t out_size)
{
    return write_url(out, out_size, "%s/login", cfg->server);
}

size_t abs_url_users(const abs_config *cfg, char *out, size_t out_size)
{
    return write_url(out, out_size, "%s/api/users", cfg->server);
}

size_t abs_url_api_keys(const abs_config *cfg, char *out, size_t out_size)
{
    return write_url(out, out_size, "%s/api/api-keys", cfg->server);
}

static void copy_string_field(const cJSON *obj, const char *key,
                              char *dst, size_t dst_size)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (dst == NULL || dst_size == 0) return;
    dst[0] = '\0';

    if (cJSON_IsString(item) && item->valuestring != NULL) {
        abs_str_copy_n(dst, dst_size, item->valuestring,
                       strlen(item->valuestring));
    }
}

int abs_parse_libraries(const char *json, abs_library *out, int max)
{
    if (json == NULL || out == NULL || max <= 0) return -1;

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) return -1;

    /* ABS returns { "libraries": [...] }. */
    const cJSON *libraries = cJSON_GetObjectItemCaseSensitive(root, "libraries");
    if (!cJSON_IsArray(libraries)) {
        cJSON_Delete(root);
        return -1;
    }

    int count = 0;
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, libraries) {
        if (count >= max) break;
        if (!cJSON_IsObject(entry)) continue;

        abs_library lib;
        copy_string_field(entry, "id", lib.id, sizeof lib.id);
        copy_string_field(entry, "name", lib.name, sizeof lib.name);
        copy_string_field(entry, "mediaType", lib.media_type, sizeof lib.media_type);

        /* No id means nothing we can request later. */
        if (lib.id[0] == '\0') continue;

        /* Podcast libraries hold no audiobooks -- skip them rather than show
         * the user shelves that will always be empty for our purposes. */
        if (strcmp(lib.media_type, "book") != 0) continue;

        out[count++] = lib;
    }

    cJSON_Delete(root);
    return count;
}

/* ---------------------------------------------------------------- sign-in -- */

/* Render a cJSON object into `out`, then free it. Returns bytes written. */
static size_t render_body(cJSON *obj, char *out, size_t out_size)
{
    size_t written = 0;

    if (obj == NULL) return 0;

    char *text = cJSON_PrintUnformatted(obj);
    if (text != NULL) {
        size_t len = strlen(text);
        if (len < out_size) {
            memcpy(out, text, len + 1);
            written = len;
        } else if (out_size > 0) {
            out[0] = '\0';
        }
        /* This buffer may hold the user's password: cJSON frees it without
         * clearing, leaving it readable in the heap. */
        memset(text, 0, len);
        cJSON_free(text);
    }

    cJSON_Delete(obj);
    return written;
}

size_t abs_build_login_body(const char *username, const char *password,
                            char *out, size_t out_size)
{
    if (username == NULL || password == NULL) return 0;

    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) return 0;

    if (cJSON_AddStringToObject(obj, "username", username) == NULL ||
        cJSON_AddStringToObject(obj, "password", password) == NULL) {
        cJSON_Delete(obj);
        return 0;
    }

    return render_body(obj, out, out_size);
}

size_t abs_build_api_key_body(const char *name, const char *user_id,
                              char *out, size_t out_size)
{
    if (name == NULL || user_id == NULL) return 0;

    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) return 0;

    if (cJSON_AddStringToObject(obj, "name", name) == NULL ||
        cJSON_AddStringToObject(obj, "userId", user_id) == NULL ||
        cJSON_AddBoolToObject(obj, "isActive", 1) == NULL) {
        cJSON_Delete(obj);
        return 0;
    }

    /* No expiresIn: the device may sit unused for weeks, and a key that
     * quietly expires looks identical to a broken app. */
    return render_body(obj, out, out_size);
}

int abs_parse_login(const char *json, char *token_out, size_t token_size,
                    char *user_id_out, size_t user_id_size)
{
    if (json == NULL) return 0;

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) return 0;

    if (token_out == NULL || token_size == 0 ||
        user_id_out == NULL || user_id_size == 0) {
        cJSON_Delete(root);
        return 0;
    }

    const cJSON *user = cJSON_GetObjectItemCaseSensitive(root, "user");
    if (!cJSON_IsObject(user)) {
        cJSON_Delete(root);
        return 0;
    }

    copy_string_field(user, "accessToken", token_out, token_size);
    copy_string_field(user, "id", user_id_out, user_id_size);

    int ok = (token_out[0] != '\0' && user_id_out[0] != '\0');
    cJSON_Delete(root);
    return ok;
}

int abs_parse_users(const char *json, abs_user *out, int max)
{
    if (json == NULL || out == NULL || max <= 0) return -1;

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) return -1;

    const cJSON *users = cJSON_GetObjectItemCaseSensitive(root, "users");
    if (!cJSON_IsArray(users)) {
        cJSON_Delete(root);
        return -1;
    }

    int count = 0;
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, users) {
        if (count >= max) break;
        if (!cJSON_IsObject(entry)) continue;

        abs_user u;
        copy_string_field(entry, "id", u.id, sizeof u.id);
        copy_string_field(entry, "username", u.username, sizeof u.username);
        copy_string_field(entry, "type", u.type, sizeof u.type);

        if (u.id[0] == '\0') continue;

        out[count++] = u;
    }

    cJSON_Delete(root);
    return count;
}

int abs_parse_api_key(const char *json, char *key_out, size_t key_size)
{
    if (json == NULL) return 0;

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) return 0;

    /* Shape is { "apiKey": { "apiKey": "<the key>", ... } } -- the outer
     * object is the record, the inner string is the secret. */
    const cJSON *record = cJSON_GetObjectItemCaseSensitive(root, "apiKey");
    if (!cJSON_IsObject(record)) {
        cJSON_Delete(root);
        return 0;
    }

    copy_string_field(record, "apiKey", key_out, key_size);

    int ok = (key_out[0] != '\0');
    cJSON_Delete(root);
    return ok;
}

/* -------------------------------------------------------------- browsing -- */

size_t abs_url_library_items(const abs_config *cfg, const char *library_id,
                             int page, int limit, char *out, size_t out_size)
{
    return write_url(out, out_size,
                     "%s/api/libraries/%s/items?limit=%d&page=%d&sort=media.metadata.title",
                     cfg->server, library_id, limit, page);
}

size_t abs_url_item(const abs_config *cfg, const char *item_id,
                    char *out, size_t out_size)
{
    return write_url(out, out_size, "%s/api/items/%s?expanded=1",
                     cfg->server, item_id);
}

size_t abs_url_track_download(const abs_config *cfg, const char *item_id,
                              const char *ino, char *out, size_t out_size)
{
    return write_url(out, out_size, "%s/api/items/%s/file/%s/download",
                     cfg->server, item_id, ino);
}

/*
 * A number from the server, clamped to something that survives a cast.
 *
 * Values like 1e30 make a double-to-int conversion undefined, and a negative
 * total size would slip past the download free-space check.
 */
#define ABS_NUMBER_MAX 1.0e12

static double number_field(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(item)) return 0.0;

    double v = item->valuedouble;
    if (!(v > 0.0)) return 0.0;                    /* also catches NaN */
    return (v > ABS_NUMBER_MAX) ? ABS_NUMBER_MAX : v;
}

/*
 * publishedYear arrives as a string on some items and a number on others,
 * depending on where the metadata came from.
 */
static void copy_year_field(const cJSON *obj, const char *key,
                            char *dst, size_t dst_size)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    dst[0] = '\0';

    if (cJSON_IsString(item) && item->valuestring != NULL) {
        snprintf(dst, dst_size, "%s", item->valuestring);
    } else if (cJSON_IsNumber(item) && item->valuedouble > 0) {
        snprintf(dst, dst_size, "%d", (int)item->valuedouble);
    }
}

/* Pull one abs_item out of a library-item object. 0 if unusable. */
static int item_from_json(const cJSON *entry, abs_item *item)
{
    memset(item, 0, sizeof *item);

    copy_string_field(entry, "id", item->id, sizeof item->id);
    if (item->id[0] == '\0') return 0;

    const cJSON *media = cJSON_GetObjectItemCaseSensitive(entry, "media");
    if (cJSON_IsObject(media)) {
        item->duration = number_field(media, "duration");

        const cJSON *meta = cJSON_GetObjectItemCaseSensitive(media, "metadata");
        if (cJSON_IsObject(meta)) {
            copy_string_field(meta, "title", item->title, sizeof item->title);
            copy_string_field(meta, "authorName", item->author, sizeof item->author);
        }
    }

    if (item->title[0] == '\0') {
        snprintf(item->title, sizeof item->title, "%s", "(untitled)");
    }
    return 1;
}

size_t abs_url_encode(const char *in, char *out, size_t out_size)
{
    static const char *hex = "0123456789ABCDEF";
    size_t w = 0;

    if (out == NULL || out_size == 0) return 0;
    if (in == NULL) { out[0] = '\0'; return 0; }

    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        unsigned char c = *p;
        int unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                         (c >= '0' && c <= '9') ||
                         c == '-' || c == '_' || c == '.' || c == '~';

        if (unreserved) {
            if (w + 1 >= out_size) { out[0] = '\0'; return 0; }
            out[w++] = (char)c;
        } else {
            if (w + 3 >= out_size) { out[0] = '\0'; return 0; }
            out[w++] = '%';
            out[w++] = hex[c >> 4];
            out[w++] = hex[c & 0x0f];
        }
    }

    out[w] = '\0';
    return w;
}

void abs_redact_token(const char *url, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) return;
    if (url == NULL) { snprintf(out, out_size, "%s", "(null)"); return; }

    const char *tok = strstr(url, "token=");
    if (tok == NULL) {
        snprintf(out, out_size, "%s", url);
        return;
    }

    size_t keep = (size_t)(tok - url) + 6;      /* through "token=" */
    if (keep >= out_size) keep = out_size - 1;

    memcpy(out, url, keep);
    out[keep] = '\0';

    /* Preserve anything after the token value so the rest still reads. */
    const char *rest = strchr(tok, '&');
    snprintf(out + keep, out_size - keep, "REDACTED%s", rest ? rest : "");
}

size_t abs_url_search(const abs_config *cfg, const char *library_id,
                      const char *query, int limit, char *out, size_t out_size)
{
    char encoded[512];
    if (abs_url_encode(query, encoded, sizeof encoded) == 0) return 0;

    return write_url(out, out_size, "%s/api/libraries/%s/search?q=%s&limit=%d",
                     cfg->server, library_id, encoded, limit);
}

int abs_parse_search_items(const char *json, abs_item *out, int max)
{
    if (json == NULL || out == NULL || max <= 0) return -1;

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) return -1;

    /* Hits are wrapped: { "book": [ { "libraryItem": {...} } ] }. */
    const cJSON *books = cJSON_GetObjectItemCaseSensitive(root, "book");
    if (!cJSON_IsArray(books)) {
        cJSON_Delete(root);
        return -1;
    }

    int count = 0;
    const cJSON *hit = NULL;
    cJSON_ArrayForEach(hit, books) {
        if (count >= max) break;
        if (!cJSON_IsObject(hit)) continue;

        const cJSON *li = cJSON_GetObjectItemCaseSensitive(hit, "libraryItem");
        if (!cJSON_IsObject(li)) continue;

        if (item_from_json(li, &out[count])) count++;
    }

    cJSON_Delete(root);
    return count;
}

int abs_parse_progress(const char *json, double *current_time, double *duration,
                       int *is_finished)
{
    if (json == NULL) return 0;

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) return 0;

    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return 0; }

    /* An object with none of these fields is not a progress record. */
    const cJSON *ct = cJSON_GetObjectItemCaseSensitive(root, "currentTime");
    if (!cJSON_IsNumber(ct)) { cJSON_Delete(root); return 0; }

    if (current_time != NULL) *current_time = ct->valuedouble;
    if (duration != NULL)     *duration = number_field(root, "duration");
    if (is_finished != NULL) {
        *is_finished = cJSON_IsTrue(
            cJSON_GetObjectItemCaseSensitive(root, "isFinished"));
    }

    cJSON_Delete(root);
    return 1;
}

int abs_parse_items(const char *json, abs_item *out, int max, int *total_out)
{
    if (json == NULL || out == NULL || max <= 0) return -1;

    if (total_out != NULL) *total_out = 0;

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) return -1;

    const cJSON *results = cJSON_GetObjectItemCaseSensitive(root, "results");
    if (!cJSON_IsArray(results)) {
        cJSON_Delete(root);
        return -1;
    }

    if (total_out != NULL) {
        *total_out = (int)number_field(root, "total");
    }

    int count = 0;
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, results) {
        if (count >= max) break;
        if (!cJSON_IsObject(entry)) continue;

        if (item_from_json(entry, &out[count])) count++;
    }

    cJSON_Delete(root);
    return count;
}

int abs_parse_item_detail(const char *json, abs_item_detail *out)
{
    if (json == NULL || out == NULL) return 0;

    memset(out, 0, sizeof *out);

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) return 0;

    /* The expanded item is the root object, not wrapped in a envelope. */
    copy_string_field(root, "id", out->id, sizeof out->id);
    if (out->id[0] == '\0') {
        cJSON_Delete(root);
        return 0;
    }

    const cJSON *media = cJSON_GetObjectItemCaseSensitive(root, "media");
    if (cJSON_IsObject(media)) {
        out->duration     = number_field(media, "duration");
        out->size         = (long long)number_field(media, "size");
        out->num_tracks   = (int)number_field(media, "numTracks");
        out->num_chapters = (int)number_field(media, "numChapters");

        const cJSON *meta = cJSON_GetObjectItemCaseSensitive(media, "metadata");
        if (cJSON_IsObject(meta)) {
            copy_string_field(meta, "title", out->title, sizeof out->title);
            copy_string_field(meta, "subtitle", out->subtitle, sizeof out->subtitle);
            copy_string_field(meta, "authorName", out->author, sizeof out->author);
            copy_string_field(meta, "narratorName", out->narrator, sizeof out->narrator);
            copy_string_field(meta, "seriesName", out->series, sizeof out->series);
            copy_year_field(meta, "publishedYear", out->published_year,
                            sizeof out->published_year);

            /* Descriptions carry markup; flatten before anyone tries to draw it. */
            const cJSON *desc = cJSON_GetObjectItemCaseSensitive(meta, "description");
            if (cJSON_IsString(desc) && desc->valuestring != NULL) {
                abs_strip_html(desc->valuestring, out->description,
                               sizeof out->description);
            }
        }
    }

    /* Audio files, in track order -- what M3 downloads. */
    if (cJSON_IsObject(media)) {
        const cJSON *files = cJSON_GetObjectItemCaseSensitive(media, "audioFiles");
        if (cJSON_IsArray(files)) {
            const cJSON *entry = NULL;
            cJSON_ArrayForEach(entry, files) {
                if (out->track_count >= ABS_MAX_TRACKS) break;
                if (!cJSON_IsObject(entry)) continue;

                abs_track t;
                memset(&t, 0, sizeof t);

                /* ino is a number in some payloads and a string in others. */
                const cJSON *ino = cJSON_GetObjectItemCaseSensitive(entry, "ino");
                if (cJSON_IsString(ino) && ino->valuestring != NULL) {
                    snprintf(t.ino, sizeof t.ino, "%s", ino->valuestring);
                } else if (cJSON_IsNumber(ino)) {
                    snprintf(t.ino, sizeof t.ino, "%lld", (long long)ino->valuedouble);
                }
                if (t.ino[0] == '\0') continue;   /* cannot be fetched */

                t.duration = number_field(entry, "duration");

                const cJSON *meta = cJSON_GetObjectItemCaseSensitive(entry, "metadata");
                if (cJSON_IsObject(meta)) {
                    copy_string_field(meta, "filename", t.filename, sizeof t.filename);
                    t.size = (long long)number_field(meta, "size");
                }
                if (t.filename[0] == '\0') {
                    snprintf(t.filename, sizeof t.filename, "track_%02d.mp3",
                             out->track_count + 1);
                }

                out->tracks_total_size += t.size;
                out->tracks[out->track_count++] = t;
            }
        }
    }

    if (out->title[0] == '\0') {
        snprintf(out->title, sizeof out->title, "%s", "(untitled)");
    }

    cJSON_Delete(root);
    return 1;
}

/* --------------------------------------------------------------- display -- */

void abs_format_duration(double seconds, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) return;

    if (!(seconds > 0)) {              /* also catches NaN */
        snprintf(out, out_size, "%s", "--");
        return;
    }

    long total = (long)(seconds + 0.5);
    long hours = total / 3600;
    long mins  = (total % 3600) / 60;

    if (hours > 0) {
        snprintf(out, out_size, "%ldh %ldm", hours, mins);
    } else if (mins > 0) {
        snprintf(out, out_size, "%ldm", mins);
    } else {
        snprintf(out, out_size, "%lds", total);
    }
}

void abs_format_size(long long bytes, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) return;

    if (bytes <= 0) {
        snprintf(out, out_size, "%s", "--");
        return;
    }

    double gb = (double)bytes / (1024.0 * 1024.0 * 1024.0);
    double mb = (double)bytes / (1024.0 * 1024.0);

    /* Compare against the rounded value, or 1073741823 bytes prints "1024 MB". */
    if (mb >= 1023.5) {
        snprintf(out, out_size, "%.1f GB", gb);
    } else if (mb >= 1.0) {
        snprintf(out, out_size, "%.0f MB", mb);
    } else {
        snprintf(out, out_size, "%.0f KB", (double)bytes / 1024.0);
    }
}

/* The entities that actually turn up in scraped book descriptions. */
static int decode_entity(const char *p, const char **after, char *decoded)
{
    static const struct { const char *name; char ch; } table[] = {
        { "&amp;",  '&'  }, { "&lt;",   '<' }, { "&gt;",  '>' },
        { "&quot;", '"'  }, { "&apos;", '\'' }, { "&#39;", '\'' },
        { "&nbsp;", ' '  },
    };

    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        size_t len = strlen(table[i].name);
        if (strncmp(p, table[i].name, len) == 0) {
            *decoded = table[i].ch;
            *after = p + len;
            return 1;
        }
    }
    return 0;
}

void abs_strip_html(const char *in, char *out, size_t out_size)
{
    size_t w = 0;
    int pending_space = 0;

    if (out == NULL || out_size == 0) return;
    if (in == NULL) { out[0] = '\0'; return; }

    for (const char *p = in; *p != '\0'; ) {
        if (*p == '<') {
            /* <br> and </p> are paragraph breaks worth keeping as spaces;
             * everything else just disappears. */
            const char *close = strchr(p, '>');
            if (close == NULL) break;          /* unterminated tag: drop rest */
            if (w > 0) pending_space = 1;
            p = close + 1;
            continue;
        }

        char decoded;
        const char *after;
        if (*p == '&' && decode_entity(p, &after, &decoded)) {
            if (decoded == ' ') {
                if (w > 0) pending_space = 1;
            } else {
                if (pending_space && w + 1 < out_size) { out[w++] = ' '; pending_space = 0; }
                if (w + 1 >= out_size) break;
                out[w++] = decoded;
            }
            p = after;
            continue;
        }

        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            if (w > 0) pending_space = 1;
            p++;
            continue;
        }

        if (pending_space) {
            if (w + 1 >= out_size) break;
            out[w++] = ' ';
            pending_space = 0;
        }

        if (w + 1 >= out_size) break;
        out[w++] = *p++;
    }

    out[w] = '\0';
}
