/*
 * Playback spike -- throwaway hardware probe, not part of ABSClient.
 *
 * Design rules learned the hard way:
 *   - Never FullUpdate on a timer. A full e-ink refresh takes ~1s, so periodic
 *     refreshes leave the panel permanently busy and taps appear to be
 *     ignored. Paint once; use PartialUpdate for small changes.
 *   - Give immediate visual feedback on tap, or the user cannot tell which
 *     action (if any) they triggered.
 *   - Never call the player getters unless a player exists: they crash
 *     otherwise, which is itself finding S0.
 */
#include <inkview.h>

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LOG_PATH  "/mnt/ext1/applications/ABSClient/spike.log"
#define M4B_PATH  "/mnt/ext1/Audio Books/Spike Test/The Message.m4b"
#define MULTI_DIR "/mnt/ext1/Audio Books/Spike Multi"

#define MAX_LINES 10

static ifont *font_btn, *font_log;
static int sw, sh, row, log_top;

static char lines[MAX_LINES][128];
static int  line_count;

/* Set once any player exists; every getter is gated on this. */
static int player_exists;

enum { A_PLAY_BARE = 1, A_PROBE, A_OPEN_PLAY, A_PLAYLIST, A_SEEK, A_TOGGLE };

static struct { int x, y, w, h, act; const char *label; } btns[6];
static int btn_count;

/* ------------------------------------------------------------------- log -- */

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void draw_log(void);

static void logf_(const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    FILE *f = fopen(LOG_PATH, "a");
    if (f != NULL) {
        time_t now = time(NULL);
        struct tm tm_buf;
        char stamp[16] = "";
        if (localtime_r(&now, &tm_buf) != NULL) {
            strftime(stamp, sizeof stamp, "%H:%M:%S", &tm_buf);
        }
        fprintf(f, "[%s] %s\n", stamp, buf);
        fclose(f);
    }

    if (line_count == MAX_LINES) {
        memmove(lines[0], lines[1], sizeof lines[0] * (MAX_LINES - 1));
        line_count--;
    }
    snprintf(lines[line_count++], sizeof lines[0], "%s", buf);
}

/* ------------------------------------------------------------------ draw -- */

/* Only the transcript area, so this stays fast. */
static void draw_log(void)
{
    FillArea(0, log_top, sw, sh - log_top, WHITE);

    SetFont(font_log, BLACK);
    int y = log_top + 8;
    for (int i = 0; i < line_count; i++) {
        DrawTextRect(16, y, sw - 32, row / 2, lines[i], ALIGN_LEFT | DOTS);
        y += row / 2;
    }
    PartialUpdate(0, log_top, sw, sh - log_top);
}

static void add_btn(int x, int y, int w, int h, const char *label, int act)
{
    if (btn_count >= 6) return;
    btns[btn_count].x = x; btns[btn_count].y = y;
    btns[btn_count].w = w; btns[btn_count].h = h;
    btns[btn_count].label = label; btns[btn_count].act = act;
    btn_count++;
}

static void paint_btn(int i, int inverted)
{
    int x = btns[i].x, y = btns[i].y, w = btns[i].w, h = btns[i].h;

    FillArea(x, y, w, h, inverted ? BLACK : WHITE);
    DrawRectRound(x, y, w, h, h / 5, BLACK);
    SetFont(font_btn, inverted ? WHITE : BLACK);
    DrawTextRect(x, y + h / 4, w, h / 2, btns[i].label, ALIGN_CENTER);
}

static void draw_all(void)
{
    ClearScreen();
    btn_count = 0;

    int m = 16;
    int bw = (sw - m * 3) / 2;
    int bh = row;
    int y = m;

    add_btn(m,             y, bw, bh, "1 PlayFile bare", A_PLAY_BARE);
    add_btn(m * 2 + bw,    y, bw, bh, "2 Probe",         A_PROBE);
    y += bh + m / 2;
    add_btn(m,             y, bw, bh, "3 Open+Play",     A_OPEN_PLAY);
    add_btn(m * 2 + bw,    y, bw, bh, "4 Playlist",      A_PLAYLIST);
    y += bh + m / 2;
    add_btn(m,             y, bw, bh, "5 Seek +120",     A_SEEK);
    add_btn(m * 2 + bw,    y, bw, bh, "6 Play/Pause",    A_TOGGLE);
    y += bh + m;

    for (int i = 0; i < btn_count; i++) paint_btn(i, 0);

    DrawLine(0, y, sw, y, DGRAY);
    log_top = y + 1;

    SetFont(font_log, BLACK);
    int ly = log_top + 8;
    for (int i = 0; i < line_count; i++) {
        DrawTextRect(16, ly, sw - 32, row / 2, lines[i], ALIGN_LEFT | DOTS);
        ly += row / 2;
    }

    FullUpdate();          /* the only full refresh in the app */
}

/* ---------------------------------------------------------------- actions -- */

static void probe_getters(const char *why)
{
    logf_("probe(%s):", why);
    int v = IsPlayingMP3();
    logf_("  IsPlayingMP3=%d", v);
    v = GetPlayerState();
    logf_("  state=%d", v);
    v = GetTrackPosition();
    logf_("  pos=%d", v);
    v = GetTrackSize();
    logf_("  size=%d", v);
    v = GetCurrentTrack();
    logf_("  track=%d (all ok)", v);
    player_exists = 1;
}

static void probe_cb(void)
{
    probe_getters("2s after bare PlayFile");
    draw_log();
}

static char *playlist[8];
static int playlist_len;

static void build_playlist(void)
{
    DIR *d = opendir(MULTI_DIR);
    if (d == NULL) { logf_("cannot open Spike Multi"); return; }

    struct dirent *de;
    while ((de = readdir(d)) != NULL && playlist_len < 7) {
        if (de->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof path, "%s/%s", MULTI_DIR, de->d_name);
        playlist[playlist_len++] = strdup(path);
    }
    closedir(d);
    playlist[playlist_len] = NULL;
    logf_("playlist: %d tracks", playlist_len);
}

static void act(int a)
{
    switch (a) {
    case A_PLAY_BARE:
        logf_("PlayFile, no OpenPlayer");
        PlayFile(M4B_PATH);
        logf_("survived PlayFile");
        player_exists = 1;              /* something should exist now */
        SetWeakTimer("spike_probe", probe_cb, 2000);
        break;

    case A_PROBE:
        /* Safe to run on a cold launch only if a player is already running
         * from a previous session -- that is exactly what we are testing. */
        probe_getters("cold");
        break;

    case A_OPEN_PLAY:
        logf_("OpenPlayer + PlayFile (expect to be killed)");
        OpenPlayer();
        player_exists = 1;
        PlayFile(M4B_PATH);
        break;

    case A_PLAYLIST:
        build_playlist();
        if (playlist_len > 0) {
            logf_("LoadPlaylist + PlayTrack(0)");
            OpenPlayer();
            player_exists = 1;
            LoadPlaylist(playlist);
            PlayTrack(0);
        }
        break;

    case A_SEEK:
        if (!player_exists) { logf_("no player yet -- press 1 or 3 first"); break; }
        {
            int before = GetTrackPosition();
            SetTrackPosition(before + 120);
            logf_("seek %d -> %d", before, GetTrackPosition());
        }
        break;

    case A_TOGGLE:
        if (!player_exists) { logf_("no player yet -- press 1 or 3 first"); break; }
        logf_("toggle (state %d)", GetPlayerState());
        TogglePlaying();
        break;
    }
}

/* ----------------------------------------------------------------- input -- */

static void tap(int x, int y)
{
    for (int i = 0; i < btn_count; i++) {
        if (x >= btns[i].x && x < btns[i].x + btns[i].w &&
            y >= btns[i].y && y < btns[i].y + btns[i].h) {

            /* Show the press immediately -- a partial update is fast enough
             * to land before the action runs. */
            paint_btn(i, 1);
            PartialUpdate(btns[i].x, btns[i].y, btns[i].w, btns[i].h);

            logf_("--- tapped %s ---", btns[i].label);
            act(btns[i].act);

            paint_btn(i, 0);
            PartialUpdate(btns[i].x, btns[i].y, btns[i].w, btns[i].h);
            draw_log();
            return;
        }
    }
}

static int handler(int type, int par1, int par2)
{
    switch (type) {
    case EVT_INIT:
        sw = ScreenWidth();
        sh = ScreenHeight();
        row = sh / 16;
        font_btn = OpenFont("default", sh / 44, 1);
        font_log = OpenFont("default", sh / 56, 1);
        logf_("=== spike start ===");
        break;

    case EVT_SHOW:
        draw_all();
        break;

    case EVT_POINTERUP:
        tap(par1, par2);
        return 1;

    case EVT_MP_STATECHANGED:
        logf_("MP_STATECHANGED par1=%d", par1);
        draw_log();
        return 1;

    case EVT_MP_TRACKCHANGED:
        logf_("MP_TRACKCHANGED par1=%d", par1);
        draw_log();
        return 1;

    case EVT_BACKGROUND:
        logf_("BACKGROUND (screen taken)");
        break;

    case EVT_FOREGROUND:
        logf_("FOREGROUND (we are back)");
        break;

    case EVT_EXIT:
        logf_("EXIT");
        ClearTimerByName("spike_probe");
        if (font_btn) CloseFont(font_btn);
        if (font_log) CloseFont(font_log);
        break;

    case EVT_KEYPRESS:
        if (par1 == IV_KEY_BACK || par1 == IV_KEY_HOME) { CloseApp(); return 1; }
        break;
    }
    return 0;
}

int main(void)
{
    InkViewMain(handler);
    return 0;
}
