/*
 * AppKit key events to the keysym/unicode pair cocosession_key takes, shared
 * by the machine's picture and the Controllers panel (keys typed there reach
 * the CoCo exactly as in the main window).
 *
 * Written to the family's AppKit pattern; built and verified on CI only (no
 * Apple toolchain here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>
#include <stdint.h>

#include "cocosession.h"

/* X11 keysym of Escape: Map backs out on it. */
#define COCO_KEYSYM_ESCAPE 0xFF1B

/* The keysym and unicode of a key event; NO when it is not a key the CoCo
 * has. */
BOOL CocoKeyFromEvent(NSEvent *event, uint32_t *keysym, uint32_t *unicode);

/* Hands a key press (down 1) or release (down 0) to the session. Returns what
 * cocosession_key does: 1 when a binding consumed the key, else 0. */
int CocoForwardKey(cocosession *session, NSEvent *event, int down);
