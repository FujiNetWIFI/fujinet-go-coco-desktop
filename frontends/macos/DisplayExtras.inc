/*
 * DisplayExtras.inc -- the keyboard and the aspect modes, included into the
 * display view's @implementation (see DisplayGlue.h).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

- (void)extraInit {}

/* The frame clock follows the window (viewDidMoveToWindow); the app asks for
 * it explicitly once the window is ordered in. */
- (void)start { [self updateVisibility]; }

/* 0: the television's 4:3, 1: square pixels, 2: whole-number scale. */
- (void)setAspectMode:(int)mode
{
    atomic_store(&s_fn_aspect_mode, mode);
    atomic_store(&_redraw, 1);
    [self setNeedsDisplay:YES];
}

/* ---- input -------------------------------------------------------------------
 * cocosession_key wants an X11/xkb keysym (or 0, meaning "look at unicode
 * instead" -- see core/src/input_map.c's coco_key_from_event, which is
 * what every frontend's translation ultimately feeds) plus the raw
 * unicode character, down/up state and modifier flags -- the session does
 * the CoCo-key translation itself, unlike ADAM's frontend-side
 * adam_key_from_event. The translation of an event to that pair is in
 * KeyForward.m, shared with the Controllers panel; a key a binding took
 * (Map) is consumed there and does not reach the machine. */

- (BOOL)acceptsFirstResponder
{
    return YES;
}

- (void)keyDown:(NSEvent *)event
{
    CocoForwardKey(_session, event, 1);
}

- (void)keyUp:(NSEvent *)event
{
    CocoForwardKey(_session, event, 0);
}

