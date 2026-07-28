/*
 * The 6809/6309 debugger window for the KDE/Qt frontend.
 *
 * PLACEHOLDER, matching frontends/gnome/debugger/dbg_window.c. The engine it
 * drives (core/debugger/debugger.c) is milestone M6; until it exists
 * cocosession_debugger() returns NULL, and this says so rather than opening
 * an empty window that looks broken.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "debugger/DebuggerWindow.h"

#include <QMessageBox>

namespace DebuggerWindow {

void show(QWidget *parent, cocosession *session)
{
    if (cocosession_debugger(session)) {
        /* Unreachable until M6 lands; when it does, this is where the window
         * gets built instead. */
        return;
    }
    QMessageBox::information(
        parent, QStringLiteral("Debugger Unavailable"),
        QStringLiteral("The 6809/6309 debugger is not built into this version "
                       "yet."));
}

} // namespace DebuggerWindow
