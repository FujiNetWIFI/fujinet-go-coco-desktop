/*
 * The Controllers window -- the CoCo's two joysticks drawn like the Tandy
 * ones (off-white base, black stick, red fire buttons) with the RESET button
 * between them, and, when asked, the keyboard under them. Held controls are lit
 * in the accent colour, the gamepads' joystick assignments are listed, and Map
 * mode rebinds any joystick control to a key or a gamepad button.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "cocosession.h"

/* Shows the singleton window (creating it on first call). `parent` is the main
 * window; it is only used the first time, to set the transient-for
 * relationship, and keys typed in this window go through it. */
void coco_controllers_show(GtkWindow *parent, cocosession *session);
/* Shows it, or hides it if it is showing. */
void coco_controllers_toggle(GtkWindow *parent, cocosession *session);
void coco_controllers_hide(void);
gboolean coco_controllers_visible(void);
