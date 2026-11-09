/*
 * The Controllers panel (Cmd+Shift+K): the CoCo's two joysticks drawn from
 * the session's layout (cocosession_controls_*), the CoCo's keyboard from its
 * keycap table (cocosession_keyboard_layout, shown by the Keyboard checkbox),
 * the gamepads with their ports, and Map mode for rebinding. Hidden at start;
 * a fixed-size floating NSPanel.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "cocosession.h"

@interface CocoControllersWindow : NSObject
- (instancetype)initWithSession:(cocosession *)session;
/* Show, or hide when showing. */
- (void)toggle;
- (void)hide;
- (BOOL)isVisible;
/* The machine changed or the session restarted: the panel is rebuilt for it
 * and lets go of everything it holds. */
- (void)refresh;
@end
