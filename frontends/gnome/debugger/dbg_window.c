/*
 * The 6809 debugger window for the GNOME frontend.
 *
 * Drives core/include/cocodebug.h and nothing else: the engine owns every
 * decision about when the machine stops, and this only asks and displays.
 *
 * Refreshing is a POLL, not a callback. The engine stops the machine on the
 * emulator thread, deep inside XRoar, and calling back into GTK from there
 * would be a threading bug waiting to happen. Instead the window watches
 * cocodebug_stop_serial() on its own frame clock and rebuilds when it
 * changes, which costs one integer read per frame while open and nothing at
 * all when closed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "debugger/dbg_window.h"

#include <stdlib.h>
#include <string.h>

#include "cocodebug.h"
#include "cocovideo.h"

/* How much disassembly to show. The list is rebuilt on every stop, so this is
 * a screenful and a bit rather than a scrollback. */
#define DISASM_LINES 24

/* The memory pane: a fixed-size hex dump rebuilt from mem_view_addr on every
 * stop, the same way the disassembly list is. */
#define MEM_BYTES_PER_ROW 16
#define MEM_ROWS 16

typedef struct {
    GtkWindow *window;
    cocosession *session;
    cocodebug *dbg;

    GtkLabel *status;
    GtkListBox *disasm;
    GtkLabel *regs;
    GtkLabel *locals;
    GtkPicture *video;
    GtkLabel *mem;
    GtkEntry *mem_addr;
    GtkButton *run_pause;
    GtkButton *step_line;
    GtkButton *step_over_line;

    /* The C source pane: a file picker over cocodebug_source_file_path()'s
     * table, and the listing of whichever one is selected. */
    GtkDropDown *src_file_combo;
    GtkListBox *src_list;
    char *src_cached_path;    /* which file src_cached_lines holds */
    char **src_cached_lines;  /* g_strsplit() result, NULL-terminated */
    int src_displayed_file_id;
    /* Where PC actually is, independent of which file the combo is showing
     * (a user may be browsing a different file to set a breakpoint ahead of
     * time): the '>' marker only appears when these match the displayed
     * file. */
    int pc_file_id, pc_line;

    /* Heap-allocated: COCOVIDEO_MAX_WIDTH*HEIGHT is too big for a GObject
     * instance, same reasoning as CocoDisplay's own frame buffer. */
    uint32_t *video_buf;

    guint tick_id;
    uint64_t seen_serial;
    int was_paused;
    uint16_t mem_view_addr;
} DebuggerUI;

/* The core's XRGB8888 is a uint32 0x00RRGGBB, so on a little-endian host the
 * bytes run B,G,R,X -- GDK_MEMORY_B8G8R8X8, no per-pixel conversion. Same
 * format CocoDisplay uses (frontends/gnome/display.c) for the same reason. */
#if G_BYTE_ORDER == G_BIG_ENDIAN
#define COCO_GDK_FORMAT GDK_MEMORY_X8R8G8B8
#else
#define COCO_GDK_FORMAT GDK_MEMORY_B8G8R8X8
#endif

/* One live window at a time: a second one would fight the first over the same
 * engine, and there is nothing useful to see twice. */
static DebuggerUI *g_ui;

/* ---- painting ----------------------------------------------------------- */

static void refresh_regs(DebuggerUI *ui)
{
    cocodebug_regs r;
    g_autofree char *text = NULL;

    if (!cocodebug_regs_get(ui->dbg, &r)) {
        gtk_label_set_text(ui->regs, "");
        return;
    }
    /* CC is the one register whose bits matter more than its value, so spell
     * them out: Entire, FIRQ mask, Half carry, IRQ mask, Negative, Zero,
     * oVerflow, Carry. */
    text = g_strdup_printf(
        "PC  %04X      A %02X  B %02X\n"
        "X   %04X      DP %02X\n"
        "Y   %04X\n"
        "U   %04X\n"
        "S   %04X\n"
        "\n"
        "CC  %02X  %c%c%c%c%c%c%c%c",
        r.pc, r.a, r.b, r.x, r.dp, r.y, r.u, r.s, r.cc,
        (r.cc & 0x80) ? 'E' : '-', (r.cc & 0x40) ? 'F' : '-',
        (r.cc & 0x20) ? 'H' : '-', (r.cc & 0x10) ? 'I' : '-',
        (r.cc & 0x08) ? 'N' : '-', (r.cc & 0x04) ? 'Z' : '-',
        (r.cc & 0x02) ? 'V' : '-', (r.cc & 0x01) ? 'C' : '-');
    gtk_label_set_text(ui->regs, text);
}

static void refresh_locals(DebuggerUI *ui)
{
    cocodebug_local locals[32];
    int n, i;
    GString *text;

    if (!cocodebug_source_available(ui->dbg)) {
        gtk_label_set_text(ui->locals, "");
        return;
    }
    n = cocodebug_locals_get(ui->dbg, locals, 32);
    if (n == 0) {
        /* Not necessarily an error: PC may just be before the function's
         * first statement (mid-prologue), where there is nothing correct to
         * show yet. */
        gtk_label_set_text(ui->locals, "(no locals in scope)");
        return;
    }
    text = g_string_new(NULL);
    for (i = 0; i < n; i++) {
        const cocodebug_local *v = &locals[i];
        g_string_append_printf(text, "%-12s %-10s %s= ", v->name, v->type,
                               v->is_param ? "(param) " : "");
        if (v->has_value)
            g_string_append_printf(text, "%u ($%X)\n", v->value, v->value);
        else
            g_string_append(text, "?\n");
    }
    if (text->len > 0 && text->str[text->len - 1] == '\n')
        g_string_truncate(text, text->len - 1);
    gtk_label_set_text(ui->locals, text->str);
    g_string_free(text, TRUE);
}

static void on_row_activated(GtkListBox *box, GtkListBoxRow *row,
                             gpointer user_data)
{
    DebuggerUI *ui = user_data;
    gpointer addr;
    (void)box;

    /* Clicking a line toggles a breakpoint on it -- the one interaction the
     * disassembly list has. */
    addr = g_object_get_data(G_OBJECT(row), "coco-addr");
    cocodebug_breakpoint_toggle(ui->dbg, (uint16_t)GPOINTER_TO_UINT(addr));
    /* Rebuild so the marker appears immediately: the machine is paused, so
     * nothing else is going to prompt a refresh. */
    ui->seen_serial = 0;
}

static void refresh_disasm(DebuggerUI *ui)
{
    coco_dasm_line lines[DISASM_LINES];
    cocodebug_regs r;
    int n, i;

    gtk_list_box_remove_all(ui->disasm);
    if (!cocodebug_regs_get(ui->dbg, &r))
        return;

    n = cocodebug_disassemble(ui->dbg, r.pc, lines, DISASM_LINES);
    for (i = 0; i < n; i++) {
        const coco_dasm_line *l = &lines[i];
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *label = gtk_label_new(NULL);
        g_autofree char *bytes = g_strdup("");
        g_autofree char *text = NULL;
        char symname[32];
        g_autofree char *symsuffix = NULL;
        int b;

        for (b = 0; b < l->len; b++) {
            g_autofree char *prev = bytes;
            bytes = g_strdup_printf("%s%02X", prev, l->bytes[b]);
        }

        symsuffix = cocodebug_symbol_at(ui->dbg, l->addr, symname, sizeof(symname))
                       ? g_strdup_printf("   ; %s", symname) : g_strdup("");

        /* ">" is where the CPU is; "*" is a breakpoint. Both in the same
         * column so the addresses stay aligned. */
        text = g_strdup_printf("%c%c %04X  %-10s %s%s",
                               (l->addr == r.pc) ? '>' : ' ',
                               cocodebug_breakpoint_is_set(ui->dbg, l->addr)
                                   ? '*' : ' ',
                               l->addr, bytes, l->text, symsuffix);
        gtk_label_set_text(GTK_LABEL(label), text);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_widget_add_css_class(label, "monospace");
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
        g_object_set_data(G_OBJECT(row), "coco-addr",
                          GUINT_TO_POINTER((guint)l->addr));
        gtk_list_box_append(ui->disasm, row);
    }
}

static void on_src_row_activated(GtkListBox *box, GtkListBoxRow *row,
                                 gpointer user_data)
{
    DebuggerUI *ui = user_data;
    gpointer lineno;
    (void)box;

    lineno = g_object_get_data(G_OBJECT(row), "coco-line");
    cocodebug_source_breakpoint_toggle(ui->dbg, ui->src_displayed_file_id,
                                       GPOINTER_TO_INT(lineno));
    ui->seen_serial = 0;
}

/* Loads (and caches) file_id's text, then redraws the list with '>' on the
 * line PC is at (if PC's file is the one showing) and '*' on any line
 * carrying a breakpoint. Re-reading the file is cheap enough here since this
 * only runs on an actual stop or a manual file switch, never on every tick
 * -- see tick_cb's early-out. */
static void populate_src_list(DebuggerUI *ui, int file_id)
{
    const char *path = cocodebug_source_file_path(ui->dbg, file_id);
    int i;

    ui->src_displayed_file_id = file_id;

    if (g_strcmp0(path, ui->src_cached_path) != 0) {
        g_autofree char *contents = NULL;
        gsize len = 0;

        g_clear_pointer(&ui->src_cached_lines, g_strfreev);
        g_free(ui->src_cached_path);
        ui->src_cached_path = g_strdup(path);

        if (path && g_file_get_contents(path, &contents, &len, NULL))
            ui->src_cached_lines = g_strsplit(contents, "\n", -1);
        else
            ui->src_cached_lines = g_new0(char *, 1); /* empty, NUL-terminated */
    }

    gtk_list_box_remove_all(ui->src_list);
    for (i = 0; ui->src_cached_lines[i]; i++) {
        int lineno = i + 1;
        int is_pc = (file_id == ui->pc_file_id && lineno == ui->pc_line);
        int has_bp = cocodebug_source_breakpoint_is_set(ui->dbg, file_id,
                                                        lineno);
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *label = gtk_label_new(NULL);
        g_autofree char *text =
            g_strdup_printf("%c%c %5d  %s", is_pc ? '>' : ' ',
                            has_bp ? '*' : ' ', lineno,
                            ui->src_cached_lines[i]);

        gtk_label_set_text(GTK_LABEL(label), text);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_widget_add_css_class(label, "monospace");
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
        g_object_set_data(G_OBJECT(row), "coco-line", GINT_TO_POINTER(lineno));
        gtk_list_box_append(ui->src_list, row);
        /* Grabbing focus scrolls it into view in any ancestor
         * GtkScrolledWindow -- the disassembly pane needs no equivalent
         * because it always starts its listing AT pc, but a source file's
         * current line can be anywhere in it. */
        if (is_pc)
            gtk_widget_grab_focus(row);
    }
}

static void refresh_source(DebuggerUI *ui)
{
    cocodebug_regs r;
    int fid, line;

    if (!cocodebug_source_available(ui->dbg)) {
        gtk_list_box_remove_all(ui->src_list);
        return;
    }
    if (!cocodebug_regs_get(ui->dbg, &r))
        return;

    if (cocodebug_source_line_at(ui->dbg, r.pc, &fid, &line)) {
        ui->pc_file_id = fid;
        ui->pc_line = line;
        if ((guint)fid != gtk_drop_down_get_selected(ui->src_file_combo))
            /* Fires notify::selected, which repopulates the list. */
            gtk_drop_down_set_selected(ui->src_file_combo, (guint)fid);
        else
            populate_src_list(ui, ui->src_displayed_file_id);
    } else {
        /* PC is not itself a mapped statement (mid-prologue, or code with no
         * line info): keep showing whatever file was last selected and just
         * drop the '>' marker rather than guess. */
        ui->pc_file_id = -1;
        ui->pc_line = -1;
        if (gtk_drop_down_get_model(ui->src_file_combo))
            populate_src_list(ui, ui->src_displayed_file_id);
    }
}

static void refresh_mem(DebuggerUI *ui)
{
    uint8_t buf[MEM_BYTES_PER_ROW * MEM_ROWS];
    GString *text;
    int row, col, n;

    n = cocodebug_read(ui->dbg, ui->mem_view_addr, buf, sizeof(buf));
    if (n <= 0) {
        gtk_label_set_text(ui->mem, "");
        return;
    }

    text = g_string_new(NULL);
    for (row = 0; row * MEM_BYTES_PER_ROW < n; row++) {
        uint16_t addr = (uint16_t)(ui->mem_view_addr + row * MEM_BYTES_PER_ROW);

        g_string_append_printf(text, "%04X ", addr);
        for (col = 0; col < MEM_BYTES_PER_ROW; col++) {
            int idx = row * MEM_BYTES_PER_ROW + col;
            if (idx < n)
                g_string_append_printf(text, " %02X", buf[idx]);
            else
                g_string_append(text, "   ");
        }
        g_string_append(text, "  ");
        for (col = 0; col < MEM_BYTES_PER_ROW; col++) {
            int idx = row * MEM_BYTES_PER_ROW + col;
            uint8_t b = (idx < n) ? buf[idx] : (uint8_t)' ';
            g_string_append_c(text, (b >= 0x20 && b < 0x7f) ? (char)b : '.');
        }
        if ((row + 1) * MEM_BYTES_PER_ROW < n)
            g_string_append_c(text, '\n');
    }
    gtk_label_set_text(ui->mem, text->str);
    g_string_free(text, TRUE);
}

static void refresh_video(DebuggerUI *ui)
{
    int w = 0, h = 0;
    GBytes *bytes;
    GdkTexture *texture;

    if (!cocodebug_video_decode(ui->dbg, ui->video_buf, &w, &h) ||
        w <= 0 || h <= 0) {
        gtk_picture_set_paintable(ui->video, NULL);
        return;
    }

    bytes = g_bytes_new(ui->video_buf, (gsize)w * (gsize)h * sizeof(uint32_t));
    texture = gdk_memory_texture_new(w, h, COCO_GDK_FORMAT, bytes,
                                     (gsize)w * sizeof(uint32_t));
    g_bytes_unref(bytes);
    gtk_picture_set_paintable(ui->video, GDK_PAINTABLE(texture));
    g_object_unref(texture);
}

static void refresh(DebuggerUI *ui)
{
    const int paused = cocodebug_is_paused(ui->dbg);
    const int src_avail = cocodebug_source_available(ui->dbg);

    gtk_button_set_label(ui->run_pause, paused ? "_Run" : "_Pause");
    gtk_label_set_text(ui->status, paused ? "Paused" : "Running");
    gtk_widget_set_sensitive(GTK_WIDGET(ui->step_line), src_avail);
    gtk_widget_set_sensitive(GTK_WIDGET(ui->step_over_line), src_avail);

    if (paused) {
        refresh_regs(ui);
        refresh_disasm(ui);
        refresh_source(ui);
        refresh_locals(ui);
        refresh_mem(ui);
        refresh_video(ui);
    } else {
        /* Everything on show is only valid while parked, so clear it rather
         * than leave a stale snapshot that looks live. */
        gtk_label_set_text(ui->regs, "");
        gtk_label_set_text(ui->locals, "");
        gtk_list_box_remove_all(ui->disasm);
        gtk_list_box_remove_all(ui->src_list);
        gtk_label_set_text(ui->mem, "");
        gtk_picture_set_paintable(ui->video, NULL);
    }
    ui->was_paused = paused;
}

static gboolean tick_cb(GtkWidget *widget, GdkFrameClock *clock,
                        gpointer user_data)
{
    DebuggerUI *ui = user_data;
    uint64_t serial;
    (void)widget;
    (void)clock;

    if (!ui->dbg)
        return G_SOURCE_CONTINUE;

    serial = cocodebug_stop_serial(ui->dbg);
    /* Rebuild when the engine has stopped somewhere new, or when it has just
     * started running again and the panes need clearing. */
    if (serial != ui->seen_serial ||
        cocodebug_is_paused(ui->dbg) != ui->was_paused) {
        ui->seen_serial = serial;
        refresh(ui);
    }
    return G_SOURCE_CONTINUE;
}

/* ---- actions ------------------------------------------------------------ */

static void on_run_pause(GtkButton *button, gpointer user_data)
{
    DebuggerUI *ui = user_data;
    (void)button;
    if (cocodebug_is_paused(ui->dbg))
        cocodebug_resume(ui->dbg);
    else
        cocodebug_pause(ui->dbg);
}

static void on_step(GtkButton *b, gpointer u)
{ (void)b; cocodebug_step(((DebuggerUI *)u)->dbg); }
static void on_step_over(GtkButton *b, gpointer u)
{ (void)b; cocodebug_step_over(((DebuggerUI *)u)->dbg); }
static void on_step_out(GtkButton *b, gpointer u)
{ (void)b; cocodebug_step_out(((DebuggerUI *)u)->dbg); }
static void on_step_line(GtkButton *b, gpointer u)
{ (void)b; cocodebug_step_line(((DebuggerUI *)u)->dbg); }
static void on_step_over_line(GtkButton *b, gpointer u)
{ (void)b; cocodebug_step_over_line(((DebuggerUI *)u)->dbg); }
static void on_clear_bp(GtkButton *b, gpointer u)
{
    DebuggerUI *ui = u;
    (void)b;
    cocodebug_breakpoint_clear_all(ui->dbg);
    ui->seen_serial = 0; /* redraw the markers */
}

static void load_symbols_done(GObject *source, GAsyncResult *result,
                              gpointer user_data)
{
    DebuggerUI *ui = user_data;
    g_autoptr(GFile) file =
        gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
    g_autofree char *path = NULL;
    int n;

    if (!file)
        return;
    path = g_file_get_path(file);
    if (!path)
        return;
    n = cocodebug_symbols_load(ui->dbg, path);
    if (n < 0) {
        gtk_label_set_text(ui->status, "Could not open symbol file");
        return;
    }
    ui->seen_serial = 0; /* redraw the disassembly with the new labels */
}

static void on_load_symbols(GtkButton *button, gpointer user_data)
{
    DebuggerUI *ui = user_data;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    GtkFileFilter *filter = gtk_file_filter_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    (void)button;

    /* LWASM's --symbol-dump has no fixed convention, but .sym is what people
     * actually name these; the dialog still shows every file if asked. */
    gtk_file_filter_set_name(filter, "Symbol files");
    gtk_file_filter_add_suffix(filter, "sym");
    gtk_file_filter_add_pattern(filter, "*");
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_title(dialog, "Load Symbols");
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_open(dialog, ui->window, NULL, load_symbols_done, ui);
    g_object_unref(filters);
    g_object_unref(dialog);
}

static void on_clear_symbols(GtkButton *b, gpointer u)
{
    DebuggerUI *ui = u;
    (void)b;
    cocodebug_symbols_clear(ui->dbg);
    ui->seen_serial = 0;
}

static void on_src_file_changed(GObject *obj, GParamSpec *pspec,
                                gpointer user_data)
{
    DebuggerUI *ui = user_data;
    guint idx;
    (void)pspec;

    if (!ui->dbg)
        return;
    idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
    if (idx == GTK_INVALID_LIST_POSITION)
        return;
    populate_src_list(ui, (int)idx);
}

/* Rebuilds the file picker from cocodebug_source_file_count()/_file_path().
 * Selection index and file_id are the same number: files are appended here
 * in exactly the id order the engine hands them out (0..count-1), so no
 * separate index<->id table is needed the way Qt's itemData would give one
 * for free. */
static void rebuild_src_combo(DebuggerUI *ui)
{
    GtkStringList *model = gtk_string_list_new(NULL);
    int n = cocodebug_source_file_count(ui->dbg);
    int i;

    for (i = 0; i < n; i++) {
        const char *path = cocodebug_source_file_path(ui->dbg, i);
        const char *base = path ? strrchr(path, '/') : NULL;
        gtk_string_list_append(model, base ? base + 1 : (path ? path : ""));
    }
    gtk_drop_down_set_model(ui->src_file_combo, G_LIST_MODEL(model));
    g_object_unref(model);
    gtk_widget_set_sensitive(GTK_WIDGET(ui->src_file_combo), n > 0);
    if (n > 0)
        /* Explicit rather than relying on notify::selected: GtkDropDown may
         * already default to index 0 on a fresh model, in which case
         * setting it again would not fire the signal. */
        populate_src_list(ui, 0);
    else
        gtk_list_box_remove_all(ui->src_list);
}

static void load_source_done(GObject *source, GAsyncResult *result,
                             gpointer user_data)
{
    DebuggerUI *ui = user_data;
    g_autoptr(GFile) file =
        gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
    g_autofree char *path = NULL;
    int n;

    if (!file)
        return;
    path = g_file_get_path(file);
    if (!path)
        return;
    n = cocodebug_source_load(ui->dbg, path);
    if (n < 0) {
        gtk_label_set_text(
            ui->status,
            "Could not open listing or its .map (both need `cmoc -i`)");
        return;
    }
    g_clear_pointer(&ui->src_cached_path, g_free);
    g_clear_pointer(&ui->src_cached_lines, g_strfreev);
    rebuild_src_combo(ui);
    ui->seen_serial = 0;
}

static void on_load_source(GtkButton *button, gpointer user_data)
{
    DebuggerUI *ui = user_data;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    GtkFileFilter *filter = gtk_file_filter_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    (void)button;

    gtk_file_filter_set_name(filter, "CMOC listings");
    gtk_file_filter_add_suffix(filter, "lst");
    gtk_file_filter_add_pattern(filter, "*");
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_title(dialog, "Load C Source");
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_open(dialog, ui->window, NULL, load_source_done, ui);
    g_object_unref(filters);
    g_object_unref(dialog);
}

static void on_clear_source(GtkButton *b, gpointer u)
{
    DebuggerUI *ui = u;
    (void)b;
    cocodebug_source_clear(ui->dbg);
    g_clear_pointer(&ui->src_cached_path, g_free);
    g_clear_pointer(&ui->src_cached_lines, g_strfreev);
    ui->pc_file_id = -1;
    ui->src_displayed_file_id = -1;
    gtk_drop_down_set_model(ui->src_file_combo, NULL);
    gtk_widget_set_sensitive(GTK_WIDGET(ui->src_file_combo), FALSE);
    gtk_list_box_remove_all(ui->src_list);
    ui->seen_serial = 0;
}

static void go_to_mem_addr(DebuggerUI *ui)
{
    const char *text = gtk_editable_get_text(GTK_EDITABLE(ui->mem_addr));
    char *end;
    unsigned long v = strtoul(text, &end, 16);

    if (end != text)
        ui->mem_view_addr = (uint16_t)v;
    ui->seen_serial = 0; /* force a redraw at the new address */
}

static void on_mem_go(GtkButton *button, gpointer user_data)
{ (void)button; go_to_mem_addr(user_data); }
static void on_mem_addr_activate(GtkEntry *entry, gpointer user_data)
{ (void)entry; go_to_mem_addr(user_data); }

static gboolean on_key(GtkEventControllerKey *c, guint keyval, guint code,
                       GdkModifierType state, gpointer user_data)
{
    DebuggerUI *ui = user_data;
    (void)c;
    (void)code;

    switch (keyval) {
    case GDK_KEY_F5:
        on_run_pause(NULL, ui);
        return TRUE;
    case GDK_KEY_F6:
        if (state & GDK_SHIFT_MASK)
            cocodebug_step_over_line(ui->dbg);
        else
            cocodebug_step_line(ui->dbg);
        return TRUE;
    case GDK_KEY_F7:
        cocodebug_step(ui->dbg);
        return TRUE;
    case GDK_KEY_F8:
        if (state & GDK_SHIFT_MASK)
            cocodebug_step_out(ui->dbg);
        else
            cocodebug_step_over(ui->dbg);
        return TRUE;
    default:
        return FALSE;
    }
}

static void on_closed(GtkWindow *window, gpointer user_data)
{
    DebuggerUI *ui = user_data;
    (void)window;

    if (ui->tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(ui->window), ui->tick_id);
        ui->tick_id = 0;
    }
    /* Closing the window releases the debugger: leaving it engaged would cost
     * the machine a hook call per instruction forever, and leaving it PAUSED
     * would look like the emulator had hung. */
    cocodebug_set_engaged(ui->dbg, 0);
    g_ui = NULL;
    g_free(ui->video_buf);
    g_free(ui->src_cached_path);
    g_strfreev(ui->src_cached_lines);
    g_free(ui);
}

/* ---- construction ------------------------------------------------------- */

static GtkWidget *tool_button(const char *label, const char *tooltip,
                              GCallback cb, DebuggerUI *ui)
{
    GtkWidget *b = gtk_button_new_with_mnemonic(label);
    gtk_widget_set_tooltip_text(b, tooltip);
    g_signal_connect(b, "clicked", cb, ui);
    return b;
}

void coco_debugger_show(GtkWindow *parent, cocosession *session)
{
    DebuggerUI *ui;
    cocodebug *dbg;
    GtkWidget *box, *panes, *scroller, *regbox, *header, *bar, *mem_bar,
              *mem_scroller, *src_box, *src_scroller;
    GtkEventController *keys;

    if (g_ui) {
        gtk_window_present(g_ui->window);
        return;
    }

    dbg = cocosession_debugger(session);
    if (!dbg) {
        AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(
            "Debugger Unavailable",
            "The debugger could not be created for this session."));
        adw_alert_dialog_add_response(dialog, "close", "_Close");
        adw_alert_dialog_set_default_response(dialog, "close");
        adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(parent));
        return;
    }

    ui = g_new0(DebuggerUI, 1);
    ui->session = session;
    ui->dbg = dbg;
    ui->pc_file_id = -1;
    ui->src_displayed_file_id = -1;
    ui->src_cached_lines = g_new0(char *, 1); /* empty, NUL-terminated */
    ui->video_buf = g_malloc0((gsize)COCOVIDEO_MAX_WIDTH *
                              COCOVIDEO_MAX_HEIGHT * sizeof(uint32_t));
    g_ui = ui;

    ui->window = GTK_WINDOW(adw_window_new());
    gtk_window_set_title(ui->window, "Debugger");
    gtk_window_set_default_size(ui->window, 980, 720);
    gtk_window_set_transient_for(ui->window, parent);

    header = adw_header_bar_new();
    ui->status = GTK_LABEL(gtk_label_new("Running"));
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header),
                                    GTK_WIDGET(ui->status));

    bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(bar, 6);
    gtk_widget_set_margin_bottom(bar, 6);
    gtk_widget_set_margin_start(bar, 6);
    gtk_widget_set_margin_end(bar, 6);
    ui->run_pause = GTK_BUTTON(tool_button("_Pause", "Run or pause (F5)",
                                           G_CALLBACK(on_run_pause), ui));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(ui->run_pause));
    gtk_box_append(GTK_BOX(bar),
                   tool_button("_Step", "One instruction (F7)",
                               G_CALLBACK(on_step), ui));
    gtk_box_append(GTK_BOX(bar),
                   tool_button("Step _Over", "Over a subroutine call (F8)",
                               G_CALLBACK(on_step_over), ui));
    gtk_box_append(GTK_BOX(bar),
                   tool_button("Step Ou_t", "Until this routine returns "
                                            "(Shift+F8)",
                               G_CALLBACK(on_step_out), ui));
    ui->step_line = GTK_BUTTON(tool_button(
        "Step L_ine",
        "Run until a different C source line (F6). Needs C source info "
        "loaded.",
        G_CALLBACK(on_step_line), ui));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(ui->step_line));
    ui->step_over_line = GTK_BUTTON(tool_button(
        "Step Over Lin_e",
        "Like Step Line, but a call on this line runs to completion rather "
        "than being stepped into (Shift+F6).",
        G_CALLBACK(on_step_over_line), ui));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(ui->step_over_line));
    gtk_box_append(GTK_BOX(bar),
                   tool_button("_Clear Breakpoints",
                               "Remove every breakpoint",
                               G_CALLBACK(on_clear_bp), ui));
    gtk_box_append(GTK_BOX(bar),
                   tool_button("Load _Symbols…",
                               "Load a symbol file (LWASM --symbol-dump, or "
                               "plain \"NAME ADDR\" lines) to label the "
                               "disassembly",
                               G_CALLBACK(on_load_symbols), ui));
    gtk_box_append(GTK_BOX(bar),
                   tool_button("Clear S_ymbols",
                               "Drop loaded symbols, keeping the built-in "
                               "Color BASIC entry points",
                               G_CALLBACK(on_clear_symbols), ui));
    gtk_box_append(GTK_BOX(bar),
                   tool_button("Load C _Source…",
                               "Load a CMOC listing (.lst, from `cmoc -i`) "
                               "for source-level stepping and locals. Its "
                               "sibling .map (also only kept by -i) is "
                               "found automatically.",
                               G_CALLBACK(on_load_source), ui));
    gtk_box_append(GTK_BOX(bar),
                   tool_button("Clear C So_urce",
                               "Drop loaded C source info",
                               G_CALLBACK(on_clear_source), ui));

    ui->disasm = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(ui->disasm, GTK_SELECTION_NONE);
    g_signal_connect(ui->disasm, "row-activated",
                     G_CALLBACK(on_row_activated), ui);
    scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller),
                                  GTK_WIDGET(ui->disasm));
    gtk_widget_set_hexpand(scroller, TRUE);
    gtk_widget_set_vexpand(scroller, TRUE);

    ui->src_file_combo = GTK_DROP_DOWN(gtk_drop_down_new(NULL, NULL));
    gtk_widget_set_sensitive(GTK_WIDGET(ui->src_file_combo), FALSE);
    g_signal_connect(ui->src_file_combo, "notify::selected",
                     G_CALLBACK(on_src_file_changed), ui);
    ui->src_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(ui->src_list, GTK_SELECTION_NONE);
    g_signal_connect(ui->src_list, "row-activated",
                     G_CALLBACK(on_src_row_activated), ui);
    src_scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(src_scroller),
                                  GTK_WIDGET(ui->src_list));
    gtk_widget_set_hexpand(src_scroller, TRUE);
    gtk_widget_set_vexpand(src_scroller, TRUE);
    src_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(src_box), GTK_WIDGET(ui->src_file_combo));
    gtk_box_append(GTK_BOX(src_box), src_scroller);
    gtk_widget_set_hexpand(src_box, TRUE);

    ui->regs = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(ui->regs, 0.0f);
    gtk_label_set_yalign(ui->regs, 0.0f);
    gtk_widget_add_css_class(GTK_WIDGET(ui->regs), "monospace");
    regbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(regbox, 12);
    gtk_widget_set_margin_end(regbox, 12);
    gtk_widget_set_margin_top(regbox, 6);
    gtk_box_append(GTK_BOX(regbox), GTK_WIDGET(ui->regs));

    ui->locals = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(ui->locals, 0.0f);
    gtk_label_set_yalign(ui->locals, 0.0f);
    gtk_widget_add_css_class(GTK_WIDGET(ui->locals), "monospace");
    gtk_box_append(GTK_BOX(regbox), GTK_WIDGET(ui->locals));

    ui->video = GTK_PICTURE(gtk_picture_new());
    /* CONTAIN: letterbox rather than distort -- modes vary in aspect (a
     * CoCo 1/2 canvas is fixed 256x192, a CoCo 3 GIME native mode can be
     * much wider), and squashing either to fill the pane would misrepresent
     * what is actually on screen. */
    gtk_picture_set_content_fit(ui->video, GTK_CONTENT_FIT_CONTAIN);
    gtk_picture_set_can_shrink(ui->video, TRUE);
    gtk_widget_set_size_request(GTK_WIDGET(ui->video), 256, 192);
    gtk_widget_set_vexpand(GTK_WIDGET(ui->video), TRUE);
    gtk_box_append(GTK_BOX(regbox), GTK_WIDGET(ui->video));

    panes = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(panes), scroller);
    gtk_box_append(GTK_BOX(panes), src_box);
    gtk_box_append(GTK_BOX(panes), regbox);

    mem_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(mem_bar, 6);
    gtk_widget_set_margin_bottom(mem_bar, 6);
    gtk_widget_set_margin_start(mem_bar, 6);
    gtk_widget_set_margin_end(mem_bar, 6);
    gtk_box_append(GTK_BOX(mem_bar), gtk_label_new("Address:"));
    ui->mem_addr = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_text(GTK_EDITABLE(ui->mem_addr), "0000");
    gtk_editable_set_width_chars(GTK_EDITABLE(ui->mem_addr), 6);
    g_signal_connect(ui->mem_addr, "activate",
                     G_CALLBACK(on_mem_addr_activate), ui);
    gtk_box_append(GTK_BOX(mem_bar), GTK_WIDGET(ui->mem_addr));
    gtk_box_append(GTK_BOX(mem_bar),
                   tool_button("_Go", "Jump to this address",
                               G_CALLBACK(on_mem_go), ui));

    ui->mem = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(ui->mem, 0.0f);
    gtk_label_set_yalign(ui->mem, 0.0f);
    gtk_widget_add_css_class(GTK_WIDGET(ui->mem), "monospace");
    mem_scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(mem_scroller),
                                  GTK_WIDGET(ui->mem));
    /* No vexpand: the disasm pane above soaks up extra space instead. This
     * just needs to be tall enough to show all MEM_ROWS without the box
     * layout shrinking it to nothing. */
    gtk_scrolled_window_set_min_content_height(
        GTK_SCROLLED_WINDOW(mem_scroller), 220);
    gtk_widget_set_margin_start(mem_scroller, 6);
    gtk_widget_set_margin_end(mem_scroller, 6);
    gtk_widget_set_margin_bottom(mem_scroller, 6);
    gtk_widget_set_hexpand(mem_scroller, TRUE);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(box), header);
    gtk_box_append(GTK_BOX(box), bar);
    gtk_box_append(GTK_BOX(box), panes);
    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(box), mem_bar);
    gtk_box_append(GTK_BOX(box), mem_scroller);
    adw_window_set_content(ADW_WINDOW(ui->window), box);

    keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), ui);
    gtk_widget_add_controller(GTK_WIDGET(ui->window), keys);
    g_signal_connect(ui->window, "close-request", G_CALLBACK(on_closed), ui);

    /* Engaging switches the emulator loop to the path that honours
     * breakpoints. Pausing immediately is what a user opening a debugger
     * expects -- otherwise the window opens showing nothing at all. */
    cocodebug_set_engaged(dbg, 1);
    cocodebug_pause(dbg);

    ui->tick_id = gtk_widget_add_tick_callback(GTK_WIDGET(ui->window),
                                               tick_cb, ui, NULL);
    gtk_window_present(ui->window);
}
