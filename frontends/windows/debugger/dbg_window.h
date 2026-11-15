/*
 * The Win32 debugger window over the 6809 / 6309 debugger engine
 * (cocodebug.h).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef COCO_WIN_DBG_WINDOW_H
#define COCO_WIN_DBG_WINDOW_H

#include <windows.h>

#include "cocosession.h"

/* Shows (creating on first use) the debugger window for the session. Showing
 * it engages the engine and pauses the machine, as the other frontends do, so
 * there is something to look at; closing it (or F12 inside it) releases the
 * engine, which lets the machine run on. */
void coco_debugger_show(HWND parent, cocosession *session);

/* F12: show, or hide and let the machine run. */
void coco_debugger_toggle(HWND parent, cocosession *session);
int  coco_debugger_visible(void);

/* Called from the frontend's message pump before TranslateMessage so the
 * debugger's function-key accelerators work while a child control (an edit
 * field, say) has the focus. Returns 1 when the message was consumed. */
int coco_debugger_pretranslate(MSG *msg);

#endif /* COCO_WIN_DBG_WINDOW_H */
