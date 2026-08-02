/*
 * CocoWindow: main window of the GNOME frontend. Header bar + menu over the
 * emulator display; keyboard capture routes everything except F10 (menu),
 * F11 (fullscreen) and F12 (debugger) to the CoCo. No on-screen input panels
 * are shown unless the user asks for them.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "window.h"

#include <stdlib.h>
#include <string.h>

#include "debugger/dbg_window.h"
#include "display.h"
#include "prefs.h"
#include "webview.h"

struct _CocoWindow {
    AdwApplicationWindow parent_instance;

    cocosession *session;
    CocoDisplay *display;
    AdwToastOverlay *toasts;
    GtkMenuButton *menu_button;
};

G_DEFINE_FINAL_TYPE(CocoWindow, coco_window, ADW_TYPE_APPLICATION_WINDOW)

/* ---- helpers ------------------------------------------------------------ */

void coco_window_toast(CocoWindow *self, const char *message)
{
    adw_toast_overlay_add_toast(self->toasts, adw_toast_new(message));
}

void coco_window_restart_session(CocoWindow *self)
{
    cocosession_start_opts opts;
    cocosession_stop(self->session);
    cocosession_default_opts(self->session, &opts);
    if (cocosession_start(self->session, &opts) != 0)
        coco_window_toast(self, cocosession_last_error(self->session));
}

static void apply_display_settings(CocoWindow *self)
{
    coco_display_set_aspect_mode(
        self->display,
        (CocoAspectMode)cocosession_get_int(self->session, "aspect_mode",
                                            COCO_ASPECT_TV_4_3));
    coco_display_set_smooth(
        self->display, cocosession_get_int(self->session, "smooth_scaling", 0));
}

/* ---- keyboard capture ---------------------------------------------------
 * Everything goes to the machine except the three function keys the frontend
 * keeps: F10 (menu), F11 (fullscreen), F12 (debugger). ALT in particular is
 * NOT passed to the window manager -- it is a real key on a CoCo 3 keyboard.
 *
 * Both press and release are forwarded, because XRoar's hkbd layer tracks
 * held keys: a missed release leaves that key down inside the machine. */

static gboolean forward_key(CocoWindow *self, guint keyval,
                            GdkModifierType state, int down)
{
    cocosession_key(self->session, down, keyval, gdk_keyval_to_unicode(keyval),
                    (state & GDK_CONTROL_MASK) != 0,
                    (state & GDK_SHIFT_MASK) != 0);
    return TRUE;
}

static gboolean on_key_pressed(GtkEventControllerKey *controller, guint keyval,
                               guint keycode, GdkModifierType state,
                               gpointer user_data)
{
    CocoWindow *self = COCO_WINDOW(user_data);
    (void)controller;
    (void)keycode;

    switch (keyval) {
    case GDK_KEY_F10: /* open the primary menu */
        gtk_menu_button_popup(self->menu_button);
        return TRUE;
    case GDK_KEY_F11:
        if (gtk_window_is_fullscreen(GTK_WINDOW(self)))
            gtk_window_unfullscreen(GTK_WINDOW(self));
        else
            gtk_window_fullscreen(GTK_WINDOW(self));
        return TRUE;
    case GDK_KEY_F12:
        gtk_widget_activate_action(GTK_WIDGET(self), "win.debugger", NULL);
        return TRUE;
    default:
        break;
    }

    return forward_key(self, keyval, state, 1);
}

static void on_key_released(GtkEventControllerKey *controller, guint keyval,
                            guint keycode, GdkModifierType state,
                            gpointer user_data)
{
    CocoWindow *self = COCO_WINDOW(user_data);
    (void)controller;
    (void)keycode;
    if (keyval == GDK_KEY_F10 || keyval == GDK_KEY_F11 ||
        keyval == GDK_KEY_F12)
        return;
    forward_key(self, keyval, state, 0);
}

/* ---- actions ------------------------------------------------------------ */

static void action_reset(GSimpleAction *action, GVariant *param,
                         gpointer user_data)
{
    CocoWindow *self = COCO_WINDOW(user_data);
    (void)action;
    (void)param;
    cocosession_reset(self->session);
    coco_window_toast(self, "Reset");
}

/* Machine, TV input and artifact renderer are stateful radio actions: they
 * switch the running machine in place, so the menu is also the readout of
 * what is currently selected. */

static void rebuild_menu(CocoWindow *self);

static void set_radio_state(CocoWindow *self, const char *name, int value)
{
    GAction *a = g_action_map_lookup_action(G_ACTION_MAP(self), name);
    if (a)
        g_simple_action_set_state(G_SIMPLE_ACTION(a), g_variant_new_int32(value));
}

static void action_machine(GSimpleAction *action, GVariant *param,
                           gpointer user_data)
{
    CocoWindow *self = COCO_WINDOW(user_data);
    int machine = g_variant_get_int32(param);
    g_autofree char *msg = NULL;

    cocosession_set_machine(self->session, machine);
    /* Switching to a CoCo 2 with RGB selected carries the TV input to
     * something that machine can produce, so read both back rather than
     * assuming they took. */
    g_simple_action_set_state(action, g_variant_new_int32(
                                  cocosession_machine(self->session)));
    set_radio_state(self, "tv-input", cocosession_tv_input(self->session));
    /* ...and rebuild the menu, so the Display submenu offers exactly the TV
     * inputs the new machine has. */
    rebuild_menu(self);
    msg = g_strdup_printf("%s (reset)", cocosession_machine_name(
                              cocosession_machine(self->session)));
    coco_window_toast(self, msg);
}

static void action_tv_input(GSimpleAction *action, GVariant *param,
                            gpointer user_data)
{
    CocoWindow *self = COCO_WINDOW(user_data);
    cocosession_set_tv_input(self->session, g_variant_get_int32(param));
    g_simple_action_set_state(action, g_variant_new_int32(
                                  cocosession_tv_input(self->session)));
}

static void action_ccr(GSimpleAction *action, GVariant *param,
                       gpointer user_data)
{
    CocoWindow *self = COCO_WINDOW(user_data);
    cocosession_set_ccr(self->session, g_variant_get_int32(param));
    g_simple_action_set_state(action, g_variant_new_int32(
                                  cocosession_ccr(self->session)));
}

static void action_cpu(GSimpleAction *action, GVariant *param,
                       gpointer user_data)
{
    CocoWindow *self = COCO_WINDOW(user_data);
    g_autofree char *msg = NULL;
    cocosession_set_cpu(self->session, g_variant_get_int32(param));
    g_simple_action_set_state(action, g_variant_new_int32(
                                  cocosession_cpu(self->session)));
    msg = g_strdup_printf("%s (reset)", cocosession_cpu_name(
                              cocosession_cpu(self->session)));
    coco_window_toast(self, msg);
}

static void import_done(GObject *source, GAsyncResult *result,
                        gpointer user_data)
{
    CocoWindow *self = COCO_WINDOW(user_data);
    g_autoptr(GFile) file =
        gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
    char dest[1024];
    g_autofree char *path = NULL;
    g_autofree char *msg = NULL;

    if (!file)
        return;
    path = g_file_get_path(file);
    if (!path)
        return;
    if (cocosession_import_media(self->session, path, dest, sizeof(dest)) != 0) {
        coco_window_toast(self, cocosession_last_error(self->session));
        return;
    }
    msg = g_strdup_printf("Copied to FujiNet SD: %s. Mount it from FujiNet "
                          "Configuration.", strrchr(dest, '/') + 1);
    coco_window_toast(self, msg);
}

static void action_import_media(GSimpleAction *action, GVariant *param,
                                gpointer user_data)
{
    CocoWindow *self = COCO_WINDOW(user_data);
    GtkFileDialog *dialog = gtk_file_dialog_new();
    GtkFileFilter *filter = gtk_file_filter_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    static const char *const exts[] = { "dsk", "os9", "vdk", "cas", "wav",
                                        NULL };
    int i;
    (void)action;
    (void)param;

    gtk_file_filter_set_name(filter, "CoCo disk and tape images");
    for (i = 0; exts[i]; i++)
        gtk_file_filter_add_suffix(filter, exts[i]);
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_title(dialog, "Import Disk or Tape Image");
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_open(dialog, GTK_WINDOW(self), NULL, import_done, self);
    g_object_unref(filters);
    g_object_unref(dialog);
}

static void rom_import_done(GObject *source, GAsyncResult *result,
                            gpointer user_data)
{
    CocoWindow *self = COCO_WINDOW(user_data);
    g_autoptr(GListModel) files =
        gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(source), result,
                                             NULL);
    g_autofree char *msg = NULL;
    guint n, i, ok = 0;

    if (!files)
        return;
    n = g_list_model_get_n_items(files);
    for (i = 0; i < n; i++) {
        g_autoptr(GFile) f = g_list_model_get_item(files, i);
        g_autofree char *path = g_file_get_path(f);
        char dest[1024];
        if (path &&
            cocosession_import_rom(self->session, path, dest, sizeof(dest)) == 0)
            ok++;
    }
    if (ok == 0) {
        coco_window_toast(self, cocosession_last_error(self->session));
        return;
    }
    /* XRoar resolves ROMs when the machine is built, so this needs a
     * restart -- an in-place machine switch would reuse the loaded images. */
    coco_window_restart_session(self);
    msg = g_strdup_printf("Imported %u ROM file%s (session restarted)", ok,
                          ok == 1 ? "" : "s");
    coco_window_toast(self, msg);
}

static void action_import_roms(GSimpleAction *action, GVariant *param,
                               gpointer user_data)
{
    CocoWindow *self = COCO_WINDOW(user_data);
    GtkFileDialog *dialog = gtk_file_dialog_new();
    GtkFileFilter *filter = gtk_file_filter_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    (void)action;
    (void)param;

    /* XRoar looks ROMs up by exact basename (bas13.rom, extbas11.rom,
     * coco3.rom, ...), so whatever is picked keeps its name. */
    gtk_file_filter_set_name(filter, "CoCo system ROMs");
    gtk_file_filter_add_suffix(filter, "rom");
    gtk_file_filter_add_suffix(filter, "ROM");
    gtk_file_filter_add_suffix(filter, "bin");
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_title(dialog, "Import System ROMs");
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_open_multiple(dialog, GTK_WINDOW(self), NULL,
                                  rom_import_done, self);
    g_object_unref(filters);
    g_object_unref(dialog);
}

static void action_fujinet_config(GSimpleAction *action, GVariant *param,
                                  gpointer user_data)
{
    CocoWindow *self = COCO_WINDOW(user_data);
    (void)action;
    (void)param;
    coco_fujinet_config_show(GTK_WINDOW(self), self->session);
}

static void action_fujinet_log(GSimpleAction *action, GVariant *param,
                               gpointer user_data)
{
    CocoWindow *self = COCO_WINDOW(user_data);
    (void)action;
    (void)param;
    coco_fujinet_log_show(GTK_WINDOW(self), self->session);
}

static void action_debugger(GSimpleAction *action, GVariant *param,
                            gpointer user_data)
{
    CocoWindow *self = COCO_WINDOW(user_data);
    (void)action;
    (void)param;
    coco_debugger_show(GTK_WINDOW(self), self->session);
}

static void action_preferences(GSimpleAction *action, GVariant *param,
                               gpointer user_data)
{
    CocoWindow *self = COCO_WINDOW(user_data);
    (void)action;
    (void)param;
    coco_prefs_show(self, self->session, apply_display_settings);
}

static void action_about(GSimpleAction *action, GVariant *param,
                         gpointer user_data)
{
    CocoWindow *self = COCO_WINDOW(user_data);
    (void)action;
    (void)param;
    adw_show_about_dialog(
        GTK_WIDGET(self),
        "application-name", "FujiNet Go CoCo",
        "application-icon", coco_icon_name(),
        "developer-name", "Thomas Cherryhomes",
        "version", COCO_VERSION_STRING,
        "license-type", GTK_LICENSE_GPL_3_0,
        "comments", "Self-contained Tandy Color Computer with built-in FujiNet",
        "website", "https://fujinet.online/",
        NULL);
}

/* ---- construction ------------------------------------------------------- */

/* Build a radio submenu from one of the session's label tables. `supported`
 * may be NULL; when set, it decides which indices are offered (that is how
 * RGB disappears on a CoCo 2). */
static GMenu *radio_menu(const char *action,
                         const char *(*name_at)(int),
                         int (*supported)(int machine, int idx), int machine)
{
    GMenu *m = g_menu_new();
    const char *label;
    int i;

    for (i = 0; (label = name_at(i)) != NULL; i++) {
        g_autofree char *detail = NULL;
        if (supported && !supported(machine, i))
            continue;
        detail = g_strdup_printf("%s(%d)", action, i);
        g_menu_append(m, label, detail);
    }
    return m;
}

static GMenu *build_menu(cocosession *session)
{
    GMenu *menu = g_menu_new();
    GMenu *machine = g_menu_new();
    GMenu *media = g_menu_new();
    GMenu *fujinet = g_menu_new();
    GMenu *view = g_menu_new();
    GMenu *tail = g_menu_new();
    GMenu *sub;

    sub = radio_menu("win.machine", cocosession_machine_name, NULL, 0);
    g_menu_append_submenu(machine, "Machine", G_MENU_MODEL(sub));
    g_object_unref(sub);
    g_menu_append(machine, "Reset", "win.reset");
    g_menu_append_section(menu, "Machine", G_MENU_MODEL(machine));

    /* Built for the machine that is running now: switching machines rebuilds
     * the menu, which is what removes RGB on a CoCo 2. */
    sub = radio_menu("win.tv-input", cocosession_tv_input_name,
                     cocosession_tv_input_supported,
                     cocosession_machine(session));
    g_menu_append_submenu(view, "Display", G_MENU_MODEL(sub));
    g_object_unref(sub);
    sub = radio_menu("win.ccr", cocosession_ccr_name, NULL, 0);
    g_menu_append_submenu(view, "Artifact Colour", G_MENU_MODEL(sub));
    g_object_unref(sub);
    sub = radio_menu("win.cpu", cocosession_cpu_name, NULL, 0);
    g_menu_append_submenu(view, "CPU", G_MENU_MODEL(sub));
    g_object_unref(sub);
    g_menu_append(view, "Debugger (F12)", "win.debugger");
    g_menu_append_section(menu, "View", G_MENU_MODEL(view));

    g_menu_append(media, "Import Disk or Tape Image…", "win.import-media");
    g_menu_append(media, "Import System ROMs…", "win.import-roms");
    g_menu_append_section(menu, "Media", G_MENU_MODEL(media));

    g_menu_append(fujinet, "FujiNet Configuration…", "win.fujinet-config");
    g_menu_append(fujinet, "FujiNet Console Log…", "win.fujinet-log");
    g_menu_append_section(menu, "FujiNet", G_MENU_MODEL(fujinet));

    g_menu_append(tail, "Preferences…", "win.preferences");
    g_menu_append(tail, "About FujiNet Go CoCo", "win.about");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(tail));

    g_object_unref(machine);
    g_object_unref(media);
    g_object_unref(fujinet);
    g_object_unref(view);
    g_object_unref(tail);
    return menu;
}

/* Rebuild the menu after a machine switch, so the Display submenu offers
 * exactly the TV inputs the new machine has. */
static void rebuild_menu(CocoWindow *self)
{
    GMenu *menu = build_menu(self->session);
    gtk_menu_button_set_menu_model(self->menu_button, G_MENU_MODEL(menu));
    g_object_unref(menu);
}

static const GActionEntry win_actions[] = {
    {.name = "reset", .activate = action_reset},
    {.name = "import-media", .activate = action_import_media},
    {.name = "import-roms", .activate = action_import_roms},
    {.name = "fujinet-config", .activate = action_fujinet_config},
    {.name = "fujinet-log", .activate = action_fujinet_log},
    {.name = "debugger", .activate = action_debugger},
    {.name = "preferences", .activate = action_preferences},
    {.name = "about", .activate = action_about},
};

/* The three stateful ones, which g_action_map_add_action_entries cannot
 * express with an int32 state. */
static void add_radio_action(CocoWindow *self, const char *name, int value,
                             GCallback change_state)
{
    GSimpleAction *a = g_simple_action_new_stateful(
        name, G_VARIANT_TYPE_INT32, g_variant_new_int32(value));
    g_signal_connect(a, "change-state", change_state, self);
    g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(a));
    g_object_unref(a);
}

static void coco_window_class_init(CocoWindowClass *klass)
{
    (void)klass;
}

static void coco_window_init(CocoWindow *self)
{
    (void)self;
}

GtkWidget *coco_window_new(AdwApplication *app, cocosession *session)
{
    CocoWindow *self = g_object_new(COCO_TYPE_WINDOW,
                                    "application", app,
                                    "title", "FujiNet Go CoCo",
                                    "default-width", 1120,
                                    "default-height", 840,
                                    NULL);
    AdwToolbarView *view;
    AdwHeaderBar *header;
    GtkEventController *keys;

    self->session = session;

    g_action_map_add_action_entries(G_ACTION_MAP(self), win_actions,
                                    G_N_ELEMENTS(win_actions), self);
    add_radio_action(self, "machine", cocosession_machine(session),
                     G_CALLBACK(action_machine));
    add_radio_action(self, "tv-input", cocosession_tv_input(session),
                     G_CALLBACK(action_tv_input));
    add_radio_action(self, "ccr", cocosession_ccr(session),
                     G_CALLBACK(action_ccr));
    add_radio_action(self, "cpu", cocosession_cpu(session),
                     G_CALLBACK(action_cpu));

    header = ADW_HEADER_BAR(adw_header_bar_new());
    self->menu_button = GTK_MENU_BUTTON(gtk_menu_button_new());
    gtk_menu_button_set_icon_name(self->menu_button, "open-menu-symbolic");
    adw_header_bar_pack_end(header, GTK_WIDGET(self->menu_button));
    rebuild_menu(self);

    self->display = COCO_DISPLAY(coco_display_new(session));
    self->toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    adw_toast_overlay_set_child(self->toasts, GTK_WIDGET(self->display));

    view = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    adw_toolbar_view_add_top_bar(view, GTK_WIDGET(header));
    adw_toolbar_view_set_content(view, GTK_WIDGET(self->toasts));
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(self),
                                       GTK_WIDGET(view));

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), self);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), self);
    gtk_widget_add_controller(GTK_WIDGET(self), keys);

    apply_display_settings(self);
    gtk_widget_grab_focus(GTK_WIDGET(self->display));

    /* Developer affordance: open the debugger alongside the main window,
     * which is what you want when the app dies before you can reach a menu. */
    if (getenv("COCO_OPEN_DEBUGGER"))
        coco_debugger_show(GTK_WINDOW(self), session);
    return GTK_WIDGET(self);
}
