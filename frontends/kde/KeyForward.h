/*
 * Key handling shared by the GPU and the software display.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "cocosession.h"

class QEvent;
class QKeyEvent;

/* ShortcutOverride: true (and accepted) for every key but F10/F11/F12. */
bool cocoClaimShortcut(QEvent *event);
void cocoKeyPress(cocosession *session, QKeyEvent *event);
void cocoKeyRelease(cocosession *session, QKeyEvent *event);
