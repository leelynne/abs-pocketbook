#include "ui/covers.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/abs_api.h"
#include "core/paths.h"
#include "ui/log.h"
#include "ui/net.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_FAILURE_STRINGS
#include "vendor/stb_image.h"

#define COVER_DIR       ABS_APP_DIR "/covers"
#define COVER_CACHE_CAP (20 * 1024 * 1024)   /* bytes on disk */
#define COVER_MAX_BYTES (4 * 1024 * 1024)    /* refuse absurd images */
/*
 * Must exceed one screenful of rows, or a single page evicts its own covers
 * while drawing them. items_per_page is derived from screen height and capped
 * at 64.
 */
#define MEM_CACHE_SIZE  20                   /* decoded thumbnails held */

typedef struct {
    char      id[ABS_MAX_ID];
    int       w, h;
    ibitmap  *bmp;
    unsigned  used;      /* for LRU */
} mem_entry;

static mem_entry mem_cache[MEM_CACHE_SIZE];
static unsigned  use_clock;

void abs_covers_init(void)
{
    iv_mkdir(ABS_APP_DIR, 0777);
    iv_mkdir(COVER_DIR, 0777);
}

static void cache_path(const char *item_id, char *out, size_t out_size)
{
    snprintf(out, out_size, "%s/%s", COVER_DIR, item_id);
}

/* ------------------------------------------------------------ disk cache -- */

/*
 * Keep the cache under its cap by deleting least-recently-used files.
 *
 * Covers are small but a big library has thousands of them, and this app is
 * about to start storing whole audiobooks -- storage is not ours to waste.
 */
static void prune_disk_cache(void)
{
    struct entry { char name[ABS_MAX_ID + 8]; time_t atime; off_t size; };
    struct entry list[512];
    int count = 0;
    long long total = 0;

    DIR *d = opendir(COVER_DIR);
    if (d == NULL) return;

    struct dirent *de;
    while ((de = readdir(d)) != NULL && count < (int)(sizeof list / sizeof list[0])) {
        if (de->d_name[0] == '.') continue;

        char path[512];
        snprintf(path, sizeof path, "%s/%s", COVER_DIR, de->d_name);

        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        snprintf(list[count].name, sizeof list[count].name, "%s", de->d_name);
        list[count].atime = st.st_atime;
        list[count].size  = st.st_size;
        total += st.st_size;
        count++;
    }
    closedir(d);

    if (total <= COVER_CACHE_CAP) return;

    /* Selection sort by age; count is small and this runs rarely. */
    for (int i = 0; i < count && total > COVER_CACHE_CAP; i++) {
        int oldest = i;
        for (int j = i + 1; j < count; j++) {
            if (list[j].atime < list[oldest].atime) oldest = j;
        }
        struct entry tmp = list[i]; list[i] = list[oldest]; list[oldest] = tmp;

        char path[512];
        snprintf(path, sizeof path, "%s/%s", COVER_DIR, list[i].name);
        if (unlink(path) == 0) {
            total -= list[i].size;
            abs_log("cover cache: evicted %s", list[i].name);
        }
    }
}

static unsigned char *read_file(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long size = ftell(f);
    rewind(f);

    if (size <= 0 || size > COVER_MAX_BYTES) { fclose(f); return NULL; }

    unsigned char *buf = malloc((size_t)size);
    if (buf == NULL) { fclose(f); return NULL; }

    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);

    if (got != (size_t)size) { free(buf); return NULL; }

    *len_out = got;
    return buf;
}

/* ---------------------------------------------------------------- decode -- */

/*
 * Box-average downscale.
 *
 * Covers arrive around 500x800 and land in a ~60px-wide row, so nearest
 * neighbour would alias badly -- text on a cover turns to noise. Averaging
 * over the source rectangle costs one pass and looks right on e-ink.
 */
static void scale_rgb(const unsigned char *src, int sw, int sh,
                      unsigned char *dst, int dw, int dh)
{
    for (int y = 0; y < dh; y++) {
        int sy0 = y * sh / dh;
        int sy1 = (y + 1) * sh / dh;
        if (sy1 <= sy0) sy1 = sy0 + 1;

        for (int x = 0; x < dw; x++) {
            int sx0 = x * sw / dw;
            int sx1 = (x + 1) * sw / dw;
            if (sx1 <= sx0) sx1 = sx0 + 1;

            unsigned long r = 0, g = 0, b = 0, n = 0;
            for (int sy = sy0; sy < sy1 && sy < sh; sy++) {
                const unsigned char *row = src + (size_t)sy * sw * 3;
                for (int sx = sx0; sx < sx1 && sx < sw; sx++) {
                    r += row[sx * 3 + 0];
                    g += row[sx * 3 + 1];
                    b += row[sx * 3 + 2];
                    n++;
                }
            }
            if (n == 0) n = 1;

            unsigned char *o = dst + ((size_t)y * dw + x) * 3;
            o[0] = (unsigned char)(r / n);
            o[1] = (unsigned char)(g / n);
            o[2] = (unsigned char)(b / n);
        }
    }
}

/* Decode `bytes` and return a bitmap fitted inside (max_w, max_h). */
static ibitmap *decode_to_bitmap(const unsigned char *bytes, size_t len,
                                 int max_w, int max_h)
{
    int sw, sh, channels;
    unsigned char *img = stbi_load_from_memory(bytes, (int)len, &sw, &sh,
                                               &channels, 3);
    if (img == NULL) return NULL;

    /* Fit inside the box without distorting the cover. */
    int dw = max_w;
    int dh = (int)((long long)sh * dw / sw);
    if (dh > max_h) {
        dh = max_h;
        dw = (int)((long long)sw * dh / sh);
    }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    unsigned char *scaled = malloc((size_t)dw * dh * 3);
    if (scaled == NULL) { stbi_image_free(img); return NULL; }

    scale_rgb(img, sw, sh, scaled, dw, dh);
    stbi_image_free(img);

    /* inkview wants each row padded to a 4-byte boundary. */
    int scanline = (dw * 3 + 3) & ~3;
    ibitmap *bm = malloc(sizeof(ibitmap) + (size_t)scanline * dh);
    if (bm == NULL) { free(scaled); return NULL; }

    bm->width    = (unsigned short)dw;
    bm->height   = (unsigned short)dh;
    bm->depth    = 24;
    bm->scanline = (unsigned short)scanline;

    for (int y = 0; y < dh; y++) {
        unsigned char *row = bm->data + (size_t)y * scanline;
        memcpy(row, scaled + (size_t)y * dw * 3, (size_t)dw * 3);
        /* Zero the row padding rather than hand uninitialised heap to the
         * framework. */
        memset(row + dw * 3, 0, (size_t)scanline - (size_t)dw * 3);
    }

    free(scaled);
    return bm;
}

/* ---------------------------------------------------------- memory cache -- */

static ibitmap *mem_lookup(const char *id, int w, int h)
{
    for (int i = 0; i < MEM_CACHE_SIZE; i++) {
        if (mem_cache[i].bmp != NULL && mem_cache[i].w == w &&
            mem_cache[i].h == h && strcmp(mem_cache[i].id, id) == 0) {
            mem_cache[i].used = ++use_clock;
            return mem_cache[i].bmp;
        }
    }
    return NULL;
}

static void mem_store(const char *id, int w, int h, ibitmap *bmp)
{
    int slot = 0;
    for (int i = 0; i < MEM_CACHE_SIZE; i++) {
        if (mem_cache[i].bmp == NULL) { slot = i; goto place; }
        if (mem_cache[i].used < mem_cache[slot].used) slot = i;
    }
    free(mem_cache[slot].bmp);

place:
    snprintf(mem_cache[slot].id, sizeof mem_cache[slot].id, "%s", id);
    mem_cache[slot].w = w;
    mem_cache[slot].h = h;
    mem_cache[slot].bmp = bmp;
    mem_cache[slot].used = ++use_clock;
}

void abs_covers_free_memory(void)
{
    for (int i = 0; i < MEM_CACHE_SIZE; i++) {
        free(mem_cache[i].bmp);
        mem_cache[i].bmp = NULL;
        mem_cache[i].id[0] = '\0';
    }
}

/* ------------------------------------------------------------------ main -- */

int abs_cover_save_beside(const abs_config *cfg, const char *item_id,
                          const char *dir)
{
    char src_path[512];
    unsigned char *bytes = NULL;
    size_t len = 0;

    if (item_id == NULL || dir == NULL || dir[0] == '\0') return 0;

    /* Prefer the copy already on disk: viewing the book warms it, so this is
     * usually free. */
    cache_path(item_id, src_path, sizeof src_path);
    bytes = read_file(src_path, &len);

    if (bytes == NULL) {
        char url[ABS_MAX_URL + ABS_MAX_TOKEN + 96];
        if (abs_url_item_cover(cfg, item_id, url, sizeof url) == 0) return 0;

        abs_http_response res;
        if (!abs_http_get_auth(cfg, url, NULL, &res, COVER_MAX_BYTES)) return 0;
        if (res.status != 200 || res.len == 0) { abs_http_free(&res); return 0; }

        len = res.len;
        bytes = malloc(len);
        if (bytes != NULL) memcpy(bytes, res.data, len);
        abs_http_free(&res);
        if (bytes == NULL) return 0;
    }

    /* Name it for what it actually is rather than assuming JPEG. */
    const char *ext = "jpg";
    if (len > 8 && bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' &&
        bytes[3] == 'G') {
        ext = "png";
    }

    char dest[640];
    snprintf(dest, sizeof dest, "%s/cover.%s", dir, ext);

    FILE *f = fopen(dest, "wb");
    if (f == NULL) { free(bytes); return 0; }

    size_t written = fwrite(bytes, 1, len, f);
    fclose(f);
    free(bytes);

    if (written != len) {
        unlink(dest);
        abs_log("cover: short write to %s", dest);
        return 0;
    }

    abs_log("cover written: %s (%lu bytes)", dest, (unsigned long)len);
    return 1;
}

ibitmap *abs_cover_get(const abs_config *cfg, const char *item_id,
                       int w, int h, int allow_fetch)
{
    char path[512];
    unsigned char *bytes = NULL;
    size_t len = 0;

    if (item_id == NULL || item_id[0] == '\0') return NULL;

    ibitmap *cached = mem_lookup(item_id, w, h);
    if (cached != NULL) return cached;

    cache_path(item_id, path, sizeof path);
    bytes = read_file(path, &len);

    if (bytes == NULL) {
        if (!allow_fetch) return NULL;

        char url[ABS_MAX_URL + ABS_MAX_TOKEN + 96];
        if (abs_url_item_cover(cfg, item_id, url, sizeof url) == 0) return NULL;

        abs_http_response res;
        /* The cover URL carries ?token=, so no bearer header is needed. */
        if (!abs_http_get_auth(cfg, url, NULL, &res, COVER_MAX_BYTES)) {
            abs_log("cover fetch failed: %s", res.error);
            return NULL;
        }
        if (res.status != 200 || res.len == 0) {
            abs_log("cover %s -> HTTP %ld", item_id, res.status);
            abs_http_free(&res);
            return NULL;
        }

        FILE *f = fopen(path, "wb");
        if (f != NULL) {
            fwrite(res.data, 1, res.len, f);
            fclose(f);
            prune_disk_cache();
        }

        len = res.len;
        bytes = malloc(len);
        if (bytes != NULL) memcpy(bytes, res.data, len);
        abs_http_free(&res);

        if (bytes == NULL) return NULL;
    }

    ibitmap *bmp = decode_to_bitmap(bytes, len, w, h);
    free(bytes);

    if (bmp == NULL) {
        abs_log("cover %s failed to decode", item_id);
        /* A corrupt cached file would otherwise fail forever. */
        unlink(path);
        return NULL;
    }

    mem_store(item_id, w, h, bmp);
    return bmp;
}
