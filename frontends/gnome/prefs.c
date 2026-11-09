/*
 * Preferences dialog: machine and display options. Values persist through the
 * shared settings store, so the KDE frontend sees the same configuration.
 *
 * Almost everything here applies LIVE. The CoCo's machine, TV input and
 * artifact renderer all switch in place on the emulator thread, so unlike the
 * Apple II target -- whose slot cards are libretro core options read once at
 * load time -- no option in this dialog needs the session restarted. The only
 * exception is the FujiNet/audio group and the Input page's Gamepads switch,
 * which decide what the session starts, and which the dialog therefore applies
 * on close.
 *
 * Everything is stored as an INDEX into the session's own label tables, which
 * is safe because those enums are the session's (core/include/cocosession.h),
 * not a core's: nothing outside this repository can renumber them.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "prefs.h"

#include <string.h>

#include "controllers/controllers_window.h"
#include "window.h"
#include "help/help_window.h"

#define MAX_ROM_ROWS 8
#define MAX_PAD_ROWS 8

static const char *const aspect_names[] = {"TV (4:3)", "Square pixels",
                                           "Integer scale", NULL};

typedef struct {
    CocoWindow *window;
    cocosession *session;
    void (*display_changed)(CocoWindow *window);
    gboolean session_dirty; /* a start-time option changed */
    GtkWidget *rom_rows[MAX_ROM_ROWS];
    AdwPreferencesGroup *pad_group;   /* the Input page's list of pads */
    GtkWidget *pad_rows[MAX_PAD_ROWS];
    int pad_row_count;
    unsigned pad_generation;
    guint pad_timer;
} PrefsState;

typedef struct {
    PrefsState *state;
    const char *key;
    int def;
    /* What to do when this row changes. */
    enum {
        APPLY_DISPLAY,  /* the widget's own painting */
        APPLY_LIVE,     /* pushed straight into the running machine */
        APPLY_ON_CLOSE, /* needs the session restarted */
    } apply;
    /* For APPLY_LIVE rows: the setter to call. */
    void (*setter)(cocosession *s, int value);
} RowBinding;

static RowBinding *binding_new(PrefsState *state, const char *key, int def,
                               int apply,
                               void (*setter)(cocosession *, int))
{
    RowBinding *b = g_new0(RowBinding, 1);
    b->state = state;
    b->key = key;
    b->def = def;
    b->apply = apply;
    b->setter = setter;
    return b;
}

static void combo_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    RowBinding *b = user_data;
    int sel = (int)adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    (void)pspec;

    switch (b->apply) {
    case APPLY_LIVE:
        /* The setter persists the value itself, and may decline it (the
         * CoCo 2 has no RGB input), so re-read rather than assume. */
        b->setter(b->state->session, sel);
        break;
    case APPLY_DISPLAY:
        if (cocosession_get_int(b->state->session, b->key, b->def) == sel)
            return;
        cocosession_set_int(b->state->session, b->key, sel);
        if (b->state->display_changed)
            b->state->display_changed(b->state->window);
        break;
    case APPLY_ON_CLOSE:
        if (cocosession_get_int(b->state->session, b->key, b->def) == sel)
            return;
        cocosession_set_int(b->state->session, b->key, sel);
        b->state->session_dirty = TRUE;
        break;
    }
}

static void switch_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    RowBinding *b = user_data;
    int on = adw_switch_row_get_active(ADW_SWITCH_ROW(row)) ? 1 : 0;
    (void)pspec;
    if (cocosession_get_int(b->state->session, b->key, b->def) == on)
        return;
    cocosession_set_int(b->state->session, b->key, on);
    if (b->apply == APPLY_DISPLAY) {
        if (b->state->display_changed)
            b->state->display_changed(b->state->window);
    } else {
        b->state->session_dirty = TRUE;
    }
}

static GtkWidget *combo_row(PrefsState *state, const char *title,
                            const char *key, int def, int cur,
                            const char *const *names, int apply,
                            void (*setter)(cocosession *, int))
{
    GtkWidget *row = adw_combo_row_new();
    GtkStringList *model = gtk_string_list_new(names);
    RowBinding *b = binding_new(state, key, def, apply, setter);

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
    adw_combo_row_set_selected(ADW_COMBO_ROW(row), (guint)cur);
    g_signal_connect_data(row, "notify::selected", G_CALLBACK(combo_changed),
                          b, (GClosureNotify)g_free, 0);
    g_object_unref(model);
    return row;
}

static GtkWidget *switch_row(PrefsState *state, const char *title,
                             const char *subtitle, const char *key, int def,
                             int apply)
{
    GtkWidget *row = adw_switch_row_new();
    RowBinding *b = binding_new(state, key, def, apply, NULL);

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    if (subtitle)
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    adw_switch_row_set_active(ADW_SWITCH_ROW(row),
                              cocosession_get_int(state->session, key, def));
    g_signal_connect_data(row, "notify::active", G_CALLBACK(switch_changed), b,
                          (GClosureNotify)g_free, 0);
    return row;
}

/* ---- the Input page ------------------------------------------------------ */

static void analog_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    PrefsState *state = user_data;
    (void)pspec;
    cocosession_set_analog(state->session,
                           adw_switch_row_get_active(ADW_SWITCH_ROW(row)) ? 1 : 0);
}

static void pad_subtitle(PrefsState *state, int idx, GtkWidget *row)
{
    const int effective = cocosession_gamepad_effective_port(state->session, idx);
    const char *text = effective == 0 ? "Driving the right joystick"
                     : effective > 0 ? "Driving the left joystick"
                     : "Driving nothing";
    if (g_strcmp0(adw_action_row_get_subtitle(ADW_ACTION_ROW(row)), text) != 0)
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), text);
}

static void pad_assign_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    PrefsState *state = g_object_get_data(row, "coco-state");
    const int idx = GPOINTER_TO_INT(user_data);
    const int sel = (int)adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    (void)pspec;
    /* 0 = automatic (connection order), 1 = the right joystick, 2 = the left */
    cocosession_gamepad_assign(state->session, idx, sel - 1);
    pad_subtitle(state, idx, GTK_WIDGET(row));
}

/* One row per connected pad, rebuilt when the set changes. */
static void rebuild_pad_rows(PrefsState *state)
{
    static const char *const choices[] = { "Automatic", "Right joystick",
                                           "Left joystick", NULL };
    const int count = cocosession_gamepad_count(state->session);
    int idx;

    for (idx = 0; idx < state->pad_row_count; idx++)
        adw_preferences_group_remove(state->pad_group, state->pad_rows[idx]);
    state->pad_row_count = 0;

    if (count == 0) {
        GtkWidget *none = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(none),
                                      "No gamepads connected");
        adw_action_row_set_subtitle(ADW_ACTION_ROW(none),
                                    "Plug one in: it is picked up as it appears");
        adw_preferences_group_add(state->pad_group, none);
        state->pad_rows[state->pad_row_count++] = none;
        return;
    }
    for (idx = 0; idx < count && idx < MAX_PAD_ROWS; idx++) {
        GtkWidget *row = adw_combo_row_new();
        GtkStringList *model = gtk_string_list_new(choices);
        const int assign = cocosession_gamepad_assignment(state->session, idx);
        char name[128];

        cocosession_gamepad_name(state->session, idx, name, sizeof name);
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), name);
        adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
        adw_combo_row_set_selected(ADW_COMBO_ROW(row),
                                   assign < 0 || assign > 1 ? 0 : (guint)assign + 1);
        g_object_set_data(G_OBJECT(row), "coco-state", state);
        g_signal_connect(row, "notify::selected", G_CALLBACK(pad_assign_changed),
                         GINT_TO_POINTER(idx));
        g_object_unref(model);
        adw_preferences_group_add(state->pad_group, row);
        state->pad_rows[state->pad_row_count++] = row;
        pad_subtitle(state, idx, row);
    }
}

/* Hot-plugged pads, and a pad's automatic assignment changing. */
static gboolean pad_tick(gpointer user_data)
{
    PrefsState *state = user_data;
    const unsigned gen = cocosession_gamepad_generation(state->session);

    if (gen != state->pad_generation) {
        state->pad_generation = gen;
        rebuild_pad_rows(state);
    } else if (cocosession_gamepad_count(state->session) > 0) {
        int idx;
        for (idx = 0; idx < state->pad_row_count; idx++)
            pad_subtitle(state, idx, state->pad_rows[idx]);
    }
    return G_SOURCE_CONTINUE;
}

static void reset_controls_clicked(GtkButton *button, gpointer user_data)
{
    PrefsState *state = user_data;
    (void)button;
    cocosession_bindings_reset(state->session);
    coco_window_toast(state->window, "Controls back to their defaults");
}

static void open_controllers_clicked(GtkButton *button, gpointer user_data)
{
    PrefsState *state = user_data;
    (void)button;
    coco_controllers_show(GTK_WINDOW(state->window), state->session);
}

static GtkWidget *button_row(const char *title, const char *subtitle,
                             const char *label, GCallback handler,
                             PrefsState *state)
{
    GtkWidget *row = adw_action_row_new();
    GtkWidget *button = gtk_button_new_with_label(label);

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    g_signal_connect(button, "clicked", handler, state);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), button);
    return row;
}

static AdwPreferencesPage *build_input_page(PrefsState *state)
{
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    AdwPreferencesGroup *pads = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *list = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *controls = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    GtkWidget *analog;

    adw_preferences_page_set_title(page, "Input");
    adw_preferences_page_set_name(page, "Input");
    adw_preferences_page_set_icon_name(page, "input-gaming-symbolic");

    adw_preferences_group_set_title(pads, "Gamepads");
    adw_preferences_group_set_header_suffix(pads, coco_help_button("prefs/Input"));
    adw_preferences_group_set_description(
        pads, "Gamepads drive the two analog joystick ports. The first one "
              "drives the right joystick, the second the left; the choice "
              "below is remembered.");
    adw_preferences_group_add(
        pads, switch_row(state, "Gamepads",
                         "Drive the analog joystick ports from connected "
                         "gamepads (applied when this dialog closes)",
                         "enable_gamepad", 1, APPLY_ON_CLOSE));
    analog = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(analog), "Analog joystick");
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(analog),
        "Move the joystick in proportion to the pad's left stick. Off, the "
        "stick acts as a D-pad: full deflection or none.");
    adw_switch_row_set_active(ADW_SWITCH_ROW(analog),
                              cocosession_get_int(state->session, "analog_joystick", 1));
    g_signal_connect(analog, "notify::active", G_CALLBACK(analog_changed), state);
    adw_preferences_group_add(pads, analog);

    adw_preferences_group_set_title(list, "Connected pads");
    state->pad_group = list;
    state->pad_generation = cocosession_gamepad_generation(state->session);
    rebuild_pad_rows(state);
    state->pad_timer = g_timeout_add(250, pad_tick, state);

    adw_preferences_group_set_title(controls, "Controls");
    adw_preferences_group_add(
        controls, button_row("Open Controllers",
                             "Keys and buttons are mapped in the Controllers "
                             "window (Ctrl+Shift+K)",
                             "Open\xe2\x80\xa6",
                             G_CALLBACK(open_controllers_clicked), state));
    adw_preferences_group_add(
        controls, button_row("Reset Controls",
                             "Put every joystick key and gamepad button back "
                             "to its default",
                             "Reset",
                             G_CALLBACK(reset_controls_clicked), state));

    adw_preferences_page_add(page, pads);
    adw_preferences_page_add(page, list);
    adw_preferences_page_add(page, controls);
    return page;
}

/* ---- the ROMs page ------------------------------------------------------- */

static const char *const ROM_EXTS[] = { "zip", "rom", "bin", NULL };

static void refresh_rows(PrefsState *st)
{
    int r;
    for (r = 0; r < cocosession_rom_item_count() && r < MAX_ROM_ROWS; r++)
        if (st->rom_rows[r])
            adw_action_row_set_subtitle(
                ADW_ACTION_ROW(st->rom_rows[r]),
                cocosession_rom_item_available(st->session, r)
                    ? "Imported" : "Not imported");
}

static void on_rom_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    PrefsState *st = user_data;
    gboolean folder = GPOINTER_TO_INT(g_object_get_data(src, "coco-folder"));
    g_autoptr(GFile) f =
        folder ? gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src),
                                                      res, NULL)
               : gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = f ? g_file_get_path(f) : NULL;
    char msg[1024];

    if (!path)
        return;
    cocosession_rom_import(st->session, path, msg, sizeof msg);
    coco_window_toast(st->window, msg);
    refresh_rows(st);
}

static void on_rom_import(GtkButton *b, gpointer user_data)
{
    PrefsState *st = user_data;
    gboolean folder =
        GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "coco-folder"));
    GtkFileDialog *dlg = coco_file_dialog(
        folder ? "Import BIOS from a Folder" : "Import BIOS",
        "BIOS images and ROM sets", ROM_EXTS);

    g_object_set_data(G_OBJECT(dlg), "coco-folder", GINT_TO_POINTER(folder));
    if (folder)
        gtk_file_dialog_select_folder(dlg, GTK_WINDOW(st->window), NULL,
                                      on_rom_chosen, st);
    else
        gtk_file_dialog_open(dlg, GTK_WINDOW(st->window), NULL, on_rom_chosen,
                             st);
    g_object_unref(dlg);
}

static AdwPreferencesPage *build_roms_page(PrefsState *st)
{
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    AdwPreferencesGroup *group = NULL;
    const char *last = NULL;
    char desc[1200];
    int r;

    adw_preferences_page_set_title(page, "ROMs");
    adw_preferences_page_set_name(page, "ROMs");
    adw_preferences_page_set_icon_name(page, "media-flash-symbolic");
    for (r = 0; r < cocosession_rom_item_count() && r < MAX_ROM_ROWS; r++) {
        const coco_rom_item *item = cocosession_rom_item(r);
        GtkWidget *row;
        if (!last || strcmp(last, item->group) != 0) {
            group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
            adw_preferences_group_set_title(group, item->group);
            if (!last) {
                GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
                GtkWidget *files = gtk_button_new_with_label("Import\xe2\x80\xa6");
                GtkWidget *dir =
                    gtk_button_new_with_label("Import a Folder\xe2\x80\xa6");
                g_snprintf(desc, sizeof desc,
                    "The CoCo's system ROMs are not part of the app. Import "
                    "them from files, a zip or a folder (MAME's coco and "
                    "coco3 sets work); they are recognised by their "
                    "checksums. Kept in %s.",
                    cocosession_roms_path(st->session));
                adw_preferences_group_set_description(group, desc);
                g_object_set_data(G_OBJECT(dir), "coco-folder",
                                  GINT_TO_POINTER(1));
                g_signal_connect(files, "clicked", G_CALLBACK(on_rom_import),
                                 st);
                g_signal_connect(dir, "clicked", G_CALLBACK(on_rom_import), st);
                gtk_widget_set_valign(files, GTK_ALIGN_CENTER);
                gtk_widget_set_valign(dir, GTK_ALIGN_CENTER);
                gtk_box_append(GTK_BOX(box), files);
                gtk_box_append(GTK_BOX(box), dir);
                gtk_box_append(GTK_BOX(box), coco_help_button("prefs/ROMs"));
                adw_preferences_group_set_header_suffix(group, box);
            }
            adw_preferences_page_add(page, group);
            last = item->group;
        }
        row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), item->name);
        adw_preferences_group_add(group, row);
        st->rom_rows[r] = row;
    }
    return page;
}

#define EXTERNAL_PLACEHOLDER "127.0.0.1:65504"

/* The address field: valid text (or none, meaning 127.0.0.1:65504) is saved
 * and marks the session dirty; anything else is flagged and not saved. */
static void addr_changed(GtkEditable *entry, gpointer user_data)
{
    PrefsState *state = user_data;
    const char *text = gtk_editable_get_text(entry);
    char host[64];
    int port;

    if (cocosession_parse_hostport(text, host, sizeof host, &port) != 0) {
        gtk_widget_add_css_class(GTK_WIDGET(entry), "error");
        return;
    }
    gtk_widget_remove_css_class(GTK_WIDGET(entry), "error");
    if (g_strcmp0(cocosession_get_str(state->session, "fujinet_external_addr",
                                      ""), text) == 0)
        return;
    cocosession_set_str(state->session, "fujinet_external_addr", text);
    state->session_dirty = TRUE;
}

static void external_toggled(GObject *row, GParamSpec *pspec,
                             gpointer user_data)
{
    gtk_widget_set_sensitive(
        GTK_WIDGET(user_data),
        adw_switch_row_get_active(ADW_SWITCH_ROW(row)));
    (void)pspec;
}

static void prefs_closed(GtkWidget *dialog, gpointer user_data)
{
    PrefsState *state = user_data;
    (void)dialog;
    if (state->pad_timer) {
        g_source_remove(state->pad_timer);
        state->pad_timer = 0;
    }
    if (state->session_dirty) {
        coco_window_restart_session(state->window);
        coco_window_toast(state->window,
                          "FujiNet options applied (session restarted)");
    } else {
        coco_window_start(state->window);   /* a ROM imported here boots it */
    }
    g_free(state);
}

/* Materialise one of the session's option tables as a NULL-terminated array
 * the GtkStringList constructor accepts. Static storage: the dialog holds the
 * pointers, and there is only ever one dialog. */
#define OPTION_TABLE(fn, storage)                                    \
    static const char *const *storage##_table(void)                  \
    {                                                                \
        static const char *names[16];                                \
        int i;                                                       \
        for (i = 0; i < (int)G_N_ELEMENTS(names) - 1; i++) {         \
            names[i] = fn(i);                                        \
            if (!names[i]) break;                                    \
        }                                                            \
        names[i] = NULL;                                             \
        return names;                                                \
    }

OPTION_TABLE(cocosession_machine_name, machine)
OPTION_TABLE(cocosession_tv_input_name, tv_input)
OPTION_TABLE(cocosession_ccr_name, ccr)

void coco_prefs_show(CocoWindow *parent, cocosession *session,
                     void (*display_changed)(CocoWindow *parent))
{
    PrefsState *state = g_new0(PrefsState, 1);
    AdwPreferencesDialog *dialog =
        ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());
    AdwPreferencesPage *page =
        ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    AdwPreferencesGroup *machine =
        ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *video =
        ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *display =
        ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *runtime =
        ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    GtkWidget *external, *addr;

    state->window = parent;
    state->session = session;
    state->display_changed = display_changed;

    adw_preferences_page_set_title(page, "Machine");
    adw_preferences_page_set_name(page, "Machine");
    adw_preferences_page_set_icon_name(page, "computer-symbolic");
    adw_preferences_group_set_title(machine, "Machine");
    adw_preferences_group_set_header_suffix(machine, coco_help_button("prefs/Machine"));
    adw_preferences_group_set_description(
        machine, "Switching machines resets it, with the matching HDB-DOS "
                 "Becker ROM. FujiNet stays connected.");
    adw_preferences_group_add(
        machine, combo_row(state, "Model", "machine",
                           COCOSESSION_MACHINE_COCO3,
                           cocosession_machine(session), machine_table(),
                           APPLY_LIVE, cocosession_set_machine));

    adw_preferences_group_set_title(video, "Video");
    adw_preferences_group_set_description(
        video, "How the machine's video reaches the display. Artifact colours "
               "-- which most hi-res CoCo games depend on -- exist only on a "
               "composite signal; the two phases are which way round they "
               "come out.");
    adw_preferences_group_add(
        video, combo_row(state, "Display", "tv_input",
                         COCOSESSION_TV_COMPOSITE_BR,
                         cocosession_tv_input(session), tv_input_table(),
                         APPLY_LIVE, cocosession_set_tv_input));
    adw_preferences_group_add(
        video, combo_row(state, "Artifact colour", "ccr",
                         COCOSESSION_CCR_5BIT, cocosession_ccr(session),
                         ccr_table(), APPLY_LIVE, cocosession_set_ccr));

    adw_preferences_group_set_title(display, "Window");
    adw_preferences_group_add(
        display, combo_row(state, "Aspect ratio", "aspect_mode", 0,
                           cocosession_get_int(session, "aspect_mode", 0),
                           aspect_names, APPLY_DISPLAY, NULL));
    adw_preferences_group_add(
        display, switch_row(state, "Smooth scaling",
                            "Interpolate when the frame is scaled, instead of "
                            "showing hard pixel edges",
                            "smooth_scaling", 0, APPLY_DISPLAY));

    adw_preferences_group_set_title(runtime, "FujiNet");
    adw_preferences_group_set_description(
        runtime, "Applied by restarting the session when this dialog closes");
    adw_preferences_group_add(
        runtime, switch_row(state, "FujiNet",
                            "Run the FujiNet runtime and serve it to the "
                            "machine over the Becker port",
                            "enable_fujinet", 1, APPLY_ON_CLOSE));
    external = switch_row(state, "Use external FujiNet",
                          "Skip the built-in FujiNet and connect to another "
                          "instance's Becker port instead",
                          "fujinet_external", 0, APPLY_ON_CLOSE);
    addr = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(addr),
                                  "Address:port, e.g. " EXTERNAL_PLACEHOLDER);
    gtk_editable_set_text(GTK_EDITABLE(addr),
                          cocosession_get_str(session, "fujinet_external_addr",
                                              ""));
    g_signal_connect(addr, "changed", G_CALLBACK(addr_changed), state);
    gtk_widget_set_sensitive(addr, adw_switch_row_get_active(
                                       ADW_SWITCH_ROW(external)));
    g_signal_connect(external, "notify::active", G_CALLBACK(external_toggled),
                     addr);
    adw_preferences_group_add(runtime, external);
    adw_preferences_group_add(runtime, addr);
    adw_preferences_group_add(
        runtime, switch_row(state, "Sound", NULL, "enable_audio", 1,
                            APPLY_ON_CLOSE));

    adw_preferences_page_add(page, machine);
    adw_preferences_page_add(page, video);
    adw_preferences_page_add(page, display);
    adw_preferences_page_add(page, runtime);
    adw_preferences_dialog_add(dialog, page);
    adw_preferences_dialog_add(dialog, build_input_page(state));
    adw_preferences_dialog_add(dialog, build_roms_page(state));
    refresh_rows(state);
    g_signal_connect(dialog, "closed", G_CALLBACK(prefs_closed), state);
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(parent));
}
