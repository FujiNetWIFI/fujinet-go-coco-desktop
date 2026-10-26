/*
 * The Win32 ROMs window -- see prefs.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef COCO_WIN_PREFS_H
#define COCO_WIN_PREFS_H

#include <windows.h>

#include "cocosession.h"

/* Imports the BIOS: one file dialog (several files) or a folder, each path
 * through cocosession_rom_import, then one "BIOS Imported" / "Nothing
 * Imported" message. Returns how many paths imported, or -1 if cancelled. */
int coco_import_bios(HWND owner, cocosession *session, int folder);

/* Called after an import from the ROMs window, so the main window can show
 * its picture (and start the machine) at once. */
typedef void (*coco_prefs_imported_fn)(void);

/* Opens (or raises) the ROMs window. */
void coco_roms_show(HWND parent, cocosession *session, coco_prefs_imported_fn imported);

#endif
