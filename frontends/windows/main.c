/*
 * FujiNet Go CoCo -- native Win32 frontend.
 *
 * Mirrors the GNOME/KDE frontends over the shared cocosession API: a
 * GDI-blitted display letterboxed to the chosen aspect, a present thread
 * that feeds the session's vsync phase-lock from DwmFlush (the Windows
 * analog of the GTK frame clock / Qt's own timer), full keyboard mapping,
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
 * XRGB8888 pixels are already GDI's BI_RGB 32bpp byte order on a
 * little-endian host, so there is no RGB565->BGRA conversion loop to
 * carry over; keyboard input is forwarded directly via cocosession_key
 * (raw keysym/unicode per keystroke) rather than ADAM's single-byte
 * pre-translated model, so it is rebuilt rather than ported; CoCo has no
 * equivalent of ADAM's Ctrl+digit ColecoVision keypad trick, so that is
 * dropped rather than translated.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cocosession.h"
#include "debugger/dbg_window.h"
#include "resource.h"

static cocosession *g_session;
static HWND g_hwnd;
static HMENU g_menu;
static HWND g_log_window;
static HWND g_log_edit;

/* Frame hand-off: the present thread copies the latest frame under this
 * lock (cocosession_copy_frame writes straight into g_frame -- no
 * conversion, see the file comment); WM_PAINT blits it. Allocated at the
 * session's maximum since the actual size changes at runtime with the
 * video mode. */
static CRITICAL_SECTION g_fb_lock;
static uint32_t g_frame[COCOSESSION_FB_MAX_WIDTH * COCOSESSION_FB_MAX_HEIGHT];
static int g_fb_w, g_fb_h;
static uint64_t g_serial;

static pthread_t g_present_thread;
static volatile int g_present_run;
static int g_aspect_mode;
static int g_smooth;
static int g_fullscreen;
static WINDOWPLACEMENT g_prev_placement = {sizeof(g_prev_placement)};

static int64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* ---- present thread: vsync pacing + frame hand-off ------------------------ */

static void *present_main(void *arg)
{
    (void)arg;
    while (g_present_run) {
        BOOL composited = FALSE;
        DwmIsCompositionEnabled(&composited);
        if (composited && SUCCEEDED(DwmFlush())) {
            /* DwmFlush returned at the compositor's vsync. */
        } else {
            Sleep(16); /* no DWM composition (RDP, safe mode): wall clock */
        }
        cocosession_notify_vsync(g_session, monotonic_ns());

        EnterCriticalSection(&g_fb_lock);
        {
            int w = 0, h = 0;
            int got = cocosession_copy_frame(g_session, g_frame, &g_serial,
                                             &w, &h);
            if (got) {
                g_fb_w = w;
                g_fb_h = h;
            }
            LeaveCriticalSection(&g_fb_lock);
            if (got)
                InvalidateRect(g_hwnd, NULL, FALSE);
        }
    }
    return NULL;
}

/* ---- display --------------------------------------------------------------
 * dest_rect and paint both read g_fb_w/g_fb_h fresh each time rather than
 * caching them, since the video mode -- and so the frame's own aspect --
 * can change at runtime (a CoCo 3 GIME mode is wider than a CoCo 1/2 VDG
 * one). */

static RECT dest_rect(int cw, int ch, int fb_w, int fb_h)
{
    double dw, dh;
    RECT r;
    if (fb_w <= 0 || fb_h <= 0) {
        r.left = r.top = r.right = r.bottom = 0;
        return r;
    }
    if (g_aspect_mode == 2) { /* integer scale */
        int scale = (cw / fb_w < ch / fb_h) ? cw / fb_w : ch / fb_h;
        if (scale < 1) scale = 1;
        dw = scale * fb_w;
        dh = scale * fb_h;
    } else {
        /* 0 = TV 4:3 (the default), 1 = square pixels (the frame's own
         * aspect) -- matching the GNOME frontend's own convention
         * (frontends/gnome/display.h's CocoAspectMode; "aspect_mode" is
         * just a plain settings int, not an enum cocosession.h itself
         * defines, so nothing enforces this across frontends except doing
         * it the same way everywhere), not a fixed "0 = native" the way
         * ADAM's own aspect_mode happens to read. */
        double aspect =
            g_aspect_mode == 0 ? 4.0 / 3.0 : (double)fb_w / fb_h;
        if ((double)cw / ch > aspect) {
            dh = ch;
            dw = ch * aspect;
        } else {
            dw = cw;
            dh = cw / aspect;
        }
    }
    r.left = (LONG)((cw - dw) / 2);
    r.top = (LONG)((ch - dh) / 2);
    r.right = r.left + (LONG)dw;
    r.bottom = r.top + (LONG)dh;
    return r;
}

static void paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT client;
    BITMAPINFO bmi;
    RECT d;
    int fb_w, fb_h;

    GetClientRect(hwnd, &client);
    FillRect(hdc, &client, (HBRUSH)GetStockObject(BLACK_BRUSH));

    EnterCriticalSection(&g_fb_lock);
    fb_w = g_fb_w;
    fb_h = g_fb_h;
    if (fb_w > 0 && fb_h > 0) {
        memset(&bmi, 0, sizeof(bmi));
        bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth = fb_w;
        bmi.bmiHeader.biHeight = -fb_h; /* top-down */
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        d = dest_rect(client.right, client.bottom, fb_w, fb_h);
        SetStretchBltMode(hdc, g_smooth ? HALFTONE : COLORONCOLOR);
        StretchDIBits(hdc, d.left, d.top, d.right - d.left, d.bottom - d.top,
                      0, 0, fb_w, fb_h, g_frame, &bmi, DIB_RGB_COLORS,
                      SRCCOPY);
    }
    LeaveCriticalSection(&g_fb_lock);

    EndPaint(hwnd, &ps);
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

static void restart_session(void)
{
    cocosession_start_opts opts;
    cocosession_stop(g_session);
    cocosession_default_opts(g_session, &opts);
    if (cocosession_start(g_session, &opts) != 0)
        MessageBoxA(g_hwnd, cocosession_last_error(g_session),
                    "FujiNet Go CoCo", MB_ICONWARNING);
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

/* OFN_ALLOWMULTISELECT packs the result as "dir\0file1\0file2\0...\0\0" when
 * more than one file was picked, or a single full path with no trailing
 * NUL-separated tail when only one was. */
static void import_roms(HWND hwnd)
{
    char buf[8192], dest[1024];
    const char *dir, *name;
    unsigned ok = 0, total = 0;
    char msg[256];

    if (!open_file(hwnd,
                   "CoCo system ROMs (*.rom;*.ROM;*.bin)\0*.rom;*.ROM;*.bin\0",
                   OFN_ALLOWMULTISELECT | OFN_EXPLORER, buf, sizeof(buf)))
        return;

    dir = buf;
    name = buf + strlen(dir) + 1;
    if (*name == '\0') {
        /* Single selection: buf is the whole path already. */
        total = 1;
        if (cocosession_import_rom(g_session, dir, dest, sizeof(dest)) == 0)
            ok++;
    } else {
        char path[MAX_PATH];
        while (*name) {
            size_t n = strlen(name);
            total++;
            _snprintf(path, sizeof(path), "%s\\%s", dir, name);
            path[sizeof(path) - 1] = '\0';
            if (cocosession_import_rom(g_session, path, dest, sizeof(dest)) == 0)
                ok++;
            name += n + 1;
        }
    }

    if (ok == 0) {
        MessageBoxA(hwnd, cocosession_last_error(g_session),
                    "FujiNet Go CoCo", MB_ICONWARNING);
        return;
    }
    /* XRoar resolves ROMs when the machine is built, so this needs a
     * restart -- an in-place machine switch would reuse the loaded images. */
    restart_session();
    _snprintf(msg, sizeof(msg), "Imported %u of %u ROM file(s) (session restarted)",
              ok, total);
    msg[sizeof(msg) - 1] = '\0';
    MessageBoxA(hwnd, msg, "FujiNet Go CoCo", MB_ICONINFORMATION);
}

static void show_fujinet_config(void)
{
    ShellExecuteA(NULL, "open", cocosession_fujinet_webui_url(g_session),
                 NULL, NULL, SW_SHOWNORMAL);
}

/* ---- console log window ----------------------------------------------------- */

#define LOG_TIMER_ID 1

static LRESULT CALLBACK log_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        MoveWindow(g_log_edit, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
        return 0;
    case WM_TIMER: {
        static char buf[128 * 1024];
        int n = cocosession_fujinet_copy_log(g_session, buf, sizeof(buf));
        SetWindowTextA(g_log_edit, n > 0 ? buf : "(no FujiNet output yet)");
        SendMessageA(g_log_edit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
        SendMessageA(g_log_edit, EM_SCROLLCARET, 0, 0);
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
    SetTimer(g_log_window, LOG_TIMER_ID, 1000, NULL);
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
    AppendMenuA(media, MF_STRING, IDM_IMPORT_ROMS, "Import System ROMs...");
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
}

/* ---- settings window ---------------------------------------------------------
 * Aspect ratio and smooth scaling apply live; FujiNet/Sound/Gamepads decide
 * what the session starts with, so -- like GNOME's Preferences dialog
 * (frontends/gnome/prefs.c), same keys and defaults, so a machine
 * configured in one frontend shows up the same in another -- they are
 * applied by restarting the session when this window closes. Machine/TV
 * input/Artifact Colour/CPU are not duplicated here: they already live in
 * the main menu as live radio groups. */

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
                InvalidateRect(g_hwnd, NULL, FALSE);
            }
            return 0;
        case IDC_SET_SMOOTH: {
            int on = SendMessageA(GetDlgItem(hwnd, IDC_SET_SMOOTH),
                                  BM_GETCHECK, 0, 0) == BST_CHECKED;
            cocosession_set_int(g_session, "smooth_scaling", on);
            apply_display_settings();
            InvalidateRect(g_hwnd, NULL, FALSE);
            return 0;
        }
        case IDC_SET_FUJINET:
            settings_apply_checkbox(hwnd, IDC_SET_FUJINET, "enable_fujinet", 1);
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
        CW_USEDEFAULT, CW_USEDEFAULT, 380, 300, NULL, NULL, inst, NULL);

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
    case WM_PAINT:
        paint(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1; /* paint() clears; avoid flicker */
    case WM_SIZE:
        /* A resize only invalidates the newly exposed strips, and BeginPaint
         * clips the repaint to them -- the rest of the client area would keep
         * the image blitted at the old size (visible as a ghost after
         * maximize or F11). Ask for the whole client area instead. */
        InvalidateRect(hwnd, NULL, FALSE);
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
        case IDM_IMPORT_ROMS:    import_roms(hwnd); break;
        case IDM_FUJINET_CONFIG: show_fujinet_config(); break;
        case IDM_FUJINET_LOG:
            show_fujinet_log((HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE));
            break;
        case IDM_FULLSCREEN: toggle_fullscreen(hwnd); break;
        case IDM_DEBUGGER:   open_debugger(); break;
        case IDM_ABOUT:
            MessageBoxA(hwnd,
                        "FujiNet Go CoCo " COCO_VERSION_STRING "\n"
                        "Self-contained Tandy Color Computer with built-in "
                        "FujiNet.\n"
                        "Copyright (C) 2026 Thomas Cherryhomes\n"
                        "GPL-3.0-or-later",
                        "About FujiNet Go CoCo", MB_ICONINFORMATION);
            break;
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
    cocosession_start_opts opts;
    (void)prev;
    (void)cmd;

    SetProcessDPIAware();
    InitializeCriticalSection(&g_fb_lock);

    g_session = cocosession_new(NULL);
    if (!g_session) {
        MessageBoxA(NULL, "Could not create the session.", "FujiNet Go CoCo",
                    MB_ICONERROR);
        return 1;
    }
    cocosession_default_opts(g_session, &opts);
    if (cocosession_start(g_session, &opts) != 0)
        MessageBoxA(NULL, cocosession_last_error(g_session), "FujiNet Go CoCo",
                    MB_ICONWARNING);
    apply_display_settings();

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    /* Redraw the whole client area on any size change; the display is
     * stretched to fit, so a partial repaint leaves stale pixels. */
    wc.style = CS_HREDRAW | CS_VREDRAW;
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
    ShowWindow(g_hwnd, show);

    g_present_run = 1;
    pthread_create(&g_present_thread, NULL, present_main, NULL);

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

    g_present_run = 0;
    pthread_join(g_present_thread, NULL);
    cocosession_free(g_session);
    DeleteCriticalSection(&g_fb_lock);
    return (int)msg.wParam;
}
