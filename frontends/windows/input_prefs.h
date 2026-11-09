/*
 * The Win32 Input window -- see input_prefs.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef COCO_WIN_INPUT_PREFS_H
#define COCO_WIN_INPUT_PREFS_H

#include <windows.h>

#include "cocosession.h"

/* Called when the window closes; restart = a setting that is applied by
 * restarting the session changed (Gamepads on or off). */
typedef void (*coco_input_closed_fn)(int restart);

/* Opens (or raises) the Input window. */
void coco_input_prefs_show(HWND parent, cocosession *session, coco_input_closed_fn closed);

/* The gamepad set changed (the main window saw a new generation). */
void coco_input_prefs_gamepads_changed(void);

/* Tab and the arrow keys move between its controls; 1 if the message was used. */
int coco_input_prefs_pretranslate(MSG *msg);

#endif
