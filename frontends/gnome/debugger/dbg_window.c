/*
 * Debugger window (GTK4/libadwaita) over the session's 6809/6309 debugger
 * engine, via core/include/cocodebug.h (the same contract as the other
 * Go desktops' family windows, and this window follows theirs).
 *
 * Tabs (COCO_DEBUGGER_TAB takes the number, or the start of the name; "cpu"
 * is still Disassembly):
 *   0 Console          the engine's command set, Tab completion, history
 *   1 Disassembly      breakpoint gutter, follow-PC, jump box, selectable row
 *   2 C Source         the CMOC listing: file picker, PC bar, red-dot
 *                      breakpoints, the function's locals
 *   3 Memory           the CPU's 64K, or a CoCo 3's physical RAM (PHYS), with
 *                      go-to (a range is highlighted), region labels, search
 *                      (hex or text) and poke
 *   4 Breakpoints      why it stopped; execute, read, write, access (PHYS on a
 *                      CoCo 3), conditions, hit counts
 *   5 Watches          addresses, symbols and ranges with their live values
 *   6 Cart & I/O       the cartridge, the memory map, the PIAs, the SAM, the
 *                      GIME, the joysticks and the Becker link
 *   7 Trace            the instruction ring, live while the machine runs
 *   8 History          the same ring, read when the machine stops
 *   9 Video            the picture (four views, overlays, hover/pin), and
 *                      pixel, registers, colours, lines and timing sections
 *  10 Sound            the DAC, the single-bit line, the mux and a waveform
 *  11 Symbols          every known symbol, filtered; a row opens the disassembly
 *  12 FujiNet          every DriveWire packet, with the Becker port's state
 * The registers (PC A B D DP X Y U S CC, the CC flags, and on a 6309 E F W V MD)
 * sit under the tabs on every tab and are editable while the machine is
 * stopped. Any tab can be popped out into a window of its own (Pop Out, then
 * Dock; closing that window docks it); one tab always stays in the main
 * window. The family's keys apply (F5 run/stop, F6 step line, Shift+F6 step
 * over line, F7 step, F8 step over, Shift+F8 step out, F9 breakpoint on the
 * selected line, F12 close).
 *
 * The engine is engaged only while this window is showing: showing it engages
 * and stops the machine, hiding it releases the engine and lets the machine run
 * on at full speed.
 *
 * The window polls the engine's generation counter on a short timer and
 * refreshes only the tabs in front, and only when something changed, so a
 * stopped machine costs nothing and a running one shows live values twice a
 * second. Every new stop (cocodebug_stop_snapshot's serial) brings the
 * disassembly back to the PC and the trace and history to their newest line.
 * Environment hooks read at creation: COCO_DEBUGGER_TAB, COCO_WATCH,
 * COCO_WATCH_BREAK, COCO_MEM_GOTO, COCO_HISTORY, COCO_POPOUT (and the Video
 * tab's COCO_DEBUGGER_VIDEO_*).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include <pango/pangocairo.h>
#include <stdlib.h>
#include <string.h>

#include "cocodebug.h"
#include "cocomemtools.h"
#include "../window.h"
#include "../help/help_window.h"
#include "dbg_video.h"

#define DISASM_WINDOW 48
#define MEM_ROWS 32
#define MEM_PAGE (MEM_ROWS * 16)
#define POKE_MAX 64
#define TRACE_MAX 512
#define HIST_SHOWN 2000   /* lines on a Trace or History page */
#define HIST_READ 65536   /* the ring */
#define NTABS 13
#define NREGS COCODEBUG_REG_COUNT
#define SYMBOLS_SHOWN 2000   /* rows in the Symbols list */
#define SOUND_SHOWN 4096     /* writes of the DAC history the waveform draws */

typedef struct {
    GtkWindow *win;
    cocosession *session;
    cocodebug *dbg;
    unsigned seen_generation;
    gboolean was_stopped;
    gboolean closing;         /* the window is being destroyed: touch nothing */
    int running_ticks;
    guint timer;

    AdwWindowTitle *title;    /* "Debugger", with the stop reason under it */
    GtkButton *run_btn;
    GtkWidget *step_line_btn, *step_over_line_btn;
    AdwToastOverlay *toasts;
    GtkNotebook *notebook;

    /* console */
    GtkTextView *console_out;
    GtkEntry *console_in;
    GPtrArray *history;
    guint history_pos;

    /* the registers strip */
    GtkEntry *reg[NREGS];
    GtkWidget *reg_box[NREGS];    /* the entry with its label (6309 ones hide) */
    GtkWidget *extra_box;         /* the 6309's E F W V MD */
    GtkCheckButton *flag[8];      /* E F H I N Z V C */
    GtkLabel *machine;
    gboolean flag_sync;           /* the boxes are being set from the engine */
    gboolean force_entries;       /* a refresh may overwrite a focused entry */

    /* memory */
    GtkTextView *mem_view;
    GtkEntry *mem_goto, *mem_write;
    GtkDropDown *mem_space;
    GtkWidget *mem_space_box;
    uint32_t mem_base[2];
    gboolean mem_hl;          /* a range is highlighted (Go to, a search hit, a watch) */
    int mem_hl_space;
    uint32_t mem_hl_lo, mem_hl_hi;
    GtkDropDown *srch_kind;
    GtkEntry *srch_entry;
    GtkLabel *srch_note;

    /* tabs: the page widget of each, and the window it is popped out into */
    GtkWidget *pages[NTABS];
    GtkWindow *detached[NTABS];
    uint64_t seen_serial;     /* the stop serial last acted on */

    /* disassembly */
    GtkTextView *disasm;
    GtkCheckButton *follow_pc;
    GtkEntry *jump;
    int disasm_top;           /* address of the first instruction */
    int line_addr[DISASM_WINDOW * 2];   /* per buffer line; -1 for a label line */
    int line_count;
    int selected;             /* the selected row's address, -1 none */

    /* C source: a file picker over the table and the listing of the one chosen */
    GtkDropDown *src_file_combo;
    GtkListBox *src_list;
    GtkLabel *src_locals;
    GtkLabel *src_note;
    char *src_cached_path;    /* which file src_cached_lines holds */
    char **src_cached_lines;  /* g_strsplit() result, NULL-terminated */
    int src_displayed_file_id;
    int pc_file_id, pc_line;  /* where PC is, whichever file the picker shows */
    gboolean src_scroll;      /* the next listing brings the PC line into view */
    guint src_scroll_id;
    GString *src_sig;         /* what the listing shows, to skip a needless rebuild */

    /* breakpoints */
    GtkListBox *bp_list;
    GtkDropDown *bp_type;
    GtkCheckButton *bp_phys;
    GtkEntry *bp_start, *bp_end, *bp_cond;
    GtkEntry *break_at;       /* the toolbar's "Break at" box */
    GtkLabel *bp_msg;
    GtkLabel *bp_stop;        /* "Stopped: ..." / "Running." */
    GString *bp_sig;          /* what the list shows, to skip a needless rebuild */

    /* watches */
    GtkEntry *wt_entry;
    GtkDropDown *wt_space;
    GtkListBox *wt_list;
    GPtrArray *wt_labels;     /* a GtkLabel per row, rewritten in place */
    unsigned wt_gen;
    gboolean wt_built;

    /* cart & i/o, sound */
    GtkTextView *cart_text;
    GtkTextView *sound_text;
    GtkWidget *sound_wave;
    gboolean sound_watching;
    cocodebug_sound_write *sound_hist;   /* newest first */
    int sound_hist_n;

    /* symbols */
    GtkEntry *sym_filter;
    GtkListBox *sym_list;
    GtkLabel *sym_note;
    GString *sym_sig;         /* what the list shows, to skip a needless rebuild */

    /* trace (0) and history (1): the same ring, two views */
    GtkToggleButton *h_rec[2];
    GtkDropDown *h_filter[2];
    GtkEntry *h_range[2];
    GtkLabel *h_note[2];
    GtkTextView *h_text[2];
    uint64_t h_sig[2];        /* what each page last drew; ~0 forces a redraw */
    gboolean h_sync;          /* the Record toggles are being set from the engine */
    int trace_ticks;

    /* fujinet */
    GtkLabel *fuji_note;
    GtkTextView *fuji_text, *fuji_state;
    GtkCheckButton *fuji_hide_ready;
    uint64_t fuji_last;
    int fuji_count;
} DbgWin;

static DbgWin *g_win;

/* The register entries, in the engine's COCODEBUG_REG_* order, each with how
 * many hex digits it shows. */
static const struct { const char *name; int digits; } k_regs[NREGS] = {
    { "PC", 4 }, { "A", 2 }, { "B", 2 }, { "D", 4 }, { "DP", 2 }, { "X", 4 }, { "Y", 4 },
    { "U", 4 }, { "S", 4 }, { "CC", 2 }, { "E", 2 }, { "F", 2 }, { "W", 4 }, { "V", 4 },
    { "MD", 2 },
};
#define REGS_6309 COCODEBUG_REG_E      /* the 6309-only registers start here */

/* The CC bits, high to low as the strip shows them. */
static const struct { const char *name; unsigned mask; } k_flags[8] = {
    { "E", COCODEBUG_CC_E }, { "F", COCODEBUG_CC_F }, { "H", COCODEBUG_CC_H },
    { "I", COCODEBUG_CC_I }, { "N", COCODEBUG_CC_N }, { "Z", COCODEBUG_CC_Z },
    { "V", COCODEBUG_CC_V }, { "C", COCODEBUG_CC_C },
};

/* The tabs, in order; COCO_DEBUGGER_TAB takes a number or the start of a name. */
static const char *const k_tab_names[NTABS] = {
    "Console", "Disassembly", "C Source", "Memory", "Breakpoints", "Watches", "Cart & I/O",
    "Trace", "History", "Video", "Sound", "Symbols", "FujiNet",
};
enum { TAB_CONSOLE, TAB_DISASM, TAB_CSRC, TAB_MEMORY, TAB_BREAKS, TAB_WATCHES, TAB_CART,
       TAB_TRACE, TAB_HISTORY, TAB_VIDEO, TAB_SOUND, TAB_SYMBOLS, TAB_FUJINET };

/* The Memory tab's drop-down, and the engine's spaces behind it. */
static const char *const k_space_labels[3] = { "CPU", "PHYS", NULL };

/* ---- helpers -------------------------------------------------------------- */

/* Engine text for a text buffer: valid UTF-8, control bytes dropped. */
static char *clean_text(const char *text)
{
    char *value = g_utf8_make_valid(text, -1), *source, *dest;
    for (source = dest = value; *source; source++)
        if ((unsigned char)*source >= 0x20 || *source == '\n' || *source == '\t') *dest++ = *source;
    *dest = '\0';
    return value;
}

static GtkAdjustment *vadj(GtkTextView *view)
{
    GtkWidget *parent = gtk_widget_get_parent(GTK_WIDGET(view));
    return GTK_IS_SCROLLED_WINDOW(parent)
        ? gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(parent)) : NULL;
}

/* A log (the console, the FujiNet packets) follows its end while its
 * reader is there. New text grows the view only once it is laid out (or its
 * tab first shows), so the end is pinned whenever the view's height
 * changes; whether to follow is decided as text arrives. */
static gboolean following(GtkTextView *view)
{
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(view), "follow")) != 0;
}

static void on_log_resized(GtkAdjustment *adj, gpointer view)
{
    if (following(view))
        gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj) -
                                      gtk_adjustment_get_page_size(adj));
}

static void make_log(GtkTextView *view)
{
    g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
    /* tied to the view: the adjustment can outlive it while the window dies */
    g_signal_connect_object(vadj(view), "changed", G_CALLBACK(on_log_resized), view, 0);
}

/* Before new text: follow if the reader is at the end (or never looked). */
static void log_note_reader(GtkTextView *view)
{
    GtkAdjustment *adj = vadj(view);
    g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(
        gtk_adjustment_get_page_size(adj) <= 0.0 ||
        gtk_adjustment_get_value(adj) + gtk_adjustment_get_page_size(adj) >=
            gtk_adjustment_get_upper(adj) - 2.0));
}

/* Replaces a view's text where the reader left it: a refresh (a step, or
 * the live values twice a second) must not throw a scrolled dump back to
 * its top. The top line is noted and scrolled back to once the new text is
 * laid out (scroll_to_mark waits for that). A log that is following goes
 * to its end instead. */
static void set_text_at(GtkTextView *view, const char *text, gboolean follow)
{
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(view);
    GtkAdjustment *adj = vadj(view);
    char *clean = clean_text(text);
    GtkTextIter iter;
    int top = 0;

    if (!follow && adj && gtk_adjustment_get_value(adj) > 0.0) {
        gtk_text_view_get_line_at_y(view, &iter, (int)gtk_adjustment_get_value(adj), NULL);
        top = gtk_text_iter_get_line(&iter);
    }
    {
        GtkTextIter iter_start, iter_end;
        char *old;
        gboolean same;

        /* unchanged text is not set again: it would reset the selection and
         * relayout for nothing */
        gtk_text_buffer_get_bounds(buffer, &iter_start, &iter_end);
        old = gtk_text_buffer_get_text(buffer, &iter_start, &iter_end, FALSE);
        same = strcmp(old, clean) == 0;
        g_free(old);
        if (same) {
            g_free(clean);
            if (follow) {
                g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
                if (adj) on_log_resized(adj, view);
            }
            return;
        }
    }
    gtk_text_buffer_set_text(buffer, clean, -1);
    g_free(clean);
    if (follow) {
        g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
        if (adj) on_log_resized(adj, view);
        return;
    }
    if (top > 0) {
        GtkTextMark *mark;
        gtk_text_buffer_get_iter_at_line(buffer, &iter, top);
        mark = gtk_text_buffer_create_mark(buffer, NULL, &iter, TRUE);
        gtk_text_view_scroll_to_mark(view, mark, 0.0, TRUE, 0.0, 0.0);
        gtk_text_buffer_delete_mark(buffer, mark);
    }
}

static void set_text(GtkTextView *view, const char *text)
{
    set_text_at(view, text, FALSE);
}

static void append_text(GtkTextView *view, const char *text, gboolean force_follow)
{
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(view);
    GtkTextIter end;
    char *clean = clean_text(text);
    log_note_reader(view);
    if (force_follow)
        g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_insert(buffer, &end, clean, -1);
    g_free(clean);
}

/* A number as typed: hex by default, $ or 0x or a trailing h for hex, #
 * for decimal. */
static int parse_num(const char *text, long *out)
{
    char buf[64], *end;
    size_t count;
    long value;
    while (*text == ' ') text++;
    g_strlcpy(buf, text, sizeof buf);
    count = strlen(buf);
    while (count && buf[count - 1] == ' ') buf[--count] = '\0';
    if (!count) return 0;
    if (buf[0] == '$') value = strtol(buf + 1, &end, 16);
    else if (buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X')) value = strtol(buf + 2, &end, 16);
    else if (buf[0] == '#') value = strtol(buf + 1, &end, 10);
    else if (buf[count - 1] == 'h' || buf[count - 1] == 'H') { buf[count - 1] = '\0'; value = strtol(buf, &end, 16); }
    else value = strtol(buf, &end, 16);
    if (end == buf || *end) return 0;
    *out = value;
    return 1;
}

/* An address box: a symbol (as the engine does, a name wins over a hex
 * literal), bare hex, or any expression the console takes ($A000+4, pc, x,
 * [s], label+2). `space` is the engine's COCODEBUG_SPACE_*: a PHYS address is
 * up to six hex digits and has no symbols. 0 with the reason in err. */
static int parse_addr(DbgWin *state, const char *text, int space, long *out, char *err, int errsz)
{
    char buf[128];
    long value, limit = space == COCODEBUG_SPACE_PHYS
        ? (long)cocodebug_space_size(state->dbg, space) - 1 : 0xFFFF;
    uint16_t sym;
    g_strlcpy(buf, text, sizeof buf);
    g_strstrip(buf);
    if (!buf[0]) {
        g_strlcpy(err, "Enter an address, a symbol or an expression", errsz);
        return 0;
    }
    if (space == COCODEBUG_SPACE_CPU && cocodebug_symbol_find(state->dbg, buf, &sym)) {
        *out = sym;
        return 1;
    }
    if (parse_num(buf, &value) || (cocodebug_eval(state->dbg, buf, &value, err, errsz) == 0)) {
        if (value < 0 || value > limit) {
            g_snprintf(err, errsz, "Out of range (0 to $%lX)", limit);
            return 0;
        }
        *out = value;
        return 1;
    }
    if (!err[0])
        g_strlcpy(err, "Not an address, a symbol or an expression", errsz);
    return 0;
}

/* A refresh does not overwrite a value being typed: a focused entry whose
 * text is not what was last put there keeps it, until Enter (force_entries)
 * says to take the engine's. */
static void set_entry(DbgWin *state, GtkEntry *entry, const char *text)
{
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(entry));
    GtkWidget *focus = root ? gtk_root_get_focus(root) : NULL;
    const char *shown = g_object_get_data(G_OBJECT(entry), "shown");
    if (focus && !state->force_entries && shown &&
        (focus == GTK_WIDGET(entry) || gtk_widget_is_ancestor(focus, GTK_WIDGET(entry))) &&
        strcmp(gtk_editable_get_text(GTK_EDITABLE(entry)), shown) != 0)
        return;
    gtk_editable_set_text(GTK_EDITABLE(entry), text);
    g_object_set_data_full(G_OBJECT(entry), "shown", g_strdup(text), g_free);
}

static GtkWidget *mono_view(GtkTextView **out, gboolean wrap)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), wrap ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_hexpand(scroll, TRUE);
    *out = GTK_TEXT_VIEW(view);
    return scroll;
}

static GtkWidget *labeled(const char *text, GtkWidget *child)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *label = gtk_label_new(text);
    gtk_widget_add_css_class(label, "dim-label");
    gtk_box_append(GTK_BOX(box), label);
    gtk_box_append(GTK_BOX(box), child);
    return box;
}

/* A heading over a block (the FujiNet pane). */
static GtkWidget *headed(const char *text, GtkWidget *child)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *label = gtk_label_new(text);
    gtk_widget_add_css_class(label, "heading");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_append(GTK_BOX(box), label);
    gtk_box_append(GTK_BOX(box), child);
    return box;
}

static GtkWidget *padded(GtkWidget *widget)
{
    gtk_widget_set_margin_top(widget, 8);
    gtk_widget_set_margin_bottom(widget, 8);
    gtk_widget_set_margin_start(widget, 8);
    gtk_widget_set_margin_end(widget, 8);
    return widget;
}

/* The CoCo accent is the tan: it is pale, so a line marked in it keeps
 * dark text on either theme. */
static void accent_tag(GtkTextView *view, const char *name)
{
    char rgb[16];
    g_snprintf(rgb, sizeof rgb, "#%06x", COCOSESSION_ACCENT_RGB);
    gtk_text_buffer_create_tag(gtk_text_view_get_buffer(view), name,
                               "paragraph-background", rgb, "foreground", "#000000", NULL);
}

static void set_rgb(cairo_t *context, guint32 rgb, double alpha)
{
    cairo_set_source_rgba(context, ((rgb >> 16) & 0xFF) / 255.0, ((rgb >> 8) & 0xFF) / 255.0,
                          (rgb & 0xFF) / 255.0, alpha);
}

/* The theme's text colour, for lines and labels drawn by hand. */
static void set_fg(cairo_t *context, GtkWidget *widget, double alpha)
{
    GdkRGBA colour;
    gtk_widget_get_color(widget, &colour);
    cairo_set_source_rgba(context, colour.red, colour.green, colour.blue, colour.alpha * alpha);
}

/* The accent as a line colour: the tan on a dark theme, the darker tan on
 * a light one (the tan alone is too pale to see against white). */
static guint32 accent_line_rgb(void)
{
    return adw_style_manager_get_dark(adw_style_manager_get_default())
        ? COCOSESSION_ACCENT_RGB : COCOSESSION_ACCENT_DARK_RGB;
}

/* Text centred in a cell, in the widget's font (monospace when asked). */
static void draw_centred(cairo_t *context, GtkWidget *widget, const char *text, double position_x, double position_y,
                         double cell_width, double cell_height, gboolean mono)
{
    PangoLayout *layout = gtk_widget_create_pango_layout(widget, text);
    int text_width, text_height;
    if (mono) {
        PangoFontDescription *font = pango_font_description_from_string("Monospace 10");
        pango_layout_set_font_description(layout, font);
        pango_font_description_free(font);
    }
    pango_layout_get_pixel_size(layout, &text_width, &text_height);
    cairo_move_to(context, position_x + (cell_width - text_width) / 2.0, position_y + (cell_height - text_height) / 2.0);
    pango_cairo_show_layout(context, layout);
    g_object_unref(layout);
}

/* Messages from Load Symbols and Save land in the console's transcript and,
 * since the console may not be the tab showing, in a toast. */
static void notice(DbgWin *state, const char *msg)
{
    char line[700];
    g_snprintf(line, sizeof line, "%s\n", msg);
    append_text(state->console_out, line, FALSE);
    adw_toast_overlay_add_toast(state->toasts, adw_toast_new(msg));
}

static void toast(DbgWin *state, const char *msg)
{
    adw_toast_overlay_add_toast(state->toasts, adw_toast_new(msg));
}

/* The step controls wait a moment for the engine to come to rest, so a
 * step does not flash "Running" before the status catches up. */
static void settle(DbgWin *state, gboolean want_stopped)
{
    int index;
    for (index = 0; index < 20 && (cocodebug_is_paused(state->dbg) != 0) != want_stopped; index++)
        g_usleep(2000);
}

/* ---- refresh -------------------------------------------------------------- */

static void refresh_cpu(DbgWin *state)
{
    cocodebug_regs regs;
    cocodebug_io chips;
    char buf[300];
    int index;
    const gboolean stopped = cocodebug_regs_get(state->dbg, &regs) != 0;

    if (stopped) {
        const unsigned vals[NREGS] = {
            regs.pc, regs.a, regs.b, (unsigned)(regs.a << 8 | regs.b), regs.dp, regs.x, regs.y, regs.u, regs.s, regs.cc,
            regs.e, regs.f, regs.w, regs.v, regs.md };
        for (index = 0; index < NREGS; index++) {
            g_snprintf(buf, sizeof buf, "%0*X", k_regs[index].digits, vals[index] & ((1u << (4 * k_regs[index].digits)) - 1));
            set_entry(state, state->reg[index], buf);
        }
        gtk_widget_set_visible(state->extra_box, regs.is6309 != 0);
        state->flag_sync = TRUE;
        for (index = 0; index < 8; index++)
            gtk_check_button_set_active(state->flag[index], (regs.cc & k_flags[index].mask) != 0);
        state->flag_sync = FALSE;
        if (cocodebug_io_get(state->dbg, &chips)) {
            g_snprintf(buf, sizeof buf, "%s, %s", chips.is_coco3 ? "CoCo 3 (GIME)" : "CoCo 1/2 (VDG, SAM)",
                       chips.is6309 ? "HD6309" : "MC6809");
            gtk_label_set_text(state->machine, buf);
        }
    } else {
        for (index = 0; index < NREGS; index++)
            set_entry(state, state->reg[index], "");
    }
    for (index = 0; index < NREGS; index++)
        gtk_widget_set_sensitive(GTK_WIDGET(state->reg[index]), stopped);
    for (index = 0; index < 8; index++)
        gtk_widget_set_sensitive(GTK_WIDGET(state->flag[index]), stopped);
}

/* A tab that reads the machine says so while it runs. */
static gboolean running_note(DbgWin *state, GtkTextView *view, const char *what)
{
    char text[200];
    if (cocodebug_is_paused(state->dbg)) return FALSE;
    g_snprintf(text, sizeof text, "The machine is running: stop it (F5) to read %s.", what);
    set_text(view, text);
    return TRUE;
}

static int mem_space_now(DbgWin *state)
{
    return gtk_drop_down_get_selected(state->mem_space) == 1 ? COCODEBUG_SPACE_PHYS
                                                         : COCODEBUG_SPACE_CPU;
}

/* PHYS is offered only on a CoCo 3 (the engine gives it no size otherwise). */
static void sync_space_choice(DbgWin *state)
{
    const gboolean phys = cocodebug_space_size(state->dbg, COCODEBUG_SPACE_PHYS) > 0;
    gtk_widget_set_visible(state->mem_space_box, phys);
    gtk_widget_set_visible(GTK_WIDGET(state->bp_phys), phys);
    gtk_widget_set_visible(GTK_WIDGET(state->wt_space), phys);
    if (!phys) {
        gtk_drop_down_set_selected(state->mem_space, 0);
        gtk_drop_down_set_selected(state->wt_space, 0);
        gtk_check_button_set_active(state->bp_phys, FALSE);
    }
}

/* The dump: a row is the address, then 3 columns per byte, then the text. The
 * highlighted range gets the accent background on its bytes (hex and text),
 * with the gap between two highlighted bytes filled so it reads as a bar. */
static void refresh_mem(DbgWin *state)
{
    uint8_t data[MEM_PAGE];
    cocodebug_region map[64];
    char region[64];
    GString *str;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(state->mem_view);
    int regions, row, col, count, space, digits, hexoff;
    uint32_t base, size;

    if (running_note(state, state->mem_view, "memory")) return;
    sync_space_choice(state);
    space = mem_space_now(state);
    size = cocodebug_space_size(state->dbg, space);
    digits = space == COCODEBUG_SPACE_PHYS ? 6 : 4;
    hexoff = digits + 2;
    if (state->mem_base[space] > size) state->mem_base[space] = 0;
    base = state->mem_base[space];
    regions = space == COCODEBUG_SPACE_CPU ? cocodebug_memory_map(state->dbg, map, 64) : 0;
    memset(data, 0, sizeof data);
    count = cocodebug_mem_read(state->dbg, space, base, data, MEM_PAGE);
    str = g_string_new(NULL);
    g_string_append_printf(str, "%*s0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n", hexoff, "");
    for (row = 0; row < MEM_ROWS; row++) {
        g_string_append_printf(str, "%0*X  ", digits, base + row * 16);
        for (col = 0; col < 16; col++) {
            if (row * 16 + col < count) g_string_append_printf(str, "%02X ", data[row * 16 + col]);
            else g_string_append(str, "-- ");
        }
        g_string_append_c(str, ' ');
        for (col = 0; col < 16; col++) {
            uint8_t cell_height = data[row * 16 + col];
            g_string_append_c(str, (row * 16 + col < count && cell_height >= 0x20 && cell_height <= 0x7E) ? (char)cell_height : '.');
        }
        /* the hardware answering at the row's first byte, from one map fetch */
        if (space == COCODEBUG_SPACE_CPU) {
            cocomem_region_name(map, regions, (uint16_t)(base + row * 16), region, sizeof region);
            if (region[0]) g_string_append_printf(str, "  %s", region);
        }
        g_string_append_c(str, '\n');
    }
    set_text(state->mem_view, str->str);
    g_string_free(str, TRUE);

    {   /* unchanged text keeps its tags: clear the old highlight first */
        GtkTextIter iter_start, iter_end;
        gtk_text_buffer_get_bounds(buffer, &iter_start, &iter_end);
        gtk_text_buffer_remove_tag_by_name(buffer, "mark", &iter_start, &iter_end);
    }
    if (state->mem_hl && state->mem_hl_space == space) {
        uint32_t address;
        for (address = base; address < base + MEM_PAGE; address++) {
            GtkTextIter iter_start, iter_end;
            int line, off;
            gboolean next;
            if (address < state->mem_hl_lo || address > state->mem_hl_hi) continue;
            line = (int)(address - base) / 16 + 1;
            col = (int)(address - base) % 16;
            next = address < state->mem_hl_hi && col < 15;
            gtk_text_buffer_get_iter_at_line_offset(buffer, &iter_start, line, hexoff + col * 3);
            gtk_text_buffer_get_iter_at_line_offset(buffer, &iter_end, line, hexoff + col * 3 + (next ? 3 : 2));
            gtk_text_buffer_apply_tag_by_name(buffer, "mark", &iter_start, &iter_end);
            off = hexoff + 48 + 1 + col;
            gtk_text_buffer_get_iter_at_line_offset(buffer, &iter_start, line, off);
            gtk_text_buffer_get_iter_at_line_offset(buffer, &iter_end, line, off + 1);
            gtk_text_buffer_apply_tag_by_name(buffer, "mark", &iter_start, &iter_end);
        }
    }
}

static void refresh_disasm(DbgWin *state)
{
    static cocodebug_line lines[DISASM_WINDOW];
    int dim_from[DISASM_WINDOW * 2];    /* per buffer line: grey from this column, -1 none */
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(state->disasm);
    GString *str;
    GtkTextIter iter_start, iter_end;
    int count, pc_line, index, line = 0, sel_line = -1, pc_buf = -1;
    gsize head;
    cocodebug_regs regs;

    if (!cocodebug_regs_get(state->dbg, &regs)) {
        set_text(state->disasm, "The machine is running: stop it (F5) to read the program.");
        state->line_count = 0;
        return;
    }
    str = g_string_new(NULL);
    /* Following, the PC sits a third of the way down the window. */
    if (gtk_check_button_get_active(state->follow_pc)) {
        state->disasm_top = cocodebug_row_address(state->dbg, regs.pc, -(DISASM_WINDOW / 3));
        if (state->disasm_top > regs.pc) state->disasm_top = 0;   /* near $0000 it wraps: start at the bottom */
    }

    count = cocodebug_disassemble_ex(state->dbg, (uint16_t)state->disasm_top, lines, DISASM_WINDOW, &pc_line);
    for (index = 0; index < count; index++) {
        /* a label gets a line of its own above its instruction */
        if (lines[index].label[0]) {
            g_string_append_printf(str, "      %s:\n", lines[index].label);
            dim_from[line] = 0;
            state->line_addr[line++] = -1;
        }
        head = str->len;
        /* a filled dot for an enabled breakpoint, a hollow one for a disabled one */
        g_string_append_printf(str, "%s%c %04X  %-11s %-18s",
                               lines[index].has_breakpoint ? "\xe2\x97\x8f"
                                   : lines[index].bp_disabled ? "\xe2\x97\x8b" : " ",
                               lines[index].is_pc ? '>' : ' ',
                               lines[index].address, lines[index].bytes, lines[index].disasm);
        dim_from[line] = -1;
        if (lines[index].comment[0]) {
            dim_from[line] = (int)g_utf8_strlen(str->str + head, -1);
            g_string_append_printf(str, "; %s", lines[index].comment);
        }
        g_string_append_c(str, '\n');
        if (lines[index].address == state->selected) sel_line = line;
        if (index == pc_line) pc_buf = line;
        state->line_addr[line++] = lines[index].address;
    }
    state->line_count = line;
    if (count == 0) g_string_append(str, "(no disassembly)\n");
    gtk_text_buffer_set_text(buffer, str->str, -1);
    g_string_free(str, TRUE);

    /* labels and comments in grey */
    for (index = 0; index < line; index++) {
        if (dim_from[index] < 0) continue;
        gtk_text_buffer_get_iter_at_line(buffer, &iter_start, index);
        if (dim_from[index] > 0) gtk_text_iter_set_line_offset(&iter_start, dim_from[index]);
        iter_end = iter_start;
        gtk_text_iter_forward_to_line_end(&iter_end);
        gtk_text_buffer_apply_tag_by_name(buffer, "dim", &iter_start, &iter_end);
    }
    if (sel_line >= 0) {
        gtk_text_buffer_get_iter_at_line(buffer, &iter_start, sel_line);
        gtk_text_buffer_get_iter_at_line(buffer, &iter_end, sel_line + 1);
        gtk_text_buffer_apply_tag_by_name(buffer, "sel", &iter_start, &iter_end);
    }
    /* the PC line in the accent colour (over the selection) */
    if (pc_buf >= 0) {
        gtk_text_buffer_get_iter_at_line(buffer, &iter_start, pc_buf);
        gtk_text_buffer_get_iter_at_line(buffer, &iter_end, pc_buf + 1);
        gtk_text_buffer_apply_tag_by_name(buffer, "pc", &iter_start, &iter_end);
    }
    /* the gutter marker: red while the breakpoint is enabled, grey off (it
     * keeps its colour on the PC line) */
    for (index = 0, line = 0; index < count; index++) {
        if (lines[index].label[0]) line++;
        if (lines[index].has_breakpoint || lines[index].bp_disabled) {
            gtk_text_buffer_get_iter_at_line(buffer, &iter_start, line);
            iter_end = iter_start;
            gtk_text_iter_forward_char(&iter_end);
            gtk_text_buffer_apply_tag_by_name(buffer, lines[index].has_breakpoint ? "bp_on" : "bp_off",
                                              &iter_start, &iter_end);
        }
        line++;
    }
}

static void refresh_video(DbgWin *state)
{
    dbg_video_refresh(state->pages[TAB_VIDEO], FALSE);
}

/* ---- C source -------------------------------------------------------------- */

static void refresh_locals(DbgWin *state)
{
    cocodebug_local locals[32];
    int count, index;
    GString *text;

    if (!cocodebug_source_available(state->dbg)) {
        gtk_label_set_text(state->src_locals, "");
        return;
    }
    if (!cocodebug_is_paused(state->dbg)) {
        gtk_label_set_text(state->src_locals, "(running)");
        return;
    }
    count = cocodebug_locals_get(state->dbg, locals, 32);
    if (count == 0) {
        /* Not necessarily an error: PC may just be before the function's
         * first statement (mid-prologue), where there is nothing correct to
         * show yet. */
        gtk_label_set_text(state->src_locals, "(no locals in scope)");
        return;
    }
    text = g_string_new(NULL);
    for (index = 0; index < count; index++) {
        const cocodebug_local *local = &locals[index];
        g_string_append_printf(text, "%-12s %-10s %s= ", local->name, local->type,
                               local->is_param ? "(param) " : "");
        if (local->has_value)
            g_string_append_printf(text, "%u ($%X)\n", local->value, local->value);
        else
            g_string_append(text, "?\n");
    }
    if (text->len > 0 && text->str[text->len - 1] == '\n')
        g_string_truncate(text, text->len - 1);
    gtk_label_set_text(state->src_locals, text->str);
    g_string_free(text, TRUE);
}

static void refresh_now(DbgWin *state);

/* The listing is rebuilt only when what it shows changed; this makes it. */
static void src_invalidate(DbgWin *state)
{
    if (state->src_sig) g_string_free(state->src_sig, TRUE);
    state->src_sig = NULL;
}

static void on_src_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)box;
    cocodebug_source_breakpoint_toggle(state->dbg, state->src_displayed_file_id,
                                       GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "coco-line")));
    src_invalidate(state);
    refresh_now(state);
}

static gboolean current_tab_is(DbgWin *state, int tab);

/* Brings the PC line into view once the listing is laid out. */
static gboolean src_scroll_to_pc(gpointer user_data)
{
    DbgWin *state = user_data;
    GtkListBoxRow *row;
    GtkWidget *scroll;
    graphene_rect_t bounds;
    GtkAdjustment *adj;
    state->src_scroll_id = 0;
    if (state->closing || state->pc_line < 1) return G_SOURCE_REMOVE;
    row = gtk_list_box_get_row_at_index(state->src_list, state->pc_line - 1);
    scroll = gtk_widget_get_ancestor(GTK_WIDGET(state->src_list), GTK_TYPE_SCROLLED_WINDOW);
    if (!row || !scroll) return G_SOURCE_REMOVE;
    adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
    if (gtk_widget_compute_bounds(GTK_WIDGET(row), GTK_WIDGET(state->src_list), &bounds))
        gtk_adjustment_set_value(adj, bounds.origin.y - gtk_adjustment_get_page_size(adj) / 3.0);
    return G_SOURCE_REMOVE;
}

/* Loads (and caches) file_id's text, then redraws the list with a bar on the
 * line PC is at (if PC's file is the one showing) and a red dot on any line
 * carrying a breakpoint. The list is rebuilt only when its signature moves. */
static void populate_src_list(DbgWin *state, int file_id)
{
    const char *path = cocodebug_source_file_path(state->dbg, file_id);
    GString *sig = g_string_new(NULL);
    int index;

    state->src_displayed_file_id = file_id;
    if (g_strcmp0(path, state->src_cached_path) != 0) {
        g_autofree char *contents = NULL;
        gsize len = 0;

        g_clear_pointer(&state->src_cached_lines, g_strfreev);
        g_free(state->src_cached_path);
        state->src_cached_path = g_strdup(path);
        if (path && g_file_get_contents(path, &contents, &len, NULL))
            state->src_cached_lines = g_strsplit(contents, "\n", -1);
        else
            state->src_cached_lines = g_new0(char *, 1);   /* empty, NUL-terminated */
        src_invalidate(state);
    }
    g_string_append_printf(sig, "%d/%s/%d:", file_id, path ? path : "", state->pc_file_id == file_id ? state->pc_line : -1);
    for (index = 0; state->src_cached_lines[index]; index++)
        if (cocodebug_source_breakpoint_is_set(state->dbg, file_id, index + 1))
            g_string_append_printf(sig, "%d,", index + 1);
    if (state->src_sig && g_string_equal(sig, state->src_sig)) {
        g_string_free(sig, TRUE);
        return;
    }
    if (state->src_sig) g_string_free(state->src_sig, TRUE);
    state->src_sig = sig;

    gtk_list_box_remove_all(state->src_list);
    for (index = 0; state->src_cached_lines[index]; index++) {
        const int lineno = index + 1;
        const gboolean is_pc = file_id == state->pc_file_id && lineno == state->pc_line;
        const gboolean has_bp = cocodebug_source_breakpoint_is_set(state->dbg, file_id, lineno);
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *label = gtk_label_new(NULL);
        g_autofree char *esc = g_markup_escape_text(state->src_cached_lines[index], -1);
        g_autofree char *markup = g_strdup_printf("%s %s %5d  %s",
            has_bp ? "<span foreground=\"#e01b24\">\xe2\x97\x8f</span>" : " ",
            is_pc ? ">" : " ", lineno, esc);

        gtk_label_set_markup(GTK_LABEL(label), markup);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_widget_add_css_class(label, "monospace");
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
        if (is_pc) gtk_widget_add_css_class(row, "coco-accent");
        g_object_set_data(G_OBJECT(row), "coco-line", GINT_TO_POINTER(lineno));
        gtk_list_box_append(state->src_list, row);
    }
    if (state->src_scroll && state->pc_file_id == file_id && state->pc_line > 0) {
        state->src_scroll = FALSE;
        if (state->src_scroll_id) g_source_remove(state->src_scroll_id);
        state->src_scroll_id = g_timeout_add(80, src_scroll_to_pc, state);
    }
}

/* The file picker from the engine's table: selection index and file id are the
 * same number (the engine hands the ids out 0..count-1). */
static void rebuild_src_combo(DbgWin *state)
{
    GtkStringList *model = gtk_string_list_new(NULL);
    const int count = cocodebug_source_file_count(state->dbg);
    int index;

    for (index = 0; index < count; index++) {
        const char *path = cocodebug_source_file_path(state->dbg, index);
        const char *base = path ? strrchr(path, '/') : NULL;
        gtk_string_list_append(model, base ? base + 1 : (path ? path : ""));
    }
    gtk_drop_down_set_model(state->src_file_combo, G_LIST_MODEL(model));
    g_object_unref(model);
    gtk_widget_set_sensitive(GTK_WIDGET(state->src_file_combo), count > 0);
    src_invalidate(state);
    if (count > 0) {
        state->src_displayed_file_id = 0;
        populate_src_list(state, 0);
    } else {
        state->src_displayed_file_id = -1;
        gtk_list_box_remove_all(state->src_list);
    }
}

static void refresh_source(DbgWin *state)
{
    cocodebug_stop stop;
    int fid, line;
    GListModel *model = gtk_drop_down_get_model(state->src_file_combo);
    const int files = cocodebug_source_file_count(state->dbg);

    if (!cocodebug_source_available(state->dbg) || files <= 0) {
        if (model && g_list_model_get_n_items(model) > 0) rebuild_src_combo(state);
        gtk_label_set_text(state->src_note,
            "No C source loaded. Load C Source opens a CMOC listing (.lst from `cmoc -i`); "
            "a program built beside its .c and .map is found on its own.");
        gtk_label_set_text(state->src_locals, "");
        return;
    }
    gtk_label_set_text(state->src_note, "");
    /* the engine may have found a listing of its own since the picker was built */
    if (!model || (int)g_list_model_get_n_items(model) != files) rebuild_src_combo(state);
    cocodebug_stop_snapshot(state->dbg, &stop);
    if (stop.paused && cocodebug_source_line_at(state->dbg, stop.pc, &fid, &line)) {
        state->pc_file_id = fid;
        state->pc_line = line;
        if ((guint)fid != gtk_drop_down_get_selected(state->src_file_combo))
            gtk_drop_down_set_selected(state->src_file_combo, (guint)fid);   /* repopulates */
        else
            populate_src_list(state, state->src_displayed_file_id);
    } else {
        /* running, or PC is not a mapped statement (mid-prologue, or code with
         * no line info): keep the file showing and drop the bar */
        state->pc_file_id = -1;
        state->pc_line = -1;
        populate_src_list(state, state->src_displayed_file_id >= 0 ? state->src_displayed_file_id : 0);
    }
    refresh_locals(state);
}

static void on_src_file_changed(GObject *obj, GParamSpec *pspec, gpointer user_data)
{
    DbgWin *state = user_data;
    guint idx;
    (void)pspec;
    if (state->closing) return;
    idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
    if (idx == GTK_INVALID_LIST_POSITION) return;
    populate_src_list(state, (int)idx);
}

/* ---- Cart & I/O ------------------------------------------------------------ */

static void append_pia(GString *text, const char *name, unsigned base, const cocodebug_pia *pia)
{
    g_string_append_printf(text,
        "  %s ($%04X)   CRA %02X  CRB %02X  DDRA %02X  DDRB %02X  ORA %02X  ORB %02X\n"
        "      pins A %02X  B %02X   CA1 %d CA2 %d CB1 %d CB2 %d   IRQ A %s B %s   flags A %d B %d\n",
        name, base, pia->cra, pia->crb, pia->ddra, pia->ddrb, pia->ora, pia->orb, pia->pina, pia->pinb,
        pia->ca1, pia->ca2, pia->cb1, pia->cb2, pia->irqa ? "on" : "-", pia->irqb ? "on" : "-",
        pia->irq_flags_a != 0, pia->irq_flags_b != 0);
}

/* The cartridge, then the memory map, then the chips. */
static void refresh_cart(DbgWin *state)
{
    cocodebug_io chips;
    cocodebug_region regions[64];
    char info[256];
    GString *text;
    int index, region_count;

    if (running_note(state, state->cart_text, "the cartridge and the chips")) return;
    text = g_string_new(NULL);
    cocodebug_cart_info(state->dbg, info, sizeof info);
    g_string_append_printf(text, "%s\n\nMemory map (as the %s maps it now)\n",
                           info, cocodebug_space_size(state->dbg, COCODEBUG_SPACE_PHYS) ? "GIME" : "SAM");
    region_count = cocodebug_memory_map(state->dbg, regions, 64);
    if (!region_count)
        g_string_append(text, "  (no map: no machine is loaded)\n");
    for (index = 0; index < region_count; index++) {
        char phys[32] = "";
        if (regions[index].physical_base >= 0)
            g_snprintf(phys, sizeof phys, "  phys %06X", regions[index].physical_base);
        g_string_append_printf(text, "  %04X-%04X  %s  %s%s\n", regions[index].lo, regions[index].hi,
                               regions[index].writable ? "RW" : "R-", regions[index].name, phys);
    }
    if (!cocodebug_io_get(state->dbg, &chips)) {
        g_string_append(text, "\n(no I/O state yet)\n");
    } else {
        g_string_append(text, "\nPIAs\n");
        append_pia(text, "PIA0", 0xFF00, &chips.pia0);
        append_pia(text, "PIA1", 0xFF20, &chips.pia1);
        if (chips.sam_valid)
            g_string_append_printf(text,
                "\nSAM\n  V %u  F %u  P %u  R %u  M %u  TY %u   display address $%04X\n",
                chips.sam_v, chips.sam_f, chips.sam_p, chips.sam_r, chips.sam_m, chips.sam_ty,
                chips.sam_video_address);
        if (chips.gime_valid) {
            g_string_append_printf(text, "\nGIME  (scanline %u, row %u)   IRQ %s  FIRQ %s\n  regs $FF90-$FF9F:",
                                   chips.gime_scanline, chips.gime_row, chips.gime_irq ? "on" : "-",
                                   chips.gime_firq ? "on" : "-");
            for (index = 0; index < 16; index++)
                g_string_append_printf(text, " %02X", chips.gime_reg[index]);
            g_string_append(text, "\n  MMU $FFA0-$FFAF:");
            for (index = 0; index < 16; index++)
                g_string_append_printf(text, "%s%02X", index == 8 ? "  |  task 1:" " " : " ", chips.gime_mmu[index]);
            g_string_append(text, "\n  palette $FFB0-$FFBF (RGB 6-bit):");
            for (index = 0; index < 16; index++)
                g_string_append_printf(text, " %02X", chips.gime_palette[index]);
            g_string_append_c(text, '\n');
        }
        g_string_append_printf(text,
            "\nJoysticks\n  right: x %5d  y %5d  buttons %d %d\n  left:  x %5d  y %5d  buttons %d %d\n",
            chips.joy_axis[0][0], chips.joy_axis[0][1], chips.joy_button[0][0], chips.joy_button[0][1],
            chips.joy_axis[1][0], chips.joy_axis[1][1], chips.joy_button[1][0], chips.joy_button[1][1]);
        g_string_append_printf(text, "\nBecker (DriveWire) link   %s   %d byte%s waiting for the CPU\n",
                               chips.becker_link_up ? "up" : "down", chips.becker_rx_pending,
                               chips.becker_rx_pending == 1 ? "" : "s");
    }
    set_text(state->cart_text, text->str);
    g_string_free(text, TRUE);
}

/* ---- Sound ------------------------------------------------------------------ */

#define WAVE_H 150

/* The DAC level (top lane, 0-63) and the single-bit line (bottom lane) over the
 * time the history covers, oldest on the left. */
static void draw_wave(GtkDrawingArea *area, cairo_t *context, int width, int height, gpointer user_data)
{
    DbgWin *state = user_data;
    GtkWidget *widget = GTK_WIDGET(area);
    const int count = state->sound_hist_n;
    double dac_top = 6, dac_bot = height - 40, bit_top = height - 28, bit_bot = height - 6;
    uint32_t first_tick, span;
    int index, bit = 0;
    double lastx = 0, lastdac = dac_bot, lastbit = bit_bot;
    gboolean have_dac = FALSE;

    set_fg(context, widget, 0.25);
    cairo_set_line_width(context, 1);
    cairo_rectangle(context, 0.5, 0.5, width - 1, height - 1);
    cairo_stroke(context);
    if (count < 2) {
        set_fg(context, widget, 0.6);
        draw_centred(context, widget, "no writes to the DAC yet", 0, 0, width, height, FALSE);
        return;
    }
    first_tick = state->sound_hist[count - 1].tick;
    span = state->sound_hist[0].tick - first_tick;
    if (!span) span = 1;
    set_rgb(context, accent_line_rgb(), 1.0);
    cairo_set_line_width(context, 1.5);
    for (index = count - 1; index >= 0; index--) {
        const cocodebug_sound_write *record = &state->sound_hist[index];
        double position_x = 2 + (double)(record->tick - first_tick) / span * (width - 4);
        if (record->port == 0) {
            double position_y = dac_bot - record->dac / 63.0 * (dac_bot - dac_top);
            if (!have_dac) { cairo_move_to(context, position_x, position_y); have_dac = TRUE; }
            else { cairo_line_to(context, position_x, lastdac); cairo_line_to(context, position_x, position_y); }
            lastdac = position_y;
        }
        lastx = position_x;
    }
    cairo_line_to(context, lastx, lastdac);
    cairo_stroke(context);
    set_fg(context, widget, 0.8);
    cairo_set_line_width(context, 1.5);
    cairo_move_to(context, 2, bit_bot);
    lastbit = bit_bot;
    for (index = count - 1; index >= 0; index--) {
        const cocodebug_sound_write *record = &state->sound_hist[index];
        double position_x = 2 + (double)(record->tick - first_tick) / span * (width - 4);
        if (record->port == 2) {
            double position_y;
            bit = (record->value >> 1) & 1;
            position_y = bit ? bit_top : bit_bot;
            cairo_line_to(context, position_x, lastbit);
            cairo_line_to(context, position_x, position_y);
            lastbit = position_y;
        }
    }
    cairo_line_to(context, width - 2, lastbit);
    cairo_stroke(context);
}

/* The 6-bit DAC, the single-bit line, the mux and the cassette lines. */
static void refresh_sound(DbgWin *state)
{
    cocodebug_sound sound;
    GString *text;
    int count;

    if (state->sound_hist) {
        count = cocodebug_sound_history(state->dbg, state->sound_hist, SOUND_SHOWN);
        if (count != state->sound_hist_n || count > 0) gtk_widget_queue_draw(state->sound_wave);
        state->sound_hist_n = count;
    }
    if (running_note(state, state->sound_text, "the sound lines")) return;
    if (!cocodebug_sound_get(state->dbg, &sound)) {
        set_text(state->sound_text, "(no sound state yet)");
        return;
    }
    text = g_string_new(NULL);
    g_string_append_printf(text,
        "Sound (the PIA1 DAC, the single-bit line and the analog mux)\n\n"
        "DAC (6-bit)     %2d of 63   ($FF20 bits 2-7)\n"
        "Single bit      %d          ($FF22 bit 1)\n"
        "Mux source      %-9s   (PIA0 CA2/CB2)\n"
        "Sound enable    %-3s         (PIA1 CB2: the mux reaches the speaker)\n"
        "Cassette motor  %-3s         (PIA1 CA2)\n"
        "Cassette in     %d          (PIA1 A bit 0)\n\n"
        "The waveform below is the newest %d writes to $FF20 (accent, top) and $FF22 (bottom),\n"
        "recorded while this tab shows.\n",
        sound.dac, sound.single_bit, sound.mux_name ? sound.mux_name : "?", sound.sound_enable ? "on" : "off",
        sound.motor ? "on" : "off", sound.cassette_in, state->sound_hist_n);
    set_text(state->sound_text, text->str);
    g_string_free(text, TRUE);
}

/* The engine records the DAC writes only while a Sound tab is in front. */
static void sync_sound_watch(DbgWin *state)
{
    const gboolean want = !state->closing && gtk_widget_get_mapped(GTK_WIDGET(state->win)) &&
                          (state->detached[TAB_SOUND] != NULL || current_tab_is(state, TAB_SOUND));
    if (want == state->sound_watching) return;
    state->sound_watching = want;
    cocodebug_sound_watch(state->dbg, want);
}

/* The packet log is rebuilt only when it changed: a new packet, or a
 * repeat (which moves the newest serial on). */
static void refresh_fujinet(DbgWin *dbg_state, gboolean force)
{
    static coco_fuji_packet packets[TRACE_MAX];
    static char state[2048];
    char note[200];
    GString *text;
    const gboolean hide = gtk_check_button_get_active(dbg_state->fuji_hide_ready);
    int count = cocodebug_fujinet_trace(dbg_state->dbg, packets, TRACE_MAX), index, shown = 0;

    if (!cocosession_fujinet_running(dbg_state->session) && !cocosession_fujinet_external(dbg_state->session))
        g_strlcpy(note, "FujiNet is off (Preferences \xe2\x96\xb8 FujiNet).", sizeof note);
    else
        g_snprintf(note, sizeof note, "%s FujiNet on the Becker port %s, %s.",
                   cocosession_fujinet_external(dbg_state->session) ? "External" : "Built-in",
                   cocosession_becker_endpoint(dbg_state->session),
                   cocosession_fujinet_link(dbg_state->session) ? "connected" : "not connected");
    gtk_label_set_text(dbg_state->fuji_note, note);
    cocodebug_fujinet_state(dbg_state->dbg, state, sizeof state);
    set_text(dbg_state->fuji_state, state);

    if (!force && count == dbg_state->fuji_count && (count == 0 || packets[count - 1].serial == dbg_state->fuji_last))
        return;
    dbg_state->fuji_count = count;
    dbg_state->fuji_last = count > 0 ? packets[count - 1].serial : 0;
    log_note_reader(dbg_state->fuji_text);
    text = g_string_new(NULL);
    for (index = 0; index < count; index++) {
        int slot;
        if (hide && cocodebug_fujinet_is_ready(&packets[index])) continue;
        g_string_append_printf(text, "%s%s", shown++ ? "\n" : "", packets[index].text);
        if (packets[index].repeat > 1) g_string_append_printf(text, " \xc3\x97%u", packets[index].repeat);
        g_string_append(text, "\n     ");
        for (slot = 0; slot < packets[index].nbytes && slot < (int)sizeof packets[index].data; slot++)
            g_string_append_printf(text, "%02X ", packets[index].data[slot]);
        if (packets[index].nbytes > (int)sizeof packets[index].data) g_string_append(text, "\xe2\x80\xa6");
    }
    if (shown == 0)
        g_string_append(text, count ? "(only READY probes since the trace was cleared)"
                             : "(no DriveWire traffic since the trace was cleared)");
    set_text_at(dbg_state->fuji_text, text->str, force || following(dbg_state->fuji_text));
    g_string_free(text, TRUE);
}

static void bp_enable_toggled(GtkCheckButton *button, gpointer user_data);
static void bp_remove_clicked(GtkButton *button, gpointer user_data);

static void refresh_bps(DbgWin *state)
{
    static cocodebug_breakpoint bps[COCODEBUG_MAX_BREAKPOINTS];
    GtkWidget *child;
    GString *sig = g_string_new(NULL);
    int count = cocodebug_breakpoints_list(state->dbg, bps, COCODEBUG_MAX_BREAKPOINTS), index;
    char line[256];
    cocodebug_stop stop;

    cocodebug_stop_snapshot(state->dbg, &stop);
    if (stop.paused) {
        if (!stop.reason[0] || strcmp(stop.reason, "stopped") == 0)
            g_strlcpy(line, "Stopped", sizeof line);
        else if (g_str_has_prefix(stop.reason, "stopped "))
            g_snprintf(line, sizeof line, "S%s", stop.reason + 1);
        else
            g_snprintf(line, sizeof line, "Stopped: %s", stop.reason);
        if (stop.reason_addr >= 0 && !strchr(stop.reason, '$'))
            g_snprintf(line + strlen(line), sizeof line - strlen(line), " at $%04X", stop.reason_addr);
    } else
        g_strlcpy(line, "Running.", sizeof line);
    gtk_label_set_text(state->bp_stop, line);

    /* the list is rebuilt only when what it shows changed (hit counts move
     * while the machine runs), so a click on a row is not pulled out from
     * under the pointer */
    for (index = 0; index < count; index++)
        g_string_append_printf(sig, "%d/%d/%u-%u/%d/%u/%s\n", bps[index].id, bps[index].type,
                               bps[index].start, bps[index].end, bps[index].enabled, bps[index].hits,
                               bps[index].condition);
    if (state->bp_sig && g_string_equal(sig, state->bp_sig)) {
        g_string_free(sig, TRUE);
        return;
    }
    if (state->bp_sig) g_string_free(state->bp_sig, TRUE);
    state->bp_sig = sig;

    while ((child = gtk_widget_get_first_child(GTK_WIDGET(state->bp_list))) != NULL)
        gtk_list_box_remove(state->bp_list, child);
    if (count == 0) {
        GtkWidget *label = gtk_label_new("No breakpoints. Select a disassembly line and press F9, "
                                     "or add one below.");
        gtk_widget_add_css_class(label, "dim-label");
        gtk_list_box_append(state->bp_list, label);
        return;
    }
    for (index = 0; index < count; index++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *enable_box = gtk_check_button_new();
        GtkWidget *remove_button = gtk_button_new_from_icon_name("user-trash-symbolic");
        GtkWidget *label;
        char text[360], desc[256];
        cocobp_describe(state->dbg, &bps[index], desc, sizeof desc);
        g_snprintf(text, sizeof text, "#%-3d %s    hits %u", bps[index].id, desc, bps[index].hits);
        label = gtk_label_new(text);
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_widget_set_hexpand(label, TRUE);
        gtk_widget_add_css_class(label, "monospace");
        gtk_check_button_set_active(GTK_CHECK_BUTTON(enable_box), bps[index].enabled != 0);
        gtk_widget_set_tooltip_text(enable_box, "Enabled");
        g_object_set_data(G_OBJECT(enable_box), "bp", GINT_TO_POINTER(bps[index].id));
        g_object_set_data(G_OBJECT(remove_button), "bp", GINT_TO_POINTER(bps[index].id));
        g_signal_connect(enable_box, "toggled", G_CALLBACK(bp_enable_toggled), state);
        g_signal_connect(remove_button, "clicked", G_CALLBACK(bp_remove_clicked), state);
        gtk_widget_add_css_class(remove_button, "flat");
        gtk_widget_set_tooltip_text(remove_button, "Remove");
        gtk_box_append(GTK_BOX(row), enable_box);
        gtk_box_append(GTK_BOX(row), label);
        gtk_box_append(GTK_BOX(row), remove_button);
        gtk_list_box_append(state->bp_list, row);
    }
}

/* ---- tabs, and tabs popped out into windows of their own -------------------- */

static void refresh_now(DbgWin *state);
static void show_in_memory(DbgWin *state, int space, uint32_t first, uint32_t last);

static int page_tab(GtkWidget *page)
{
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(page), "tab"));
}

/* The tab showing in the main window (-1 if the notebook is empty). */
static int current_tab(DbgWin *state)
{
    GtkWidget *page = gtk_notebook_get_nth_page(state->notebook,
                                                gtk_notebook_get_current_page(state->notebook));
    return page ? page_tab(page) : -1;
}

static gboolean current_tab_is(DbgWin *state, int tab)
{
    return current_tab(state) == tab;
}

/* In front of the user: the current page, or a tab popped out. */
static gboolean tab_visible(DbgWin *state, int tab)
{
    return state->detached[tab] != NULL || current_tab(state) == tab;
}

static void show_tab(DbgWin *state, int tab)
{
    if (state->detached[tab])
        gtk_window_present(state->detached[tab]);
    else
        gtk_notebook_set_current_page(state->notebook,
                                      gtk_notebook_page_num(state->notebook, state->pages[tab]));
}

/* ---- symbols ------------------------------------------------------------------ */

/* A row opens the disassembly at its address. */
static void on_sym_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)box;
    if (!row) return;
    gtk_check_button_set_active(state->follow_pc, FALSE);
    state->disasm_top = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "addr"));
    show_tab(state, TAB_DISASM);
    refresh_disasm(state);
}

/* The index order is unstable (loading, clearing and lookups reorder it), so
 * the list is fetched through the filter every time, each row caches its
 * address, and the list is rebuilt only when what it would show changed. */
static void refresh_symbols(DbgWin *state)
{
    static int indices[SYMBOLS_SHOWN + 1];
    GString *sig = g_string_new(NULL);
    const char *filter = gtk_editable_get_text(GTK_EDITABLE(state->sym_filter));
    const int total = cocodebug_symbols_count(state->dbg);
    const int matched = cocodebug_symbols_filter(state->dbg, filter, indices, SYMBOLS_SHOWN + 1);
    const int shown = matched > SYMBOLS_SHOWN ? SYMBOLS_SHOWN : matched;
    static uint16_t addresses[SYMBOLS_SHOWN];
    static char names[SYMBOLS_SHOWN][64];
    int index;
    char note[160];

    for (index = 0; index < shown; index++) {
        if (!cocodebug_symbols_get(state->dbg, indices[index], &addresses[index], names[index], sizeof names[index])) {
            addresses[index] = 0;
            names[index][0] = '\0';
        }
        g_string_append_printf(sig, "%04X/%s\n", addresses[index], names[index]);
    }
    g_string_append_printf(sig, "%d/%d", matched, total);
    if (state->sym_sig && g_string_equal(sig, state->sym_sig)) {
        g_string_free(sig, TRUE);
        return;
    }
    if (state->sym_sig) g_string_free(state->sym_sig, TRUE);
    state->sym_sig = sig;

    gtk_list_box_remove_all(state->sym_list);
    for (index = 0; index < shown; index++) {
        GtkWidget *row_label;
        GtkWidget *row = gtk_list_box_row_new();
        char text[160];
        g_snprintf(text, sizeof text, "$%04X  %s", addresses[index], names[index]);
        row_label = gtk_label_new(text);
        gtk_label_set_xalign(GTK_LABEL(row_label), 0);
        gtk_widget_add_css_class(row_label, "monospace");
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), row_label);
        g_object_set_data(G_OBJECT(row), "addr", GINT_TO_POINTER(addresses[index]));
        gtk_list_box_append(state->sym_list, row);
    }
    if (matched > SYMBOLS_SHOWN)
        g_snprintf(note, sizeof note, "%d of %d symbols match; the first %d are shown "
                   "(narrow the filter)", matched, total, SYMBOLS_SHOWN);
    else
        g_snprintf(note, sizeof note, "%d of %d symbol%s", matched, total, total == 1 ? "" : "s");
    gtk_label_set_text(state->sym_note, note);
}

/* a new filter, typed or cleared: show it now */
static void on_sym_filter_changed(GtkEditable *editable, gpointer user_data)
{
    (void)editable;
    refresh_symbols(user_data);
}

/* ---- watches ------------------------------------------------------------------ */

static void on_wt_remove(GtkButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    cocowatch_remove(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "wt-idx")));
    refresh_now(state);
}

static void on_wt_memory(GtkButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    const int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "wt-idx"));
    uint32_t first, last;
    if (!cocowatch_range(state->dbg, idx, &first, &last)) {
        toast(state, "That watch does not resolve to an address yet");
        return;
    }
    show_in_memory(state, cocowatch_space(idx), first, last);
    show_tab(state, TAB_MEMORY);
}

/* Break when the watched bytes change: a write whose value differs from the
 * byte it replaced. */
static void break_on_change(DbgWin *state, int space, uint32_t first, uint32_t last)
{
    const int type = COCODEBUG_BP_WRITE | (space == COCODEBUG_SPACE_PHYS ? COCODEBUG_BP_PHYS : 0);
    if (cocodebug_breakpoint_add(state->dbg, type, first, last, "value != old") < 0) {
        toast(state, "The breakpoint was refused");
        return;
    }
    toast(state, "Breaking when it changes");
}

static void on_wt_break(GtkButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    const int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "wt-idx"));
    uint32_t first, last;
    if (!cocowatch_range(state->dbg, idx, &first, &last)) {
        toast(state, "That watch does not resolve to an address yet");
        return;
    }
    break_on_change(state, cocowatch_space(idx), first, last);
    refresh_now(state);
}

static void wt_rebuild(DbgWin *state)
{
    const int count = cocowatch_count();
    int index;

    gtk_list_box_remove_all(state->wt_list);
    g_ptr_array_set_size(state->wt_labels, 0);
    if (count == 0) {
        GtkWidget *placeholder = gtk_label_new("No watches. Type an address, a symbol, symbol+3, [ptr] "
                                     "or a range (0400-05FF) above.");
        gtk_widget_add_css_class(placeholder, "dim-label");
        gtk_list_box_append(state->wt_list, placeholder);
    }
    for (index = 0; index < count; index++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *label = gtk_label_new(NULL);
        static const struct { const char *text, *tip; GCallback callback; } btns[] = {
            { "Memory", "Show it in the Memory tab", G_CALLBACK(on_wt_memory) },
            { "Break on change", "Stop when a write changes it", G_CALLBACK(on_wt_break) },
            { "Remove", "Forget this watch", G_CALLBACK(on_wt_remove) },
        };
        size_t slot;
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_widget_set_hexpand(label, TRUE);
        gtk_widget_add_css_class(label, "monospace");
        gtk_box_append(GTK_BOX(row), label);
        for (slot = 0; slot < G_N_ELEMENTS(btns); slot++) {
            GtkWidget *button = gtk_button_new_with_label(btns[slot].text);
            gtk_widget_add_css_class(button, "flat");
            gtk_widget_set_tooltip_text(button, btns[slot].tip);
            g_object_set_data(G_OBJECT(button), "wt-idx", GINT_TO_POINTER(index));
            g_signal_connect(button, "clicked", btns[slot].callback, state);
            gtk_box_append(GTK_BOX(row), button);
        }
        gtk_list_box_append(state->wt_list, row);
        g_ptr_array_add(state->wt_labels, label);
    }
}

/* The rows are rebuilt only when the list changed; the values are written
 * into the labels in place, so a click on a row's button is never pulled
 * from under the pointer. */
static void refresh_watches(DbgWin *state)
{
    const unsigned gen = cocowatch_generation();
    int index;
    if (!state->wt_built || gen != state->wt_gen || cocowatch_count() != (int)state->wt_labels->len) {
        wt_rebuild(state);
        state->wt_gen = gen;
        state->wt_built = TRUE;
    }
    for (index = 0; index < (int)state->wt_labels->len; index++) {
        char line[200];
        cocowatch_format(state->dbg, index, line, sizeof line);
        gtk_label_set_text(g_ptr_array_index(state->wt_labels, index), line);
    }
}

static void on_wt_add(GtkWidget *wgt, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)wgt;
    if (cocowatch_add(gtk_editable_get_text(GTK_EDITABLE(state->wt_entry)),
                      gtk_drop_down_get_selected(state->wt_space) == 1 ? COCODEBUG_SPACE_PHYS
                                                                  : COCODEBUG_SPACE_CPU) < 0) {
        toast(state, "Type an address, a symbol, an expression or a range");
        return;
    }
    gtk_editable_set_text(GTK_EDITABLE(state->wt_entry), "");
    refresh_now(state);
}

/* ---- trace and history -------------------------------------------------------- */

/* Both tabs read the one ring. Trace (0) follows it while the machine runs;
 * History (1) is read once the machine has stopped. The newest HIST_SHOWN
 * matches are shown, oldest first. */
static void refresh_hist(DbgWin *state, int idx, gboolean force)
{
    static cocodebug_trace_entry buf[HIST_READ];
    static int hit[HIST_SHOWN];
    const gboolean recording = cocodebug_trace_enabled(state->dbg) != 0;
    const gboolean stopped = cocodebug_is_paused(state->dbg) != 0;
    const int count = cocodebug_trace_count(state->dbg);
    uint32_t first = 0, last = 0xFFFF;
    int filter = COCODEBUG_TRACE_ALL, entry_count, index, shown = 0, matched = 0;
    uint64_t sig;
    cocodebug_trace_entry newest;
    char note[200];
    GString *text;

    state->h_sync = TRUE;       /* the Record toggles follow the engine */
    gtk_toggle_button_set_active(state->h_rec[0], recording);
    gtk_toggle_button_set_active(state->h_rec[1], recording);
    state->h_sync = FALSE;

    if (idx == 1 && !stopped) {
        gtk_label_set_text(state->h_note[1], "Stop the machine to read its history.");
        state->h_sig[1] = ~(uint64_t)0;
        return;
    }
    memset(&newest, 0, sizeof newest);
    cocodebug_trace_read(state->dbg, &newest, 1);
    sig = ((uint64_t)count << 40) ^ newest.cycles ^ (recording ? 1 : 0);
    if (!force && sig == state->h_sig[idx]) return;

    switch (gtk_drop_down_get_selected(state->h_filter[idx])) {
    case 1: filter = COCODEBUG_TRACE_BRANCHES; break;
    case 2:
        if (!cocosym_parse_range(state->dbg, COCODEBUG_SPACE_CPU,
                                 gtk_editable_get_text(GTK_EDITABLE(state->h_range[idx])), &first, &last)) {
            gtk_label_set_text(state->h_note[idx], "Type a range: 0400-04FF, a symbol, symbol..symbol");
            return;
        }
        filter = COCODEBUG_TRACE_RANGE;
        break;
    default: break;
    }
    state->h_sig[idx] = sig;
    entry_count = cocodebug_trace_read(state->dbg, buf, filter == COCODEBUG_TRACE_ALL ? HIST_SHOWN : HIST_READ);
    for (index = 0; index < entry_count; index++)             /* newest first: take the newest matches */
        if (cocodebug_trace_match(&buf[index], filter, (uint16_t)first, (uint16_t)last)) {
            matched++;
            if (shown < HIST_SHOWN) hit[shown++] = index;
        }
    text = g_string_new(NULL);
    if (shown == 0)
        g_string_append(text, recording
                                ? (count > 0 ? "(no recorded instruction matches this filter)"
                                             : "(nothing recorded yet: let the machine run)")
                              : "Recording is off. Press Record, then let the machine run.");
    for (index = shown - 1; index >= 0; index--) {  /* oldest first */
        char line[256];
        cocodebug_trace_format(&buf[hit[index]], line, sizeof line);
        g_string_append(text, line);
        g_string_append_c(text, '\n');
    }
    set_text_at(state->h_text[idx], text->str, TRUE);
    g_string_free(text, TRUE);
    g_snprintf(note, sizeof note, "%s, %d instruction%s%s", recording ? "recording" : "recording off",
               count, count == 1 ? "" : "s",
               matched > shown ? ", newest 2000 shown" : "");
    gtk_label_set_text(state->h_note[idx], note);
}

static void refresh_status(DbgWin *state)
{
    cocodebug_stop stop;
    char text[256];
    cocodebug_stop_snapshot(state->dbg, &stop);
    if (stop.paused) {
        const gboolean plain = !stop.reason[0] || strcmp(stop.reason, "stopped") == 0;
        const gboolean restated = g_str_has_prefix(stop.reason, "stopped ");   /* "stopped at $0412" */
        if (restated)
            g_snprintf(text, sizeof text, "S%s", stop.reason + 1);
        else
            g_snprintf(text, sizeof text, "Stopped%s%s", plain ? "" : ": ", plain ? "" : stop.reason);
        if (stop.reason_addr >= 0 && !strchr(stop.reason, '$'))
            g_snprintf(text + strlen(text), sizeof text - strlen(text), " at $%04X", stop.reason_addr);
    } else
        g_snprintf(text, sizeof text, "Running");
    adw_window_title_set_subtitle(state->title, text);
    gtk_button_set_label(state->run_btn, stop.paused ? "Run (F5)" : "Pause (F5)");
    if (stop.paused) gtk_widget_add_css_class(GTK_WIDGET(state->run_btn), "coco-accent");
    else gtk_widget_remove_css_class(GTK_WIDGET(state->run_btn), "coco-accent");
    {   /* C-line stepping needs a listing */
        const gboolean src = cocodebug_source_available(state->dbg) != 0;
        gtk_widget_set_sensitive(state->step_line_btn, src);
        gtk_widget_set_sensitive(state->step_over_line_btn, src);
    }
}

/* Only the tabs in front of the user are brought up to date (the one showing,
 * and any popped out); the others catch up when their tab is switched to. */
static void refresh_tab(DbgWin *state, int tab)
{
    if (state->closing) return;
    switch (tab) {
    case TAB_DISASM: refresh_disasm(state); break;
    case TAB_CSRC: refresh_source(state); break;
    case TAB_MEMORY: refresh_mem(state); break;
    case TAB_BREAKS: refresh_bps(state); break;
    case TAB_WATCHES: refresh_watches(state); break;
    case TAB_CART: refresh_cart(state); break;
    case TAB_SOUND: refresh_sound(state); break;
    case TAB_SYMBOLS: refresh_symbols(state); break;
    case TAB_TRACE: refresh_hist(state, 0, FALSE); break;
    case TAB_HISTORY: refresh_hist(state, 1, FALSE); break;
    case TAB_VIDEO: refresh_video(state); break;
    case TAB_FUJINET: refresh_fujinet(state, FALSE); break;
    default: break;
    }
}

/* The status, the registers and the tabs in front, and the generation they
 * now reflect. */
static void refresh_now(DbgWin *state)
{
    int tab;
    if (state->closing) return;
    state->seen_generation = cocodebug_generation(state->dbg);
    state->was_stopped = cocodebug_is_paused(state->dbg) != 0;
    refresh_status(state);
    refresh_cpu(state);
    for (tab = 0; tab < NTABS; tab++)
        if (tab_visible(state, tab)) refresh_tab(state, tab);
    sync_sound_watch(state);
}

static void attach_and_refresh(DbgWin *state);

static void install_css(void)
{
    static gboolean done;
    GtkCssProvider *css;
    char buf[256];
    if (done) return;
    done = TRUE;
    css = gtk_css_provider_new();
    /* dark ink on the green (the shared .coco-accent comes from window.c) */
    g_snprintf(buf, sizeof buf,
        "button.coco-toggle:checked, togglebutton.coco-toggle:checked {"
        " background-image: none; background-color: #%06x; color: #%06x; }",
        COCOSESSION_ACCENT_RGB, COCOSESSION_ACCENT_INK_RGB);
    gtk_css_provider_load_from_string(css, buf);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

/* A new stop brings every view to the current spot: the disassembly to the PC
 * (done by the caller), the dump back to its top row, and the trace and
 * history to their newest line (they redraw and follow their end). */
static void recentre_views(DbgWin *state)
{
    GtkAdjustment *adj = vadj(state->mem_view);
    int index;
    if (adj) gtk_adjustment_set_value(adj, 0);
    for (index = 0; index < 2; index++) {
        state->h_sig[index] = ~(uint64_t)0;
        g_object_set_data(G_OBJECT(state->h_text[index]), "follow", GINT_TO_POINTER(1));
    }
    state->src_scroll = TRUE;           /* and the C listing to the PC's line */
    src_invalidate(state);
    refresh_now(state);
}

static gboolean tick_refresh(gpointer user_data)
{
    DbgWin *state = user_data;
    unsigned gen;
    gboolean stopped;
    if (!gtk_widget_get_visible(GTK_WIDGET(state->win))) return G_SOURCE_CONTINUE;
    /* Showing across a session restart: the engine comes up with the machine. */
    if (!cocodebug_is_engaged(state->dbg) && cocosession_is_running(state->session)) {
        attach_and_refresh(state);
        return G_SOURCE_CONTINUE;
    }
    gen = cocodebug_generation(state->dbg);
    stopped = cocodebug_is_paused(state->dbg) != 0;
    {   /* a new stop brings the disassembly back to the PC, wherever it was scrolled */
        cocodebug_stop stop;
        cocodebug_stop_snapshot(state->dbg, &stop);
        if (stop.serial != state->seen_serial) {
            GtkAdjustment *adj = vadj(state->disasm);
            state->seen_serial = stop.serial;
            if (adj) gtk_adjustment_set_value(adj, 0);
            if (stop.paused && !gtk_check_button_get_active(state->follow_pc))
                gtk_check_button_set_active(state->follow_pc, TRUE);
            state->running_ticks = 0;
            recentre_views(state);
            return G_SOURCE_CONTINUE;
        }
    }
    if (!stopped && tab_visible(state, TAB_TRACE) && ++state->trace_ticks >= 4) {
        state->trace_ticks = 0;     /* the trace follows the ring while the machine runs */
        refresh_hist(state, 0, FALSE);
    }
    if (gen != state->seen_generation || stopped != state->was_stopped) {
        state->running_ticks = 0;
        refresh_now(state);
    } else if (!stopped && ++state->running_ticks >= 5) {
        /* live values twice a second while the machine runs */
        state->running_ticks = 0;
        refresh_now(state);
    }
    return G_SOURCE_CONTINUE;
}

/* The refresh timer runs only while the window is mapped. */
static void on_win_map(GtkWidget *widget, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)widget;
    if (!state->timer && !state->closing)
        state->timer = g_timeout_add(100, tick_refresh, state);
}

static void on_win_unmap(GtkWidget *widget, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)widget;
    if (state->timer) g_source_remove(state->timer);
    state->timer = 0;
}

/* ---- handlers ------------------------------------------------------------- */

static void on_run(GtkButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)button;
    if (cocodebug_is_paused(state->dbg)) {
        cocodebug_resume(state->dbg);
    } else {
        cocodebug_pause(state->dbg);
        settle(state, TRUE);
    }
    refresh_now(state);
}

/* A step: the machine is stopped, runs a moment, stops again. */
#define STEP_HANDLER(name, call) \
    static void name(GtkButton *button, gpointer user_data) \
    { \
        DbgWin *state = user_data; \
        (void)button; \
        call; \
        settle(state, TRUE); \
        refresh_now(state); \
    }
STEP_HANDLER(on_step, cocodebug_step(state->dbg))
STEP_HANDLER(on_step_over, cocodebug_step_over(state->dbg))
STEP_HANDLER(on_step_out, cocodebug_step_out(state->dbg))
STEP_HANDLER(on_step_line, cocodebug_step_line(state->dbg))
STEP_HANDLER(on_step_over_line, cocodebug_step_over_line(state->dbg))
#undef STEP_HANDLER

static void on_run_to_cursor(GtkButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)button;
    if (state->selected < 0) {
        toast(state, "Select a disassembly line to run to");
        return;
    }
    cocodebug_run_to(state->dbg, (uint16_t)state->selected);
    settle(state, TRUE);
    refresh_now(state);
}

/* The gutter and F9: a click on a breakpoint that is switched off (the hollow
 * dot) switches it back on; otherwise the breakpoint is set or removed. */
static void toggle_exec_breakpoint(DbgWin *state, uint16_t address)
{
    static cocodebug_breakpoint list[COCODEBUG_MAX_BREAKPOINTS];
    const int count = cocodebug_breakpoints_list(state->dbg, list, COCODEBUG_MAX_BREAKPOINTS);
    int index;
    for (index = 0; index < count; index++) {
        if (list[index].type == COCODEBUG_BP_EXEC && !list[index].enabled &&
            address >= list[index].start && address <= list[index].end) {
            cocodebug_breakpoint_enable(state->dbg, list[index].id, 1);
            return;
        }
    }
    cocodebug_breakpoint_toggle(state->dbg, address);
}

/* F9 and the gutter: an execute breakpoint on the selected line. */
static void toggle_selected(DbgWin *state)
{
    if (state->selected < 0) {
        toast(state, "Select a disassembly line first");
        return;
    }
    toggle_exec_breakpoint(state, (uint16_t)state->selected);
    refresh_now(state);
}

static void on_console(GtkEntry *entry, gpointer user_data)
{
    DbgWin *state = user_data;
    static char out[65536];
    const char *cmd = gtk_editable_get_text(GTK_EDITABLE(entry));
    char line[512];
    gboolean was_stopped = cocodebug_is_paused(state->dbg) != 0;
    if (!cmd || !*cmd) return;
    /* the history keeps each command once in a row */
    if (state->history->len == 0 ||
        strcmp(g_ptr_array_index(state->history, state->history->len - 1), cmd) != 0)
        g_ptr_array_add(state->history, g_strdup(cmd));
    state->history_pos = state->history->len;
    g_snprintf(line, sizeof line, "> %s\n", cmd);
    append_text(state->console_out, line, TRUE);
    out[0] = '\0';
    cocodebug_command(state->dbg, cmd, out, sizeof out);
    append_text(state->console_out, out, TRUE);
    if (out[0] && out[strlen(out) - 1] != '\n')
        append_text(state->console_out, "\n", TRUE);
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
    if (was_stopped) settle(state, TRUE);
    refresh_now(state);
}

static void history_show(DbgWin *state)
{
    const char *text = state->history_pos < state->history->len
                           ? g_ptr_array_index(state->history, state->history_pos) : "";
    gtk_editable_set_text(GTK_EDITABLE(state->console_in), text);
    gtk_editable_set_position(GTK_EDITABLE(state->console_in), -1);
}

/* Tab completes the current word against the commands, the registers and
 * the labels; several matches are listed instead. Up and Down walk the
 * command history. */
static gboolean on_console_key(GtkEventControllerKey *controller, guint keyval, guint code,
                               GdkModifierType modifiers, gpointer user_data)
{
    DbgWin *state = user_data;
    char comps[4096];
    const char *text;
    const char *word;
    int count;
    (void)controller; (void)code; (void)modifiers;
    if (keyval == GDK_KEY_Up || keyval == GDK_KEY_KP_Up) {
        if (state->history_pos > 0) {
            state->history_pos--;
            history_show(state);
        }
        return TRUE;
    }
    if (keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down) {
        if (state->history_pos < state->history->len) {
            state->history_pos++;
            history_show(state);
        }
        return TRUE;
    }
    if (keyval != GDK_KEY_Tab) return FALSE;
    text = gtk_editable_get_text(GTK_EDITABLE(state->console_in));
    comps[0] = '\0';
    /* the engine completes the end of the whole line (a command, then its
     * arguments), so the whole line goes and the first word is replaced */
    count = cocodebug_completions(state->dbg, text, comps, sizeof comps);
    word = strrchr(text, ' ');
    word = word ? word + 1 : text;
    if (count == 1) {
        char merged[600];
        char *newline = strchr(comps, '\n');
        if (newline) *newline = '\0';
        g_snprintf(merged, sizeof merged, "%.*s%s ", (int)(word - text), text, comps);
        gtk_editable_set_text(GTK_EDITABLE(state->console_in), merged);
        gtk_editable_set_position(GTK_EDITABLE(state->console_in), -1);
    } else if (count > 1) {
        /* the common start is filled in; the candidates are listed */
        char **cand = g_strsplit(comps, "\n", -1);
        size_t common = strlen(cand[0]), len = strlen(word);
        int index;
        for (index = 1; cand[index] && *cand[index]; index++) {
            size_t slot = 0;
            while (slot < common && cand[index][slot] == cand[0][slot]) slot++;
            common = slot;
        }
        if (common > len) {
            char merged[600];
            g_snprintf(merged, sizeof merged, "%.*s%.*s", (int)(word - text), text,
                       (int)common, cand[0]);
            gtk_editable_set_text(GTK_EDITABLE(state->console_in), merged);
            gtk_editable_set_position(GTK_EDITABLE(state->console_in), -1);
        }
        g_strfreev(cand);
        append_text(state->console_out, comps, FALSE);
        if (comps[0] && comps[strlen(comps) - 1] != '\n')
            append_text(state->console_out, "\n", FALSE);
    }
    return TRUE;
}

static void on_reg_activate(GtkEntry *entry, gpointer user_data)
{
    DbgWin *state = user_data;
    int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(entry), "reg"));
    long value;
    if (!cocodebug_is_paused(state->dbg))
        toast(state, "Stop the machine to edit the registers");
    else if (parse_num(gtk_editable_get_text(GTK_EDITABLE(entry)), &value)) {
        if (!cocodebug_regs_set(state->dbg, index, (unsigned)value))
            toast(state, "That register does not exist on this CPU");
    }
    /* Enter ends the edit: show the engine's value, or put the old one back */
    state->force_entries = TRUE;
    refresh_now(state);
    state->force_entries = FALSE;
}

/* A CC flag box: the engine takes the bit while the machine is stopped. */
static void on_flag_toggled(GtkCheckButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    const int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "flag"));
    if (state->flag_sync || state->closing) return;
    if (!cocodebug_is_paused(state->dbg))
        toast(state, "Stop the machine to edit the flags");
    else
        cocodebug_flag_set(state->dbg, k_flags[index].mask, gtk_check_button_get_active(button));
    state->force_entries = TRUE;
    refresh_now(state);
    state->force_entries = FALSE;
}

/* "address value...": bytes written into the space showing. */
static void on_mem_write(GtkEntry *entry, gpointer user_data)
{
    DbgWin *state = user_data;
    char **parts = g_strsplit_set(gtk_editable_get_text(GTK_EDITABLE(entry)), " \t:=,", -1);
    char err[128] = "";
    uint8_t bytes[POKE_MAX];
    const int space = mem_space_now(state);
    long address, value;
    int count = 0, index;
    gboolean valid;
    char *addr = NULL;
    for (index = 0; parts[index]; index++) {
        if (!*parts[index]) continue;
        if (!addr) addr = parts[index];
        else if (count < POKE_MAX && parse_num(parts[index], &value) && value >= 0 && value <= 255)
            bytes[count++] = (uint8_t)value;
        else { count = -1; break; }
    }
    valid = addr && count > 0 && parse_addr(state, addr, space, &address, err, sizeof err);
    if (valid && !cocodebug_is_paused(state->dbg)) {
        toast(state, "Stop the machine to poke memory");
    } else if (valid) {
        if (cocodebug_mem_write(state->dbg, space, (uint32_t)address, bytes, count) < count)
            toast(state, "Only RAM takes a poke: ROM and I/O were skipped");
        gtk_editable_set_text(GTK_EDITABLE(entry), "");
        refresh_now(state);
    } else {
        toast(state, err[0] ? err : "Poke takes an address and hex bytes, e.g. 0400 41 42");
    }
    g_strfreev(parts);
}

static void mem_set_base(DbgWin *state, long base)
{
    const int space = mem_space_now(state);
    const long size = (long)cocodebug_space_size(state->dbg, space);
    if (base > size - MEM_PAGE) base = size - MEM_PAGE;
    if (base < 0) base = 0;
    state->mem_base[space] = (uint32_t)base & ~0xFu;
    refresh_mem(state);
}

/* The page holding a range, the range highlighted (a little context above it). */
static void show_in_memory(DbgWin *state, int space, uint32_t first, uint32_t last)
{
    state->mem_hl = TRUE;
    state->mem_hl_space = space;
    state->mem_hl_lo = first;
    state->mem_hl_hi = last;
    gtk_drop_down_set_selected(state->mem_space, space == COCODEBUG_SPACE_PHYS ? 1 : 0);
    mem_set_base(state, ((long)first & ~0xF) - 4 * 16);
}

/* Go to: an address, a symbol, an expression or a range, in the space showing. */
static void on_mem_goto(GtkEntry *entry, gpointer user_data)
{
    DbgWin *state = user_data;
    const int space = mem_space_now(state);
    uint32_t first, last;
    if (!cocosym_parse_range(state->dbg, space, gtk_editable_get_text(GTK_EDITABLE(entry)), &first, &last)) {
        toast(state, "Not an address, a symbol or a range in this space");
        return;
    }
    show_in_memory(state, space, first, last);
}

/* The space chooser changed: the other space has its own page and highlight. */
static void on_mem_space(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)object; (void)pspec;
    if (!state->closing) refresh_mem(state);
}

/* Find Next / Find Previous: from the highlight (else the top of the page). */
static void on_search(GtkWidget *button, gpointer user_data)
{
    DbgWin *state = user_data;
    const int backward = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "back"));
    const int space = mem_space_now(state);
    uint8_t pat[COCOSEARCH_MAX], mask;
    int count, flags, result;
    uint32_t from, found;

    if (!cocosearch_parse((int)gtk_drop_down_get_selected(state->srch_kind),
                          gtk_editable_get_text(GTK_EDITABLE(state->srch_entry)),
                          pat, &count, &mask, &flags)) {
        gtk_label_set_text(state->srch_note, "Type hex bytes (86 41) or some text");
        return;
    }
    if (!cocodebug_is_paused(state->dbg)) {
        gtk_label_set_text(state->srch_note, "Stop the machine first");
        return;
    }
    if (state->mem_hl && state->mem_hl_space == space)
        from = state->mem_hl_lo;
    else if (backward)
        from = state->mem_base[space];
    else
        from = state->mem_base[space] ? state->mem_base[space] - 1 : cocodebug_space_size(state->dbg, space) - 1;
    result = cocodebug_search_next(state->dbg, space, pat, count, mask, flags, from, backward, &found);
    if (result > 0) {
        char msg[64];
        g_snprintf(msg, sizeof msg, "Found at $%0*X", space == COCODEBUG_SPACE_PHYS ? 6 : 4, found);
        show_in_memory(state, space, found, found + count - 1);
        gtk_label_set_text(state->srch_note, msg);
    } else if (result == 0) {
        gtk_label_set_text(state->srch_note, "Not found");
    } else {
        gtk_label_set_text(state->srch_note, "Stop the machine first");
    }
}

static void on_search_activate(GtkEntry *entry, gpointer user_data)
{
    on_search(g_object_get_data(G_OBJECT(entry), "next-button"), user_data);
}

static void on_mem_page(GtkButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    mem_set_base(state, (long)state->mem_base[mem_space_now(state)] +
                     GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "delta")) * MEM_PAGE);
}

/* Click selects a row; a click on the gutter or a double-click toggles its
 * breakpoint (a label line has no address, so it selects nothing). The
 * address comes from the row, never from the text. */
static void on_disasm_click(GtkGestureClick *gesture, int count, double position_x, double position_y, gpointer user_data)
{
    DbgWin *state = user_data;
    GtkTextIter iter, third;
    GdkRectangle rect;
    int buffer_x, buffer_y, line, top, height;
    gboolean gutter;
    (void)gesture;
    gtk_text_view_window_to_buffer_coords(state->disasm, GTK_TEXT_WINDOW_WIDGET, (int)position_x, (int)position_y, &buffer_x, &buffer_y);
    /* by line, not by hit-test: the margin left of the gutter is the gutter */
    gtk_text_view_get_line_at_y(state->disasm, &iter, buffer_y, NULL);
    gtk_text_view_get_line_yrange(state->disasm, &iter, &top, &height);
    if (buffer_y < top || buffer_y >= top + height) return;
    line = gtk_text_iter_get_line(&iter);
    if (line < 0 || line >= state->line_count || state->line_addr[line] < 0) return;
    third = iter;
    gtk_text_iter_set_line_offset(&third, 2);
    gtk_text_view_get_iter_location(state->disasm, &third, &rect);
    gutter = buffer_x < rect.x;
    state->selected = state->line_addr[line];
    if ((count == 1 && gutter) || (count == 2 && !gutter))
        toggle_exec_breakpoint(state, (uint16_t)state->selected);
    refresh_disasm(state);
}

static void on_follow_toggled(GtkCheckButton *button, gpointer user_data)
{
    (void)button;
    refresh_disasm(user_data);
}

static void on_jump(GtkEntry *entry, gpointer user_data)
{
    DbgWin *state = user_data;
    char err[128] = "";
    long address;
    if (!parse_addr(state, gtk_editable_get_text(GTK_EDITABLE(entry)), COCODEBUG_SPACE_CPU, &address, err, sizeof err)) {
        toast(state, err);
        return;
    }
    gtk_check_button_set_active(state->follow_pc, FALSE);
    state->disasm_top = (int)address;
    refresh_disasm(state);
}

/* The wheel scrolls by instruction. */
static gboolean on_disasm_scroll(GtkEventControllerScroll *controller, double delta_x, double delta_y, gpointer user_data)
{
    DbgWin *state = user_data;
    int rows = (int)(delta_y * 3);
    (void)controller; (void)delta_x;
    if (!rows) rows = delta_y > 0 ? 1 : -1;
    gtk_check_button_set_active(state->follow_pc, FALSE);
    state->disasm_top = cocodebug_row_address(state->dbg, (uint16_t)state->disasm_top, rows);
    refresh_disasm(state);
    return TRUE;
}

static void bp_enable_toggled(GtkCheckButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    cocodebug_breakpoint_enable(state->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "bp")),
                                gtk_check_button_get_active(button));
    refresh_now(state);
}

static void bp_remove_clicked(GtkButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    cocodebug_breakpoint_remove(state->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "bp")));
    refresh_now(state);
}

static void on_bp_add(GtkWidget *wgt, gpointer user_data)
{
    DbgWin *state = user_data;
    static const int types[4] = {
        COCODEBUG_BP_EXEC, COCODEBUG_BP_READ, COCODEBUG_BP_WRITE, COCODEBUG_BP_ACCESS };
    guint sel = gtk_drop_down_get_selected(state->bp_type);
    const gboolean phys = gtk_widget_get_visible(GTK_WIDGET(state->bp_phys)) &&
                          gtk_check_button_get_active(state->bp_phys);
    const int space = phys ? COCODEBUG_SPACE_PHYS : COCODEBUG_SPACE_CPU;
    long start, finish;
    int breakpoint_id, type;
    char err[256] = "";
    const char *end = gtk_editable_get_text(GTK_EDITABLE(state->bp_end));
    (void)wgt;
    if (sel >= G_N_ELEMENTS(types)) sel = 0;
    type = types[sel];
    if (phys && type == COCODEBUG_BP_EXEC) {
        gtk_label_set_text(state->bp_msg, "PHYS applies to read, write and access breakpoints");
        return;
    }
    if (!parse_addr(state, gtk_editable_get_text(GTK_EDITABLE(state->bp_start)), space, &start, err, sizeof err)) {
        gtk_label_set_text(state->bp_msg, err);
        return;
    }
    finish = start;
    if (end && *end && !parse_addr(state, end, space, &finish, err, sizeof err)) {
        gtk_label_set_text(state->bp_msg, err);
        return;
    }
    if (finish < start) {
        gtk_label_set_text(state->bp_msg, "The end comes before the start");
        return;
    }
    breakpoint_id = cocodebug_breakpoint_add(state->dbg, type | (phys ? COCODEBUG_BP_PHYS : 0), (uint32_t)start,
                                  (uint32_t)finish, gtk_editable_get_text(GTK_EDITABLE(state->bp_cond)));
    if (breakpoint_id < 0) {
        gtk_label_set_text(state->bp_msg, "The breakpoint was refused (does the condition parse?)");
        return;
    }
    gtk_label_set_text(state->bp_msg, "");
    gtk_editable_set_text(GTK_EDITABLE(state->bp_start), "");
    gtk_editable_set_text(GTK_EDITABLE(state->bp_end), "");
    gtk_editable_set_text(GTK_EDITABLE(state->bp_cond), "");
    refresh_now(state);
}

/* "Break at": a symbol or a hex address, then Enter, sets an execute
 * breakpoint there (once). */
static void on_break_at(GtkEntry *entry, gpointer user_data)
{
    DbgWin *state = user_data;
    char err[128] = "";
    long address;
    if (!parse_addr(state, gtk_editable_get_text(GTK_EDITABLE(entry)), COCODEBUG_SPACE_CPU, &address, err, sizeof err)) {
        toast(state, err);
        return;
    }
    if (!cocodebug_breakpoint_is_set(state->dbg, (uint16_t)address))
        cocodebug_breakpoint_toggle(state->dbg, (uint16_t)address);
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
    refresh_now(state);
}

static void on_bp_clear(GtkButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)button;
    cocodebug_breakpoint_clear_all(state->dbg);
    refresh_now(state);
}

static void on_bp_clear_hits(GtkButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)button;
    cocodebug_breakpoint_clear_hits(state->dbg);
    refresh_now(state);
}

static void load_symbols(DbgWin *state, const char *path)
{
    char msg[600];
    const int count = cocodebug_symbols_load(state->dbg, path);
    if (count < 0)
        g_snprintf(msg, sizeof msg, "Could not open the symbol file %s", path);
    else
        g_snprintf(msg, sizeof msg, "Loaded %d symbol%s from %s", count, count == 1 ? "" : "s", path);
    notice(state, msg);
    refresh_now(state);
}

static void on_clear_symbols(GtkButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)button;
    cocodebug_symbols_clear(state->dbg);
    notice(state, "Symbols cleared (the built-ins stay)");
    refresh_now(state);
}

static void on_symbols_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    DbgWin *state = user_data;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    if (!file || state->closing) return;
    path = g_file_get_path(file);
    if (path) load_symbols(state, path);
}

static void on_symbols(GtkWidget *button, gpointer user_data)
{
    DbgWin *state = user_data;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *syms = gtk_file_filter_new();
    GtkFileFilter *all = gtk_file_filter_new();
    (void)button;

    gtk_file_filter_set_name(syms, "Symbol files (*.sym, *.map, *.txt)");
    gtk_file_filter_add_suffix(syms, "sym");
    gtk_file_filter_add_suffix(syms, "map");
    gtk_file_filter_add_suffix(syms, "txt");
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    g_list_store_append(filters, syms);
    g_list_store_append(filters, all);
    gtk_file_dialog_set_title(dlg, "Load Symbols (LWASM --symbol-dump, or NAME ADDR lines)");
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    gtk_file_dialog_open(dlg, state->win, NULL, on_symbols_chosen, state);
    g_object_unref(syms);
    g_object_unref(all);
    g_object_unref(filters);
    g_object_unref(dlg);
}

/* One or more .lst files, one per module of a multi-file CMOC program --
 * cocodebug_source_load() adds to what is already loaded (cmoc names the .map
 * after the linked executable, so every module's listing wants the very .map
 * its neighbours do), so loading them all in one dialog is the multi-file case
 * working as intended. */
static void on_source_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    DbgWin *state = user_data;
    g_autoptr(GListModel) files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(src), res, NULL);
    guint index, count;
    int loaded = 0, failed = 0;

    if (!files || state->closing) return;
    count = g_list_model_get_n_items(files);
    for (index = 0; index < count; index++) {
        g_autoptr(GFile) file = g_list_model_get_item(files, index);
        g_autofree char *path = g_file_get_path(file);
        if (!path) continue;
        if (cocodebug_source_load(state->dbg, path) < 0) failed++;
        else loaded++;
    }
    if (failed && !loaded)
        notice(state, "Could not open the listing or its .map (both need `cmoc -i`)");
    else if (failed)
        notice(state, "Some listings could not be opened");
    if (loaded) {
        g_clear_pointer(&state->src_cached_path, g_free);
        g_clear_pointer(&state->src_cached_lines, g_strfreev);
        state->src_cached_lines = g_new0(char *, 1);
        rebuild_src_combo(state);
        show_tab(state, TAB_CSRC);
    }
    refresh_now(state);
}

static void on_load_source(GtkButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *lst = gtk_file_filter_new();
    GtkFileFilter *all = gtk_file_filter_new();
    (void)button;

    gtk_file_filter_set_name(lst, "CMOC listings (*.lst)");
    gtk_file_filter_add_suffix(lst, "lst");
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    g_list_store_append(filters, lst);
    g_list_store_append(filters, all);
    gtk_file_dialog_set_title(dlg, "Load C Source (select every module's .lst)");
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    gtk_file_dialog_open_multiple(dlg, state->win, NULL, on_source_chosen, state);
    g_object_unref(lst);
    g_object_unref(all);
    g_object_unref(filters);
    g_object_unref(dlg);
}

static void on_clear_source(GtkButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)button;
    cocodebug_source_clear(state->dbg);
    g_clear_pointer(&state->src_cached_path, g_free);
    g_clear_pointer(&state->src_cached_lines, g_strfreev);
    state->src_cached_lines = g_new0(char *, 1);
    state->pc_file_id = -1;
    rebuild_src_combo(state);
    notice(state, "C source cleared");
    refresh_now(state);
}

/* Break at the symbol row picked in the Symbols tab. */
static void on_sym_break(GtkButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    GtkListBoxRow *row = gtk_list_box_get_selected_row(state->sym_list);
    (void)button;
    if (!row) {
        toast(state, "Select a symbol first");
        return;
    }
    {
        const uint16_t address = (uint16_t)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "addr"));
        if (!cocodebug_breakpoint_is_set(state->dbg, address)) cocodebug_breakpoint_toggle(state->dbg, address);
    }
    refresh_now(state);
}

static void on_fuji_clear(GtkButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)button;
    cocodebug_fujinet_trace_clear(state->dbg);
    refresh_fujinet(state, TRUE);
}

static void on_fuji_hide_ready(GtkCheckButton *button, gpointer user_data)
{
    (void)button;
    refresh_fujinet(user_data, TRUE);
}

/* Help's contexts (data/help/help.md), one per tab */
static const char *const k_tab_contexts[NTABS] = {
    "debugger/console", "debugger/disassembly", "debugger/csource", "debugger/memory",
    "debugger/breakpoints", "debugger/watches", "debugger/cart", "debugger/trace",
    "debugger/history", "debugger/video", "debugger/sound", "debugger/symbols",
    "debugger/fujinet",
};

/* The notebook also switches pages as it is taken apart with the window
 * (main window closing), before the window's "destroy": only a mapped window
 * has a tab to refresh, a hidden one refreshes when it is shown. */
static void on_switch_page(GtkNotebook *notebook, GtkWidget *page, guint num, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)notebook; (void)num;
    if (state->closing) return;
    coco_help_set_context(state->win, k_tab_contexts[page_tab(page)]);
    if (gtk_widget_get_mapped(GTK_WIDGET(state->win))) {
        refresh_tab(state, page_tab(page));
    }
    {   /* the page is in front only after this signal: decide from the page */
        const gboolean want = page_tab(page) == TAB_SOUND || state->detached[TAB_SOUND] != NULL;
        if (want != state->sound_watching) {
            state->sound_watching = want;
            cocodebug_sound_watch(state->dbg, want);
        }
    }
}

/* Record on either page is the one recorder. */
static void on_hist_rec(GtkToggleButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    if (state->h_sync) return;
    cocodebug_trace_enable(state->dbg, gtk_toggle_button_get_active(button));
    state->h_sig[0] = state->h_sig[1] = ~(uint64_t)0;
    refresh_now(state);
}

static void on_hist_clear(GtkButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)button;
    cocodebug_trace_clear(state->dbg);
    state->h_sig[0] = state->h_sig[1] = ~(uint64_t)0;
    refresh_now(state);
}

static void on_hist_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)object; (void)pspec;
    state->h_sig[0] = state->h_sig[1] = ~(uint64_t)0;
    refresh_now(state);
}

static void on_hist_activate(GtkEntry *entry, gpointer user_data)
{
    on_hist_changed(NULL, NULL, user_data);
    (void)entry;
}

static gboolean on_key(GtkEventControllerKey *controller, guint keyval, guint code,
                       GdkModifierType modifiers, gpointer user_data);

/* Puts a popped-out tab back in the notebook, at its place in the order. */
static void dock_tab(DbgWin *state, int tab, gboolean destroy)
{
    GtkWindow *win = state->detached[tab];
    GtkWidget *page = state->pages[tab], *vbox;
    int pos = 0, tab_index;

    if (!win) return;
    vbox = g_object_get_data(G_OBJECT(win), "vbox");
    g_object_ref(page);
    gtk_box_remove(GTK_BOX(vbox), page);
    state->detached[tab] = NULL;
    for (tab_index = 0; tab_index < tab; tab_index++)
        if (!state->detached[tab_index]) pos++;
    gtk_notebook_insert_page(state->notebook, page, gtk_label_new(k_tab_names[tab]), pos);
    g_object_unref(page);
    gtk_notebook_set_current_page(state->notebook, pos);
    if (destroy) gtk_window_destroy(win);
    if (!state->closing) refresh_now(state);
}

static void on_dock_button(GtkButton *button, gpointer user_data)
{
    dock_tab(user_data, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "tab")), TRUE);
}

static gboolean on_detached_close(GtkWindow *win, gpointer user_data)
{
    dock_tab(user_data, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(win), "tab")), FALSE);
    return FALSE;     /* let it close */
}

/* Any tab can move to a window of its own; one always stays. */
static void pop_out_tab(DbgWin *state, int tab)
{
    GtkWidget *page, *vbox, *header, *dock;
    GtkWindow *win;
    GtkEventController *keys;
    char title[96];

    if (tab < 0 || tab >= NTABS || state->detached[tab]) return;
    if (gtk_notebook_get_n_pages(state->notebook) <= 1) {
        toast(state, "One tab always stays in the debugger");
        return;
    }
    page = state->pages[tab];
    g_object_ref(page);
    gtk_notebook_remove_page(state->notebook, gtk_notebook_page_num(state->notebook, page));

    win = GTK_WINDOW(adw_window_new());
    g_snprintf(title, sizeof title, "CoCo Debugger - %s", k_tab_names[tab]);
    gtk_window_set_title(win, title);
    gtk_window_set_icon_name(win, coco_icon_name());
    gtk_window_set_default_size(win, 800, 560);
    gtk_window_set_transient_for(win, state->win);
    header = adw_header_bar_new();
    dock = gtk_button_new_with_label("Dock");
    gtk_widget_set_tooltip_text(dock, "Put this tab back in the debugger");
    g_object_set_data(G_OBJECT(dock), "tab", GINT_TO_POINTER(tab));
    g_signal_connect(dock, "clicked", G_CALLBACK(on_dock_button), state);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), dock);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), coco_help_button(NULL));
    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(vbox), header);
    gtk_box_append(GTK_BOX(vbox), page);
    gtk_widget_set_vexpand(page, TRUE);
    adw_window_set_content(ADW_WINDOW(win), vbox);
    g_object_unref(page);
    g_object_set_data(G_OBJECT(win), "vbox", vbox);
    g_object_set_data(G_OBJECT(win), "tab", GINT_TO_POINTER(tab));
    coco_help_set_context(win, k_tab_contexts[tab]);
    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), state);
    gtk_widget_add_controller(GTK_WIDGET(win), keys);
    g_signal_connect(win, "close-request", G_CALLBACK(on_detached_close), state);
    state->detached[tab] = win;
    gtk_window_present(win);
    refresh_now(state);
}

static void on_pop_out(GtkButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)button;
    pop_out_tab(state, current_tab(state));
}

/* Closing hides: the machine runs on, and the window is back on F12. */
static void hide_window(DbgWin *state)
{
    int tab;
    for (tab = 0; tab < NTABS; tab++)
        dock_tab(state, tab, TRUE);       /* popped-out tabs go home with the window */
    if (state->sound_watching) {
        state->sound_watching = FALSE;
        cocodebug_sound_watch(state->dbg, 0);
    }
    cocodebug_set_engaged(state->dbg, 0);     /* releasing resumes the machine */
    gtk_widget_set_visible(GTK_WIDGET(state->win), FALSE);
}

static gboolean on_key(GtkEventControllerKey *controller, guint keyval, guint code,
                       GdkModifierType modifiers, gpointer user_data)
{
    DbgWin *state = user_data;
    GtkWidget *src = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
    (void)code;
    switch (keyval) {
    case GDK_KEY_F1: coco_help_for(GTK_IS_WINDOW(src) ? GTK_WINDOW(src) : state->win); return TRUE;
    case GDK_KEY_F5: on_run(NULL, state); return TRUE;
    case GDK_KEY_F6:
        if (modifiers & GDK_SHIFT_MASK) on_step_over_line(NULL, state); else on_step_line(NULL, state);
        return TRUE;
    case GDK_KEY_F7: on_step(NULL, state); return TRUE;
    case GDK_KEY_F8:
        if (modifiers & GDK_SHIFT_MASK) on_step_out(NULL, state); else on_step_over(NULL, state);
        return TRUE;
    case GDK_KEY_F9: toggle_selected(state); return TRUE;
    case GDK_KEY_F12: hide_window(state); return TRUE;
    default: return FALSE;
    }
}

static gboolean on_close(GtkWindow *win, gpointer user_data)
{
    (void)win;
    hide_window(user_data);
    return TRUE;
}

/* ---- construction --------------------------------------------------------- */

/* A horizontal row; flow_add appends to it. */
static GtkWidget *flow_row(void)
{
    return gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
}

static void flow_add(GtkWidget *row, GtkWidget *child)
{
    gtk_box_append(GTK_BOX(row), child);
}

static GtkWidget *build_toolbar(DbgWin *state)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *stepping = flow_row();
    GtkWidget *tools = flow_row();
    GtkWidget *button;

    gtk_widget_set_margin_start(bar, 8);
    gtk_widget_set_margin_end(bar, 8);
    gtk_widget_set_margin_top(bar, 6);
    gtk_widget_set_margin_bottom(bar, 6);
#define TB(label, tip, callback, var) do { button = gtk_button_new_with_label(label); \
    gtk_widget_set_tooltip_text(button, tip); \
    g_signal_connect(button, "clicked", G_CALLBACK(callback), state); flow_add(stepping, button); \
    var = button; } while (0)
    GtkWidget *unused;
    state->run_btn = GTK_BUTTON(gtk_button_new_with_label("Pause (F5)"));
    g_signal_connect(state->run_btn, "clicked", G_CALLBACK(on_run), state);
    flow_add(stepping, GTK_WIDGET(state->run_btn));
    TB("Step (F7)", "One instruction", on_step, unused);
    TB("Over (F8)", "Over a subroutine call", on_step_over, unused);
    TB("Out (\xe2\x87\xa7""F8)", "Until this routine returns", on_step_out, unused);
    TB("Line (F6)", "Run until a different C source line (needs C source loaded)", on_step_line,
       state->step_line_btn);
    TB("Over Line (\xe2\x87\xa7""F6)",
       "Like Step Line, but a call on this line runs to completion", on_step_over_line,
       state->step_over_line_btn);
    TB("To Cursor", "Run to the selected disassembly line", on_run_to_cursor, unused);
    TB("Clear BPs", "Remove every breakpoint", on_bp_clear, unused);
#undef TB
    (void)unused;

    /* Break at: a symbol or a hex address, then Enter */
    state->break_at = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(state->break_at, "symbol or hex");
    gtk_editable_set_width_chars(GTK_EDITABLE(state->break_at), 14);
    gtk_widget_set_tooltip_text(GTK_WIDGET(state->break_at),
                                "Set an execute breakpoint at a symbol, a hex address or an expression");
    g_signal_connect(state->break_at, "activate", G_CALLBACK(on_break_at), state);
    flow_add(tools, labeled("Break at", GTK_WIDGET(state->break_at)));

    button = gtk_button_new_with_label("Symbols\xe2\x80\xa6");
    gtk_widget_set_tooltip_text(button, "Load a symbol file (LWASM --symbol-dump, or NAME ADDR lines)");
    g_signal_connect(button, "clicked", G_CALLBACK(on_symbols), state);
    flow_add(tools, button);
    button = gtk_button_new_with_label("Clear Symbols");
    gtk_widget_set_tooltip_text(button, "Forget every loaded symbol; the built-in BASIC vectors stay");
    g_signal_connect(button, "clicked", G_CALLBACK(on_clear_symbols), state);
    flow_add(tools, button);
    button = gtk_button_new_with_label("C Source\xe2\x80\xa6");
    gtk_widget_set_tooltip_text(button, "Load CMOC listings (.lst, from `cmoc -i`) for source-level stepping");
    g_signal_connect(button, "clicked", G_CALLBACK(on_load_source), state);
    flow_add(tools, button);
    button = gtk_button_new_with_label("Clear C Source");
    gtk_widget_set_tooltip_text(button, "Drop the loaded C source info");
    g_signal_connect(button, "clicked", G_CALLBACK(on_clear_source), state);
    flow_add(tools, button);

    button = gtk_button_new_with_label("Pop Out");
    gtk_widget_set_tooltip_text(button, "Move the tab showing into a window of its own");
    gtk_widget_set_halign(button, GTK_ALIGN_END);
    gtk_widget_set_hexpand(button, TRUE);
    g_signal_connect(button, "clicked", G_CALLBACK(on_pop_out), state);
    flow_add(tools, button);
    gtk_box_append(GTK_BOX(bar), stepping);
    gtk_box_append(GTK_BOX(bar), tools);
    return bar;
}

static GtkWidget *build_console(DbgWin *state)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *scroll = mono_view(&state->console_out, TRUE);
    GtkEventController *keys;

    make_log(state->console_out);
    state->console_in = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(state->console_in,
        "Command (help, step, break, bpw, print, mem, poke, video, disasm, cart, ...) "
        "\xe2\x80\x94 Tab completes, Up/Down recall");
    gtk_widget_set_hexpand(GTK_WIDGET(state->console_in), TRUE);
    g_signal_connect(state->console_in, "activate", G_CALLBACK(on_console), state);
    /* ahead of the entry's own text handling, so Tab and Up/Down are ours */
    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_console_key), state);
    gtk_widget_add_controller(GTK_WIDGET(state->console_in), keys);
    state->history = g_ptr_array_new_with_free_func(g_free);

    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(state->console_in));
    set_text_at(state->console_out, "CoCo debugger console (MC6809 / HD6309). Type 'help' for every command. "
                                "Numbers: $0400, 0x400, 400h, 1024.\n", TRUE);
    return padded(box);
}

/* Under the notebook, on every tab: the registers (Enter sets one while the
 * machine is stopped), the CC flags and the machine. */
static GtkWidget *reg_entry(DbgWin *state, int index)
{
    state->reg[index] = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_max_length(state->reg[index], 6);
    gtk_editable_set_width_chars(GTK_EDITABLE(state->reg[index]), k_regs[index].digits + 1);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(state->reg[index]), k_regs[index].digits + 1);
    gtk_widget_add_css_class(GTK_WIDGET(state->reg[index]), "monospace");
    g_object_set_data(G_OBJECT(state->reg[index]), "reg", GINT_TO_POINTER(index));
    g_signal_connect(state->reg[index], "activate", G_CALLBACK(on_reg_activate), state);
    state->reg_box[index] = labeled(k_regs[index].name, GTK_WIDGET(state->reg[index]));
    return state->reg_box[index];
}

/* PC A B D DP X Y U S CC, the eight CC boxes, and (a 6309 only) E F W V MD. */
static GtkWidget *build_regs(DbgWin *state)
{
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *second = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *flags = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    int index;
    for (index = 0; index < COCODEBUG_REG_CC; index++)
        gtk_box_append(GTK_BOX(box), reg_entry(state, index));
    gtk_box_append(GTK_BOX(second), reg_entry(state, COCODEBUG_REG_CC));  /* CC, then its flags */
    for (index = 0; index < 8; index++) {
        state->flag[index] = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(k_flags[index].name));
        g_object_set_data(G_OBJECT(state->flag[index]), "flag", GINT_TO_POINTER(index));
        g_signal_connect(state->flag[index], "toggled", G_CALLBACK(on_flag_toggled), state);
        gtk_box_append(GTK_BOX(flags), GTK_WIDGET(state->flag[index]));
    }
    gtk_widget_set_tooltip_text(flags, "CC: Entire, FIRQ mask, Half carry, IRQ mask, Negative, Zero, "
                                       "oVerflow, Carry (edit while stopped)");
    gtk_box_append(GTK_BOX(second), flags);
    state->extra_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    for (index = REGS_6309; index < NREGS; index++)
        gtk_box_append(GTK_BOX(state->extra_box), reg_entry(state, index));
    gtk_box_append(GTK_BOX(second), state->extra_box);
    gtk_widget_set_visible(state->extra_box, FALSE);     /* shown on a 6309 */
    state->machine = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(state->machine), "dim-label");
    gtk_widget_set_hexpand(GTK_WIDGET(state->machine), TRUE);
    gtk_label_set_xalign(state->machine, 1.0f);
    gtk_box_append(GTK_BOX(second), GTK_WIDGET(state->machine));
    gtk_box_append(GTK_BOX(outer), box);
    gtk_box_append(GTK_BOX(outer), second);
    return padded(outer);
}

/* Find Next / Previous over the dump, with a note for what happened. */
static GtkWidget *build_search_bar(DbgWin *state)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *next = gtk_button_new_with_label("Find Next");
    GtkWidget *prev = gtk_button_new_with_label("Find Previous");
    static const char *const kinds[] = { "Hex", "Text", NULL };

    g_object_set_data(G_OBJECT(prev), "back", GINT_TO_POINTER(1));
    g_signal_connect(next, "clicked", G_CALLBACK(on_search), state);
    g_signal_connect(prev, "clicked", G_CALLBACK(on_search), state);
    state->srch_kind = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(kinds));
    gtk_widget_set_tooltip_text(GTK_WIDGET(state->srch_kind), "Hex bytes (86 41) or text (one byte per character)");
    state->srch_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(state->srch_entry, "search the space showing: 86 41, or text");
    gtk_widget_set_hexpand(GTK_WIDGET(state->srch_entry), TRUE);
    g_object_set_data(G_OBJECT(state->srch_entry), "next-button", next);
    g_signal_connect(state->srch_entry, "activate", G_CALLBACK(on_search_activate), state);
    state->srch_note = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(state->srch_note), "dim-label");
    gtk_label_set_ellipsize(state->srch_note, PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(state->srch_note, 18);
    gtk_label_set_max_width_chars(state->srch_note, 18);
    gtk_box_append(GTK_BOX(bar), gtk_label_new("Search"));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(state->srch_kind));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(state->srch_entry));
    gtk_box_append(GTK_BOX(bar), next);
    gtk_box_append(GTK_BOX(bar), prev);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(state->srch_note));
    return bar;
}

static GtkWidget *build_memory(DbgWin *state)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *mem = mono_view(&state->mem_view, FALSE);
    GtkWidget *edit = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *page_up, *down;

    state->mem_goto = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_width_chars(GTK_EDITABLE(state->mem_goto), 16);
    gtk_entry_set_placeholder_text(state->mem_goto, "$0400 / symbol / lo-hi");
    g_signal_connect(state->mem_goto, "activate", G_CALLBACK(on_mem_goto), state);
    state->mem_write = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_width_chars(GTK_EDITABLE(state->mem_write), 14);
    gtk_entry_set_placeholder_text(state->mem_write, "0400 41 42");
    g_signal_connect(state->mem_write, "activate", G_CALLBACK(on_mem_write), state);
    page_up = gtk_button_new_with_label("Page Up");
    down = gtk_button_new_with_label("Page Down");
    g_object_set_data(G_OBJECT(page_up), "delta", GINT_TO_POINTER(-1));
    g_object_set_data(G_OBJECT(down), "delta", GINT_TO_POINTER(1));
    g_signal_connect(page_up, "clicked", G_CALLBACK(on_mem_page), state);
    g_signal_connect(down, "clicked", G_CALLBACK(on_mem_page), state);
    state->mem_space = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(k_space_labels));
    gtk_widget_set_tooltip_text(GTK_WIDGET(state->mem_space),
        "CPU: the 64K the CPU sees. PHYS: a CoCo 3's physical RAM (the GIME's MMU banks)");
    g_signal_connect(state->mem_space, "notify::selected", G_CALLBACK(on_mem_space), state);
    state->mem_space_box = labeled("Space", GTK_WIDGET(state->mem_space));
    gtk_widget_set_visible(state->mem_space_box, FALSE);     /* PHYS exists on a CoCo 3 only */
    gtk_box_append(GTK_BOX(edit), state->mem_space_box);
    gtk_box_append(GTK_BOX(edit), labeled("Go to", GTK_WIDGET(state->mem_goto)));
    gtk_box_append(GTK_BOX(edit), page_up);
    gtk_box_append(GTK_BOX(edit), down);
    gtk_box_append(GTK_BOX(edit), labeled("Poke (address bytes)", GTK_WIDGET(state->mem_write)));

    gtk_box_append(GTK_BOX(box), edit);
    gtk_box_append(GTK_BOX(box), build_search_bar(state));
    gtk_box_append(GTK_BOX(box), mem);
    /* the highlight covers a few bytes, not a paragraph */
    gtk_text_buffer_create_tag(gtk_text_view_get_buffer(state->mem_view), "mark",
                               "background", "#8BC34A", "foreground", "#000000", NULL);
    state->mem_hl = FALSE;
    return padded(box);
}

static GtkWidget *build_disasm(DbgWin *state)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *scroll = mono_view(&state->disasm, FALSE);
    GtkGesture *click = gtk_gesture_click_new();
    GtkEventController *scrollc = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(state->disasm);
    GtkWidget *hint;

    /* later tags win: the selection under the PC line, the gutter marker
     * over both */
    gtk_text_buffer_create_tag(buffer, "dim", "foreground", "#8a8a8a", NULL);
    gtk_text_buffer_create_tag(buffer, "sel", "paragraph-background", "rgba(128,128,128,0.35)", NULL);
    accent_tag(state->disasm, "pc");
    gtk_text_buffer_create_tag(buffer, "bp_on", "foreground", "#e01b24", NULL);
    gtk_text_buffer_create_tag(buffer, "bp_off", "foreground", "#8a8a8a", NULL);

    state->follow_pc = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Follow PC"));
    gtk_check_button_set_active(state->follow_pc, TRUE);
    g_signal_connect(state->follow_pc, "toggled", G_CALLBACK(on_follow_toggled), state);
    state->jump = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(state->jump, "$A000 / symbol / expression");
    gtk_editable_set_width_chars(GTK_EDITABLE(state->jump), 22);
    g_signal_connect(state->jump, "activate", G_CALLBACK(on_jump), state);
    hint = gtk_label_new("Click selects a line; double-click, F9 or the gutter toggles its "
                         "breakpoint; scroll to browse");
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(state->follow_pc));
    gtk_box_append(GTK_BOX(row), labeled("Jump to", GTK_WIDGET(state->jump)));
    gtk_box_append(GTK_BOX(row), hint);

    g_signal_connect(click, "pressed", G_CALLBACK(on_disasm_click), state);
    gtk_widget_add_controller(GTK_WIDGET(state->disasm), GTK_EVENT_CONTROLLER(click));
    g_signal_connect(scrollc, "scroll", G_CALLBACK(on_disasm_scroll), state);
    gtk_widget_add_controller(GTK_WIDGET(state->disasm), scrollc);
    /* the wheel is ours; EXTERNAL (unlike NEVER) does not ask for a window
     * tall enough to show every one of the DISASM_WINDOW lines */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_EXTERNAL);

    gtk_box_append(GTK_BOX(box), row);
    gtk_box_append(GTK_BOX(box), scroll);
    state->disasm_top = 0;
    state->selected = -1;
    return padded(box);
}

static GtkWidget *build_breaks(DbgWin *state)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *add, *clear, *hits, *head, *hint;
    static const char *const types[] = { "Execute", "Read", "Write", "Access", NULL };

    state->bp_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(state->bp_list, GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(state->bp_list));
    gtk_widget_set_vexpand(scroll, TRUE);

    state->bp_type = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(types));
    state->bp_start = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(state->bp_start, "$0400 / symbol / expression");
    gtk_editable_set_width_chars(GTK_EDITABLE(state->bp_start), 22);
    state->bp_end = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(state->bp_end, "(end)");
    gtk_editable_set_width_chars(GTK_EDITABLE(state->bp_end), 8);
    state->bp_cond = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(state->bp_cond, "condition, e.g. a == $FF && value != old "
                                               "\xe2\x80\x94 optional");
    gtk_widget_set_hexpand(GTK_WIDGET(state->bp_cond), TRUE);
    g_signal_connect(state->bp_start, "activate", G_CALLBACK(on_bp_add), state);
    g_signal_connect(state->bp_end, "activate", G_CALLBACK(on_bp_add), state);
    g_signal_connect(state->bp_cond, "activate", G_CALLBACK(on_bp_add), state);
    add = gtk_button_new_with_label("Add");
    g_signal_connect(add, "clicked", G_CALLBACK(on_bp_add), state);
    clear = gtk_button_new_with_label("Clear All");
    g_signal_connect(clear, "clicked", G_CALLBACK(on_bp_clear), state);
    state->bp_phys = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("PHYS"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(state->bp_phys),
        "The range is a CoCo 3 physical RAM address (read, write and access only)");
    gtk_widget_set_visible(GTK_WIDGET(state->bp_phys), FALSE);   /* a CoCo 3 only */
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(state->bp_type));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(state->bp_phys));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(state->bp_start));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(state->bp_end));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(state->bp_cond));
    gtk_box_append(GTK_BOX(row), add);
    gtk_box_append(GTK_BOX(row), clear);
    state->bp_msg = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(state->bp_msg), "coco-accent-text");
    gtk_label_set_xalign(state->bp_msg, 0);
    gtk_label_set_wrap(state->bp_msg, TRUE);
    hint = gtk_label_new("Numbers are hex. A condition is an expression like the console's "
                         "(a, x, cc, [$0400]); `value`, `addr` and `old` are the access. "
                         "A disabled execute breakpoint shows as a grey hollow dot.");
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);

    state->bp_stop = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(state->bp_stop, 0);
    gtk_widget_set_hexpand(GTK_WIDGET(state->bp_stop), TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(state->bp_stop), "heading");
    hits = gtk_button_new_with_label("Clear Hits");
    gtk_widget_set_tooltip_text(hits, "Zero every hit count");
    g_signal_connect(hits, "clicked", G_CALLBACK(on_bp_clear_hits), state);
    head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(head), GTK_WIDGET(state->bp_stop));
    gtk_box_append(GTK_BOX(head), hits);

    gtk_box_append(GTK_BOX(box), head);
    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), row);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(state->bp_msg));
    gtk_box_append(GTK_BOX(box), hint);
    return padded(box);
}

static GtkWidget *build_watches(DbgWin *state)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *form = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *add;

    state->wt_labels = g_ptr_array_new();
    state->wt_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(state->wt_entry, "address, symbol, symbol+3, [ptr], 0400-05FF");
    gtk_widget_set_hexpand(GTK_WIDGET(state->wt_entry), TRUE);
    g_signal_connect(state->wt_entry, "activate", G_CALLBACK(on_wt_add), state);
    add = gtk_button_new_with_label("Add");
    g_signal_connect(add, "clicked", G_CALLBACK(on_wt_add), state);
    state->wt_space = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(k_space_labels));
    gtk_widget_set_tooltip_text(GTK_WIDGET(state->wt_space), "The space the address is in (PHYS on a CoCo 3)");
    gtk_widget_set_visible(GTK_WIDGET(state->wt_space), FALSE);
    gtk_box_append(GTK_BOX(form), gtk_label_new("Watch"));
    gtk_box_append(GTK_BOX(form), GTK_WIDGET(state->wt_space));
    gtk_box_append(GTK_BOX(form), GTK_WIDGET(state->wt_entry));
    gtk_box_append(GTK_BOX(form), add);

    state->wt_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(state->wt_list, GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(state->wt_list));
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(box), form);
    gtk_box_append(GTK_BOX(box), scroll);
    return padded(box);
}

static GtkWidget *build_cart(DbgWin *state)
{
    return padded(mono_view(&state->cart_text, FALSE));
}

static GtkWidget *build_sound(DbgWin *state)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *text = mono_view(&state->sound_text, FALSE);

    state->sound_hist = g_new0(cocodebug_sound_write, SOUND_SHOWN);
    state->sound_wave = gtk_drawing_area_new();
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(state->sound_wave), WAVE_H);
    gtk_widget_set_hexpand(state->sound_wave, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(state->sound_wave), draw_wave, state, NULL);
    gtk_box_append(GTK_BOX(box), text);
    gtk_box_append(GTK_BOX(box), headed("DAC writes", state->sound_wave));
    return padded(box);
}

/* The C Source tab: a file picker over the loaded listings, the listing (a
 * click toggles a breakpoint on a line), and the function's locals beside it. */
static GtkWidget *build_csrc(DbgWin *state)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *list_scroll = gtk_scrolled_window_new();
    GtkWidget *locals_scroll = gtk_scrolled_window_new();

    state->src_file_combo = GTK_DROP_DOWN(gtk_drop_down_new(NULL, NULL));
    gtk_widget_set_sensitive(GTK_WIDGET(state->src_file_combo), FALSE);
    g_signal_connect(state->src_file_combo, "notify::selected", G_CALLBACK(on_src_file_changed), state);
    state->src_note = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(state->src_note), "dim-label");
    gtk_label_set_xalign(state->src_note, 0);
    gtk_label_set_wrap(state->src_note, TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(state->src_note), TRUE);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(state->src_file_combo));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(state->src_note));

    state->src_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(state->src_list, GTK_SELECTION_NONE);
    g_signal_connect(state->src_list, "row-activated", G_CALLBACK(on_src_row_activated), state);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(list_scroll), GTK_WIDGET(state->src_list));
    gtk_widget_set_hexpand(list_scroll, TRUE);
    gtk_widget_set_vexpand(list_scroll, TRUE);

    state->src_locals = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(state->src_locals, 0.0f);
    gtk_label_set_yalign(state->src_locals, 0.0f);
    gtk_widget_add_css_class(GTK_WIDGET(state->src_locals), "monospace");
    gtk_widget_set_margin_start(GTK_WIDGET(state->src_locals), 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(locals_scroll), GTK_WIDGET(state->src_locals));
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(locals_scroll), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(locals_scroll, 400, -1);
    gtk_widget_set_vexpand(locals_scroll, TRUE);

    gtk_box_append(GTK_BOX(body), list_scroll);
    gtk_box_append(GTK_BOX(body), headed("Locals", locals_scroll));
    gtk_box_append(GTK_BOX(box), row);
    gtk_box_append(GTK_BOX(box), body);
    state->pc_file_id = state->pc_line = -1;
    state->src_displayed_file_id = -1;
    state->src_cached_lines = g_new0(char *, 1);
    return padded(box);
}

/* A filter over every known symbol, built-in and loaded; a row opens the
 * disassembly at its address. */
static GtkWidget *build_symbols(DbgWin *state)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *scroll = gtk_scrolled_window_new();

    state->sym_filter = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(state->sym_filter, "filter by name");
    gtk_widget_set_hexpand(GTK_WIDGET(state->sym_filter), TRUE);
    g_signal_connect(state->sym_filter, "changed", G_CALLBACK(on_sym_filter_changed), state);
    state->sym_note = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(state->sym_note), "dim-label");
    gtk_box_append(GTK_BOX(bar), gtk_label_new("Filter"));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(state->sym_filter));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(state->sym_note));
    {
        static const struct { const char *text; GCallback callback; } btns[] = {
            { "Break at", G_CALLBACK(on_sym_break) },
            { "Load\xe2\x80\xa6", G_CALLBACK(on_symbols) },
            { "Clear", G_CALLBACK(on_clear_symbols) },
        };
        size_t slot;
        for (slot = 0; slot < G_N_ELEMENTS(btns); slot++) {
            GtkWidget *button = gtk_button_new_with_label(btns[slot].text);
            g_signal_connect(button, "clicked", btns[slot].callback, state);
            gtk_box_append(GTK_BOX(bar), button);
        }
    }

    state->sym_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(state->sym_list, GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(state->sym_list, FALSE);
    g_signal_connect(state->sym_list, "row-activated", G_CALLBACK(on_sym_row_activated), state);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(state->sym_list));
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(box), bar);
    gtk_box_append(GTK_BOX(box), scroll);
    return padded(box);
}

/* Trace (idx 0) and History (idx 1): the same controls over the one ring. */
static GtkWidget *build_hist(DbgWin *state, int idx)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scroll = mono_view(&state->h_text[idx], FALSE);
    GtkWidget *clear = gtk_button_new_with_label("Clear");
    static const char *const filters[] = { "All instructions", "Branches only", "In range", NULL };

    state->h_rec[idx] = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Record"));
    gtk_widget_add_css_class(GTK_WIDGET(state->h_rec[idx]), "coco-toggle");
    gtk_widget_set_tooltip_text(GTK_WIDGET(state->h_rec[idx]),
                                "Record every instruction the CPU runs (the machine runs slower)");
    g_signal_connect(state->h_rec[idx], "toggled", G_CALLBACK(on_hist_rec), state);
    state->h_filter[idx] = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(filters));
    g_signal_connect(state->h_filter[idx], "notify::selected", G_CALLBACK(on_hist_changed), state);
    state->h_range[idx] = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(state->h_range[idx], "range: A000-A0FF or a symbol");
    g_signal_connect(state->h_range[idx], "activate", G_CALLBACK(on_hist_activate), state);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_hist_clear), state);
    state->h_note[idx] = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(state->h_note[idx]), "dim-label");
    gtk_widget_set_hexpand(GTK_WIDGET(state->h_note[idx]), TRUE);
    gtk_label_set_xalign(state->h_note[idx], 1.0f);
    make_log(state->h_text[idx]);
    state->h_sig[idx] = ~(uint64_t)0;

    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(state->h_rec[idx]));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(state->h_filter[idx]));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(state->h_range[idx]));
    gtk_box_append(GTK_BOX(bar), clear);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(state->h_note[idx]));
    gtk_box_append(GTK_BOX(box), bar);
    gtk_box_append(GTK_BOX(box), scroll);
    return padded(box);
}

/* The packets left (a note and Clear over them), the BoIP link and its
 * counts right. */
static GtkWidget *build_fujinet(DbgWin *state)
{
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *clear = gtk_button_new_with_label("Clear");
    GtkWidget *hide = gtk_check_button_new_with_label("Hide READY probes");
    GtkWidget *start, *end;

    state->fuji_hide_ready = GTK_CHECK_BUTTON(hide);
    gtk_check_button_set_active(state->fuji_hide_ready, TRUE);
    gtk_widget_set_tooltip_text(hide, "fujinet-lib asks E2 00 before every call");
    g_signal_connect(hide, "toggled", G_CALLBACK(on_fuji_hide_ready), state);
    state->fuji_note = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(state->fuji_note), "dim-label");
    gtk_label_set_xalign(state->fuji_note, 0);
    gtk_label_set_ellipsize(state->fuji_note, PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(GTK_WIDGET(state->fuji_note), TRUE);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_fuji_clear), state);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(state->fuji_note));
    gtk_box_append(GTK_BOX(row), hide);
    gtk_box_append(GTK_BOX(row), clear);
    gtk_box_append(GTK_BOX(left), row);
    gtk_box_append(GTK_BOX(left), mono_view(&state->fuji_text, FALSE));
    make_log(state->fuji_text);

    start = headed("DriveWire packets", left);
    end = headed("Becker port", mono_view(&state->fuji_state, FALSE));
    gtk_paned_set_start_child(GTK_PANED(paned), start);
    gtk_paned_set_end_child(GTK_PANED(paned), end);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), FALSE);
    gtk_widget_set_size_request(start, 560, -1);
    gtk_widget_set_size_request(end, 340, -1);
    state->fuji_count = -1;
    return padded(paned);
}

/* The state outlives the window: the widgets go before the window's
 * "destroy" and can still call back (the notebook's page switches), so it is
 * freed only once the window object itself is gone. */
static void on_win_finalized(gpointer data, GObject *where)
{
    DbgWin *state = data;
    (void)where;
    if (state->history) g_ptr_array_unref(state->history);
    if (state->bp_sig) g_string_free(state->bp_sig, TRUE);
    if (state->wt_labels) g_ptr_array_unref(state->wt_labels);
    if (state->sym_sig) g_string_free(state->sym_sig, TRUE);
    if (state->src_sig) g_string_free(state->src_sig, TRUE);
    g_free(state->src_cached_path);
    g_strfreev(state->src_cached_lines);
    g_free(state->sound_hist);
    g_free(state);
}

static void on_destroy(GtkWidget *widget, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)widget;
    state->closing = TRUE;
    if (state->timer) g_source_remove(state->timer);
    state->timer = 0;
    if (state->src_scroll_id) g_source_remove(state->src_scroll_id);
    state->src_scroll_id = 0;
    if (state->sound_watching) cocodebug_sound_watch(state->dbg, 0);
    state->sound_watching = FALSE;
    cocodebug_set_engaged(state->dbg, 0);
    if (g_win == state) g_win = NULL;
}

static void attach_and_refresh(DbgWin *state)
{
    cocodebug_set_engaged(state->dbg, 1);
    cocodebug_pause(state->dbg);        /* a user opening a debugger expects it stopped */
    settle(state, TRUE);
    state->running_ticks = 0;
    if (state->bp_sig) { g_string_free(state->bp_sig, TRUE); state->bp_sig = NULL; }
    src_invalidate(state);
    refresh_now(state);
}

/* A tab from an environment variable (COCO_DEBUGGER_TAB, COCO_POPOUT): a
 * number, or a tab's name or the start of one, any case. */
static int tab_from_env(const char *var)
{
    static const struct { const char *name; int tab; } aliases[] = {
        { "cpu", TAB_DISASM }, { "disasm", TAB_DISASM }, { "csource", TAB_CSRC },
        { "source", TAB_CSRC }, { "src", TAB_CSRC }, { "cart", TAB_CART }, { "io", TAB_CART },
        { "i/o", TAB_CART }, { "breaks", TAB_BREAKS }, { "mem", TAB_MEMORY },
        { "regs", TAB_CONSOLE },
    };
    const char *tab = g_getenv(var);
    char *end;
    long count;
    unsigned index;
    if (!tab || !*tab) return -1;
    count = strtol(tab, &end, 10);
    if (end != tab && *end == '\0')
        return (count >= 0 && count < (long)G_N_ELEMENTS(k_tab_names)) ? (int)count : -1;
    for (index = 0; index < G_N_ELEMENTS(aliases); index++)
        if (g_ascii_strcasecmp(tab, aliases[index].name) == 0)
            return aliases[index].tab;
    for (index = 0; index < G_N_ELEMENTS(k_tab_names); index++)
        if (g_ascii_strncasecmp(tab, k_tab_names[index], strlen(tab)) == 0)
            return (int)index;
    for (index = 0; index < G_N_ELEMENTS(aliases); index++)
        if (g_ascii_strncasecmp(tab, aliases[index].name, strlen(tab)) == 0)
            return aliases[index].tab;
    return -1;
}

/* "phys:1F400" or "cpu:4000" before a capture variable's text picks the space. */
static const char *env_space(const char *text, int *space)
{
    *space = COCODEBUG_SPACE_CPU;
    if (g_ascii_strncasecmp(text, "phys:", 5) == 0) {
        *space = COCODEBUG_SPACE_PHYS;
        return text + 5;
    }
    if (g_ascii_strncasecmp(text, "cpu:", 4) == 0)
        return text + 4;
    return text;
}

void coco_debugger_show(GtkWindow *parent, cocosession *session)
{
    DbgWin *state;
    GtkWidget *toolbar, *header, *root, *notebook, *overlay;
    GtkEventController *keys;
    cocodebug *dbg;
    int tab, index;

    if (g_win) {
        gboolean shown = gtk_widget_get_visible(GTK_WIDGET(g_win->win));
        gtk_window_present(g_win->win);
        if (!shown) attach_and_refresh(g_win);
        return;
    }
    dbg = cocosession_debugger(session);
    if (!dbg) {
        AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(
            "Debugger Unavailable", "The debugger could not be created for this session."));
        adw_alert_dialog_add_response(dialog, "close", "_Close");
        adw_alert_dialog_set_default_response(dialog, "close");
        adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(parent));
        return;
    }
    install_css();
    state = g_new0(DbgWin, 1);
    state->session = session;
    state->dbg = dbg;
    g_win = state;

    state->win = GTK_WINDOW(adw_window_new());
    g_object_weak_ref(G_OBJECT(state->win), on_win_finalized, state);
    gtk_window_set_title(state->win, "Debugger");
    gtk_window_set_icon_name(state->win, coco_icon_name());
    gtk_window_set_default_size(state->win, 1150, 800);
    gtk_window_set_transient_for(state->win, parent);
    gtk_window_set_application(state->win, gtk_window_get_application(parent));
    gtk_window_set_destroy_with_parent(state->win, TRUE);
    g_signal_connect(state->win, "close-request", G_CALLBACK(on_close), state);
    g_signal_connect(state->win, "destroy", G_CALLBACK(on_destroy), state);

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(root), build_toolbar(state));
    notebook = gtk_notebook_new();
    state->notebook = GTK_NOTEBOOK(notebook);
    gtk_widget_set_vexpand(notebook, TRUE);
    state->pages[TAB_CONSOLE] = build_console(state);
    state->pages[TAB_DISASM] = build_disasm(state);
    state->pages[TAB_CSRC] = build_csrc(state);
    state->pages[TAB_MEMORY] = build_memory(state);
    state->pages[TAB_BREAKS] = build_breaks(state);
    state->pages[TAB_WATCHES] = build_watches(state);
    state->pages[TAB_CART] = build_cart(state);
    state->pages[TAB_TRACE] = build_hist(state, 0);
    state->pages[TAB_HISTORY] = build_hist(state, 1);
    state->pages[TAB_VIDEO] = dbg_video_new(state->dbg);
    state->pages[TAB_SOUND] = build_sound(state);
    state->pages[TAB_SYMBOLS] = build_symbols(state);
    state->pages[TAB_FUJINET] = build_fujinet(state);
    for (index = 0; index < NTABS; index++) {
        g_object_set_data(G_OBJECT(state->pages[index]), "tab", GINT_TO_POINTER(index));
        gtk_notebook_append_page(state->notebook, state->pages[index], gtk_label_new(k_tab_names[index]));
    }
    gtk_box_append(GTK_BOX(root), notebook);
    gtk_box_append(GTK_BOX(root), build_regs(state));
    tab = tab_from_env("COCO_DEBUGGER_TAB");
    if (tab >= 0) gtk_notebook_set_current_page(state->notebook, tab);
    g_signal_connect(notebook, "switch-page", G_CALLBACK(on_switch_page), state);
    coco_help_set_context(state->win, k_tab_contexts[current_tab(state)]);

    overlay = adw_toast_overlay_new();
    state->toasts = ADW_TOAST_OVERLAY(overlay);
    adw_toast_overlay_set_child(state->toasts, root);

    /* the status line -- why the machine stopped -- is the title's subtitle */
    header = adw_header_bar_new();
    state->title = ADW_WINDOW_TITLE(adw_window_title_new("Debugger", ""));
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), GTK_WIDGET(state->title));
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), coco_help_button(NULL));
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), overlay);
    adw_window_set_content(ADW_WINDOW(state->win), toolbar);

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), state);
    gtk_widget_add_controller(GTK_WIDGET(state->win), keys);

    attach_and_refresh(state);      /* before the captures: PHYS exists only once the machine is read */
    /* captures: a watch, a break-on-change, a Go to, recording, a popped-out tab */
    {
        const char *watch_text = g_getenv("COCO_WATCH"), *watch_break = g_getenv("COCO_WATCH_BREAK"),
                   *mem_goto_text = g_getenv("COCO_MEM_GOTO");
        uint32_t first, last;
        int space;
        if (watch_text && *watch_text) {        /* a comma-separated list; "phys:" starts one in PHYS */
            char **entries = g_strsplit(watch_text, ",", -1);
            int entry;
            for (entry = 0; entries[entry]; entry++) {
                const char *text = env_space(g_strstrip(entries[entry]), &space);
                if (*text) cocowatch_add(text, space);
            }
            g_strfreev(entries);
        }
        if (watch_break && *watch_break) {
            const char *text = env_space(watch_break, &space);
            if (cocosym_parse_range(state->dbg, space, text, &first, &last)) break_on_change(state, space, first, last);
        }
        if (mem_goto_text && *mem_goto_text) {
            const char *text = env_space(mem_goto_text, &space);
            if (cocosym_parse_range(state->dbg, space, text, &first, &last)) show_in_memory(state, space, first, last);
        }
        if (g_getenv("COCO_HISTORY")) cocodebug_trace_enable(state->dbg, 1);
    }
    g_signal_connect(state->win, "map", G_CALLBACK(on_win_map), state);
    g_signal_connect(state->win, "unmap", G_CALLBACK(on_win_unmap), state);
    gtk_window_present(state->win);
    if (current_tab(state) == TAB_CONSOLE)
        gtk_widget_grab_focus(GTK_WIDGET(state->console_in));
    tab = tab_from_env("COCO_POPOUT");
    if (tab >= 0) pop_out_tab(state, tab);
}

/* F12 and the menu item: show it, or hide it if it is showing. */
void coco_debugger_toggle(GtkWindow *parent, cocosession *session)
{
    if (g_win && gtk_widget_get_visible(GTK_WIDGET(g_win->win)))
        hide_window(g_win);
    else
        coco_debugger_show(parent, session);
}

