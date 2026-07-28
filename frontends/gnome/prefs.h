/*
 * Preferences dialog for the GNOME frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "cocosession.h"

G_BEGIN_DECLS

typedef struct _CocoWindow CocoWindow;

/* Shows the preferences dialog. display_changed is invoked immediately when
 * a display-only option changes; machine options restart the session when
 * the dialog closes. */
void coco_prefs_show(CocoWindow *parent, cocosession *session,
                       void (*display_changed)(CocoWindow *parent));

G_END_DECLS
