/*
 * The Win32 Controllers window (Ctrl+Shift+K): the two joysticks and the
 * RESET button drawn as a player sees them, the machine's keyboard as a
 * second section, the gamepads with their assignments, and Map mode, in a
 * fixed-size floating tool window. Hidden at start.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <windows.h>

#include "cocosession.h"

/* Show / hide (Ctrl+Shift+K). Created on first use, hidden not destroyed after. */
void coco_controllers_window_toggle(HWND parent, cocosession *session);
/* Show (and raise) it; the Input window's Open Controllers button. */
void coco_controllers_window_show(HWND parent, cocosession *session);
int  coco_controllers_window_visible(void);

/* The gamepad set or the machine changed: refresh. */
void coco_controllers_window_refresh(void);

/* Map mode is armed: the next host key is a binding, not typing. The main
 * window gives its keys here first; 1 if consumed. */
int coco_controllers_map_key(UINT msg, WPARAM wpar, LPARAM lpar);
