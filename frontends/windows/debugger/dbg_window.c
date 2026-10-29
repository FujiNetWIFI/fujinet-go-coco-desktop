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
 *     pane, dynamic size) and a symbol table
 *     (cocodebug_symbol_at/find/load/clear) with its own Load
 *     Symbols.../Clear Symbols toolbar buttons -- adamdebug has no
 *     equivalent UI for this to copy from, since its symbol tables load
 *     automatically with no frontend affordance.
 *   * coco_dasm_line carries no embedded symbol pointer; a match is a
 *     separate cocodebug_symbol_at() call per line, appended as a
 *     "   ; NAME" suffix (GNOME's rendering convention) rather than a
 *     label line of its own.
 *
 * Two tabs, as in the other frontends: all of the above, and FujiNet's
 * DriveWire traffic on the Becker port, refreshed twice a second while it
 * shows. That text is UTF-8 (arrows), so its views are set as wide text.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include <commctrl.h>
#include <commdlg.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cocodebug.h"
#include "cocovideo.h"

#define DISASM_LINES 40
#define MEM_ROWS 16
#define MEM_COLS 16
#define LOCALS_MAX 32

#define WM_DBG_ACCEPT (WM_APP + 1) /* wp: control id -- Enter in an edit */
#define TIMER_REFRESH 1

/* The tabs, in order: the names COCO_DEBUGGER_TAB takes. */
enum { TAB_CPU, TAB_FUJINET, TAB_COUNT };
static const char *const k_tab_names[TAB_COUNT] = { "cpu", "fujinet" };
static const char *const k_tab_labels[TAB_COUNT] = { "CPU", "FujiNet" };

/* Entries the FujiNet tab asks for: all the engine keeps, plus the one in
 * progress. */
#define FUJI_TRACE_MAX 513

enum {
    IDC_RUN = 1000, IDC_STEP, IDC_STEP_OVER, IDC_STEP_OUT, IDC_CLEAR_BP,
    IDC_LOAD_SYM, IDC_CLEAR_SYM, IDC_STATUS,
    IDC_STEP_LINE, IDC_STEP_OVER_LINE, IDC_LOAD_SRC, IDC_CLEAR_SRC,
    IDC_SRC_COMBO, IDC_SRC_VIEW, IDC_LOCALS,
    IDC_DISASM, IDC_REGS, IDC_VIDEO,
    IDC_MEM_ADDR, IDC_MEM_GO, IDC_MEM_VIEW,
    IDC_TABS, IDC_FUJI_TITLE, IDC_FUJI_NOTE, IDC_FUJI_HIDE, IDC_FUJI_CLEAR,
    IDC_FUJI_TEXT, IDC_FUJI_STATE_TITLE, IDC_FUJI_STATE,
};

typedef struct {
    const uint32_t *pixels; /* points at d->video_buf; NULL when nothing decoded */
    int w, h;
} pixel_view;

typedef struct {
    HWND hwnd;
    HWND run_btn, step_btn, step_over_btn, step_out_btn, clear_bp_btn;
    HWND load_sym_btn, clear_sym_btn, status;
    HWND step_line_btn, step_over_line_btn, load_src_btn, clear_src_btn;
    HWND src_combo, src_view, locals;
    HWND disasm, regs, video;
    HWND mem_addr, mem_go, mem_view;
    HWND tabs;
    HWND fuji_title, fuji_note, fuji_hide, fuji_clear, fuji_text;
    HWND state_title, state_text;

    HFONT mono;
    int mono_h;
    HACCEL accel;
    int page;
    int fuji_count;
    uint64_t fuji_last;
    int fuji_ticks;

    cocosession *session;
    cocodebug *dbg;

    uint16_t mem_base;
    uint64_t seen_serial;
    int was_paused;

    /* The source pane's own cache: whichever file's text is currently held
     * in src_cached_lines, so a stop that doesn't change the displayed file
     * doesn't re-read it from disk. src_cached_buf holds the raw bytes (with
     * '\n's turned into NULs); src_cached_lines points into it, one entry
     * per line. */
    char src_cached_path[MAX_PATH];
    char *src_cached_buf;
    char **src_cached_lines;
    int src_cached_nlines;
    int src_displayed_file_id;
    /* Where PC actually is, independent of which file the combo is showing
     * (a user may be browsing a different file to set a breakpoint ahead of
     * time): the '>' marker only appears when these match the displayed
     * file. */
    int pc_file_id, pc_line;

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

/* ---- the FujiNet tab's text views --------------------------------------------- */

/* Engine text (UTF-8, \n) as the wide text, with CR LF, an EDIT shows. */
static wchar_t *wide_text(const char *text)
{
    size_t n = strlen(text), i, o = 0;
    char *buf = (char *)malloc(n * 2 + 1);
    wchar_t *w = NULL;
    int wn;
    if (!buf)
        return NULL;
    for (i = 0; i < n; i++) {
        if (text[i] == '\n')
            buf[o++] = '\r';
        buf[o++] = text[i];
    }
    buf[o] = '\0';
    wn = MultiByteToWideChar(CP_UTF8, 0, buf, -1, NULL, 0);
    if (wn > 0 && (w = (wchar_t *)malloc((size_t)wn * sizeof(*w))) != NULL)
        MultiByteToWideChar(CP_UTF8, 0, buf, -1, w, wn);
    free(buf);
    return w;
}

static void set_label_utf8(HWND h, const char *text)
{
    wchar_t *w = wide_text(text);
    if (!w)
        return;
    SetWindowTextW(h, w);
    free(w);
}

/* Whether a log's reader is at its end (it then follows new lines). */
static int edit_at_end(debugger *d, HWND h)
{
    RECT c;
    int rows;
    const int first = (int)SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0);
    const int count = (int)SendMessageW(h, EM_GETLINECOUNT, 0, 0);
    GetClientRect(h, &c);
    rows = d->mono_h > 0 && c.bottom > d->mono_h ? c.bottom / d->mono_h : 1;
    return first + rows >= count;
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
static void set_text_at(HWND h, const char *text, int follow)
{
    const int first = (int)SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0);
    wchar_t *w = wide_text(text);
    if (!w)
        return;
    SendMessageW(h, WM_SETREDRAW, FALSE, 0);
    SetWindowTextW(h, w);
    if (follow)
        SendMessageW(h, EM_LINESCROLL, 0,
                     SendMessageW(h, EM_GETLINECOUNT, 0, 0));
    else
        SendMessageW(h, EM_LINESCROLL, 0,
                     first - (int)SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0));
    SendMessageW(h, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(h, NULL, TRUE);
    free(w);
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

static void refresh_locals(debugger *d)
{
    cocodebug_local locals[LOCALS_MAX];
    char text[LOCALS_MAX * 96];
    size_t len = 0;
    int n, i;

    if (!cocodebug_source_available(d->dbg)) {
        set_text(d->locals, "");
        return;
    }
    n = cocodebug_locals_get(d->dbg, locals, LOCALS_MAX);
    text[0] = '\0';
    if (n == 0) {
        /* Not necessarily an error: PC may just be before the function's
         * first statement (mid-prologue), where there is nothing correct to
         * show yet. */
        set_text(d->locals, "(no locals in scope)");
        return;
    }
    for (i = 0; i < n; i++) {
        const cocodebug_local *v = &locals[i];
        if (v->has_value)
            len += (size_t)snprintf(text + len, sizeof(text) - len,
                                    "%-12s %-10s %s= %u ($%X)\r\n", v->name,
                                    v->type, v->is_param ? "(param) " : "",
                                    v->value, v->value);
        else
            len += (size_t)snprintf(text + len, sizeof(text) - len,
                                    "%-12s %-10s %s= ?\r\n", v->name, v->type,
                                    v->is_param ? "(param) " : "");
    }
    set_text(d->locals, text);
}

/* Loads (and caches) file_id's text, splitting it in place the way
 * g_strsplit does (GNOME's dbg_window.c): '\n' -> '\0', one pointer per
 * line. A trailing '\r' left by CRLF endings is trimmed too. */
static void load_src_file_cache(debugger *d, int file_id)
{
    const char *path = cocodebug_source_file_path(d->dbg, file_id);
    FILE *f;
    long sz;
    int i, nlines;
    char *p;

    if (path && strncmp(path, d->src_cached_path, sizeof(d->src_cached_path)) == 0)
        return; /* already cached */

    free(d->src_cached_buf);
    free(d->src_cached_lines);
    d->src_cached_buf = NULL;
    d->src_cached_lines = NULL;
    d->src_cached_nlines = 0;
    d->src_cached_path[0] = '\0';
    if (!path)
        return;

    f = fopen(path, "rb");
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) {
        fclose(f);
        return;
    }

    d->src_cached_buf = (char *)malloc((size_t)sz + 1);
    if (!d->src_cached_buf) {
        fclose(f);
        return;
    }
    fread(d->src_cached_buf, 1, (size_t)sz, f);
    d->src_cached_buf[sz] = '\0';
    fclose(f);

    nlines = 1;
    for (p = d->src_cached_buf; *p; p++)
        if (*p == '\n')
            nlines++;
    d->src_cached_lines = (char **)malloc(sizeof(char *) * (size_t)nlines);
    if (!d->src_cached_lines) {
        free(d->src_cached_buf);
        d->src_cached_buf = NULL;
        return;
    }
    d->src_cached_lines[0] = d->src_cached_buf;
    d->src_cached_nlines = 1;
    for (p = d->src_cached_buf; *p; p++) {
        if (*p == '\n') {
            *p = '\0';
            d->src_cached_lines[d->src_cached_nlines++] = p + 1;
        }
    }
    for (i = 0; i < d->src_cached_nlines; i++) {
        size_t len = strlen(d->src_cached_lines[i]);
        if (len > 0 && d->src_cached_lines[i][len - 1] == '\r')
            d->src_cached_lines[i][len - 1] = '\0';
    }
    strncpy(d->src_cached_path, path, sizeof(d->src_cached_path) - 1);
    d->src_cached_path[sizeof(d->src_cached_path) - 1] = '\0';
}

/* Redraws the source list with '>' on the line PC is at (if PC's file is the
 * one showing) and '*' on any line carrying a breakpoint. Re-reading the
 * file (inside load_src_file_cache, which no-ops when already cached) is
 * cheap enough here since this only runs on an actual stop or a manual file
 * switch, never on every WM_TIMER tick -- see dbg_proc's early-out. */
static void populate_src_list(debugger *d, int file_id)
{
    char *text;
    size_t cap, len = 0;
    int i;

    load_src_file_cache(d, file_id);
    d->src_displayed_file_id = file_id;

    if (!d->src_cached_lines) {
        set_text(d->src_view, "");
        return;
    }

    cap = (size_t)d->src_cached_nlines * 140 + 4096;
    text = (char *)malloc(cap);
    if (!text)
        return;
    text[0] = '\0';

    for (i = 0; i < d->src_cached_nlines; i++) {
        int lineno = i + 1;
        int is_pc = (file_id == d->pc_file_id && lineno == d->pc_line);
        int has_bp = cocodebug_source_breakpoint_is_set(d->dbg, file_id, lineno);

        if (len + 300 >= cap) {
            /* The estimate above was short (an unusually long line) --
             * grow rather than truncate silently. */
            char *grown;
            cap *= 2;
            grown = (char *)realloc(text, cap);
            if (!grown)
                break;
            text = grown;
        }
        len += (size_t)snprintf(text + len, cap - len, "%c%c %5d  %s\r\n",
                                is_pc ? '>' : ' ', has_bp ? '*' : ' ', lineno,
                                d->src_cached_lines[i]);
    }
    set_text(d->src_view, text);
    free(text);
}

/* Follows PC's file automatically. If PC is not itself a mapped statement
 * (mid-prologue, or code with no line info), keeps showing whatever file
 * was last selected and just drops the '>' marker rather than guess. */
static void refresh_source(debugger *d)
{
    cocodebug_regs r;
    int fid, line;

    if (!cocodebug_source_available(d->dbg)) {
        set_text(d->src_view, "");
        return;
    }
    if (!cocodebug_regs_get(d->dbg, &r))
        return;

    if (cocodebug_source_line_at(d->dbg, r.pc, &fid, &line)) {
        d->pc_file_id = fid;
        d->pc_line = line;
        if ((int)SendMessageA(d->src_combo, CB_GETCURSEL, 0, 0) != fid) {
            SendMessageA(d->src_combo, CB_SETCURSEL, (WPARAM)fid, 0);
            populate_src_list(d, fid);
        } else {
            populate_src_list(d, d->src_displayed_file_id);
        }
    } else {
        d->pc_file_id = -1;
        d->pc_line = -1;
        if (SendMessageA(d->src_combo, CB_GETCOUNT, 0, 0) > 0)
            populate_src_list(d, d->src_displayed_file_id);
    }
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
    set_text(d->locals, "");
    set_text(d->src_view, "");
    set_text(d->mem_view, "");
    d->video_view.pixels = NULL;
    InvalidateRect(d->video, NULL, FALSE);
}

static void refresh_fujinet(debugger *d, int force)
{
    static coco_fuji_packet p[FUJI_TRACE_MAX];
    static char t[FUJI_TRACE_MAX * 320 + 128];
    char state[1024];
    size_t len = 0;
    const int n = cocodebug_fujinet_trace(d->dbg, p, FUJI_TRACE_MAX);
    const int hide =
        SendMessageA(d->fuji_hide, BM_GETCHECK, 0, 0) == BST_CHECKED;
    int i, follow, shown = 0;

    if (!cocosession_fujinet_running(d->session) &&
        !cocosession_fujinet_external(d->session))
        set_label_utf8(d->fuji_note,
                       "FujiNet is off (Machine \xe2\x96\xb8 Settings).");
    else {
        char note[160];
        snprintf(note, sizeof note, "%s FujiNet on the Becker port %s, %s.",
                 cocosession_fujinet_external(d->session) ? "External"
                                                           : "Built-in",
                 cocosession_becker_endpoint(d->session),
                 cocosession_fujinet_link(d->session) ? "connected"
                                                       : "not connected");
        set_label_utf8(d->fuji_note, note);
    }
    cocodebug_fujinet_state(d->dbg, state, sizeof(state));
    set_text_at(d->state_text, state, 0);
    d->fuji_ticks = 0;

    if (!force && n == d->fuji_count &&
        (n == 0 || p[n - 1].serial == d->fuji_last))
        return;
    follow = force || d->fuji_count < 0 || edit_at_end(d, d->fuji_text);
    d->fuji_count = n;
    d->fuji_last = n > 0 ? p[n - 1].serial : 0;
    t[0] = '\0';
    for (i = 0; i < n && len + 400 < sizeof(t); i++) {
        int k;
        if (hide && cocodebug_fujinet_is_ready(&p[i]))
            continue;
        len += (size_t)snprintf(t + len, sizeof(t) - len, "%s%s\n     ",
                                shown++ ? "\n" : "", p[i].text);
        for (k = 0; k < p[i].nbytes && k < (int)sizeof p[i].data; k++)
            len += (size_t)snprintf(t + len, sizeof(t) - len, "%02X ",
                                    p[i].data[k]);
        if (p[i].nbytes > (int)sizeof p[i].data)
            len += (size_t)snprintf(t + len, sizeof(t) - len,
                                    "\xe2\x80\xa6");
    }
    if (shown == 0)
        snprintf(t, sizeof(t), "%s",
                 n ? "(only READY probes since the trace was cleared)"
                   : "(no DriveWire traffic since the trace was cleared)");
    set_text_at(d->fuji_text, t, follow);
}

static void refresh_all(debugger *d, int paused)
{
    set_text(d->run_btn, paused ? "Run" : "Pause");
    set_text(d->status, paused ? "Paused" : "Running");
    EnableWindow(d->step_line_btn, cocodebug_source_available(d->dbg));
    EnableWindow(d->step_over_line_btn, cocodebug_source_available(d->dbg));
    if (paused) {
        refresh_disasm(d);
        refresh_regs(d);
        refresh_source(d);
        refresh_locals(d);
        refresh_mem(d);
        refresh_video(d);
    } else {
        /* Everything on show is only valid while the machine is parked, so
         * clear it rather than leave a stale snapshot that looks live. */
        clear_views(d);
    }
    if (d->page == TAB_FUJINET)
        refresh_fujinet(d, 0);
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

/* Rebuilds the file picker from cocodebug_source_file_count()/_file_path().
 * Selection index and file_id are the same number: files are appended here
 * in exactly the id order the engine hands them out (0..count-1). */
static void rebuild_src_combo(debugger *d)
{
    int n = cocodebug_source_file_count(d->dbg);
    int i;

    SendMessageA(d->src_combo, CB_RESETCONTENT, 0, 0);
    for (i = 0; i < n; i++) {
        const char *path = cocodebug_source_file_path(d->dbg, i);
        const char *base = path ? strrchr(path, '\\') : NULL;
        if (!base && path)
            base = strrchr(path, '/');
        SendMessageA(d->src_combo, CB_ADDSTRING, 0,
                    (LPARAM)(base ? base + 1 : (path ? path : "")));
    }
    EnableWindow(d->src_combo, n > 0);
    if (n > 0) {
        SendMessageA(d->src_combo, CB_SETCURSEL, 0, 0);
        populate_src_list(d, 0);
    } else {
        set_text(d->src_view, "");
    }
}

/* One or more .lst files, one per module of a multi-file CMOC program --
 * cocodebug_source_load() adds to what is already loaded rather than
 * replacing it (see its own doc comment for why: cmoc names the .map after
 * the linked executable, not any one .lst, so every module's listing wants
 * the very .map its neighbours do), so loading them all here in one dialog
 * is exactly the multi-file case working as intended. */
static void load_source(debugger *d)
{
    OPENFILENAMEA ofn;
    static char buf[16384]; /* zeroed: static duration, not stack garbage --
                              * the single/multi disambiguation below relies
                              * on the byte past the first NUL already being
                              * '\0' unless GetOpenFileNameA itself put a
                              * second filename there. */
    char dir[MAX_PATH];
    char *p;
    int loaded = 0, failed = 0;

    memset(buf, 0, sizeof(buf));
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = d->hwnd;
    ofn.lpstrFilter = "CMOC listings (*.lst)\0*.lst\0All files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = sizeof(buf);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
               OFN_ALLOWMULTISELECT | OFN_EXPLORER;
    if (!GetOpenFileNameA(&ofn))
        return;

    p = buf + strlen(buf) + 1;
    if (*p == '\0') {
        /* Exactly one file: buf is the whole path, with none of the
         * directory/filename split below -- that split only happens when
         * there is more than one filename to share a directory with. */
        if (cocodebug_source_load(d->dbg, buf) < 0)
            failed++;
        else
            loaded++;
    } else {
        strncpy(dir, buf, sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = '\0';
        while (*p) {
            char full[MAX_PATH * 2]; /* dir + '\\' + filename, each up to
                                      * MAX_PATH on its own */
            size_t n = strlen(p);
            snprintf(full, sizeof(full), "%s\\%s", dir, p);
            if (cocodebug_source_load(d->dbg, full) < 0)
                failed++;
            else
                loaded++;
            p += n + 1;
        }
    }

    if (failed && !loaded) {
        set_text(d->status,
                "Could not open listing or its .map (both need `cmoc -i`)");
        return;
    }
    if (failed)
        set_text(d->status, "Some listings could not be opened");
    if (loaded) {
        d->src_cached_path[0] = '\0'; /* the file list may have changed */
        rebuild_src_combo(d);
    }
}

static void clear_source(debugger *d)
{
    cocodebug_source_clear(d->dbg);
    free(d->src_cached_buf);
    free(d->src_cached_lines);
    d->src_cached_buf = NULL;
    d->src_cached_lines = NULL;
    d->src_cached_nlines = 0;
    d->src_cached_path[0] = '\0';
    d->pc_file_id = -1;
    d->src_displayed_file_id = -1;
    SendMessageA(d->src_combo, CB_RESETCONTENT, 0, 0);
    EnableWindow(d->src_combo, FALSE);
    set_text(d->src_view, "");
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

static WNDPROC g_src_proc;

/* Unlike disasm_subclass, no text parsing is needed to recover which line
 * was clicked: the source view has exactly one physical line per source
 * line (WS_HSCROLL disables word-wrap, same as the other mono views), so
 * the display row EM_CHARFROMPOS reports is the 0-based line index
 * directly. */
static LRESULT CALLBACK src_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_LBUTTONDOWN && g_dbg && cocodebug_is_paused(g_dbg->dbg)) {
        LRESULT pos = SendMessageA(hwnd, EM_CHARFROMPOS, 0, lp);
        int line_idx = HIWORD(pos);
        if (line_idx >= 0 && line_idx < g_dbg->src_cached_nlines) {
            cocodebug_source_breakpoint_toggle(g_dbg->dbg,
                                               g_dbg->src_displayed_file_id,
                                               line_idx + 1);
            populate_src_list(g_dbg, g_dbg->src_displayed_file_id);
        }
    }
    return CallWindowProcA(g_src_proc, hwnd, msg, wp, lp);
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
    d->step_line_btn = child(d, "BUTTON", "Step Line (F6)", BS_PUSHBUTTON,
                             IDC_STEP_LINE);
    d->step_over_line_btn = child(d, "BUTTON", "Step Over Line (Shift+F6)",
                                  BS_PUSHBUTTON, IDC_STEP_OVER_LINE);
    d->clear_bp_btn = child(d, "BUTTON", "Clear Breakpoints", BS_PUSHBUTTON,
                            IDC_CLEAR_BP);
    d->load_sym_btn = child(d, "BUTTON", "Load Symbols...", BS_PUSHBUTTON,
                            IDC_LOAD_SYM);
    d->clear_sym_btn = child(d, "BUTTON", "Clear Symbols", BS_PUSHBUTTON,
                             IDC_CLEAR_SYM);
    d->load_src_btn = child(d, "BUTTON", "Load C Source...", BS_PUSHBUTTON,
                            IDC_LOAD_SRC);
    d->clear_src_btn = child(d, "BUTTON", "Clear C Source", BS_PUSHBUTTON,
                             IDC_CLEAR_SRC);
    d->status = child(d, "STATIC", "Running", SS_LEFT, IDC_STATUS);

    d->disasm = mono_view(d, IDC_DISASM);
    g_disasm_proc = (WNDPROC)SetWindowLongPtrA(d->disasm, GWLP_WNDPROC,
                                               (LONG_PTR)disasm_subclass);

    /* CBS_DROPDOWNLIST: a plain read-only picker, matching KDE/GNOME's file
     * combo -- no free text entry. Disabled until a listing is loaded. */
    d->src_combo = child(d, "COMBOBOX", "",
                         CBS_DROPDOWNLIST | WS_VSCROLL, IDC_SRC_COMBO);
    EnableWindow(d->src_combo, FALSE);
    d->src_view = mono_view(d, IDC_SRC_VIEW);
    g_src_proc = (WNDPROC)SetWindowLongPtrA(d->src_view, GWLP_WNDPROC,
                                            (LONG_PTR)src_subclass);

    d->regs = mono_view(d, IDC_REGS);
    d->locals = mono_view(d, IDC_LOCALS);
    d->video = child(d, "CocoDbgPixels", "", 0, IDC_VIDEO);
    SetWindowLongPtrA(d->video, GWLP_USERDATA, (LONG_PTR)&d->video_view);

    d->mem_addr = field(d, IDC_MEM_ADDR);
    d->mem_go = child(d, "BUTTON", "Go", BS_PUSHBUTTON, IDC_MEM_GO);
    d->mem_view = mono_view(d, IDC_MEM_VIEW);

    /* The tab control is the strip alone: the pages sit on the window's own
     * background. */
    {
        TCITEMA item;
        int i;
        d->tabs = child(d, WC_TABCONTROLA, "", WS_CLIPSIBLINGS, IDC_TABS);
        memset(&item, 0, sizeof(item));
        item.mask = TCIF_TEXT;
        for (i = 0; i < TAB_COUNT; i++) {
            item.pszText = (char *)k_tab_labels[i];
            SendMessageA(d->tabs, TCM_INSERTITEMA, (WPARAM)i, (LPARAM)&item);
        }
    }

    d->fuji_title = child(d, "STATIC", "DriveWire packets", SS_LEFT,
                          IDC_FUJI_TITLE);
    d->fuji_note = child(d, "STATIC", "", SS_LEFT | SS_ENDELLIPSIS,
                         IDC_FUJI_NOTE);
    d->fuji_hide = child(d, "BUTTON", "Hide READY probes", BS_AUTOCHECKBOX,
                         IDC_FUJI_HIDE);
    SendMessageA(d->fuji_hide, BM_SETCHECK, BST_CHECKED, 0);
    d->fuji_clear = child(d, "BUTTON", "Clear", BS_PUSHBUTTON, IDC_FUJI_CLEAR);
    d->fuji_text = mono_view(d, IDC_FUJI_TEXT);
    d->state_title = child(d, "STATIC", "Becker port", SS_LEFT,
                           IDC_FUJI_STATE_TITLE);
    d->state_text = mono_view(d, IDC_FUJI_STATE);
}

static void show_page(debugger *d, int page)
{
    HWND cpu[] = {
        d->run_btn, d->step_btn, d->step_over_btn, d->step_out_btn,
        d->step_line_btn, d->step_over_line_btn, d->clear_bp_btn,
        d->load_sym_btn, d->clear_sym_btn, d->load_src_btn, d->clear_src_btn,
        d->status, d->disasm, d->src_combo, d->src_view, d->regs, d->locals,
        d->video, d->mem_addr, d->mem_go, d->mem_view,
    };
    HWND fuji[] = {
        d->fuji_title, d->fuji_note, d->fuji_hide, d->fuji_clear,
        d->fuji_text, d->state_title, d->state_text,
    };
    size_t i;

    d->page = page;
    for (i = 0; i < sizeof(cpu) / sizeof(cpu[0]); i++)
        ShowWindow(cpu[i], page == TAB_CPU ? SW_SHOW : SW_HIDE);
    for (i = 0; i < sizeof(fuji) / sizeof(fuji[0]); i++)
        ShowWindow(fuji[i], page == TAB_FUJINET ? SW_SHOW : SW_HIDE);
}

/* COCO_DEBUGGER_TAB: a tab's number or name ("cpu", "fujinet"), or the
 * start of one, any case. */
static int tab_from_env(void)
{
    const char *tab = getenv("COCO_DEBUGGER_TAB");
    char *end;
    long n;
    int i;
    size_t k;

    if (!tab || !*tab)
        return -1;
    n = strtol(tab, &end, 10);
    if (end != tab && *end == '\0')
        return (n >= 0 && n < TAB_COUNT) ? (int)n : -1;
    for (i = 0; i < TAB_COUNT; i++) {
        for (k = 0; tab[k] && tolower((unsigned char)tab[k]) ==
                                  k_tab_names[i][k]; k++)
            ;
        if (!tab[k])
            return i;
    }
    return -1;
}

static void layout(debugger *d)
{
    RECT client, strip;
    int y, bx = 8;
    int top_h, mid_top, mid_h, left_w, src_w, right_x1, right_x2, right_w;
    int mem_row_y;

    GetClientRect(d->hwnd, &client);

    SetRect(&strip, 0, 0, 400, 400);
    SendMessageA(d->tabs, TCM_ADJUSTRECT, FALSE, (LPARAM)&strip);
    MoveWindow(d->tabs, 8, 8, client.right - 16, strip.top, TRUE);
    y = 8 + strip.top + 6;

    /* FujiNet: the packets left (a note, the READY filter and Clear over
     * them), the Becker port right. */
    {
        const int h = client.bottom - 8 - y;
        int rw = (client.right - 16) / 3;
        int lw;
        if (rw > 380) rw = 380;
        if (rw < 240) rw = 240;
        lw = client.right - 16 - rw - 12;
        MoveWindow(d->fuji_title, 8, y, lw, 20, TRUE);
        MoveWindow(d->fuji_clear, 8 + lw - 80, y + 24, 80, 26, TRUE);
        MoveWindow(d->fuji_hide, 8 + lw - 80 - 8 - 190, y + 26, 190, 22, TRUE);
        MoveWindow(d->fuji_note, 8, y + 29, lw - 80 - 8 - 190 - 8, 20, TRUE);
        MoveWindow(d->fuji_text, 8, y + 56, lw, h - 56, TRUE);
        MoveWindow(d->state_title, 8 + lw + 12, y, rw, 20, TRUE);
        MoveWindow(d->state_text, 8 + lw + 12, y + 24, rw, h - 24, TRUE);
    }

    MoveWindow(d->run_btn, bx, y, 110, 26, TRUE);            bx += 116;
    MoveWindow(d->step_btn, bx, y, 100, 26, TRUE);           bx += 106;
    MoveWindow(d->step_over_btn, bx, y, 130, 26, TRUE);      bx += 136;
    MoveWindow(d->step_out_btn, bx, y, 150, 26, TRUE);       bx += 156;
    MoveWindow(d->step_line_btn, bx, y, 130, 26, TRUE);      bx += 136;
    MoveWindow(d->step_over_line_btn, bx, y, 190, 26, TRUE); bx += 196;
    MoveWindow(d->clear_bp_btn, bx, y, 140, 26, TRUE);       bx += 146;
    MoveWindow(d->load_sym_btn, bx, y, 120, 26, TRUE);       bx += 126;
    MoveWindow(d->clear_sym_btn, bx, y, 110, 26, TRUE);      bx += 116;
    MoveWindow(d->load_src_btn, bx, y, 140, 26, TRUE);       bx += 146;
    MoveWindow(d->clear_src_btn, bx, y, 120, 26, TRUE);      bx += 126;
    MoveWindow(d->status, bx, y + 5, client.right - bx - 8, 20, TRUE);
    top_h = y + 26 + 10;

    /* Middle area, three columns: disassembly, C source (file picker over a
     * listing), then registers + locals + video stacked. */
    mid_top = top_h;
    mem_row_y = client.bottom - 8 - 22 - 4 - 160; /* address row + mem view */
    mid_h = mem_row_y - mid_top - 8;
    if (mid_h < 100) mid_h = 100;

    left_w = (client.right - 24) * 35 / 100;
    src_w = (client.right - 24) * 30 / 100;
    MoveWindow(d->disasm, 8, mid_top, left_w, mid_h, TRUE);

    right_x1 = 8 + left_w + 8;
    /* The combo's own height parameter sets how tall its dropdown list can
     * open, not the closed box (the system sizes that from the font) -- 200
     * is just "tall enough to show several files without scrolling". */
    MoveWindow(d->src_combo, right_x1, mid_top, src_w, 200, TRUE);
    MoveWindow(d->src_view, right_x1, mid_top + 28, src_w, mid_h - 28, TRUE);

    right_x2 = right_x1 + src_w + 8;
    right_w = client.right - right_x2 - 8;
    MoveWindow(d->regs, right_x2, mid_top, right_w, 130, TRUE);
    MoveWindow(d->locals, right_x2, mid_top + 138, right_w, 110, TRUE);
    MoveWindow(d->video, right_x2, mid_top + 256, right_w, mid_h - 256, TRUE);

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
            } else if (d->page == TAB_FUJINET && ++d->fuji_ticks >= 5) {
                /* the traffic twice a second, whether or not it stops */
                refresh_fujinet(d, 0);
            }
        }
        return 0;
    case WM_NOTIFY: {
        const NMHDR *nh = (const NMHDR *)lp;
        if (nh->hwndFrom == d->tabs && nh->code == (UINT)TCN_SELCHANGE) {
            show_page(d, (int)SendMessageA(d->tabs, TCM_GETCURSEL, 0, 0));
            if (d->page == TAB_FUJINET)
                refresh_fujinet(d, 0);
            return 0;
        }
        break;
    }
    case WM_DBG_ACCEPT:
        on_accept(d, (int)wp);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_RUN:        toggle_run(d); return 0;
        case IDC_STEP:       cocodebug_step(d->dbg); return 0;
        case IDC_STEP_OVER:  cocodebug_step_over(d->dbg); return 0;
        case IDC_STEP_OUT:   cocodebug_step_out(d->dbg); return 0;
        case IDC_STEP_LINE:      cocodebug_step_line(d->dbg); return 0;
        case IDC_STEP_OVER_LINE: cocodebug_step_over_line(d->dbg); return 0;
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
        case IDC_LOAD_SRC:   load_source(d); return 0;
        case IDC_CLEAR_SRC:  clear_source(d); return 0;
        case IDC_SRC_COMBO:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int idx = (int)SendMessageA(d->src_combo, CB_GETCURSEL, 0, 0);
                if (idx != CB_ERR)
                    populate_src_list(d, idx);
            }
            return 0;
        case IDC_MEM_GO:     on_accept(d, IDC_MEM_ADDR); return 0;
        case IDC_FUJI_CLEAR:
            cocodebug_fujinet_trace_clear(d->dbg);
            refresh_fujinet(d, 1);
            return 0;
        case IDC_FUJI_HIDE:
            if (HIWORD(wp) == BN_CLICKED)
                refresh_fujinet(d, 1);
            return 0;
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
        free(d->src_cached_buf);
        free(d->src_cached_lines);
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
    ACCEL accels[6];

    if (g_dbg) {
        ShowWindow(g_dbg->hwnd, SW_SHOW);
        cocodebug_set_engaged(g_dbg->dbg, 1);
        cocodebug_pause(g_dbg->dbg);
        SetForegroundWindow(g_dbg->hwnd);
        return;
    }

    inst = (HINSTANCE)GetWindowLongPtrA(parent, GWLP_HINSTANCE);

    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_TAB_CLASSES;
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
    d->pc_file_id = -1;
    d->src_displayed_file_id = -1;
    d->fuji_count = -1;
    g_dbg = d;

    d->hwnd = CreateWindowExA(0, "CocoDebuggerWindow", "Debugger",
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              1750, 860, NULL, NULL, inst, NULL);
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
    {
        HDC dc = GetDC(d->hwnd);
        HGDIOBJ prev = SelectObject(dc, d->mono);
        TEXTMETRICA tm;
        d->mono_h = GetTextMetricsA(dc, &tm) ? tm.tmHeight : 15;
        SelectObject(dc, prev);
        ReleaseDC(d->hwnd, dc);
    }

    build_controls(d);
    {
        const int tab = tab_from_env();
        if (tab >= 0)
            SendMessageA(d->tabs, TCM_SETCURSEL, (WPARAM)tab, 0);
        show_page(d, tab >= 0 ? tab : TAB_CPU);
    }

    /* F5/F6/F7/F8/Shift+F6/Shift+F8 work wherever the focus is inside the
     * window, matching the other frontends' shortcuts. */
    accels[0].fVirt = FVIRTKEY;          accels[0].key = VK_F5;
    accels[0].cmd = IDC_RUN;
    accels[1].fVirt = FVIRTKEY;          accels[1].key = VK_F7;
    accels[1].cmd = IDC_STEP;
    accels[2].fVirt = FVIRTKEY;          accels[2].key = VK_F8;
    accels[2].cmd = IDC_STEP_OVER;
    accels[3].fVirt = FVIRTKEY | FSHIFT; accels[3].key = VK_F8;
    accels[3].cmd = IDC_STEP_OUT;
    accels[4].fVirt = FVIRTKEY;          accels[4].key = VK_F6;
    accels[4].cmd = IDC_STEP_LINE;
    accels[5].fVirt = FVIRTKEY | FSHIFT; accels[5].key = VK_F6;
    accels[5].cmd = IDC_STEP_OVER_LINE;
    d->accel = CreateAcceleratorTableA(accels, 6);

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
