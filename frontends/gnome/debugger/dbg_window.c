/*
 * The 6809/6309 debugger window for the GNOME frontend.
 *
 * PLACEHOLDER. The engine it drives (core/debugger/debugger.c) is milestone
 * M6; until it exists cocosession_debugger() returns NULL, and this says so
 * rather than opening an empty window that looks broken.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "debugger/dbg_window.h"

void coco_debugger_show(GtkWindow *parent, cocosession *session)
{
    AdwAlertDialog *dialog;

    if (cocosession_debugger(session)) {
        /* Unreachable until M6 lands; when it does, this is where the window
         * gets built instead. */
        return;
    }

    dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(
        "Debugger Unavailable",
        "The 6809/6309 debugger is not built into this version yet."));
    adw_alert_dialog_add_response(dialog, "close", "_Close");
    adw_alert_dialog_set_default_response(dialog, "close");
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(parent));
}
