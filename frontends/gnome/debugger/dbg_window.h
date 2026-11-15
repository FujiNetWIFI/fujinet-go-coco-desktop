/*
 * Debugger window for the GNOME frontend.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "cocosession.h"

G_BEGIN_DECLS

/* Shows (creating on first use) the debugger window for the session.
 * Showing engages the debugger engine and stops the machine. */
void coco_debugger_show(GtkWindow *parent, cocosession *session);
/* F12: shows it, or hides it (releasing the engine, so the machine runs on). */
void coco_debugger_toggle(GtkWindow *parent, cocosession *session);

G_END_DECLS
