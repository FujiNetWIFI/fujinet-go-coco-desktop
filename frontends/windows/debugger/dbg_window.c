/*
 * Debugger window (Win32) over the 6809 / 6309 debugger engine, via
 * core/include/cocodebug.h. Mirrors the GNOME, Qt and Studio II debuggers tab
 * for tab -- Console, Disassembly, C Source, Memory, Breakpoints, Watches,
 * Cart & I/O, Trace, History, Video, Sound, Symbols, FujiNet -- built from
 * plain common controls, with the disassembly, the C listing, the memory dump
 * and the video pictures drawn by hand. The registers and flags sit in a strip
 * under the tabs, on every tab; the 6309's E F W V MD appear only on a 6309.
 *
 * Each tab's controls are children of a container window of their own, so
 * Pop Out can SetParent a tab into a window titled "CoCo Debugger - <Tab>"
 * (Dock, or closing that window, puts it back in its place; one tab always
 * stays).
 *
 * The engine only engages while this window is showing: showing it engages
 * (and pauses the machine, as on every sibling) and hiding it (the close
 * box, or F12 inside the window) releases it, which lets the machine run on at
 * full speed. The toolbar carries a single Pause/Run button (the accent while
 * paused), the stepping controls (Step, Step Over, Step Out, Step Line, Step
 * Over Line, Run to Cursor), Clear Breakpoints, a "Break at" box, symbol and C
 * source loading and clearing; F5 F6 F7 F8 Shift+F6 Shift+F8 F9 F12 work
 * wherever the focus is inside the window (coco_debugger_pretranslate).
 *
 * A 100 ms timer polls the engine's generation counter and stop snapshot
 * (never a callback, which would fire off the UI thread) and refreshes the tab
 * showing, and only that one, when something changed; while the machine runs
 * it refreshes the live values of that tab twice a second. Every new stop
 * serial brings every view to the spot: the Disassembly and the C Source to the
 * PC, the Memory dump to the PC's neighbourhood, Trace and History to their
 * newest line. After a step the window waits a few milliseconds for the stop to
 * settle so the status does not flash "Running".
 *
 * Memory has two address spaces: CPU (the 64K the SAM or the GIME's MMU maps)
 * and, on a CoCo 3, PHYS (its 512K or 2M of physical RAM). Go To, Find, Poke,
 * the highlights and Show in Memory apply to the space selected; the dump
 * reads only the rows on screen, so a 2M space costs nothing. Address fields
 * take a symbol, else bare hex, else the engine's grammar ($ or 0x, registers,
 * arithmetic, [ptr]); the register boxes show hex without a prefix and read it
 * back the same way. Only the console reads bare digits as decimal.
 *
 * The C Source tab keeps the CMOC listing (a file picker, the lines with
 * their breakpoint dots and the PC bar, the locals); the Video tab
 * (dbg_video.c, over core/include/cocovidtab.h) and the FujiNet tab (the
 * DriveWire packets on the Becker port, with Hide READY probes) carry over.
 *
 * COCO_DEBUGGER_TAB (a tab number, or the start of a tab's name or label, in
 * any case; cpu is the Disassembly) picks the tab the window opens on: 0
 * Console, 1 Disassembly, 2 C Source, 3 Memory, 4 Breakpoints, 5 Watches,
 * 6 Cart & I/O, 7 Trace, 8 History, 9 Video, 10 Sound, 11 Symbols, 12
 * FujiNet. For captures: COCO_WATCH (a comma-separated list of watches to add,
 * each optionally prefixed cpu: or phys:), COCO_WATCH_BREAK (a watch added and
 * a break-on-change armed over it), COCO_MEM_GOTO (the Memory tab's Go To),
 * COCO_MEM_SPACE (cpu or phys), COCO_HISTORY=1 (recording on) and COCO_POPOUT
 * (a tab to pop out, named like COCO_DEBUGGER_TAB). The Video tab reads
 * COCO_DEBUGGER_VIDEO_*.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cocodebug.h"
#include "cocomemtools.h"
#include "cocosession.h"
#include "dbg_video.h"
#include "resource.h"
#include "../help/help_window.h"

/* The CoCo green (COCOSESSION_ACCENT_RGB) takes the dark ink on fills; the dark accent is for outlines and text. */
#define COCO_ACCENT_COLORREF RGB((COCOSESSION_ACCENT_RGB >> 16) & 255, (COCOSESSION_ACCENT_RGB >> 8) & 255, COCOSESSION_ACCENT_RGB & 255)
#define COCO_ACCENT_TEXT RGB((COCOSESSION_ACCENT_INK_RGB >> 16) & 255, (COCOSESSION_ACCENT_INK_RGB >> 8) & 255, COCOSESSION_ACCENT_INK_RGB & 255)
#define COCO_ACCENT_DARK_COLORREF RGB((COCOSESSION_ACCENT_DARK_RGB >> 16) & 255, (COCOSESSION_ACCENT_DARK_RGB >> 8) & 255, COCOSESSION_ACCENT_DARK_RGB & 255)

static HBRUSH coco_accent_brush(void)
{
    static HBRUSH brush;
    if (!brush) brush = CreateSolidBrush(COCO_ACCENT_COLORREF);
    return brush;
}

static HBRUSH coco_accent_hot_brush(void)
{
    static HBRUSH brush;
    if (!brush)
        brush = CreateSolidBrush(RGB(0x8B + (255 - 0x8B) / 3, 0xC3 + (255 - 0xC3) / 3, 0x4A + (255 - 0x4A) / 3));
    return brush;
}

static HBRUSH coco_accent_dark_brush(void)
{
    static HBRUSH brush;
    if (!brush) brush = CreateSolidBrush(COCO_ACCENT_DARK_COLORREF);
    return brush;
}

/* A checkable push button's NM_CUSTOMDRAW: the accent while it is on, the
 * stock look (and focus, hover and press) while off. */
static LRESULT coco_accent_customdraw(const NMCUSTOMDRAW *cd, int on)
{
    char text[64];
    RECT r;
    if (cd->dwDrawStage != CDDS_PREPAINT || !on) return CDRF_DODEFAULT;
    r = cd->rc;
    FillRect(cd->hdc, &r, (cd->uItemState & CDIS_HOT) ? coco_accent_hot_brush() : coco_accent_brush());
    FrameRect(cd->hdc, &r, coco_accent_dark_brush());
    SetBkMode(cd->hdc, TRANSPARENT);
    SetTextColor(cd->hdc, COCO_ACCENT_TEXT);
    text[0] = '\0';
    GetWindowTextA(cd->hdr.hwndFrom, text, sizeof text);
    {
        HFONT f = (HFONT)SendMessageA(cd->hdr.hwndFrom, WM_GETFONT, 0, 0);
        HGDIOBJ old = f ? SelectObject(cd->hdc, f) : NULL;
        DrawTextA(cd->hdc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (old) SelectObject(cd->hdc, old);
    }
    if (GetFocus() == cd->hdr.hwndFrom) {
        InflateRect(&r, -3, -3);
        DrawFocusRect(cd->hdc, &r);
    }
    return CDRF_SKIPDEFAULT;
}

#define DISASM_MAX 96        /* rows fetched at most; the view shows what fits */
#define MEM_BUF_ROWS 160     /* dump rows (16 bytes) held: more than any window shows */
#define NSPACES 2            /* CPU PHYS */
#define EVENTS_MAX 400       /* sound writes listed */
#define MAX_BPS 128
#define HISTORY_MAX 64
#define TRACE_MAX 513        /* FujiNet packets asked for: all the engine keeps, plus the one in progress */
#define HIST_RING 65536      /* the engine's instruction ring */
#define HIST_SHOWN 2000      /* the newest matching lines a Trace/History tab shows */
#define HIST_TICKS 4         /* the Trace tab, every ~400 ms while the machine runs */
#define MEM_REGION_MAX 64    /* regions of the memory map held */
#define SYMBOL_MAX 4096      /* symbols the Symbols tab lists at most */
#define LOCALS_MAX 32
#define CONSOLE_KEEP (256 * 1024)
#define NOTICE_MS 4000       /* how long a notice holds the status line */

/* A disassembly row's columns, in characters: address, bytes, label, the
 * instruction, and the comment. */
#define COL_BYTES 6
#define COL_LABEL 22
#define LABEL_W 22
#define COL_DISASM (COL_LABEL + LABEL_W + 1)
#define COMMENT_COL (COL_DISASM + 30)

/* Sizes at 96 dpi; S() scales them to the window's. */
#define GUTTER_W 18          /* the disassembly's breakpoint column */
#define MONO_PX 13           /* the monospace font */
#define START_W 1500
#define START_H 820

#define WM_DBG_ACCEPT  (WM_APP + 2)   /* wp: control id -- Enter in an edit */
#define WM_DBG_TAB     (WM_APP + 3)   /* Tab in the console: complete */
#define WM_DBG_HISTORY (WM_APP + 4)   /* wp: 0 Up (older), 1 Down (newer) in the console */
#define WM_DBG_SYNC    (WM_APP + 5)   /* resync once a list's notification is over */
#define WM_DBG_LINE    (WM_APP + 6)   /* a click in the C listing: wp control id, lp the line number */

#define TIMER_REFRESH 1

#define DBG_CLASS "CocoDebuggerWindow"
#define DISASM_CLASS "CocoDbgDisasm"
#define MEMVIEW_CLASS "CocoDbgMem"
#define LINES_CLASS "CocoDbgLines"
#define PAGE_CLASS "CocoDbgPage"
#define POP_CLASS "CocoDbgPop"

enum { TAB_CONSOLE, TAB_DISASM, TAB_CSOURCE, TAB_MEMORY, TAB_BREAKS, TAB_WATCHES, TAB_CART, TAB_TRACE, TAB_HISTORY,
       TAB_VIDEO, TAB_SOUND, TAB_SYMBOLS, TAB_FUJINET, TAB_COUNT };

/* The tabs, in order: the names COCO_DEBUGGER_TAB takes, and their labels
 * (which it takes too). */
static const struct { const char *name, *label; } k_tabs[TAB_COUNT] = {
    { "console", "Console" }, { "disassembly", "Disassembly" }, { "csource", "C Source" }, { "memory", "Memory" },
    { "breakpoints", "Breakpoints" }, { "watches", "Watches" }, { "cart", "Cart & I/O" },
    { "trace", "Trace" }, { "history", "History" }, { "video", "Video" }, { "sound", "Sound" },
    { "symbols", "Symbols" }, { "fujinet", "FujiNet" },
};

#define NREGS 15            /* PC A B D DP X Y U S CC, then the 6309's E F W V MD */
#define NREGS_6809 10
#define NCHECKS 8           /* E F H I N Z V C */

enum {
    IDC_RUN = 1000, IDC_STEP, IDC_OVER, IDC_OUT, IDC_STEP_LINE, IDC_OVER_LINE, IDC_RUN_TO, IDC_SYMBOLS,
    IDC_STATUS, IDC_TABS, IDC_TOGGLE_BP, IDC_HELP_BTN, IDC_POPOUT, IDC_DOCK, IDC_CLEAR_BPS,
    IDC_BREAK_AT, IDC_CLEAR_SYMBOLS, IDC_LOAD_SRC, IDC_CLEAR_SRC, IDC_SYM_FILTER, IDC_SYM_LIST,
    IDC_SYM_GOTO, IDC_SYM_BREAK, IDC_SYM_LOAD, IDC_SYM_CLEAR,
    IDC_CON_OUT, IDC_CON_PROMPT, IDC_CON_IN,
    IDC_REG0, IDC_REG_LAST = IDC_REG0 + NREGS - 1,
    IDC_CHK0, IDC_CHK_LAST = IDC_CHK0 + NCHECKS - 1,
    IDC_MEM_VIEW, IDC_MEM_GOTO, IDC_MEM_WRITE,
    IDC_FOLLOW_PC, IDC_JUMP, IDC_DISASM,
    IDC_SRC_FILES, IDC_SRC_VIEW, IDC_SRC_LOCALS,
    IDC_BP_LIST, IDC_BP_REMOVE, IDC_BP_CLEAR, IDC_BP_TYPE, IDC_BP_PHYS, IDC_BP_START, IDC_BP_END, IDC_BP_COND,
    IDC_BP_ADD, IDC_BP_CLEAR_HITS,
    IDC_WT_ENTRY, IDC_WT_ADD, IDC_WT_LIST, IDC_WT_REMOVE, IDC_WT_MEM, IDC_WT_BRK,
    IDC_SRCH_KIND, IDC_SRCH_ENTRY, IDC_SRCH_NEXT, IDC_SRCH_PREV, IDC_MEM_SPACE,
    IDC_WT_SPACE,
    IDC_CART_TEXT, IDC_CART_IO,
    IDC_SOUND_TEXT, IDC_SOUND_LEVEL, IDC_SOUND_LOG,
    IDC_H_REC0, IDC_H_REC1, IDC_H_CLEAR0, IDC_H_CLEAR1, IDC_H_FILTER0, IDC_H_FILTER1, IDC_H_RANGE0, IDC_H_RANGE1,
    IDC_H_TEXT0, IDC_H_TEXT1,
    IDC_FUJI_CLEAR, IDC_FUJI_HIDE, IDC_FUJI_TEXT, IDC_FUJI_STATE,
    IDC_LABEL_FIRST
};

typedef struct {
    HWND hwnd, tabs;
    HWND run_btn, step_btn, over_btn, out_btn, step_line_btn, over_line_btn, run_to_btn, clear_bps_btn, break_at_label,
         break_at, sym_btn, clear_sym_btn, src_btn, clear_src_btn, popout_btn, help_btn, status;

    /* Each tab's controls are children of a container of its own, so a tab can
     * be peeled off into a window (SetParent) and put back. */
    HWND pg[TAB_COUNT];
    HWND popped[TAB_COUNT];     /* the window a tab lives in, or NULL */
    int tab_page[TAB_COUNT];    /* the tab at each tab-control index */
    int ntabs;
    HWND new_parent;            /* where child() puts the next control */

    /* console */
    HWND con_out, con_prompt, con_in;
    char history[HISTORY_MAX][256];
    int nhistory, history_pos;

    /* the registers strip, under the tabs */
    HWND reg_label[NREGS], reg[NREGS], chk[NCHECKS], info;
    int strip_rows;             /* register rows the strip's last layout used */
    int is6309;                 /* the 6309's registers are showing */
    cocodebug_regs regs;        /* as last read */
    int regs_ok;                /* regs holds a stop's values */

    /* memory */
    HWND mem_title, mem_space, mem_view, goto_label, mem_goto, write_label, mem_write;
    HWND srch_kind, srch_entry, srch_next, srch_prev, srch_note;
    int mem_sp;                 /* the space showing: COCODEBUG_SPACE_* */
    int mem_digits;             /* hex digits of an address in it */
    uint32_t mem_size;          /* its size in bytes */
    int mem_hl;                 /* a highlighted range (Go to, Find, a watch) */
    int mem_hl_space;           /* ... and the space it was made in */
    uint32_t mem_hl_lo, mem_hl_hi;
    uint8_t mem_buf[MEM_BUF_ROWS * 16];   /* the rows on screen */
    int mem_buf_line;           /* the dump line mem_buf starts at (-1: none), mem_buf_rows rows held */
    int mem_buf_rows;
    int mem_buf_space;
    cocodebug_region mem_map[MEM_REGION_MAX];   /* the memory map the rows are labelled from (CPU) */
    int mem_map_count;
    int mem_first;              /* the dump's first visible text line */
    int mem_recentre;           /* a stop happened: bring the PC's row into view */

    /* disassembly */
    HWND follow_pc, jump_label, jump, disasm_hint, disasm;
    int disasm_top;             /* address of the first disassembly line */
    int recentre;               /* a stop happened: bring the PC into view */
    uint64_t stop_serial;       /* the last stop the window followed */
    int selected;               /* the selected line's address, or -1 */
    cocodebug_line lines[DISASM_MAX];
    int line_count, pc_line;

    /* C source: a listing of the file picked, the locals */
    HWND src_files, src_note, src_view, src_locals_label, src_locals;
    char src_path[MAX_PATH];    /* the file whose text is held */
    char *src_buf;              /* its bytes, '\n' turned into NULs */
    char **src_text;            /* one pointer a line */
    int src_lines;
    int src_file;               /* the file id the listing shows, -1 for none */
    int src_pc_file, src_pc_line;   /* where the PC is, whatever file is showing */
    char src_sig[MAX_PATH + 32];    /* the file list the picker was built from */
    uint64_t src_serial;

    /* video: the page holds its own controls (dbg_video.c), made on first use */
    HWND video;

    /* sound */
    HWND sound_text, sound_level, sound_log_label, sound_log;
    int snd_watching;           /* cocodebug_sound_watch is on */
    cocodebug_sound_write snd_events[EVENTS_MAX];

    /* cart & I/O */
    HWND cart_text, cart_io_label, cart_io;

    /* symbols */
    HWND sym_filter_label, sym_filter, sym_count, sym_list, sym_goto, sym_break, sym_load, sym_clear;
    int sym_indices[SYMBOL_MAX];
    int sym_shown;
    char sym_sig[128];
    int sym_force;              /* rebuild the list even if its signature held (symbols loaded) */

    /* breakpoints */
    HWND bp_hint, bp_stop, bp_remove, bp_clear, bp_clear_hits, bp_list, bp_empty;
    HWND bp_type, bp_phys, bp_start, bp_end, bp_cond, bp_add, bp_msg;
    cocodebug_breakpoint bps[MAX_BPS];
    int nbps;
    char bp_sig[MAX_BPS * 300]; /* what the list shows, to rebuild on change */
    int bp_sig_ok;

    /* watches */
    HWND wt_label, wt_space, wt_entry, wt_add, wt_list, wt_remove, wt_mem, wt_brk, wt_msg;
    char wt_sig[COCOWATCH_MAX * 120];
    int wt_sig_ok;

    /* trace (0) and history (1) share the engine's ring */
    HWND h_rec[2], h_clear[2], h_filter[2], h_range[2], h_note[2], h_text[2];
    uint64_t h_sig[2];
    int h_ok[2];
    int h_ticks;

    /* fujinet */
    HWND fuji_title, fuji_note, fuji_hide, fuji_clear, fuji_text, fuji_state_title, fuji_state;
    uint64_t fuji_last;
    int fuji_count;

    HWND views[24];             /* the read-only text views, drawn on the window colour */
    int nviews;
    HWND dim[64];               /* captions and hints, drawn grey */
    int ndim, nlabels;

    HFONT ui, ui_bold, mono;
    int dpi;
    int mono_w, mono_h, line_h;
    HACCEL accel;
    HBRUSH bp_on;

    cocosession *session;
    cocodebug *dbg;

    int page;
    unsigned seen_gen;
    int was_stopped;
    int running_ticks;
    int updating;               /* a refresh is setting controls */

    char notice[200];           /* a message holding the status line a moment */
    DWORD notice_until;
} debugger;

static debugger *g_dbg;

/* The editable registers, in the order the strip lays them out: the programmer's
 * 6809 set (D is A:B), then the 6309's extras, which show only on a 6309. */
static const char *const k_reg_names[NREGS] = { "PC", "A", "B", "D", "DP", "X", "Y", "U", "S", "CC",
                                                "E", "F", "W", "V", "MD" };
static const int k_reg_ids[NREGS] = { COCODEBUG_REG_PC, COCODEBUG_REG_A, COCODEBUG_REG_B, COCODEBUG_REG_D,
                                      COCODEBUG_REG_DP, COCODEBUG_REG_X, COCODEBUG_REG_Y, COCODEBUG_REG_U,
                                      COCODEBUG_REG_S, COCODEBUG_REG_CC, COCODEBUG_REG_E, COCODEBUG_REG_F,
                                      COCODEBUG_REG_W, COCODEBUG_REG_V, COCODEBUG_REG_MD };
static const int k_reg_digits[NREGS] = { 4, 2, 2, 4, 2, 4, 4, 4, 4, 2, 2, 2, 4, 4, 2 };

/* The CC check boxes, high bit first. */
static const char *const k_chk_names[NCHECKS] = { "E", "F", "H", "I", "N", "Z", "V", "C" };
static const unsigned k_chk_masks[NCHECKS] = { COCODEBUG_CC_E, COCODEBUG_CC_F, COCODEBUG_CC_H, COCODEBUG_CC_I,
                                               COCODEBUG_CC_N, COCODEBUG_CC_Z, COCODEBUG_CC_V, COCODEBUG_CC_C };

static const char *const k_space_names[NSPACES] = { "CPU", "PHYS" };
static const char *const k_space_titles[NSPACES] = {
    "CPU: the 64K the 6809 sees through the SAM or the GIME's MMU, labelled by the live memory map",
    "PHYS: the CoCo 3's physical RAM (GIME address = MMU bank x 8K + offset)" };

#define NBPTYPES 4
static const char *const k_bp_types[NBPTYPES] = { "Execute", "Read", "Write", "Access" };
static const int k_bp_type_ids[NBPTYPES] = { COCODEBUG_BP_EXEC, COCODEBUG_BP_READ, COCODEBUG_BP_WRITE,
                                             COCODEBUG_BP_ACCESS };

/* ---- helpers ---------------------------------------------------------------- */

static void edit_text(HWND h, char *out, int outsz) { GetWindowTextA(h, out, outsz); }

static void wide(const char *utf8, wchar_t *out, int outn)
{
    if (outn <= 0) return;
    out[0] = 0;
    if (utf8 && MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, outn) <= 0) out[0] = 0;
    out[outn - 1] = 0;
}

/* A label set only when it changed (no flicker on the live refresh). */
static void set_label(HWND h, const char *utf8)
{
    wchar_t cur[512], w[512];
    wide(utf8, w, 512);
    GetWindowTextW(h, cur, 512);
    if (wcscmp(cur, w) != 0) SetWindowTextW(h, w);
}

/* EDIT controls want CR LF and draw control characters as boxes: engine
 * text (UTF-8) as the wide text a view shows. */
static wchar_t *wide_view_text(const char *text)
{
    size_t n = strlen(text), i, o = 0;
    char *buf = malloc(n * 2 + 1);
    wchar_t *w = NULL;
    int wn;
    if (!buf) return NULL;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n') { buf[o++] = '\r'; buf[o++] = '\n'; }
        else if (c >= 0x20 || c == '\t') buf[o++] = (char)c;
    }
    buf[o] = '\0';
    wn = MultiByteToWideChar(CP_UTF8, 0, buf, -1, NULL, 0);
    if (wn > 0 && (w = malloc((size_t)wn * sizeof *w)) != NULL)
        MultiByteToWideChar(CP_UTF8, 0, buf, -1, w, wn);
    free(buf);
    return w;
}

static size_t addf(char *s, size_t len, size_t cap, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (len >= cap) return len;
    va_start(ap, fmt);
    n = vsnprintf(s + len, cap - len, fmt, ap);
    va_end(ap);
    if (n < 0) return len;
    return len + (size_t)n < cap ? len + (size_t)n : cap - 1;
}

/* Blanks off both ends, in place. */
static char *strip(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = '\0';
    return s;
}

/* The engine's address grammar in one space (symbols, $ and 0x, registers,
 * label+n, [ptr], lo-hi and lo..hi ranges; a bare token of hex digits is hex
 * unless it names a symbol). 1 and lo..hi, or 0 with the reason in err. */
static int parse_in_space(debugger *d, int space, const char *text, uint32_t *lo, uint32_t *hi, char *err, int errsz)
{
    char buf[128], *s;
    snprintf(buf, sizeof buf, "%s", text);
    s = strip(buf);
    if (!*s) {
        snprintf(err, (size_t)errsz, "Nothing typed");
        return 0;
    }
    err[0] = '\0';
    if (!cocosym_parse_range(d->dbg, space, s, lo, hi)) {
        snprintf(err, (size_t)errsz, "Not an address, symbol or expression in %s",
                 space >= 0 && space < NSPACES ? k_space_names[space] : "?");
        return 0;
    }
    return 1;
}

/* An expression through the engine's grammar (CPU); one value. 0 and
 * *value, or -1 with the reason in err. */
static int eval_expression(debugger *d, const char *text, long *value, char *err, int errsz)
{
    uint32_t low, high;
    if (!parse_in_space(d, COCODEBUG_SPACE_CPU, text, &low, &high, err, errsz)) return -1;
    *value = (long)low;
    return 0;
}

/* A CPU address field: a symbol first, then bare digits as hex (as the dumps
 * show them), else the engine's grammar, wrapped to 16 bits. -1 with the
 * reason in err when it is none of them. (The console keeps the engine's own
 * rule: bare digits decimal.) */
static int resolve_addr(debugger *d, const char *text, char *err, int errsz)
{
    uint32_t low, high;
    if (!parse_in_space(d, COCODEBUG_SPACE_CPU, text, &low, &high, err, errsz)) return -1;
    return (int)(low & 0xFFFF);
}

/* A CPU range field: "lo-hi" or "lo..hi", else a symbol, an address or an
 * expression (one byte). */
static int parse_range_ui(debugger *d, const char *text, uint16_t *lo, uint16_t *hi, char *err, int errsz)
{
    uint32_t low, high;
    if (!parse_in_space(d, COCODEBUG_SPACE_CPU, text, &low, &high, err, errsz)) return 0;
    *lo = (uint16_t)low;
    *hi = (uint16_t)high;
    return 1;
}

/* A register or byte box: hex without a prefix, as the boxes show it, or
 * else the engine's grammar. */
static int parse_hexish(debugger *d, const char *text, long *out)
{
    char buf[64], err[64], *s, *end;
    long v;
    snprintf(buf, sizeof buf, "%s", text);
    s = strip(buf);
    if (!*s) return 0;
    v = strtol(s, &end, 16);
    if (end != s && !*end) {
        *out = v;
        return 1;
    }
    err[0] = '\0';
    return eval_expression(d, s, out, err, sizeof err) == 0;
}

static const char *base_name(const char *path)
{
    const char *a = strrchr(path, '\\'), *b = strrchr(path, '/');
    if (b && (!a || b > a)) a = b;
    return a ? a + 1 : path;
}

/* ---- scale and fonts --------------------------------------------------------- */

static int system_dpi(void)
{
    HDC dc = GetDC(NULL);
    int v = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(NULL, dc);
    return v > 0 ? v : 96;
}

/* The window's DPI (Windows 10 1607+), else the system's. */
static int window_dpi(HWND hwnd)
{
    typedef UINT (WINAPI *dpi_fn)(HWND);
    static dpi_fn fn;
    static int looked;
    if (!looked) {
        HMODULE user = GetModuleHandleA("user32.dll");
        fn = user ? (dpi_fn)(void (*)(void))GetProcAddress(user, "GetDpiForWindow") : NULL;
        looked = 1;
    }
    if (fn && hwnd) {
        const UINT v = fn(hwnd);
        if (v) return (int)v;
    }
    return system_dpi();
}

static int S(const debugger *d, int v) { return MulDiv(v, d->dpi, 96); }

/* The message font at dpi (system metrics come at the system DPI), at its
 * own size (px 0) or px pixels at 96 dpi; face NULL keeps the message
 * font's, otherwise a fixed-pitch face (Consolas). */
static HFONT make_font(int dpi, int px, int weight, const char *face)
{
    NONCLIENTMETRICSA ncm;
    LOGFONTA lf;
    ZeroMemory(&ncm, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0))
        lf = ncm.lfMessageFont;
    else
        GetObjectA(GetStockObject(DEFAULT_GUI_FONT), sizeof lf, &lf);
    if (px > 0) lf.lfHeight = -MulDiv(px, dpi, 96);
    else lf.lfHeight = -MulDiv(abs(lf.lfHeight ? lf.lfHeight : 12), dpi, system_dpi());
    lf.lfWidth = 0;
    lf.lfWeight = weight;
    if (face) {
        snprintf(lf.lfFaceName, sizeof lf.lfFaceName, "%s", face);
        lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    }
    return CreateFontIndirectA(&lf);
}

static BOOL CALLBACK refont_child(HWND h, LPARAM lp)
{
    const HFONT *map = (const HFONT *)lp;     /* old ui, bold, mono; new ui, bold, mono */
    const HFONT f = (HFONT)SendMessageA(h, WM_GETFONT, 0, 0);
    int i;
    for (i = 0; i < 3; i++)
        if (f && f == map[i]) SendMessageA(h, WM_SETFONT, (WPARAM)map[i + 3], TRUE);
    return TRUE;
}

/* The message font and Consolas at the window's DPI; the controls already
 * made take the new ones. */
static void make_fonts(debugger *d)
{
    HFONT old[3] = { d->ui, d->ui_bold, d->mono };
    HDC dc;
    TEXTMETRICA tm;
    HGDIOBJ prev;
    int i;

    d->ui = make_font(d->dpi, 0, FW_NORMAL, NULL);
    d->ui_bold = make_font(d->dpi, 0, FW_BOLD, NULL);
    d->mono = make_font(d->dpi, MONO_PX, FW_NORMAL, "Consolas");

    /* the monospace cell, for the hand-drawn views */
    dc = GetDC(d->hwnd);
    prev = SelectObject(dc, d->mono);
    d->mono_w = S(d, 7);
    d->mono_h = S(d, 15);
    if (GetTextMetricsA(dc, &tm)) {
        d->mono_w = tm.tmAveCharWidth;
        d->mono_h = tm.tmHeight;
    }
    d->line_h = d->mono_h + S(d, 2);
    SelectObject(dc, prev);
    ReleaseDC(d->hwnd, dc);

    if (old[0]) {
        HFONT map[6] = { old[0], old[1], old[2], d->ui, d->ui_bold, d->mono };
        int p;
        EnumChildWindows(d->hwnd, refont_child, (LPARAM)map);
        for (p = 0; p < TAB_COUNT; p++)
            if (d->popped[p]) EnumChildWindows(d->popped[p], refont_child, (LPARAM)map);
        if (d->video) dbg_video_fonts(d->video, d->ui, d->mono, d->dpi);
    }
    for (i = 0; i < 3; i++)
        if (old[i]) DeleteObject(old[i]);
}

static int text_w(debugger *d, HFONT f, const char *text)
{
    HDC dc = GetDC(d->hwnd);
    HGDIOBJ old = SelectObject(dc, f);
    SIZE sz = { 0, 0 };
    GetTextExtentPoint32A(dc, text, (int)strlen(text), &sz);
    SelectObject(dc, old);
    ReleaseDC(d->hwnd, dc);
    return sz.cx;
}

static int btn_w(debugger *d, const char *text)
{
    const int w = text_w(d, d->ui, text) + S(d, 24);
    return w < S(d, 64) ? S(d, 64) : w;
}

static int in_list(HWND h, const HWND *list, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (list[i] == h) return 1;
    return 0;
}

/* ---- the text views ------------------------------------------------------------ */

/* The rows an EDIT view shows. */
static int edit_rows(debugger *d, HWND h)
{
    RECT c;
    GetClientRect(h, &c);
    return d->mono_h > 0 && c.bottom > d->mono_h ? c.bottom / d->mono_h : 1;
}

/* Whether a log's reader is at its end (it then follows new lines). */
static int edit_at_end(debugger *d, HWND h)
{
    const int first = (int)SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0);
    const int count = (int)SendMessageW(h, EM_GETLINECOUNT, 0, 0);
    return first + edit_rows(d, h) >= count;
}

static void edit_to_end(HWND h)
{
    SendMessageW(h, EM_LINESCROLL, 0, SendMessageW(h, EM_GETLINECOUNT, 0, 0));
}

static void edit_scroll_to(HWND h, int first)
{
    SendMessageW(h, EM_LINESCROLL, 0, first - (int)SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0));
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
static void set_text_at(HWND h, const char *text, int follow)
{
    const int first = (int)SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0);
    wchar_t *w = wide_view_text(text);
    if (!w) return;
    SendMessageW(h, WM_SETREDRAW, FALSE, 0);
    SetWindowTextW(h, w);
    if (follow) edit_to_end(h);
    else edit_scroll_to(h, first);
    SendMessageW(h, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(h, NULL, TRUE);
    free(w);
}

static void refresh_status(debugger *d);

static int notice_showing(debugger *d)
{
    return d->notice[0] && (LONG)(GetTickCount() - d->notice_until) < 0;
}

/* The other frontends' toasts: the message holds the status line a moment. */
static void toast(debugger *d, const char *msg)
{
    snprintf(d->notice, sizeof d->notice, "%s", msg);
    d->notice_until = GetTickCount() + NOTICE_MS;
    refresh_status(d);
}

/* ---- the console ---------------------------------------------------------------- */

/* Long sessions keep the last CONSOLE_KEEP characters, whole lines. */
static void console_trim(debugger *d)
{
    const int len = GetWindowTextLengthW(d->con_out);
    wchar_t *all, *nl;
    int cut;
    if (len <= CONSOLE_KEEP) return;
    all = malloc(((size_t)len + 1) * sizeof *all);
    if (!all) return;
    GetWindowTextW(d->con_out, all, len + 1);
    cut = len - CONSOLE_KEEP * 3 / 4;
    nl = wcschr(all + cut, L'\n');
    if (nl) cut = (int)(nl - all) + 1;
    free(all);
    SendMessageW(d->con_out, EM_SETSEL, 0, (LPARAM)cut);
    SendMessageW(d->con_out, EM_REPLACESEL, FALSE, (LPARAM)L"");
}

/* Onto the console's transcript, which follows its end only while the
 * reader is there. */
static void console_append(debugger *d, const char *text)
{
    const int follow = edit_at_end(d, d->con_out);
    const int first = (int)SendMessageW(d->con_out, EM_GETFIRSTVISIBLELINE, 0, 0);
    wchar_t *w = wide_view_text(text);
    int len;
    if (!w) return;
    len = GetWindowTextLengthW(d->con_out);
    SendMessageW(d->con_out, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(d->con_out, EM_REPLACESEL, FALSE, (LPARAM)w);
    free(w);
    console_trim(d);
    if (follow) edit_to_end(d->con_out);
    else edit_scroll_to(d->con_out, first);
}

/* Command output, ended with a newline. */
static void console_output(debugger *d, const char *text)
{
    const size_t n = strlen(text);
    if (!n) return;
    console_append(d, text);
    if (text[n - 1] != '\n') console_append(d, "\n");
}

/* Messages from Load Symbols and Save land in the console and, since it may
 * not be the tab showing, on the status line. */
static void say(debugger *d, const char *msg)
{
    console_output(d, msg);
    toast(d, msg);
}

/* ---- refreshers -------------------------------------------------------------- */

static void layout(debugger *d);

static void refresh_status(debugger *d)
{
    cocodebug_stop stop;
    char text[260];
    int sourced;

    cocodebug_stop_snapshot(d->dbg, &stop);
    if (notice_showing(d)) {
        snprintf(text, sizeof text, "%s", d->notice);
    } else if (stop.paused) {
        snprintf(text, sizeof text, "Paused%s%s", stop.reason[0] ? ": " : "", stop.reason);
    } else {
        snprintf(text, sizeof text, "Running");
    }
    set_label(d->status, text);
    set_label(d->run_btn, stop.paused ? "Run (F5)" : "Pause (F5)");
    sourced = cocodebug_source_available(d->dbg);
    EnableWindow(d->step_line_btn, sourced);
    EnableWindow(d->over_line_btn, sourced);
    InvalidateRect(d->run_btn, NULL, FALSE);
    InvalidateRect(d->status, NULL, TRUE);
}

/* The strip: every register's box and the CC flags from the stop; blank
 * while the machine runs (the engine's registers are only valid parked). */
static void refresh_cpu(debugger *d)
{
    const cocodebug_regs *regs = &d->regs;
    char buf[240], name[64];
    int idx, show6309;

    d->regs_ok = cocodebug_regs_get(d->dbg, &d->regs);
    show6309 = d->regs_ok ? regs->is6309 != 0 : d->is6309;
    if (show6309 != d->is6309) {
        d->is6309 = show6309;
        for (idx = NREGS_6809; idx < NREGS; idx++) {
            ShowWindow(d->reg_label[idx], show6309 ? SW_SHOW : SW_HIDE);
            ShowWindow(d->reg[idx], show6309 ? SW_SHOW : SW_HIDE);
        }
        layout(d);
    }
    if (d->regs_ok) {
        const unsigned value[NREGS] = { regs->pc, regs->a, regs->b, (unsigned)(regs->a << 8 | regs->b), regs->dp, regs->x,
                                        regs->y, regs->u, regs->s, regs->cc, regs->e, regs->f, regs->w, regs->v,
                                        regs->md };
        for (idx = 0; idx < NREGS; idx++) {
            if (GetFocus() == d->reg[idx]) continue;      /* do not fight the user's typing */
            snprintf(buf, sizeof buf, "%0*X", k_reg_digits[idx], value[idx] & ((1u << (4 * k_reg_digits[idx])) - 1u));
            set_label(d->reg[idx], buf);
        }
    } else {
        for (idx = 0; idx < NREGS; idx++)
            if (GetFocus() != d->reg[idx]) set_label(d->reg[idx], "");
    }
    d->updating = 1;
    for (idx = 0; idx < NCHECKS; idx++)
        SendMessageA(d->chk[idx], BM_SETCHECK, d->regs_ok && (regs->cc & k_chk_masks[idx]) ? BST_CHECKED : BST_UNCHECKED, 0);
    d->updating = 0;
    if (d->regs_ok) {
        name[0] = '\0';
        cocodebug_symbol_at(d->dbg, regs->pc, name, sizeof name);
        snprintf(buf, sizeof buf, "%s   PC $%04X%s%s", regs->is6309 ? "HD6309" : "MC6809", regs->pc, name[0] ? "  " : "",
                 name);
    } else {
        snprintf(buf, sizeof buf, "running: the registers are those of a stop");
    }
    set_label(d->info, buf);
}

/* ---- the memory dump ------------------------------------------------------------ */

/* An EDIT cannot give a byte range its own background, so the dump is a
 * small window class: a header line and a row of 16 bytes a line, painted by
 * hand, scrolled on its own scroll bar, the highlighted range (Go to, Find,
 * a watch) in the accent. It shows the space selected, and only the rows on
 * screen are read, so a 2M physical space costs nothing. */
static int mem_lines(const debugger *d)
{
    return (int)((d->mem_size + 15) / 16) + 1;
}

static int mem_addr_w(const debugger *d)
{
    return d->mem_digits + 2;
}

static int mem_vis_rows(debugger *d)
{
    RECT c;
    GetClientRect(d->mem_view, &c);
    return d->line_h > 0 && c.bottom > d->line_h ? c.bottom / d->line_h : 1;
}

static void mem_sync_scroll(debugger *d)
{
    SCROLLINFO info;
    const int vis = mem_vis_rows(d), total = mem_lines(d);
    int max_first = total - vis;
    if (max_first < 0) max_first = 0;
    if (d->mem_first > max_first) d->mem_first = max_first;
    if (d->mem_first < 0) d->mem_first = 0;
    memset(&info, 0, sizeof info);
    info.cbSize = sizeof info;
    info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    info.nMin = 0;
    info.nMax = total - 1;
    info.nPage = (UINT)vis;
    info.nPos = d->mem_first;
    SetScrollInfo(d->mem_view, SB_VERT, &info, TRUE);
}

static void refresh_mem(debugger *d);

static void mem_scroll_to(debugger *d, int first)
{
    d->mem_first = first;
    mem_sync_scroll(d);
    refresh_mem(d);                 /* the rows now on screen */
    InvalidateRect(d->mem_view, NULL, FALSE);
}

/* A row's address in the space's own notation. */
static int mem_addr_text(const debugger *d, uint32_t base, char *out, size_t cap)
{
    return snprintf(out, cap, "%0*X", d->mem_digits, (unsigned)base);
}

/* The 16 bytes of dump line `line` (1 and up), or NULL when they were not read. */
static const uint8_t *mem_row(const debugger *d, int line)
{
    const int row = line - d->mem_buf_line;
    if (d->mem_buf_line < 0 || d->mem_buf_space != d->mem_sp || row < 0 || row >= d->mem_buf_rows) return NULL;
    return d->mem_buf + row * 16;
}

static void paint_mem(debugger *d, HDC dc, const RECT *c)
{
    const int x0 = S(d, 4), cw = d->mono_w, total = mem_lines(d), aw = mem_addr_w(d);
    int idx, col;
    FillRect(dc, c, GetSysColorBrush(COLOR_WINDOW));
    SelectObject(dc, d->mono);
    SetBkMode(dc, TRANSPARENT);
    for (idx = 0; (d->mem_first + idx) < total && idx * d->line_h < c->bottom; idx++) {
        const int line = d->mem_first + idx, y = idx * d->line_h, ty = y + (d->line_h - d->mono_h) / 2;
        char text[24];
        if (line == 0) {
            SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
            for (col = 0; col < 16; col++) {
                const int len = snprintf(text, sizeof text, "%X", col);
                TextOutA(dc, x0 + cw * (aw + col * 3 + (col >= 8)), ty, text, len);
            }
        } else {
            const uint32_t base = (uint32_t)(line - 1) * 16;
            const uint8_t *row = mem_row(d, line);
            const int hl_here = d->mem_hl && d->mem_hl_space == d->mem_sp;
            int len = mem_addr_text(d, base, text, sizeof text);
            SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
            TextOutA(dc, x0, ty, text, len);
            for (col = 0; col < 16; col++) {
                const int hl = hl_here && base + (uint32_t)col >= d->mem_hl_lo && base + (uint32_t)col <= d->mem_hl_hi;
                const int bx = x0 + cw * (aw + col * 3 + (col >= 8));
                const int ax = x0 + cw * (aw + 16 * 3 + 2 + col);
                char ascii = row ? (char)(row[col] >= 0x20 && row[col] < 0x7F ? row[col] : '.') : ' ';
                if (base + (uint32_t)col >= d->mem_size) break;
                if (hl) {
                    RECT b1 = { bx - 1, y, bx + 2 * cw + 1, y + d->line_h };
                    RECT b2 = { ax, y, ax + cw, y + d->line_h };
                    FillRect(dc, &b1, coco_accent_brush());
                    FillRect(dc, &b2, coco_accent_brush());
                    SetTextColor(dc, COCO_ACCENT_TEXT);
                } else {
                    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
                }
                len = row ? snprintf(text, sizeof text, "%02X", row[col]) : snprintf(text, sizeof text, "..");
                TextOutA(dc, bx, ty, text, len);
                TextOutA(dc, ax, ty, &ascii, 1);
            }
            {
                char region[64];
                const int rx = x0 + cw * (aw + 16 * 3 + 2 + 16 + 2);
                region[0] = '\0';
                if (d->mem_sp == COCODEBUG_SPACE_CPU)
                    cocomem_region_name(d->mem_map, d->mem_map_count, (uint16_t)base, region, sizeof region);
                SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
                TextOutA(dc, rx, ty, region, (int)strlen(region));
            }
        }
    }
}

static LRESULT CALLBACK memview_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;
    if (!d) return DefWindowProcA(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps), mem;
        RECT c;
        HBITMAP bmp;
        HGDIOBJ oldbmp;
        GetClientRect(hwnd, &c);
        mem = CreateCompatibleDC(dc);
        bmp = CreateCompatibleBitmap(dc, c.right > 0 ? c.right : 1, c.bottom > 0 ? c.bottom : 1);
        oldbmp = SelectObject(mem, bmp);
        paint_mem(d, mem, &c);
        BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldbmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_PRINTCLIENT: {
        RECT c;
        GetClientRect(hwnd, &c);
        paint_mem(d, (HDC)wp, &c);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (d->mem_view == hwnd) {
            mem_sync_scroll(d);
            refresh_mem(d);
        }
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        return 0;
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int rows = -delta * 3 / WHEEL_DELTA;
        if (!rows) rows = delta > 0 ? -1 : 1;
        mem_scroll_to(d, d->mem_first + rows);
        return 0;
    }
    case WM_VSCROLL: {
        SCROLLINFO info;
        const int vis = mem_vis_rows(d);
        int first = d->mem_first;
        memset(&info, 0, sizeof info);
        info.cbSize = sizeof info;
        info.fMask = SIF_TRACKPOS;
        GetScrollInfo(hwnd, SB_VERT, &info);
        switch (LOWORD(wp)) {
        case SB_LINEUP: first--; break;
        case SB_LINEDOWN: first++; break;
        case SB_PAGEUP: first -= vis; break;
        case SB_PAGEDOWN: first += vis; break;
        case SB_TOP: first = 0; break;
        case SB_BOTTOM: first = mem_lines(d); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: first = info.nTrackPos; break;
        default: break;
        }
        mem_scroll_to(d, first);
        return 0;
    }
    case WM_KEYDOWN: {
        const int vis = mem_vis_rows(d);
        int first = d->mem_first;
        switch (wp) {
        case VK_UP: first--; break;
        case VK_DOWN: first++; break;
        case VK_PRIOR: first -= vis; break;
        case VK_NEXT: first += vis; break;
        case VK_HOME: first = 0; break;
        case VK_END: first = mem_lines(d); break;
        default: return DefWindowProcA(hwnd, msg, wp, lp);
        }
        mem_scroll_to(d, first);
        return 0;
    }
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* Reads the rows on screen (and a few around them) of the space showing; a
 * running machine keeps what the last stop read. */
static void refresh_mem(debugger *d)
{
    static const int digits[NSPACES] = { 4, 6 };
    uint32_t size = cocodebug_space_size(d->dbg, d->mem_sp);
    int vis, first_line, rows;

    d->mem_digits = digits[d->mem_sp];
    d->mem_size = size;
    if (d->mem_sp == COCODEBUG_SPACE_CPU) {
        d->mem_map_count = cocodebug_memory_map(d->dbg, d->mem_map, MEM_REGION_MAX);
        if (d->mem_map_count < 0) d->mem_map_count = 0;
    }
    if (d->mem_recentre && cocodebug_is_paused(d->dbg)) {
        /* a new stop: the CPU space to the PC, a third of the way down */
        d->mem_recentre = 0;
        if (d->mem_sp == COCODEBUG_SPACE_CPU && d->regs_ok)
            d->mem_first = 1 + d->regs.pc / 16 - mem_vis_rows(d) / 3;
    }
    mem_sync_scroll(d);
    vis = mem_vis_rows(d);
    first_line = d->mem_first > 1 ? d->mem_first : 1;
    rows = vis + 2;
    if (rows > MEM_BUF_ROWS) rows = MEM_BUF_ROWS;
    if (cocodebug_is_paused(d->dbg) && size > 0) {
        const uint32_t from = (uint32_t)(first_line - 1) * 16;
        const int want = (int)(size - from > (uint32_t)rows * 16 ? (uint32_t)rows * 16 : size - from);
        memset(d->mem_buf, 0, sizeof d->mem_buf);
        if (from < size && want > 0 && cocodebug_mem_read(d->dbg, d->mem_sp, from, d->mem_buf, want) > 0) {
            d->mem_buf_line = first_line;
            d->mem_buf_rows = (want + 15) / 16;
            d->mem_buf_space = d->mem_sp;
        }
    }
    InvalidateRect(d->mem_view, NULL, FALSE);
}

/* The Memory tab shows `space`. */
static void set_mem_space(debugger *d, int space)
{
    if (space < 0 || space >= NSPACES) return;
    if (space == COCODEBUG_SPACE_PHYS && cocodebug_space_size(d->dbg, COCODEBUG_SPACE_PHYS) == 0) {
        toast(d, "PHYS is the CoCo 3's physical RAM: this machine has none");
        space = COCODEBUG_SPACE_CPU;
    }
    if (space != d->mem_sp) {
        d->mem_sp = space;
        d->mem_first = 0;
        d->mem_recentre = 1;
        d->mem_buf_line = -1;
    }
    d->updating = 1;
    SendMessageA(d->mem_space, CB_SETCURSEL, (WPARAM)space, 0);
    d->updating = 0;
    set_label(d->mem_title, k_space_titles[space]);
    layout(d);
    refresh_mem(d);
}

/* lo..hi of `space` highlighted and scrolled to in the dump (a Go to, a Find,
 * a watch's Memory button). */
static void show_in_memory(debugger *d, int space, uint32_t lo, uint32_t hi)
{
    int line;
    d->mem_hl = 1;
    d->mem_hl_space = space;
    d->mem_hl_lo = lo;
    d->mem_hl_hi = hi;
    d->mem_recentre = 0;
    if (space != d->mem_sp) set_mem_space(d, space);
    /* the row 30% of the way down */
    line = 1 + (int)(lo / 16) - mem_vis_rows(d) * 3 / 10;
    mem_scroll_to(d, line > 0 ? line : 0);
}

/* The disassembly rows the view has room for. */
static int disasm_rows(debugger *d)
{
    RECT c;
    int rows;
    if (!d->disasm || d->line_h <= 0) return 1;
    GetClientRect(d->disasm, &c);
    rows = (c.bottom - c.top) / d->line_h;
    if (rows < 1) rows = 1;
    if (rows > DISASM_MAX) rows = DISASM_MAX;
    return rows;
}

static int row_address(debugger *d, int addr, int rows)
{
    if (!cocodebug_is_paused(d->dbg)) return addr;
    return cocodebug_row_address(d->dbg, (uint16_t)addr, rows) & 0xFFFF;
}

static int following(debugger *d)
{
    return SendMessageA(d->follow_pc, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

/* The lines from disasm_top. A running machine cannot be read, so the lines
 * of the last stop stay (the PC bar is dropped by paint_disasm). */
static void refresh_disasm(debugger *d)
{
    int pc_line = -1, count;
    const int rows = disasm_rows(d);

    if (!cocodebug_is_paused(d->dbg)) {
        d->pc_line = -1;
        InvalidateRect(d->disasm, NULL, FALSE);
        return;
    }
    if ((following(d) || d->recentre) && d->regs_ok) {
        /* the PC a third of the way down, so what led to it shows too; after a
         * stop, even if the user had scrolled away */
        d->disasm_top = row_address(d, d->regs.pc, -(rows / 3));
    }
    d->recentre = 0;
    count = cocodebug_disassemble_ex(d->dbg, (uint16_t)d->disasm_top, d->lines, rows, &pc_line);
    d->line_count = count < 0 ? 0 : count;
    d->pc_line = pc_line;
    InvalidateRect(d->disasm, NULL, FALSE);
}

static void lv_set(HWND lv, int row, int col, const char *text)
{
    wchar_t w[256];
    LVITEMW it;
    wide(text, w, 256);
    memset(&it, 0, sizeof it);
    it.iSubItem = col;
    it.pszText = w;
    SendMessageW(lv, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&it);
}

static int page_visible(debugger *d, int page)
{
    return d->popped[page] != NULL || d->page == page;
}

/* The list, or (empty) the hint in its place. */
static void show_bp_list(debugger *d)
{
    const int on = page_visible(d, TAB_BREAKS);
    ShowWindow(d->bp_list, on && d->nbps > 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(d->bp_empty, on && d->nbps == 0 ? SW_SHOW : SW_HIDE);
}

/* "Paused: <why>" / "Running." over the list. */
static void refresh_bp_stop(debugger *d)
{
    cocodebug_stop stop;
    char text[200];
    cocodebug_stop_snapshot(d->dbg, &stop);
    if (stop.paused) snprintf(text, sizeof text, stop.reason[0] ? "Paused: %s" : "Paused.", stop.reason);
    else snprintf(text, sizeof text, "Running.");
    set_label(d->bp_stop, text);
}

/* Rebuilt only when something in it changed (each step refreshes), keeping
 * the selected breakpoint selected. */
static void refresh_bps(debugger *d)
{
    static char sig[sizeof d->bp_sig];
    size_t len = 0;
    int idx, sel_id = -1;
    const int sel = (int)SendMessageA(d->bp_list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);

    refresh_bp_stop(d);
    if (sel >= 0 && sel < d->nbps) sel_id = d->bps[sel].id;
    d->nbps = cocodebug_breakpoints_list(d->dbg, d->bps, MAX_BPS);
    if (d->nbps < 0) d->nbps = 0;
    sig[0] = '\0';
    for (idx = 0; idx < d->nbps; idx++) {
        char desc[256];
        cocobp_describe(d->dbg, &d->bps[idx], desc, sizeof desc);
        len = addf(sig, len, sizeof sig, "%d %d %u %s\n", d->bps[idx].id, d->bps[idx].enabled, d->bps[idx].hits, desc);
    }
    if (d->bp_sig_ok && strcmp(sig, d->bp_sig) == 0) { show_bp_list(d); return; }
    memcpy(d->bp_sig, sig, sizeof sig);
    d->bp_sig_ok = 1;

    d->updating = 1;
    SendMessageA(d->bp_list, LVM_DELETEALLITEMS, 0, 0);
    for (idx = 0; idx < d->nbps; idx++) {
        const cocodebug_breakpoint *bpt = &d->bps[idx];
        LVITEMA item;
        char id[16], desc[256], hits[24];
        snprintf(id, sizeof id, "#%d", bpt->id);
        cocobp_describe(d->dbg, bpt, desc, sizeof desc);
        snprintf(hits, sizeof hits, "%u", bpt->hits);
        memset(&item, 0, sizeof item);
        item.mask = LVIF_TEXT;
        item.iItem = idx;
        item.pszText = id;
        SendMessageA(d->bp_list, LVM_INSERTITEMA, 0, (LPARAM)&item);
        lv_set(d->bp_list, idx, 1, desc);
        lv_set(d->bp_list, idx, 2, hits);
        memset(&item, 0, sizeof item);
        item.stateMask = LVIS_STATEIMAGEMASK | LVIS_SELECTED | LVIS_FOCUSED;
        item.state = INDEXTOSTATEIMAGEMASK(bpt->enabled ? 2 : 1);
        if (bpt->id == sel_id) item.state |= LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageA(d->bp_list, LVM_SETITEMSTATE, (WPARAM)idx, (LPARAM)&item);
    }
    d->updating = 0;
    show_bp_list(d);
}

/* ---- watches ---------------------------------------------------------------------- */

/* The rows are rebuilt when a value or the list changed, keeping the
 * selection. */
static void refresh_watches(debugger *d)
{
    static char sig[sizeof d->wt_sig];
    const int n = cocowatch_count();
    const int keep = (int)SendMessageA(d->wt_list, LB_GETCURSEL, 0, 0);
    char lines[COCOWATCH_MAX][120];
    size_t len = 0;
    int i;

    sig[0] = '\0';
    for (i = 0; i < n; i++) {
        cocowatch_format(d->dbg, i, lines[i], sizeof lines[i]);
        len = addf(sig, len, sizeof sig, "%s\n", lines[i]);
    }
    if (d->wt_sig_ok && strcmp(sig, d->wt_sig) == 0) return;
    memcpy(d->wt_sig, sig, sizeof sig);
    d->wt_sig_ok = 1;
    SendMessageA(d->wt_list, WM_SETREDRAW, FALSE, 0);
    SendMessageA(d->wt_list, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < n; i++) SendMessageA(d->wt_list, LB_ADDSTRING, 0, (LPARAM)lines[i]);
    if (keep >= 0 && keep < n) SendMessageA(d->wt_list, LB_SETCURSEL, (WPARAM)keep, 0);
    SendMessageA(d->wt_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(d->wt_list, NULL, TRUE);
}

/* ---- cart & I/O ------------------------------------------------------------------- */

static size_t pia_text(char *text, size_t len, size_t cap, const char *name, const char *base, const cocodebug_pia *pia)
{
    len = addf(text, len, cap, "%s (%s)\n", name, base);
    len = addf(text, len, cap, "  CRA %02X  DDRA %02X  ORA %02X  pins A %02X   CA1 %d CA2 %d   IRQ A %s%s\n", pia->cra, pia->ddra,
               pia->ora, pia->pina, pia->ca1, pia->ca2, pia->irqa ? "raised" : "clear",
               pia->irq_flags_a ? " (flag set)" : "");
    len = addf(text, len, cap, "  CRB %02X  DDRB %02X  ORB %02X  pins B %02X   CB1 %d CB2 %d   IRQ B %s%s\n", pia->crb, pia->ddrb,
               pia->orb, pia->pinb, pia->cb1, pia->cb2, pia->irqb ? "raised" : "clear",
               pia->irq_flags_b ? " (flag set)" : "");
    return len;
}

static void refresh_cart(debugger *d)
{
    static char text[16384], current[16384], io_text[16384], io_current[16384];
    cocodebug_io io;
    size_t len = 0;
    int idx;
    char info[256];

    if (!cocodebug_is_paused(d->dbg)) return;       /* the engine reads these parked */
    cocodebug_cart_info(d->dbg, info, sizeof info);
    d->mem_map_count = cocodebug_memory_map(d->dbg, d->mem_map, MEM_REGION_MAX);
    if (d->mem_map_count < 0) d->mem_map_count = 0;
    len = addf(text, len, sizeof text, "%s\n\nMemory map (CPU, as mapped now)\n", info);
    for (idx = 0; idx < d->mem_map_count; idx++) {
        const cocodebug_region *reg = &d->mem_map[idx];
        static const char *const kinds[5] = { "unmapped", "RAM", "ROM", "cart ROM", "I/O" };
        len = addf(text, len, sizeof text, "  %04X-%04X  %-9s %s  %s", reg->lo, reg->hi,
                   reg->kind >= 0 && reg->kind < 5 ? kinds[reg->kind] : "?", reg->writable ? "rw" : "r-", reg->name);
        if (reg->physical_base >= 0) len = addf(text, len, sizeof text, "   PHYS $%06X", (unsigned)reg->physical_base);
        len = addf(text, len, sizeof text, "\n");
    }
    if (strcmp(text, current) != 0) {
        memcpy(current, text, sizeof current);
        set_text_at(d->cart_text, text, 0);
    }

    len = 0;
    if (!cocodebug_io_get(d->dbg, &io)) {
        len = addf(io_text, len, sizeof io_text, "(no I/O state yet)\n");
    } else {
        len = addf(io_text, len, sizeof io_text, "%s, %s\n\n", io.is_coco3 ? "CoCo 3 (GIME)" : "CoCo 1/2 (SAM and VDG)",
                   io.is6309 ? "HD6309" : "MC6809");
        len = pia_text(io_text, len, sizeof io_text, "PIA0", "$FF00-$FF03: keyboard, joystick comparator, HSYNC/FSYNC",
                       &io.pia0);
        len = pia_text(io_text, len, sizeof io_text, "PIA1", "$FF20-$FF23: DAC, cassette, VDG mode, cartridge FIRQ",
                       &io.pia1);
        if (io.sam_valid)
            len = addf(io_text, len, sizeof io_text,
                       "\nSAM  V %u  F %u  P %u  R %u  M %u  TY %u   display address $%04X\n", io.sam_v, io.sam_f, io.sam_p,
                       io.sam_r, io.sam_m, io.sam_ty, io.sam_video_address);
        if (io.gime_valid) {
            int row, col;
            len = addf(io_text, len, sizeof io_text, "\nGIME  IRQ %s  FIRQ %s   scanline %u  row %u\n",
                       io.gime_irq ? "raised" : "clear", io.gime_firq ? "raised" : "clear", io.gime_scanline,
                       io.gime_row);
            len = addf(io_text, len, sizeof io_text, "  registers $FF90-$FF9F\n   ");
            for (col = 0; col < 16; col++) len = addf(io_text, len, sizeof io_text, " %02X", io.gime_reg[col]);
            len = addf(io_text, len, sizeof io_text, "\n  palette $FFB0-$FFBF (6-bit)\n   ");
            for (col = 0; col < 16; col++) len = addf(io_text, len, sizeof io_text, " %02X", io.gime_palette[col]);
            len = addf(io_text, len, sizeof io_text, "\n  MMU $FFA0-$FFAF (task 0, then task 1)\n   ");
            for (row = 0; row < 16; row++) len = addf(io_text, len, sizeof io_text, " %02X", io.gime_mmu[row]);
            len = addf(io_text, len, sizeof io_text, "\n");
        }
        len = addf(io_text, len, sizeof io_text, "\nJoysticks\n");
        for (idx = 0; idx < 2; idx++)
            len = addf(io_text, len, sizeof io_text, "  %-5s axis %5d %5d   buttons %d %d\n", idx ? "left" : "right",
                       io.joy_axis[idx][0], io.joy_axis[idx][1], io.joy_button[idx][0], io.joy_button[idx][1]);
        len = addf(io_text, len, sizeof io_text, "\nBecker (FujiNet) link %s, %d byte%s waiting for the CoCo\n",
                   io.becker_link_up ? "up" : "down", io.becker_rx_pending, io.becker_rx_pending == 1 ? "" : "s");
    }
    if (strcmp(io_text, io_current) != 0) {
        memcpy(io_current, io_text, sizeof io_current);
        set_text_at(d->cart_io, io_text, 0);
    }
}

/* The sound the PIA1 DAC and single-bit port make: the live levels, and the
 * writes to them (recorded only while this tab shows). */
static void refresh_sound(debugger *d)
{
    static char text[2048], current[2048], log[EVENTS_MAX * 80 + 64], log_current[EVENTS_MAX * 80 + 64];
    cocodebug_sound snd;
    size_t len = 0;
    int idx, count;

    if (cocodebug_sound_get(d->dbg, &snd)) {
        len = addf(text, len, sizeof text, "DAC           %2d of 63   (PIA1 A bits 2-7, $FF20)\n", snd.dac);
        len = addf(text, len, sizeof text, "Single bit    %d           (PIA1 B bit 1, $FF22)\n", snd.single_bit);
        len = addf(text, len, sizeof text, "Sound enable  %s     (PIA1 CB2: the multiplexer's output reaches the speaker)\n",
                   snd.sound_enable ? "on " : "off");
        len = addf(text, len, sizeof text, "Mux source    %s        (PIA0 CB2:CA2)\n", snd.mux_name ? snd.mux_name : "?");
        len = addf(text, len, sizeof text, "Cassette      motor %s, data in %d\n", snd.motor ? "on" : "off", snd.cassette_in);
        SendMessageA(d->sound_level, PBM_SETPOS, (WPARAM)snd.dac, 0);
    } else {
        len = addf(text, len, sizeof text, "(stop the machine to read the sound state)\n");
    }
    if (strcmp(text, current) != 0) {
        memcpy(current, text, sizeof current);
        set_text_at(d->sound_text, text, 0);
    }

    count = cocodebug_sound_history(d->dbg, d->snd_events, EVENTS_MAX);
    if (count < 0) count = 0;
    len = 0;
    log[0] = '\0';
    for (idx = count - 1; idx >= 0; idx--) {         /* oldest first */
        const cocodebug_sound_write *evt = &d->snd_events[idx];
        len = addf(log, len, sizeof log, "%stick %-10u  $FF2%d = %02X   DAC %2d", idx == count - 1 ? "" : "\n",
                   (unsigned)evt->tick, evt->port, evt->value, evt->dac);
    }
    if (count == 0) len = addf(log, len, sizeof log, "(no writes to $FF20 or $FF22 since this tab was shown)");
    if (strcmp(log, log_current) != 0) {
        const int follow = edit_at_end(d, d->sound_log);
        memcpy(log_current, log, sizeof log_current);
        set_text_at(d->sound_log, log, follow);
    }
}

/* The engine records the DAC writes only while a Sound tab shows. */
static void sound_watch_sync(debugger *d)
{
    const int want = IsWindowVisible(d->hwnd) && page_visible(d, TAB_SOUND);
    if (want != d->snd_watching) {
        d->snd_watching = want;
        cocodebug_sound_watch(d->dbg, want);
    }
}

/* The symbol table, filtered by the box: address and name, in the engine's
 * order (address, then name). The indices are fetched afresh every time: the
 * engine reorders them when symbols are loaded or cleared. */
static void refresh_symbols(debugger *d)
{
    char filter[96], signature[128], count_text[96];
    int total, shown, idx;

    edit_text(d->sym_filter, filter, sizeof filter);
    total = cocodebug_symbols_count(d->dbg);
    shown = cocodebug_symbols_filter(d->dbg, strip(filter), d->sym_indices, SYMBOL_MAX);
    if (shown < 0) shown = 0;
    snprintf(signature, sizeof signature, "%d %d %s", total, shown, filter);
    if (d->sym_shown >= 0 && strcmp(signature, d->sym_sig) == 0 && !d->sym_force) return;
    d->sym_force = 0;
    snprintf(d->sym_sig, sizeof d->sym_sig, "%s", signature);
    d->sym_shown = shown;
    SendMessageA(d->sym_list, WM_SETREDRAW, FALSE, 0);
    SendMessageA(d->sym_list, LVM_DELETEALLITEMS, 0, 0);
    for (idx = 0; idx < shown; idx++) {
        uint16_t address = 0;
        char name[96], address_text[16];
        LVITEMA item;
        if (!cocodebug_symbols_get(d->dbg, d->sym_indices[idx], &address, name, sizeof name)) continue;
        snprintf(address_text, sizeof address_text, "%04X", address);
        memset(&item, 0, sizeof item);
        item.mask = LVIF_TEXT;
        item.iItem = idx;
        item.pszText = address_text;
        SendMessageA(d->sym_list, LVM_INSERTITEMA, 0, (LPARAM)&item);
        lv_set(d->sym_list, idx, 1, name);
    }
    SendMessageA(d->sym_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(d->sym_list, NULL, TRUE);
    snprintf(count_text, sizeof count_text, "%d of %d symbol%s", shown, total, total == 1 ? "" : "s");
    set_label(d->sym_count, count_text);
}

/* ---- trace and history ------------------------------------------------------------- */

/* Both tabs read the engine's one ring (newest first) and show the newest
 * HIST_SHOWN matching entries, oldest first. Record on either toggles the
 * same recorder. */
static void hist_refresh(debugger *d, int idx, int force)
{
    static cocodebug_trace_entry ring[HIST_RING];
    static int hit[HIST_SHOWN];
    static char text[HIST_SHOWN * 120];
    const int on = cocodebug_trace_enabled(d->dbg);
    const int total = cocodebug_trace_count(d->dbg);
    uint16_t lo = 0, hi = 0xFFFF;
    int filter = COCODEBUG_TRACE_ALL, n, i, shown = 0, k;
    uint64_t sig;
    size_t len = 0;
    char note[120], range[96], err[96];

    for (k = 0; k < 2; k++) {                /* both Record buttons follow the engine */
        const int was = SendMessageA(d->h_rec[k], BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (was != (on != 0)) {
            SendMessageA(d->h_rec[k], BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
            InvalidateRect(d->h_rec[k], NULL, FALSE);
        }
    }
    switch ((int)SendMessageA(d->h_filter[idx], CB_GETCURSEL, 0, 0)) {
    case 1: filter = COCODEBUG_TRACE_BRANCHES; break;
    case 2:
        edit_text(d->h_range[idx], range, sizeof range);
        if (parse_range_ui(d, range, &lo, &hi, err, sizeof err)) filter = COCODEBUG_TRACE_RANGE;
        else {
            set_label(d->h_note[idx], err);
            return;
        }
        break;
    default: break;
    }
    n = total > 0 ? cocodebug_trace_read(d->dbg, ring, HIST_RING) : 0;
    /* what the view shows depends on the newest entry, the count and the filter */
    sig = (uint64_t)n * 1000003u + (n ? ring[0].cycles * 31u + ring[0].pc : 0) + (uint64_t)filter * 7919u +
          lo * 131u + hi + (uint64_t)on * 17u;
    if (!force && d->h_ok[idx] && sig == d->h_sig[idx]) return;
    d->h_sig[idx] = sig;
    d->h_ok[idx] = 1;
    for (i = 0; i < n && shown < HIST_SHOWN; i++)       /* newest matches first */
        if (cocodebug_trace_match(&ring[i], filter, lo, hi)) hit[shown++] = i;
    text[0] = '\0';
    for (i = shown - 1; i >= 0; i--) {                   /* oldest first */
        char line[160];
        cocodebug_trace_format(&ring[hit[i]], line, sizeof line);
        len = addf(text, len, sizeof text, "%s\n", line);
    }
    if (!shown) len = addf(text, len, sizeof text, on ? "(nothing recorded yet)" : "(recording is off: press Record)");
    set_text_at(d->h_text[idx], text, 1);
    if (!on && n == 0) snprintf(note, sizeof note, "recording off");
    else snprintf(note, sizeof note, "%s%d instruction%s recorded, %d shown", on ? "Recording. " : "", n, n == 1 ? "" : "s",
                  shown);
    set_label(d->h_note[idx], note);
}

/* The packet log is rebuilt only when it changed: a new packet, or a
 * repeat (which moves the newest serial on), or the READY filter. */
static void refresh_fujinet(debugger *d, int force)
{
    static coco_fuji_packet p[TRACE_MAX];
    static char t[TRACE_MAX * 320 + 128];   /* an entry is at most ~300 */
    static char state[4096];
    char note[200];
    size_t len = 0;
    const int count = cocodebug_fujinet_trace(d->dbg, p, TRACE_MAX);
    const int hide = SendMessageA(d->fuji_hide, BM_GETCHECK, 0, 0) == BST_CHECKED;
    int idx, follow, shown = 0;

    if (!cocosession_fujinet_running(d->session) && !cocosession_fujinet_external(d->session)) {
        snprintf(note, sizeof note, "FujiNet is off (Machine \xe2\x96\xb8 Settings).");
    } else {
        snprintf(note, sizeof note, "%s FujiNet on the Becker port %s, %s.",
                 cocosession_fujinet_external(d->session) ? "External" : "Built-in",
                 cocosession_becker_endpoint(d->session),
                 cocosession_fujinet_link(d->session) ? "connected" : "not connected");
    }
    set_label(d->fuji_note, note);
    state[0] = '\0';
    cocodebug_fujinet_state(d->dbg, state, (int)sizeof state);
    set_text_at(d->fuji_state, state, 0);

    if (!force && count == d->fuji_count && (count == 0 || p[count - 1].serial == d->fuji_last)) return;
    follow = force || d->fuji_count < 0 || edit_at_end(d, d->fuji_text);
    d->fuji_count = count;
    d->fuji_last = count > 0 ? p[count - 1].serial : 0;
    t[0] = '\0';
    for (idx = 0; idx < count && len + 400 < sizeof t; idx++) {
        int byte;
        if (hide && cocodebug_fujinet_is_ready(&p[idx])) continue;
        len = addf(t, len, sizeof t, "%s%s\n     ", shown++ ? "\n" : "", p[idx].text);
        for (byte = 0; byte < p[idx].nbytes && byte < (int)sizeof p[idx].data; byte++)
            len = addf(t, len, sizeof t, "%02X ", p[idx].data[byte]);
        if (p[idx].nbytes > (int)sizeof p[idx].data) len = addf(t, len, sizeof t, "\xe2\x80\xa6");
    }
    if (shown == 0)
        len = addf(t, len, sizeof t, "%s", count ? "(only READY probes since the trace was cleared)"
                                               : "(no DriveWire traffic since the trace was cleared)");
    set_text_at(d->fuji_text, t, follow);
}

/* ---- the line view (the C listing) -------------------------------------------
 * An EDIT control cannot paint a current-line bar or a breakpoint dot, so the
 * C Source tab's listing is a small window class: it keeps the lines it was
 * given (text, a flag set, a value handed back on a click), paints the PC's
 * line as a bar in the accent colour and a breakpoint as a dot in the gutter,
 * and scrolls on its own scroll bar. A click posts WM_DBG_LINE to the debugger
 * window. */

#define LVF_PC 1
#define LVF_BP 2

typedef struct {
    int count, cap, first;
    char **text;
    unsigned char *flags;
    int *aux;
    HFONT font;
} lineview;

static lineview *view_of(HWND hwnd)
{
    return (lineview *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
}

static int view_row_h(HWND hwnd, const lineview *view)
{
    HDC dc = GetDC(hwnd);
    HGDIOBJ old = SelectObject(dc, view->font ? view->font : GetStockObject(DEFAULT_GUI_FONT));
    TEXTMETRICA metrics;
    int height = 15;
    if (GetTextMetricsA(dc, &metrics)) height = metrics.tmHeight + 2;
    SelectObject(dc, old);
    ReleaseDC(hwnd, dc);
    return height;
}

static int view_rows(HWND hwnd, const lineview *view)
{
    RECT c;
    int rows;
    GetClientRect(hwnd, &c);
    rows = c.bottom / view_row_h(hwnd, view);
    return rows < 1 ? 1 : rows;
}

static void view_sync(HWND hwnd, lineview *view)
{
    SCROLLINFO info;
    const int rows = view_rows(hwnd, view);
    int max_first = view->count - rows;
    if (max_first < 0) max_first = 0;
    if (view->first > max_first) view->first = max_first;
    if (view->first < 0) view->first = 0;
    memset(&info, 0, sizeof info);
    info.cbSize = sizeof info;
    info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    info.nMin = 0;
    info.nMax = view->count > 0 ? view->count - 1 : 0;
    info.nPage = (UINT)rows;
    info.nPos = view->first;
    SetScrollInfo(hwnd, SB_VERT, &info, TRUE);
    InvalidateRect(hwnd, NULL, FALSE);
}

static void view_begin(HWND hwnd)
{
    lineview *view = view_of(hwnd);
    int idx;
    if (!view) return;
    for (idx = 0; idx < view->count; idx++) free(view->text[idx]);
    view->count = 0;
}

static void view_add(HWND hwnd, const char *text, int flags, int aux)
{
    lineview *view = view_of(hwnd);
    char buf[512];
    size_t out = 0;
    if (!view) return;
    for (; *text && out + 5 < sizeof buf; text++) {   /* tabs -> 4 spaces */
        if (*text == '\t') {
            memcpy(buf + out, "    ", 4);
            out += 4;
        } else if (*text != '\r' && *text != '\n') {
            buf[out++] = *text;
        }
    }
    buf[out] = '\0';
    if (view->count == view->cap) {
        const int ncap = view->cap ? view->cap * 2 : 64;
        char **ntext = realloc(view->text, (size_t)ncap * sizeof *ntext);
        unsigned char *nflags = realloc(view->flags, (size_t)ncap);
        int *naux = realloc(view->aux, (size_t)ncap * sizeof *naux);
        if (ntext) view->text = ntext;
        if (nflags) view->flags = nflags;
        if (naux) view->aux = naux;
        if (!ntext || !nflags || !naux) return;
        view->cap = ncap;
    }
    view->text[view->count] = _strdup(buf);
    view->flags[view->count] = (unsigned char)flags;
    view->aux[view->count] = aux;
    view->count++;
}

/* reveal: a line to bring into view (centred if it is off screen), or -1 */
static void view_end(HWND hwnd, int reveal)
{
    lineview *view = view_of(hwnd);
    if (!view) return;
    if (reveal >= 0) {
        const int rows = view_rows(hwnd, view);
        if (reveal < view->first || reveal >= view->first + rows - 1) view->first = reveal - rows / 2;
    }
    view_sync(hwnd, view);
}

static void view_paint(HWND hwnd, const lineview *view, HDC hdc)
{
    RECT c;
    HGDIOBJ oldfont;
    TEXTMETRICA metrics;
    HBRUSH red = CreateSolidBrush(RGB(0xe0, 0x1b, 0x24));
    HPEN redpen = CreatePen(PS_SOLID, 1, RGB(0xe0, 0x1b, 0x24));
    int height = 15, ypos = 0, idx;

    GetClientRect(hwnd, &c);
    FillRect(hdc, &c, GetSysColorBrush(COLOR_WINDOW));
    oldfont = SelectObject(hdc, view && view->font ? view->font : GetStockObject(DEFAULT_GUI_FONT));
    if (GetTextMetricsA(hdc, &metrics)) height = metrics.tmHeight + 2;
    SetBkMode(hdc, TRANSPARENT);
    for (idx = view ? view->first : 0; view && idx < view->count && ypos < c.bottom; idx++, ypos += height) {
        const int flags = view->flags[idx];
        const int is_pc = (flags & LVF_PC) != 0;
        RECT row;
        SetRect(&row, 0, ypos, c.right, ypos + height);
        if (is_pc) {
            FillRect(hdc, &row, coco_accent_brush());
            SetTextColor(hdc, COCO_ACCENT_TEXT);
        } else {
            SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
        }
        TextOutA(hdc, 22, ypos + 1, view->text[idx], (int)strlen(view->text[idx]));
        if (flags & LVF_BP) {
            const int top = ypos + (height - 10) / 2;
            HGDIOBJ oldbrush = SelectObject(hdc, is_pc ? GetStockObject(WHITE_BRUSH) : red);
            HGDIOBJ oldpen = SelectObject(hdc, is_pc ? GetStockObject(BLACK_PEN) : redpen);
            Ellipse(hdc, 6, top, 16, top + 10);
            SelectObject(hdc, oldbrush);
            SelectObject(hdc, oldpen);
        }
    }
    SelectObject(hdc, oldfont);
    DeleteObject(red);
    DeleteObject(redpen);
}

static LRESULT CALLBACK lines_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    lineview *view = view_of(hwnd);
    int idx;

    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)calloc(1, sizeof(lineview)));
        break;
    case WM_NCDESTROY:
        if (view) {
            for (idx = 0; idx < view->count; idx++) free(view->text[idx]);
            free(view->text);
            free(view->flags);
            free(view->aux);
            free(view);
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, 0);
        }
        break;
    case WM_SETFONT:
        if (view) {
            view->font = (HFONT)wp;
            view_sync(hwnd, view);
        }
        return 0;
    case WM_GETFONT:
        return view ? (LRESULT)view->font : 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (view) view_sync(hwnd, view);
        return 0;
    case WM_VSCROLL:
        if (view) {
            const int rows = view_rows(hwnd, view);
            int pos = view->first;
            switch (LOWORD(wp)) {
            case SB_LINEUP: pos--; break;
            case SB_LINEDOWN: pos++; break;
            case SB_PAGEUP: pos -= rows - 1; break;
            case SB_PAGEDOWN: pos += rows - 1; break;
            case SB_TOP: pos = 0; break;
            case SB_BOTTOM: pos = view->count; break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION: {
                SCROLLINFO info;
                memset(&info, 0, sizeof info);
                info.cbSize = sizeof info;
                info.fMask = SIF_TRACKPOS;
                GetScrollInfo(hwnd, SB_VERT, &info);
                pos = info.nTrackPos;
                break;
            }
            default: break;
            }
            view->first = pos;
            view_sync(hwnd, view);
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (view) {
            int rows = -GET_WHEEL_DELTA_WPARAM(wp) * 3 / WHEEL_DELTA;
            if (!rows) rows = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1;
            view->first += rows;
            view_sync(hwnd, view);
        }
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        if (view && g_dbg) {
            const int line = view->first + GET_Y_LPARAM(lp) / view_row_h(hwnd, view);
            if (line >= 0 && line < view->count)
                PostMessageA(g_dbg->hwnd, WM_DBG_LINE, (WPARAM)GetDlgCtrlID(hwnd), (LPARAM)view->aux[line]);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT c;
        HDC mem;
        HBITMAP bmp;
        HGDIOBJ oldbmp;
        GetClientRect(hwnd, &c);
        mem = CreateCompatibleDC(dc);
        bmp = CreateCompatibleBitmap(dc, c.right > 0 ? c.right : 1, c.bottom > 0 ? c.bottom : 1);
        oldbmp = SelectObject(mem, bmp);
        view_paint(hwnd, view, mem);
        BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldbmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- the C Source tab --------------------------------------------------------- */

static void resync(debugger *d);

/* Loads (and caches) file_id's text, splitting it in place: '\n' -> '\0', one
 * pointer per line, a trailing '\r' of CRLF endings trimmed. */
static void load_src_file_cache(debugger *d, int file_id)
{
    const char *path = cocodebug_source_file_path(d->dbg, file_id);
    FILE *file;
    long size;
    int idx, nlines;
    char *ptr;

    if (path && strncmp(path, d->src_path, sizeof d->src_path) == 0 && d->src_buf) return;   /* cached */

    free(d->src_buf);
    free(d->src_text);
    d->src_buf = NULL;
    d->src_text = NULL;
    d->src_lines = 0;
    d->src_path[0] = '\0';
    if (!path) return;

    file = fopen(path, "rb");
    if (!file) return;
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size < 0) {
        fclose(file);
        return;
    }
    d->src_buf = (char *)malloc((size_t)size + 1);
    if (!d->src_buf) {
        fclose(file);
        return;
    }
    size = (long)fread(d->src_buf, 1, (size_t)size, file);
    d->src_buf[size] = '\0';
    fclose(file);

    nlines = 1;
    for (ptr = d->src_buf; *ptr; ptr++)
        if (*ptr == '\n') nlines++;
    d->src_text = (char **)malloc(sizeof(char *) * (size_t)nlines);
    if (!d->src_text) {
        free(d->src_buf);
        d->src_buf = NULL;
        return;
    }
    d->src_text[0] = d->src_buf;
    d->src_lines = 1;
    for (ptr = d->src_buf; *ptr; ptr++) {
        if (*ptr == '\n') {
            *ptr = '\0';
            d->src_text[d->src_lines++] = ptr + 1;
        }
    }
    for (idx = 0; idx < d->src_lines; idx++) {
        const size_t len = strlen(d->src_text[idx]);
        if (len > 0 && d->src_text[idx][len - 1] == '\r') d->src_text[idx][len - 1] = '\0';
    }
    snprintf(d->src_path, sizeof d->src_path, "%s", path);
}

/* The file picker from the engine's file list (selection index = file id). */
static void rebuild_src_files(debugger *d)
{
    const int count = cocodebug_source_file_count(d->dbg);
    int idx;

    SendMessageA(d->src_files, CB_RESETCONTENT, 0, 0);
    for (idx = 0; idx < count; idx++) {
        const char *path = cocodebug_source_file_path(d->dbg, idx);
        SendMessageA(d->src_files, CB_ADDSTRING, 0, (LPARAM)(path ? base_name(path) : ""));
    }
    EnableWindow(d->src_files, count > 0);
    d->src_path[0] = '\0';
    d->src_file = count > 0 ? 0 : -1;
    if (count > 0) SendMessageA(d->src_files, CB_SETCURSEL, 0, 0);
}

/* The current function's parameters and locals. */
static void refresh_locals(debugger *d)
{
    cocodebug_local locals[LOCALS_MAX];
    static char text[LOCALS_MAX * 96];
    size_t len = 0;
    int count, idx;

    text[0] = '\0';
    if (!cocodebug_source_available(d->dbg) || !cocodebug_is_paused(d->dbg)) {
        set_text_at(d->src_locals, text, 0);
        return;
    }
    count = cocodebug_locals_get(d->dbg, locals, LOCALS_MAX);
    if (count <= 0) {
        /* not an error: PC may be before the function's first statement, where there is nothing to show yet */
        set_text_at(d->src_locals, "(no locals in scope)", 0);
        return;
    }
    for (idx = 0; idx < count; idx++) {
        const cocodebug_local *local = &locals[idx];
        if (local->has_value)
            len = addf(text, len, sizeof text, "%-12s %-10s %s= %u ($%X)\n", local->name, local->type,
                       local->is_param ? "(param) " : "", local->value, local->value);
        else
            len = addf(text, len, sizeof text, "%-12s %-10s %s= ?\n", local->name, local->type,
                       local->is_param ? "(param) " : "");
    }
    set_text_at(d->src_locals, text, 0);
}

/* Re-renders the listing when the tab is brought up to date: the lines of the
 * file picked, the PC's bar (when the PC is in it), the breakpoint dots; a
 * new stop shows the PC's file and brings its line into view. */
static void refresh_csource(debugger *d)
{
    cocodebug_stop stop;
    char sig[MAX_PATH + 32];
    int file, line, idx, reveal = -1, fresh_stop;
    const int count = cocodebug_source_file_count(d->dbg);
    const char *first = count > 0 ? cocodebug_source_file_path(d->dbg, 0) : NULL;

    cocodebug_stop_snapshot(d->dbg, &stop);
    snprintf(sig, sizeof sig, "%d %s", count, first ? first : "");
    if (count != (int)SendMessageA(d->src_files, CB_GETCOUNT, 0, 0) || strcmp(sig, d->src_sig) != 0) {
        snprintf(d->src_sig, sizeof d->src_sig, "%s", sig);
        rebuild_src_files(d);
    }
    EnableWindow(d->step_line_btn, count > 0);
    EnableWindow(d->over_line_btn, count > 0);
    if (count == 0) {
        view_begin(d->src_view);
        view_end(d->src_view, -1);
        set_label(d->src_note, "No C source loaded: Load C Source... takes the .lst of a `cmoc -i` build "
                               "(its .map must sit beside the program).");
        set_text_at(d->src_locals, "", 0);
        return;
    }
    if (!stop.paused) {
        /* the machine went: the listing keeps its lines, the bar goes */
        lineview *shown = view_of(d->src_view);
        for (idx = 0; shown && idx < shown->count; idx++) shown->flags[idx] &= (unsigned char)~LVF_PC;
        InvalidateRect(d->src_view, NULL, FALSE);
        set_label(d->src_note, "Running: stop the machine to see the PC's line and the locals.");
        return;
    }
    d->src_pc_file = -1;
    d->src_pc_line = -1;
    if (d->regs_ok && cocodebug_source_line_at(d->dbg, d->regs.pc, &file, &line)) {
        d->src_pc_file = file;
        d->src_pc_line = line;
    }
    fresh_stop = stop.serial != d->src_serial;
    d->src_serial = stop.serial;
    if (fresh_stop && d->src_pc_file >= 0 && d->src_pc_file != d->src_file) {   /* followed into another file */
        d->src_file = d->src_pc_file;
        SendMessageA(d->src_files, CB_SETCURSEL, (WPARAM)d->src_file, 0);
    }
    if (d->src_file < 0 || d->src_file >= count) d->src_file = 0;

    load_src_file_cache(d, d->src_file);
    view_begin(d->src_view);
    for (idx = 0; idx < d->src_lines; idx++) {
        const int number = idx + 1;
        char text[560];
        const int is_pc = d->src_file == d->src_pc_file && number == d->src_pc_line;
        const int has_bp = cocodebug_source_breakpoint_is_set(d->dbg, d->src_file, number) > 0;
        snprintf(text, sizeof text, "%5d  %.500s", number, d->src_text[idx]);
        view_add(d->src_view, text, (is_pc ? LVF_PC : 0) | (has_bp ? LVF_BP : 0), number);
    }
    if (fresh_stop && d->src_file == d->src_pc_file && d->src_pc_line > 0) reveal = d->src_pc_line - 1;
    view_end(d->src_view, reveal);
    if (d->src_lines == 0)
        set_label(d->src_note, "The source file could not be read.");
    else if (d->src_pc_line > 0)
        set_label(d->src_note, d->src_file == d->src_pc_file ? "Click a line for a breakpoint. The bar is the PC."
                                                           : "The PC is in another file. Click a line for a breakpoint.");
    else
        set_label(d->src_note, "The PC is not on a line of C (mid-prologue, assembly or the ROM). Click a line for a breakpoint.");
    refresh_locals(d);
}

/* A click in the listing: a breakpoint on that line of C. */
static void src_click(debugger *d, int line)
{
    int result;
    if (d->src_file < 0) return;
    result = cocodebug_source_breakpoint_toggle(d->dbg, d->src_file, line);
    if (result == -2) toast(d, "That line has no code");
    else if (result < 0) toast(d, "The breakpoint table is full");
    resync(d);
}

static void ensure_video(debugger *d)
{
    if (d->video) return;
    d->video = dbg_video_create(d->pg[TAB_VIDEO], d->dbg, d->ui, d->mono, d->dpi);
    if (d->video) layout(d);
}

/* One tab's contents. */
static void refresh_tab(debugger *d, int page)
{
    switch (page) {
    case TAB_DISASM: refresh_disasm(d); break;
    case TAB_CSOURCE: refresh_csource(d); break;
    case TAB_MEMORY: refresh_mem(d); break;
    case TAB_BREAKS: refresh_bps(d); break;
    case TAB_WATCHES: refresh_watches(d); break;
    case TAB_CART: refresh_cart(d); break;
    case TAB_TRACE: hist_refresh(d, 0, 0); break;
    case TAB_HISTORY: hist_refresh(d, 1, 0); break;
    case TAB_VIDEO:
        ensure_video(d);
        if (d->video) dbg_video_refresh(d->video, 0);
        break;
    case TAB_SOUND: refresh_sound(d); break;
    case TAB_SYMBOLS: refresh_symbols(d); break;
    case TAB_FUJINET: refresh_fujinet(d, 0); break;
    default: break;
    }
}

/* The registers strip, and every tab on screen (the one the tab control
 * shows and the popped-out ones); the others catch up when they are shown. */
static void refresh_visible(debugger *d)
{
    int page;
    refresh_cpu(d);
    sound_watch_sync(d);
    for (page = 0; page < TAB_COUNT; page++)
        if (page_visible(d, page)) refresh_tab(d, page);
}

/* At a stop the console says why, as the other frontends' do. */
static void console_stop(debugger *d, const cocodebug_stop *stop)
{
    char line[200];
    if (!stop->reason[0]) return;
    snprintf(line, sizeof line, "\xe2\x80\x94 %s", stop->reason);
    console_output(d, line);
}

/* After anything that may have moved the engine: the status and the tab
 * showing follow it. */
static void resync(debugger *d)
{
    cocodebug_stop stop;
    cocodebug_stop_snapshot(d->dbg, &stop);
    if (stop.paused && !d->was_stopped) console_stop(d, &stop);
    d->was_stopped = stop.paused;
    d->seen_gen = cocodebug_generation(d->dbg);
    d->running_ticks = 0;
    d->h_ticks = 0;
    /* a new stop brings every view to the spot: the Disassembly back to the
     * PC, wherever it was scrolled, the Memory dump to the PC's row, Trace and
     * History to their newest line (they follow the end of their text when
     * rebuilt) */
    if (stop.serial != d->stop_serial) {
        d->stop_serial = stop.serial;
        d->recentre = 1;
        d->mem_recentre = 1;
        d->h_ok[0] = d->h_ok[1] = 0;
    }
    refresh_status(d);
    refresh_visible(d);
}

/* A step takes microseconds: a moment's wait for the stop keeps the window
 * from flashing "Running" in between. */
static void settle_and_sync(debugger *d, int expect_stop)
{
    int idx;
    d->notice[0] = '\0';        /* the machine moved: its new state replaces a notice */
    for (idx = 0; expect_stop && idx < 40 && !cocodebug_is_paused(d->dbg); idx++)
        Sleep(1);
    resync(d);
}

static void tick(debugger *d)
{
    unsigned gen;
    int stopped;
    if (!IsWindowVisible(d->hwnd)) return;
    if (d->notice[0] && !notice_showing(d)) {
        d->notice[0] = '\0';
        refresh_status(d);
    }
    gen = cocodebug_generation(d->dbg);
    stopped = cocodebug_is_paused(d->dbg) != 0;
    if (gen != d->seen_gen || stopped != d->was_stopped) {
        if (stopped != d->was_stopped) d->notice[0] = '\0';   /* a stop (or a go) says so at once */
        resync(d);
    } else if (!stopped) {
        int page;
        if (++d->running_ticks >= 5) {
            /* running: the live values of the tabs showing, twice a second
             * (History waits for a stop; Trace has its own, faster, beat) */
            d->running_ticks = 0;
            refresh_status(d);
            refresh_cpu(d);
            for (page = 0; page < TAB_COUNT; page++)
                if (page_visible(d, page) && page != TAB_HISTORY && page != TAB_TRACE) refresh_tab(d, page);
        }
        if (++d->h_ticks >= HIST_TICKS) {
            d->h_ticks = 0;
            if (page_visible(d, TAB_TRACE)) hist_refresh(d, 0, 0);
        }
    }
}

/* ---- actions ------------------------------------------------------------------ */

static void on_run(debugger *d)
{
    const int paused = cocodebug_is_paused(d->dbg);
    if (paused) cocodebug_resume(d->dbg);
    else cocodebug_pause(d->dbg);
    settle_and_sync(d, !paused);
}

static void on_run_to(debugger *d)
{
    if (d->selected < 0) {
        toast(d, "Select a line in the Disassembly to run to");
        return;
    }
    cocodebug_run_to(d->dbg, (uint16_t)d->selected);
    settle_and_sync(d, 1);
}

/* A breakpoint on a disassembly address; the lines on screen follow at once,
 * even when the machine runs and they are those of the last stop. */
static void toggle_breakpoint(debugger *d, int addr)
{
    int idx;
    cocodebug_breakpoint_toggle(d->dbg, (uint16_t)addr);
    for (idx = 0; idx < d->line_count; idx++)
        if (d->lines[idx].address == addr) {
            d->lines[idx].has_breakpoint = cocodebug_breakpoint_is_set(d->dbg, (uint16_t)addr);
            d->lines[idx].bp_disabled = 0;
        }
    resync(d);
}

/* F9: the breakpoint on the selected disassembly line. */
static void on_toggle_bp(debugger *d)
{
    if (d->selected < 0) {
        toast(d, "Select a line in the Disassembly first");
        return;
    }
    toggle_breakpoint(d, d->selected);
}

static void on_console(debugger *d)
{
    static char out[65536];
    char buf[256], line[300], *cmd;
    const int was = cocodebug_is_paused(d->dbg);

    edit_text(d->con_in, buf, sizeof buf);
    cmd = strip(buf);
    if (!*cmd) return;
    /* history: the newest last, no repeats of the one before */
    if (d->nhistory == 0 || strcmp(d->history[d->nhistory - 1], cmd) != 0) {
        if (d->nhistory == HISTORY_MAX) {
            memmove(d->history[0], d->history[1], sizeof d->history[0] * (HISTORY_MAX - 1));
            d->nhistory--;
        }
        snprintf(d->history[d->nhistory++], sizeof d->history[0], "%s", cmd);
    }
    d->history_pos = d->nhistory;
    snprintf(line, sizeof line, "> %s\n", cmd);
    edit_to_end(d->con_out);     /* a command brings the reader back down */
    console_append(d, line);

    out[0] = '\0';
    cocodebug_command(d->dbg, cmd, out, sizeof out);
    SetWindowTextA(d->con_in, "");
    console_output(d, out);
    settle_and_sync(d, was);
}

/* Up and Down walk the commands already run; past the newest is an empty
 * line again. */
static void history_step(debugger *d, int newer)
{
    int len;
    if (d->nhistory == 0) return;
    d->history_pos += newer ? 1 : -1;
    if (d->history_pos < 0) d->history_pos = 0;
    if (d->history_pos >= d->nhistory) {
        d->history_pos = d->nhistory;
        SetWindowTextA(d->con_in, "");
        return;
    }
    SetWindowTextA(d->con_in, d->history[d->history_pos]);
    len = GetWindowTextLengthA(d->con_in);
    SendMessageA(d->con_in, EM_SETSEL, (WPARAM)len, (LPARAM)len);
}

/* Tab: the engine completes the end of the line; one answer takes its
 * place, several are listed (and the part they share is typed). */
static void complete_console(debugger *d)
{
    char text[256], comps[4096], common[256] = "", line[256];
    char *sp, *cur, *nl;
    int n, first = 1;
    edit_text(d->con_in, text, sizeof text);
    n = cocodebug_completions(d->dbg, text, comps, sizeof comps);
    if (n <= 0) return;
    for (cur = comps; *cur; cur = nl ? nl + 1 : cur + strlen(cur)) {
        int k = 0;
        nl = strchr(cur, '\n');
        snprintf(line, sizeof line, "%.*s", nl ? (int)(nl - cur) : (int)strlen(cur), cur);
        if (!line[0]) continue;
        if (first) {
            snprintf(common, sizeof common, "%s", line);
            first = 0;
        } else {
            while (common[k] && line[k] && common[k] == line[k]) k++;
            common[k] = '\0';
        }
    }
    if (n > 1) console_append(d, comps);
    sp = strrchr(text, ' ');
    if ((int)strlen(common) < (int)strlen(sp ? sp + 1 : text))
        snprintf(common, sizeof common, "%s", sp ? sp + 1 : text);   /* never shorter than typed */
    if (sp) sp[1] = '\0'; else text[0] = '\0';
    {
        char joined[600];
        snprintf(joined, sizeof joined, "%s%s%s", text, common, n == 1 ? " " : "");
        SetWindowTextA(d->con_in, joined);
        SendMessageA(d->con_in, EM_SETSEL, (WPARAM)strlen(joined), (LPARAM)strlen(joined));
    }
}

/* A register box: set while paused, refused with a notice otherwise. */
static void on_reg(debugger *d, int idx)
{
    char buf[32];
    long value;
    edit_text(d->reg[idx], buf, sizeof buf);
    if (!parse_hexish(d, buf, &value))
        toast(d, "Not a number (hex, or an expression)");
    else if (!cocodebug_is_paused(d->dbg))
        toast(d, "Pause the machine first to change a register");
    else if (!cocodebug_regs_set(d->dbg, k_reg_ids[idx], (unsigned)value))
        toast(d, "That register cannot be set");
    else if (idx == 0)
        d->recentre = 1;        /* a new PC: the Disassembly follows */
    SetFocus(d->hwnd);
    resync(d);
}

static void on_check(debugger *d, int idx)
{
    if (d->updating) return;
    if (!cocodebug_is_paused(d->dbg)) {
        toast(d, "Pause the machine first to change a flag");
        refresh_cpu(d);     /* the box goes back to the CPU's */
        return;
    }
    cocodebug_flag_set(d->dbg, k_chk_masks[idx], SendMessageA(d->chk[idx], BM_GETCHECK, 0, 0) == BST_CHECKED);
    resync(d);
}

/* Go to: an address, symbol or range in the space showing; the dump from
 * just above it, the bytes found highlighted and scrolled to. */
static void on_mem_goto(debugger *d)
{
    char buf[64], err[96];
    uint32_t lo, hi;
    edit_text(d->mem_goto, buf, sizeof buf);
    if (!parse_in_space(d, d->mem_sp, buf, &lo, &hi, err, sizeof err)) {
        d->mem_hl = 0;
        refresh_mem(d);
        toast(d, err);
        return;
    }
    if (lo >= d->mem_size) {
        toast(d, "That is past the end of the space");
        return;
    }
    show_in_memory(d, d->mem_sp, lo, hi);
}

/* Find Next / Find Previous: from the highlighted bytes on (or back), else
 * from the top of the space; the machine must be paused. */
static void on_search(debugger *d, int backward)
{
    char text[200];
    uint8_t pat[COCOSEARCH_MAX], mask;
    uint32_t found, from;
    int count = 0, flags = 0, result;
    edit_text(d->srch_entry, text, sizeof text);
    if (!cocosearch_parse((int)SendMessageA(d->srch_kind, CB_GETCURSEL, 0, 0), text, pat, &count, &mask, &flags) ||
        count <= 0) {
        set_label(d->srch_note, "Type hex bytes (3F 00) or some text.");
        return;
    }
    if (!cocodebug_is_paused(d->dbg)) {
        set_label(d->srch_note, "Pause the machine first");
        return;
    }
    from = d->mem_hl && d->mem_hl_space == d->mem_sp ? (backward ? d->mem_hl_lo : d->mem_hl_lo + 1) : 0;
    result = cocodebug_search_next(d->dbg, d->mem_sp, pat, count, mask, flags, from, backward, &found);
    if (result > 0) {
        char note[64];
        snprintf(note, sizeof note, "Found at $%0*X", d->mem_digits, (unsigned)found);
        set_label(d->srch_note, note);
        show_in_memory(d, d->mem_sp, found, found + (uint32_t)count - 1);
    } else {
        set_label(d->srch_note, result < 0 ? "Pause the machine first" : "Not found");
    }
}

/* "address value [value...]": bytes into the space showing from the address on. */
static void on_mem_write(debugger *d)
{
    char buf[256], err[96], *tok, *save = NULL, msg[160];
    uint8_t bytes[64];
    uint32_t lo, hi;
    int count = 0, stored;
    edit_text(d->mem_write, buf, sizeof buf);
    tok = strtok_s(buf, " \t,", &save);
    if (!tok) return;
    if (!parse_in_space(d, d->mem_sp, tok, &lo, &hi, err, sizeof err)) {
        toast(d, err);
        return;
    }
    while ((tok = strtok_s(NULL, " \t,", &save)) != NULL) {
        long value;
        if (!parse_hexish(d, tok, &value) || value < 0 || value > 0xFF) {
            toast(d, "A value is a byte (00-FF)");
            return;
        }
        if (lo + (uint32_t)count >= d->mem_size || count >= (int)sizeof bytes) {
            toast(d, "That is past the end of the space");
            return;
        }
        bytes[count++] = (uint8_t)value;
    }
    if (!count) {
        toast(d, "Type: address value [value...]");
        return;
    }
    if (!cocodebug_is_paused(d->dbg)) {
        toast(d, "Pause the machine first to poke memory");
        return;
    }
    stored = cocodebug_mem_write(d->dbg, d->mem_sp, lo, bytes, count);
    snprintf(msg, sizeof msg, "Wrote %d byte%s at $%0*X in %s%s", stored, stored == 1 ? "" : "s", d->mem_digits,
             (unsigned)lo, k_space_names[d->mem_sp], stored < count ? " (ROM and I/O are not poked)" : "");
    toast(d, msg);
    resync(d);
}

static void on_jump(debugger *d)
{
    char buf[64], err[96];
    int addr;
    edit_text(d->jump, buf, sizeof buf);
    addr = resolve_addr(d, buf, err, sizeof err);
    if (addr < 0) {
        toast(d, err);
        return;
    }
    SendMessageA(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
    d->disasm_top = addr;
    d->selected = addr;
    refresh_disasm(d);
}

/* The selected breakpoint row's index, or -1. */
static int bp_selected(debugger *d)
{
    const int idx = (int)SendMessageA(d->bp_list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    return idx >= 0 && idx < d->nbps ? idx : -1;
}

static void on_bp_remove(debugger *d)
{
    const int idx = bp_selected(d);
    if (idx < 0) {
        set_label(d->bp_msg, "Select a breakpoint to remove");
        return;
    }
    set_label(d->bp_msg, "");
    cocodebug_breakpoint_remove(d->dbg, d->bps[idx].id);
    resync(d);
}

/* Whether the PHYS box is ticked and usable. */
static int bp_wants_phys(debugger *d)
{
    return IsWindowEnabled(d->bp_phys) && SendMessageA(d->bp_phys, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static void on_bp_add(debugger *d)
{
    char first[64], last[64], cond[256], err[160];
    uint32_t start, end, lo, hi, limit;
    int sel, id, phys, space;
    edit_text(d->bp_start, first, sizeof first);
    edit_text(d->bp_end, last, sizeof last);
    edit_text(d->bp_cond, cond, sizeof cond);
    sel = (int)SendMessageA(d->bp_type, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= NBPTYPES) sel = 0;
    phys = bp_wants_phys(d);
    space = phys ? COCODEBUG_SPACE_PHYS : COCODEBUG_SPACE_CPU;
    if (!parse_in_space(d, space, first, &lo, &hi, err, sizeof err)) {
        set_label(d->bp_msg, err);
        return;
    }
    start = lo;
    end = hi;
    if (strip(last)[0]) {
        if (!parse_in_space(d, space, last, &lo, &hi, err, sizeof err)) {
            set_label(d->bp_msg, err);
            return;
        }
        end = lo;
    }
    if (end < start) end = start;
    limit = cocodebug_space_size(d->dbg, space);
    if (end >= limit) {
        set_label(d->bp_msg, "That is past the end of the space");
        return;
    }
    id = cocodebug_breakpoint_add(d->dbg, k_bp_type_ids[sel] | (phys ? COCODEBUG_BP_PHYS : 0), start, end, strip(cond));
    if (id < 0) {
        set_label(d->bp_msg, "The condition does not parse (or the table is full)");
        return;
    }
    set_label(d->bp_msg, "");
    SetWindowTextA(d->bp_start, "");
    SetWindowTextA(d->bp_end, "");
    SetWindowTextA(d->bp_cond, "");
    resync(d);
}

/* The type decides what the boxes take: a physical address only for a data breakpoint, and only on a CoCo 3. */
static void on_bp_type(debugger *d)
{
    const int sel = (int)SendMessageA(d->bp_type, CB_GETCURSEL, 0, 0);
    const int can_phys = sel > 0 && cocodebug_space_size(d->dbg, COCODEBUG_SPACE_PHYS) > 0;
    EnableWindow(d->bp_phys, can_phys);
    if (!can_phys) SendMessageA(d->bp_phys, BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(d->bp_start, EM_SETCUEBANNER, TRUE, (LPARAM)(bp_wants_phys(d) ? L"01F400 (physical)" : L"A1CB / symbol"));
    SendMessageW(d->bp_end, EM_SETCUEBANNER, TRUE, (LPARAM)L"(end)");
}

static int pick_file(debugger *d, int save, const char *title, const char *filter, char *path, DWORD pathsz)
{
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    path[0] = '\0';
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = d->hwnd;
    ofn.lpstrFile = path;
    ofn.nMaxFile = pathsz;
    ofn.lpstrTitle = title;
    ofn.lpstrFilter = filter;
    if (save) {
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        return GetSaveFileNameA(&ofn) ? 1 : 0;
    }
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
}

static void goto_tab(debugger *d, int page);

static void on_symbols(debugger *d)
{
    char path[MAX_PATH], msg[512];
    int count;
    if (!pick_file(d, 0, "Load symbols",
                   "Symbol files (*.sym;*.map;*.txt)\0*.sym;*.map;*.txt\0All files\0*.*\0\0",
                   path, sizeof path))
        return;
    count = cocodebug_symbols_load(d->dbg, path);
    if (count < 0) snprintf(msg, sizeof msg, "Could not open %s", base_name(path));
    else if (count == 0) snprintf(msg, sizeof msg, "No symbols found in %s", base_name(path));
    else snprintf(msg, sizeof msg, "Loaded %d symbol%s from %s", count, count == 1 ? "" : "s", base_name(path));
    d->sym_force = 1;
    say(d, msg);
    resync(d);
}

static void on_clear_symbols(debugger *d)
{
    cocodebug_symbols_clear(d->dbg);
    d->sym_force = 1;
    d->bp_sig_ok = 0;
    say(d, "Symbols cleared (the built-in ones stay)");
    resync(d);
}

/* One or more .lst files, one per module of a multi-file CMOC program:
 * cocodebug_source_load adds to what is loaded (cmoc names the .map after the
 * linked executable, not any one .lst), so loading them all from one dialog is
 * the multi-file case working as intended. */
static void on_load_src(debugger *d)
{
    OPENFILENAMEA ofn;
    static char names[16384];   /* zeroed: the byte past the first NUL tells one file from several */
    char folder[MAX_PATH], msg[200];
    char *name;
    int loaded = 0, failed = 0;

    memset(names, 0, sizeof names);
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = d->hwnd;
    ofn.lpstrTitle = "Load C source";
    ofn.lpstrFilter = "CMOC listings (*.lst)\0*.lst\0All files\0*.*\0\0";
    ofn.lpstrFile = names;
    ofn.nMaxFile = sizeof names;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&ofn)) return;

    name = names + strlen(names) + 1;
    if (*name == '\0') {
        /* exactly one file: names is the whole path */
        if (cocodebug_source_load(d->dbg, names) < 0) failed++;
        else loaded++;
    } else {
        snprintf(folder, sizeof folder, "%.*s", MAX_PATH - 1, names);
        while (*name) {
            char full[sizeof names + MAX_PATH];
            snprintf(full, sizeof full, "%s\\%s", folder, name);
            if (cocodebug_source_load(d->dbg, full) < 0) failed++;
            else loaded++;
            name += strlen(name) + 1;
        }
    }
    if (failed && !loaded)
        snprintf(msg, sizeof msg, "Could not open the listing or its .map (both need `cmoc -i`)");
    else if (failed)
        snprintf(msg, sizeof msg, "Loaded %d listing%s; %d could not be opened", loaded, loaded == 1 ? "" : "s", failed);
    else
        snprintf(msg, sizeof msg, "Loaded %d C source listing%s", loaded, loaded == 1 ? "" : "s");
    say(d, msg);
    d->src_sig[0] = '\0';       /* the file list may have changed */
    if (loaded) goto_tab(d, TAB_CSOURCE);
    resync(d);
}

static void on_clear_src(debugger *d)
{
    cocodebug_source_clear(d->dbg);
    free(d->src_buf);
    free(d->src_text);
    d->src_buf = NULL;
    d->src_text = NULL;
    d->src_lines = 0;
    d->src_path[0] = '\0';
    d->src_sig[0] = '\0';
    d->src_pc_file = d->src_pc_line = -1;
    say(d, "C source cleared");
    resync(d);
}

static void on_clear_breakpoints(debugger *d)
{
    cocodebug_breakpoint_clear_all(d->dbg);
    set_label(d->bp_msg, "");
    resync(d);
}

/* "Break at:" -- a symbol or an address, Enter: an execute breakpoint. */
static void on_break_at(debugger *d)
{
    char text[96], err[96], msg[160];
    int address;
    edit_text(d->break_at, text, sizeof text);
    address = resolve_addr(d, text, err, sizeof err);
    if (address < 0) {
        toast(d, err);
        return;
    }
    if (!cocodebug_breakpoint_is_set(d->dbg, (uint16_t)address))
        cocodebug_breakpoint_toggle(d->dbg, (uint16_t)address);
    snprintf(msg, sizeof msg, "Breakpoint at $%04X", address);
    SetWindowTextA(d->break_at, "");
    toast(d, msg);
    resync(d);
}

/* The address of the selected Symbols row, from the row's own text: the
 * engine's table order is not stable between the listing and the click (a
 * lookup can move entries). */
static int symbol_row_address(debugger *d, unsigned *address)
{
    const int row = (int)SendMessageA(d->sym_list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    char address_text[32] = "";
    LVITEMA item;
    if (row < 0 || row >= d->sym_shown) return 0;
    memset(&item, 0, sizeof item);
    item.iSubItem = 0;
    item.pszText = address_text;
    item.cchTextMax = (int)sizeof address_text;
    SendMessageA(d->sym_list, LVM_GETITEMTEXTA, (WPARAM)row, (LPARAM)&item);
    return sscanf(address_text, "%x", address) == 1;
}

/* A symbol row (double-click, Enter, the button) goes to its address in the Disassembly. */
static void on_symbol_activate(debugger *d)
{
    unsigned address;
    char text[32];
    if (!symbol_row_address(d, &address)) {
        toast(d, "Select a symbol first");
        return;
    }
    snprintf(text, sizeof text, "$%04X", address & 0xFFFF);
    goto_tab(d, TAB_DISASM);
    SetWindowTextA(d->jump, text);
    on_jump(d);
}

/* The symbol row's address as an execute breakpoint. */
static void on_symbol_break(debugger *d)
{
    unsigned address;
    char msg[64];
    if (!symbol_row_address(d, &address)) {
        toast(d, "Select a symbol first");
        return;
    }
    if (!cocodebug_breakpoint_is_set(d->dbg, (uint16_t)address)) cocodebug_breakpoint_toggle(d->dbg, (uint16_t)address);
    snprintf(msg, sizeof msg, "Breakpoint at $%04X", address & 0xFFFF);
    toast(d, msg);
    resync(d);
}

/* ---- watches, trace and history ---------------------------------------------------- */

static int watch_space_chosen(debugger *d)
{
    const int sel = (int)SendMessageA(d->wt_space, CB_GETCURSEL, 0, 0);
    return sel >= 0 && sel < NSPACES ? sel : COCODEBUG_SPACE_CPU;
}

static void on_watch_add(debugger *d)
{
    char text[96];
    edit_text(d->wt_entry, text, sizeof text);
    if (cocowatch_add(strip(text), watch_space_chosen(d)) < 0) {
        set_label(d->wt_msg, cocowatch_count() >= COCOWATCH_MAX ? "The watch list is full"
                                                                  : "Type an address, label or expression");
        return;
    }
    set_label(d->wt_msg, "");
    SetWindowTextA(d->wt_entry, "");
    refresh_watches(d);
}

static int watch_selected(debugger *d)
{
    const int i = (int)SendMessageA(d->wt_list, LB_GETCURSEL, 0, 0);
    return i >= 0 && i < cocowatch_count() ? i : -1;
}

static void on_watch_remove(debugger *d)
{
    const int i = watch_selected(d);
    if (i < 0) {
        set_label(d->wt_msg, "Select a watch to remove");
        return;
    }
    set_label(d->wt_msg, "");
    cocowatch_remove(i);
    refresh_watches(d);
}

static void goto_tab(debugger *d, int page);

/* The selected watch's bytes in the Memory tab, in the space it was made in. */
static void on_watch_memory(debugger *d)
{
    uint32_t lo, hi;
    const int i = watch_selected(d);
    if (i < 0) {
        set_label(d->wt_msg, "Select a watch first");
        return;
    }
    if (!cocowatch_range(d->dbg, i, &lo, &hi)) {
        set_label(d->wt_msg, "That watch does not resolve to an address");
        return;
    }
    set_label(d->wt_msg, "");
    goto_tab(d, TAB_MEMORY);
    show_in_memory(d, cocowatch_space(i), lo, hi);
}

/* A write breakpoint over a range of a space that stops only when the value
 * changes. */
static int break_on_change(debugger *d, int space, uint32_t lo, uint32_t hi, char *err, int errsz)
{
    const int type = COCODEBUG_BP_WRITE | (space == COCODEBUG_SPACE_PHYS ? COCODEBUG_BP_PHYS : 0);
    const int id = cocodebug_breakpoint_add(d->dbg, type, lo, hi, "value != old");
    if (id < 0) snprintf(err, (size_t)errsz, "That breakpoint could not be set (the table may be full)");
    return id;
}

static void on_watch_break(debugger *d)
{
    uint32_t lo, hi;
    char err[160];
    const int i = watch_selected(d);
    if (i < 0) {
        set_label(d->wt_msg, "Select a watch first");
        return;
    }
    if (!cocowatch_range(d->dbg, i, &lo, &hi)) {
        set_label(d->wt_msg, "That watch does not resolve to an address");
        return;
    }
    err[0] = '\0';
    if (break_on_change(d, cocowatch_space(i), lo, hi, err, sizeof err) < 0) {
        set_label(d->wt_msg, err[0] ? err : "That breakpoint could not be set");
        return;
    }
    set_label(d->wt_msg, "Breaking when it changes (see Breakpoints)");
    resync(d);
}

/* Record on either tab toggles the one recorder. */
static void on_record(debugger *d, int idx)
{
    cocodebug_trace_enable(d->dbg, SendMessageA(d->h_rec[idx], BM_GETCHECK, 0, 0) == BST_CHECKED);
    d->h_ok[0] = d->h_ok[1] = 0;
    resync(d);
}

static void on_hist_clear(debugger *d, int idx)
{
    (void)idx;
    cocodebug_trace_clear(d->dbg);
    d->h_ok[0] = d->h_ok[1] = 0;
    if (page_visible(d, TAB_TRACE)) hist_refresh(d, 0, 1);
    if (page_visible(d, TAB_HISTORY)) hist_refresh(d, 1, 1);
}

static void on_hist_filter(debugger *d, int idx)
{
    EnableWindow(d->h_range[idx], SendMessageA(d->h_filter[idx], CB_GETCURSEL, 0, 0) == 2);
    hist_refresh(d, idx, 1);
}

static void on_accept(debugger *d, int id)
{
    if (id >= IDC_REG0 && id <= IDC_REG_LAST) {
        on_reg(d, id - IDC_REG0);
        return;
    }
    switch (id) {
    case IDC_CON_IN: on_console(d); break;
    case IDC_MEM_GOTO: on_mem_goto(d); break;
    case IDC_SRCH_ENTRY: on_search(d, 0); break;
    case IDC_WT_ENTRY: on_watch_add(d); break;
    case IDC_H_RANGE0: hist_refresh(d, 0, 1); break;
    case IDC_H_RANGE1: hist_refresh(d, 1, 1); break;
    case IDC_MEM_WRITE: on_mem_write(d); break;
    case IDC_JUMP: on_jump(d); break;
    case IDC_BREAK_AT: on_break_at(d); break;
    case IDC_BP_START:
    case IDC_BP_END:
    case IDC_BP_COND: on_bp_add(d); break;
    default: break;
    }
}

/* ---- the disassembly -------------------------------------------------------------- */

/* One run of a row's text at a column. */
static void put(debugger *d, HDC dc, int col, int ty, COLORREF fg, const char *text, int len)
{
    SetTextColor(dc, fg);
    TextOutA(dc, S(d, GUTTER_W) + d->mono_w * col, ty, text, len);
}

/* Drawn by hand, one row per line: the breakpoint gutter (a red dot when the
 * breakpoint is enabled, grey when disabled), address, bytes, label,
 * instruction, and the comment (a jump target's label, a port's name); the
 * label and the comment in grey. The selected line takes the system
 * selection colours, the PC's line (applied last, so it wins) is filled with
 * the accent and drawn in white. */
static void paint_disasm(debugger *d, HWND hwnd, HDC dc)
{
    RECT c;
    int i;
    const int paused = cocodebug_is_paused(d->dbg);
    GetClientRect(hwnd, &c);
    FillRect(dc, &c, GetSysColorBrush(COLOR_WINDOW));
    SelectObject(dc, d->mono);
    SetBkMode(dc, TRANSPARENT);
    if (d->line_count == 0) {
        put(d, dc, 0, 0, GetSysColor(COLOR_GRAYTEXT), "(no disassembly)", 16);
        return;
    }
    for (i = 0; i < d->line_count && i * d->line_h < c.bottom; i++) {
        const cocodebug_line *l = &d->lines[i];
        const int y = i * d->line_h, ty = y + (d->line_h - d->mono_h) / 2;
        const int is_pc = l->is_pc && paused, sel = l->address == d->selected;
        COLORREF fg = GetSysColor(COLOR_WINDOWTEXT), dim = GetSysColor(COLOR_GRAYTEXT);
        RECT row;
        char text[240];
        int len;
        SetRect(&row, 0, y, c.right, y + d->line_h);
        if (sel) {
            FillRect(dc, &row, GetSysColorBrush(COLOR_HIGHLIGHT));
            fg = dim = GetSysColor(COLOR_HIGHLIGHTTEXT);
        }
        if (is_pc) {
            FillRect(dc, &row, coco_accent_brush());
            fg = dim = COCO_ACCENT_TEXT;
        }
        if (l->has_breakpoint || l->bp_disabled) {
            /* a red dot for an enabled breakpoint, a grey hollow one for a disabled one */
            const int size = d->line_h - S(d, 6) > 4 ? d->line_h - S(d, 6) : 4;
            HGDIOBJ oldb = SelectObject(dc, l->has_breakpoint ? d->bp_on
                                                              : GetStockObject(NULL_BRUSH));
            HPEN grey_pen = CreatePen(PS_SOLID, 2, is_pc ? RGB(0x5a, 0x58, 0x55) : RGB(0x9a, 0x99, 0x96));
            HGDIOBJ oldp = SelectObject(dc, l->has_breakpoint ? GetStockObject(NULL_PEN) : grey_pen);
            Ellipse(dc, S(d, 4), y + (d->line_h - size) / 2, S(d, 4) + size + 1, y + (d->line_h - size) / 2 + size + 1);
            SelectObject(dc, oldp);
            SelectObject(dc, oldb);
            DeleteObject(grey_pen);
        }
        len = snprintf(text, sizeof text, "%04X", l->address);
        put(d, dc, 0, ty, fg, text, len);
        len = snprintf(text, sizeof text, "%.15s", l->bytes);
        put(d, dc, COL_BYTES, ty, fg, text, len);
        len = snprintf(text, sizeof text, "%.*s", LABEL_W, l->label);
        if (len > 0) put(d, dc, COL_LABEL, ty, dim, text, len);
        len = snprintf(text, sizeof text, "%s", l->disasm);
        if (len > (int)sizeof text - 1) len = (int)sizeof text - 1;
        if (len > 0) put(d, dc, COL_DISASM, ty, fg, text, len);
        if (l->comment[0]) {
            len = snprintf(text, sizeof text, "; %s", l->comment);
            if (len > (int)sizeof text - 1) len = (int)sizeof text - 1;
            put(d, dc, COMMENT_COL, ty, dim, text, len);
        }
    }
}

/* A click selects a line (it works while the machine runs, too); a click on
 * the gutter, or a double-click, toggles its breakpoint. The wheel and the
 * keys browse by instructions (and turn Follow PC off). */
static LRESULT CALLBACK disasm_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;
    if (!d) return DefWindowProcA(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT c;
        HDC mem;
        HBITMAP bmp;
        HGDIOBJ oldbmp, oldfont;
        GetClientRect(hwnd, &c);
        mem = CreateCompatibleDC(dc);
        bmp = CreateCompatibleBitmap(dc, c.right > 0 ? c.right : 1, c.bottom > 0 ? c.bottom : 1);
        oldbmp = SelectObject(mem, bmp);
        oldfont = SelectObject(mem, d->mono);
        paint_disasm(d, hwnd, mem);
        BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldfont);
        SelectObject(mem, oldbmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_PRINTCLIENT:
        paint_disasm(d, hwnd, (HDC)wp);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        /* more or fewer rows fit now */
        if (d->disasm == hwnd && page_visible(d, TAB_DISASM)) refresh_disasm(d);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        const int line = d->line_h > 0 ? GET_Y_LPARAM(lp) / d->line_h : -1;
        const int gutter = GET_X_LPARAM(lp) < S(d, GUTTER_W);
        SetFocus(hwnd);
        if (line < 0 || line >= d->line_count) return 0;
        d->selected = d->lines[line].address;
        if (gutter ? msg == WM_LBUTTONDOWN : msg == WM_LBUTTONDBLCLK)
            toggle_breakpoint(d, d->lines[line].address);
        else
            InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int rows = -delta * 3 / WHEEL_DELTA;
        if (!rows) rows = delta > 0 ? -1 : 1;
        SendMessageA(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
        d->disasm_top = row_address(d, d->disasm_top, rows);
        refresh_disasm(d);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_PRIOR || wp == VK_NEXT || wp == VK_UP || wp == VK_DOWN) {
            const int half = disasm_rows(d) / 2 > 0 ? disasm_rows(d) / 2 : 1;
            const int rows = wp == VK_PRIOR ? -half : wp == VK_NEXT ? half : wp == VK_UP ? -1 : 1;
            SendMessageA(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
            d->disasm_top = row_address(d, d->disasm_top, rows);
            refresh_disasm(d);
            return 0;
        }
        break;
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- edit subclassing ------------------------------------------------------------- */

static WNDPROC g_edit_proc;

/* Enter in a single-line field means "apply this value"; Tab in the console
 * completes and Up/Down recall. None may beep its way through the default
 * handler. */
static LRESULT CALLBACK edit_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    const int id = (int)GetWindowLongPtrW(hwnd, GWLP_ID);
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        PostMessageA(g_dbg->hwnd, WM_DBG_ACCEPT, (WPARAM)id, 0);
        return 0;
    }
    if (msg == WM_CHAR && (wp == '\r' || wp == '\n')) return 0;
    if (id == IDC_CON_IN) {
        if (msg == WM_KEYDOWN && wp == VK_TAB) {
            PostMessageA(g_dbg->hwnd, WM_DBG_TAB, 0, 0);
            return 0;
        }
        if (msg == WM_KEYDOWN && (wp == VK_UP || wp == VK_DOWN)) {
            PostMessageA(g_dbg->hwnd, WM_DBG_HISTORY, wp == VK_DOWN, 0);
            return 0;
        }
        if (msg == WM_CHAR && wp == VK_TAB) return 0;
        if (msg == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    }
    return CallWindowProcW(g_edit_proc, hwnd, msg, wp, lp);
}

/* ---- construction ----------------------------------------------------------------- */

/* The standard controls are made as Unicode windows, so the engine's UTF-8
 * (the arrows and dashes of its text) shows as it is. */
static HWND child_ex(debugger *d, DWORD ex, const char *cls, const char *text, DWORD style, int id, HFONT font)
{
    wchar_t wcls[64], wtext[512];
    HWND h;
    wide(cls, wcls, 64);
    wide(text, wtext, 512);
    h = CreateWindowExW(ex, wcls, wtext, WS_CHILD | (d->new_parent ? WS_VISIBLE : 0) | style, 0, 0, 10, 10,
                        d->new_parent ? d->new_parent : d->hwnd,
                        (HMENU)(INT_PTR)id,
                        (HINSTANCE)GetWindowLongPtrA(d->hwnd, GWLP_HINSTANCE), NULL);
    SendMessageA(h, WM_SETFONT, (WPARAM)font, TRUE);
    return h;
}

static HWND child(debugger *d, const char *cls, const char *text, DWORD style, int id, HFONT font)
{
    return child_ex(d, 0, cls, text, style, id, font);
}

/* A read-only monospace text view. */
static HWND view(debugger *d, int id, DWORD extra)
{
    HWND h = child(d, "EDIT", "", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | extra,
                   id, d->mono);
    SendMessageA(h, EM_SETLIMITTEXT, 0, 0);
    if (d->nviews < (int)(sizeof d->views / sizeof d->views[0])) d->views[d->nviews++] = h;
    return h;
}

static HWND field(debugger *d, int id, const wchar_t *cue)
{
    HWND h = child(d, "EDIT", "", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, id, d->mono);
    g_edit_proc = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)edit_subclass);
    if (cue) SendMessageW(h, EM_SETCUEBANNER, TRUE, (LPARAM)cue);
    return h;
}

static HWND label(debugger *d, const char *text, HFONT font, DWORD style)
{
    return child(d, "STATIC", text, style | SS_NOPREFIX, IDC_LABEL_FIRST + d->nlabels++, font);
}

/* A caption or hint, drawn grey. */
static HWND dim_label(debugger *d, const char *text, DWORD style)
{
    HWND h = label(d, text, d->ui, style);
    if (d->ndim < (int)(sizeof d->dim / sizeof d->dim[0])) d->dim[d->ndim++] = h;
    return h;
}

static HWND button(debugger *d, const char *text, int id)
{
    return child(d, "BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP, id, d->ui);
}

static HWND checkbox(debugger *d, const char *text, int id)
{
    return child(d, "BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, id, d->ui);
}

static void size_columns(debugger *d)
{
    static const int widths[3] = { 60, 700, 80 };
    int i;
    for (i = 0; i < 3; i++)
        SendMessageA(d->bp_list, LVM_SETCOLUMNWIDTH, (WPARAM)i, S(d, widths[i]));
}

/* Starts a tab: its container, which the controls made next are children of. */
static void begin_page(debugger *d, int page)
{
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrA(d->hwnd, GWLP_HINSTANCE);
    d->pg[page] = CreateWindowExA(0, PAGE_CLASS, "", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 10, 10,
                                  d->hwnd, NULL, inst, NULL);
    SetWindowLongPtrA(d->pg[page], GWLP_USERDATA, (LONG_PTR)d->hwnd);
    d->new_parent = d->pg[page];
}

/* A tab's label for the tab control ("&" would underline the next letter). */
static void tab_text(int page, char *text, size_t n)
{
    const char *s;
    size_t o = 0;
    for (s = k_tabs[page].label; *s && o < n - 2; s++) {
        if (*s == '&') text[o++] = '&';
        text[o++] = *s;
    }
    text[o] = '\0';
}

static void build_controls(debugger *d)
{
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrA(d->hwnd, GWLP_HINSTANCE);
    TCITEMA item;
    int idx;

    d->tabs = child(d, "SysTabControl32", "", WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP, IDC_TABS, d->ui);
    memset(&item, 0, sizeof item);
    item.mask = TCIF_TEXT;
    for (idx = 0; idx < TAB_COUNT; idx++) {
        char text[32];
        tab_text(idx, text, sizeof text);
        item.pszText = text;
        SendMessageA(d->tabs, TCM_INSERTITEMA, (WPARAM)idx, (LPARAM)&item);
        d->tab_page[idx] = idx;
    }
    d->ntabs = TAB_COUNT;

    /* The toolbar. Run/Pause is owner-drawn so it can wear the accent colour
     * while the machine is paused. */
    d->run_btn = child(d, "BUTTON", "Pause (F5)", BS_OWNERDRAW | WS_TABSTOP, IDC_RUN, d->ui);
    d->step_btn = button(d, "Step (F7)", IDC_STEP);
    d->over_btn = button(d, "Step Over (F8)", IDC_OVER);
    d->out_btn = button(d, "Step Out (Shift+F8)", IDC_OUT);
    d->step_line_btn = button(d, "Step Line (F6)", IDC_STEP_LINE);
    d->over_line_btn = button(d, "Step Over Line (Shift+F6)", IDC_OVER_LINE);
    d->run_to_btn = button(d, "Run to Cursor", IDC_RUN_TO);
    d->clear_bps_btn = button(d, "Clear Breakpoints", IDC_CLEAR_BPS);
    d->break_at_label = dim_label(d, "Break at:", SS_RIGHT);
    d->break_at = field(d, IDC_BREAK_AT, L"symbol or hex");
    d->sym_btn = button(d, "Load Symbols...", IDC_SYMBOLS);
    d->clear_sym_btn = button(d, "Clear Symbols", IDC_CLEAR_SYMBOLS);
    d->src_btn = button(d, "Load C Source...", IDC_LOAD_SRC);
    d->clear_src_btn = button(d, "Clear C Source", IDC_CLEAR_SRC);
    d->popout_btn = button(d, "Pop Out", IDC_POPOUT);
    d->help_btn = button(d, "Help", IDC_HELP_BTN);
    d->status = child(d, "STATIC", "", SS_RIGHT | SS_ENDELLIPSIS | SS_NOPREFIX, IDC_STATUS, d->ui);
    if (d->ndim < (int)(sizeof d->dim / sizeof d->dim[0])) d->dim[d->ndim++] = d->status;
    {
        HWND bar[17] = { d->run_btn, d->step_btn, d->over_btn, d->out_btn, d->step_line_btn, d->over_line_btn,
                         d->run_to_btn, d->clear_bps_btn, d->break_at_label, d->break_at, d->sym_btn,
                         d->clear_sym_btn, d->src_btn, d->clear_src_btn, d->popout_btn, d->status, d->help_btn };
        for (idx = 0; idx < 17; idx++) ShowWindow(bar[idx], SW_SHOW);
    }

    /* The registers strip, under the tabs (children of the window itself); the 6309's start hidden */
    for (idx = 0; idx < NREGS; idx++) {
        d->reg_label[idx] = dim_label(d, k_reg_names[idx], SS_RIGHT);
        d->reg[idx] = field(d, IDC_REG0 + idx, NULL);
        SendMessageA(d->reg[idx], EM_SETLIMITTEXT, 12, 0);
        if (idx < NREGS_6809) {
            ShowWindow(d->reg_label[idx], SW_SHOW);
            ShowWindow(d->reg[idx], SW_SHOW);
        }
    }
    for (idx = 0; idx < NCHECKS; idx++) {
        d->chk[idx] = checkbox(d, k_chk_names[idx], IDC_CHK0 + idx);
        ShowWindow(d->chk[idx], SW_SHOW);
    }
    d->info = label(d, "", d->ui, SS_LEFT | SS_ENDELLIPSIS);
    ShowWindow(d->info, SW_SHOW);

    /* Console */
    begin_page(d, TAB_CONSOLE);
    d->con_out = view(d, IDC_CON_OUT, 0);
    SetWindowTextA(d->con_out, "CoCo debugger console. Type 'help' for every command; Tab completes, Up/Down "
                               "recall. Numbers are decimal; $ (or 0x) for hex.\r\n");
    d->con_prompt = child(d, "STATIC", ">", SS_LEFT, IDC_CON_PROMPT, d->mono);
    d->con_in = field(d, IDC_CON_IN, L"help, step, over, break $A1CB, bpw $0400, mem $0400, disasm, regs, video, ...");

    /* Disassembly. Follow PC is a toggle button: the accent while it is on. */
    begin_page(d, TAB_DISASM);
    d->follow_pc = child(d, "BUTTON", "Follow PC", BS_AUTOCHECKBOX | BS_PUSHLIKE | WS_TABSTOP, IDC_FOLLOW_PC,
                         d->ui);
    SendMessageA(d->follow_pc, BM_SETCHECK, BST_CHECKED, 0);
    d->jump_label = dim_label(d, "Jump to", SS_LEFT);
    d->jump = field(d, IDC_JUMP, L"A1CB or a symbol");
    d->disasm_hint = dim_label(d, "Click a line to select it (Run to Cursor); click the gutter, double-click or "
                                  "press F9 for a breakpoint; scroll to browse",
                               SS_LEFT | SS_ENDELLIPSIS);
    d->disasm = CreateWindowExA(WS_EX_CLIENTEDGE, DISASM_CLASS, "", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10,
                                d->pg[TAB_DISASM], (HMENU)(INT_PTR)IDC_DISASM, inst, NULL);

    /* C Source: the file picker, the listing, the locals */
    begin_page(d, TAB_CSOURCE);
    d->src_files = child(d, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_SRC_FILES, d->ui);
    EnableWindow(d->src_files, FALSE);
    d->src_note = dim_label(d, "", SS_LEFT | SS_ENDELLIPSIS);
    d->src_view = CreateWindowExA(WS_EX_CLIENTEDGE, LINES_CLASS, "", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP, 0, 0,
                                  10, 10, d->pg[TAB_CSOURCE], (HMENU)(INT_PTR)IDC_SRC_VIEW, inst, NULL);
    SendMessageA(d->src_view, WM_SETFONT, (WPARAM)d->mono, TRUE);
    d->src_locals_label = label(d, "Locals and parameters", d->ui_bold, SS_LEFT);
    d->src_locals = view(d, IDC_SRC_LOCALS, WS_HSCROLL);
    d->src_file = -1;
    d->src_pc_file = d->src_pc_line = -1;

    /* Memory: the space selector, the dump, Go to, Poke, and the search bar */
    begin_page(d, TAB_MEMORY);
    d->mem_title = label(d, k_space_titles[0], d->ui_bold, SS_LEFT);
    d->mem_space = child(d, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_MEM_SPACE, d->ui);
    for (idx = 0; idx < NSPACES; idx++) SendMessageA(d->mem_space, CB_ADDSTRING, 0, (LPARAM)k_space_names[idx]);
    SendMessageA(d->mem_space, CB_SETCURSEL, 0, 0);
    d->mem_view = CreateWindowExA(WS_EX_CLIENTEDGE, MEMVIEW_CLASS, "", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP, 0, 0,
                                  10, 10, d->pg[TAB_MEMORY], (HMENU)(INT_PTR)IDC_MEM_VIEW, inst, NULL);
    d->goto_label = dim_label(d, "Go to", SS_LEFT);
    d->mem_goto = field(d, IDC_MEM_GOTO, L"0400 / symbol / lo-hi / expression");
    d->write_label = dim_label(d, "Poke", SS_LEFT);
    d->mem_write = field(d, IDC_MEM_WRITE, L"0400 41 42 (address, hex bytes)");
    d->srch_kind = child(d, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_SRCH_KIND, d->ui);
    SendMessageA(d->srch_kind, CB_ADDSTRING, 0, (LPARAM)"Hex");
    SendMessageA(d->srch_kind, CB_ADDSTRING, 0, (LPARAM)"Text");
    SendMessageA(d->srch_kind, CB_SETCURSEL, 0, 0);
    d->srch_entry = field(d, IDC_SRCH_ENTRY, L"3F 00 (hex bytes) or text");
    d->srch_next = button(d, "Find Next", IDC_SRCH_NEXT);
    d->srch_prev = button(d, "Find Previous", IDC_SRCH_PREV);
    d->srch_note = dim_label(d, "", SS_LEFT | SS_ENDELLIPSIS);

    /* Breakpoints */
    begin_page(d, TAB_BREAKS);
    d->bp_stop = label(d, "Running.", d->ui_bold, SS_LEFT | SS_ENDELLIPSIS);
    d->bp_hint = dim_label(d, "Click a disassembly line's gutter to set an execute breakpoint; the box enables it.",
                           SS_LEFT | SS_ENDELLIPSIS);
    d->bp_remove = button(d, "Remove", IDC_BP_REMOVE);
    d->bp_clear = button(d, "Clear All", IDC_BP_CLEAR);
    d->bp_clear_hits = button(d, "Clear Hits", IDC_BP_CLEAR_HITS);
    d->bp_list = child_ex(d, WS_EX_CLIENTEDGE, "SysListView32", "",
                          WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL | LVS_NOSORTHEADER,
                          IDC_BP_LIST, d->mono);
    SendMessageA(d->bp_list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
    {
        static const char *const cols[3] = { "#", "Breakpoint", "Hits" };
        LVCOLUMNA col;
        memset(&col, 0, sizeof col);
        col.mask = LVCF_TEXT;
        for (idx = 0; idx < 3; idx++) {
            col.pszText = (char *)cols[idx];
            SendMessageA(d->bp_list, LVM_INSERTCOLUMNA, (WPARAM)idx, (LPARAM)&col);
        }
        size_columns(d);
    }
    d->bp_empty = dim_label(d, "No breakpoints. Click the gutter of a disassembly line, or add one below.", SS_CENTER);
    d->bp_type = child(d, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_BP_TYPE, d->ui);
    for (idx = 0; idx < NBPTYPES; idx++) SendMessageA(d->bp_type, CB_ADDSTRING, 0, (LPARAM)k_bp_types[idx]);
    SendMessageA(d->bp_type, CB_SETCURSEL, 0, 0);
    d->bp_phys = checkbox(d, "PHYS", IDC_BP_PHYS);
    d->bp_start = field(d, IDC_BP_START, NULL);
    d->bp_end = field(d, IDC_BP_END, NULL);
    d->bp_cond = field(d, IDC_BP_COND, L"condition, e.g. value != old  (value, old, addr, a, x, pc, ...)");
    d->bp_add = button(d, "Add", IDC_BP_ADD);
    d->bp_msg = label(d, "", d->ui, SS_LEFT | SS_ENDELLIPSIS);
    on_bp_type(d);

    /* Watches */
    begin_page(d, TAB_WATCHES);
    d->wt_label = dim_label(d, "Watch", SS_LEFT);
    d->wt_space = child(d, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_WT_SPACE, d->ui);
    for (idx = 0; idx < NSPACES; idx++) SendMessageA(d->wt_space, CB_ADDSTRING, 0, (LPARAM)k_space_names[idx]);
    SendMessageA(d->wt_space, CB_SETCURSEL, 0, 0);
    d->wt_entry = field(d, IDC_WT_ENTRY, L"an address, symbol or expression, or lo-hi for a range");
    d->wt_add = button(d, "Add", IDC_WT_ADD);
    d->wt_remove = button(d, "Remove", IDC_WT_REMOVE);
    d->wt_mem = button(d, "Memory", IDC_WT_MEM);
    d->wt_brk = button(d, "Break on change", IDC_WT_BRK);
    d->wt_list = child_ex(d, WS_EX_CLIENTEDGE, "LISTBOX", "",
                          WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, IDC_WT_LIST, d->mono);
    d->wt_msg = label(d, "", d->ui, SS_LEFT | SS_ENDELLIPSIS);

    /* Cart & I/O: the cartridge and the memory map left; the PIAs, SAM, GIME, joysticks and Becker right */
    begin_page(d, TAB_CART);
    d->cart_text = view(d, IDC_CART_TEXT, WS_HSCROLL);
    d->cart_io_label = dim_label(d, "I/O: the PIAs, the SAM, the GIME, the joysticks and the Becker link, as read at the stop",
                                 SS_LEFT | SS_ENDELLIPSIS);
    d->cart_io = view(d, IDC_CART_IO, WS_HSCROLL);

    /* Trace (0) and History (1) */
    for (idx = 0; idx < 2; idx++) {
        begin_page(d, idx ? TAB_HISTORY : TAB_TRACE);
        d->h_rec[idx] = child(d, "BUTTON", "Record", BS_AUTOCHECKBOX | BS_PUSHLIKE | WS_TABSTOP, IDC_H_REC0 + idx, d->ui);
        d->h_clear[idx] = button(d, "Clear", IDC_H_CLEAR0 + idx);
        d->h_filter[idx] = child(d, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_H_FILTER0 + idx, d->ui);
        SendMessageA(d->h_filter[idx], CB_ADDSTRING, 0, (LPARAM)"All");
        SendMessageA(d->h_filter[idx], CB_ADDSTRING, 0, (LPARAM)"Branches");
        SendMessageA(d->h_filter[idx], CB_ADDSTRING, 0, (LPARAM)"Range");
        SendMessageA(d->h_filter[idx], CB_SETCURSEL, 0, 0);
        d->h_range[idx] = field(d, IDC_H_RANGE0 + idx, L"A000-BFFF or a symbol");
        EnableWindow(d->h_range[idx], FALSE);
        d->h_note[idx] = dim_label(d, "recording off", SS_LEFT | SS_ENDELLIPSIS);
        d->h_text[idx] = view(d, IDC_H_TEXT0 + idx, WS_HSCROLL);
    }

    /* Video: one child page with its own controls and controller, made on first use (ensure_video) */
    begin_page(d, TAB_VIDEO);

    /* Sound: the PIA1 DAC and single-bit port */
    begin_page(d, TAB_SOUND);
    d->sound_text = view(d, IDC_SOUND_TEXT, WS_HSCROLL);
    d->sound_level = child(d, PROGRESS_CLASSA, "", PBS_SMOOTH, IDC_SOUND_LEVEL, d->ui);
    SendMessageA(d->sound_level, PBM_SETRANGE32, 0, 63);
    d->sound_log_label = dim_label(d, "Writes to the DAC ($FF20) and the single-bit port ($FF22), newest last",
                                   SS_LEFT);
    d->sound_log = view(d, IDC_SOUND_LOG, WS_HSCROLL);

    /* Symbols: a filter over the table, a row opens the Disassembly there */
    begin_page(d, TAB_SYMBOLS);
    d->sym_filter_label = dim_label(d, "Filter", SS_LEFT);
    d->sym_filter = field(d, IDC_SYM_FILTER, L"part of a name");
    d->sym_count = dim_label(d, "", SS_LEFT | SS_ENDELLIPSIS);
    d->sym_goto = button(d, "Disassemble", IDC_SYM_GOTO);
    d->sym_break = button(d, "Break at", IDC_SYM_BREAK);
    d->sym_load = button(d, "Load...", IDC_SYM_LOAD);
    d->sym_clear = button(d, "Clear", IDC_SYM_CLEAR);
    d->sym_list = child_ex(d, WS_EX_CLIENTEDGE, "SysListView32", "",
                           WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL, IDC_SYM_LIST, d->mono);
    SendMessageA(d->sym_list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT);
    {
        static const char *const symbol_columns[2] = { "Address", "Name" };
        LVCOLUMNA symbol_column;
        memset(&symbol_column, 0, sizeof symbol_column);
        symbol_column.mask = LVCF_TEXT | LVCF_WIDTH;
        for (idx = 0; idx < 2; idx++) {
            symbol_column.pszText = (char *)symbol_columns[idx];
            symbol_column.cx = S(d, idx == 1 ? 360 : 80);
            SendMessageA(d->sym_list, LVM_INSERTCOLUMNA, (WPARAM)idx, (LPARAM)&symbol_column);
        }
    }
    d->sym_shown = -1;

    /* FujiNet: the note and the log are UTF-8 (see wide_view_text) */
    begin_page(d, TAB_FUJINET);
    d->fuji_title = label(d, "DriveWire packets", d->ui_bold, SS_LEFT);
    d->fuji_note = dim_label(d, "", SS_LEFT | SS_ENDELLIPSIS);
    d->fuji_hide = checkbox(d, "Hide READY probes", IDC_FUJI_HIDE);
    SendMessageA(d->fuji_hide, BM_SETCHECK, BST_CHECKED, 0);
    d->fuji_clear = button(d, "Clear", IDC_FUJI_CLEAR);
    d->fuji_text = view(d, IDC_FUJI_TEXT, WS_HSCROLL);
    d->fuji_state_title = label(d, "Becker port", d->ui_bold, SS_LEFT);
    d->fuji_state = view(d, IDC_FUJI_STATE, WS_HSCROLL);
    d->new_parent = NULL;
}

/* Help's contexts (data/help/help.md), one per tab */
static const char *const k_tab_contexts[TAB_COUNT] = {
    "debugger/console", "debugger/disassembly", "debugger/csource", "debugger/memory", "debugger/breakpoints",
    "debugger/watches", "debugger/cart", "debugger/trace", "debugger/history", "debugger/video", "debugger/sound",
    "debugger/symbols", "debugger/fujinet",
};

/* The tab control's index for a tab, or -1 when it is in a window of its own. */
static int tab_index(debugger *d, int page)
{
    int i;
    for (i = 0; i < d->ntabs; i++)
        if (d->tab_page[i] == page) return i;
    return -1;
}

static void show_page(debugger *d, int page)
{
    int p;
    d->page = page;
    /* Help, F1 and Ctrl+Shift+H open the tab's section */
    if (page >= 0 && page < TAB_COUNT) coco_help_set_context(d->hwnd, k_tab_contexts[page], 1);
    for (p = 0; p < TAB_COUNT; p++)   /* a tab's controls follow its container */
        ShowWindow(d->pg[p], d->popped[p] || p == page ? SW_SHOW : SW_HIDE);
    show_bp_list(d);
    if (page == TAB_VIDEO) {
        ensure_video(d);
        if (d->video) dbg_video_refresh(d->video, 1);
    }
    sound_watch_sync(d);
}

static void place(HWND h, int x, int y, int w, int hgt)
{
    MoveWindow(h, x, y, w > 1 ? w : 1, hgt > 1 ? hgt : 1, TRUE);
}

/* One tab's controls inside its container, which is pw x ph. */
static void layout_page(debugger *d, int page, int pw, int ph)
{
    const int bh = S(d, 26), eh = S(d, 24), lh = S(d, 18), gap = S(d, 6);
    const int px = 0, py = 0;
    int x, y, w, i;

    switch (page) {
    case TAB_CONSOLE:
        /* the transcript, then the prompt and its line */
        w = d->mono_w + S(d, 6);
        place(d->con_out, px, py, pw, ph - eh - S(d, 8));
        place(d->con_prompt, px, py + ph - eh + (eh - d->mono_h) / 2, w, d->mono_h);
        place(d->con_in, px + w + S(d, 2), py + ph - eh, pw - w - S(d, 2), eh);
        break;

    case TAB_DISASM:
        /* Follow PC and Jump to over the lines */
        w = btn_w(d, "Follow PC");
        place(d->follow_pc, px, py - S(d, 1), w, bh);
        x = px + w + S(d, 12);
        w = text_w(d, d->ui, "Jump to") + S(d, 6);
        place(d->jump_label, x, py + (eh - lh) / 2, w, lh);
        x += w;
        place(d->jump, x, py, S(d, 150), eh);
        x += S(d, 162);
        place(d->disasm_hint, x, py + (eh - lh) / 2, px + pw - x, lh);
        place(d->disasm, px, py + eh + S(d, 6), pw, ph - eh - S(d, 6));
        break;

    case TAB_CSOURCE: {
        /* the file picker and a note over the listing; the locals right */
        const int lw = pw * 62 / 100, rx = px + lw + S(d, 12), rw = pw - lw - S(d, 12);
        place(d->src_files, px, py, S(d, 240), S(d, 240));
        place(d->src_note, px + S(d, 252), py + (eh - lh) / 2, pw - S(d, 252), lh);
        place(d->src_view, px, py + eh + S(d, 6), lw, ph - eh - S(d, 6));
        place(d->src_locals_label, rx, py + eh + S(d, 6), rw, lh);
        place(d->src_locals, rx, py + eh + S(d, 10) + lh, rw, ph - eh - S(d, 10) - lh);
        break;
    }

    case TAB_MEMORY: {
        /* the space and its title, the dump, then Go to / Poke, then the search bar */
        const int row2 = py + ph - eh, row1 = row2 - eh - S(d, 8);
        const int top = py + eh + S(d, 4);
        place(d->mem_space, px, py, S(d, 100), S(d, 200));
        place(d->mem_title, px + S(d, 112), py + (eh - lh) / 2, pw - S(d, 112), lh);
        place(d->mem_view, px, top, pw, row1 - S(d, 6) - top);
        y = row1 + (eh - lh) / 2;
        x = px;
        w = text_w(d, d->ui, "Go to") + S(d, 6);
        place(d->goto_label, x, y, w, lh);
        x += w;
        place(d->mem_goto, x, row1, S(d, 190), eh);
        x += S(d, 196);
        x += S(d, 24);
        w = text_w(d, d->ui, "Poke") + S(d, 6);
        place(d->write_label, x, y, w, lh);
        x += w;
        place(d->mem_write, x, row1, S(d, 260), eh);

        x = px;
        place(d->srch_kind, x, row2, S(d, 70), S(d, 120));
        x += S(d, 76);
        place(d->srch_entry, x, row2, S(d, 250), eh);
        x += S(d, 256);
        w = btn_w(d, "Find Next");
        place(d->srch_next, x, row2 - S(d, 1), w, bh);
        x += w + S(d, 4);
        w = btn_w(d, "Find Previous");
        place(d->srch_prev, x, row2 - S(d, 1), w, bh);
        x += w + S(d, 12);
        place(d->srch_note, x, row2 + (eh - lh) / 2, px + pw - x, lh);
        break;
    }

    case TAB_BREAKS: {
        /* "Paused: ..." over the hint and the buttons; the list; the add row
         * and a message under it */
        const int msg_y = py + ph - lh, add_y = msg_y - S(d, 8) - eh;
        const int row_y = py + lh + S(d, 4), list_y = row_y + bh + S(d, 6);
        int bw = btn_w(d, "Clear All"), hw = btn_w(d, "Clear Hits"), rbw = btn_w(d, "Remove");
        place(d->bp_stop, px, py, pw, lh);
        place(d->bp_clear, px + pw - bw, row_y, bw, bh);
        place(d->bp_clear_hits, px + pw - bw - gap - hw, row_y, hw, bh);
        place(d->bp_remove, px + pw - bw - hw - 2 * gap - rbw, row_y, rbw, bh);
        place(d->bp_hint, px, row_y + (bh - lh) / 2, pw - bw - hw - rbw - 4 * gap, lh);
        place(d->bp_list, px, list_y, pw, add_y - gap - list_y);
        {
            RECT lc;
            int cw;
            GetClientRect(d->bp_list, &lc);
            cw = lc.right - S(d, 60) - S(d, 80) - GetSystemMetrics(SM_CXVSCROLL);
            SendMessageA(d->bp_list, LVM_SETCOLUMNWIDTH, 1, cw > S(d, 200) ? cw : S(d, 200));
        }
        place(d->bp_empty, px, list_y + S(d, 12), pw, lh);
        x = px;
        place(d->bp_type, x, add_y, S(d, 100), S(d, 240));
        x += S(d, 106);
        w = btn_w(d, "PHYS") - S(d, 14);
        place(d->bp_phys, x, add_y + (eh - S(d, 22)) / 2, w, S(d, 22));
        x += w + S(d, 6);
        place(d->bp_start, x, add_y, S(d, 140), eh);
        x += S(d, 146);
        place(d->bp_end, x, add_y, S(d, 110), eh);
        x += S(d, 116);
        bw = btn_w(d, "Add");
        place(d->bp_cond, x, add_y, px + pw - x - bw - gap, eh);
        place(d->bp_add, px + pw - bw, add_y - S(d, 1), bw, bh);
        place(d->bp_msg, px, msg_y, pw, lh);
        break;
    }

    case TAB_WATCHES: {
        const int msg_y = py + ph - lh, row_y = py + eh + S(d, 8);
        int bw = btn_w(d, "Add");
        w = text_w(d, d->ui, "Watch") + S(d, 6);
        place(d->wt_label, px, py + (eh - lh) / 2, w, lh);
        place(d->wt_space, px + w, py, S(d, 100), S(d, 200));
        place(d->wt_entry, px + w + S(d, 108), py, pw - w - S(d, 108) - bw - gap, eh);
        place(d->wt_add, px + pw - bw, py - S(d, 1), bw, bh);
        x = px;
        bw = btn_w(d, "Remove");
        place(d->wt_remove, x, row_y, bw, bh);
        x += bw + gap;
        bw = btn_w(d, "Memory");
        place(d->wt_mem, x, row_y, bw, bh);
        x += bw + gap;
        bw = btn_w(d, "Break on change");
        place(d->wt_brk, x, row_y, bw, bh);
        place(d->wt_list, px, row_y + bh + S(d, 6), pw, msg_y - gap - (row_y + bh + S(d, 6)));
        place(d->wt_msg, px, msg_y, pw, lh);
        break;
    }

    case TAB_CART: {
        /* the cartridge and the memory map left; the I/O state right */
        const int lw = pw * 44 / 100, rx = px + lw + S(d, 12), rw = pw - lw - S(d, 12);
        place(d->cart_text, px, py, lw, ph);
        place(d->cart_io_label, rx, py, rw, lh);
        place(d->cart_io, rx, py + lh + S(d, 4), rw, ph - lh - S(d, 4));
        break;
    }

    case TAB_TRACE:
    case TAB_HISTORY: {
        const int k = page == TAB_HISTORY;
        x = px;
        w = btn_w(d, "Record");
        place(d->h_rec[k], x, py - S(d, 1), w, bh);
        x += w + gap;
        w = btn_w(d, "Clear");
        place(d->h_clear[k], x, py - S(d, 1), w, bh);
        x += w + S(d, 16);
        place(d->h_filter[k], x, py, S(d, 100), S(d, 120));
        x += S(d, 106);
        place(d->h_range[k], x, py, S(d, 190), eh);
        x += S(d, 202);
        place(d->h_note[k], x, py + (eh - lh) / 2, px + pw - x, lh);
        place(d->h_text[k], px, py + bh + S(d, 6), pw, ph - bh - S(d, 6));
        break;
    }

    case TAB_VIDEO:
        if (d->video) place(d->video, px, py, pw, ph);
        break;

    case TAB_SOUND: {
        const int text_h = d->mono_h * 8 + S(d, 12);
        place(d->sound_text, px, py, pw, text_h);
        place(d->sound_level, px, py + text_h + S(d, 8), pw, S(d, 20));
        place(d->sound_log_label, px, py + text_h + S(d, 36), pw, lh);
        place(d->sound_log, px, py + text_h + S(d, 40) + lh, pw, ph - text_h - S(d, 40) - lh);
        break;
    }

    case TAB_SYMBOLS: {
        const char *const names[4] = { "Disassemble", "Break at", "Load...", "Clear" };
        HWND const buttons[4] = { d->sym_goto, d->sym_break, d->sym_load, d->sym_clear };
        int right = px + pw;
        w = text_w(d, d->ui, "Filter") + S(d, 6);
        place(d->sym_filter_label, px, py + (eh - lh) / 2, w, lh);
        place(d->sym_filter, px + w, py, S(d, 260), eh);
        for (i = 3; i >= 0; i--) {
            const int bw = btn_w(d, names[i]);
            right -= bw;
            place(buttons[i], right, py - S(d, 1), bw, bh);
            right -= gap;
        }
        place(d->sym_count, px + w + S(d, 272), py + (eh - lh) / 2, right - (px + w + S(d, 272)), lh);
        place(d->sym_list, px, py + eh + S(d, 6), pw, ph - eh - S(d, 6));
        break;
    }

    case TAB_FUJINET: {
        /* the packets left (a note, the READY filter and Clear over them), the
         * Becker port's state right */
        const int rw = pw / 3 > S(d, 380) ? S(d, 380) : pw / 3 > S(d, 240) ? pw / 3 : S(d, 240);
        const int lw = pw - rw - S(d, 12) > S(d, 120) ? pw - rw - S(d, 12) : S(d, 120);
        const int cw = btn_w(d, "Clear"), hw = text_w(d, d->ui, "Hide READY probes") + S(d, 28);
        place(d->fuji_title, px, py, lw, lh);
        place(d->fuji_clear, px + lw - cw, py + lh + S(d, 4), cw, bh);
        place(d->fuji_hide, px + lw - cw - gap - hw, py + lh + S(d, 4) + (bh - S(d, 22)) / 2, hw, S(d, 22));
        place(d->fuji_note, px, py + lh + S(d, 4) + (bh - lh) / 2, lw - cw - hw - 2 * gap, lh);
        place(d->fuji_text, px, py + lh + bh + S(d, 10), lw, ph - lh - bh - S(d, 10));
        place(d->fuji_state_title, px + lw + S(d, 12), py, rw, lh);
        place(d->fuji_state, px + lw + S(d, 12), py + lh + S(d, 4), rw, ph - lh - S(d, 4));
        break;
    }
    default: break;
    }
    (void)i;
}

/* The registers strip: PC A B D DP X Y U S CC (and the 6309's E F W V MD) in
 * one line or several when narrow, then the flags and the PC's symbol.
 * Returns its height. */
static int reg_count(const debugger *d)
{
    return d->is6309 ? NREGS : NREGS_6809;
}

static int strip_height(debugger *d, int width, int *rows)
{
    const int eh = S(d, 24), gap = S(d, 6);
    const int lw = text_w(d, d->ui, "MD") + S(d, 6), rw = d->mono_w * 5 + S(d, 10);
    const int cell = lw + S(d, 4) + rw + S(d, 8);
    int per_row = (width + S(d, 8)) / cell;
    if (per_row < 1) per_row = 1;
    *rows = (reg_count(d) + per_row - 1) / per_row;
    return *rows * (eh + gap) + S(d, 22) + gap;
}

static void layout_strip(debugger *d, int x0, int y0, int width)
{
    const int eh = S(d, 24), gap = S(d, 6), lh = S(d, 18);
    const int lw = text_w(d, d->ui, "MD") + S(d, 6), rw = d->mono_w * 5 + S(d, 10);
    const int cell = lw + S(d, 4) + rw + S(d, 8);
    int rows, per_row, idx, x, y;
    strip_height(d, width, &rows);
    /* the same number of registers on every row */
    per_row = (reg_count(d) + rows - 1) / rows;
    for (idx = 0; idx < reg_count(d); idx++) {
        const int row = idx / per_row, col = idx % per_row;
        const int ry = y0 + row * (eh + gap);
        place(d->reg_label[idx], x0 + col * cell, ry + (eh - lh) / 2, lw, lh);
        place(d->reg[idx], x0 + col * cell + lw + S(d, 4), ry, rw, eh);
    }
    d->strip_rows = rows;
    y = y0 + rows * (eh + gap);
    x = x0;
    for (idx = 0; idx < NCHECKS; idx++) {
        const int w = text_w(d, d->ui, k_chk_names[idx]) + S(d, 28);
        place(d->chk[idx], x, y, w, S(d, 22));
        x += w + S(d, 6);
    }
    place(d->info, x + S(d, 16), y + (S(d, 22) - lh) / 2, x0 + width - x - S(d, 16), lh);
}

static void layout(debugger *d)
{
    RECT client, page;
    int x, y, w, i, px, py, pw, ph, srows, sh;
    const int bh = S(d, 26), eh = S(d, 24), lh = S(d, 18), gap = S(d, 6);

    if (!d->tabs) return;
    GetClientRect(d->hwnd, &client);

    /* The toolbar, two rows: stepping, then breakpoints, symbols, C source
     * and pop out; the status after the first row, or (too narrow for the
     * whole stop reason beside it) on a line of its own. */
    {
        HWND stepping[7] = { d->run_btn, d->step_btn, d->over_btn, d->out_btn, d->step_line_btn, d->over_line_btn,
                             d->run_to_btn };
        const char *stepping_text[7] = { "Pause (F5)", "Step (F7)", "Step Over (F8)", "Step Out (Shift+F8)",
                                         "Step Line (F6)", "Step Over Line (Shift+F6)", "Run to Cursor" };
        HWND tools[5] = { d->clear_bps_btn, d->sym_btn, d->clear_sym_btn, d->src_btn, d->clear_src_btn };
        const char *tools_text[5] = { "Clear Breakpoints", "Load Symbols...", "Clear Symbols", "Load C Source...",
                                      "Clear C Source" };
        char status_text[260];
        int status_w, status_own_line;
        x = S(d, 8);
        for (i = 0; i < 7; i++) {
            w = btn_w(d, stepping_text[i]);
            place(stepping[i], x, S(d, 8), w, bh);
            x += w + gap + (i == 3 || i == 5 ? S(d, 14) : 0);
        }
        {
            const int hw = btn_w(d, "Help");
            const int sright = client.right - hw - gap - S(d, 8);
            place(d->help_btn, client.right - S(d, 8) - hw, S(d, 8), hw, bh);
            GetWindowTextA(d->status, status_text, sizeof status_text);
            status_w = text_w(d, d->ui, status_text) + S(d, 16);
            if (status_w < S(d, 220)) status_w = S(d, 220);
            y = S(d, 8) + bh + S(d, 6);
            status_own_line = sright - x < status_w;
            if (!status_own_line) place(d->status, x, S(d, 8) + (bh - lh) / 2, sright - x, lh);
        }
        x = S(d, 8);
        for (i = 0; i < 5; i++) {
            if (i == 1) {
                const int label_w = text_w(d, d->ui, "Break at:") + S(d, 6);
                place(d->break_at_label, x, y + (eh - lh) / 2, label_w, lh);
                x += label_w;
                place(d->break_at, x, y, S(d, 130), eh);
                x += S(d, 130) + S(d, 14);
            }
            w = btn_w(d, tools_text[i]);
            place(tools[i], x, y - S(d, 1), w, bh);
            x += w + gap;
        }
        w = btn_w(d, "Pop Out");
        place(d->popout_btn, x, y - S(d, 1), w, bh);
        y += bh + S(d, 6);
        if (status_own_line) {          /* too narrow for the whole stop reason beside the buttons */
            place(d->status, S(d, 8), y, client.right - S(d, 16), lh);
            y += lh + S(d, 4);
        }
    }

    /* The tab control is the strip alone: the pages sit on the window's own
     * background, which their captions are drawn on. */
    SetRect(&page, 0, 0, S(d, 400), S(d, 400));
    SendMessageA(d->tabs, TCM_ADJUSTRECT, FALSE, (LPARAM)&page);
    place(d->tabs, S(d, 8), y, client.right - S(d, 16), page.top);
    px = S(d, 12);
    py = y + page.top + S(d, 6);
    pw = client.right - 2 * px;

    /* the registers strip under the pages, on every tab */
    sh = strip_height(d, pw, &srows);
    ph = client.bottom - S(d, 6) - sh - S(d, 6) - py;
    if (pw < S(d, 300)) pw = S(d, 300);
    if (ph < S(d, 200)) ph = S(d, 200);
    layout_strip(d, px, py + ph + S(d, 6), pw);

    for (i = 0; i < TAB_COUNT; i++) {
        int cw = pw, ch = ph;
        if (d->popped[i]) {
            RECT pc;
            GetClientRect(d->popped[i], &pc);
            cw = pc.right - S(d, 16);
            ch = pc.bottom - S(d, 46);
            place(d->pg[i], S(d, 8), S(d, 38), cw, ch);
        } else {
            place(d->pg[i], px, py, pw, ph);
        }
        layout_page(d, i, cw > 1 ? cw : 1, ch > 1 ? ch : 1);
    }

    /* everything again: what the controls left behind where they were */
    RedrawWindow(d->hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    for (i = 0; i < TAB_COUNT; i++)
        if (d->popped[i]) RedrawWindow(d->popped[i], NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

/* ---- peeling a tab off into a window of its own, and putting it back ------------------ */

static void goto_tab(debugger *d, int page)
{
    const int i = tab_index(d, page);
    if (d->popped[page]) {
        SetForegroundWindow(d->popped[page]);
        return;
    }
    if (i >= 0) {
        SendMessageA(d->tabs, TCM_SETCURSEL, (WPARAM)i, 0);
        show_page(d, page);
        layout(d);
        refresh_visible(d);
    }
}

static void dock_page(debugger *d, int page)
{
    TCITEMA item;
    char text[32];
    HWND pop = d->popped[page];
    int pos = 0, p, i;

    if (!pop) return;
    d->popped[page] = NULL;
    SetParent(d->pg[page], d->hwnd);
    for (p = 0; p < page; p++)   /* back where it was in the order */
        if (!d->popped[p]) pos++;
    for (i = d->ntabs; i > pos; i--) d->tab_page[i] = d->tab_page[i - 1];
    d->tab_page[pos] = page;
    d->ntabs++;
    memset(&item, 0, sizeof item);
    item.mask = TCIF_TEXT;
    tab_text(page, text, sizeof text);
    item.pszText = text;
    SendMessageA(d->tabs, TCM_INSERTITEMA, (WPARAM)pos, (LPARAM)&item);
    DestroyWindow(pop);
    SendMessageA(d->tabs, TCM_SETCURSEL, (WPARAM)pos, 0);
    show_page(d, page);
    SetWindowPos(d->tabs, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    layout(d);
    refresh_visible(d);
}

static void pop_out(debugger *d, int page)
{
    HWND pop, dock;
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrA(d->hwnd, GWLP_HINSTANCE);
    char title[96];
    int idx, i, next;

    if (page < 0 || page >= TAB_COUNT || d->popped[page] || d->ntabs <= 1) {
        if (d->ntabs <= 1) toast(d, "One tab always stays in the main window");
        return;
    }
    idx = tab_index(d, page);
    if (idx < 0) return;
    SendMessageA(d->tabs, TCM_DELETEITEM, (WPARAM)idx, 0);
    for (i = idx; i < d->ntabs - 1; i++) d->tab_page[i] = d->tab_page[i + 1];
    d->ntabs--;

    snprintf(title, sizeof title, "CoCo Debugger - %s", k_tabs[page].label);
    pop = CreateWindowExA(0, POP_CLASS, title, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                          S(d, 900), S(d, 620), d->hwnd, NULL, inst, NULL);
    SetWindowLongPtrA(pop, GWLP_USERDATA, page);
    dock = CreateWindowExA(0, "BUTTON", "Dock", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, S(d, 8), S(d, 6),
                           btn_w(d, "Dock"), S(d, 26), pop, (HMENU)(INT_PTR)IDC_DOCK, inst, NULL);
    SendMessageA(dock, WM_SETFONT, (WPARAM)d->ui, TRUE);
    coco_help_set_context(pop, k_tab_contexts[page], 1);
    SetParent(d->pg[page], pop);
    d->popped[page] = pop;
    /* the tab shown stays (when it was the popped one, its neighbour takes over) */
    next = d->page == page ? (idx < d->ntabs ? idx : d->ntabs - 1) : tab_index(d, d->page);
    if (next < 0) next = 0;
    SendMessageA(d->tabs, TCM_SETCURSEL, (WPARAM)next, 0);
    show_page(d, d->tab_page[next]);
    layout(d);
    ShowWindow(pop, SW_SHOW);
    refresh_visible(d);
}

/* A tab container: its controls' messages go to the debugger window. */
static LRESULT CALLBACK page_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    HWND main = (HWND)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_COMMAND:
    case WM_NOTIFY:
    case WM_DRAWITEM:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        if (main && g_dbg) return SendMessageA(main, msg, wp, lp);
        break;
    case WM_ERASEBKGND: {
        RECT c;
        GetClientRect(hwnd, &c);
        FillRect((HDC)wp, &c, GetSysColorBrush(COLOR_BTNFACE));
        return 1;
    }
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* The window a popped-out tab lives in. */
static LRESULT CALLBACK pop_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    const int page = (int)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_SIZE:
        if (g_dbg) layout(g_dbg);
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = 400;
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = 300;
        return 0;
    case WM_COMMAND:
        if (g_dbg && LOWORD(wp) == IDC_DOCK) dock_page(g_dbg, page);
        return 0;
    case WM_MOUSEWHEEL:
        if (g_dbg) return SendMessageA(g_dbg->hwnd, msg, wp, lp);
        break;
    case WM_CLOSE:
        if (g_dbg) dock_page(g_dbg, page);
        else DestroyWindow(hwnd);
        return 0;
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- the window ------------------------------------------------------------------- */

static void hide_window(debugger *d)
{
    KillTimer(d->hwnd, TIMER_REFRESH);
    cocodebug_set_engaged(d->dbg, 0);    /* lets the machine run */
    ShowWindow(d->hwnd, SW_HIDE);
    sound_watch_sync(d);
}

static void attach_and_refresh(debugger *d)
{
    cocodebug_set_engaged(d->dbg, 1);
    cocodebug_pause(d->dbg);     /* stops the machine */
    d->was_stopped = 0;
    d->seen_gen = cocodebug_generation(d->dbg);
    SetTimer(d->hwnd, TIMER_REFRESH, 100, NULL);
    settle_and_sync(d, 1);
}

/* Stop/Run: the accent with white text while the machine is stopped (the
 * button that will start it), the stock look while it runs. */
static void draw_run_button(debugger *d, const DRAWITEMSTRUCT *di)
{
    char text[32];
    const int stopped = cocodebug_is_paused(d->dbg);
    RECT r = di->rcItem;
    if (stopped) {
        FillRect(di->hDC, &r, coco_accent_brush());
        FrameRect(di->hDC, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
        SetTextColor(di->hDC, COCO_ACCENT_TEXT);
    } else {
        DrawFrameControl(di->hDC, &r, DFC_BUTTON, DFCS_BUTTONPUSH | ((di->itemState & ODS_SELECTED) ? DFCS_PUSHED : 0));
        SetTextColor(di->hDC, GetSysColor(COLOR_BTNTEXT));
    }
    SetBkMode(di->hDC, TRANSPARENT);
    GetWindowTextA(di->hwndItem, text, sizeof text);
    SelectObject(di->hDC, d->ui);
    DrawTextA(di->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (di->itemState & ODS_FOCUS) {
        InflateRect(&r, -3, -3);
        DrawFocusRect(di->hDC, &r);
    }
}


static LRESULT on_notify(debugger *d, NMHDR *nh)
{
    if (nh->hwndFrom == d->tabs && nh->code == (UINT)TCN_SELCHANGE) {
        const int sel = (int)SendMessageA(d->tabs, TCM_GETCURSEL, 0, 0);
        if (sel >= 0 && sel < d->ntabs) {
            show_page(d, d->tab_page[sel]);
            refresh_visible(d);
        }
        return 0;
    }
    if (nh->hwndFrom == d->follow_pc && nh->code == (UINT)NM_CUSTOMDRAW)
        return coco_accent_customdraw((const NMCUSTOMDRAW *)nh, following(d));
    if (nh->code == (UINT)NM_CUSTOMDRAW) {
        /* the Record toggles: the accent while recording */
        int k;
        for (k = 0; k < 2; k++)
            if (nh->hwndFrom == d->h_rec[k])
                return coco_accent_customdraw((const NMCUSTOMDRAW *)nh,
                                               SendMessageA(d->h_rec[k], BM_GETCHECK, 0, 0) == BST_CHECKED);
    }
    if (nh->hwndFrom == d->sym_list) {
        if (nh->code == (UINT)NM_DBLCLK || nh->code == (UINT)NM_RETURN) on_symbol_activate(d);
        else if (nh->code == (UINT)LVN_KEYDOWN && ((const NMLVKEYDOWN *)nh)->wVKey == VK_RETURN) on_symbol_activate(d);
        return 0;
    }
    if (nh->hwndFrom != d->bp_list) return 0;
    if (nh->code == (UINT)LVN_ITEMCHANGED) {
        const NMLISTVIEW *nm = (const NMLISTVIEW *)nh;
        /* a row's box: its breakpoint on or off (the list follows after) */
        if (!d->updating && nm->iItem >= 0 && nm->iItem < d->nbps && (nm->uChanged & LVIF_STATE) &&
            ((nm->uNewState ^ nm->uOldState) & LVIS_STATEIMAGEMASK) && (nm->uOldState & LVIS_STATEIMAGEMASK)) {
            const int on = ((nm->uNewState & LVIS_STATEIMAGEMASK) >> 12) == 2;
            if (on != (d->bps[nm->iItem].enabled != 0)) {
                cocodebug_breakpoint_enable(d->dbg, d->bps[nm->iItem].id, on);
                PostMessageA(d->hwnd, WM_DBG_SYNC, 0, 0);
            }
        }
    } else if (nh->code == (UINT)LVN_KEYDOWN) {
        /* after the list's own handling: removing rebuilds it */
        if (((const NMLVKEYDOWN *)nh)->wVKey == VK_DELETE)
            PostMessageA(d->hwnd, WM_COMMAND, MAKEWPARAM(IDC_BP_REMOVE, BN_CLICKED), 0);
    } else if (nh->code == (UINT)NM_DBLCLK) {
        /* an address breakpoint's row opens the disassembly there */
        const int i = bp_selected(d);
        if (i >= 0 && !(d->bps[i].type & COCODEBUG_BP_PHYS)) {
            char a[16];
            snprintf(a, sizeof a, "$%04X", d->bps[i].start);
            goto_tab(d, TAB_DISASM);
            SetWindowTextA(d->jump, a);
            on_jump(d);
        }
    }
    return 0;
}

static LRESULT CALLBACK dbg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;
    if (msg == WM_GETMINMAXINFO) {
        const int dpi = d && d->dpi ? d->dpi : 96;
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = MulDiv(860, dpi, 96);
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = MulDiv(560, dpi, 96);
        return 0;
    }
    if (!d || d->hwnd != hwnd) return DefWindowProcA(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_SIZE:
        layout(d);
        return 0;
    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)lp;
        d->dpi = HIWORD(wp);
        make_fonts(d);       /* (also the controls of popped-out tabs) */
        size_columns(d);
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        layout(d);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_REFRESH) tick(d);
        return 0;
    case WM_DBG_ACCEPT:
        on_accept(d, (int)wp);
        return 0;
    case WM_DBG_TAB:
        complete_console(d);
        return 0;
    case WM_DBG_HISTORY:
        history_step(d, (int)wp);
        return 0;
    case WM_DBG_SYNC:
        resync(d);
        return 0;
    case WM_DBG_LINE:
        if (wp == IDC_SRC_VIEW) src_click(d, (int)lp);
        return 0;
    case WM_NOTIFY:
        return on_notify(d, (NMHDR *)lp);
    case WM_DRAWITEM:
        if (wp == IDC_RUN) {
            draw_run_button(d, (const DRAWITEMSTRUCT *)lp);
            return TRUE;
        }
        break;
    case WM_CTLCOLORSTATIC: {
        HWND h = (HWND)lp;
        HDC dc = (HDC)wp;
        if (in_list(h, d->views, d->nviews)) {
            /* read-only views on the window colour, as text views are */
            SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        if (h == d->bp_msg || h == d->wt_msg) {
            SetTextColor(dc, RGB(0xc0, 0x1c, 0x28));
            SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        if (h == d->status && notice_showing(d)) {
            SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
            SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        if (in_list(h, d->dim, d->ndim)) {
            SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
            SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    }
    case WM_MOUSEWHEEL: {
        /* the disassembly scrolls under the pointer, focused or not */
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT r;
        GetWindowRect(d->disasm, &r);
        if (page_visible(d, TAB_DISASM) && PtInRect(&r, p)) return SendMessageA(d->disasm, msg, wp, lp);
        GetWindowRect(d->mem_view, &r);
        if (page_visible(d, TAB_MEMORY) && PtInRect(&r, p)) return SendMessageA(d->mem_view, msg, wp, lp);
        GetWindowRect(d->src_view, &r);
        if (page_visible(d, TAB_CSOURCE) && PtInRect(&r, p)) return SendMessageA(d->src_view, msg, wp, lp);
        break;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id == IDC_SYM_FILTER && HIWORD(wp) == EN_CHANGE) {
            refresh_symbols(d);
            return 0;
        }
        if (id >= IDC_CHK0 && id <= IDC_CHK_LAST && HIWORD(wp) == BN_CLICKED) {
            on_check(d, id - IDC_CHK0);
            return 0;
        }
        switch (id) {
        case IDC_RUN: on_run(d); return 0;
        case IDC_STEP: cocodebug_step(d->dbg); settle_and_sync(d, 1); return 0;
        case IDC_OVER: cocodebug_step_over(d->dbg); settle_and_sync(d, 1); return 0;
        case IDC_OUT: cocodebug_step_out(d->dbg); settle_and_sync(d, 1); return 0;
        case IDC_STEP_LINE: cocodebug_step_line(d->dbg); settle_and_sync(d, 1); return 0;
        case IDC_OVER_LINE: cocodebug_step_over_line(d->dbg); settle_and_sync(d, 1); return 0;
        case IDC_CLEAR_BPS: on_clear_breakpoints(d); return 0;
        case IDC_CLEAR_SYMBOLS: on_clear_symbols(d); return 0;
        case IDC_RUN_TO: on_run_to(d); return 0;
        case IDC_TOGGLE_BP: on_toggle_bp(d); return 0;
        case IDC_HELP_BTN: coco_help_for(d->hwnd); return 0;
        case IDC_SYMBOLS: on_symbols(d); return 0;
        case IDC_LOAD_SRC: on_load_src(d); return 0;
        case IDC_CLEAR_SRC: on_clear_src(d); return 0;
        case IDC_SRC_FILES:
            if (HIWORD(wp) == CBN_SELCHANGE && !d->updating) {
                const int sel = (int)SendMessageA(d->src_files, CB_GETCURSEL, 0, 0);
                if (sel >= 0) d->src_file = sel;
                refresh_csource(d);
            }
            return 0;
        case IDC_SYM_GOTO: on_symbol_activate(d); return 0;
        case IDC_SYM_BREAK: on_symbol_break(d); return 0;
        case IDC_SYM_LOAD: on_symbols(d); return 0;
        case IDC_SYM_CLEAR: on_clear_symbols(d); return 0;
        case IDC_FUJI_HIDE:
            if (HIWORD(wp) == BN_CLICKED) refresh_fujinet(d, 1);
            return 0;
        case IDC_BP_PHYS:
            if (HIWORD(wp) == BN_CLICKED) on_bp_type(d);
            return 0;
        case IDC_FOLLOW_PC: refresh_disasm(d); return 0;
        case IDC_POPOUT:
            pop_out(d, d->page);
            return 0;
        case IDC_MEM_SPACE:
            if (HIWORD(wp) == CBN_SELCHANGE && !d->updating)
                set_mem_space(d, (int)SendMessageA(d->mem_space, CB_GETCURSEL, 0, 0));
            return 0;
        case IDC_SRCH_NEXT: on_search(d, 0); return 0;
        case IDC_SRCH_PREV: on_search(d, 1); return 0;
        case IDC_WT_ADD: on_watch_add(d); return 0;
        case IDC_WT_REMOVE: on_watch_remove(d); return 0;
        case IDC_WT_MEM: on_watch_memory(d); return 0;
        case IDC_WT_BRK: on_watch_break(d); return 0;
        case IDC_WT_LIST:
            if (HIWORD(wp) == LBN_DBLCLK) on_watch_memory(d);
            return 0;
        case IDC_H_REC0: on_record(d, 0); return 0;
        case IDC_H_REC1: on_record(d, 1); return 0;
        case IDC_H_CLEAR0: on_hist_clear(d, 0); return 0;
        case IDC_H_CLEAR1: on_hist_clear(d, 1); return 0;
        case IDC_H_FILTER0:
            if (HIWORD(wp) == CBN_SELCHANGE) on_hist_filter(d, 0);
            return 0;
        case IDC_H_FILTER1:
            if (HIWORD(wp) == CBN_SELCHANGE) on_hist_filter(d, 1);
            return 0;
        case IDC_BP_CLEAR_HITS:
            cocodebug_breakpoint_clear_hits(d->dbg);
            d->bp_sig_ok = 0;
            resync(d);
            return 0;
        case IDC_BP_TYPE:
            if (HIWORD(wp) == CBN_SELCHANGE) on_bp_type(d);
            return 0;
        case IDC_BP_ADD: on_bp_add(d); return 0;
        case IDC_BP_REMOVE: on_bp_remove(d); return 0;
        case IDC_BP_CLEAR: on_clear_breakpoints(d); return 0;
        case IDC_FUJI_CLEAR:
            cocodebug_fujinet_trace_clear(d->dbg);
            refresh_fujinet(d, 1);
            return 0;
        case IDCANCEL: hide_window(d); return 0;
        default: break;
        }
        break;
    }
    case WM_CLOSE:
        /* Hide and let the machine run; F12 brings it back, stopped. */
        hide_window(d);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_REFRESH);
        if (cocodebug_is_engaged(d->dbg)) cocodebug_set_engaged(d->dbg, 0);
        if (d->snd_watching) cocodebug_sound_watch(d->dbg, 0);
        DeleteObject(d->ui);
        DeleteObject(d->ui_bold);
        DeleteObject(d->mono);
        DeleteObject(d->bp_on);
        DestroyAcceleratorTable(d->accel);
        free(d->src_buf);
        free(d->src_text);
        free(d);
        g_dbg = NULL;
        return 0;
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- entry points ----------------------------------------------------------------- */

static void register_class(HINSTANCE inst, const char *name, WNDPROC proc, HBRUSH background, UINT style)
{
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = style;
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = background;
    wc.lpszClassName = name;
    if (proc == dbg_proc || proc == pop_proc) wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(IDI_APPICON));
    RegisterClassExA(&wc);
}

/* COCO_DEBUGGER_TAB: a tab number, or a tab's name or label (any case), or
 * the start of one, or of a word of a label ("memory", "source", "i/o"...).
 * The old two-tab names still work: cpu is the Disassembly. */
static int tab_from_env_var(const char *var)
{
    const char *tab = getenv(var);
    char *end;
    long number;
    size_t len;
    int idx, pass;
    if (!tab || !*tab) return -1;
    if (_stricmp(tab, "io") == 0 || _stricmp(tab, "i/o") == 0) return TAB_CART;
    if (_stricmp(tab, "cpu") == 0) return TAB_DISASM;
    number = strtol(tab, &end, 10);
    if (end != tab && *end == '\0')
        return (number >= 0 && number < TAB_COUNT) ? (int)number : -1;
    len = strlen(tab);
    for (pass = 0; pass < 3; pass++)
        for (idx = 0; idx < TAB_COUNT; idx++) {
            if (pass == 0 && _strnicmp(tab, k_tabs[idx].name, len) == 0) return idx;
            if (pass == 1 && _strnicmp(tab, k_tabs[idx].label, len) == 0) return idx;
            if (pass == 2) {
                const char *word = k_tabs[idx].label;
                while ((word = strchr(word, ' ')) != NULL)
                    if (_strnicmp(tab, ++word, len) == 0) return idx;
            }
        }
    return -1;
}

/* A space by name (any case, a prefix will do); -1 if none. */
static int space_from_name(const char *name)
{
    int idx;
    if (!name || !*name) return -1;
    for (idx = 0; idx < NSPACES; idx++)
        if (_strnicmp(name, k_space_names[idx], strlen(name)) == 0) return idx;
    return -1;
}

static int tab_from_env(void)
{
    return tab_from_env_var("COCO_DEBUGGER_TAB");
}

/* The env hooks for headless captures, once the window is up: the Memory
 * space, a watch, a break-on-change, the Memory tab's Go to, recording and a
 * popped-out tab. */
static void env_hooks(debugger *d)
{
    const char *value;
    int tab;
    if ((value = getenv("COCO_MEM_SPACE")) != NULL && *value && space_from_name(value) >= 0)
        set_mem_space(d, space_from_name(value));
    if ((value = getenv("COCO_WATCH")) != NULL && *value) {
        /* a comma-separated list; each may start cpu: or phys: */
        char list[512], *item, *save = NULL;
        snprintf(list, sizeof list, "%s", value);
        for (item = strtok_s(list, ",", &save); item; item = strtok_s(NULL, ",", &save)) {
            char *colon;
            int space = COCODEBUG_SPACE_CPU;
            item = strip(item);
            colon = strchr(item, ':');
            if (colon && colon - item < 8) {
                *colon = '\0';
                space = space_from_name(strip(item));
                if (space < 0) space = COCODEBUG_SPACE_CPU;
                item = strip(colon + 1);
            }
            if (*item) cocowatch_add(item, space);
        }
    }
    if ((value = getenv("COCO_WATCH_BREAK")) != NULL && *value) {
        /* a watch, and a break on change armed over it */
        const int index = cocowatch_add(value, COCODEBUG_SPACE_CPU);
        uint32_t lo, hi;
        char err[96];
        if (index >= 0 && cocowatch_range(d->dbg, index, &lo, &hi))
            break_on_change(d, COCODEBUG_SPACE_CPU, lo, hi, err, sizeof err);
    }
    if ((value = getenv("COCO_MEM_GOTO")) != NULL && *value) {
        SetWindowTextA(d->mem_goto, value);
        on_mem_goto(d);
    }
    if ((value = getenv("COCO_HISTORY")) != NULL && *value && *value != '0') cocodebug_trace_enable(d->dbg, 1);
    if ((tab = tab_from_env_var("COCO_POPOUT")) >= 0) pop_out(d, tab);
    resync(d);
}

void coco_debugger_show(HWND parent, cocosession *session)
{
    static int registered;
    INITCOMMONCONTROLSEX icc;
    HINSTANCE inst;
    debugger *d;
    ACCEL accels[8];
    int tab, w, h;
    MONITORINFO mi;

    if (g_dbg) {
        d = g_dbg;
        ShowWindow(d->hwnd, IsIconic(d->hwnd) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(d->hwnd);
        if (!cocodebug_is_engaged(d->dbg) || !IsWindowVisible(d->hwnd) || !cocodebug_is_paused(d->dbg))
            attach_and_refresh(d);
        else
            resync(d);
        return;
    }

    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icc);
    inst = (HINSTANCE)GetWindowLongPtrA(parent, GWLP_HINSTANCE);
    if (!inst) inst = GetModuleHandleA(NULL);
    if (!registered) {
        register_class(inst, DBG_CLASS, dbg_proc, (HBRUSH)(COLOR_BTNFACE + 1), CS_HREDRAW | CS_VREDRAW);
        register_class(inst, DISASM_CLASS, disasm_proc, NULL, CS_DBLCLKS);
        register_class(inst, MEMVIEW_CLASS, memview_proc, NULL, 0);
        register_class(inst, LINES_CLASS, lines_proc, NULL, 0);
        register_class(inst, PAGE_CLASS, page_proc, (HBRUSH)(COLOR_BTNFACE + 1), 0);
        register_class(inst, POP_CLASS, pop_proc, (HBRUSH)(COLOR_BTNFACE + 1), CS_HREDRAW | CS_VREDRAW);
        registered = 1;
    }

    d = calloc(1, sizeof *d);
    if (!d) return;
    d->session = session;
    d->dbg = cocosession_debugger(session);
    if (!d->dbg) {
        free(d);
        MessageBoxA(parent, "The debugger could not be created for this session.", "Debugger Unavailable",
                    MB_ICONERROR);
        return;
    }
    d->selected = -1;
    d->pc_line = -1;
    d->fuji_count = -1;
    d->mem_sp = COCODEBUG_SPACE_CPU;
    d->mem_digits = 4;
    d->mem_size = 0x10000;
    d->mem_buf_line = -1;
    g_dbg = d;

    /* the start size, held inside the monitor's work area */
    d->dpi = window_dpi(parent);
    w = S(d, START_W);
    h = S(d, START_H);
    mi.cbSize = sizeof mi;
    if (GetMonitorInfoA(MonitorFromWindow(parent, MONITOR_DEFAULTTONEAREST), &mi)) {
        if (w > mi.rcWork.right - mi.rcWork.left - S(d, 40)) w = mi.rcWork.right - mi.rcWork.left - S(d, 40);
        if (h > mi.rcWork.bottom - mi.rcWork.top - S(d, 40)) h = mi.rcWork.bottom - mi.rcWork.top - S(d, 40);
    }
    /* owned by the main window: over it, and gone with it */
    d->hwnd = CreateWindowExA(0, DBG_CLASS, "CoCo Debugger", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              CW_USEDEFAULT, CW_USEDEFAULT, w, h, parent, NULL, inst, NULL);
    if (!d->hwnd) {
        free(d);
        g_dbg = NULL;
        return;
    }
    if (window_dpi(d->hwnd) != d->dpi) {
        /* placed on a monitor of another DPI than the parent's */
        const int was = d->dpi;
        d->dpi = window_dpi(d->hwnd);
        SetWindowPos(d->hwnd, NULL, 0, 0, MulDiv(w, d->dpi, was), MulDiv(h, d->dpi, was),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    make_fonts(d);
    d->bp_on = CreateSolidBrush(RGB(0xe0, 0x1b, 0x24));

    build_controls(d);

    /* the tab body under every page: its repaints never cover a label or a
     * pane (the Win32 FujiNet-tab overlap of the 2026-11-01 notes) */
    SetWindowPos(d->tabs, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    /* F5/F6/F7/F8/Shift+F6/Shift+F8/F9/F12 work wherever the focus is in the
     * window. */
    accels[0].fVirt = FVIRTKEY;          accels[0].key = VK_F5;  accels[0].cmd = IDC_RUN;
    accels[1].fVirt = FVIRTKEY;          accels[1].key = VK_F7;  accels[1].cmd = IDC_STEP;
    accels[2].fVirt = FVIRTKEY;          accels[2].key = VK_F8;  accels[2].cmd = IDC_OVER;
    accels[3].fVirt = FVIRTKEY | FSHIFT; accels[3].key = VK_F8;  accels[3].cmd = IDC_OUT;
    accels[4].fVirt = FVIRTKEY;          accels[4].key = VK_F6;  accels[4].cmd = IDC_STEP_LINE;
    accels[5].fVirt = FVIRTKEY | FSHIFT; accels[5].key = VK_F6;  accels[5].cmd = IDC_OVER_LINE;
    accels[6].fVirt = FVIRTKEY;          accels[6].key = VK_F9;  accels[6].cmd = IDC_TOGGLE_BP;
    accels[7].fVirt = FVIRTKEY;          accels[7].key = VK_F12; accels[7].cmd = IDCANCEL;
    d->accel = CreateAcceleratorTableA(accels, 8);

    tab = tab_from_env();
    d->page = tab >= 0 ? tab : TAB_CONSOLE;
    SendMessageA(d->tabs, TCM_SETCURSEL, (WPARAM)d->page, 0);
    show_page(d, d->page);
    layout(d);

    ShowWindow(d->hwnd, SW_SHOW);
    if (d->page == TAB_CONSOLE) SetFocus(d->con_in);
    attach_and_refresh(d);
    env_hooks(d);
}

int coco_debugger_visible(void)
{
    return g_dbg && IsWindowVisible(g_dbg->hwnd);
}

void coco_debugger_toggle(HWND parent, cocosession *session)
{
    if (coco_debugger_visible()) hide_window(g_dbg);
    else coco_debugger_show(parent, session);
}

int coco_debugger_pretranslate(MSG *msg)
{
    if (!g_dbg || !g_dbg->accel) return 0;
    if (msg->hwnd != g_dbg->hwnd && !IsChild(g_dbg->hwnd, msg->hwnd)) return 0;
    /* F12 closes without waiting for the accelerator (Windows keeps F12 for
     * debuggers); its key-up goes with it */
    if (msg->message == WM_KEYDOWN && msg->wParam == VK_F12) {
        hide_window(g_dbg);
        return 1;
    }
    return TranslateAcceleratorA(g_dbg->hwnd, g_dbg->accel, msg) ? 1 : 0;
}
