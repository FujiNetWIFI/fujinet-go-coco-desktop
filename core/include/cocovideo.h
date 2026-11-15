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
#include "cocovidcap.h"

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

/* ---- one scanline, from a captured record (cocovidcap.h) -------------------
 * Draws a line from the registers and bytes the video taps recorded for it,
 * independent of the machine, so the Video tab can show a frame whose mode,
 * palette or address changed halfway down. */
typedef struct {
    int width;          /* pixels written to codes[] (256 on a CoCo 1/2 and in the GIME's compatible mode; the
                           mode's own width in its native modes); 0 when the line is not a display line */
    int native_w;       /* the mode's own pixels across (width may repeat them) */
    int nbytes;         /* bytes of the fetch the line shows */
    int text;           /* alpha, semigraphics or GIME text */
    int attr;           /* GIME text with an attribute byte after each character */
    int bits;           /* bits per pixel of a graphics mode, else 0 */
    char mode[64];      /* "RG6 256 x 192, 2 colours" */
    int pal_n;          /* entries of pal in use: 12 (VDG colours) or 16 (GIME palette slots) */
    uint32_t pal[16];   /* code -> 0xFFRRGGBB */
} cocovideo_line_info;

/* Fills codes[0..width) with one colour code per pixel (a VDG colour,
 * enum vdg_colour, or a GIME palette slot 0-15; info->pal resolves them) and
 * returns the width, or 0 when the record is not a display line. `max` is the
 * room in codes (at least 256; COCOVIDEO_MAX_WIDTH covers every mode). */
int cocovideo_line_decode(const cocovid_line *line, uint8_t *codes, int max,
                          cocovideo_line_info *info);

/* Which fetched byte drew pixel x of a decoded line: its index into
 * line->data, or -1; *sub gets the pixel's position inside that byte's span
 * (0 = leftmost). */
int cocovideo_line_byte_at(const cocovid_line *line,
                           const cocovideo_line_info *info, int x, int *sub);

/* How far the chip's fetch address advances from one new row to the next (0
 * for a line that is not a display line). A line that repeats its row starts
 * at the same address; a scroll or a split shows as any other start. */
int cocovideo_line_stride(const cocovid_line *line);

#ifdef __cplusplus
}
#endif

#endif /* COCOVIDEO_H */
