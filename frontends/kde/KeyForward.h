/*
 * Key handling shared by the GPU and the software display.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "cocosession.h"

class QEvent;
class QKeyEvent;

/* The app's own keys, which never reach the CoCo: F10/F11/F12 and
 * Ctrl+Shift+letter (Controllers, Help). */
bool cocoIsAppKey(const QKeyEvent *event);
/* The canonical key identity bindings use (cocosession_bindable_key), or 0
 * for a key that cannot be bound. */
uint32_t cocoBindableKey(const QKeyEvent *event);
/* ShortcutOverride: true (and accepted) for every key but the app's own. */
bool cocoClaimShortcut(QEvent *event);
void cocoKeyPress(cocosession *session, QKeyEvent *event);
void cocoKeyRelease(cocosession *session, QKeyEvent *event);
