/*
 * cocovideo -- decodes the CoCo's screen contents for the debugger's video
 * pane, from raw memory and hardware register state, independent of XRoar's
 * own live renderer (core/xroar/desktop/ui_desktop.c).
 *
 * Reuses none of XRoar's rendering code on purpose: that code line-renders in
 * real time as the machine runs, driven by VDG/GIME timing events, and none
 * of its internal mode state is exposed publicly (MC6847's mode bits are
 * private to mc6847.c with no getter). This instead re-derives "what is on
 * screen" from the same registers the machine itself reads, all of which
 * *are* public:
 *
 *   - CoCo 1/2 (VDG + SAM): the VDG's mode bits (A/G, GM2-0, CSS) are what
 *     src/dragon/dragon.c's dragon_update_vdg_mode() reads from PIA1 side B
 *     (struct MC6821, fully public) to drive the real VDG -- read the same
 *     way here. The video RAM base address comes from the SAM's F register,
 *     read via struct MC6883's public get_register().
 *   - CoCo 3 (GIME): see the GIME half of this file for its own register
 *     access.
 *
 * Colours are a simplified flat RGB approximation of the 12 VDG colours
 * (enum vdg_colour in mc6847.h), not XRoar's NTSC-composite-voltage palette
 * (vdg_palette.h) -- this is a debugging aid, not a second video renderer,
 * so exact composite artifacting is out of scope.
 *
 * THREADING / lifetime: same rule as the rest of cocodebug.h -- only valid
 * while the machine is paused, since it reads the machine's memory and
 * component state from a UI thread.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef COCOVIDEO_H
#define COCOVIDEO_H

#include <stdint.h>

#include "cocosession.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Generous enough for every VDG and GIME mode's native resolution; reuses
 * the session frame store's own bound (core/include/cocosession.h) rather
 * than inventing a second one. */
#define COCOVIDEO_MAX_WIDTH  COCOSESSION_FB_MAX_WIDTH
#define COCOVIDEO_MAX_HEIGHT COCOSESSION_FB_MAX_HEIGHT

/* Decodes the current screen into an XRGB8888 buffer (caller-owned, row
 * major, pitch == the returned width -- same convention as
 * cocohost_emit_frame's frame store). out_w and out_h receive the native
 * resolution of the current mode, which varies (eg 256x192 for CoCo 1/2
 * RG6, 64x64 for CG1, up to COCOVIDEO_MAX_WIDTH/HEIGHT on a CoCo 3 GIME
 * high-resolution mode).
 *
 * Returns 1 on success. Returns 0 -- and touches nothing -- when not paused,
 * same as cocodebug_read/cocodebug_disassemble, or when the running machine
 * has no video hardware this can identify. */
int cocodebug_video_decode(cocodebug *d, uint32_t *pixels, int *out_w,
                           int *out_h);

#ifdef __cplusplus
}
#endif

#endif /* COCOVIDEO_H */
