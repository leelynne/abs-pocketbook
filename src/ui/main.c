/*
 * M0: device-info screen.
 *
 * Proves the toolchain, the event loop, fonts and drawing -- and prints the
 * device facts the later milestones need (screen geometry drives all UI
 * scaling; model and firmware decide which SDK quirks apply).
 */
#include <inkview.h>

#include <stdio.h>

#include "core/paths.h"
#include "core/version.h"

static ifont *font_title;
static ifont *font_body;
static ifont *font_hint;

static int screen_w;
static int screen_h;

static const char *safe(const char *s)
{
    return (s != NULL && *s != '\0') ? s : "(unknown)";
}

static void open_fonts(void)
{
    /* Scale off screen height so this is not Verse Pro-specific: the Verse Pro
     * is 1072x1448, but the same binary should stay legible elsewhere. */
    int title_size = screen_h / 26;
    int body_size  = screen_h / 40;
    int hint_size  = screen_h / 52;

    font_title = OpenFont("default", title_size, 1);
    font_body  = OpenFont("default", body_size, 1);
    font_hint  = OpenFont("default", hint_size, 1);
}

static void close_fonts(void)
{
    if (font_title) { CloseFont(font_title); font_title = NULL; }
    if (font_body)  { CloseFont(font_body);  font_body  = NULL; }
    if (font_hint)  { CloseFont(font_hint);  font_hint  = NULL; }
}

/* Draws "label: value" and returns the y for the next row. */
static int draw_row(int y, int margin, const char *label, const char *value)
{
    int row_h = screen_h / 28;
    char buf[256];

    snprintf(buf, sizeof buf, "%s: %s", label, value);
    SetFont(font_body, BLACK);
    DrawTextRect(margin, y, screen_w - (margin * 2), row_h, buf, ALIGN_LEFT);
    return y + row_h;
}

static int draw_row_int(int y, int margin, const char *label, int value)
{
    char buf[32];
    snprintf(buf, sizeof buf, "%d", value);
    return draw_row(y, margin, label, buf);
}

static void draw_screen(void)
{
    int margin = screen_w / 16;
    int y;
    char buf[128];

    ClearScreen();

    y = screen_h / 12;
    SetFont(font_title, BLACK);
    DrawTextRect(0, y, screen_w, screen_h / 20, "Audiobookshelf", ALIGN_CENTER);
    y += screen_h / 20;

    SetFont(font_hint, DGRAY);
    DrawTextRect(0, y, screen_w, screen_h / 34, abs_version_string(), ALIGN_CENTER);
    y += screen_h / 15;

    DrawLine(margin, y, screen_w - margin, y, LGRAY);
    y += screen_h / 40;

    y = draw_row(y, margin, "Model",    safe(GetDeviceModel()));
    y = draw_row(y, margin, "Hardware", safe(GetHardwareType()));
    y = draw_row(y, margin, "Firmware", safe(GetSoftwareVersion()));

    snprintf(buf, sizeof buf, "%d x %d", screen_w, screen_h);
    y = draw_row(y, margin, "Screen", buf);

    y = draw_row_int(y, margin, "Battery", GetBatteryPower());

    y += screen_h / 40;
    DrawLine(margin, y, screen_w - margin, y, LGRAY);
    y += screen_h / 40;

    y = draw_row(y, margin, "State dir", ABS_APP_DIR);
    y = draw_row(y, margin, "Audio dir", ABS_AUDIO_DIR);

    SetFont(font_hint, DGRAY);
    DrawTextRect(0, screen_h - (screen_h / 12), screen_w, screen_h / 34,
                 "Tap or press Back to exit", ALIGN_CENTER);

    FullUpdate();
}

static int main_handler(int type, int par1, int par2)
{
    (void)par2;

    switch (type) {
    case EVT_INIT:
        screen_w = ScreenWidth();
        screen_h = ScreenHeight();
        open_fonts();
        break;

    case EVT_SHOW:
        draw_screen();
        break;

    case EVT_KEYPRESS:
        if (par1 == IV_KEY_BACK || par1 == IV_KEY_HOME || par1 == IV_KEY_OK) {
            CloseApp();
        }
        break;

    case EVT_POINTERUP:
        CloseApp();
        break;

    case EVT_EXIT:
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
