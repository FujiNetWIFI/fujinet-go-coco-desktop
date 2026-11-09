/*
 * The Win32 Input window (Preferences > Input in the other frontends): the
 * Gamepads switch (applied by restarting the session when this window
 * closes), the Analog joystick option (live), one row per connected gamepad
 * with its Automatic / Right joystick / Left joystick choice (rebuilt as pads
 * come and go), Reset Controls (every key and pad button back to its default)
 * and Open Controllers. Keys and buttons are mapped in the Controllers window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "input_prefs.h"

#include <stdio.h>
#include <string.h>

#include "controllers/controllers_window.h"
#include "help/help_window.h"
#include "resource.h"

#define INPUT_CLASS "CocoInputWindow"
#define MAX_PADS    8       /* the core's limit */
#define IDT_INPUT_PADS 1

static HWND g_win, g_parent_win, g_gamepad, g_analog, g_note, g_empty;
static HWND g_name[MAX_PADS], g_combo[MAX_PADS];
static cocosession *g_session;
static coco_input_closed_fn g_closed;
static int g_dpi = 96;
static int g_gamepad_at_open;
static unsigned g_seen_gen;
static int g_seen_count, g_seen_assign[MAX_PADS], g_seen_eff[MAX_PADS];

#define SCL(val) MulDiv((val), g_dpi, 96)

static void set_text_utf8(HWND hwnd, const char *utf8)
{
    wchar_t *wide = coco_wide_dup(utf8);
    if (wide) { SetWindowTextW(hwnd, wide); free(wide); }
}

/* One row per gamepad: its name and the joystick it drives now, and the
 * choice. A combo that is open is left alone. */
static void refresh_pads(int force)
{
    int count, assign[MAX_PADS], eff[MAX_PADS], idx;
    unsigned gen;

    if (!g_win || !g_session) return;
    count = cocosession_gamepad_count(g_session) < MAX_PADS ? cocosession_gamepad_count(g_session) : MAX_PADS;
    gen = cocosession_gamepad_generation(g_session);
    for (idx = 0; idx < count; idx++) {
        assign[idx] = cocosession_gamepad_assignment(g_session, idx);
        eff[idx] = cocosession_gamepad_effective_port(g_session, idx);
    }
    if (!force && gen == g_seen_gen && count == g_seen_count &&
        memcmp(assign, g_seen_assign, sizeof(int) * (size_t)count) == 0 &&
        memcmp(eff, g_seen_eff, sizeof(int) * (size_t)count) == 0)
        return;
    g_seen_gen = gen;
    g_seen_count = count;
    memcpy(g_seen_assign, assign, sizeof(int) * (size_t)count);
    memcpy(g_seen_eff, eff, sizeof(int) * (size_t)count);

    ShowWindow(g_empty, count == 0 ? SW_SHOW : SW_HIDE);
    for (idx = 0; idx < MAX_PADS; idx++) {
        if (idx >= count) {
            ShowWindow(g_name[idx], SW_HIDE);
            ShowWindow(g_combo[idx], SW_HIDE);
            continue;
        }
        {
            char name[128], line[200];
            static const char *const drives[3] = { "no joystick", "right joystick", "left joystick" };
            cocosession_gamepad_name(g_session, idx, name, sizeof name);
            snprintf(line, sizeof line, "%s (%s)", name,
                     drives[eff[idx] >= 0 && eff[idx] < COCOSESSION_JOYSTICK_PORTS ? eff[idx] + 1 : 0]);
            set_text_utf8(g_name[idx], line);
        }
        if (!SendMessageA(g_combo[idx], CB_GETDROPPEDSTATE, 0, 0))
            SendMessageA(g_combo[idx], CB_SETCURSEL, (WPARAM)(assign[idx] + 1), 0);
        ShowWindow(g_name[idx], SW_SHOW);
        ShowWindow(g_combo[idx], SW_SHOW);
    }
}

static LRESULT CALLBACK input_proc(HWND hwnd, UINT msg, WPARAM wpar, LPARAM lpar)
{
    switch (msg) {
    case WM_COMMAND: {
        const unsigned ctl = LOWORD(wpar);
        if (ctl == IDC_INPUT_GAMEPAD) {
            const int enabled = SendMessageA(g_gamepad, BM_GETCHECK, 0, 0) == BST_CHECKED;
            cocosession_set_int(g_session, "enable_gamepad", enabled);
        } else if (ctl == IDC_INPUT_ANALOG) {
            cocosession_set_analog(g_session, SendMessageA(g_analog, BM_GETCHECK, 0, 0) == BST_CHECKED);
        } else if (ctl == IDC_INPUT_RESET) {
            cocosession_bindings_reset(g_session);
            SetWindowTextA(g_note, "Every key and pad button is back to its default.");
        } else if (ctl == IDC_INPUT_OPEN) {
            coco_controllers_window_show(g_parent_win, g_session);
        } else if (ctl == IDC_INPUT_HELP) {
            coco_help_for(hwnd);
        } else if (ctl == IDC_INPUT_CLOSE || ctl == IDCANCEL) {
            DestroyWindow(hwnd);
        } else if (ctl >= IDC_INPUT_PAD_BASE && ctl < IDC_INPUT_PAD_BASE + MAX_PADS && HIWORD(wpar) == CBN_SELCHANGE) {
            const int row = (int)(ctl - IDC_INPUT_PAD_BASE);
            const int sel = (int)SendMessageA(g_combo[row], CB_GETCURSEL, 0, 0);
            if (sel >= 0) cocosession_gamepad_assign(g_session, row, sel - 1);
            refresh_pads(1);
        }
        return 0;
    }
    case WM_TIMER:
        if (wpar == IDT_INPUT_PADS) refresh_pads(0);
        return 0;
    case WM_DESTROY: {
        const int restart = cocosession_get_int(g_session, "enable_gamepad", 1) != g_gamepad_at_open;
        KillTimer(hwnd, IDT_INPUT_PADS);
        g_win = NULL;
        if (restart && g_closed) g_closed(1);
        return 0;
    }
    }
    return DefWindowProcA(hwnd, msg, wpar, lpar);
}

static HWND child(const char *cls, const char *text, DWORD style, int ctlid, HINSTANCE inst)
{
    HWND hwnd = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1, g_win,
                                (HMENU)(INT_PTR)ctlid, inst, NULL);
    SendMessageA(hwnd, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return hwnd;
}

void coco_input_prefs_show(HWND parent, cocosession *session, coco_input_closed_fn closed)
{
    static int registered;
    HINSTANCE inst = GetModuleHandleA(NULL);
    RECT want = { 0, 0, 0, 0 };
    HDC hdc;
    int idx, posy;
    const int marg = 16;
    static const char *const choices[3] = { "Automatic", "Right joystick", "Left joystick" };

    if (g_win) {
        SetForegroundWindow(g_win);
        return;
    }
    g_session = session;
    g_parent_win = parent;
    g_closed = closed;
    if (!registered) {
        WNDCLASSA wcls;
        memset(&wcls, 0, sizeof wcls);
        wcls.lpfnWndProc = input_proc;
        wcls.hInstance = inst;
        wcls.hCursor = LoadCursor(NULL, IDC_ARROW);
        wcls.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wcls.lpszClassName = INPUT_CLASS;
        wcls.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(IDI_APPICON));
        RegisterClassA(&wcls);
        registered = 1;
    }
    hdc = GetDC(NULL);
    g_dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(NULL, hdc);

    /* fixed size, sized from the client at this DPI */
    want.right = SCL(480);
    want.bottom = SCL(396);
    AdjustWindowRect(&want, WS_CAPTION | WS_SYSMENU, FALSE);
    g_win = CreateWindowExA(WS_EX_DLGMODALFRAME, INPUT_CLASS, "Input", WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, want.right - want.left, want.bottom - want.top, parent,
                            NULL, inst, NULL);
    if (!g_win) return;

    g_gamepad_at_open = cocosession_get_int(session, "enable_gamepad", 1) != 0;
    posy = SCL(12);
    g_gamepad = child("BUTTON", "Gamepads (applied when this window closes)", WS_TABSTOP | BS_AUTOCHECKBOX,
                      IDC_INPUT_GAMEPAD, inst);
    SendMessageA(g_gamepad, BM_SETCHECK, g_gamepad_at_open ? BST_CHECKED : BST_UNCHECKED, 0);
    MoveWindow(g_gamepad, SCL(marg), posy, SCL(440), SCL(22), TRUE);
    posy += SCL(28);
    g_analog = child("BUTTON", "Analog joystick (a pad's left stick moves the joystick in proportion)",
                     WS_TABSTOP | BS_AUTOCHECKBOX, IDC_INPUT_ANALOG, inst);
    SendMessageA(g_analog, BM_SETCHECK,
                 cocosession_get_int(session, "analog_joystick", 1) ? BST_CHECKED : BST_UNCHECKED, 0);
    MoveWindow(g_analog, SCL(marg), posy, SCL(448), SCL(22), TRUE);
    posy += SCL(36);
    MoveWindow(child("STATIC", "Each gamepad drives the right joystick, the left one, or the next free one:",
                     SS_LEFT | SS_NOPREFIX, 0, inst),
               SCL(marg), posy, SCL(448), SCL(18), TRUE);
    posy += SCL(24);
    g_empty = child("STATIC", "No gamepads connected", SS_LEFT | SS_NOPREFIX, 0, inst);
    MoveWindow(g_empty, SCL(marg), posy + SCL(4), SCL(300), SCL(18), TRUE);
    for (idx = 0; idx < MAX_PADS; idx++) {
        int sub;
        g_name[idx] = child("STATIC", "", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 0, inst);
        MoveWindow(g_name[idx], SCL(marg), posy + SCL(4), SCL(290), SCL(18), TRUE);
        g_combo[idx] = child("COMBOBOX", "", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                             IDC_INPUT_PAD_BASE + idx, inst);
        MoveWindow(g_combo[idx], SCL(marg + 298), posy, SCL(150), SCL(120), TRUE);
        for (sub = 0; sub < 3; sub++)
            SendMessageA(g_combo[idx], CB_ADDSTRING, 0, (LPARAM)choices[sub]);
        posy += SCL(26);
    }
    posy += SCL(6);
    g_note = child("STATIC", "Keys and buttons are mapped in the Controllers window (Ctrl+Shift+K).",
                   SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 0, inst);
    MoveWindow(g_note, SCL(marg), posy, SCL(448), SCL(18), TRUE);
    posy += SCL(26);
    MoveWindow(child("BUTTON", "Reset Controls", WS_TABSTOP, IDC_INPUT_RESET, inst), SCL(marg), posy, SCL(120),
               SCL(28), TRUE);
    MoveWindow(child("BUTTON", "Open Controllers", WS_TABSTOP, IDC_INPUT_OPEN, inst), SCL(marg + 128), posy,
               SCL(130), SCL(28), TRUE);
    MoveWindow(child("BUTTON", "Help", WS_TABSTOP, IDC_INPUT_HELP, inst), SCL(480 - marg - 80 - 8 - 80), posy,
               SCL(80), SCL(28), TRUE);
    MoveWindow(child("BUTTON", "Close", WS_TABSTOP | BS_DEFPUSHBUTTON, IDC_INPUT_CLOSE, inst),
               SCL(480 - marg - 80), posy, SCL(80), SCL(28), TRUE);
    coco_help_set_context(g_win, "prefs/Input", 1);

    memset(g_seen_assign, 0, sizeof g_seen_assign);
    memset(g_seen_eff, 0, sizeof g_seen_eff);
    refresh_pads(1);
    SetTimer(g_win, IDT_INPUT_PADS, 250, NULL);
    ShowWindow(g_win, SW_SHOW);
}

void coco_input_prefs_gamepads_changed(void)
{
    refresh_pads(0);
}

int coco_input_prefs_pretranslate(MSG *msg)
{
    if (!g_win || !msg->hwnd || (msg->hwnd != g_win && !IsChild(g_win, msg->hwnd))) return 0;
    /* F1 and the Ctrl shortcuts (Ctrl+Shift+H, Help) are the app's, not a dialog's */
    if ((msg->message == WM_KEYDOWN || msg->message == WM_SYSKEYDOWN) &&
        (msg->wParam == VK_F1 || GetKeyState(VK_CONTROL) < 0))
        return 0;
    return IsDialogMessageA(g_win, msg) ? 1 : 0;
}
