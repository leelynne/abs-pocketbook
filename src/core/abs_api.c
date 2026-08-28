#include "core/abs_api.h"

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
    return write_url(out, out_size, "%s/api/items/%s/cover?token=%s",
                     cfg->server, item_id, cfg->token);
}

size_t abs_auth_header(const abs_config *cfg, char *out, size_t out_size)
{
    return write_url(out, out_size, "Authorization: Bearer %s", cfg->token);
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
    dst[0] = '\0';
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        size_t len = strlen(item->valuestring);
        if (len >= dst_size) len = dst_size - 1;
        memcpy(dst, item->valuestring, len);
        dst[len] = '\0';
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
