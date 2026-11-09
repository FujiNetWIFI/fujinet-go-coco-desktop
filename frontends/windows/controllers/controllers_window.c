/*
 * The Win32 Controllers window: the Color Computer's two joysticks and its
 * RESET button drawn as a player sees them -- Tandy's cream joystick bases
 * with the round gate, the black stick and the red fire button(s), the LEFT
 * joystick on the left and the RIGHT one on the right -- from the session's
 * layout (cocosession_controls_layout), so every frontend shows the same
 * machine. Under them, chosen by the Keyboard button, the machine's own
 * keyboard (cocosession_keyboard_layout: 53 keys on a CoCo 2, 57 on a CoCo 3).
 * Then the gamepads with their assignments, and Map mode.
 *
 * Mouse: a press on the gate or its ring follows the pointer as the analog
 * stick (cocosession_stick_from_vector, a square range: a corner is a full
 * diagonal), the fire buttons are held while pressed, and a right or middle
 * press anywhere on a joystick holds its button 1 or 2. Hold stick keeps the
 * stick where it was left (the real Tandy sticks do not centre). A key is held
 * while pressed; SHIFT, CTRL and ALT latch until the next ordinary key lets go.
 * Presses are the raw button-down and button-up, never a click: the machine
 * samples its inputs once a frame, so a press present only for the instant of
 * a click would fall between frames. The mouse is captured on press; letting
 * go anywhere, losing the capture, hiding or deactivating the window releases
 * whatever the pointer holds.
 *
 * What the machine sees held -- keyboard, gamepads and this window together
 * (cocosession_joystick_held, buttons_held, keyboard_held) -- is polled every
 * 40 ms and painted in the accent, so the window doubles as a gamepad and
 * keyboard tester.
 *
 * FIXED SIZE: laid out once for the joysticks (and the keyboard, when it is
 * on), three gamepad rows and the strip, in a caption-only tool window with no
 * thick frame and no maximize box, so neither Map mode nor a long gamepad name
 * changes its size. Only the Keyboard button and a machine change resize it.
 *
 * MAP MODE: Map arms a two-step rebind -- click a control to pick the target
 * (Stop in Debugger below the joysticks too), then press a host key OR a
 * gamepad button (Esc cancels). While armed the controls show what drives
 * them, the controls not picked are dimmed, typing does not reach the machine,
 * and rebinding steals the key from whatever held it. Every key belongs to the
 * CoCo, so none is bound to begin with. "Show bindings" shows the same legends
 * outside Map mode. The keyboard's keys are the machine's: Map does not apply.
 *
 * The art is drawn at twice the size into a memory bitmap and shrunk with
 * HALFTONE (GDI has no anti-aliasing of its own); the text goes on after at
 * the window's own size.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include "controllers_window.h"

/* GET_X_LPARAM / GET_Y_LPARAM live here, not in windows.h. */
#include <windowsx.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../help/help_window.h"
#include "../key_identity.h"

#define PANEL_CLASS   "FujiNetGoCocoControllers"
#define PANEL_STYLE   (WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN)
#define PANEL_EXSTYLE WS_EX_TOOLWINDOW

/* Sizes at 96 dpi; scl() scales them to the window's. */
#define JOY_UNIT     34     /* pixels per joystick layout unit (a fire button) */
#define KEY_UNIT     26     /* pixels per key unit (a key; the pitch is 1.1) */
#define MARGIN       12
#define CASE_PAD     14
#define CASE_WIDE    483    /* the keyboard's 17.5 key units and a padding either side */
#define HEAD_HIGH    22
#define ROW_HIGH     26     /* one gamepad's row */
#define PAD_ROWS      3     /* gamepad rows the window has room for */
#define SEG_WIDE     66     /* Automatic / Right / Left */
#define STATE_WIDE   96     /* "drives right" */
#define BTN_HIGH     28
#define BTN_GAP       6
#define MAX_CAPS     64

#define KNOB_TRAVEL  1.25   /* layout units: the gate's radius less the knob's */
#define KNOB_RADIUS  0.55
#define PUSH_STICK   150    /* milli-units: past the dead zone, the knob is lit */
#define PUSH_SECTOR  700    /* a sector lights when pushed this far its way */

#define IDT_CONTROLLERS_CAPTURE 1
#define IDT_CONTROLLERS_HELD    2
#define IDT_CONTROLLERS_PADS    3

#define PI_VALUE 3.14159265358979323846

/* The strip's buttons (everything below the machine that is pressed). */
enum {
    SBT_MAP, SBT_CLEAR, SBT_DEFAULTS, SBT_SHOW, SBT_HELP,
    SBT_KEYBOARD, SBT_HOLD, SBT_STOP,
    SBT_COUNT
};

enum { HOLD_NONE, HOLD_STICK, HOLD_PRESS, HOLD_CAP, HOLD_ACTION };

typedef struct {
    unsigned generation;
    int count, assign[PAD_ROWS], effective[PAD_ROWS];
} pads_state;

typedef struct {
    int stick[COCOSESSION_JOYSTICK_PORTS][2];
    unsigned buttons[COCOSESSION_JOYSTICK_PORTS];
    int reset;
    unsigned char caps[MAX_CAPS];
} held_state;

typedef struct {
    unsigned casetop, casebot, casehigh, caseedge, strip, striptext;
    unsigned keycap, keyside, keytext, keyshift, keyspecial, keyfunc, ink;
    const char *title;
} palette;

static const palette k_coco3 = {
    0xB4B5B8, 0x8E8F93, 0xD9DADC, 0x5E5F63, 0x26272A, 0xC9CACC,
    0x2E2F33, 0x1F2023, 0xECECEC, 0x9FA0A5, 0x3E3F45, 0x55565C, 0x232326,
    "COLOR COMPUTER 3"
};
static const palette k_coco2 = {
    0xE6E1D3, 0xCFC9B8, 0xF4F1E8, 0x9B9484, 0x3A3A3C, 0xE6E1D3,
    0x3B3A38, 0x262523, 0xF2EFE6, 0xA8A59B, 0x4A4844, 0x4A4844, 0x2E2D2A,
    "COLOR COMPUTER 2"
};

static HWND g_panel, g_parent;
static cocosession *g_session;
static int g_dpi = 96;

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 waiting for a
 * key or pad button to bind to that target. */
static int g_map_state = -2;
static int g_show_bindings;
static int g_kb_on;                 /* the keyboard section is showing */
static int g_hold_stick;            /* the stick stays where it is left */
static WPARAM g_swallow_vkey;       /* the key Map took: its repeats and release too */
static char g_hint[300];

/* The layouts (units), and where they sit in the client. */
static const coco_control *g_ctl;
static int g_nctl;
static float g_ctl_wide, g_ctl_high;
static const coco_keycap *g_cap;
static int g_ncap;
static float g_cap_wide, g_cap_high;
static int g_built_machine = -1;
static double g_joy_unit, g_joy_x, g_joy_y, g_key_unit, g_key_x, g_key_y;
static struct { double cenx, ceny, rad; } g_gate[COCOSESSION_JOYSTICK_PORTS];
static RECT g_base_rc[COCOSESSION_JOYSTICK_PORTS];

/* What the pointer holds. */
static struct {
    int kind;
    int port;
    int target;         /* HOLD_PRESS */
    int part;           /* the layout part (HOLD_PRESS) or key cap (HOLD_CAP) it holds, -1 anywhere */
    int mouse;          /* 1 left, 2 right, 3 middle */
    int action;         /* HOLD_ACTION: the strip button */
} g_hold = { HOLD_NONE, -1, -1, -1, 0, -1 };
static int g_stick_sent[COCOSESSION_JOYSTICK_PORTS][2];

/* What was last painted as held. */
static held_state g_shown;
static struct { double xpos, ypos; } g_knob[COCOSESSION_JOYSTICK_PORTS];

static pads_state g_pads_shown;
static int g_total_w, g_total_h;
static RECT g_case_rc, g_head_rc, g_kb_rc, g_gp_rc, g_gp_head_rc, g_gp_row_rc[PAD_ROWS];
static RECT g_btn_rc[SBT_COUNT], g_hint_rc;

static HBRUSH g_accent_brush;
static HFONT g_font, g_bold, g_small, g_legend, g_tiny, g_mono, g_keybig, g_keysmall, g_keyshift, g_keytiny;

static void refresh_held(void);
static void binding_label(int target, char *out, size_t outsz);

static int scl(int val) { return coco_scale(val, g_dpi); }

/* ---- fonts ------------------------------------------------------------------ */

static void free_fonts(void)
{
    HFONT *const slots[] = { &g_font, &g_bold, &g_small, &g_legend, &g_tiny, &g_mono, &g_keybig,
                             &g_keysmall, &g_keyshift, &g_keytiny };
    size_t idx;
    for (idx = 0; idx < sizeof slots / sizeof slots[0]; idx++)
        if (*slots[idx]) { DeleteObject(*slots[idx]); *slots[idx] = NULL; }
}

static void make_fonts(void)
{
    free_fonts();
    g_font = coco_make_font(g_dpi, 0, FW_NORMAL, NULL);
    g_bold = coco_make_font(g_dpi, 0, FW_BOLD, NULL);
    g_small = coco_make_font(g_dpi, 11, FW_BOLD, NULL);
    g_legend = coco_make_font(g_dpi, 10, FW_BOLD, NULL);
    g_tiny = coco_make_font(g_dpi, 10, FW_NORMAL, NULL);
    g_mono = coco_make_font(g_dpi, 12, FW_BOLD, "Consolas");
    g_keybig = coco_make_font(g_dpi, 13, FW_BOLD, NULL);
    g_keysmall = coco_make_font(g_dpi, 9, FW_BOLD, NULL);
    g_keyshift = coco_make_font(g_dpi, 8, FW_NORMAL, NULL);
    g_keytiny = coco_make_font(g_dpi, 8, FW_BOLD, NULL);
}

/* ---- text -------------------------------------------------------------------- */

/* UTF-8 (legends, key names, gamepad names) through DrawTextW. */
static int text_out(HDC hdc, const char *utf8, RECT *rect, UINT fmt)
{
    wchar_t wide[256];
    if (!utf8 || !*utf8) return 0;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, (int)(sizeof wide / sizeof wide[0])) <= 0) return 0;
    return DrawTextW(hdc, wide, -1, rect, fmt | DT_NOPREFIX);
}

/* Width in pixels of utf8 in the font now selected. */
static int text_width(HDC hdc, const char *utf8)
{
    wchar_t wide[256];
    SIZE extent;
    if (!utf8 || !*utf8) return 0;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, (int)(sizeof wide / sizeof wide[0])) <= 1) return 0;
    if (!GetTextExtentPoint32W(hdc, wide, (int)wcslen(wide), &extent)) return 0;
    return extent.cx;
}

static void text_line(HDC hdc, const char *utf8, const RECT *rect, UINT fmt)
{
    RECT copy = *rect;
    text_out(hdc, utf8, &copy, fmt | DT_SINGLELINE | DT_VCENTER);
}

/* Vertically centred in the rect, wrapped, across by `align` (DT_CENTER, DT_LEFT,
 * DT_RIGHT; a Map legend is a key and a pad button). */
static void text_block_align(HDC hdc, const char *utf8, const RECT *rect, UINT align)
{
    RECT copy = *rect;
    int high;
    InflateRect(&copy, -scl(1), 0);
    high = text_out(hdc, utf8, &copy, align | DT_WORDBREAK | DT_CALCRECT);
    copy.left = rect->left + scl(1);
    copy.right = rect->right - scl(1);
    copy.top = rect->top + ((rect->bottom - rect->top) - high) / 2;
    if (copy.top < rect->top) copy.top = rect->top;
    copy.bottom = rect->bottom;
    text_out(hdc, utf8, &copy, align | DT_WORDBREAK);
}

static COLORREF rgb_of(unsigned val)
{
    return RGB((val >> 16) & 0xff, (val >> 8) & 0xff, val & 0xff);
}

/* pct per cent of over, the rest of base. */
static COLORREF mix_col(COLORREF base, COLORREF over, int pct)
{
    return RGB((GetRValue(base) * (100 - pct) + GetRValue(over) * pct) / 100,
               (GetGValue(base) * (100 - pct) + GetGValue(over) * pct) / 100,
               (GetBValue(base) * (100 - pct) + GetBValue(over) * pct) / 100);
}

#define C_ACCENT   rgb_of(COCOSESSION_ACCENT_RGB)
#define C_ACCENT_D mix_col(rgb_of(COCOSESSION_ACCENT_RGB), RGB(0, 0, 0), 35)
#define C_ON_ACC   RGB(0x14, 0x21, 0x0A)         /* dark text on the accent */
#define C_WHITE    RGB(0xff, 0xff, 0xff)
#define C_BASE_TOP RGB(0xDD, 0xD9, 0xCF)
#define C_BASE_BOT RGB(0xC2, 0xBD, 0xB1)
#define C_BASE_EDG RGB(0x8D, 0x88, 0x7C)
#define C_GATE_RIM RGB(0x8F, 0x8B, 0x80)
#define C_GATE_FLR RGB(0xA9, 0xA5, 0x98)
#define C_GATE_TIC RGB(0x7A, 0x76, 0x6B)
#define C_SECTOR   RGB(0xBD, 0xB9, 0xAC)
#define C_SHAFT    RGB(0x56, 0x52, 0x48)
#define C_FIRE     RGB(0xC8, 0x28, 0x1E)
#define C_FIRE_HI  RGB(0xE5, 0x59, 0x4C)
#define C_FIRE_RIM RGB(0x7D, 0x14, 0x0E)
#define C_RESET    RGB(0x3A, 0x3A, 0x3D)
#define C_RESET_SD RGB(0x1F, 0x1F, 0x21)
#define C_RESET_LN RGB(0xD3, 0x3A, 0x2F)
#define C_READOUT  RGB(0xB9, 0xB5, 0xA8)
#define C_READ_INK RGB(0x3B, 0x3A, 0x36)
#define C_LEGEND   RGB(0x3A, 0x3A, 0x3C)
#define C_HINT     RGB(0x5D, 0x5A, 0x52)
#define C_KNOB     RGB(0x1A, 0x1A, 0x1C)
#define C_KNOB_MID RGB(0x3A, 0x3A, 0x3E)
#define C_KNOB_HI  RGB(0x5A, 0x5A, 0x60)
#define C_KNOB_RIM RGB(0x0A, 0x0A, 0x0B)

static const palette *pal(void)
{
    return g_built_machine == COCOSESSION_MACHINE_COCO2 ? &k_coco2 : &k_coco3;
}

/* ---- art: GDI at a supersampled scale ------------------------------------------- */

/* One art pass draws the client rectangle with its top-left at (g_art_x,
 * g_art_y) into a bitmap g_art_k times the size. Faded parts (Map mode) are
 * drawn at 40% toward g_back, the stand-in for alpha. */
static int g_art_k = 1, g_art_x, g_art_y, g_art_wide, g_art_high;
static int g_fade;
static COLORREF g_back;

static COLORREF tint(COLORREF col)
{
    return g_fade ? mix_col(g_back, col, 40) : col;
}

static int art_x(double val) { return (int)floor((val - g_art_x) * g_art_k + 0.5); }
static int art_y(double val) { return (int)floor((val - g_art_y) * g_art_k + 0.5); }
static int art_len(double val) { return (int)floor(val * g_art_k + 0.5); }

static void poly(HDC hdc, const POINT *pts, int count, COLORREF fill, COLORREF line, double lwid)
{
    HBRUSH brush = CreateSolidBrush(tint(fill));
    HPEN pen = CreatePen(PS_SOLID, lwid > 0 ? art_len(lwid) : 0, tint(line));
    HGDIOBJ oldbrush = SelectObject(hdc, brush);
    HGDIOBJ oldpen = SelectObject(hdc, lwid > 0 ? (HGDIOBJ)pen : GetStockObject(NULL_PEN));
    Polygon(hdc, pts, count);
    SelectObject(hdc, oldpen);
    SelectObject(hdc, oldbrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

static void ellipse(HDC hdc, double cenx, double ceny, double radx, double rady, COLORREF fill, COLORREF line,
                    double lwid)
{
    HBRUSH brush = CreateSolidBrush(tint(fill));
    HPEN pen = CreatePen(PS_SOLID, lwid > 0 ? art_len(lwid) : 0, tint(line));
    HGDIOBJ oldbrush = SelectObject(hdc, brush);
    HGDIOBJ oldpen = SelectObject(hdc, lwid > 0 ? (HGDIOBJ)pen : GetStockObject(NULL_PEN));
    Ellipse(hdc, art_x(cenx - radx), art_y(ceny - rady), art_x(cenx + radx), art_y(ceny + rady));
    SelectObject(hdc, oldpen);
    SelectObject(hdc, oldbrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

/* An outline only. */
static void ring(HDC hdc, double cenx, double ceny, double radius, COLORREF col, double lwid)
{
    HPEN pen = CreatePen(PS_SOLID, art_len(lwid), tint(col));
    HGDIOBJ oldbrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    HGDIOBJ oldpen = SelectObject(hdc, pen);
    Ellipse(hdc, art_x(cenx - radius), art_y(ceny - radius), art_x(cenx + radius), art_y(ceny + radius));
    SelectObject(hdc, oldpen);
    SelectObject(hdc, oldbrush);
    DeleteObject(pen);
}

static void rrect(HDC hdc, double posx, double posy, double wide, double high, double radius, COLORREF fill,
                  COLORREF line, double lwid)
{
    HBRUSH brush = CreateSolidBrush(tint(fill));
    HPEN pen = CreatePen(PS_SOLID, lwid > 0 ? art_len(lwid) : 0, tint(line));
    HGDIOBJ oldbrush = SelectObject(hdc, brush);
    HGDIOBJ oldpen = SelectObject(hdc, lwid > 0 ? (HGDIOBJ)pen : GetStockObject(NULL_PEN));
    RoundRect(hdc, art_x(posx), art_y(posy), art_x(posx + wide), art_y(posy + high), art_len(radius * 2), art_len(radius * 2));
    SelectObject(hdc, oldpen);
    SelectObject(hdc, oldbrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

/* A rounded rectangle filled top to bottom from one colour to another, with an
 * outline. */
static void rrect_grad(HDC hdc, double posx, double posy, double wide, double high, double radius, COLORREF topcol,
                       COLORREF botcol, COLORREF edge, double lwid)
{
    const int top = art_y(posy), bottom = art_y(posy + high), left = art_x(posx), right = art_x(posx + wide);
    HRGN region = CreateRoundRectRgn(left, top, right + 1, bottom + 1, art_len(radius * 2), art_len(radius * 2));
    HGDIOBJ brush = GetStockObject(DC_BRUSH);
    int line;
    SelectClipRgn(hdc, region);
    for (line = top < 0 ? 0 : top; line < bottom && line < g_art_high; line++) {
        RECT strip;
        SetRect(&strip, left, line, right, line + 1);
        SetDCBrushColor(hdc, tint(mix_col(topcol, botcol, bottom > top ? (line - top) * 100 / (bottom - top) : 0)));
        FillRect(hdc, &strip, (HBRUSH)brush);
    }
    SelectClipRgn(hdc, NULL);
    DeleteObject(region);
    if (lwid > 0) {
        HPEN pen = CreatePen(PS_SOLID, art_len(lwid), tint(edge));
        HGDIOBJ oldbrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        HGDIOBJ oldpen = SelectObject(hdc, pen);
        RoundRect(hdc, left, top, right, bottom, art_len(radius * 2), art_len(radius * 2));
        SelectObject(hdc, oldpen);
        SelectObject(hdc, oldbrush);
        DeleteObject(pen);
    }
}

static void stroke(HDC hdc, const POINT *pts, int count, COLORREF col, double lwid)
{
    HPEN pen = CreatePen(PS_SOLID, art_len(lwid), tint(col));
    HGDIOBJ oldpen = SelectObject(hdc, pen);
    Polyline(hdc, pts, count);
    SelectObject(hdc, oldpen);
    DeleteObject(pen);
}

static void line2(HDC hdc, double startx, double starty, double endx, double endy, COLORREF col, double lwid)
{
    POINT pts[2];
    pts[0].x = art_x(startx);
    pts[0].y = art_y(starty);
    pts[1].x = art_x(endx);
    pts[1].y = art_y(endy);
    stroke(hdc, pts, 2, col, lwid);
}

/* The pixel box of a joystick layout part, and of a key cap. */
static void ctl_box(const coco_control *ctl, double *posx, double *posy, double *wide, double *high)
{
    *posx = g_joy_x + (double)ctl->left * g_joy_unit;
    *posy = g_joy_y + (double)ctl->top * g_joy_unit;
    *wide = (double)ctl->wide * g_joy_unit;
    *high = (double)ctl->high * g_joy_unit;
}

static void ctl_rect(const coco_control *ctl, RECT *rect)
{
    double posx, posy, wide, high;
    ctl_box(ctl, &posx, &posy, &wide, &high);
    SetRect(rect, (int)floor(posx), (int)floor(posy), (int)ceil(posx + wide), (int)ceil(posy + high));
}

static void cap_rect(const coco_keycap *cap, RECT *rect)
{
    const double posx = g_key_x + (double)cap->left * g_key_unit, posy = g_key_y + (double)cap->top * g_key_unit;
    SetRect(rect, (int)floor(posx), (int)floor(posy), (int)ceil(posx + (double)cap->wide * g_key_unit),
            (int)ceil(posy + (double)cap->high * g_key_unit));
}

static int binding_mode(void)
{
    return g_map_state != -2 || g_show_bindings;
}

/* Whether a direction is pushed its way, hard. */
static int pushed_dir(const int stick[2], int act)
{
    switch (act) {
    case COCO_ACT_RIGHT: return stick[0] >= PUSH_SECTOR;
    case COCO_ACT_LEFT: return stick[0] <= -PUSH_SECTOR;
    case COCO_ACT_UP: return stick[1] <= -PUSH_SECTOR;
    case COCO_ACT_DOWN: return stick[1] >= PUSH_SECTOR;
    default: return 0;
    }
}

static int ctl_act(const coco_control *ctl)
{
    return ctl->port >= 0 ? ctl->target - ctl->port * COCO_ACT_PER_PORT : -1;
}

/* Whether a part is shown held: what the machine sees, in the accent. In Map
 * mode the picked target is the one lit. */
static int part_lit(const coco_control *ctl)
{
    if (g_map_state != -2)
        return g_map_state >= 0 && ctl->target >= 0 && ctl->target == g_map_state;
    switch (ctl->kind) {
    case COCO_CTL_BUTTON:
        return (g_shown.buttons[ctl->port] >> (ctl_act(ctl) - COCO_ACT_BUTTON1)) & 1;
    case COCO_CTL_RESET:
        return g_shown.reset || (g_hold.kind == HOLD_PRESS && g_hold.target == ctl->target);
    case COCO_CTL_DIRECTION:
        return pushed_dir(g_shown.stick[ctl->port], ctl_act(ctl));
    default:
        return 0;
    }
}

/* In Map mode the controls not picked fade: the gate's knob and the readout
 * always, and with a target picked the other buttons and sectors too. */
static int part_faded(const coco_control *ctl)
{
    if (g_map_state == -2) return 0;
    switch (ctl->kind) {
    case COCO_CTL_BUTTON:
    case COCO_CTL_RESET:
    case COCO_CTL_DIRECTION:
        return g_map_state >= 0 && ctl->target != g_map_state;
    case COCO_CTL_READOUT:
        return 1;
    default:
        return 0;
    }
}

/* The sector of the gate's ring a direction owns: centred on RIGHT 0, UP 90,
 * LEFT 180, DOWN 270 degrees (screen), a little under 90 wide so they read as
 * separate. */
static double sector_centre(int act)
{
    switch (act) {
    case COCO_ACT_UP: return 90.0;
    case COCO_ACT_LEFT: return 180.0;
    case COCO_ACT_DOWN: return 270.0;
    default: return 0.0;
    }
}

static void art_sector(HDC hdc, int port, int act, COLORREF fill, COLORREF line)
{
    POINT pts[40];
    int count = 0, step;
    const double cenx = g_gate[port].cenx, ceny = g_gate[port].ceny, rad = g_gate[port].rad;
    const double start = (sector_centre(act) - 43.0) * PI_VALUE / 180.0, span = 86.0 * PI_VALUE / 180.0;
    for (step = 0; step <= 9; step++) {
        const double ang = start + span * step / 9.0;
        pts[count].x = art_x(cenx + rad * cos(ang));
        pts[count++].y = art_y(ceny - rad * sin(ang));
    }
    for (step = 9; step >= 0; step--) {
        const double ang = start + span * step / 9.0;
        pts[count].x = art_x(cenx + rad * 0.52 * cos(ang));
        pts[count++].y = art_y(ceny - rad * 0.52 * sin(ang));
    }
    poly(hdc, pts, count, fill, line, 1.0);
}

static void art_base(HDC hdc, const coco_control *ctl)
{
    double posx, posy, wide, high;
    const double unit = g_joy_unit;
    ctl_box(ctl, &posx, &posy, &wide, &high);
    rrect(hdc, posx + unit * 0.06, posy + unit * 0.1, wide, high, unit * 0.5, mix_col(g_back, RGB(0, 0, 0), 30),
          mix_col(g_back, RGB(0, 0, 0), 30), 0);
    rrect_grad(hdc, posx, posy, wide, high, unit * 0.5, C_BASE_TOP, C_BASE_BOT, C_BASE_EDG, 1.0);
}

static void art_gate(HDC hdc, const coco_control *ctl)
{
    const int port = ctl->port;
    const double cenx = g_gate[port].cenx, ceny = g_gate[port].ceny, rad = g_gate[port].rad;
    int dia;
    ellipse(hdc, cenx, ceny, rad * 1.05, rad * 1.05, C_GATE_RIM, C_GATE_RIM, 0);
    ellipse(hdc, cenx, ceny, rad, rad, C_GATE_FLR, C_GATE_TIC, 1.0);
    line2(hdc, cenx - rad * 0.94, ceny, cenx + rad * 0.94, ceny, C_GATE_TIC, 1.0);
    line2(hdc, cenx, ceny - rad * 0.94, cenx, ceny + rad * 0.94, C_GATE_TIC, 1.0);
    for (dia = 0; dia < 4; dia++) {
        const double ang = (45.0 + 90.0 * dia) * PI_VALUE / 180.0;
        line2(hdc, cenx + rad * 0.80 * cos(ang), ceny - rad * 0.80 * sin(ang), cenx + rad * 0.94 * cos(ang),
              ceny - rad * 0.94 * sin(ang), C_GATE_TIC, 1.0);
    }
}

static void art_button(HDC hdc, const coco_control *ctl)
{
    double posx, posy, wide, high;
    const int lit = part_lit(ctl);
    ctl_box(ctl, &posx, &posy, &wide, &high);
    {
        const double cenx = posx + wide / 2, ceny = posy + high / 2, rad = wide / 2;
        const double drop = g_joy_unit * 0.09;
        ellipse(hdc, cenx, ceny + drop, rad, rad, lit ? C_ACCENT_D : C_FIRE_RIM, lit ? C_ACCENT_D : C_FIRE_RIM, 0);
        ellipse(hdc, cenx, ceny, rad * 0.97, rad * 0.97, lit ? C_ACCENT : C_FIRE, lit ? C_WHITE : C_FIRE_RIM,
                lit ? 2.0 : 1.0);
        ellipse(hdc, cenx - rad * 0.28, ceny - rad * 0.32, rad * 0.42, rad * 0.34,
                lit ? mix_col(C_ACCENT, C_WHITE, 45) : C_FIRE_HI, lit ? mix_col(C_ACCENT, C_WHITE, 45) : C_FIRE_HI, 0);
    }
}

static void art_reset(HDC hdc, const coco_control *ctl)
{
    double posx, posy, wide, high;
    const int lit = part_lit(ctl);
    const double unit = g_joy_unit;
    ctl_box(ctl, &posx, &posy, &wide, &high);
    rrect(hdc, posx, posy + unit * 0.1, wide, high - unit * 0.1, unit * 0.3, lit ? C_ACCENT_D : C_RESET_SD,
          lit ? C_ACCENT_D : C_RESET_SD, 0);
    rrect(hdc, posx, posy, wide, high - unit * 0.1, unit * 0.3, lit ? C_ACCENT : C_RESET, lit ? C_WHITE : C_RESET_SD,
          lit ? 2.0 : 1.0);
    if (!lit)
        rrect(hdc, posx + unit * 0.14, posy + unit * 0.1, wide - unit * 0.28, unit * 0.12, unit * 0.06,
              mix_col(C_RESET, C_WHITE, 18), mix_col(C_RESET, C_WHITE, 18), 0);
    /* the red underline of its legend, which sits under the cap */
    rrect(hdc, posx + wide * 0.18, posy + high + unit * 0.5, wide * 0.64, unit * 0.07, unit * 0.03, C_RESET_LN,
          C_RESET_LN, 0);
}

static void art_readout(HDC hdc, const coco_control *ctl)
{
    double posx, posy, wide, high;
    const int *stick = g_shown.stick[ctl->port];
    const int active = g_map_state == -2 && (abs(stick[0]) >= PUSH_STICK || abs(stick[1]) >= PUSH_STICK);
    ctl_box(ctl, &posx, &posy, &wide, &high);
    rrect(hdc, posx, posy, wide, high, g_joy_unit * 0.12, active ? C_ACCENT : C_READOUT, active ? C_ACCENT_D : C_BASE_EDG,
          1.0);
}

/* The stick: a shaft from the gate's centre to the knob, the black cap on it
 * at the merged position (eased), ringed in the accent while pushed. */
static void art_knob(HDC hdc, int port)
{
    const double unit = g_joy_unit;
    const double cenx = g_gate[port].cenx, ceny = g_gate[port].ceny;
    double posx = g_knob[port].xpos, posy = g_knob[port].ypos, len = sqrt(posx * posx + posy * posy), rad;
    const int *stick = g_shown.stick[port];
    const int active = g_map_state == -2 && (abs(stick[0]) >= PUSH_STICK || abs(stick[1]) >= PUSH_STICK);
    if (len > 1.0) { posx /= len; posy /= len; }
    {
        const double knobx = cenx + posx * KNOB_TRAVEL * unit, knoby = ceny + posy * KNOB_TRAVEL * unit;
        rad = KNOB_RADIUS * unit;
        line2(hdc, cenx, ceny + unit * 0.06, knobx, knoby + unit * 0.06, C_SHAFT, unit * 0.34);
        ellipse(hdc, cenx, ceny + unit * 0.06, unit * 0.17, unit * 0.17, C_SHAFT, C_SHAFT, 0);
        ellipse(hdc, knobx + unit * 0.05, knoby + unit * 0.1, rad, rad, C_SHAFT, C_SHAFT, 0);
        ellipse(hdc, knobx, knoby, rad, rad, C_KNOB, C_KNOB_RIM, 1.0);
        ellipse(hdc, knobx - rad * 0.12, knoby - rad * 0.14, rad * 0.78, rad * 0.78, C_KNOB_MID, C_KNOB_MID, 0);
        ellipse(hdc, knobx - rad * 0.28, knoby - rad * 0.32, rad * 0.4, rad * 0.34, C_KNOB_HI, C_KNOB_HI, 0);
        if (active)
            ring(hdc, knobx, knoby, rad + g_joy_unit * 0.07, C_ACCENT, 2.5);
    }
}

static void art_joysticks(HDC hdc)
{
    int idx, port;
    for (idx = 0; idx < g_nctl; idx++) {
        const coco_control *ctl = &g_ctl[idx];
        g_fade = part_faded(ctl);
        switch (ctl->kind) {
        case COCO_CTL_BASE: art_base(hdc, ctl); break;
        case COCO_CTL_STICK: art_gate(hdc, ctl); break;
        case COCO_CTL_DIRECTION: {
            const int lit = part_lit(ctl);
            if (lit || binding_mode())
                art_sector(hdc, ctl->port, ctl_act(ctl), lit ? C_ACCENT : C_SECTOR, lit ? C_WHITE : C_GATE_TIC);
            break;
        }
        case COCO_CTL_BUTTON: art_button(hdc, ctl); break;
        case COCO_CTL_RESET: art_reset(hdc, ctl); break;
        case COCO_CTL_READOUT: art_readout(hdc, ctl); break;
        default: break;
        }
    }
    g_fade = g_map_state != -2;
    for (port = 0; port < COCOSESSION_JOYSTICK_PORTS; port++)
        art_knob(hdc, port);
    g_fade = 0;
}

static COLORREF cap_colour(const coco_keycap *cap)
{
    const palette *pals = pal();
    switch (cap->kind) {
    case COCO_KEYCAP_SPECIAL:
    case COCO_KEYCAP_MODIFIER:
    case COCO_KEYCAP_SPACE:
        return rgb_of(pals->keyspecial);
    case COCO_KEYCAP_FUNCTION:
        return rgb_of(pals->keyfunc);
    default:
        return rgb_of(pals->keycap);
    }
}

static void art_keyboard(HDC hdc)
{
    const palette *pals = pal();
    const COLORREF plate = mix_col(rgb_of(pals->casebot), RGB(0, 0, 0), 28);
    int idx;
    rrect(hdc, g_kb_rc.left, g_kb_rc.top, g_kb_rc.right - g_kb_rc.left, g_kb_rc.bottom - g_kb_rc.top, scl(8), plate,
          mix_col(plate, RGB(0, 0, 0), 30), 1.0);
    g_back = plate;
    g_fade = g_map_state != -2;
    for (idx = 0; idx < g_ncap; idx++) {
        const coco_keycap *cap = &g_cap[idx];
        const int held = idx < MAX_CAPS ? g_shown.caps[idx] : 0;
        const int lit = (held & 3) != 0;
        RECT rect;
        double posx, posy, wide, high;
        cap_rect(cap, &rect);
        posx = rect.left;
        posy = rect.top;
        wide = rect.right - rect.left;
        high = rect.bottom - rect.top;
        rrect(hdc, posx, posy + scl(2), wide, high - scl(2), scl(4), lit ? C_ACCENT_D : rgb_of(pals->keyside),
              lit ? C_ACCENT_D : rgb_of(pals->keyside), 0);
        rrect(hdc, posx, posy, wide, high - scl(2), scl(4), lit ? C_ACCENT : cap_colour(cap),
              lit ? C_WHITE : rgb_of(pals->keyside), lit ? 1.5 : 1.0);
        if (held & 2)       /* a latched modifier: a dot as well, so it reads when other keys are down */
            ellipse(hdc, posx + scl(6), posy + scl(6), scl(3), scl(3), C_ON_ACC, C_ON_ACC, 0);
    }
    g_fade = 0;
}

/* The case: its rounded body, the header strip with its accent line, the
 * joysticks and the keyboard. */
static void art_case(HDC hdc)
{
    const palette *pals = pal();
    const int wide = g_case_rc.right - g_case_rc.left, high = g_case_rc.bottom - g_case_rc.top;
    g_back = mix_col(rgb_of(pals->casetop), rgb_of(pals->casebot), 50);
    g_fade = 0;
    rrect_grad(hdc, g_case_rc.left, g_case_rc.top, wide, high, scl(16), rgb_of(pals->casetop), rgb_of(pals->casebot),
               rgb_of(pals->caseedge), 1.5);
    line2(hdc, g_case_rc.left + scl(16), g_case_rc.top + scl(2), g_case_rc.right - scl(16), g_case_rc.top + scl(2),
          rgb_of(pals->casehigh), 1.5);
    rrect(hdc, g_head_rc.left, g_head_rc.top, g_head_rc.right - g_head_rc.left, g_head_rc.bottom - g_head_rc.top,
          scl(5), rgb_of(pals->strip), rgb_of(pals->strip), 0);
    rrect(hdc, g_head_rc.left, g_head_rc.bottom - scl(2), g_head_rc.right - g_head_rc.left, scl(2), 0, C_ACCENT,
          C_ACCENT, 0);
    art_joysticks(hdc);
    if (g_kb_on)
        art_keyboard(hdc);
}

/* The labels and legends over the art, at the window's own size. */
static void text_joysticks(HDC hdc)
{
    const palette *pals = pal();
    int idx;
    SetBkMode(hdc, TRANSPARENT);
    g_back = mix_col(rgb_of(pals->casetop), rgb_of(pals->casebot), 50);
    for (idx = 0; idx < g_nctl; idx++) {
        const coco_control *ctl = &g_ctl[idx];
        RECT rect;
        const int lit = part_lit(ctl);
        ctl_rect(ctl, &rect);
        g_fade = part_faded(ctl);
        switch (ctl->kind) {
        case COCO_CTL_CAPTION:
            SelectObject(hdc, g_bold);
            SetTextColor(hdc, rgb_of(pals->ink));
            text_line(hdc, ctl->label, &rect, DT_CENTER);
            break;
        case COCO_CTL_BUTTON:
        case COCO_CTL_RESET: {
            RECT cap = rect, under;
            cap.bottom = rect.top + (LONG)(g_joy_unit + 0.5);
            if (binding_mode() && ctl->target >= 0) {
                /* beside the cap, toward the middle of the plate; never on it */
                char legend[96];
                const int first = ctl_act(ctl) == COCO_ACT_BUTTON1;
                const int note_wide = (int)(g_joy_unit * 1.3);
                RECT note = cap;
                if (ctl->kind == COCO_CTL_RESET) {
                    /* RESET is wide: its legend stays on the cap */
                } else if (first) {
                    note.left = rect.right + scl(4);
                    note.right = note.left + note_wide;
                } else {
                    note.right = rect.left - scl(4);
                    note.left = note.right - note_wide;
                }
                binding_label(ctl->target, legend, sizeof legend);
                SelectObject(hdc, g_tiny);
                SetTextColor(hdc, ctl->kind == COCO_CTL_RESET ? tint(lit ? C_ON_ACC : RGB(0xE0, 0xE0, 0xE0))
                                                              : tint(C_LEGEND));
                text_block_align(hdc, legend, &note,
                                 ctl->kind == COCO_CTL_RESET ? DT_CENTER : (first ? DT_LEFT : DT_RIGHT));
            }
            if (ctl->kind == COCO_CTL_BUTTON) {
                /* its number on the base, under the cap */
                SetRect(&under, rect.left - scl(4), cap.bottom + scl(1), rect.right + scl(4),
                        cap.bottom + (int)(g_joy_unit * 0.5));
                SelectObject(hdc, g_small);
                SetTextColor(hdc, tint(C_LEGEND));
                text_line(hdc, ctl->label, &under, DT_CENTER);
            } else {
                SetRect(&under, rect.left - (int)(g_joy_unit * 0.3), cap.bottom + scl(1),
                        rect.right + (int)(g_joy_unit * 0.3), cap.bottom + (int)(g_joy_unit * 0.5));
                SelectObject(hdc, g_legend);
                SetTextColor(hdc, tint(rgb_of(pals->ink)));
                text_line(hdc, "RESET", &under, DT_CENTER);
            }
            break;
        }
        case COCO_CTL_DIRECTION:
            if (binding_mode() && ctl->target >= 0) {
                /* a sector is small: the legend sits at its middle, the key over the
                 * pad button, each on one line */
                char legend[96], *newline;
                const double ang = sector_centre(ctl_act(ctl)) * PI_VALUE / 180.0, side = fabs(cos(sector_centre(ctl_act(ctl)) * PI_VALUE / 180.0)) > 0.5,
                             dist = g_gate[ctl->port].rad * (side ? 0.62 : 0.76);
                const int midx = (int)(g_gate[ctl->port].cenx + dist * cos(ang));
                const int midy = (int)(g_gate[ctl->port].ceny - dist * sin(ang));
                const int line = scl(11), half = (int)(g_joy_unit * 0.85);
                RECT part;
                binding_label(ctl->target, legend, sizeof legend);
                newline = strchr(legend, '\n');
                if (newline) *newline = '\0';
                SelectObject(hdc, g_tiny);
                SetTextColor(hdc, tint(lit ? C_ON_ACC : C_LEGEND));
                SetRect(&part, midx - half, midy - line, midx + half, midy);
                text_line(hdc, legend, &part, DT_CENTER | DT_END_ELLIPSIS);
                SetRect(&part, midx - half, midy, midx + half, midy + line);
                if (newline) text_line(hdc, newline + 1, &part, DT_CENTER | DT_END_ELLIPSIS);
            }
            break;
        case COCO_CTL_READOUT: {
            char text[32];
            const int *stick = g_shown.stick[ctl->port];
            snprintf(text, sizeof text, "X %2d  Y %2d", coco_axis_6bit(stick[0]), coco_axis_6bit(stick[1]));
            SelectObject(hdc, g_mono);
            SetTextColor(hdc, tint(g_map_state == -2 && (abs(stick[0]) >= PUSH_STICK || abs(stick[1]) >= PUSH_STICK)
                                       ? C_ON_ACC : C_READ_INK));
            text_line(hdc, text, &rect, DT_CENTER);
            break;
        }
        default:
            break;
        }
    }
    g_fade = 0;
}

static void text_keyboard(HDC hdc)
{
    const palette *pals = pal();
    int idx;
    SetBkMode(hdc, TRANSPARENT);
    g_back = mix_col(rgb_of(pals->casebot), RGB(0, 0, 0), 28);
    g_fade = g_map_state != -2;
    for (idx = 0; idx < g_ncap; idx++) {
        const coco_keycap *cap = &g_cap[idx];
        const int lit = idx < MAX_CAPS && (g_shown.caps[idx] & 3);
        RECT rect, main;
        cap_rect(cap, &rect);
        rect.bottom -= scl(2);
        main = rect;
        SetTextColor(hdc, tint(lit ? C_ON_ACC : rgb_of(pals->keytext)));
        if (cap->shifted) {
            RECT top = rect;
            top.left += scl(3);
            top.bottom = rect.top + (rect.bottom - rect.top) / 2;
            SelectObject(hdc, g_keyshift);
            SetTextColor(hdc, tint(lit ? C_ON_ACC : rgb_of(pals->keyshift)));
            text_line(hdc, cap->shifted, &top, DT_LEFT);
            main.top += (rect.bottom - rect.top) / 5;
            SetTextColor(hdc, tint(lit ? C_ON_ACC : rgb_of(pals->keytext)));
        }
        if (cap->kind == COCO_KEYCAP_NORMAL || cap->kind == COCO_KEYCAP_ARROW) {
            SelectObject(hdc, g_keybig);
        } else {
            /* a named key steps down a size until it clears both edges */
            SelectObject(hdc, g_keysmall);
            if (text_width(hdc, cap->label) > (main.right - main.left) - scl(4))
                SelectObject(hdc, g_keytiny);
        }
        text_line(hdc, cap->label, &main, DT_CENTER);
    }
    g_fade = 0;
}

static void text_header(HDC hdc)
{
    const palette *pals = pal();
    RECT rect = g_head_rc;
    rect.left += scl(10);
    rect.bottom -= scl(2);
    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, g_bold);
    SetTextColor(hdc, rgb_of(pals->striptext));
    text_line(hdc, pals->title, &rect, DT_LEFT);
}

/* ---- the strip: gamepads, Map ------------------------------------------------------- */

static void draw_face(HDC hdc, const RECT *rect, int pushed, int accent)
{
    RECT copy = *rect;
    if (accent) {
        FillRect(hdc, &copy, g_accent_brush);
        FrameRect(hdc, &copy, (HBRUSH)GetStockObject(GRAY_BRUSH));
        SetTextColor(hdc, C_ON_ACC);
    } else {
        DrawFrameControl(hdc, &copy, DFC_BUTTON, DFCS_BUTTONPUSH | (pushed ? DFCS_PUSHED : 0));
        SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
    }
}

/* A pad button's name short enough for a sector or a legend: "D-pad Right" is
 * "D-Right", the shoulders, triggers and stick clicks are two letters. */
static void pad_legend(int button, char *dst, size_t dstsz)
{
    const char *name = coco_pad_button_name(button);
    if (!strncmp(name, "D-pad ", 6))
        snprintf(dst, dstsz, "D-%s", name + 6);
    else if (!strcmp(name, "Left Shoulder"))
        snprintf(dst, dstsz, "LB");
    else if (!strcmp(name, "Right Shoulder"))
        snprintf(dst, dstsz, "RB");
    else if (!strcmp(name, "Left Trigger"))
        snprintf(dst, dstsz, "LT");
    else if (!strcmp(name, "Right Trigger"))
        snprintf(dst, dstsz, "RT");
    else if (!strcmp(name, "Left Stick"))
        snprintf(dst, dstsz, "LS");
    else if (!strcmp(name, "Right Stick"))
        snprintf(dst, dstsz, "RS");
    else
        snprintf(dst, dstsz, "%s", name);
}

/* In Map mode a target shows what drives it: the key, then the pad button. */
static void binding_label(int target, char *out, size_t outsz)
{
    const coco_binding bind = cocosession_binding_get(g_session, target);
    char key[32];
    key[0] = '\0';
    cocosession_keysym_name(bind.keysym, key, sizeof key);
    if (bind.button != COCO_PAD_BTN_NONE) {
        char pad[32];
        pad_legend(bind.button, pad, sizeof pad);
        snprintf(out, outsz, "%s\n%s", key[0] ? key : "\xe2\x80\x94", pad);
    } else {
        snprintf(out, outsz, "%s", key[0] ? key : "\xe2\x80\x94");
    }
}

static const char *strip_label(int btn)
{
    switch (btn) {
    case SBT_MAP: return g_map_state == -2 ? "Map" : "Done";
    case SBT_CLEAR: return "Clear";
    case SBT_DEFAULTS: return "Defaults";
    case SBT_SHOW: return "Show bindings";
    case SBT_KEYBOARD: return "Keyboard";
    case SBT_HOLD: return "Hold stick";
    case SBT_STOP: return "Stop in Debugger";
    default: return "Help";
    }
}

static int strip_target(int btn)
{
    return btn == SBT_STOP ? COCO_TARGET_SYSACT(COCO_SYSACT_DEBUG_STOP) : -1;
}

static int strip_on(int btn)
{
    return (btn == SBT_MAP && g_map_state != -2) || (btn == SBT_SHOW && g_show_bindings) ||
           (btn == SBT_KEYBOARD && g_kb_on) || (btn == SBT_HOLD && g_hold_stick) ||
           (g_map_state >= 0 && strip_target(btn) == g_map_state);
}

static void segment_rect(int row, int seg, RECT *rect)
{
    const RECT *rowrc = &g_gp_row_rc[row];
    const int posx = rowrc->right - (3 - seg) * (scl(SEG_WIDE) + scl(4)) + scl(4);
    SetRect(rect, posx, rowrc->top + 1, posx + scl(SEG_WIDE), rowrc->bottom - 1);
}

static void gp_name_rect(int row, RECT *rect)
{
    RECT seg0;
    segment_rect(row, 0, &seg0);
    *rect = g_gp_row_rc[row];
    rect->right = seg0.left - scl(STATE_WIDE) - scl(8);
}

/* One row per gamepad: its name, the joystick it drives now, and the
 * Automatic / Right / Left choice, the current one pushed in. */
static void paint_gamepads(HDC hdc)
{
    static const char *const segs[3] = { "Automatic", "Right", "Left" };
    static const char *const drives[3] = { "drives nothing", "drives right", "drives left" };
    const int count = cocosession_gamepad_count(g_session);
    int idx, seg;

    SelectObject(hdc, g_bold);
    SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
    text_line(hdc, "Gamepads", &g_gp_head_rc, DT_LEFT);
    SelectObject(hdc, g_font);
    SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
    if (count == 0) {
        text_line(hdc, "No gamepads connected", &g_gp_row_rc[0], DT_LEFT);
        return;
    }
    if (count > PAD_ROWS) {
        char more[48];
        snprintf(more, sizeof more, "%d more not shown (see Input)", count - PAD_ROWS);
        text_line(hdc, more, &g_gp_head_rc, DT_RIGHT);
    }
    for (idx = 0; idx < count && idx < PAD_ROWS; idx++) {
        const int assign = cocosession_gamepad_assignment(g_session, idx);
        const int eff = cocosession_gamepad_effective_port(g_session, idx);
        const int chosen = assign < 0 || assign >= COCOSESSION_JOYSTICK_PORTS ? 0 : assign + 1;
        char name[128];
        RECT name_rc, eff_rc, seg0;

        gp_name_rect(idx, &name_rc);
        segment_rect(idx, 0, &seg0);
        cocosession_gamepad_name(g_session, idx, name, sizeof name);
        SelectObject(hdc, g_font);
        SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
        text_line(hdc, name, &name_rc, DT_LEFT | DT_END_ELLIPSIS);
        SetRect(&eff_rc, name_rc.right + scl(8), name_rc.top, seg0.left - scl(8), name_rc.bottom);
        SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
        text_line(hdc, drives[eff >= 0 && eff < COCOSESSION_JOYSTICK_PORTS ? eff + 1 : 0], &eff_rc, DT_RIGHT);
        for (seg = 0; seg < 3; seg++) {
            RECT segrc;
            segment_rect(idx, seg, &segrc);
            draw_face(hdc, &segrc, seg == chosen, 0);
            text_line(hdc, segs[seg], &segrc, DT_CENTER);
        }
    }
}

static void paint_strip(HDC hdc)
{
    int btn;
    SelectObject(hdc, g_font);
    for (btn = 0; btn < SBT_COUNT; btn++) {
        draw_face(hdc, &g_btn_rc[btn], g_hold.kind == HOLD_ACTION && btn == g_hold.action, strip_on(btn));
        text_line(hdc, strip_label(btn), &g_btn_rc[btn], DT_CENTER);
    }
    SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
    text_line(hdc, g_hint, &g_hint_rc, DT_LEFT | DT_END_ELLIPSIS);
}

/* The whole window: the art supersampled, its text, the strip. Only what the
 * update region needs is drawn. */
static void paint_rect(HDC hdc, const RECT *inval)
{
    RECT art;
    const int kay = 2;
    HDC mem;
    HBITMAP bmp;
    void *bits = NULL;
    BITMAPINFO bmi;
    HGDIOBJ old;

    FillRect(hdc, inval, GetSysColorBrush(COLOR_BTNFACE));
    SetBkMode(hdc, TRANSPARENT);

    /* the art: the intersection with the case, at 2x */
    IntersectRect(&art, inval, &g_case_rc);
    if (!IsRectEmpty(&art)) {
        const int wide = art.right - art.left, high = art.bottom - art.top;
        memset(&bmi, 0, sizeof bmi);
        bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
        bmi.bmiHeader.biWidth = wide * kay;
        bmi.bmiHeader.biHeight = -high * kay;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        mem = CreateCompatibleDC(hdc);
        bmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (bmp) {
            HBRUSH face = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
            RECT all = { 0, 0, wide * kay, high * kay };
            old = SelectObject(mem, bmp);
            FillRect(mem, &all, face);
            DeleteObject(face);
            SetBkMode(mem, TRANSPARENT);
            g_art_k = kay;
            g_art_x = art.left;
            g_art_y = art.top;
            g_art_wide = wide * kay;
            g_art_high = high * kay;
            art_case(mem);
            g_art_k = 1;
            SetStretchBltMode(hdc, HALFTONE);
            SetBrushOrgEx(hdc, 0, 0, NULL);
            StretchBlt(hdc, art.left, art.top, wide, high, mem, 0, 0, wide * kay, high * kay, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
        }
        DeleteDC(mem);
        SelectObject(hdc, g_font);
        text_header(hdc);
        text_joysticks(hdc);
        if (g_kb_on)
            text_keyboard(hdc);
    }
    paint_gamepads(hdc);
    paint_strip(hdc);
}

/* ---- pressing ----------------------------------------------------------------- */

static void invalidate_part(int idx)
{
    RECT rect;
    if (!g_panel || idx < 0 || idx >= g_nctl) return;
    ctl_rect(&g_ctl[idx], &rect);
    InflateRect(&rect, scl(14), scl(14));
    InvalidateRect(g_panel, &rect, FALSE);
}

static void invalidate_joystick(int port)
{
    if (g_panel && port >= 0 && port < COCOSESSION_JOYSTICK_PORTS)
        InvalidateRect(g_panel, &g_base_rc[port], FALSE);
}

static void invalidate_cap(int idx)
{
    RECT rect;
    if (!g_panel || idx < 0 || idx >= g_ncap) return;
    cap_rect(&g_cap[idx], &rect);
    InflateRect(&rect, scl(2), scl(2));
    InvalidateRect(g_panel, &rect, FALSE);
}

/* The pointer's stick on a joystick, in milli-units. */
static void set_stick(int port, int xaxis, int yaxis)
{
    if (port < 0 || port >= COCOSESSION_JOYSTICK_PORTS) return;
    if (g_stick_sent[port][0] == xaxis && g_stick_sent[port][1] == yaxis) return;
    g_stick_sent[port][0] = xaxis;
    g_stick_sent[port][1] = yaxis;
    cocosession_pointer_set(g_session, port, xaxis, yaxis, 0);
}

static void centre_sticks(void)
{
    int port;
    for (port = 0; port < COCOSESSION_JOYSTICK_PORTS; port++) {
        g_stick_sent[port][0] = g_stick_sent[port][1] = 0;
        cocosession_pointer_set(g_session, port, 0, 0, 0);
    }
}

/* Lets go of what the pointer holds in this window (only that: the main window
 * releases its own keys). A switch or a session action fires on release inside
 * its button, like any button. */
static void mouse_release(int fire)
{
    const int kind = g_hold.kind, port = g_hold.port, target = g_hold.target, part = g_hold.part,
              action = g_hold.action;
    g_hold.kind = HOLD_NONE;
    g_hold.port = g_hold.target = g_hold.part = g_hold.action = -1;
    g_hold.mouse = 0;
    switch (kind) {
    case HOLD_STICK:
        if (!g_hold_stick) set_stick(port, 0, 0);
        invalidate_joystick(port);
        break;
    case HOLD_PRESS:
        if (target == COCO_TARGET_SWITCH(COCO_SW_RESET)) {
            if (fire) {
                cocosession_press(g_session, target, 1);
                cocosession_press(g_session, target, 0);
            }
        } else {
            cocosession_press(g_session, target, 0);
        }
        invalidate_part(part);
        invalidate_joystick(port);
        break;
    case HOLD_CAP:
        cocosession_keyboard_press(g_session, part, 0);
        invalidate_cap(part);
        break;
    case HOLD_ACTION:
        if (fire) cocosession_sysaction_post(g_session, COCO_SYSACT_DEBUG_STOP);
        if (g_panel && action >= 0) InvalidateRect(g_panel, &g_btn_rc[action], FALSE);
        break;
    default:
        break;
    }
}

/* Everything this window holds, the stick centred and the keyboard let go. */
static void release_everything(void)
{
    mouse_release(0);
    centre_sticks();
    cocosession_keyboard_release(g_session);
}

static void set_hint(const char *text)
{
    snprintf(g_hint, sizeof g_hint, "%s", text);
    if (g_panel) InvalidateRect(g_panel, &g_hint_rc, FALSE);
}

#define IDLE_HINT "Drag a stick, press the fire buttons, type on the keys. Map rebinds."

static void set_map_state(int state)
{
    g_map_state = state;
    if (state < 0) {
        if (g_panel) KillTimer(g_panel, IDT_CONTROLLERS_CAPTURE);
        cocosession_gamepad_capture_cancel(g_session);
    }
    if (state != -2) {
        /* typing is for the bindings now: what the machine holds lets go */
        release_everything();
        cocosession_release_all(g_session);
    }
    if (state == -2) {
        set_hint(IDLE_HINT);
    } else if (state == -1) {
        set_hint("Click a control to remap. Esc finishes.");
    } else {
        char msg[200];
        snprintf(msg, sizeof msg, "Press a key or a gamepad button for %s. Esc cancels.", coco_target_name(state));
        set_hint(msg);
        cocosession_gamepad_capture_begin(g_session);
        if (g_panel) SetTimer(g_panel, IDT_CONTROLLERS_CAPTURE, 50, NULL);
    }
    if (g_panel) {
        InvalidateRect(g_panel, NULL, FALSE);
        refresh_held();
    }
}

/* After a rebind: what it is now, and whom it was taken from. */
static void bound_hint(const char *what, const char *stolen)
{
    char msg[260];
    if (stolen[0]) snprintf(msg, sizeof msg, "Bound %s (it was %s's)", what, stolen);
    else snprintf(msg, sizeof msg, "Bound %s (it stops going to the CoCo)", what);
    set_hint(msg);
}

/* ---- layout --------------------------------------------------------------------- */

static int machine_now(void)
{
    const int machine = cocosession_machine(g_session);
    return machine == COCOSESSION_MACHINE_COCO2 ? COCOSESSION_MACHINE_COCO2 : COCOSESSION_MACHINE_COCO3;
}

/* The case with the joysticks (and the keyboard) in it, the gamepad rows, the
 * strip: the window is this size always, bar the Keyboard button. */
static void layout_static(void)
{
    const int left = scl(MARGIN);
    int posy = scl(MARGIN), idx, posx, content_w;
    double bottom;
    static const int btn_wide[SBT_COUNT] = { 60, 60, 76, 116, 64, 100, 104, 130 };

    g_ctl = cocosession_controls_layout(g_built_machine, &g_nctl, &g_ctl_wide, &g_ctl_high);
    g_cap = cocosession_keyboard_layout(g_built_machine, &g_ncap, &g_cap_wide, &g_cap_high);
    g_joy_unit = (double)JOY_UNIT * g_dpi / 96.0;
    g_key_unit = (double)KEY_UNIT * g_dpi / 96.0;
    content_w = scl(CASE_WIDE);
    g_total_w = 2 * left + content_w;

    SetRect(&g_case_rc, left, posy, left + content_w, 0);
    SetRect(&g_head_rc, g_case_rc.left + scl(6), g_case_rc.top + scl(6), g_case_rc.right - scl(6),
            g_case_rc.top + scl(6) + scl(HEAD_HIGH));
    g_joy_x = g_case_rc.left + (content_w - (double)g_ctl_wide * g_joy_unit) / 2;
    g_joy_y = g_head_rc.bottom + scl(12);
    bottom = g_joy_y + (double)g_ctl_high * g_joy_unit;
    SetRectEmpty(&g_kb_rc);
    if (g_kb_on) {
        g_key_x = g_case_rc.left + (content_w - (double)g_cap_wide * g_key_unit) / 2;
        g_key_y = bottom + scl(18);
        SetRect(&g_kb_rc, (int)g_key_x - scl(8), (int)g_key_y - scl(8), (int)ceil(g_key_x + g_cap_wide * g_key_unit) + scl(8),
                (int)ceil(g_key_y + g_cap_high * g_key_unit) + scl(8));
        bottom = g_kb_rc.bottom;
    }
    g_case_rc.bottom = (int)ceil(bottom) + scl(CASE_PAD);
    posy = g_case_rc.bottom + scl(12);

    for (idx = 0; idx < g_nctl; idx++) {
        const coco_control *ctl = &g_ctl[idx];
        if (ctl->port < 0 || ctl->port >= COCOSESSION_JOYSTICK_PORTS) continue;
        if (ctl->kind == COCO_CTL_STICK) {
            g_gate[ctl->port].cenx = g_joy_x + ((double)ctl->left + ctl->wide / 2) * g_joy_unit;
            g_gate[ctl->port].ceny = g_joy_y + ((double)ctl->top + ctl->high / 2) * g_joy_unit;
            g_gate[ctl->port].rad = (double)ctl->wide / 2 * g_joy_unit;
        } else if (ctl->kind == COCO_CTL_BASE) {
            ctl_rect(ctl, &g_base_rc[ctl->port]);
            InflateRect(&g_base_rc[ctl->port], scl(8), scl(8));
        }
    }

    SetRect(&g_gp_head_rc, left, posy, left + content_w, posy + scl(HEAD_HIGH));
    posy += scl(HEAD_HIGH);
    for (idx = 0; idx < PAD_ROWS; idx++) {
        SetRect(&g_gp_row_rc[idx], left, posy, left + content_w, posy + scl(ROW_HIGH));
        posy += scl(ROW_HIGH) + scl(2);
    }
    SetRect(&g_gp_rc, left, g_gp_head_rc.top, left + content_w, posy);
    posy += scl(8);

    posx = left;
    for (idx = SBT_MAP; idx <= SBT_HELP; idx++) {
        if (idx == SBT_HELP) posx = left + content_w - scl(btn_wide[idx]);
        SetRect(&g_btn_rc[idx], posx, posy, posx + scl(btn_wide[idx]), posy + scl(BTN_HIGH));
        posx += scl(btn_wide[idx]) + scl(BTN_GAP);
        if (idx == SBT_SHOW) posx += scl(8);
    }
    posy += scl(BTN_HIGH) + scl(BTN_GAP);
    posx = left;
    for (idx = SBT_KEYBOARD; idx <= SBT_STOP; idx++) {
        SetRect(&g_btn_rc[idx], posx, posy, posx + scl(btn_wide[idx]), posy + scl(BTN_HIGH));
        posx += scl(btn_wide[idx]) + scl(BTN_GAP);
    }
    posy += scl(BTN_HIGH) + scl(BTN_GAP);
    SetRect(&g_hint_rc, left, posy, left + content_w, posy + scl(20));
    g_total_h = posy + scl(20) + scl(MARGIN);
}

/* The window's outer size for the laid-out client. */
static void window_size(HWND hwnd, SIZE *size)
{
    RECT wrect, crect;
    if (hwnd && GetWindowRect(hwnd, &wrect) && GetClientRect(hwnd, &crect) && crect.right > 0) {
        size->cx = (wrect.right - wrect.left) - crect.right + g_total_w;
        size->cy = (wrect.bottom - wrect.top) - crect.bottom + g_total_h;
    } else {
        SetRect(&wrect, 0, 0, g_total_w, g_total_h);
        AdjustWindowRectEx(&wrect, PANEL_STYLE, FALSE, PANEL_EXSTYLE);
        size->cx = wrect.right - wrect.left;
        size->cy = wrect.bottom - wrect.top;
    }
}

/* Exactly the laid-out client size, whatever the frame is at this DPI. */
static void fit_client(void)
{
    RECT wrect, crect;
    GetWindowRect(g_panel, &wrect);
    GetClientRect(g_panel, &crect);
    if (crect.right == g_total_w && crect.bottom == g_total_h) return;
    SetWindowPos(g_panel, NULL, 0, 0, (wrect.right - wrect.left) - crect.right + g_total_w,
                 (wrect.bottom - wrect.top) - crect.bottom + g_total_h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void rebuild(void)
{
    if (g_panel) release_everything();
    if (g_panel && GetCapture() == g_panel) ReleaseCapture();
    g_built_machine = machine_now();
    make_fonts();
    layout_static();
    memset(&g_shown, 0, sizeof g_shown);
    memset(g_knob, 0, sizeof g_knob);
    if (g_panel) {
        fit_client();
        InvalidateRect(g_panel, NULL, FALSE);
        refresh_held();
    }
}

static void rescale(int dpi)
{
    g_dpi = dpi;
    rebuild();
}

/* ---- polling -------------------------------------------------------------------------- */

/* What the machine sees held, lit; the knobs ease toward it. */
static void refresh_held(void)
{
    held_state now;
    int port, idx;
    if (!g_panel || g_map_state != -2) return;
    memset(&now, 0, sizeof now);
    for (port = 0; port < COCOSESSION_JOYSTICK_PORTS; port++) {
        cocosession_joystick_held(g_session, port, &now.stick[port][0], &now.stick[port][1]);
        now.buttons[port] = cocosession_buttons_held(g_session, port);
    }
    now.reset = cocosession_switch_held(g_session, COCO_SW_RESET) != 0;
    if (g_kb_on)
        for (idx = 0; idx < g_ncap && idx < MAX_CAPS; idx++)
            now.caps[idx] = (unsigned char)cocosession_keyboard_held(g_session, idx);

    for (port = 0; port < COCOSESSION_JOYSTICK_PORTS; port++) {
        const double tgtx = now.stick[port][0] / 1000.0, tgty = now.stick[port][1] / 1000.0;
        const double oldx = g_knob[port].xpos, oldy = g_knob[port].ypos;
        g_knob[port].xpos = fabs(tgtx - oldx) < 0.004 ? tgtx : oldx + (tgtx - oldx) * 0.4;
        g_knob[port].ypos = fabs(tgty - oldy) < 0.004 ? tgty : oldy + (tgty - oldy) * 0.4;
        if (memcmp(now.stick[port], g_shown.stick[port], sizeof now.stick[port]) != 0 ||
            now.buttons[port] != g_shown.buttons[port] || g_knob[port].xpos != oldx || g_knob[port].ypos != oldy)
            invalidate_joystick(port);
    }
    if (now.reset != g_shown.reset) {
        for (idx = 0; idx < g_nctl; idx++)
            if (g_ctl[idx].kind == COCO_CTL_RESET) invalidate_part(idx);
    }
    for (idx = 0; idx < g_ncap && idx < MAX_CAPS; idx++)
        if (now.caps[idx] != g_shown.caps[idx]) invalidate_cap(idx);
    g_shown = now;
}

/* Hot-plugged pads, auto-assignment changes, and the machine changed in the
 * main window. */
static void refresh_pads(void)
{
    pads_state now;
    int idx;
    memset(&now, 0, sizeof now);
    now.generation = cocosession_gamepad_generation(g_session);
    now.count = cocosession_gamepad_count(g_session);
    for (idx = 0; idx < PAD_ROWS && idx < now.count; idx++) {
        now.assign[idx] = cocosession_gamepad_assignment(g_session, idx);
        now.effective[idx] = cocosession_gamepad_effective_port(g_session, idx);
    }
    if (memcmp(&now, &g_pads_shown, sizeof now) != 0) {
        g_pads_shown = now;
        InvalidateRect(g_panel, &g_gp_rc, FALSE);
    }
    if (g_built_machine != machine_now())
        rebuild();
}

/* ---- keys ------------------------------------------------------------------------------ */

static void hide_panel(void)
{
    set_map_state(-2);
    release_everything();
    if (GetCapture() == g_panel) ReleaseCapture();
    g_swallow_vkey = 0;
    ShowWindow(g_panel, SW_HIDE);
}

/* A key while Map is armed: 1 if Map took it. Waiting for a target, a key is
 * not typing; waiting for a binding, the key is the binding (not F10-F12 or
 * the app's own Ctrl+Shift chords, which stay the main window's). */
int coco_controllers_map_key(UINT msg, WPARAM wpar, LPARAM lpar)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;

    if (!g_panel) return 0;
    if (g_swallow_vkey && wpar == g_swallow_vkey) {
        if (!down) g_swallow_vkey = 0;
        return 1;
    }
    if (g_map_state == -2) return 0;
    if (wpar == VK_F10 || wpar == VK_F11 || wpar == VK_F12) return 0;
    if (msg == WM_SYSKEYDOWN && wpar == VK_F4) return 0;
    if (wpar >= 'A' && wpar <= 'Z' && (GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_SHIFT) & 0x8000))
        return 0;
    if (!down || (lpar & (1 << 30))) return 1;
    if (wpar == VK_ESCAPE) {
        set_map_state(g_map_state >= 0 ? -1 : -2);
        g_swallow_vkey = wpar;
        return 1;
    }
    if (g_map_state >= 0) {
        char stolen[128], name[32];
        const int target = g_map_state;
        unsigned keysym;
        uint32_t unicode, canon;
        if (!coco_key_identity(wpar, lpar, &keysym, &unicode))
            return 1;                   /* nothing to call it: keep waiting */
        canon = cocosession_bindable_key(keysym, unicode);
        if (!canon) return 1;           /* a bare modifier, F10-F12: keep waiting */
        stolen[0] = '\0';
        cocosession_binding_set_key(g_session, target, canon, stolen, sizeof stolen);
        cocosession_keysym_name(canon, name, sizeof name);
        g_swallow_vkey = wpar;
        set_map_state(-1);              /* stay armed: remapping several in a row is normal */
        bound_hint(name, stolen);
    }
    return 1;
}

/* A key with the window focused: 1 if taken. Every key belongs to the CoCo,
 * so the main window deals with them as if it had the focus. */
static int panel_key(UINT msg, WPARAM wpar, LPARAM lpar)
{
    if (coco_controllers_map_key(msg, wpar, lpar)) return 1;
    if (msg == WM_SYSKEYDOWN && wpar == VK_F4) return 0;     /* Alt+F4 closes this window */
    if (g_parent) SendMessageA(g_parent, msg, wpar, lpar);
    return 1;
}

/* ---- the mouse ------------------------------------------------------------------------ */

/* The joystick part under a client point, or -1. */
static int part_at(POINT pnt)
{
    if (!g_ctl) return -1;
    return cocosession_controls_hit(g_built_machine, (float)((pnt.x - g_joy_x) / g_joy_unit),
                                    (float)((pnt.y - g_joy_y) / g_joy_unit));
}

/* The key cap under a client point, or -1. */
static int cap_at(POINT pnt)
{
    int idx;
    if (!g_kb_on) return -1;
    for (idx = g_ncap - 1; idx >= 0; idx--) {
        RECT rect;
        cap_rect(&g_cap[idx], &rect);
        if (PtInRect(&rect, pnt)) return idx;
    }
    return -1;
}

/* A stick drag: where the pointer is from the gate's centre is the position
 * (a square range: a corner is a full diagonal). */
static void stick_follow(int port, POINT pnt)
{
    int xaxis, yaxis;
    cocosession_stick_from_vector((float)(pnt.x - g_gate[port].cenx), (float)(pnt.y - g_gate[port].ceny),
                                  (float)(KNOB_TRAVEL * g_joy_unit), &xaxis, &yaxis);
    set_stick(port, xaxis, yaxis);
    refresh_held();
}

static void action_button(int btn)
{
    switch (btn) {
    case SBT_MAP:
        set_map_state(g_map_state == -2 ? -1 : -2);
        break;
    case SBT_CLEAR:
        if (g_map_state >= 0) {
            char msg[200];
            const int target = g_map_state;
            cocosession_binding_set_key(g_session, target, 0, NULL, 0);
            cocosession_binding_set_button(g_session, target, COCO_PAD_BTN_NONE, NULL, 0);
            set_map_state(-1);
            snprintf(msg, sizeof msg, "Cleared %s", coco_target_name(target));
            set_hint(msg);
        } else {
            set_hint("Map, click a control, then Clear unbinds it.");
        }
        break;
    case SBT_DEFAULTS:
        cocosession_bindings_reset(g_session);
        release_everything();
        set_hint("Every key and button is back to its default");
        InvalidateRect(g_panel, NULL, FALSE);
        break;
    case SBT_SHOW:
        g_show_bindings = !g_show_bindings;
        cocosession_set_int(g_session, "controllers_show_bindings", g_show_bindings);
        InvalidateRect(g_panel, NULL, FALSE);
        break;
    case SBT_HELP:
        coco_help_for(g_panel);
        break;
    case SBT_KEYBOARD:
        mouse_release(0);
        cocosession_keyboard_release(g_session);
        g_kb_on = !g_kb_on;
        cocosession_set_int(g_session, "controllers_keyboard", g_kb_on);
        memset(g_shown.caps, 0, sizeof g_shown.caps);
        layout_static();
        fit_client();
        InvalidateRect(g_panel, NULL, FALSE);
        break;
    case SBT_HOLD:
        g_hold_stick = !g_hold_stick;
        cocosession_set_int(g_session, "joystick_hold", g_hold_stick);
        if (!g_hold_stick) { mouse_release(0); centre_sticks(); }
        InvalidateRect(g_panel, &g_btn_rc[SBT_HOLD], FALSE);
        break;
    default:
        break;
    }
}

/* Whether a strip button is a toggle or a plain action pressed on release
 * (everything but Stop in Debugger, which waits for the release over it). */
static void mouse_down(HWND hwnd, POINT pnt, int button)
{
    int idx, row, seg, btn;
    SetFocus(hwnd);

    for (btn = 0; btn < SBT_COUNT; btn++) {
        if (!PtInRect(&g_btn_rc[btn], pnt)) continue;
        if (button != 1) return;
        if (btn == SBT_STOP) {
            if (g_map_state != -2) {                 /* armed: picks the action */
                set_map_state(strip_target(btn));
            } else {
                mouse_release(0);
                g_hold.kind = HOLD_ACTION;
                g_hold.action = btn;
                g_hold.mouse = 1;
                SetCapture(hwnd);
                InvalidateRect(hwnd, &g_btn_rc[btn], FALSE);
            }
            return;
        }
        action_button(btn);
        return;
    }
    for (row = 0; row < PAD_ROWS && row < cocosession_gamepad_count(g_session); row++)
        for (seg = 0; seg < 3; seg++) {
            RECT rect;
            segment_rect(row, seg, &rect);
            if (PtInRect(&rect, pnt) && button == 1) {
                /* 0 = automatic (connection order), 1-2 = that joystick */
                cocosession_gamepad_assign(g_session, row, seg - 1);
                refresh_pads();
                InvalidateRect(hwnd, &g_gp_rc, FALSE);
                return;
            }
        }

    if (g_map_state == -2 && button == 1) {
        idx = cap_at(pnt);
        if (idx >= 0) {
            mouse_release(0);
            g_hold.kind = HOLD_CAP;
            g_hold.part = idx;
            g_hold.mouse = 1;
            cocosession_keyboard_press(g_session, idx, 1);
            SetCapture(hwnd);
            refresh_held();
            return;
        }
    }

    idx = part_at(pnt);
    if (idx < 0) return;
    {
        const coco_control *ctl = &g_ctl[idx];
        if (g_map_state != -2) {
            /* armed: picks (or changes) the target */
            if (button == 1 && ctl->target >= 0 && ctl->kind != COCO_CTL_STICK) set_map_state(ctl->target);
            return;
        }
        mouse_release(0);
        if (button != 1) {
            /* right: this joystick's button 1, middle: its button 2 */
            if (ctl->port < 0) return;
            g_hold.kind = HOLD_PRESS;
            g_hold.port = ctl->port;
            g_hold.target = COCO_TARGET_PORT(ctl->port, button == 2 ? COCO_ACT_BUTTON1 : COCO_ACT_BUTTON2);
            g_hold.part = -1;
            g_hold.mouse = button;
            cocosession_press(g_session, g_hold.target, 1);
        } else {
            switch (ctl->kind) {
            case COCO_CTL_STICK:
            case COCO_CTL_DIRECTION:
                g_hold.kind = HOLD_STICK;
                g_hold.port = ctl->port;
                g_hold.mouse = 1;
                stick_follow(ctl->port, pnt);
                break;
            case COCO_CTL_BUTTON:
            case COCO_CTL_RESET:
                g_hold.kind = HOLD_PRESS;
                g_hold.port = ctl->port;
                g_hold.target = ctl->target;
                g_hold.part = idx;
                g_hold.mouse = 1;
                if (ctl->kind == COCO_CTL_BUTTON) cocosession_press(g_session, ctl->target, 1);
                break;
            default:
                return;
            }
        }
        invalidate_part(idx);
        SetCapture(hwnd);
        refresh_held();
    }
}

static void mouse_move(HWND hwnd, POINT pnt)
{
    switch (g_hold.kind) {
    case HOLD_STICK:
        stick_follow(g_hold.port, pnt);
        break;
    case HOLD_PRESS:
        /* dragged off what it holds: let go, as a button does */
        if (g_hold.part >= 0 && (g_hold.part >= g_nctl || part_at(pnt) != g_hold.part)) {
            mouse_release(0);
            if (GetCapture() == hwnd) ReleaseCapture();
            refresh_held();
        }
        break;
    case HOLD_CAP:
        if (cap_at(pnt) != g_hold.part) {
            mouse_release(0);
            if (GetCapture() == hwnd) ReleaseCapture();
            refresh_held();
        }
        break;
    case HOLD_ACTION:
        if (!PtInRect(&g_btn_rc[g_hold.action], pnt)) {
            mouse_release(0);
            if (GetCapture() == hwnd) ReleaseCapture();
        }
        break;
    default:
        break;
    }
}

static void mouse_up(HWND hwnd, int button)
{
    if (g_hold.kind != HOLD_NONE && button == g_hold.mouse) {
        mouse_release(1);
        refresh_held();
    }
    if (g_hold.kind == HOLD_NONE && GetCapture() == hwnd) ReleaseCapture();
}

/* ---- the window ----------------------------------------------------------------------- */

static LRESULT CALLBACK panel_proc(HWND hwnd, UINT msg, WPARAM wpar, LPARAM lpar)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC hdc = BeginPaint(hwnd, &paint);
        /* Off-screen first: the held highlights repaint at 25 Hz. */
        RECT inval = paint.rcPaint;
        HDC mem;
        HBITMAP bmp;
        HGDIOBJ oldbmp, oldfont;
        const int wide = inval.right - inval.left, high = inval.bottom - inval.top;
        if (wide <= 0 || high <= 0) { EndPaint(hwnd, &paint); return 0; }
        mem = CreateCompatibleDC(hdc);
        bmp = CreateCompatibleBitmap(hdc, wide, high);
        oldbmp = SelectObject(mem, bmp);
        oldfont = SelectObject(mem, g_font);
        SetViewportOrgEx(mem, -inval.left, -inval.top, NULL);
        paint_rect(mem, &inval);
        SetViewportOrgEx(mem, 0, 0, NULL);
        BitBlt(hdc, inval.left, inval.top, wide, high, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldfont);
        SelectObject(mem, oldbmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: {
        POINT pnt;
        pnt.x = GET_X_LPARAM(lpar);
        pnt.y = GET_Y_LPARAM(lpar);
        mouse_down(hwnd, pnt, msg == WM_LBUTTONDOWN ? 1 : msg == WM_RBUTTONDOWN ? 2 : 3);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pnt;
        pnt.x = GET_X_LPARAM(lpar);
        pnt.y = GET_Y_LPARAM(lpar);
        mouse_move(hwnd, pnt);
        return 0;
    }
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
        /* released wherever the mouse ended up: capture means this arrives
         * even if the pointer left the part */
        mouse_up(hwnd, msg == WM_LBUTTONUP ? 1 : msg == WM_RBUTTONUP ? 2 : 3);
        return 0;
    case WM_CAPTURECHANGED:
        /* lost the mouse some other way (Alt+Tab mid-press): let go */
        if ((HWND)lpar != hwnd) {
            mouse_release(0);
            refresh_held();
        }
        return 0;

    case WM_TIMER:
        if (wpar == IDT_CONTROLLERS_CAPTURE) {
            int button;
            if (g_map_state < 0) {
                KillTimer(hwnd, IDT_CONTROLLERS_CAPTURE);
            } else if (cocosession_gamepad_capture_poll(g_session, &button)) {
                char stolen[128];
                const int target = g_map_state;
                stolen[0] = '\0';
                cocosession_binding_set_button(g_session, target, button, stolen, sizeof stolen);
                set_map_state(-1);   /* stay armed: remapping several in a row is normal */
                bound_hint(coco_pad_button_name(button), stolen);
            }
        } else if (wpar == IDT_CONTROLLERS_HELD) {
            refresh_held();
        } else if (wpar == IDT_CONTROLLERS_PADS) {
            refresh_pads();
        }
        return 0;

    /* WM_SYSKEY* too: Alt is a CoCo 3 key */
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
        if (panel_key(msg, wpar, lpar)) return 0;
        break;
    case WM_CHAR: case WM_SYSCHAR:
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(wpar) == WA_INACTIVE) {
            const HWND other = (HWND)lpar;
            mouse_release(0);
            if (GetCapture() == hwnd) ReleaseCapture();
            g_swallow_vkey = 0;     /* its release goes elsewhere */
            /* Leaving the app: nothing typed here stays held (within it, the
             * window that gets the key-up lets go). The latched modifiers and
             * the stick stay as the user left them. */
            if (!other || GetWindowThreadProcessId(other, NULL) != GetCurrentThreadId())
                cocosession_release_all(g_session);
            refresh_held();
        }
        break;                      /* DefWindowProc gives the window the focus */
    case WM_SHOWWINDOW:
        if (wpar) {
            refresh_held();
            SetTimer(hwnd, IDT_CONTROLLERS_HELD, 40, NULL);
            SetTimer(hwnd, IDT_CONTROLLERS_PADS, 250, NULL);
        } else {
            KillTimer(hwnd, IDT_CONTROLLERS_HELD);
            KillTimer(hwnd, IDT_CONTROLLERS_PADS);
        }
        break;
    case WM_GETMINMAXINFO:
        /* Fixed: the size limits say so too, so neither maximizing nor a
         * compositor (Wine passes them on) makes it any other size. */
        if (g_total_w) {
            MINMAXINFO *info = (MINMAXINFO *)lpar;
            SIZE size;
            window_size(hwnd, &size);
            info->ptMinTrackSize.x = info->ptMaxTrackSize.x = info->ptMaxSize.x = size.cx;
            info->ptMinTrackSize.y = info->ptMaxTrackSize.y = info->ptMaxSize.y = size.cy;
            return 0;
        }
        break;
    case WM_DPICHANGED: {
        const RECT *rect = (const RECT *)lpar;
        rescale(HIWORD(wpar));
        SetWindowPos(hwnd, NULL, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        fit_client();
        return 0;
    }
    case WM_CLOSE:
        /* Hide, do not destroy: a remap and the position survive closing it. */
        hide_panel();
        return 0;
    case WM_DESTROY:
        /* with the main window: the next toggle starts afresh */
        set_map_state(-2);
        release_everything();
        g_panel = NULL;
        g_built_machine = -1;
        free_fonts();
        break;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wpar, lpar);
}

static int create_panel(void)
{
    static int registered;
    HINSTANCE inst = GetModuleHandleA(NULL);
    RECT want;

    {   /* "controllers_show_bindings"; the older spellings are read when it is absent */
        int saved = cocosession_get_int(g_session, "controllers_show_bindings", -1);
        if (saved < 0) saved = cocosession_get_int(g_session, "controllers_bindings", -1);
        if (saved < 0) saved = cocosession_get_int(g_session, "controllers_key_names", -1);
        if (saved < 0) saved = cocosession_get_int(g_session, "show_bindings", -1);
        g_show_bindings = saved > 0;
    }
    g_kb_on = cocosession_get_int(g_session, "controllers_keyboard", 0) != 0;
    g_hold_stick = cocosession_get_int(g_session, "joystick_hold", 0) != 0;

    if (!registered) {
        WNDCLASSEXA wcls;
        memset(&wcls, 0, sizeof wcls);
        wcls.cbSize = sizeof wcls;
        wcls.lpfnWndProc = panel_proc;    /* no CS_DBLCLKS: fast clicks stay press/release pairs */
        wcls.hInstance = inst;
        wcls.hCursor = LoadCursor(NULL, IDC_ARROW);
        wcls.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wcls.lpszClassName = PANEL_CLASS;
        registered = RegisterClassExA(&wcls) != 0;
    }
    if (!g_accent_brush)
        g_accent_brush = CreateSolidBrush(rgb_of(COCOSESSION_ACCENT_RGB));
    g_dpi = coco_window_dpi(g_parent);
    g_built_machine = machine_now();
    make_fonts();
    layout_static();

    SetRect(&want, 0, 0, g_total_w, g_total_h);
    /* A tool window with a caption and no thick frame or maximize box: it is
     * exactly the size of its contents, and stays off the taskbar. */
    AdjustWindowRectEx(&want, PANEL_STYLE, FALSE, PANEL_EXSTYLE);
    g_panel = CreateWindowExA(PANEL_EXSTYLE, PANEL_CLASS, "Controllers", PANEL_STYLE,
                              CW_USEDEFAULT, CW_USEDEFAULT, want.right - want.left, want.bottom - want.top,
                              g_parent, NULL, inst, NULL);
    if (!g_panel) return 0;
    coco_help_set_context(g_panel, "controllers", 0);    /* F1 is a CoCo 3 key here */
    rescale(coco_window_dpi(g_panel));    /* placed on another monitor than the main window */
    fit_client();
    memset(&g_pads_shown, 0, sizeof g_pads_shown);
    memset(&g_shown, 0, sizeof g_shown);
    g_map_state = -2;
    set_hint(IDLE_HINT);
    return 1;
}

static void show_panel(void)
{
    if (g_built_machine != machine_now())
        rebuild();
    refresh_pads();
    ShowWindow(g_panel, SW_SHOW);
    SetForegroundWindow(g_panel);
}

void coco_controllers_window_toggle(HWND parent, cocosession *session)
{
    g_session = session;
    if (parent)
        g_parent = parent;
    if (!g_panel && !create_panel())
        return;
    if (IsWindowVisible(g_panel))
        hide_panel();
    else
        show_panel();
}

void coco_controllers_window_show(HWND parent, cocosession *session)
{
    g_session = session;
    if (parent)
        g_parent = parent;
    if (!g_panel && !create_panel())
        return;
    if (IsWindowVisible(g_panel))
        SetForegroundWindow(g_panel);
    else
        show_panel();
}

int coco_controllers_window_visible(void)
{
    return g_panel && IsWindowVisible(g_panel);
}

void coco_controllers_window_refresh(void)
{
    if (!g_panel)
        return;
    if (IsWindowVisible(g_panel)) {
        refresh_pads();             /* rebuilds if the machine changed */
        InvalidateRect(g_panel, NULL, FALSE);
    } else {
        g_built_machine = -1;       /* rebuilt when next shown */
    }
}
