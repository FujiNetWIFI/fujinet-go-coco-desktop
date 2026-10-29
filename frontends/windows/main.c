/*
 * FujiNet Go CoCo -- native Win32 frontend.
 *
 * Mirrors the GNOME/KDE frontends over the shared cocosession API: a
 * Direct3D 11 display (display.c; GDI as the fallback) letterboxed to the
 * chosen aspect, whose render thread feeds the session's vsync phase-lock
 * (the Windows analog of the GTK frame clock / Qt's own timer), a No-BIOS
 * page in the picture's place until the app has the system ROMs, full
 * keyboard mapping,
 * a menu bar (Machine/TV input/Artifact Colour/CPU are live radio groups
 * built from the session's own *_name() tables, exactly like GNOME's
 * radio_menu() helper), media import, and the FujiNet configuration
 * (default browser -- no embedded WebView2 in this first pass) and
 * console-log windows.
 *
 * Gamepads are handled inside the core's SDL thread; there is nothing for
 * this file to do for them, the same as every other frontend.
 *
 * Ported from fujinet-go-adam-desktop's Windows frontend, not copied
 * line-for-line -- see frontends/windows/debugger/dbg_window.c's file
 * comment for the debugger side of what differs. The main differences
 * here: cocosession_copy_frame reports a DYNAMIC frame size (up to
 * COCOSESSION_FB_MAX_WIDTH x MAX_HEIGHT), not a fixed one, and its
 * XRGB8888 pixels need no RGB565->BGRA conversion loop (display.c
 * uploads them as they are); keyboard input is forwarded directly via cocosession_key
 * (raw keysym/unicode per keystroke) rather than ADAM's single-byte
 * pre-translated model, so it is rebuilt rather than ported; CoCo has no
 * equivalent of ADAM's Ctrl+digit ColecoVision keypad trick, so that is
 * dropped rather than translated.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <objbase.h>
#include <shellapi.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cocosession.h"
#include "debugger/dbg_window.h"
#include "display.h"
#include "prefs.h"
#include "resource.h"

static cocosession *g_session;
static HWND g_hwnd;
static HMENU g_menu;
static HWND g_log_window;
static HWND g_log_edit;

static HWND g_display;
static HWND g_nobios, g_bios_title, g_bios_text, g_bios_files, g_bios_folder;
static HFONT g_ui_font, g_title_font;
static HICON g_big_icon;
static int g_dpi = 96;
static int g_aspect_mode;
static int g_smooth;
static int g_fullscreen;
static WINDOWPLACEMENT g_prev_placement = {sizeof(g_prev_placement)};

#define S(v) MulDiv((v), g_dpi, 96)

static void start_machine(void);

/* ---- the No-BIOS page ------------------------------------------------------
 * Shown in the picture's place while the machine is not running for want of
 * its system ROMs: a title, what to do, and the two import buttons, centred
 * in a child window that covers the client. */

enum { IDC_BIOS_FILES = 240, IDC_BIOS_FOLDER };

static const char k_nobios_title[] = "No CoCo BIOS";
static const char k_nobios_text[] =
    "The CoCo needs its system ROMs (BASIC), which are not part of the app. "
    "Import the ones you have -- a file, a zip such as MAME's coco and coco3 "
    "sets, or a folder -- and the CoCo starts.";

static int need_bios(void)
{
    return !cocosession_is_running(g_session) &&
           !cocosession_has_system_roms(g_session, cocosession_machine(g_session));
}

static int text_height(HFONT font, const char *text, int width)
{
    HDC dc = GetDC(g_hwnd);
    HGDIOBJ old = SelectObject(dc, font);
    RECT r = {0, 0, width, 0};
    DrawTextA(dc, text, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_CENTER | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(g_hwnd, dc);
    return r.bottom - r.top;
}

static int button_width(const char *text)
{
    HDC dc = GetDC(g_hwnd);
    HGDIOBJ old = SelectObject(dc, g_ui_font);
    SIZE sz = {0, 0};
    GetTextExtentPoint32A(dc, text, (int)strlen(text), &sz);
    SelectObject(dc, old);
    ReleaseDC(g_hwnd, dc);
    return sz.cx + S(28) < S(80) ? S(80) : sz.cx + S(28);
}

static RECT g_icon_rect;

static void nobios_layout(int aw, int ah)
{
    const int width = aw - S(48) < S(560) ? aw - S(48) : S(560);
    const int icon = S(64), bh = S(32);
    const int title_h = text_height(g_title_font, k_nobios_title, width);
    const int desc_h = text_height(g_ui_font, k_nobios_text, width);
    const int bw1 = button_width("Import BIOS"), bw2 = button_width("Import a Folder");
    const int total = icon + S(16) + title_h + S(10) + desc_h + S(24) + bh;
    const int x0 = (aw - width) / 2, cx = aw / 2;
    int y = (ah - total) / 2;

    if (y < S(8)) y = S(8);
    SetRect(&g_icon_rect, cx - icon / 2, y, cx + icon / 2, y + icon);
    y += icon + S(16);
    MoveWindow(g_bios_title, x0, y, width, title_h, TRUE);
    y += title_h + S(10);
    MoveWindow(g_bios_text, x0, y, width, desc_h, TRUE);
    y += desc_h + S(24);
    MoveWindow(g_bios_files, cx - (bw1 + S(12) + bw2) / 2, y, bw1, bh, TRUE);
    MoveWindow(g_bios_folder, cx - (bw1 + S(12) + bw2) / 2 + bw1 + S(12), y, bw2, bh,
               TRUE);
    InvalidateRect(g_nobios, NULL, TRUE);
}

static LRESULT CALLBACK nobios_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        nobios_layout(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_COMMAND:
        SendMessageA(g_hwnd, WM_COMMAND, LOWORD(wp), 0); /* the buttons */
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        SetTextColor(dc, GetSysColor((HWND)lp == g_bios_text ? COLOR_GRAYTEXT
                                                             : COLOR_WINDOWTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (g_big_icon)
            DrawIconEx(dc, g_icon_rect.left, g_icon_rect.top, g_big_icon,
                       g_icon_rect.right - g_icon_rect.left,
                       g_icon_rect.bottom - g_icon_rect.top, 0, NULL, DI_NORMAL);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(hwnd, &r);
        FillRect((HDC)wp, &r, GetSysColorBrush(COLOR_WINDOW));
        return 1;
    }
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void make_fonts(void)
{
    NONCLIENTMETRICSA ncm;
    LOGFONTA lf;

    memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
        lf = ncm.lfMessageFont;
    else
        GetObjectA(GetStockObject(DEFAULT_GUI_FONT), sizeof(lf), &lf);
    g_ui_font = CreateFontIndirectA(&lf);
    lf.lfHeight = lf.lfHeight * 8 / 5;
    lf.lfWeight = FW_BOLD;
    g_title_font = CreateFontIndirectA(&lf);
}

static void create_nobios(HINSTANCE inst)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = nobios_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "CocoNoBios";
    RegisterClassA(&wc);

    make_fonts();
    g_big_icon = (HICON)LoadImageA(inst, MAKEINTRESOURCEA(IDI_APPICON), IMAGE_ICON,
                                   S(64), S(64), LR_DEFAULTCOLOR);
    g_nobios = CreateWindowA("CocoNoBios", "", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 1,
                             1, g_hwnd, NULL, inst, NULL);
    g_bios_title = CreateWindowA("STATIC", k_nobios_title,
                                 WS_CHILD | WS_VISIBLE | SS_CENTER | SS_NOPREFIX, 0,
                                 0, 1, 1, g_nobios, NULL, inst, NULL);
    g_bios_text = CreateWindowA("STATIC", k_nobios_text,
                                WS_CHILD | WS_VISIBLE | SS_CENTER | SS_NOPREFIX, 0, 0,
                                1, 1, g_nobios, NULL, inst, NULL);
    g_bios_files = CreateWindowA("BUTTON", "Import BIOS",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 1, 1,
                                 g_nobios, (HMENU)(INT_PTR)IDC_BIOS_FILES, inst, NULL);
    g_bios_folder = CreateWindowA("BUTTON", "Import a Folder",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 1, 1,
                                  g_nobios, (HMENU)(INT_PTR)IDC_BIOS_FOLDER, inst,
                                  NULL);
    SendMessageA(g_bios_title, WM_SETFONT, (WPARAM)g_title_font, TRUE);
    SendMessageA(g_bios_text, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    SendMessageA(g_bios_files, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    SendMessageA(g_bios_folder, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
}

/* ---- display ------------------------------------------------------------- */

/* The picture, or the No-BIOS page, fills the client. */
static void layout_display(void)
{
    RECT rc;
    if (!g_display)
        return;
    GetClientRect(g_hwnd, &rc);
    MoveWindow(g_display, 0, 0, rc.right, rc.bottom, TRUE);
    MoveWindow(g_nobios, 0, 0, rc.right, rc.bottom, TRUE);
}

static void show_page(void)
{
    const int bios = !need_bios();
    ShowWindow(g_display, bios ? SW_SHOW : SW_HIDE);
    ShowWindow(g_nobios, bios ? SW_HIDE : SW_SHOW);
    layout_display();
}

/* ---- keyboard --------------------------------------------------------------
 * cocosession_key wants an X11/xkb keysym (or 0, meaning "look at unicode
 * instead" -- see core/src/input_map.c's coco_key_from_event, which every
 * frontend's translation ultimately feeds). Win32's VK_* codes carry no
 * shift state of their own (VK_A is the same code whether or not shift is
 * held), so rather than reconstruct a shifted symbol with ToUnicode (and
 * have to keep key-down and key-up consistent despite the transient
 * modifier state), non-special keys are resolved straight to their base
 * US-layout character -- 'a', '1', '-', ... -- which is exactly the
 * "unshifted symbol on the same physical key" coco_key_from_event's own
 * printable_scancode()/unshift_us() want, and is stable across down/up by
 * construction. */

static unsigned special_keysym(WPARAM vk)
{
    switch (vk) {
    case VK_RETURN:   return 0xFF0D;
    case VK_ESCAPE:   return 0xFF1B;
    case VK_TAB:      return 0xFF09;
    case VK_BACK:     return 0xFF08;
    case VK_DELETE:   return 0xFFFF;
    case VK_INSERT:   return 0xFF63;
    case VK_HOME:     return 0xFF50;
    case VK_END:      return 0xFF57;
    case VK_PRIOR:    return 0xFF55; /* Page Up */
    case VK_NEXT:     return 0xFF56; /* Page Down */
    case VK_UP:       return 0xFF52;
    case VK_DOWN:     return 0xFF54;
    case VK_LEFT:     return 0xFF51;
    case VK_RIGHT:    return 0xFF53;
    case VK_CAPITAL:  return 0xFFE5;
    case VK_LSHIFT:   return 0xFFE1;
    case VK_RSHIFT:   return 0xFFE2;
    case VK_LCONTROL: return 0xFFE3;
    case VK_RCONTROL: return 0xFFE4;
    case VK_LMENU:    return 0xFFE9;
    case VK_RMENU:    return 0xFFEA;
    case VK_LWIN:     return 0xFFEB;
    case VK_RWIN:     return 0xFFEC;
    case VK_NUMPAD0:  return 0xFFB0;
    case VK_DECIMAL:  return 0xFFAE;
    case VK_DIVIDE:   return 0xFFAF;
    case VK_MULTIPLY: return 0xFFAA;
    case VK_SUBTRACT: return 0xFFAD;
    case VK_ADD:      return 0xFFAB;
    default:
        if (vk >= VK_F1 && vk <= VK_F12)
            return 0xFFBE + (unsigned)(vk - VK_F1);
        if (vk >= VK_NUMPAD1 && vk <= VK_NUMPAD9)
            return 0xFFB1 + (unsigned)(vk - VK_NUMPAD1);
        return 0;
    }
}

/* The base (unshifted) US-layout character for a printable key -- see the
 * file comment above. Matches core/src/input_map.c's printable_scancode()
 * switch one for one. */
static uint32_t base_char(WPARAM vk)
{
    if (vk >= 'A' && vk <= 'Z') return (uint32_t)(vk - 'A' + 'a');
    if (vk >= '0' && vk <= '9') return (uint32_t)vk;
    switch (vk) {
    case VK_SPACE:      return ' ';
    case VK_OEM_MINUS:  return '-';
    case VK_OEM_PLUS:   return '=';
    case VK_OEM_4:      return '[';
    case VK_OEM_6:      return ']';
    case VK_OEM_5:      return '\\';
    case VK_OEM_1:      return ';';
    case VK_OEM_7:      return '\'';
    case VK_OEM_3:      return '`';
    case VK_OEM_COMMA:  return ',';
    case VK_OEM_PERIOD: return '.';
    case VK_OEM_2:      return '/';
    default:            return 0;
    }
}

/* WM_(SYS)KEYDOWN/UP only report the generic VK_SHIFT/CONTROL/MENU; the
 * left/right pair is recovered from the scancode (Shift) or the extended-key
 * bit (Control/Alt), the standard Win32 idiom for this. */
static WPARAM resolve_side(WPARAM vk, LPARAM lp)
{
    UINT scancode;
    int extended;

    switch (vk) {
    case VK_SHIFT:
        scancode = (UINT)((lp >> 16) & 0xFF);
        return MapVirtualKeyA(scancode, MAPVK_VSC_TO_VK_EX);
    case VK_CONTROL:
        extended = (lp & 0x01000000) != 0;
        return extended ? VK_RCONTROL : VK_LCONTROL;
    case VK_MENU:
        extended = (lp & 0x01000000) != 0;
        return extended ? VK_RMENU : VK_LMENU;
    default:
        return vk;
    }
}

static void toggle_fullscreen(HWND hwnd);
static void open_debugger(void);

static void on_key(HWND hwnd, WPARAM vk, LPARAM lp, int down)
{
    WPARAM rvk;
    unsigned keysym;
    uint32_t unicode = 0;
    int ctrl, shift;

    if (down) {
        switch (vk) {
        case VK_F10:
            return; /* let the system open the menu */
        case VK_F11:
            toggle_fullscreen(hwnd);
            return;
        case VK_F12:
            open_debugger();
            return;
        default:
            break;
        }
    } else if (vk == VK_F10 || vk == VK_F11 || vk == VK_F12) {
        return;
    }

    rvk = resolve_side(vk, lp);
    keysym = special_keysym(rvk);
    if (!keysym) {
        unicode = base_char(rvk);
        if (!unicode)
            return; /* not a key the emulated keyboard has */
    }
    ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    cocosession_key(g_session, down, keysym, unicode, ctrl, shift);
}

/* ---- menu actions ---------------------------------------------------------- */

static void start_session(void)
{
    cocosession_start_opts opts;
    cocosession_default_opts(g_session, &opts);
    if (cocosession_start(g_session, &opts) != 0)
        MessageBoxA(g_hwnd, cocosession_last_error(g_session),
                    "FujiNet Go CoCo", MB_ICONWARNING);
}

/* Without the system ROMs there is nothing to start: the No-BIOS page shows. */
static void restart_session(void)
{
    cocosession_stop(g_session);
    if (cocosession_has_system_roms(g_session, cocosession_machine(g_session)))
        start_session();
    show_page();
}

/* After an import: the core rebuilt a running machine in place; one that
 * could not start for want of ROMs starts now. */
static void start_machine(void)
{
    if (!cocosession_is_running(g_session) &&
        cocosession_has_system_roms(g_session, cocosession_machine(g_session)))
        start_session();
    show_page();
}

static int open_file(HWND hwnd, const char *filter, DWORD extra_flags,
                     char *out, DWORD outsz)
{
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    memset(out, 0, outsz);
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = out;
    ofn.nMaxFile = outsz;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | extra_flags;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
}

static void import_media(HWND hwnd)
{
    char src[MAX_PATH], dest[1024];
    if (!open_file(hwnd,
                   "CoCo disk and tape images (*.dsk;*.os9;*.vdk;*.cas;*.wav)\0"
                   "*.dsk;*.os9;*.vdk;*.cas;*.wav\0",
                   0, src, sizeof(src)))
        return;
    if (cocosession_import_media(g_session, src, dest, sizeof(dest)) != 0)
        MessageBoxA(hwnd, cocosession_last_error(g_session),
                    "FujiNet Go CoCo", MB_ICONWARNING);
}

static void show_fujinet_config(void)
{
    if (cocosession_fujinet_external(g_session)) {
        MessageBoxA(g_hwnd, "FujiNet is external: use the web UI of the "
                    "instance this app connects to.", "FujiNet Go CoCo",
                    MB_ICONINFORMATION);
        return;
    }
    ShellExecuteA(NULL, "open", cocosession_fujinet_webui_url(g_session),
                 NULL, NULL, SW_SHOWNORMAL);
}

/* ---- console log window ----------------------------------------------------- */

#define LOG_TIMER_ID 1

static int g_log_first; /* the next fill is the first: go to the bottom */

static LRESULT CALLBACK log_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        MoveWindow(g_log_edit, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
        return 0;
    case WM_TIMER: {
        static char buf[128 * 1024];
        static char shown[128 * 1024];
        int n = cocosession_fujinet_copy_log(g_session, buf, sizeof(buf));
        const char *text = n > 0 ? buf : "(no FujiNet output yet)";
        LRESULT first, lines;
        RECT rc;
        HDC dc;
        int visible = 1, at_end;

        /* Unchanged: leave the control alone (keeps the scroll position and
         * any selection). */
        if (!g_log_first && strcmp(text, shown) == 0)
            return 0;
        snprintf(shown, sizeof(shown), "%s", text);

        /* Follow the tail only while the reader is already at the bottom;
         * otherwise put them back on the line they were reading. The first
         * fill starts at the bottom. */
        first = SendMessageA(g_log_edit, EM_GETFIRSTVISIBLELINE, 0, 0);
        lines = SendMessageA(g_log_edit, EM_GETLINECOUNT, 0, 0);
        GetClientRect(g_log_edit, &rc);
        dc = GetDC(g_log_edit);
        if (dc) {
            HGDIOBJ old = SelectObject(
                dc, (HGDIOBJ)SendMessageA(g_log_edit, WM_GETFONT, 0, 0));
            TEXTMETRICA tm;
            if (GetTextMetricsA(dc, &tm) && tm.tmHeight > 0)
                visible = (rc.bottom - rc.top) / tm.tmHeight;
            SelectObject(dc, old);
            ReleaseDC(g_log_edit, dc);
        }
        at_end = g_log_first || first + (visible > 0 ? visible : 1) >= lines;
        g_log_first = 0;

        SendMessageA(g_log_edit, WM_SETREDRAW, FALSE, 0);
        SetWindowTextA(g_log_edit, text);
        if (at_end) {
            int len = GetWindowTextLengthA(g_log_edit);
            SendMessageA(g_log_edit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
            SendMessageA(g_log_edit, EM_SCROLLCARET, 0, 0);
        } else {
            LRESULT now = SendMessageA(g_log_edit, EM_GETFIRSTVISIBLELINE, 0, 0);
            SendMessageA(g_log_edit, EM_LINESCROLL, 0, (LPARAM)(first - now));
        }
        SendMessageA(g_log_edit, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(g_log_edit, NULL, TRUE);
        return 0;
    }
    case WM_CLOSE:
        KillTimer(hwnd, LOG_TIMER_ID);
        DestroyWindow(hwnd);
        g_log_window = NULL;
        g_log_edit = NULL;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void show_fujinet_log(HINSTANCE inst)
{
    if (cocosession_fujinet_external(g_session)) {
        MessageBoxA(g_hwnd, "FujiNet is external: its console output is in "
                    "the process that is running it.", "FujiNet Go CoCo",
                    MB_ICONINFORMATION);
        return;
    }
    static int registered;
    if (g_log_window) {
        SetForegroundWindow(g_log_window);
        return;
    }
    if (!registered) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = log_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = "CocoLogWindow";
        RegisterClassA(&wc);
        registered = 1;
    }
    g_log_window = CreateWindowA(
        "CocoLogWindow", "FujiNet Console Log", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 820, 560, NULL, NULL, inst, NULL);
    g_log_edit = CreateWindowA(
        "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
            ES_AUTOVSCROLL,
        0, 0, 0, 0, g_log_window, NULL, inst, NULL);
    SendMessageA(g_log_edit, WM_SETFONT, (WPARAM)GetStockObject(ANSI_FIXED_FONT),
                TRUE);
    g_log_first = 1;
    SetTimer(g_log_window, LOG_TIMER_ID, 1000, NULL);
    SendMessageA(g_log_window, WM_TIMER, LOG_TIMER_ID, 0);
    ShowWindow(g_log_window, SW_SHOW);
}

static void open_debugger(void)
{
    coco_debugger_show(g_hwnd, g_session);
}

/* ---- fullscreen -------------------------------------------------------------- */

static void toggle_fullscreen(HWND hwnd)
{
    DWORD style = (DWORD)GetWindowLongPtr(hwnd, GWL_STYLE);
    if (!g_fullscreen) {
        MONITORINFO mi = {sizeof(mi)};
        if (GetWindowPlacement(hwnd, &g_prev_placement) &&
            GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY),
                           &mi)) {
            SetWindowLongPtr(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetMenu(hwnd, NULL);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            g_fullscreen = 1;
        }
    } else {
        SetWindowLongPtr(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        /* Unlike a line-for-line port of this trick, the menu bar is
         * reattached here -- omitting this leaves the window permanently
         * menu-less after the first fullscreen toggle. */
        SetMenu(hwnd, g_menu);
        SetWindowPlacement(hwnd, &g_prev_placement);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = 0;
    }
}

/* ---- menu -------------------------------------------------------------------
 * Machine, TV input, Artifact Colour and CPU are live radio groups built by
 * walking the session's own cocosession_*_name(idx) tables until NULL --
 * the same shape as GNOME's radio_menu() helper -- rather than a fixed menu
 * resource, so a new entry in cocosession.h needs no matching edit here. */

static void append_radio_menu(HMENU parent, const char *title, HMENU sub,
                              const char *(*name_at)(int),
                              int (*supported)(int, int), int ctx,
                              unsigned base, unsigned current)
{
    const char *label;
    int i;

    for (i = 0; (label = name_at(i)) != NULL; i++) {
        if (supported && !supported(ctx, i))
            continue;
        AppendMenuA(sub, MF_STRING, base + (unsigned)i, label);
    }
    if (current < base + IDM_RADIO_SPAN)
        CheckMenuRadioItem(sub, base, base + IDM_RADIO_SPAN - 1,
                           (UINT)current, MF_BYCOMMAND);
    AppendMenuA(parent, MF_POPUP, (UINT_PTR)sub, title);
}

static HMENU build_menu(void)
{
    HMENU bar = CreateMenu();
    HMENU machine = CreatePopupMenu();
    HMENU machine_sub = CreatePopupMenu();
    HMENU media = CreatePopupMenu();
    HMENU fujinet = CreatePopupMenu();
    HMENU view = CreatePopupMenu();
    HMENU display_sub = CreatePopupMenu();
    HMENU ccr_sub = CreatePopupMenu();
    HMENU cpu_sub = CreatePopupMenu();
    HMENU help = CreatePopupMenu();
    int m = cocosession_machine(g_session);

    append_radio_menu(machine, "Machine", machine_sub,
                      cocosession_machine_name, NULL, 0, IDM_MACHINE_BASE,
                      (unsigned)(IDM_MACHINE_BASE + m));
    AppendMenuA(machine, MF_STRING, IDM_RESET, "Reset");
    AppendMenuA(machine, MF_SEPARATOR, 0, NULL);
    AppendMenuA(machine, MF_STRING, IDM_IMPORT_BIOS, "Import &BIOS...");
    AppendMenuA(machine, MF_STRING, IDM_IMPORT_BIOS_DIR,
                "Import BIOS from a &Folder...");
    AppendMenuA(machine, MF_STRING, IDM_ROMS, "&ROMs...");
    AppendMenuA(machine, MF_SEPARATOR, 0, NULL);
    AppendMenuA(machine, MF_STRING, IDM_SETTINGS, "Settings...");
    AppendMenuA(machine, MF_SEPARATOR, 0, NULL);
    AppendMenuA(machine, MF_STRING, IDM_EXIT, "E&xit");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)machine, "&Machine");

    /* Built for the machine running now: switching machines rebuilds the
     * whole menu, which is what removes RGB on a CoCo 2. */
    append_radio_menu(view, "Display", display_sub, cocosession_tv_input_name,
                      cocosession_tv_input_supported, m, IDM_TVINPUT_BASE,
                      (unsigned)(IDM_TVINPUT_BASE + cocosession_tv_input(g_session)));
    append_radio_menu(view, "Artifact Colour", ccr_sub, cocosession_ccr_name,
                      NULL, 0, IDM_CCR_BASE,
                      (unsigned)(IDM_CCR_BASE + cocosession_ccr(g_session)));
    append_radio_menu(view, "CPU", cpu_sub, cocosession_cpu_name, NULL, 0,
                      IDM_CPU_BASE,
                      (unsigned)(IDM_CPU_BASE + cocosession_cpu(g_session)));
    AppendMenuA(view, MF_STRING, IDM_FULLSCREEN, "Fullscreen\tF11");
    AppendMenuA(view, MF_STRING, IDM_DEBUGGER, "Debugger\tF12");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)view, "&View");

    AppendMenuA(media, MF_STRING, IDM_IMPORT_MEDIA, "Import Disk or Tape Image...");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)media, "M&edia");

    AppendMenuA(fujinet, MF_STRING, IDM_FUJINET_CONFIG, "Configuration...");
    AppendMenuA(fujinet, MF_STRING, IDM_FUJINET_LOG, "Console Log...");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)fujinet, "&FujiNet");

    AppendMenuA(help, MF_STRING, IDM_ABOUT, "About FujiNet Go CoCo");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)help, "&Help");
    return bar;
}

/* Rebuilds and re-attaches the menu bar, e.g. after a machine switch (so
 * Display offers exactly the TV inputs the new machine has) or after
 * restoring from fullscreen. Frees the previous HMENU -- Win32 does not do
 * that automatically when SetMenu replaces one. */
static void rebuild_menu(HWND hwnd)
{
    HMENU old = g_menu;
    g_menu = build_menu();
    SetMenu(hwnd, g_menu);
    DrawMenuBar(hwnd);
    if (old)
        DestroyMenu(old);
}

static void apply_display_settings(void)
{
    g_aspect_mode = cocosession_get_int(g_session, "aspect_mode", 0);
    g_smooth = cocosession_get_int(g_session, "smooth_scaling", 0);
    coco_display_set_options(g_aspect_mode, g_smooth);
}

/* ---- settings window ---------------------------------------------------------
 * Aspect ratio and smooth scaling apply live; FujiNet/Sound/Gamepads decide
 * what the session starts with, so -- like GNOME's Preferences dialog
 * (frontends/gnome/prefs.c), same keys and defaults, so a machine
 * configured in one frontend shows up the same in another -- they are
 * applied by restarting the session when this window closes. Machine/TV
 * input/Artifact Colour/CPU are not duplicated here: they already live in
 * the main menu as live radio groups. */

#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501
#endif

static HWND g_settings_window;
static int g_settings_dirty;

static void settings_apply_checkbox(HWND hwnd, int id, const char *key,
                                    int def)
{
    int on = SendMessageA(GetDlgItem(hwnd, id), BM_GETCHECK, 0, 0) ==
            BST_CHECKED;
    if (cocosession_get_int(g_session, key, def) != on) {
        cocosession_set_int(g_session, key, on);
        g_settings_dirty = 1;
    }
}

/* Trims the address field into addr; with the switch on, text that is not
 * host:port is refused (returns 0) and nothing is saved. */
static int settings_save_external(HWND hwnd)
{
    char addr[96], host[64], *start = addr, *end;
    int port, external, changed;

    external = SendDlgItemMessageA(hwnd, IDC_SET_EXTERNAL, BM_GETCHECK, 0,
                                   0) == BST_CHECKED;
    GetDlgItemTextA(hwnd, IDC_SET_ADDR, addr, sizeof(addr));
    while (*start == ' ' || *start == '\t') start++;
    memmove(addr, start, strlen(start) + 1);
    end = addr + strlen(addr);
    while (end > addr && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
    if (external &&
        cocosession_parse_hostport(addr, host, sizeof(host), &port) != 0) {
        MessageBoxA(hwnd, "That is not an address:port.\n\nExample: "
                    "127.0.0.1:65504", "External FujiNet", MB_ICONWARNING);
        SetFocus(GetDlgItem(hwnd, IDC_SET_ADDR));
        return 0;
    }
    changed = cocosession_get_int(g_session, "fujinet_external", 0) !=
                  external ||
              strcmp(cocosession_get_str(g_session, "fujinet_external_addr",
                                         ""), addr) != 0;
    cocosession_set_int(g_session, "fujinet_external", external);
    cocosession_set_str(g_session, "fujinet_external_addr", addr);
    if (changed)
        g_settings_dirty = 1;
    return 1;
}

static LRESULT CALLBACK settings_proc(HWND hwnd, UINT msg, WPARAM wp,
                                      LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_SET_ASPECT:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int sel = (int)SendMessageA(GetDlgItem(hwnd, IDC_SET_ASPECT),
                                            CB_GETCURSEL, 0, 0);
                cocosession_set_int(g_session, "aspect_mode", sel);
                apply_display_settings();
            }
            return 0;
        case IDC_SET_SMOOTH: {
            int on = SendMessageA(GetDlgItem(hwnd, IDC_SET_SMOOTH),
                                  BM_GETCHECK, 0, 0) == BST_CHECKED;
            cocosession_set_int(g_session, "smooth_scaling", on);
            apply_display_settings();
            return 0;
        }
        case IDC_SET_FUJINET:
            settings_apply_checkbox(hwnd, IDC_SET_FUJINET, "enable_fujinet", 1);
            return 0;
        case IDC_SET_EXTERNAL:
            EnableWindow(GetDlgItem(hwnd, IDC_SET_ADDR),
                         SendDlgItemMessageA(hwnd, IDC_SET_EXTERNAL,
                                             BM_GETCHECK, 0, 0) ==
                             BST_CHECKED);
            return 0;
        case IDC_SET_AUDIO:
            settings_apply_checkbox(hwnd, IDC_SET_AUDIO, "enable_audio", 1);
            return 0;
        case IDC_SET_GAMEPAD:
            settings_apply_checkbox(hwnd, IDC_SET_GAMEPAD, "enable_gamepad", 1);
            return 0;
        default:
            break;
        }
        break;
    case WM_CLOSE:
        if (!settings_save_external(hwnd))
            return 0;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_settings_window = NULL;
        if (g_settings_dirty) {
            g_settings_dirty = 0;
            restart_session();
        }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static HWND settings_checkbox(HWND parent, HINSTANCE inst, const char *text,
                              int id, int y, int checked)
{
    HWND h = CreateWindowExA(0, "BUTTON", text,
                             WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 16, y,
                             320, 22, parent, (HMENU)(INT_PTR)id, inst, NULL);
    SendMessageA(h, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageA(h, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT),
                TRUE);
    return h;
}

static void show_settings(HINSTANCE inst)
{
    static const char *const aspect_names[] = {
        "TV (4:3)", "Square pixels", "Integer scale"
    };
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HWND combo, note;
    int y = 16, i;

    if (g_settings_window) {
        SetForegroundWindow(g_settings_window);
        return;
    }
    {
        static int registered;
        if (!registered) {
            WNDCLASSA wc;
            memset(&wc, 0, sizeof(wc));
            wc.lpfnWndProc = settings_proc;
            wc.hInstance = inst;
            wc.hCursor = LoadCursor(NULL, IDC_ARROW);
            wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
            wc.lpszClassName = "CocoSettingsWindow";
            RegisterClassA(&wc);
            registered = 1;
        }
    }
    g_settings_window = CreateWindowA(
        "CocoSettingsWindow", "Settings",
        WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME),
        CW_USEDEFAULT, CW_USEDEFAULT, 380, 370, NULL, NULL, inst, NULL);

    CreateWindowExA(0, "STATIC", "Aspect ratio:", WS_CHILD | WS_VISIBLE, 16,
                    y, 120, 20, g_settings_window, NULL, inst, NULL);
    combo = CreateWindowExA(
        0, "COMBOBOX", "",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 140, y - 2,
        200, 200, g_settings_window, (HMENU)(INT_PTR)IDC_SET_ASPECT, inst,
        NULL);
    for (i = 0; i < (int)(sizeof(aspect_names) / sizeof(aspect_names[0])); i++)
        SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)aspect_names[i]);
    SendMessageA(combo, CB_SETCURSEL,
                (WPARAM)cocosession_get_int(g_session, "aspect_mode", 0), 0);
    SendMessageA(combo, WM_SETFONT, (WPARAM)font, TRUE);
    y += 32;

    settings_checkbox(g_settings_window, inst, "Smooth scaling",
                      IDC_SET_SMOOTH, y,
                      cocosession_get_int(g_session, "smooth_scaling", 0));
    y += 36;

    note = CreateWindowExA(
        0, "STATIC",
        "FujiNet options apply by restarting the session when this window "
        "closes:",
        WS_CHILD | WS_VISIBLE, 16, y, 340, 32, g_settings_window,
        (HMENU)(INT_PTR)IDC_SET_NOTE, inst, NULL);
    SendMessageA(note, WM_SETFONT, (WPARAM)font, TRUE);
    y += 36;

    settings_checkbox(g_settings_window, inst, "FujiNet", IDC_SET_FUJINET, y,
                      cocosession_get_int(g_session, "enable_fujinet", 1));
    y += 28;
    settings_checkbox(g_settings_window, inst, "Use external FujiNet",
                      IDC_SET_EXTERNAL, y,
                      cocosession_get_int(g_session, "fujinet_external", 0));
    y += 28;
    CreateWindowExA(0, "STATIC", "Address:port:", WS_CHILD | WS_VISIBLE, 32,
                    y + 3, 90, 20, g_settings_window, NULL, inst, NULL);
    {
        HWND edit = CreateWindowExA(
            WS_EX_CLIENTEDGE, "EDIT",
            cocosession_get_str(g_session, "fujinet_external_addr", ""),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 126, y, 210,
            24, g_settings_window, (HMENU)(INT_PTR)IDC_SET_ADDR, inst, NULL);
        SendMessageA(edit, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageA(edit, EM_SETCUEBANNER, FALSE, (LPARAM)L"127.0.0.1:65504");
        EnableWindow(edit,
                     cocosession_get_int(g_session, "fujinet_external", 0));
    }
    y += 32;
    settings_checkbox(g_settings_window, inst, "Sound", IDC_SET_AUDIO, y,
                      cocosession_get_int(g_session, "enable_audio", 1));
    y += 28;
    settings_checkbox(g_settings_window, inst, "Gamepads", IDC_SET_GAMEPAD, y,
                      cocosession_get_int(g_session, "enable_gamepad", 1));

    ShowWindow(g_settings_window, SW_SHOW);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1; /* the display child covers the client */
    case WM_SIZE:
        layout_display();
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        on_key(hwnd, wp, lp, 1);
        /* F10 falls through to DefWindowProc so the system opens the menu;
         * everything else is fully handled above. */
        if (wp == VK_F10)
            break;
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        on_key(hwnd, wp, lp, 0);
        if (wp == VK_F10)
            break;
        return 0;
    case WM_COMMAND: {
        unsigned id = LOWORD(wp);
        if (id >= IDM_MACHINE_BASE && id < IDM_MACHINE_BASE + IDM_RADIO_SPAN) {
            cocosession_set_machine(g_session, (int)(id - IDM_MACHINE_BASE));
            rebuild_menu(hwnd);
            start_machine();
        } else if (id >= IDM_TVINPUT_BASE &&
                  id < IDM_TVINPUT_BASE + IDM_RADIO_SPAN) {
            cocosession_set_tv_input(g_session, (int)(id - IDM_TVINPUT_BASE));
            rebuild_menu(hwnd);
        } else if (id >= IDM_CCR_BASE && id < IDM_CCR_BASE + IDM_RADIO_SPAN) {
            cocosession_set_ccr(g_session, (int)(id - IDM_CCR_BASE));
            rebuild_menu(hwnd);
        } else if (id >= IDM_CPU_BASE && id < IDM_CPU_BASE + IDM_RADIO_SPAN) {
            cocosession_set_cpu(g_session, (int)(id - IDM_CPU_BASE));
            rebuild_menu(hwnd);
        } else switch (id) {
        case IDM_RESET:          cocosession_reset(g_session); break;
        case IDM_SETTINGS:
            show_settings((HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE));
            break;
        case IDM_IMPORT_MEDIA:   import_media(hwnd); break;
        case IDM_IMPORT_BIOS:
        case IDC_BIOS_FILES:
            if (coco_import_bios(hwnd, g_session, 0) >= 0)
                start_machine();
            break;
        case IDM_IMPORT_BIOS_DIR:
        case IDC_BIOS_FOLDER:
            if (coco_import_bios(hwnd, g_session, 1) >= 0)
                start_machine();
            break;
        case IDM_ROMS:
            coco_roms_show(hwnd, g_session, start_machine);
            break;
        case IDM_FUJINET_CONFIG: show_fujinet_config(); break;
        case IDM_FUJINET_LOG:
            show_fujinet_log((HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE));
            break;
        case IDM_FULLSCREEN: toggle_fullscreen(hwnd); break;
        case IDM_DEBUGGER:   open_debugger(); break;
        case IDM_ABOUT: {
            /* MB_USERICON with the app's own icon, not a stock Windows one. */
            MSGBOXPARAMSA mb;
            char text[512];
            snprintf(text, sizeof(text),
                     "FujiNet Go CoCo " COCO_VERSION_STRING "\n"
                     "Self-contained Tandy Color Computer with built-in "
                     "FujiNet.\n"
                     "Copyright (C) 2026 Thomas Cherryhomes\n"
                     "GPL-3.0-or-later\n\n"
                     "Display: %s",
                     coco_display_renderer());
            memset(&mb, 0, sizeof(mb));
            mb.cbSize = sizeof(mb);
            mb.hwndOwner = hwnd;
            mb.hInstance = (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE);
            mb.lpszText = text;
            mb.lpszCaption = "About FujiNet Go CoCo";
            mb.dwStyle = MB_OK | MB_USERICON;
            mb.lpszIcon = MAKEINTRESOURCEA(IDI_APPICON);
            MessageBoxIndirectA(&mb);
            break;
        }
        case IDM_EXIT: DestroyWindow(hwnd); break;
        default: break;
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSEXA wc;
    MSG msg;
    (void)prev;
    (void)cmd;

    SetProcessDPIAware();
    {
        HDC dc = GetDC(NULL);
        g_dpi = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(NULL, dc);
    }
    InitCommonControls();

    g_session = cocosession_new(NULL);
    if (!g_session) {
        MessageBoxA(NULL, "Could not create the session.", "FujiNet Go CoCo",
                    MB_ICONERROR);
        return 1;
    }
    /* No system ROMs yet: the window opens on the No-BIOS page instead. */
    if (cocosession_has_system_roms(g_session, cocosession_machine(g_session)))
        start_session();
    apply_display_settings();

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = 0;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "CocoMainWindow";
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(IDI_APPICON));
    wc.hIconSm = wc.hIcon;
    RegisterClassExA(&wc);

    g_menu = build_menu();
    g_hwnd = CreateWindowExA(0, "CocoMainWindow", "FujiNet Go CoCo",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             1120, 840, NULL, g_menu, inst, NULL);
    if (!g_hwnd)
        return 1;
    g_display = coco_display_create(g_hwnd, g_session);
    create_nobios(inst);
    show_page();
    ShowWindow(g_hwnd, show);

    /* Developer affordance: open the debugger alongside the main window,
     * which is what you want when the app dies before you can reach a
     * menu. Matches the other frontends' COCO_OPEN_DEBUGGER convention. */
    if (getenv("COCO_OPEN_DEBUGGER"))
        open_debugger();

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        /* The debugger's F5/F7/F8/Shift+F8 accelerators get first refusal
         * while its window (or one of its fields) has the focus. */
        if (coco_debugger_pretranslate(&msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    coco_display_destroy();
    cocosession_free(g_session);
    return (int)msg.wParam;
}
