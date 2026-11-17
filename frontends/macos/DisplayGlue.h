/*
 * DisplayGlue.h -- what DisplayView.m needs to know about THIS machine: the
 * session's functions, the frame size and the shape of the picture.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdatomic.h>

#include "cocosession.h"

#define FN_VIEW      DisplayView
#define FN_SESSION   cocosession
#define FN_FB_W      COCOSESSION_FB_MAX_WIDTH
#define FN_FB_H      COCOSESSION_FB_MAX_HEIGHT
#define FN_INITIAL_W 800
#define FN_INITIAL_H 600

/* The frame size changes at runtime with the video mode: the frame's own size
 * drives the picture. */
#define FN_VARIABLE_WIDTH 1

/* The keyboard, start(), and the aspect modes (DisplayExtras.inc). */
#define FN_EXTRA_METHODS "DisplayExtras.inc"

/* Whole-number scaling in mode 2. */
#define FN_SNAP_SIZE 1

/* 0: the television's 4:3, 1: square pixels, 2: whole-number scale. */
static _Atomic int s_fn_aspect_mode = 0;

/* NTSC runs ~60 Hz; the session has no refresh-rate call (a PAL machine then
 * simply does not lock to the display, as before). */
static inline int fn_machine_hz(FN_SESSION *s) { (void)s; return 60; }

static inline void fn_notify_vsync(FN_SESSION *s, int64_t ns)
{
    cocosession_notify_vsync(s, ns);
}

static inline int fn_pull_frame_w(FN_SESSION *s, uint32_t *dst, int *width, int *lines, uint64_t *serial)
{
    int w = 0, h = 0;
    if (!cocosession_copy_frame(s, dst, serial, &w, &h) || w <= 0 || h <= 0)
        return 0;
    *width = w;
    *lines = h;
    return 1;
}

static inline double fn_picture_aspect_w(FN_SESSION *s, int width, int lines, BOOL tv)
{
    (void)s; (void)tv;
    return atomic_load(&s_fn_aspect_mode) == 0 ? 4.0 / 3.0 : (double)width / (double)lines;
}

/* Mode 2: the largest whole multiple of the frame that fits (at least 1). */
static inline BOOL fn_snap_size(CGSize avail, int width, int lines, CGSize *out)
{
    if (atomic_load(&s_fn_aspect_mode) != 2) return NO;
    int scale = (int)fmin(avail.width / width, avail.height / lines);
    if (scale < 1) scale = 1;
    *out = CGSizeMake(scale * width, scale * lines);
    return YES;
}
