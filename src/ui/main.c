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
#include "core/version.h"
#include "ui/log.h"
#include "ui/net.h"

#define MAX_LIBRARIES 32
#define MAX_USERS     128
#define RESPONSE_CAP  (2 * 1024 * 1024)

typedef enum {
    SCREEN_SETUP,
    SCREEN_LIBRARIES
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
       ACT_EDIT_TARGET_USER, ACT_SIGN_IN, ACT_OPEN_SETUP, ACT_SHOW_LIBRARIES };

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

static void draw_current_screen(void);
static void fetch_libraries(void);
static void autofetch_cb(void);

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

static void draw_header(const char *title)
{
    SetFont(font_title, BLACK);
    DrawTextRect(margin, header_h / 4, screen_w - margin * 2, header_h / 2,
                 title, ALIGN_LEFT | DOTS);
    DrawLine(0, header_h, screen_w, header_h, DGRAY);
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

static void draw_current_screen(void)
{
    if (current_screen == SCREEN_SETUP) draw_setup_screen();
    else                                draw_libraries_screen();
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

    current_screen = SCREEN_LIBRARIES;
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

static void autofetch_cb(void)
{
    abs_log("autofetch timer fired");
    fetch_libraries();
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

    default:
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

static int main_handler(int type, int par1, int par2)
{
    switch (type) {
    case EVT_INIT:
        screen_w = ScreenWidth();
        screen_h = ScreenHeight();
        margin   = screen_w / 20;
        row_h    = screen_h / 15;
        header_h = screen_h / 12;
        {
            unsigned long t = abs_now_ms();
            open_fonts();
            abs_log_init();
            abs_log("fonts opened in %lums", abs_now_ms() - t);

            t = abs_now_ms();
            abs_log("start: %dx%d, %s", screen_w, screen_h, GetDeviceModel());
            config_load();
            abs_log("config loaded in %lums", abs_now_ms() - t);
        }

        current_screen = (abs_config_validate(&config) == ABS_CONFIG_OK)
            ? SCREEN_LIBRARIES : SCREEN_SETUP;
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
        break;

    case EVT_KEYPRESS:
        if (par1 == IV_KEY_BACK) {
            if (current_screen == SCREEN_SETUP && config.token[0] && library_count >= 0) {
                current_screen = SCREEN_LIBRARIES;
                draw_current_screen();
            } else {
                CloseApp();
            }
        } else if (par1 == IV_KEY_MENU) {
            current_screen = (current_screen == SCREEN_SETUP)
                ? SCREEN_LIBRARIES : SCREEN_SETUP;
            draw_current_screen();
        }
        break;

    case EVT_EXIT:
        wipe_password();
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
