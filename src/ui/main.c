/*
 * Setup form + library list.
 *
 * The device has no clipboard, and an ABS API key is a long JWT that nobody
 * should have to type on an e-ink keyboard. So the app never asks for a key:
 * it takes an admin sign-in once, mints a key over the API, stores that, and
 * forgets the password.
 *
 * Screens are drawn with inkview primitives (there is no widget toolkit) and
 * scale off ScreenWidth()/ScreenHeight() rather than assuming a Verse Pro.
 */
#include <inkview.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/abs_api.h"
#include "core/config.h"
#include "core/paths.h"
#include "core/manifest.h"
#include "core/state.h"
#include "core/sync.h"
#include "core/version.h"
#include "ui/covers.h"
#include "ui/downloader.h"
#include "ui/log.h"
#include "ui/net.h"
#include "ui/pbdb.h"

#define MAX_LIBRARIES 32
#define MAX_USERS     128
#define MAX_ITEMS     64      /* one page; paging fetches the next */

/*
 * How far one "page" of description scrolls. DrawTextRect reports no line
 * metrics, so this is a character count tuned to the body font rather than a
 * real line count -- see the note in docs/PLAN.md.
 */
#define DESC_PAGE     400
#define RESPONSE_CAP  (2 * 1024 * 1024)

typedef enum {
    SCREEN_SETUP,
    SCREEN_LIBRARIES,
    SCREEN_ITEMS,
    SCREEN_DETAIL,
    SCREEN_DOWNLOAD
} screen_id;

typedef struct {
    int x, y, w, h, action;
} hit_row;

#define MAX_ROWS 24

static ifont *font_title;
static ifont *font_body;
static ifont *font_hint;

static int screen_w, screen_h;
static int margin, row_h, header_h;

static screen_id current_screen = SCREEN_SETUP;

static abs_config config;
static abs_library libraries[MAX_LIBRARIES];
static int library_count = -1;          /* -1 = not fetched yet */
static char status_message[320];

static hit_row rows[MAX_ROWS];
static int row_count;

enum { ACT_NONE = 0, ACT_EDIT_SERVER, ACT_EDIT_ADMIN_USER, ACT_EDIT_ADMIN_PASS,
       ACT_EDIT_TARGET_USER, ACT_SIGN_IN, ACT_OPEN_SETUP, ACT_SHOW_LIBRARIES,
       ACT_PREV_PAGE, ACT_NEXT_PAGE, ACT_BACK_TO_ITEMS, ACT_GO_BACK,
       ACT_SCROLL_UP, ACT_SCROLL_DOWN,
       ACT_DOWNLOAD, ACT_CANCEL_DL, ACT_PLAY, ACT_DELETE,
       ACT_SEARCH, ACT_CLEAR_SEARCH,
       ACT_PICK_LIBRARY_BASE = 1000,     /* + index into `libraries` */
       ACT_PICK_ITEM_BASE    = 2000 };   /* + index into `items` */

/*
 * These double as the keyboard's edit buffers: OpenKeyboard writes into the
 * buffer it is given and hands that same pointer back to the callback, so the
 * callback must not copy onto it or clear it.
 */
static char form_server[ABS_MAX_URL];
static char form_admin_user[ABS_MAX_USERNAME];
static char form_admin_pass[ABS_MAX_PASSWORD];
static char form_target_user[ABS_MAX_USERNAME];

static abs_user users[MAX_USERS];

/* Browsing state. */
static abs_item items[MAX_ITEMS];
static int  item_count;
static int  item_total;          /* server's count, drives paging */
static int  item_page;
static int  items_per_page;      /* derived from screen height */
static char current_library_id[ABS_MAX_ID];
static char current_library_name[ABS_MAX_NAME];

static abs_item_detail detail;
static int detail_scroll;

/*
 * Snapshot of the book being downloaded.
 *
 * The transfer keeps running after the user leaves the download screen, so by
 * the time it finishes `detail` may hold a different book entirely. Filing the
 * result against that would break path-to-item resolution and, with it, sync.
 */
static abs_download dl_entry;

/* Progress the server holds for the open book, so the reader can see where
 * they got to on another device. -1 means "not fetched or none". */
static double server_position = -1;

/* Non-empty means the item list is showing search results, not a page. */
static char search_query[128];

/* The keyboard edits this; it is promoted to search_query only on success, so
 * cancelling cannot leave results on screen with the pager back. */
static char search_input[128];

/* Cover geometry, derived from row height at init. */
static int cover_w, cover_h;
static int item_row_y[MAX_ITEMS];   /* for repainting a single row */
static int cover_load_index;        /* progressive load cursor */

/* Position restored from the previous run, consumed as we navigate back to it. */
static abs_state restore;
static int restoring;

static abs_manifest manifest;

static void draw_current_screen(void);
static void fetch_libraries(void);
static void autofetch_cb(void);
static void fetch_items(void);
static void fetch_search(void);
static void fetch_detail(const char *item_id);
static void cover_tick_cb(void);
static void start_cover_loading(void);
static void save_state(void);
static void dl_pump_cb(void);
static void sync_cb(void);

/* ---------------------------------------------------------------- config -- */

static void config_load(void)
{
    memset(&config, 0, sizeof config);

    FILE *f = fopen(ABS_CONFIG_PATH, "r");
    if (f == NULL) {
        abs_log("no config at %s", ABS_CONFIG_PATH);
        return;
    }

    char buf[ABS_MAX_URL + ABS_MAX_TOKEN + 512];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    fclose(f);

    abs_config_parse(buf, &config);
    snprintf(form_server, sizeof form_server, "%s", config.server);

    abs_log("config loaded: server=%s token=%s insecure=%d",
            config.server[0] ? config.server : "(unset)",
            config.token[0] ? "(set)" : "(unset)", config.insecure);
}

static int config_save(void)
{
    iv_mkdir(ABS_APP_DIR, 0777);

    char buf[ABS_MAX_URL + ABS_MAX_TOKEN + 512];
    size_t n = abs_config_serialize(&config, buf, sizeof buf);
    if (n == 0) return 0;

    FILE *f = fopen(ABS_CONFIG_PATH, "w");
    if (f == NULL) {
        abs_log("could not write %s", ABS_CONFIG_PATH);
        return 0;
    }
    fwrite(buf, 1, n, f);
    fclose(f);
    abs_log("config saved");
    return 1;
}

/*
 * Persist where the user is.
 *
 * Written on EVT_BACKGROUND and EVT_EXIT rather than on every tap: the Home
 * key always produces both, and writing a file on each navigation would mean
 * a FAT32 write per tap for no benefit.
 */
static void save_state(void)
{
    abs_state st;
    char buf[1024];

    memset(&st, 0, sizeof st);
    st.screen = (int)current_screen;
    st.page = item_page;
    snprintf(st.library_id, sizeof st.library_id, "%s", current_library_id);
    snprintf(st.library_name, sizeof st.library_name, "%s", current_library_name);
    if (current_screen == SCREEN_DETAIL) {
        snprintf(st.item_id, sizeof st.item_id, "%s", detail.id);
    }

    size_t n = abs_state_serialize(&st, buf, sizeof buf);
    if (n == 0) return;

    FILE *f = fopen(ABS_STATE_PATH, "w");
    if (f == NULL) return;
    fwrite(buf, 1, n, f);
    fclose(f);
    abs_log("state saved: screen=%d lib=%s page=%d", st.screen, st.library_id, st.page);
}

static void manifest_load(void)
{
    memset(&manifest, 0, sizeof manifest);

    FILE *f = fopen(ABS_MANIFEST_PATH, "r");
    if (f == NULL) return;

    static char buf[64 * 1024];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    fclose(f);

    abs_manifest_parse(buf, &manifest);
    abs_log("manifest: %d downloaded book(s)", manifest.count);
}

static void manifest_save(void)
{
    static char buf[64 * 1024];
    size_t n = abs_manifest_serialize(&manifest, buf, sizeof buf);
    if (n == 0) { abs_log("manifest too large to write"); return; }

    FILE *f = fopen(ABS_MANIFEST_PATH, "w");
    if (f == NULL) { abs_log("could not write manifest"); return; }
    fwrite(buf, 1, n, f);
    fclose(f);
}

static void load_state(void)
{
    memset(&restore, 0, sizeof restore);

    FILE *f = fopen(ABS_STATE_PATH, "r");
    if (f == NULL) return;

    char buf[1024];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    fclose(f);

    abs_state_parse(buf, &restore);
    abs_log("state loaded: screen=%d lib=%s page=%d item=%s",
            restore.screen, restore.library_id, restore.page, restore.item_id);
}

static void wipe_password(void)
{
    memset(form_admin_pass, 0, sizeof form_admin_pass);
}

/* ---------------------------------------------------------------- drawing -- */

/*
 * Register a tappable area.
 *
 * Both axes are stored. Matching on y alone made every side-by-side pair
 * resolve to whichever was registered first -- Delete played the book, and
 * "Next >" fired Prev on any page after the first, leaving the hardware key
 * as the only way forward.
 */
static void add_row_at(int x, int y, int w, int h, int action)
{
    if (row_count < MAX_ROWS) {
        rows[row_count].x = x;
        rows[row_count].y = y;
        rows[row_count].w = w;
        rows[row_count].h = h;
        rows[row_count].action = action;
        row_count++;
    }
}

/* Full-width row. */
static void add_row(int y, int h, int action)
{
    add_row_at(0, y, screen_w, h, action);
}

/*
 * `back` draws a tappable chevron on the left.
 *
 * There is no way to ask a PocketBook whether it has page/back keys --
 * inkview offers QueryTouchpanel() but nothing for hardware keys, and keys are
 * user-remappable anyway. So every key action must also have a touch route.
 */
static void draw_header_ex(const char *title, int back)
{
    int text_x = margin;

    if (back) {
        int chip = header_h * 2 / 3;
        int cy = (header_h - chip) / 2;

        DrawRectRound(margin / 2, cy, chip, chip, chip / 4, BLACK);
        SetFont(font_body, BLACK);
        DrawTextRect(margin / 2, cy + chip / 4, chip, chip / 2, "<", ALIGN_CENTER);
        add_row(cy, chip, ACT_GO_BACK);

        text_x = margin / 2 + chip + margin / 2;
    }

    SetFont(font_title, BLACK);
    DrawTextRect(text_x, header_h / 4, screen_w - text_x - margin, header_h / 2,
                 title, ALIGN_LEFT | DOTS);
    DrawLine(0, header_h, screen_w, header_h, DGRAY);
}

static void draw_header(const char *title)
{
    draw_header_ex(title, 0);
}

static void draw_footer(const char *hint)
{
    SetFont(font_hint, DGRAY);
    DrawTextRect(margin, screen_h - row_h, screen_w - margin * 2, row_h / 2,
                 hint, ALIGN_CENTER | DOTS);
}

static int draw_field(int y, const char *label, const char *value, int action)
{
    int label_w = (screen_w - margin * 2) * 2 / 5;

    SetFont(font_body, BLACK);
    DrawTextRect(margin, y + row_h / 4, label_w, row_h / 2, label, ALIGN_LEFT | DOTS);
    DrawTextRect(margin + label_w, y + row_h / 4, screen_w - margin * 2 - label_w,
                 row_h / 2, value, ALIGN_RIGHT | DOTS);
    DrawLine(margin, y + row_h, screen_w - margin, y + row_h, LGRAY);

    add_row(y, row_h, action);
    return y + row_h;
}

static void draw_button_at(int x, int y, int w, const char *text, int action)
{
    DrawRectRound(x, y, w, row_h, row_h / 4, BLACK);
    SetFont(font_body, BLACK);
    DrawTextRect(x, y + row_h / 4, w, row_h / 2, text, ALIGN_CENTER);

    add_row_at(x, y, w, row_h, action);
}

static int draw_button(int y, const char *text, int action)
{
    draw_button_at(margin, y, screen_w - margin * 2, text, action);
    return y + row_h;
}

static void draw_setup_screen(void)
{
    ClearScreen();
    row_count = 0;

    draw_header("Audiobookshelf");

    int y = header_h + row_h / 3;

    SetFont(font_hint, DGRAY);
    DrawTextRect(margin, y, screen_w - margin * 2, row_h,
                 "Sign in once as an admin. This device gets its own API key; "
                 "your password is not saved.", ALIGN_LEFT);
    y += row_h + row_h / 4;

    y = draw_field(y, "Server",
                   form_server[0] ? form_server : "(tap to set)",
                   ACT_EDIT_SERVER);
    y = draw_field(y, "Admin username",
                   form_admin_user[0] ? form_admin_user : "(tap to set)",
                   ACT_EDIT_ADMIN_USER);
    y = draw_field(y, "Admin password",
                   form_admin_pass[0] ? "(entered)" : "(tap to set)",
                   ACT_EDIT_ADMIN_PASS);
    y = draw_field(y, "Create key for",
                   form_target_user[0] ? form_target_user
                                       : (form_admin_user[0] ? form_admin_user
                                                             : "(same as admin)"),
                   ACT_EDIT_TARGET_USER);

    y += row_h / 2;
    y = draw_button(y, "Sign in and create key", ACT_SIGN_IN);
    y += row_h / 3;

    if (status_message[0]) {
        SetFont(font_hint, BLACK);
        DrawTextRect(margin, y, screen_w - margin * 2, row_h * 3,
                     status_message, ALIGN_LEFT);
        y += row_h * 3;
    }

    if (config.token[0] && library_count >= 0) {
        y = draw_button(y, "Show libraries", ACT_SHOW_LIBRARIES);
    }

    SetFont(font_hint, DGRAY);
    DrawTextRect(margin, screen_h - row_h * 2, screen_w - margin * 2, row_h / 2,
                 abs_version_string(), ALIGN_CENTER);
    draw_footer("Back to exit");

    FullUpdate();
}

static void draw_libraries_screen(void)
{
    ClearScreen();
    row_count = 0;

    draw_header("Libraries");

    int y = header_h + row_h / 3;

    /*
     * Sync reports here, not on the setup screen: this is where the app lands
     * on launch, and the result was previously written to status_message and
     * never drawn.
     */
    if (status_message[0]) {
        SetFont(font_hint, DGRAY);
        DrawTextRect(margin, y, screen_w - margin * 2, row_h / 2,
                     status_message, ALIGN_LEFT | DOTS);
        y += row_h / 2 + row_h / 6;
        DrawLine(margin, y, screen_w - margin, y, LGRAY);
        y += row_h / 6;
    }

    if (library_count > 0) {
        SetFont(font_body, BLACK);
        for (int i = 0; i < library_count; i++) {
            DrawTextRect(margin, y + row_h / 4, screen_w - margin * 2, row_h / 2,
                         libraries[i].name, ALIGN_LEFT | DOTS);
            DrawLine(margin, y + row_h, screen_w - margin, y + row_h, LGRAY);
            add_row(y, row_h, ACT_PICK_LIBRARY_BASE + i);
            y += row_h;
            if (y > screen_h - row_h * 3) break;
        }
    } else {
        SetFont(font_body, DGRAY);
        DrawTextRect(margin, y, screen_w - margin * 2, row_h * 2,
                     (library_count == 0) ? "No book libraries on this server."
                                          : "Not connected yet.",
                     ALIGN_LEFT);
        y += row_h * 2;
    }

    y += row_h / 2;
    draw_button(y, "Setup", ACT_OPEN_SETUP);

    draw_footer("Menu for setup  |  Back to exit");
    FullUpdate();
}

/*
 * One row of the book list: cover on the left, title above author and length.
 *
 * Split out so a cover arriving later can repaint just its own row instead of
 * forcing a full-screen refresh, which on e-ink is both slow and visibly
 * flashy.
 */
static void draw_item_row(int i, int y)
{
    char meta[64];
    int text_x = margin;

    if (cover_w > 0) {
        int cy = y + (row_h - cover_h) / 2;

        /* Covers are only drawn from cache here: fetching happens on a timer,
         * never during a paint. */
        ibitmap *bmp = abs_cover_get(&config, items[i].id, cover_w, cover_h, 0);
        if (bmp != NULL) {
            DrawBitmap(margin + (cover_w - bmp->width) / 2,
                       y + (row_h - bmp->height) / 2, bmp);
        } else {
            /* Placeholder keeps the text aligned while covers load. */
            DrawRect(margin, cy, cover_w, cover_h, LGRAY);
        }
        text_x = margin + cover_w + margin / 2;
    }

    SetFont(font_body, BLACK);
    DrawTextRect(text_x, y + row_h / 8, screen_w - text_x - margin, row_h / 2,
                 items[i].title, ALIGN_LEFT | DOTS);

    abs_format_duration(items[i].duration, meta, sizeof meta);
    SetFont(font_hint, DGRAY);
    DrawTextRect(text_x, y + row_h / 2, screen_w - text_x - margin * 3,
                 row_h / 3, items[i].author, ALIGN_LEFT | DOTS);
    DrawTextRect(text_x, y + row_h / 2, screen_w - text_x - margin, row_h / 3,
                 meta, ALIGN_RIGHT | DOTS);

    DrawLine(margin, y + row_h, screen_w - margin, y + row_h, LGRAY);
}

static void draw_items_screen(void)
{
    char counter[64];

    ClearScreen();
    row_count = 0;

    draw_header_ex(search_query[0] ? search_query
                                   : (current_library_name[0] ? current_library_name
                                                              : "Books"), 1);

    int y = header_h + row_h / 4;

    if (item_count <= 0) {
        SetFont(font_body, DGRAY);
        DrawTextRect(margin, y, screen_w - margin * 2, row_h * 2,
                     "No books on this page.", ALIGN_LEFT);
    }

    for (int i = 0; i < item_count; i++) {
        item_row_y[i] = y;
        draw_item_row(i, y);
        add_row(y, row_h, ACT_PICK_ITEM_BASE + i);
        y += row_h;
    }

    /* Pager. */
    int pages = (items_per_page > 0)
        ? (item_total + items_per_page - 1) / items_per_page : 1;
    if (pages < 1) pages = 1;

    int py = screen_h - row_h * 2;

    if (search_query[0]) {
        /* Results are a single list, so the pager is replaced by a way out. */
        draw_button_at(margin, py, screen_w - margin * 2, "Clear search",
                       ACT_CLEAR_SEARCH);
    } else {
        int third = (screen_w - margin * 2 - margin) / 3;

        if (item_page > 0) {
            draw_button_at(margin, py, third, "< Prev", ACT_PREV_PAGE);
        }
        draw_button_at(margin + third + margin / 2, py, third, "Search", ACT_SEARCH);
        if (item_page + 1 < pages) {
            draw_button_at(margin + (third + margin / 2) * 2, py, third,
                           "Next >", ACT_NEXT_PAGE);
        }
    }

    if (search_query[0]) {
        snprintf(counter, sizeof counter, "%d result%s for \"%s\"",
                 item_count, item_count == 1 ? "" : "s", search_query);
    } else {
        snprintf(counter, sizeof counter, "Page %d of %d  -  %d books  -  %s",
                 item_page + 1, pages, item_total, abs_version_string());
    }
    draw_footer(counter);

    FullUpdate();
}

static void draw_detail_screen(void)
{
    char line[512];
    char dur[32], size[32];

    ClearScreen();
    row_count = 0;

    draw_header_ex(current_library_name[0] ? current_library_name : "Book", 1);

    int y = header_h + row_h / 3;

    int text_x = margin;
    int big_w = screen_w / 3;
    int big_h = big_w * 3 / 2;

    ibitmap *cover = abs_cover_get(&config, detail.id, big_w, big_h, 0);
    if (cover != NULL) {
        DrawBitmap(margin, y, cover);
        text_x = margin + cover->width + margin;
    }

    SetFont(font_title, BLACK);
    int title_h = row_h;
    DrawTextRect(text_x, y, screen_w - text_x - margin, title_h * 2,
                 detail.title, ALIGN_LEFT);
    y += title_h * 2;

    if (detail.subtitle[0]) {
        SetFont(font_hint, DGRAY);
        DrawTextRect(text_x, y, screen_w - text_x - margin, row_h / 2,
                     detail.subtitle, ALIGN_LEFT | DOTS);
        y += row_h / 2;
    }

    SetFont(font_body, BLACK);
    if (detail.author[0]) {
        DrawTextRect(text_x, y, screen_w - text_x - margin, row_h / 2,
                     detail.author, ALIGN_LEFT | DOTS);
        y += row_h / 2;
    }

    SetFont(font_hint, DGRAY);
    if (detail.narrator[0]) {
        snprintf(line, sizeof line, "Read by %s", detail.narrator);
        DrawTextRect(text_x, y, screen_w - text_x - margin, row_h / 3, line,
                     ALIGN_LEFT | DOTS);
        y += row_h / 3;
    }
    if (detail.series[0]) {
        DrawTextRect(text_x, y, screen_w - text_x - margin, row_h / 3,
                     detail.series, ALIGN_LEFT | DOTS);
        y += row_h / 3;
    }

    /* Push past the cover before the stats line and description. */
    if (cover != NULL && y < header_h + row_h / 3 + cover->height) {
        y = header_h + row_h / 3 + cover->height + row_h / 6;
    }

    abs_format_duration(detail.duration, dur, sizeof dur);
    abs_format_size(detail.size, size, sizeof size);
    snprintf(line, sizeof line, "%s  -  %d file%s  -  %d chapters  -  %s%s%s",
             dur, detail.num_tracks, detail.num_tracks == 1 ? "" : "s",
             detail.num_chapters, size,
             detail.published_year[0] ? "  -  " : "", detail.published_year);
    DrawTextRect(margin, y, screen_w - margin * 2, row_h / 3, line,
                 ALIGN_LEFT | DOTS);
    y += row_h / 2;

    if (server_position >= 0 || server_position == -2) {
        char pos_s[64];
        SetFont(font_hint, BLACK);
        if (server_position == -2) {
            snprintf(line, sizeof line, "Audiobookshelf: finished");
        } else {
            abs_format_duration(server_position, pos_s, sizeof pos_s);
            snprintf(line, sizeof line, "Audiobookshelf: %s of %s", pos_s, dur);
        }
        DrawTextRect(text_x, y, screen_w - text_x - margin, row_h / 3, line,
                     ALIGN_LEFT | DOTS);
        y += row_h / 3;
    }

    DrawLine(margin, y, screen_w - margin, y, LGRAY);
    y += row_h / 4;

    /* Description fills whatever is left, scrolled with the page buttons. */
    int desc_top = y;
    int desc_h = screen_h - row_h * 3 - desc_top;
    if (desc_h > 0 && detail.description[0]) {
        const char *text = detail.description;
        int len = (int)strlen(text);
        if (detail_scroll > 0 && detail_scroll < len) text += detail_scroll;

        SetFont(font_body, BLACK);
        DrawTextRect(margin, desc_top, screen_w - margin * 2, desc_h, text, ALIGN_LEFT);
    } else if (!detail.description[0]) {
        SetFont(font_hint, DGRAY);
        DrawTextRect(margin, desc_top, screen_w - margin * 2, row_h,
                     "No description.", ALIGN_LEFT);
    }

    /* Tap the lower part of the description to go down, the upper part to go
     * back up -- the usual e-reader idiom, and it needs no extra chrome. */
    if (desc_h > row_h) {
        int split = desc_top + desc_h / 3;
        add_row(desc_top, split - desc_top, ACT_SCROLL_UP);
        add_row(split, desc_top + desc_h - split, ACT_SCROLL_DOWN);
    }

    /* Actions sit above the description so they are always reachable without
     * scrolling. */
    int act_y = screen_h - row_h * 2;
    const abs_download *have = abs_manifest_find(&manifest, detail.id);
    int half = (screen_w - margin * 2 - margin / 2) / 2;

    if (have != NULL) {
        draw_button_at(margin, act_y, half, "Play", ACT_PLAY);
        draw_button_at(margin + half + margin / 2, act_y, half, "Delete", ACT_DELETE);
    } else if (detail.track_count > 0) {
        char label[64], sz[32];
        abs_format_size(detail.tracks_total_size, sz, sizeof sz);
        snprintf(label, sizeof label, "Download  (%s)", sz);
        draw_button_at(margin, act_y, screen_w - margin * 2, label, ACT_DOWNLOAD);
    }

    draw_footer("Tap lower half to scroll  |  < to go back");
    FullUpdate();
}

/*
 * The download screen is split in two.
 *
 * Only the counter, bar and byte total change while a transfer runs, so they
 * live in their own strip that is repainted with PartialUpdate. Repainting
 * the whole screen would mean a full e-ink refresh -- a visible flash -- every
 * second for the length of a 300 MB download.
 */
static int dl_dyn_y, dl_dyn_h;

static void draw_download_dynamic(int flush)
{
    const abs_dl_status *st = abs_dl_status_get();
    char line[256], done_s[32], total_s[32];

    if (dl_dyn_h <= 0) return;

    FillArea(0, dl_dyn_y, screen_w, dl_dyn_h, WHITE);

    int y = dl_dyn_y;

    SetFont(font_hint, DGRAY);
    snprintf(line, sizeof line, "File %d of %d  -  %s",
             st->track_index + 1, st->track_count, st->current_name);
    DrawTextRect(margin, y, screen_w - margin * 2, row_h / 2, line,
                 ALIGN_CENTER | DOTS);
    y += row_h;

    int bar_w = screen_w - margin * 2;
    int bar_h = row_h / 2;
    int pct = 0;
    if (st->total_bytes > 0) {
        pct = (int)((st->done_bytes * 100) / st->total_bytes);
        if (pct > 100) pct = 100;
    }

    DrawRect(margin, y, bar_w, bar_h, BLACK);
    if (pct > 0) FillArea(margin + 2, y + 2, (bar_w - 4) * pct / 100, bar_h - 4, BLACK);
    y += bar_h + row_h / 3;

    abs_format_size(st->done_bytes, done_s, sizeof done_s);
    abs_format_size(st->total_bytes, total_s, sizeof total_s);
    snprintf(line, sizeof line, "%s of %s  (%d%%)", done_s, total_s, pct);
    SetFont(font_body, BLACK);
    DrawTextRect(margin, y, screen_w - margin * 2, row_h / 2, line, ALIGN_CENTER);

    if (flush) PartialUpdate(0, dl_dyn_y, screen_w, dl_dyn_h);
}

static void draw_download_screen(void)
{
    const abs_dl_status *st = abs_dl_status_get();

    ClearScreen();
    row_count = 0;

    draw_header("Downloading");

    int y = header_h + row_h;

    SetFont(font_body, BLACK);
    DrawTextRect(margin, y, screen_w - margin * 2, row_h, detail.title,
                 ALIGN_CENTER | DOTS);
    y += row_h;

    /* Everything that changes during the transfer lives in this strip. */
    dl_dyn_y = y;
    dl_dyn_h = row_h * 2 + row_h / 2;
    draw_download_dynamic(0);
    y += dl_dyn_h + row_h / 3;

    if (st->state == DL_RUNNING) {
        DrawRectRound(margin, y, screen_w - margin * 2, row_h, row_h / 4, BLACK);
        SetFont(font_body, BLACK);
        DrawTextRect(margin, y + row_h / 4, screen_w - margin * 2, row_h / 2,
                     "Cancel", ALIGN_CENTER);
        add_row(y, row_h, ACT_CANCEL_DL);
    } else {
        SetFont(font_hint, BLACK);
        DrawTextRect(margin, y, screen_w - margin * 2, row_h * 2,
                     st->message, ALIGN_CENTER);
        y += row_h * 2;

        draw_button_at(margin, y, screen_w - margin * 2, "Back to book",
                       ACT_BACK_TO_ITEMS);
    }

    SetFont(font_hint, DGRAY);
    DrawTextRect(margin, screen_h - row_h, screen_w - margin * 2, row_h / 2,
                 "Sleep is held off while downloading", ALIGN_CENTER | DOTS);

    FullUpdate();
}

static void draw_current_screen(void)
{
    switch (current_screen) {
    case SCREEN_SETUP:     draw_setup_screen();     break;
    case SCREEN_LIBRARIES: draw_libraries_screen(); break;
    case SCREEN_ITEMS:     draw_items_screen();     break;
    case SCREEN_DETAIL:    draw_detail_screen();    break;
    case SCREEN_DOWNLOAD:  draw_download_screen();  break;
    }
}

/*
 * Paint a full-screen status and flush it before doing something blocking.
 *
 * inkview is single-threaded: anything slow in the event handler freezes the
 * UI and swallows taps. We cannot avoid that for NetConnect (which can take
 * ~30s while the radio wakes and associates), but we can make sure the screen
 * says what is happening instead of looking dead.
 */
static void draw_busy(const char *msg)
{
    ClearScreen();

    /* Drop the previous screen's hit targets: a tap landing while we are
     * blocked would otherwise fire whatever used to be at that spot. */
    row_count = 0;

    draw_header("Audiobookshelf");

    SetFont(font_body, BLACK);
    DrawTextRect(margin, screen_h / 2 - row_h, screen_w - margin * 2, row_h * 2,
                 msg, ALIGN_CENTER);

    SetFont(font_hint, DGRAY);
    DrawTextRect(margin, screen_h / 2 + row_h, screen_w - margin * 2, row_h,
                 "This can take up to a minute on the first connection.",
                 ALIGN_CENTER | DOTS);

    FullUpdate();
}

static void fail(const char *msg)
{
    wipe_password();
    snprintf(status_message, sizeof status_message, "%s", msg);
    current_screen = SCREEN_SETUP;
    draw_current_screen();
}

/* ------------------------------------------------------------- libraries -- */

static void fetch_libraries(void)
{
    char url[ABS_MAX_URL + 64];
    abs_http_response res;

    abs_config_status vs = abs_config_validate(&config);
    if (vs != ABS_CONFIG_OK) {
        fail(abs_config_status_message(vs));
        return;
    }

    char neterr[192];
    unsigned long t0 = abs_now_ms();
    if (!abs_net_connect(neterr, sizeof neterr)) {
        abs_log("net connect FAILED after %lums", abs_now_ms() - t0);
        fail(neterr);
        return;
    }
    abs_log("net connect took %lums", abs_now_ms() - t0);

    draw_busy("Loading libraries...");

    if (abs_url_libraries(&config, url, sizeof url) == 0) {
        fail("Server address is too long.");
        return;
    }

    t0 = abs_now_ms();
    int ok = abs_http_get(&config, url, &res, RESPONSE_CAP);
    abs_log("libraries request took %lums", abs_now_ms() - t0);

    if (!ok) { fail(res.error); return; }

    const char *status_msg = abs_http_status_message(res.status);
    if (status_msg != NULL) {
        abs_http_free(&res);
        fail(status_msg);
        return;
    }

    int n = abs_parse_libraries(res.data, libraries, MAX_LIBRARIES);
    abs_http_free(&res);

    if (n < 0) {
        fail("Unexpected reply. Is that address an Audiobookshelf server?");
        return;
    }

    library_count = n;
    status_message[0] = '\0';
    abs_log("fetched %d book libraries", n);

    /* Report anything the stock player recorded while we were not running.
     * Deferred so the list paints first. */
    if (manifest.count > 0) {
        SetWeakTimer("abs_sync", sync_cb, 400);
    }

    /* Carry on into wherever the user was when the app was last closed. */
    if (restoring && restore.library_id[0]) {
        for (int i = 0; i < library_count; i++) {
            if (strcmp(libraries[i].id, restore.library_id) == 0) {
                snprintf(current_library_id, sizeof current_library_id, "%s",
                         restore.library_id);
                snprintf(current_library_name, sizeof current_library_name, "%s",
                         restore.library_name[0] ? restore.library_name
                                                 : libraries[i].name);
                item_page = restore.page;
                draw_busy("Restoring...");
                fetch_items();
                return;
            }
        }
        /* The library is gone from the server -- fall back to the list. */
        restoring = 0;
    }

    current_screen = SCREEN_LIBRARIES;
    draw_current_screen();
}

/* -------------------------------------------------------------- browsing -- */

/*
 * Shared prologue for any request made after setup: make sure we have a
 * usable config and a network, painting the reason if not.
 * Returns 1 when it is safe to issue the request.
 */
static int ready_to_request(void)
{
    abs_config_status vs = abs_config_validate(&config);
    if (vs != ABS_CONFIG_OK) {
        fail(abs_config_status_message(vs));
        return 0;
    }

    char neterr[192];
    unsigned long t0 = abs_now_ms();
    if (!abs_net_connect(neterr, sizeof neterr)) {
        abs_log("net connect FAILED after %lums", abs_now_ms() - t0);
        fail(neterr);
        return 0;
    }
    return 1;
}

static void fetch_items(void)
{
    char url[ABS_MAX_URL + 128];
    abs_http_response res;

    if (!ready_to_request()) return;

    if (abs_url_library_items(&config, current_library_id, item_page,
                              items_per_page, url, sizeof url) == 0) {
        fail("Server address is too long.");
        return;
    }

    unsigned long t0 = abs_now_ms();
    int ok = abs_http_get(&config, url, &res, RESPONSE_CAP);
    abs_log("items request took %lums", abs_now_ms() - t0);

    if (!ok) { fail(res.error); return; }

    const char *status_msg = abs_http_status_message(res.status);
    if (status_msg != NULL) {
        abs_http_free(&res);
        fail(status_msg);
        return;
    }

    int n = abs_parse_items(res.data, items, MAX_ITEMS, &item_total);
    abs_http_free(&res);

    if (n < 0) { fail("Could not read the book list."); return; }

    item_count = n;
    abs_log("page %d: %d items of %d total", item_page, n, item_total);

    current_screen = SCREEN_ITEMS;
    draw_current_screen();
    start_cover_loading();

    if (restoring) {
        char wanted[ABS_MAX_ID];
        snprintf(wanted, sizeof wanted, "%s", restore.item_id);
        restoring = 0;                       /* only restore once */
        if (restore.screen == SCREEN_DETAIL && wanted[0]) {
            fetch_detail(wanted);
        }
    }
}

static void fetch_search(void)
{
    char url[ABS_MAX_URL + 640];
    abs_http_response res;

    if (!ready_to_request()) return;

    if (abs_url_search(&config, current_library_id, search_query, MAX_ITEMS,
                       url, sizeof url) == 0) {
        fail("That search is too long.");
        return;
    }

    unsigned long t0 = abs_now_ms();
    int ok = abs_http_get(&config, url, &res, RESPONSE_CAP);
    abs_log("search request took %lums", abs_now_ms() - t0);

    if (!ok) { fail(res.error); return; }

    const char *status_msg = abs_http_status_message(res.status);
    if (status_msg != NULL) {
        abs_http_free(&res);
        fail(status_msg);
        return;
    }

    int n = abs_parse_search_items(res.data, items, MAX_ITEMS);
    abs_http_free(&res);

    if (n < 0) { fail("Could not read the search results."); return; }

    item_count = n;
    item_total = n;
    item_page = 0;
    abs_log("search \"%s\": %d result(s)", search_query, n);

    current_screen = SCREEN_ITEMS;
    draw_current_screen();
    start_cover_loading();
}

static void fetch_detail(const char *item_id)
{
    char url[ABS_MAX_URL + 128];
    abs_http_response res;

    if (!ready_to_request()) return;

    if (abs_url_item(&config, item_id, url, sizeof url) == 0) {
        fail("Server address is too long.");
        return;
    }

    unsigned long t0 = abs_now_ms();
    int ok = abs_http_get(&config, url, &res, RESPONSE_CAP);
    abs_log("detail request took %lums", abs_now_ms() - t0);

    if (!ok) { fail(res.error); return; }

    const char *status_msg = abs_http_status_message(res.status);
    if (status_msg != NULL) {
        abs_http_free(&res);
        fail(status_msg);
        return;
    }

    int parsed = abs_parse_item_detail(res.data, &detail);
    abs_http_free(&res);

    if (!parsed) { fail("Could not read that book's details."); return; }

    detail_scroll = 0;
    server_position = -1;

    /*
     * What does the server think? Cheap, and it is how a reader sees progress
     * made on another device -- we deliberately do not write position back to
     * the firmware, so showing it is the honest half of that.
     */
    char purl[ABS_MAX_URL + 96];
    if (abs_url_progress(&config, detail.id, purl, sizeof purl) > 0) {
        abs_http_response pres;
        if (abs_http_get(&config, purl, &pres, 64 * 1024)) {
            double ct = 0, dur = 0;
            int finished = 0;
            /* 404 simply means no progress recorded yet. */
            if (pres.status == 200 &&
                abs_parse_progress(pres.data, &ct, &dur, &finished)) {
                server_position = finished ? -2 : ct;
            }
            abs_http_free(&pres);
        }
    }

    /*
     * Warm the cover before the first paint. Fetching between two paints cost
     * a second full-screen refresh for no benefit, since the fetch blocks
     * either way.
     */
    int big_w = screen_w / 3;
    abs_cover_get(&config, detail.id, big_w, big_w * 3 / 2, 1);

    current_screen = SCREEN_DETAIL;
    draw_current_screen();
}

/* ---------------------------------------------------------------- sign-in -- */

/* Find the id of `username` in the fetched user list. NULL if absent. */
static const char *find_user_id(const char *username, int count)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(users[i].username, username) == 0) return users[i].id;
    }
    /* Usernames are case-sensitive server-side, but a typed capital is a
     * likely slip, so fall back to a case-insensitive match. */
    for (int i = 0; i < count; i++) {
        if (strcasecmp(users[i].username, username) == 0) return users[i].id;
    }
    return NULL;
}

static void do_sign_in(void)
{
    char url[ABS_MAX_URL + 64];
    char body[ABS_MAX_USERNAME + ABS_MAX_PASSWORD + 128];
    char access_token[ABS_MAX_TOKEN];
    char self_id[ABS_MAX_ID];
    abs_http_response res;

    status_message[0] = '\0';

    snprintf(config.server, sizeof config.server, "%s", form_server);
    abs_config_normalize_server(config.server);

    if (config.server[0] == '\0') { fail("Set the server address first."); return; }
    if (strncmp(config.server, "http://", 7) != 0 &&
        strncmp(config.server, "https://", 8) != 0) {
        fail("Server must start with http:// or https://");
        return;
    }
    if (form_admin_user[0] == '\0') { fail("Enter the admin username."); return; }
    if (form_admin_pass[0] == '\0') { fail("Enter the admin password."); return; }

    /* The caller already painted "Connecting to Wi-Fi...". */
    char neterr[192];
    unsigned long t0 = abs_now_ms();
    if (!abs_net_connect(neterr, sizeof neterr)) {
        abs_log("net connect FAILED after %lums", abs_now_ms() - t0);
        fail(neterr);
        return;
    }
    abs_log("net connect took %lums", abs_now_ms() - t0);

    draw_busy("Signing in...");

    /* --- 1. log in ------------------------------------------------------- */
    if (abs_url_login(&config, url, sizeof url) == 0 ||
        abs_build_login_body(form_admin_user, form_admin_pass,
                             body, sizeof body) == 0) {
        fail("Could not build the sign-in request.");
        return;
    }

    int ok = abs_http_post_json(&config, url, body, NULL, &res, RESPONSE_CAP);
    memset(body, 0, sizeof body);       /* held the password */
    wipe_password();

    if (!ok) { fail(res.error); return; }

    if (res.status == 401 || res.status == 403) {
        abs_http_free(&res);
        fail("Server rejected that username or password.");
        return;
    }
    if (res.status >= 400) {
        long st = res.status;
        abs_http_free(&res);
        char msg[128];
        snprintf(msg, sizeof msg, "Sign-in failed (HTTP %ld).", st);
        fail(msg);
        return;
    }

    int parsed = abs_parse_login(res.data, access_token, sizeof access_token,
                                 self_id, sizeof self_id);
    abs_http_free(&res);

    if (!parsed) {
        fail("Signed in, but the reply had no access token.");
        return;
    }
    abs_log("signed in as %s", form_admin_user);
    draw_busy("Creating API key...");

    /* --- 2. resolve which user the key is for ---------------------------- */
    const char *target_id = self_id;
    int want_other = (form_target_user[0] != '\0' &&
                      strcmp(form_target_user, form_admin_user) != 0);

    if (want_other) {
        if (abs_url_users(&config, url, sizeof url) == 0) {
            fail("Server address is too long.");
            return;
        }

        ok = abs_http_get_auth(&config, url, access_token, &res, RESPONSE_CAP);
        if (!ok) { fail(res.error); return; }

        if (res.status == 403) {
            abs_http_free(&res);
            fail("That account is not an admin, so it cannot create keys for "
                 "other users. Leave 'Create key for' blank to make one for "
                 "yourself.");
            return;
        }
        if (res.status >= 400) {
            long st = res.status;
            abs_http_free(&res);
            char msg[128];
            snprintf(msg, sizeof msg, "Could not list users (HTTP %ld).", st);
            fail(msg);
            return;
        }

        int n = abs_parse_users(res.data, users, MAX_USERS);
        abs_http_free(&res);

        if (n <= 0) { fail("Could not read the user list."); return; }

        const char *found = find_user_id(form_target_user, n);
        if (found == NULL) {
            char msg[256];
            snprintf(msg, sizeof msg, "No user called \"%s\" on this server.",
                     form_target_user);
            fail(msg);
            return;
        }
        target_id = found;
    }

    /* --- 3. mint the key ------------------------------------------------- */
    char key_name[128];
    const char *model = GetDeviceModel();
    snprintf(key_name, sizeof key_name, "PocketBook %s", model ? model : "reader");

    if (abs_url_api_keys(&config, url, sizeof url) == 0 ||
        abs_build_api_key_body(key_name, target_id, body, sizeof body) == 0) {
        fail("Could not build the key request.");
        return;
    }

    ok = abs_http_post_json(&config, url, body, access_token, &res, RESPONSE_CAP);
    if (!ok) { fail(res.error); return; }

    if (res.status == 403) {
        abs_http_free(&res);
        fail("Not allowed to create a key for that account. Creating one for "
             "the root user requires signing in as root.");
        return;
    }
    if (res.status >= 400) {
        long st = res.status;
        abs_http_free(&res);
        char msg[160];
        snprintf(msg, sizeof msg,
                 "Could not create key (HTTP %ld). Admin rights are required.", st);
        fail(msg);
        return;
    }

    char key[ABS_MAX_TOKEN];
    parsed = abs_parse_api_key(res.data, key, sizeof key);
    abs_http_free(&res);

    if (!parsed) {
        fail("Key created, but the server did not return it. Check the API "
             "Keys page on the server.");
        return;
    }

    snprintf(config.token, sizeof config.token, "%s", key);
    memset(key, 0, sizeof key);
    memset(access_token, 0, sizeof access_token);

    if (!config_save()) {
        fail("Got a key but could not save it to the device.");
        return;
    }

    abs_log("stored new api key");
    fetch_libraries();
}

/*
 * Load one cover per tick, then repaint just that row.
 *
 * Fetch-and-decode is far too slow to do for a whole page inside a paint: the
 * list would sit blank for several seconds. Doing one per timer tick lets the
 * list appear immediately and fill in, and keeps taps responsive between
 * ticks.
 */
static void cover_tick_cb(void)
{
    if (current_screen != SCREEN_ITEMS) return;      /* user moved on */
    if (cover_load_index >= item_count) return;

    int i = cover_load_index++;
    ibitmap *bmp = abs_cover_get(&config, items[i].id, cover_w, cover_h, 1);

    if (bmp != NULL && i < MAX_ITEMS) {
        int y = item_row_y[i];
        FillArea(0, y, screen_w, row_h, WHITE);
        draw_item_row(i, y);
        PartialUpdate(0, y, screen_w, row_h);
    }

    if (cover_load_index < item_count) {
        SetWeakTimer("abs_covers", cover_tick_cb, 50);
    }
}

static void start_cover_loading(void)
{
    cover_load_index = 0;
    if (item_count > 0 && cover_w > 0) {
        SetWeakTimer("abs_covers", cover_tick_cb, 250);
    }
}

/*
 * Drive the transfer from a timer, repainting the progress bar as it moves.
 *
 * The pump itself is non-blocking (curl_multi), so the event loop keeps
 * running between ticks -- which is what makes Cancel work at all.
 */
static void dl_pump_cb(void)
{
    static int repaint_counter;

    int still_running = abs_dl_pump();
    const abs_dl_status *st = abs_dl_status_get();

    if (still_running) {
        /* Repaint about once a second: the bar has to move, but a full e-ink
         * refresh every 100ms would make the device unusable. */
        if (++repaint_counter >= 10) {
            repaint_counter = 0;
            if (current_screen == SCREEN_DOWNLOAD) draw_download_dynamic(1);
        }
        SetWeakTimer("abs_dl", dl_pump_cb, 100);
        return;
    }

    repaint_counter = 0;

    if (st->state == DL_DONE && dl_entry.item_id[0] != '\0') {
        dl_entry.size = st->done_bytes;
        abs_manifest_put(&manifest, &dl_entry);
        manifest_save();
        abs_log("recorded download: %s -> %s", dl_entry.item_id, dl_entry.dir);
    }

    if (current_screen == SCREEN_DOWNLOAD) draw_current_screen();
}

/*
 * Push listening progress to Audiobookshelf.
 *
 * We cannot observe playback as it happens -- the firmware terminates this app
 * when a book opens -- so this runs at launch and reports what the stock
 * player recorded since last time. It therefore also captures listening done
 * entirely outside this app, which live polling never could.
 */
static void sync_progress(void)
{
    static abs_pb_state states[ABS_MAX_PB_BOOKS];

    int n = abs_pbdb_read_states(states, ABS_MAX_PB_BOOKS);
    if (n <= 0) return;

    int pushed = 0, dirty = 0, checked = 0;

    for (int i = 0; i < n; i++) {
        const abs_download *entry =
            abs_manifest_find_by_path(&manifest, states[i].path);
        if (entry == NULL) continue;      /* not something we downloaded */
        checked++;

        /* Prefer the server's duration: it is authoritative, and the
         * firmware's is derived from the file. */
        double duration = (entry->duration > 0) ? entry->duration
                                                : states[i].duration;
        double pos = abs_sync_position_seconds(states[i].raw_loc, duration);

        if (!abs_sync_should_push(pos, entry->synced_pos)) continue;

        char url[ABS_MAX_URL + 96];
        char body[256];
        if (abs_url_progress(&config, entry->item_id, url, sizeof url) == 0) continue;
        if (abs_build_progress_body(pos, duration, body, sizeof body) == 0) continue;

        abs_http_response res;
        if (!abs_http_patch_json(&config, url, body, config.token, &res,
                                 64 * 1024)) {
            abs_log("sync: %s failed (%s)", entry->title, res.error);
            continue;
        }

        long st = res.status;
        abs_http_free(&res);

        if (st >= 400) {
            abs_log("sync: %s -> HTTP %ld", entry->title, st);
            continue;
        }

        abs_log("sync: %s at %.0fs (raw %.0f)", entry->title, pos,
                states[i].raw_loc);

        /* Record what we sent so the same position is not pushed again. */
        abs_download updated = *entry;
        updated.synced_pos = pos;
        abs_manifest_put(&manifest, &updated);
        dirty = 1;
        pushed++;
    }

    if (dirty) manifest_save();

    if (pushed > 0) {
        snprintf(status_message, sizeof status_message,
                 "Synced progress for %d book%s.", pushed, pushed == 1 ? "" : "s");
    } else if (checked > 0) {
        snprintf(status_message, sizeof status_message,
                 "Progress up to date (%d book%s checked).",
                 checked, checked == 1 ? "" : "s");
    }
}

static void sync_cb(void)
{
    sync_progress();

    /*
     * Repaint only the status line. A full redraw here meant two full e-ink
     * refreshes back to back on every launch -- the list, then this -- to
     * change one line of text.
     */
    if (current_screen == SCREEN_LIBRARIES && status_message[0]) {
        int y = header_h + row_h / 3;
        FillArea(0, y, screen_w, row_h / 2, WHITE);
        SetFont(font_hint, DGRAY);
        DrawTextRect(margin, y, screen_w - margin * 2, row_h / 2,
                     status_message, ALIGN_LEFT | DOTS);
        PartialUpdate(0, y, screen_w, row_h / 2);
    }
}

static void autofetch_cb(void)
{
    abs_log("autofetch timer fired");
    fetch_libraries();
}

/*
 * Requests are always kicked off from a timer, never straight from a tap.
 * See the EVT_SHOW comment: work inside a handler leaves the panel stale.
 */
static char pending_item_id[ABS_MAX_ID];

static void fetch_items_cb(void)
{
    fetch_items();
}

static void fetch_detail_cb(void)
{
    fetch_detail(pending_item_id);
}

static void defer_items(const char *busy_message)
{
    draw_busy(busy_message);
    SetWeakTimer("abs_items", fetch_items_cb, 200);
}

static void defer_detail(const char *item_id)
{
    snprintf(pending_item_id, sizeof pending_item_id, "%s", item_id);
    draw_busy("Loading book...");
    SetWeakTimer("abs_detail", fetch_detail_cb, 200);
}

/* --------------------------------------------------------------- keyboard -- */

/*
 * `text` is the very buffer that was handed to OpenKeyboard, so these
 * callbacks must not copy it onto itself or clear it. (Clearing it here is
 * what previously sent every login with an empty password.)
 */
static void do_sign_in_cb(void)
{
    do_sign_in();
}

static void search_cb(void)
{
    fetch_search();
}

static void search_entered(char *text)
{
    if (text == NULL || text[0] == '\0') return;   /* cancelled or empty */

    snprintf(search_query, sizeof search_query, "%s", search_input);
    draw_busy("Searching...");
    SetWeakTimer("abs_search", search_cb, 200);
}

static void server_entered(char *text)
{
    if (text == NULL) return;               /* cancelled */
    abs_config_normalize_server(form_server);
    draw_current_screen();
}

static void admin_user_entered(char *text)
{
    if (text == NULL) return;
    draw_current_screen();
}

static void admin_pass_entered(char *text)
{
    if (text == NULL) return;
    draw_current_screen();
}

static void target_user_entered(char *text)
{
    if (text == NULL) return;
    draw_current_screen();
}

/* ------------------------------------------------------------------ input -- */

/*
 * Walk one step up the hierarchy: detail -> list -> libraries -> exit.
 * Reached from both IV_KEY_BACK and the header chevron.
 */
static void go_back(void)
{
    switch (current_screen) {
    case SCREEN_DOWNLOAD:
        /* Leaving the screen does not stop the transfer; it keeps running on
         * its timer and the manifest is updated when it finishes. */
        current_screen = SCREEN_DETAIL;
        draw_current_screen();
        break;
    case SCREEN_DETAIL:
        current_screen = SCREEN_ITEMS;
        draw_current_screen();
        break;
    case SCREEN_ITEMS:
        current_screen = SCREEN_LIBRARIES;
        draw_current_screen();
        break;
    case SCREEN_SETUP:
        if (config.token[0] && library_count >= 0) {
            current_screen = SCREEN_LIBRARIES;
            draw_current_screen();
        } else {
            CloseApp();
        }
        break;
    case SCREEN_LIBRARIES:
    default:
        CloseApp();
        break;
    }
}

static void handle_action(int action)
{
    switch (action) {
    case ACT_EDIT_SERVER:
        OpenKeyboard("Server address", form_server, sizeof form_server - 1,
                     KBD_URL, server_entered);
        break;

    case ACT_EDIT_ADMIN_USER:
        OpenKeyboard("Admin username", form_admin_user,
                     sizeof form_admin_user - 1, KBD_NORMAL, admin_user_entered);
        break;

    case ACT_EDIT_ADMIN_PASS:
        OpenKeyboard("Admin password", form_admin_pass,
                     sizeof form_admin_pass - 1, KBD_PASSWORD, admin_pass_entered);
        break;

    case ACT_EDIT_TARGET_USER:
        OpenKeyboard("Create key for (blank = yourself)", form_target_user,
                     sizeof form_target_user - 1, KBD_NORMAL, target_user_entered);
        break;

    case ACT_SIGN_IN:
        /* Same rule as every other request: never from inside a handler. */
        draw_busy("Connecting to Wi-Fi...");
        SetWeakTimer("abs_signin", do_sign_in_cb, 200);
        break;

    case ACT_SHOW_LIBRARIES:
        current_screen = SCREEN_LIBRARIES;
        draw_current_screen();
        break;

    case ACT_OPEN_SETUP:
        current_screen = SCREEN_SETUP;
        draw_current_screen();
        break;

    case ACT_DOWNLOAD: {
        /* Already downloading? Show it rather than starting a second one on
         * top of the first. */
        if (abs_dl_status_get()->state == DL_RUNNING) {
            current_screen = SCREEN_DOWNLOAD;
            draw_current_screen();
            break;
        }

        char dir[ABS_MAX_DIR];
        if (abs_manifest_dir_for(detail.author, detail.title, dir, sizeof dir) == 0) {
            Message(ICON_WARNING, "Cannot download",
                    "That book's title is too long for a folder name.", 3000);
            break;
        }

        /* Capture the book now: `detail` may be a different one by the time
         * the transfer finishes. */
        memset(&dl_entry, 0, sizeof dl_entry);
        snprintf(dl_entry.item_id, sizeof dl_entry.item_id, "%s", detail.id);
        snprintf(dl_entry.dir, sizeof dl_entry.dir, "%s", dir);
        snprintf(dl_entry.title, sizeof dl_entry.title, "%s", detail.title);
        snprintf(dl_entry.author, sizeof dl_entry.author, "%s", detail.author);
        dl_entry.duration = detail.duration;
        dl_entry.track_count = detail.track_count;

        if (abs_dl_start(&config, &detail, dir)) {
            SetWeakTimer("abs_dl", dl_pump_cb, 100);
        }
        current_screen = SCREEN_DOWNLOAD;
        draw_current_screen();
        break;
    }

    case ACT_CANCEL_DL:
        abs_dl_cancel();
        ClearTimerByName("abs_dl");
        draw_current_screen();
        break;

    case ACT_PLAY: {
        /*
         * Handing a file to the firmware player terminates this app -- that is
         * how the platform works, not a failure. Save first so relaunching
         * comes back here, and let the player own the screen.
         */
        const abs_download *have = abs_manifest_find(&manifest, detail.id);
        if (have == NULL) break;

        if (detail.track_count == 0) {
            Message(ICON_WARNING, "Nothing to play",
                    "This book has no audio files.", 3000);
            break;
        }

        char safe_name[ABS_MAX_NAME];
        char path[ABS_MAX_DIR + ABS_MAX_NAME + 8];
        abs_sanitize_component(detail.tracks[0].filename, safe_name, sizeof safe_name);
        snprintf(path, sizeof path, "%s/%s", have->dir, safe_name);

        /*
         * OpenBook routes through the firmware's file-handler association, so
         * an .m4b lands in the audiobook app -- at that book, with its
         * chapters and its own saved position. PlayFile goes to the music
         * player instead, which just dropped us on the home screen.
         *
         * Either way this app is terminated, so save first.
         */
        const char *handler = GetFileHandler(path);
        abs_log("handing off: %s (handler: %s)", path,
                handler ? handler : "(none)");

        save_state();
        manifest_save();

        int rc = OpenBook(path, NULL, 0);
        abs_log("OpenBook -> %d", rc);

        if (rc <= 0) {
            /* Fall back rather than leaving the user with a dead button. */
            abs_log("OpenBook failed, falling back to PlayFile");
            PlayFile(path);
        }
        break;
    }

    case ACT_DELETE: {
        const abs_download *have = abs_manifest_find(&manifest, detail.id);
        if (have == NULL) break;
        abs_log("forgetting download %s", have->item_id);
        /* Only the record is dropped here; the audio stays on the device for
         * the stock player, and the user can delete it from the library. */
        abs_manifest_remove(&manifest, detail.id);
        manifest_save();
        draw_current_screen();
        break;
    }

    case ACT_SEARCH:
        search_input[0] = '\0';
        OpenKeyboard("Search this library", search_input,
                     sizeof search_input - 1, KBD_NORMAL, search_entered);
        break;

    case ACT_CLEAR_SEARCH:
        search_query[0] = '\0';
        item_page = 0;
        defer_items("Loading books...");
        break;

    case ACT_PREV_PAGE:
        if (item_page > 0) {
            item_page--;
            defer_items("Loading...");
        }
        break;

    case ACT_NEXT_PAGE:
        item_page++;
        defer_items("Loading...");
        break;

    case ACT_BACK_TO_ITEMS:
        current_screen = SCREEN_DETAIL;
        draw_current_screen();
        break;

    case ACT_GO_BACK:
        go_back();
        break;

    case ACT_SCROLL_DOWN:
        if (current_screen == SCREEN_DETAIL) {
            int len = (int)strlen(detail.description);
            if (detail_scroll + DESC_PAGE < len) {
                detail_scroll += DESC_PAGE;
                draw_current_screen();
            }
        }
        break;

    case ACT_SCROLL_UP:
        if (current_screen == SCREEN_DETAIL && detail_scroll > 0) {
            detail_scroll = (detail_scroll > DESC_PAGE) ? detail_scroll - DESC_PAGE : 0;
            draw_current_screen();
        }
        break;

    default:
        if (action >= ACT_PICK_ITEM_BASE) {
            int i = action - ACT_PICK_ITEM_BASE;
            if (i >= 0 && i < item_count) defer_detail(items[i].id);
        } else if (action >= ACT_PICK_LIBRARY_BASE) {
            int i = action - ACT_PICK_LIBRARY_BASE;
            if (i >= 0 && i < library_count) {
                snprintf(current_library_id, sizeof current_library_id, "%s",
                         libraries[i].id);
                snprintf(current_library_name, sizeof current_library_name, "%s",
                         libraries[i].name);
                item_page = 0;
                search_query[0] = '\0';
                defer_items("Loading books...");
            }
        }
        break;
    }
}

static void handle_tap(int x, int y)
{
    for (int i = 0; i < row_count; i++) {
        if (x >= rows[i].x && x < rows[i].x + rows[i].w &&
            y >= rows[i].y && y < rows[i].y + rows[i].h) {
            handle_action(rows[i].action);
            return;
        }
    }
}

/* ------------------------------------------------------------------- init -- */

static void open_fonts(void)
{
    font_title = OpenFont("default", screen_h / 30, 1);
    font_body  = OpenFont("default", screen_h / 40, 1);
    font_hint  = OpenFont("default", screen_h / 52, 1);
}

static void close_fonts(void)
{
    if (font_title) { CloseFont(font_title); font_title = NULL; }
    if (font_body)  { CloseFont(font_body);  font_body  = NULL; }
    if (font_hint)  { CloseFont(font_hint);  font_hint  = NULL; }
}

/* Keys we act on, so we can also swallow their release events. */
static int is_navigation_key(int key)
{
    return key == IV_KEY_BACK || key == IV_KEY_HOME || key == IV_KEY_MENU ||
           key == IV_KEY_PREV || key == IV_KEY_PREV2 ||
           key == IV_KEY_NEXT || key == IV_KEY_NEXT2;
}

static int main_handler(int type, int par1, int par2)
{
    /*
     * Log every event while diagnostics are on. A key that closes the app
     * without ever reaching EVT_KEYPRESS is invisible otherwise -- which is
     * exactly the situation this is here to resolve.
     */
    if (abs_log_enabled() && type != EVT_POINTERMOVE) {
        abs_log("evt %d par1=0x%02x par2=%d screen=%d", type, par1, par2,
                current_screen);
    }

    switch (type) {
    case EVT_INIT:
        screen_w = ScreenWidth();
        screen_h = ScreenHeight();
        margin   = screen_w / 20;
        row_h    = screen_h / 15;
        header_h = screen_h / 12;

        /* Fill the space between header and pager, leaving room for both.
         * Must be non-zero: the server treats limit=0 as "no limit" and would
         * return the entire library in one response. */
        items_per_page = (screen_h - header_h - row_h * 3) / row_h;
        if (items_per_page < 4) items_per_page = 4;
        if (items_per_page > MAX_ITEMS) items_per_page = MAX_ITEMS;

        /* Cover thumbnails sit inside a list row, at book proportions.
         * These were previously left at zero, which disabled list covers and
         * the whole progressive-loading path along with them. */
        cover_h = row_h - row_h / 8;
        cover_w = cover_h * 2 / 3;

        {
            unsigned long t = abs_now_ms();
            open_fonts();
            abs_log_init();
            abs_log("fonts opened in %lums", abs_now_ms() - t);

            t = abs_now_ms();
            abs_log("start: %dx%d, %s", screen_w, screen_h, GetDeviceModel());
            config_load();
            abs_covers_init();
            load_state();
            manifest_load();
            abs_log("config loaded in %lums", abs_now_ms() - t);
        }

        current_screen = (abs_config_validate(&config) == ABS_CONFIG_OK)
            ? SCREEN_LIBRARIES : SCREEN_SETUP;

        /* Only worth restoring if we got past setup last time. */
        restoring = (current_screen == SCREEN_LIBRARIES &&
                     restore.library_id[0] != '\0');
        break;

    case EVT_SHOW: {
        unsigned long t = abs_now_ms();
        int autofetch = (current_screen == SCREEN_LIBRARIES && library_count < 0);

        /* Paint, but never work, inside EVT_SHOW.
         *
         * inkview has its own show sequence to finish after this handler
         * returns. Blocking here (NetConnect, HTTP) stops the panel ever
         * flushing what we drew -- the screen sits stale until some later
         * event pumps the loop, which looks exactly like a long hang that
         * "clears when you press a button". So we paint a status, return
         * immediately, and do the real work from a timer on the next turn
         * of the event loop. */
        if (autofetch) {
            draw_busy("Connecting to Wi-Fi...");
        } else {
            draw_current_screen();
        }
        abs_log("first paint in %lums (screen %d)", abs_now_ms() - t, current_screen);

        if (autofetch) {
            SetWeakTimer("abs_autofetch", autofetch_cb, 200);
        }
        break;
    }

    case EVT_POINTERUP:
        handle_tap(par1, par2);
        return 1;

    /*
     * Claim the release of any key we acted on. Handling only the press
     * leaves the release to fall through to inkview's default, and for the
     * back/home keys that default is "leave the application" -- which looked
     * like our own navigation being ignored.
     */
    case EVT_KEYRELEASE:
    case EVT_KEYREPEAT:
        if (is_navigation_key(par1)) return 1;
        break;

    case EVT_KEYPRESS:
        abs_log("key 0x%02x on screen %d", par1, current_screen);

        /*
         * Treat Back and Home alike: the Verse Pro has few physical keys, and
         * an unhandled key falls through to inkview's default -- which for
         * Home is "leave the app". Returning 1 below says we dealt with it so
         * the framework does not also act on it.
         */
        if (par1 == IV_KEY_BACK || par1 == IV_KEY_HOME) {
            go_back();
            return 1;
        } else if (par1 == IV_KEY_NEXT || par1 == IV_KEY_NEXT2) {
            if (current_screen == SCREEN_ITEMS) {
                handle_action(ACT_NEXT_PAGE);
            } else if (current_screen == SCREEN_DETAIL) {
                handle_action(ACT_SCROLL_DOWN);
            }
            return 1;
        } else if (par1 == IV_KEY_PREV || par1 == IV_KEY_PREV2) {
            if (current_screen == SCREEN_ITEMS) {
                handle_action(ACT_PREV_PAGE);
            } else if (current_screen == SCREEN_DETAIL) {
                handle_action(ACT_SCROLL_UP);
            }
            return 1;
        } else if (par1 == IV_KEY_MENU) {
            current_screen = (current_screen == SCREEN_SETUP)
                ? SCREEN_LIBRARIES : SCREEN_SETUP;
            draw_current_screen();
            return 1;
        }
        break;

    /*
     * The Home key is handled entirely by the firmware: it backgrounds this
     * app and terminates it, and the key never reaches us. Saving here is what
     * makes that lossless.
     */
    case EVT_BACKGROUND:
        save_state();
        break;

    case EVT_EXIT:
        save_state();
        wipe_password();
        abs_covers_free_memory();
        close_fonts();
        break;

    default:
        break;
    }

    return 0;
}

int main(void)
{
    InkViewMain(main_handler);
    return 0;
}
