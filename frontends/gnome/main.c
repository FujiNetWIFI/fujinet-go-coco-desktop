/*
 * FujiNet Go CoCo -- GNOME frontend entry point.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <adwaita.h>

#include "cocosession.h"
#include "window.h"

static cocosession *g_session;

/* The installed icons are named after the desktop-entry id. Running straight
 * out of the build tree there is nothing installed to look up, so point the
 * icon theme at the in-tree artwork (named for the project, not the id) and
 * report that name instead. */
const char *coco_icon_name(void)
{
    static const char *name;
    GtkIconTheme *theme;

    if (name)
        return name;

    name = COCO_APP_ID;
    theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
    if (theme && !gtk_icon_theme_has_icon(theme, name)) {
        gtk_icon_theme_add_search_path(theme, COCO_SOURCE_ICON_DIR);
        if (gtk_icon_theme_has_icon(theme, "fujinet-go-coco"))
            name = "fujinet-go-coco";
    }
    return name;
}

static void on_activate(AdwApplication *app, gpointer user_data)
{
    GtkWindow *win;
    (void)user_data;

    win = gtk_application_get_active_window(GTK_APPLICATION(app));
    if (win) {
        gtk_window_present(win);
        return;
    }

    if (!g_session) {
        cocosession_start_opts opts;
        g_session = cocosession_new(NULL);
        if (!g_session) {
            g_printerr("fatal: could not create the session\n");
            g_application_quit(G_APPLICATION(app));
            return;
        }
        cocosession_default_opts(g_session, &opts);
        /* Without ROMs the window opens on its No-BIOS page instead. */
        if (cocosession_has_system_roms(g_session,
                                        cocosession_machine(g_session)) &&
            cocosession_start(g_session, &opts) != 0)
            g_printerr("session start: %s\n",
                       cocosession_last_error(g_session));
    }

    win = GTK_WINDOW(coco_window_new(app, g_session));
    gtk_window_set_icon_name(win, coco_icon_name());
    gtk_window_present(win);
}

static void on_shutdown(GApplication *app, gpointer user_data)
{
    (void)app;
    (void)user_data;
    if (g_session) {
        cocosession_free(g_session);
        g_session = NULL;
    }
}

int main(int argc, char *argv[])
{
    g_autoptr(AdwApplication) app = adw_application_new(
        COCO_APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), NULL);
    return g_application_run(G_APPLICATION(app), argc, argv);
}
