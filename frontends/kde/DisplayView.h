/*
 * What the main window needs from its display, so the GPU display and the
 * software fallback are interchangeable.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

class QWidget;

/* Same order as the GNOME frontend's CocoAspectMode -- the mode is stored in
 * the shared settings, so the two must agree. */
enum CocoAspect {
    CocoAspectTv43 = 0,
    CocoAspectSquarePixels = 1,
    CocoAspectInteger = 2,
};

class DisplayView {
public:
    virtual ~DisplayView() = default;
    virtual QWidget *widget() = 0;
    virtual void setAspectMode(int mode) = 0;
    virtual void setSmooth(bool smooth) = 0;
};
