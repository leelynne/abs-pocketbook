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
#include "core/state.h"
#include "core/version.h"
#include "ui/covers.h"
#include "ui/log.h"
#include "ui/net.h"

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
    SCREEN_DETAIL
} screen_id;

typedef struct {
    int y, h, action;
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

/* Cover geometry, derived from row height at init. */
static int cover_w, cover_h;
static int item_row_y[MAX_ITEMS];   /* for repainting a single row */
static int cover_load_index;        /* progressive load cursor */

/* Position restored from the previous run, consumed as we navigate back to it. */
static abs_state restore;
static int restoring;

static void draw_current_screen(void);
static void fetch_libraries(void);
static void autofetch_cb(void);
static void fetch_items(void);
static void fetch_detail(const char *item_id);
static void cover_tick_cb(void);
static void start_cover_loading(void);
static void save_state(void);

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

static void add_row(int y, int h, int action)
{
    if (row_count < MAX_ROWS) {
        rows[row_count].y = y;
        rows[row_count].h = h;
        rows[row_count].action = action;
        row_count++;
    }
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

static int draw_button(int y, const char *text, int action)
{
    int bw = screen_w - margin * 2;

    DrawRectRound(margin, y, bw, row_h, row_h / 4, BLACK);
    SetFont(font_body, BLACK);
    DrawTextRect(margin, y + row_h / 4, bw, row_h / 2, text, ALIGN_CENTER);

    add_row(y, row_h, action);
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

    draw_header_ex(current_library_name[0] ? current_library_name : "Books", 1);

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
    int half = (screen_w - margin * 2) / 2;

    if (item_page > 0) {
        DrawRectRound(margin, py, half - margin / 2, row_h, row_h / 4, BLACK);
        SetFont(font_body, BLACK);
        DrawTextRect(margin, py + row_h / 4, half - margin / 2, row_h / 2,
                     "< Prev", ALIGN_CENTER);
        add_row(py, row_h, ACT_PREV_PAGE);
    }
    if (item_page + 1 < pages) {
        DrawRectRound(margin + half + margin / 2, py, half - margin / 2, row_h,
                      row_h / 4, BLACK);
        SetFont(font_body, BLACK);
        DrawTextRect(margin + half + margin / 2, py + row_h / 4, half - margin / 2,
                     row_h / 2, "Next >", ALIGN_CENTER);
        add_row(py, row_h, ACT_NEXT_PAGE);
    }

    snprintf(counter, sizeof counter, "Page %d of %d  -  %d books  -  %s",
             item_page + 1, pages, item_total, abs_version_string());
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

    DrawLine(margin, y, screen_w - margin, y, LGRAY);
    y += row_h / 4;

    /* Description fills whatever is left, scrolled with the page buttons. */
    int desc_top = y;
    int desc_h = screen_h - row_h * 2 - desc_top;
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

    draw_footer("Tap lower half to scroll  |  < to go back");
    FullUpdate();
}

static void draw_current_screen(void)
{
    switch (current_screen) {
    case SCREEN_SETUP:     draw_setup_screen();     break;
    case SCREEN_LIBRARIES: draw_libraries_screen(); break;
    case SCREEN_ITEMS:     draw_items_screen();     break;
    case SCREEN_DETAIL:    draw_detail_screen();    break;
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
    current_screen = SCREEN_DETAIL;
    draw_current_screen();

    /* Pull the large cover, then repaint once it is in the cache. */
    int big_w = screen_w / 3;
    if (abs_cover_get(&config, detail.id, big_w, big_w * 3 / 2, 1) != NULL &&
        current_screen == SCREEN_DETAIL) {
        draw_current_screen();
    }
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

    draw_busy("Connecting to Wi-Fi...");

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
        do_sign_in();
        break;

    case ACT_SHOW_LIBRARIES:
        current_screen = SCREEN_LIBRARIES;
        draw_current_screen();
        break;

    case ACT_OPEN_SETUP:
        current_screen = SCREEN_SETUP;
        draw_current_screen();
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
        current_screen = SCREEN_ITEMS;
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
                defer_items("Loading books...");
            }
        }
        break;
    }
}

static void handle_tap(int y)
{
    for (int i = 0; i < row_count; i++) {
        if (y >= rows[i].y && y < rows[i].y + rows[i].h) {
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
        handle_tap(par2);
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
