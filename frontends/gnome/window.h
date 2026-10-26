/*
 * CocoWindow: main window of the GNOME frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "cocosession.h"

G_BEGIN_DECLS

#define COCO_TYPE_WINDOW (coco_window_get_type())
G_DECLARE_FINAL_TYPE(CocoWindow, coco_window, COCO, WINDOW,
                     AdwApplicationWindow)

GtkWidget *coco_window_new(AdwApplication *app, cocosession *session);
void coco_window_toast(CocoWindow *self, const char *message);
void coco_window_restart_session(CocoWindow *self);
/* The picture, or the No-BIOS page when the machine has no ROMs to boot. */
void coco_window_show_page(CocoWindow *self);
/* Starts the machine if it isn't running and its ROMs are there. */
void coco_window_start(CocoWindow *self);
/* A file chooser with a "these" filter (suffixes, any case) and All files. */
GtkFileDialog *coco_file_dialog(const char *title, const char *filter_name,
                                const char *const *exts);

/* main.c: the icon name to use, which differs between an installed app and
 * one running out of the build tree. */
const char *coco_icon_name(void);

G_END_DECLS
