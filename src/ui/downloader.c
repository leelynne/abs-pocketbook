#include "ui/downloader.h"

#include <curl/curl.h>
#include <errno.h>
#include <inkview.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "core/paths.h"
#include "ui/log.h"

static abs_dl_status status;

static CURLM  *multi;
static CURL   *easy;
static FILE   *out_file;
static char    part_path[ABS_MAX_DIR + ABS_MAX_NAME + 16];
static char    final_path[ABS_MAX_DIR + ABS_MAX_NAME + 8];
static char    target_dir[ABS_MAX_DIR];

static abs_config      dl_cfg;
static abs_item_detail dl_item;
static long long       completed_bytes;   /* tracks finished before this one */
static int             sleep_was_blocked;

static int start_track(int index);

/* ------------------------------------------------------------------ utils -- */

long long abs_dl_free_space(void)
{
    struct statvfs st;
    if (statvfs(ABS_AUDIO_DIR, &st) != 0) {
        if (statvfs("/mnt/ext1", &st) != 0) return 0;
    }
    return (long long)st.f_bavail * (long long)st.f_frsize;
}

/* Create every missing component of `path`. */
static int make_dirs(const char *path)
{
    char tmp[ABS_MAX_DIR];
    snprintf(tmp, sizeof tmp, "%s", path);

    for (char *p = tmp + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            iv_mkdir(tmp, 0777);
            *p = '/';
        }
    }
    iv_mkdir(tmp, 0777);

    struct stat sb;
    return (stat(path, &sb) == 0 && S_ISDIR(sb.st_mode)) ? 1 : 0;
}

static void block_sleep(int on)
{
    /*
     * A download runs for minutes; the device would otherwise nap partway
     * through and drop the transfer. Restored the moment we stop.
     */
    if (on && !sleep_was_blocked) {
        iv_sleepmode(0);
        sleep_was_blocked = 1;
    } else if (!on && sleep_was_blocked) {
        iv_sleepmode(1);
        sleep_was_blocked = 0;
    }
}

static void cleanup_transfer(void)
{
    if (easy != NULL) {
        if (multi != NULL) curl_multi_remove_handle(multi, easy);
        curl_easy_cleanup(easy);
        easy = NULL;
    }
    if (out_file != NULL) {
        fclose(out_file);
        out_file = NULL;
    }
}

static void finish(abs_dl_state final_state, const char *msg)
{
    cleanup_transfer();
    if (multi != NULL) { curl_multi_cleanup(multi); multi = NULL; }
    block_sleep(0);

    status.state = final_state;
    if (msg != NULL) snprintf(status.message, sizeof status.message, "%s", msg);
    abs_log("download %s: %s", final_state == DL_DONE ? "done" : "stopped",
            status.message);
}

/* ------------------------------------------------------------------ write -- */

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    (void)userdata;
    size_t n = size * nmemb;

    if (out_file == NULL) return 0;
    size_t written = fwrite(ptr, 1, n, out_file);

    status.done_bytes = completed_bytes + (long long)ftell(out_file);
    return written;
}

/* ------------------------------------------------------------------ start -- */

static int start_track(int index)
{
    if (index >= dl_item.track_count) {
        finish(DL_DONE, "Download complete.");
        return 0;
    }

    const abs_track *t = &dl_item.tracks[index];
    char safe_name[ABS_MAX_NAME];
    abs_sanitize_component(t->filename, safe_name, sizeof safe_name);

    snprintf(final_path, sizeof final_path, "%s/%s", target_dir, safe_name);
    snprintf(part_path, sizeof part_path, "%s.part", final_path);

    status.track_index = index;
    snprintf(status.current_name, sizeof status.current_name, "%s", safe_name);

    /* Already downloaded in a previous run? Skip it. */
    struct stat sb;
    if (stat(final_path, &sb) == 0 && sb.st_size > 0) {
        abs_log("skip existing %s", safe_name);
        completed_bytes += sb.st_size;
        status.done_bytes = completed_bytes;
        return start_track(index + 1);
    }

    /* Resume a partial file rather than starting over -- these are big, and
     * a dropped connection two thirds of the way through is common. */
    long long resume_from = 0;
    if (stat(part_path, &sb) == 0 && sb.st_size > 0) {
        resume_from = sb.st_size;
        abs_log("resuming %s at %lld bytes", safe_name, resume_from);
    }

    out_file = fopen(part_path, resume_from > 0 ? "ab" : "wb");
    if (out_file == NULL) {
        finish(DL_FAILED, "Could not write to the audiobook folder.");
        return 0;
    }

    char url[ABS_MAX_URL + ABS_MAX_TOKEN + 128];
    if (abs_url_track_download(&dl_cfg, dl_item.id, t->ino, url, sizeof url) == 0) {
        finish(DL_FAILED, "Server address is too long.");
        return 0;
    }

    easy = curl_easy_init();
    if (easy == NULL) {
        finish(DL_FAILED, "Could not start the transfer.");
        return 0;
    }

    curl_easy_setopt(easy, CURLOPT_URL, url);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, "ABSClient-PocketBook/0.2");
    /* Abort a stalled transfer rather than hanging forever: under 1 KB/s for
     * 60s means the connection is gone. No overall timeout -- these are big. */
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, 60L);

    if (resume_from > 0) {
        curl_easy_setopt(easy, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)resume_from);
        completed_bytes -= 0;   /* resume bytes are counted via ftell */
    }
    if (dl_cfg.insecure) {
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    curl_multi_add_handle(multi, easy);
    abs_log("downloading %s (%lld bytes)", safe_name, t->size);
    return 1;
}

int abs_dl_start(const abs_config *cfg, const abs_item_detail *item,
                 const char *dir)
{
    memset(&status, 0, sizeof status);

    if (item->track_count <= 0) {
        status.state = DL_FAILED;
        snprintf(status.message, sizeof status.message,
                 "This book has no audio files.");
        return 0;
    }

    dl_cfg = *cfg;
    dl_item = *item;
    snprintf(target_dir, sizeof target_dir, "%s", dir);
    completed_bytes = 0;

    if (!make_dirs(target_dir)) {
        status.state = DL_FAILED;
        snprintf(status.message, sizeof status.message,
                 "Could not create the audiobook folder.");
        return 0;
    }

    long long free_bytes = abs_dl_free_space();
    if (free_bytes > 0 && item->tracks_total_size > 0 &&
        free_bytes < item->tracks_total_size + (32 * 1024 * 1024)) {
        status.state = DL_FAILED;
        snprintf(status.message, sizeof status.message,
                 "Not enough space: needs %lld MB, %lld MB free.",
                 item->tracks_total_size / (1024 * 1024),
                 free_bytes / (1024 * 1024));
        return 0;
    }

    multi = curl_multi_init();
    if (multi == NULL) {
        status.state = DL_FAILED;
        snprintf(status.message, sizeof status.message, "Could not start downloads.");
        return 0;
    }

    status.state = DL_RUNNING;
    status.track_count = item->track_count;
    status.total_bytes = item->tracks_total_size;
    snprintf(status.message, sizeof status.message, "Starting...");

    block_sleep(1);
    return start_track(0);
}

/* ------------------------------------------------------------------- pump -- */

int abs_dl_pump(void)
{
    if (status.state != DL_RUNNING || multi == NULL) return 0;

    int running = 0;
    CURLMcode mc = curl_multi_perform(multi, &running);
    if (mc != CURLM_OK) {
        finish(DL_FAILED, curl_multi_strerror(mc));
        return 0;
    }

    /* Harvest completions. */
    int queued = 0;
    CURLMsg *msg;
    while ((msg = curl_multi_info_read(multi, &queued)) != NULL) {
        if (msg->msg != CURLMSG_DONE) continue;

        CURLcode result = msg->data.result;
        long http_code = 0;
        curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE, &http_code);

        long long got = (out_file != NULL) ? (long long)ftell(out_file) : 0;
        cleanup_transfer();

        if (result != CURLE_OK) {
            char err[192];
            snprintf(err, sizeof err, "%s", curl_easy_strerror(result));
            finish(DL_FAILED, err);
            return 0;
        }
        if (http_code >= 400) {
            char err[128];
            snprintf(err, sizeof err, "Server refused the file (HTTP %ld).", http_code);
            finish(DL_FAILED, err);
            return 0;
        }

        /* Only now is the file whole, so only now does it get its real name.
         * A .part left behind is a resume point, not a broken audiobook. */
        if (rename(part_path, final_path) != 0) {
            finish(DL_FAILED, "Could not finish writing the file.");
            return 0;
        }

        completed_bytes += got;
        status.done_bytes = completed_bytes;
        abs_log("finished track %d/%d", status.track_index + 1, status.track_count);

        if (!start_track(status.track_index + 1)) return 0;
    }

    return status.state == DL_RUNNING;
}

void abs_dl_cancel(void)
{
    if (status.state != DL_RUNNING) return;
    /* The .part file is left deliberately: cancelling should cost time, not
     * the bytes already fetched. */
    finish(DL_CANCELLED, "Download stopped. It can be resumed.");
}

const abs_dl_status *abs_dl_status_get(void)
{
    return &status;
}
