/*
 * Debugger window (Win32): disassembly with click-to-toggle breakpoints,
 * registers, memory view and the decoded video pane, over the shared
 * cocodebug engine -- the same content as the GNOME/KDE debuggers, built
 * from plain common controls.
 *
 * Ported from fujinet-go-adam-desktop's Windows debugger, not copied
 * line-for-line -- cocodebug.h's shape differs from adamdebug.h in ways
 * that matter here:
 *
 *   * No stop callback. adamdebug fires one on the emulator thread,
 *     marshaled to the UI thread with a custom WM_DBG_STOPPED message.
 *     cocodebug is poll-only (cocodebug_stop_serial()), because XRoar's
 *     instruction-hook context makes a cross-thread callback into a
 *     toolkit unsafe (see cocodebug.h's own comment) -- so this polls on
 *     its WM_TIMER instead, the same pattern GNOME's dbg_window.c uses.
 *   * No register or memory *editing*: cocodebug_regs_get has no _set
 *     counterpart and cocodebug_read has no _write one. Registers and
 *     memory are display-only here too, matching GNOME.
 *   * No VDP, no instruction trace -- neither exists for a CoCo. In their
 *     place: a single decoded screen image (cocodebug_video_decode, one
 *     pane, dynamic size, no tab control needed at all) and a symbol
 *     table (cocodebug_symbol_at/find/load/clear) with its own Load
 *     Symbols.../Clear Symbols toolbar buttons -- adamdebug has no
 *     equivalent UI for this to copy from, since its symbol tables load
 *     automatically with no frontend affordance.
 *   * coco_dasm_line carries no embedded symbol pointer; a match is a
 *     separate cocodebug_symbol_at() call per line, appended as a
 *     "   ; NAME" suffix (GNOME's rendering convention) rather than a
 *     label line of its own.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include <commctrl.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cocodebug.h"
#include "cocovideo.h"

#define DISASM_LINES 40
#define MEM_ROWS 16
#define MEM_COLS 16

#define WM_DBG_ACCEPT (WM_APP + 1) /* wp: control id -- Enter in an edit */
#define TIMER_REFRESH 1

enum {
    IDC_RUN = 1000, IDC_STEP, IDC_STEP_OVER, IDC_STEP_OUT, IDC_CLEAR_BP,
    IDC_LOAD_SYM, IDC_CLEAR_SYM, IDC_STATUS,
    IDC_DISASM, IDC_REGS, IDC_VIDEO,
    IDC_MEM_ADDR, IDC_MEM_GO, IDC_MEM_VIEW,
};

typedef struct {
    const uint32_t *pixels; /* points at d->video_buf; NULL when nothing decoded */
    int w, h;
} pixel_view;

typedef struct {
    HWND hwnd;
    HWND run_btn, step_btn, step_over_btn, step_out_btn, clear_bp_btn;
    HWND load_sym_btn, clear_sym_btn, status;
    HWND disasm, regs, video;
    HWND mem_addr, mem_go, mem_view;

    HFONT mono;
    HACCEL accel;

    cocosession *session;
    cocodebug *dbg;

    uint16_t mem_base;
    uint64_t seen_serial;
    int was_paused;

    uint32_t *video_buf; /* COCOVIDEO_MAX_WIDTH * MAX_HEIGHT, heap */
    pixel_view video_view;
} debugger;

static debugger *g_dbg;

/* ---- small helpers --------------------------------------------------------- */

static void set_text(HWND h, const char *text)
{
    SetWindowTextA(h, text);
}

static void edit_text(HWND h, char *out, int outsz)
{
    GetWindowTextA(h, out, outsz);
}

/* Hex (optionally $-prefixed) or a symbol name. */
static int parse_addr(debugger *d, const char *text, uint16_t *out)
{
    char buf[128];
    char *p = buf;
    char *end;
    unsigned long v;
    uint16_t addr;

    strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '$')
        p++;
    if (*p) {
        v = strtoul(p, &end, 16);
        while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
            end++;
        if (*end == '\0' && v <= 0xFFFF) {
            *out = (uint16_t)v;
            return 1;
        }
    }
    if (cocodebug_symbol_find(d->dbg, text, &addr)) {
        *out = addr;
        return 1;
    }
    return 0;
}

/* ---- refreshers ------------------------------------------------------------- */

static void refresh_disasm(debugger *d)
{
    coco_dasm_line lines[DISASM_LINES];
    cocodebug_regs r;
    char *text;
    size_t cap = DISASM_LINES * 160;
    size_t len = 0;
    int i, n;

    if (!cocodebug_regs_get(d->dbg, &r))
        return;

    text = (char *)malloc(cap);
    if (!text)
        return;
    text[0] = '\0';

    n = cocodebug_disassemble(d->dbg, r.pc, lines, DISASM_LINES);
    for (i = 0; i < n; i++) {
        char bytes[16] = "";
        char symname[32];
        int b;
        for (b = 0; b < lines[i].len; b++)
            snprintf(bytes + b * 2, 3, "%02X", lines[i].bytes[b]);
        len += (size_t)snprintf(
            text + len, cap - len, "%c%c %04X  %-10s %s",
            lines[i].addr == r.pc ? '>' : ' ',
            cocodebug_breakpoint_is_set(d->dbg, lines[i].addr) ? '*' : ' ',
            lines[i].addr, bytes, lines[i].text);
        if (cocodebug_symbol_at(d->dbg, lines[i].addr, symname, sizeof(symname)))
            len += (size_t)snprintf(text + len, cap - len, "   ; %s", symname);
        len += (size_t)snprintf(text + len, cap - len, "\r\n");
        if (len + 200 >= cap)
            break;
    }
    set_text(d->disasm, text);
    free(text);
}

static const char *cc_bits(uint8_t cc, char *out)
{
    static const char names[] = "EFHINZVC";
    int i;
    for (i = 0; i < 8; i++)
        out[i] = (cc & (0x80 >> i)) ? names[i] : '-';
    out[8] = '\0';
    return out;
}

static void refresh_regs(debugger *d)
{
    cocodebug_regs r;
    char bits[9];
    char buf[256];

    if (!cocodebug_regs_get(d->dbg, &r))
        return;
    snprintf(buf, sizeof(buf),
            "PC  %04X      A %02X  B %02X\r\n"
            "X   %04X      DP %02X\r\n"
            "Y   %04X\r\n"
            "U   %04X\r\n"
            "S   %04X\r\n"
            "\r\n"
            "CC  %02X  %s",
            r.pc, r.a, r.b, r.x, r.dp, r.y, r.u, r.s, r.cc,
            cc_bits(r.cc, bits));
    set_text(d->regs, buf);
}

static void refresh_mem(debugger *d)
{
    uint8_t data[MEM_ROWS * MEM_COLS];
    char text[MEM_ROWS * 96];
    size_t len = 0;
    int n, row, col;

    n = cocodebug_read(d->dbg, d->mem_base, data, sizeof(data));
    text[0] = '\0';
    if (n <= 0) {
        set_text(d->mem_view, "");
        return;
    }
    for (row = 0; row * MEM_COLS < n; row++) {
        len += (size_t)snprintf(text + len, sizeof(text) - len, "%04X ",
                                (unsigned)(d->mem_base + row * MEM_COLS));
        for (col = 0; col < MEM_COLS; col++) {
            int idx = row * MEM_COLS + col;
            if (idx < n)
                len += (size_t)snprintf(text + len, sizeof(text) - len, " %02X",
                                        data[idx]);
            else
                len += (size_t)snprintf(text + len, sizeof(text) - len, "   ");
        }
        len += (size_t)snprintf(text + len, sizeof(text) - len, "  ");
        for (col = 0; col < MEM_COLS; col++) {
            int idx = row * MEM_COLS + col;
            uint8_t c = (idx < n) ? data[idx] : (uint8_t)' ';
            len += (size_t)snprintf(text + len, sizeof(text) - len, "%c",
                                    (c >= 0x20 && c < 0x7F) ? (char)c : '.');
        }
        len += (size_t)snprintf(text + len, sizeof(text) - len, "\r\n");
    }
    set_text(d->mem_view, text);
}

static void refresh_video(debugger *d)
{
    int w = 0, h = 0;
    if (cocodebug_video_decode(d->dbg, d->video_buf, &w, &h) && w > 0 && h > 0) {
        d->video_view.pixels = d->video_buf;
        d->video_view.w = w;
        d->video_view.h = h;
    } else {
        d->video_view.pixels = NULL;
        d->video_view.w = 0;
        d->video_view.h = 0;
    }
    InvalidateRect(d->video, NULL, FALSE);
}

static void clear_views(debugger *d)
{
    set_text(d->disasm, "");
    set_text(d->regs, "");
    set_text(d->mem_view, "");
    d->video_view.pixels = NULL;
    InvalidateRect(d->video, NULL, FALSE);
}

static void refresh_all(debugger *d, int paused)
{
    set_text(d->run_btn, paused ? "Run" : "Pause");
    set_text(d->status, paused ? "Paused" : "Running");
    if (paused) {
        refresh_disasm(d);
        refresh_regs(d);
        refresh_mem(d);
        refresh_video(d);
    } else {
        /* Everything on show is only valid while the machine is parked, so
         * clear it rather than leave a stale snapshot that looks live. */
        clear_views(d);
    }
}

/* ---- actions ---------------------------------------------------------------- */

static void toggle_run(debugger *d)
{
    if (cocodebug_is_paused(d->dbg))
        cocodebug_resume(d->dbg);
    else
        cocodebug_pause(d->dbg);
}

static void load_symbols(debugger *d)
{
    OPENFILENAMEA ofn;
    char path[MAX_PATH] = "";

    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = d->hwnd;
    ofn.lpstrFilter = "Symbol files (*.sym)\0*.sym\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = sizeof(path);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameA(&ofn))
        return;

    if (cocodebug_symbols_load(d->dbg, path) < 0)
        set_text(d->status, "Could not open symbol file");
    if (cocodebug_is_paused(d->dbg))
        refresh_disasm(d); /* redraw with the new labels */
}

static void on_accept(debugger *d, int id)
{
    char buf[128];
    uint16_t addr;

    if (id == IDC_MEM_ADDR) {
        edit_text(d->mem_addr, buf, sizeof(buf));
        if (parse_addr(d, buf, &addr)) {
            d->mem_base = addr;
            if (cocodebug_is_paused(d->dbg))
                refresh_mem(d);
        }
    }
}

/* ---- video pane (custom child window class) ---------------------------------
 * Same reasoning as the main display (main.c): the core's XRGB8888 is
 * already GDI's BI_RGB 32bpp byte order on a little-endian host, so this
 * blits pv->pixels straight through with no BGRA conversion. */

static LRESULT CALLBACK pixels_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    pixel_view *pv = (pixel_view *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT c;
        BITMAPINFO bmi;

        GetClientRect(hwnd, &c);
        if (pv && pv->pixels && pv->w > 0 && pv->h > 0) {
            memset(&bmi, 0, sizeof(bmi));
            bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
            bmi.bmiHeader.biWidth = pv->w;
            bmi.bmiHeader.biHeight = -pv->h; /* top-down */
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(hdc, COLORONCOLOR); /* nearest: keep pixels crisp */
            FillRect(hdc, &c, (HBRUSH)GetStockObject(BLACK_BRUSH));
            StretchDIBits(hdc, 0, 0, c.right, c.bottom, 0, 0, pv->w, pv->h,
                         pv->pixels, &bmi, DIB_RGB_COLORS, SRCCOPY);
        } else {
            FillRect(hdc, &c, (HBRUSH)GetStockObject(BLACK_BRUSH));
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- edit subclassing --------------------------------------------------------
 * Enter in the memory-address field applies it; a click on a disassembly
 * line toggles the breakpoint there (address sits at columns 3-6, matching
 * refresh_disasm's "%c%c %04X  ..." format). */

static WNDPROC g_edit_proc;
static WNDPROC g_disasm_proc;

static LRESULT CALLBACK edit_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        PostMessageA(GetParent(hwnd), WM_DBG_ACCEPT,
                    (WPARAM)GetWindowLongPtrA(hwnd, GWLP_ID), 0);
        return 0;
    }
    if (msg == WM_CHAR && wp == VK_RETURN)
        return 0;
    return CallWindowProcA(g_edit_proc, hwnd, msg, wp, lp);
}

static LRESULT CALLBACK disasm_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_LBUTTONDOWN && g_dbg && cocodebug_is_paused(g_dbg->dbg)) {
        LRESULT pos = SendMessageA(hwnd, EM_CHARFROMPOS, 0, lp);
        int line = HIWORD(pos);
        char buf[256];
        unsigned addr;
        int n;

        *(WORD *)buf = (WORD)(sizeof(buf) - 1);
        n = (int)SendMessageA(hwnd, EM_GETLINE, (WPARAM)line, (LPARAM)buf);
        if (n >= 7) {
            buf[n] = '\0';
            if (sscanf(buf + 3, "%4X", &addr) == 1 && addr <= 0xFFFF) {
                cocodebug_breakpoint_toggle(g_dbg->dbg, (uint16_t)addr);
                refresh_disasm(g_dbg);
            }
        }
    }
    return CallWindowProcA(g_disasm_proc, hwnd, msg, wp, lp);
}

/* ---- construction ------------------------------------------------------------ */

static HWND child(debugger *d, const char *cls, const char *text, DWORD style,
                  int id)
{
    HWND h = CreateWindowExA(
        0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, d->hwnd,
        (HMENU)(INT_PTR)id, (HINSTANCE)GetWindowLongPtrA(d->hwnd, GWLP_HINSTANCE),
        NULL);
    SendMessageA(h, WM_SETFONT, (WPARAM)d->mono, TRUE);
    return h;
}

static HWND mono_view(debugger *d, int id)
{
    return child(d, "EDIT", "",
                WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
                    ES_READONLY | ES_AUTOVSCROLL,
                id);
}

static HWND field(debugger *d, int id)
{
    HWND h = child(d, "EDIT", "", WS_BORDER | ES_AUTOHSCROLL, id);
    g_edit_proc = (WNDPROC)SetWindowLongPtrA(h, GWLP_WNDPROC,
                                             (LONG_PTR)edit_subclass);
    return h;
}

static void build_controls(debugger *d)
{
    d->run_btn = child(d, "BUTTON", "Pause (F5)", BS_PUSHBUTTON, IDC_RUN);
    d->step_btn = child(d, "BUTTON", "Step (F7)", BS_PUSHBUTTON, IDC_STEP);
    d->step_over_btn = child(d, "BUTTON", "Step Over (F8)", BS_PUSHBUTTON,
                             IDC_STEP_OVER);
    d->step_out_btn = child(d, "BUTTON", "Step Out (Shift+F8)", BS_PUSHBUTTON,
                            IDC_STEP_OUT);
    d->clear_bp_btn = child(d, "BUTTON", "Clear Breakpoints", BS_PUSHBUTTON,
                            IDC_CLEAR_BP);
    d->load_sym_btn = child(d, "BUTTON", "Load Symbols...", BS_PUSHBUTTON,
                            IDC_LOAD_SYM);
    d->clear_sym_btn = child(d, "BUTTON", "Clear Symbols", BS_PUSHBUTTON,
                             IDC_CLEAR_SYM);
    d->status = child(d, "STATIC", "Running", SS_LEFT, IDC_STATUS);

    d->disasm = mono_view(d, IDC_DISASM);
    g_disasm_proc = (WNDPROC)SetWindowLongPtrA(d->disasm, GWLP_WNDPROC,
                                               (LONG_PTR)disasm_subclass);
    d->regs = mono_view(d, IDC_REGS);
    d->video = child(d, "CocoDbgPixels", "", 0, IDC_VIDEO);
    SetWindowLongPtrA(d->video, GWLP_USERDATA, (LONG_PTR)&d->video_view);

    d->mem_addr = field(d, IDC_MEM_ADDR);
    d->mem_go = child(d, "BUTTON", "Go", BS_PUSHBUTTON, IDC_MEM_GO);
    d->mem_view = mono_view(d, IDC_MEM_VIEW);
}

static void layout(debugger *d)
{
    RECT client;
    int y = 8, bx = 8;
    int top_h, mid_top, mid_h, left_w, right_x, right_w;
    int mem_row_y;

    GetClientRect(d->hwnd, &client);

    MoveWindow(d->run_btn, bx, y, 110, 26, TRUE);        bx += 116;
    MoveWindow(d->step_btn, bx, y, 100, 26, TRUE);       bx += 106;
    MoveWindow(d->step_over_btn, bx, y, 130, 26, TRUE);  bx += 136;
    MoveWindow(d->step_out_btn, bx, y, 150, 26, TRUE);   bx += 156;
    MoveWindow(d->clear_bp_btn, bx, y, 140, 26, TRUE);   bx += 146;
    MoveWindow(d->load_sym_btn, bx, y, 120, 26, TRUE);   bx += 126;
    MoveWindow(d->clear_sym_btn, bx, y, 110, 26, TRUE);  bx += 116;
    MoveWindow(d->status, bx, y + 5, client.right - bx - 8, 20, TRUE);
    top_h = y + 26 + 10;

    /* Middle area: disassembly on the left, registers + video stacked on
     * the right. */
    mid_top = top_h;
    mem_row_y = client.bottom - 8 - 22 - 4 - 160; /* address row + mem view */
    mid_h = mem_row_y - mid_top - 8;
    if (mid_h < 100) mid_h = 100;

    left_w = (client.right - 16) * 55 / 100;
    MoveWindow(d->disasm, 8, mid_top, left_w, mid_h, TRUE);

    right_x = 8 + left_w + 8;
    right_w = client.right - right_x - 8;
    MoveWindow(d->regs, right_x, mid_top, right_w, 130, TRUE);
    MoveWindow(d->video, right_x, mid_top + 138, right_w, mid_h - 138, TRUE);

    MoveWindow(d->mem_addr, 8, mem_row_y, 120, 22, TRUE);
    MoveWindow(d->mem_go, 134, mem_row_y, 60, 24, TRUE);
    MoveWindow(d->mem_view, 8, mem_row_y + 28, client.right - 16,
              client.bottom - (mem_row_y + 28) - 8, TRUE);
}

/* ---- window proc --------------------------------------------------------------- */

static LRESULT CALLBACK dbg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;

    if (!d || d->hwnd != hwnd)
        return DefWindowProcA(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_SIZE:
        layout(d);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_REFRESH) {
            uint64_t serial = cocodebug_stop_serial(d->dbg);
            int paused = cocodebug_is_paused(d->dbg);
            if (serial != d->seen_serial || paused != d->was_paused) {
                d->seen_serial = serial;
                d->was_paused = paused;
                refresh_all(d, paused);
            }
        }
        return 0;
    case WM_DBG_ACCEPT:
        on_accept(d, (int)wp);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_RUN:        toggle_run(d); return 0;
        case IDC_STEP:       cocodebug_step(d->dbg); return 0;
        case IDC_STEP_OVER:  cocodebug_step_over(d->dbg); return 0;
        case IDC_STEP_OUT:   cocodebug_step_out(d->dbg); return 0;
        case IDC_CLEAR_BP:
            cocodebug_breakpoint_clear_all(d->dbg);
            if (cocodebug_is_paused(d->dbg))
                refresh_disasm(d);
            return 0;
        case IDC_LOAD_SYM:   load_symbols(d); return 0;
        case IDC_CLEAR_SYM:
            cocodebug_symbols_clear(d->dbg);
            if (cocodebug_is_paused(d->dbg))
                refresh_disasm(d);
            return 0;
        case IDC_MEM_GO:     on_accept(d, IDC_MEM_ADDR); return 0;
        default: break;
        }
        break;
    case WM_CLOSE:
        /* Keep state (F12 brings it back instantly) but stop paying the
         * per-instruction hook cost -- unlike a naive port of the "hide,
         * don't destroy" trick, this must happen HERE, not deferred to
         * WM_DESTROY (which, with this same hide-not-destroy shape, only
         * ever fires at process exit): leaving the debugger engaged after
         * every close would run the emulator in the slow instruction-
         * hooked path forever with the window merely hidden. */
        cocodebug_set_engaged(d->dbg, 0);
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_REFRESH);
        DeleteObject(d->mono);
        free(d->video_buf);
        free(d);
        g_dbg = NULL;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- entry points ---------------------------------------------------------------- */

void coco_debugger_show(HWND parent, cocosession *session)
{
    INITCOMMONCONTROLSEX icc;
    HINSTANCE inst;
    WNDCLASSA wc;
    debugger *d;
    ACCEL accels[4];

    if (g_dbg) {
        ShowWindow(g_dbg->hwnd, SW_SHOW);
        cocodebug_set_engaged(g_dbg->dbg, 1);
        cocodebug_pause(g_dbg->dbg);
        SetForegroundWindow(g_dbg->hwnd);
        return;
    }

    inst = (HINSTANCE)GetWindowLongPtrA(parent, GWLP_HINSTANCE);

    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = dbg_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "CocoDebuggerWindow";
    RegisterClassA(&wc);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = pixels_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "CocoDbgPixels";
    RegisterClassA(&wc);

    d = (debugger *)calloc(1, sizeof(*d));
    if (!d)
        return;
    d->session = session;
    d->dbg = cocosession_debugger(session);
    if (!d->dbg) {
        free(d);
        MessageBoxA(parent, "The debugger could not be created for this session.",
                    "Debugger Unavailable", MB_ICONERROR);
        return;
    }
    d->video_buf = (uint32_t *)malloc((size_t)COCOVIDEO_MAX_WIDTH *
                                      COCOVIDEO_MAX_HEIGHT * sizeof(uint32_t));
    if (!d->video_buf) {
        free(d);
        return;
    }
    g_dbg = d;

    d->hwnd = CreateWindowExA(0, "CocoDebuggerWindow", "Debugger",
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              1180, 780, NULL, NULL, inst, NULL);
    if (!d->hwnd) {
        free(d->video_buf);
        free(d);
        g_dbg = NULL;
        return;
    }

    d->mono = CreateFontA(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                          FIXED_PITCH | FF_MODERN, "Consolas");

    build_controls(d);

    /* F5/F7/F8/Shift+F8 work wherever the focus is inside the window,
     * matching the other frontends' shortcuts. */
    accels[0].fVirt = FVIRTKEY;          accels[0].key = VK_F5;
    accels[0].cmd = IDC_RUN;
    accels[1].fVirt = FVIRTKEY;          accels[1].key = VK_F7;
    accels[1].cmd = IDC_STEP;
    accels[2].fVirt = FVIRTKEY;          accels[2].key = VK_F8;
    accels[2].cmd = IDC_STEP_OVER;
    accels[3].fVirt = FVIRTKEY | FSHIFT; accels[3].key = VK_F8;
    accels[3].cmd = IDC_STEP_OUT;
    d->accel = CreateAcceleratorTableA(accels, 4);

    /* Engaging switches the emulator loop to the path that honours
     * breakpoints. Pausing immediately is what a user opening a debugger
     * expects -- otherwise the window opens showing nothing at all. */
    cocodebug_set_engaged(d->dbg, 1);
    cocodebug_pause(d->dbg);
    SetTimer(d->hwnd, TIMER_REFRESH, 100, NULL);

    layout(d);
    d->was_paused = -1; /* force the first WM_TIMER tick to refresh */
    ShowWindow(d->hwnd, SW_SHOW);
}

int coco_debugger_pretranslate(MSG *msg)
{
    if (!g_dbg || !g_dbg->accel)
        return 0;
    if (msg->hwnd != g_dbg->hwnd && !IsChild(g_dbg->hwnd, msg->hwnd))
        return 0;
    return TranslateAcceleratorA(g_dbg->hwnd, g_dbg->accel, msg) ? 1 : 0;
}
