/*
 * The Controllers window (Ctrl+Shift+K): the CoCo's two joysticks drawn as a
 * player sees them -- clickable, draggable, lit while held from any source --
 * with RESET, an optional on-screen keyboard, the gamepads and their port
 * assignments, and Map mode for rebinding the joystick controls. Hidden at
 * start; a fixed-size floating window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QWidget>

#include "cocosession.h"

/* Shows the window, or hides it when it is showing. */
void cocoControllersToggle(QWidget *parent, cocosession *session);
/* Shows (and raises) it; the toggle's other half, for "Open Controllers". */
void cocoControllersShow(QWidget *parent, cocosession *session);
bool cocoControllersVisible();
/* The session restarted or the machine changed: redraw from the new layout. */
void cocoControllersRefresh();
