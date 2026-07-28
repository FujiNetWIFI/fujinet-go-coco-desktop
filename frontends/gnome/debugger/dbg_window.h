/*
 * The 6809/6309 debugger window for the GNOME frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "cocosession.h"

G_BEGIN_DECLS

void coco_debugger_show(GtkWindow *parent, cocosession *session);

G_END_DECLS
