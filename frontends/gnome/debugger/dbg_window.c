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

#include "cocodebug.h"

/* How much disassembly to show. The list is rebuilt on every stop, so this is
 * a screenful and a bit rather than a scrollback. */
#define DISASM_LINES 24

typedef struct {
    GtkWindow *window;
    cocosession *session;
    cocodebug *dbg;

    GtkLabel *status;
    GtkListBox *disasm;
    GtkLabel *regs;
    GtkButton *run_pause;

    guint tick_id;
    uint64_t seen_serial;
    int was_paused;
} DebuggerUI;

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
        int b;

        for (b = 0; b < l->len; b++) {
            g_autofree char *prev = bytes;
            bytes = g_strdup_printf("%s%02X", prev, l->bytes[b]);
        }

        /* ">" is where the CPU is; "*" is a breakpoint. Both in the same
         * column so the addresses stay aligned. */
        text = g_strdup_printf("%c%c %04X  %-10s %s",
                               (l->addr == r.pc) ? '>' : ' ',
                               cocodebug_breakpoint_is_set(ui->dbg, l->addr)
                                   ? '*' : ' ',
                               l->addr, bytes, l->text);
        gtk_label_set_text(GTK_LABEL(label), text);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_widget_add_css_class(label, "monospace");
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
        g_object_set_data(G_OBJECT(row), "coco-addr",
                          GUINT_TO_POINTER((guint)l->addr));
        gtk_list_box_append(ui->disasm, row);
    }
}

static void refresh(DebuggerUI *ui)
{
    const int paused = cocodebug_is_paused(ui->dbg);

    gtk_button_set_label(ui->run_pause, paused ? "_Run" : "_Pause");
    gtk_label_set_text(ui->status, paused ? "Paused" : "Running");

    if (paused) {
        refresh_regs(ui);
        refresh_disasm(ui);
    } else {
        /* Everything on show is only valid while parked, so clear it rather
         * than leave a stale snapshot that looks live. */
        gtk_label_set_text(ui->regs, "");
        gtk_list_box_remove_all(ui->disasm);
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
static void on_clear_bp(GtkButton *b, gpointer u)
{
    DebuggerUI *ui = u;
    (void)b;
    cocodebug_breakpoint_clear_all(ui->dbg);
    ui->seen_serial = 0; /* redraw the markers */
}

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
    GtkWidget *box, *panes, *scroller, *regbox, *header, *bar;
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
    g_ui = ui;

    ui->window = GTK_WINDOW(adw_window_new());
    gtk_window_set_title(ui->window, "Debugger");
    gtk_window_set_default_size(ui->window, 640, 560);
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
    gtk_box_append(GTK_BOX(bar),
                   tool_button("_Clear Breakpoints",
                               "Remove every breakpoint",
                               G_CALLBACK(on_clear_bp), ui));

    ui->disasm = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(ui->disasm, GTK_SELECTION_NONE);
    g_signal_connect(ui->disasm, "row-activated",
                     G_CALLBACK(on_row_activated), ui);
    scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller),
                                  GTK_WIDGET(ui->disasm));
    gtk_widget_set_hexpand(scroller, TRUE);
    gtk_widget_set_vexpand(scroller, TRUE);

    ui->regs = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(ui->regs, 0.0f);
    gtk_label_set_yalign(ui->regs, 0.0f);
    gtk_widget_add_css_class(GTK_WIDGET(ui->regs), "monospace");
    regbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(regbox, 12);
    gtk_widget_set_margin_end(regbox, 12);
    gtk_widget_set_margin_top(regbox, 6);
    gtk_box_append(GTK_BOX(regbox), GTK_WIDGET(ui->regs));

    panes = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(panes), scroller);
    gtk_box_append(GTK_BOX(panes), regbox);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(box), header);
    gtk_box_append(GTK_BOX(box), bar);
    gtk_box_append(GTK_BOX(box), panes);
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
