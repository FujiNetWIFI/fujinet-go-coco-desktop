/*
 * CocoDisplay: the emulator video widget for the GNOME frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "cocosession.h"

G_BEGIN_DECLS

#define COCO_TYPE_DISPLAY (coco_display_get_type())
G_DECLARE_FINAL_TYPE(CocoDisplay, coco_display, COCO, DISPLAY, GtkWidget)

typedef enum {
    /* Default: what the machine looked like on a television. XRoar's viewport
     * is whatever the video mode renders -- 720x270 for a CoCo 3 text screen
     * -- which is not 4:3, because CoCo pixels are not square. */
    COCO_ASPECT_TV_4_3 = 0,
    /* One emulated pixel per display pixel, i.e. the frame's own ratio. */
    COCO_ASPECT_SQUARE_PIXELS = 1,
    COCO_ASPECT_INTEGER = 2,
} CocoAspectMode;

GtkWidget *coco_display_new(cocosession *session);
void coco_display_set_aspect_mode(CocoDisplay *self, CocoAspectMode mode);
void coco_display_set_smooth(CocoDisplay *self, gboolean smooth);

G_END_DECLS
