/*
 * The emulator picture -- see display.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef COCO_WIN_DISPLAY_H
#define COCO_WIN_DISPLAY_H

#include <windows.h>

#include "cocosession.h"

/* Creates the picture as a child of parent (size it with MoveWindow) and
 * starts its render thread. One per process. */
HWND coco_display_create(HWND parent, cocosession *session);
/* Stops the render thread; call before the parent is destroyed. */
void coco_display_destroy(void);
/* aspect: 0 TV (4:3), 1 square pixels, 2 integer scale; smooth: bilinear
 * (else sharp pixels). Safe from any thread. */
void coco_display_set_options(int aspect, int smooth);
/* "Direct3D 11", "Direct3D 11 (WARP)" or "GDI": what is drawing. */
const char *coco_display_renderer(void);

#endif
