/*
 * cocovidcap -- what the video chip showed on every scanline of a frame, for
 * the debugger's Video tab (cocovidtab.h). Taken from the video taps while a
 * tab exists (cocovid_host_capture_enable), so a reader sees one consistent
 * frame while the machine runs. A stopped machine is read live: lines the
 * beam has passed carry this frame, the rest the last one's.
 *
 * A line record holds the chip registers as they stood when the line ended and
 * the bytes the chip fetched to draw it, so a picture rebuilt from records
 * shows a mode, palette or address change in the middle of a frame.
 *
 * Lines are numbered as the video output counts them: line 0 is the first
 * render_line after the vertical sync (the VDG's own scanline counter on a
 * CoCo 1/2; the GIME's starts a few lines earlier, so its chip_line differs).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef COCOVIDCAP_H
#define COCOVIDCAP_H

#include <stdint.h>

#include "cocosession.h"

#ifdef __cplusplus
extern "C" {
#endif

#define COCOVID_MAX_LINES 336   /* a PAL GIME frame has 314 */
#define COCOVID_MAX_BYTES 160   /* the most a line fetches: 160 graphics bytes, or 80 text cells with attributes */
#define COCOVID_MAX_PIXELS (COCOSESSION_FB_MAX_WIDTH * COCOSESSION_FB_MAX_HEIGHT)

enum {
    COCOVID_LF_ACTIVE = 1,      /* the chip fetched display bytes on this line */
    COCOVID_LF_MIDLINE = 2      /* the mode changed between the line's start and its end */
};

typedef struct {
    uint8_t valid;              /* the line has a record from this frame or the one before */
    uint8_t fresh;              /* ... from this frame */
    uint8_t flags;              /* COCOVID_LF_* */
    uint8_t gime;               /* a GIME line (CoCo 3); else VDG/SAM */
    uint16_t chip_line;         /* the chip's own scanline counter */
    uint16_t mode0, mode1;      /* mode signature at the line's start and end (VDG: PIA1 B & $F8; GIME: VMODE:VRES) */
    uint32_t stamp;             /* frame number the line was drawn in */
    uint32_t tick;              /* event tick the line ended */

    /* VDG, SAM, PIA1 (CoCo 1/2; PIA1 B also drives the GIME's compatible mode) */
    uint8_t pia_mode;           /* PIA1 B & $F8: bit 7 A/G, 6-4 GM2-0, 3 CSS */
    uint8_t vdg_row;            /* the VDG's row counter during the line, 0-11 */
    uint8_t sam_v, sam_ty;      /* V (addressing mode), TY (map type) */
    uint16_t sam_f;             /* F: the display base, A15..A9 in bits 6..0 */
    uint16_t sam_reg;           /* all SAM bits packed, as get_register() */

    /* GIME (CoCo 3), decoded from $FF90-$FF9F as of the line's end */
    uint8_t g_coco, g_bp, g_moch, g_lpr, g_lpf, g_hres, g_cres, g_hven;
    uint8_t g_vsc, g_row, g_brdr, g_sam_v;
    uint16_t g_sam_f;
    uint8_t g_regs[16];         /* $FF90-$FF9F */
    uint8_t g_pal[16];          /* $FFB0-$FFBF, 6 bits each */
    uint32_t g_y;               /* VOFF: the frame's video base */
    uint32_t g_b;               /* the chip's line address register */
    uint8_t g_x;                /* HOFF: horizontal offset */

    /* the fetch */
    uint32_t addr;              /* where the first byte came from: the SAM's address counter (CoCo 1/2), the GIME's
                                 * 19-bit RAM address (CoCo 3); byte n came from addr + n */
    uint16_t nbytes;            /* bytes fetched, up to COCOVID_MAX_BYTES */
    uint8_t data[COCOVID_MAX_BYTES];
} cocovid_line;

typedef struct cocovid_capture {
    uint64_t serial;            /* 0: nothing captured yet */
    uint32_t frame;             /* the frame the lines belong to: a fresh line has this stamp */
    int gime;                   /* CoCo 3 */
    int nlines;                 /* lines the frame has: lines[0 .. nlines-1] can hold records */
    int stopped;                /* a live fill of a stopped machine (else a finished frame) */
    int has_pixels;             /* pixels[] holds a frame of frame_w x frame_h */
    int frame_w, frame_h;
    int first_line;             /* video line of the frame's row 0 */
    int act_x, act_y, act_w, act_h;   /* the video output's active area: x and w in frame pixels, y in video lines */
    int beam_line;              /* lines the beam has finished this frame (nlines when not stopped) */
    int rows_done;              /* frame rows drawn so far (stopped; else frame_h) */
    uint32_t tick;              /* event tick when captured */
    uint32_t line_ticks;        /* ticks in one line, from the last two lines (0 when unknown) */
    uint32_t beam_ticks;        /* ticks since the last line ended (stopped) */
    cocovid_line lines[COCOVID_MAX_LINES];
    uint32_t pixels[COCOVID_MAX_PIXELS];   /* 0xFFRRGGBB, tightly packed rows of frame_w */
} cocovid_capture;

/* ---- the host side (emulator thread writes, any thread reads) ------------- */

/* Reference-counted: while any user is enabled the machine's video taps feed
 * the capture. Safe from any thread; the taps themselves are put on the chips
 * by the emulator thread, at the next frame. */
void cocovid_host_capture_enable(int on);

/* Copy the newest finished-frame capture into dst when its serial differs from
 * *serial_inout; returns 1 and updates the serial, else 0. */
int  cocovid_host_capture_get(cocovid_capture *dst, uint64_t *serial_inout);

/* Fill dst from a machine that is parked (inside the debugger's hook): lines
 * the beam has passed carry this frame (fresh), the rest the previous one;
 * pixels hold the frame as far as it has been drawn (rows at or past
 * rows_done are the previous frame's). Returns 0 when there is no machine or
 * no line has been captured. */
int  cocovid_host_capture_live(cocovid_capture *dst);

/* Called by cocohost_emit_frame on the emulator thread with each published
 * frame. Snapshots the lines drawn since the last one together with the
 * frame, and keeps the taps on the current machine. */
void cocovid_host_frame(const uint32_t *xrgb8888, int width, int height);

/* The video output has ONE tap slot (machine_debug_video_tap_install); the
 * capture owns it and passes every event on to these subscribers (the
 * debugger's trace uses it for the beam position). add/remove from the
 * emulator thread or while it is parked; at most 4. Subscribers get events
 * only while a capture user is enabled or the subscriber list is not empty. */
struct machine_video_tap;
int  cocovid_host_tap_add(const struct machine_video_tap *tap);
void cocovid_host_tap_remove(const struct machine_video_tap *tap);

#ifdef __cplusplus
}
#endif

#endif
