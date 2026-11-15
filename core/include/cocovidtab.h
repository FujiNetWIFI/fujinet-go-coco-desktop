/*
 * cocovidtab -- the Video tab's controller. All four frontends draw the same
 * tab from it, so what they own is widgets: a view and zoom chooser, overlay
 * toggles, a picture, the inspector and four tables. The lessons of the other
 * debuggers' video tabs live here once:
 *
 *   - the machine is only COPIED: a finished frame and what the video chip
 *     drew on every line (the registers and fetched bytes, cocovidcap.h) is
 *     captured once per frame on the machine thread, and a stopped machine is
 *     read while it is parked. Decoding, scaling and overlays run on the
 *     caller's (UI) thread;
 *   - the picture is rebuilt only when something it shows changed, into one
 *     reused buffer;
 *   - hover and pin are state here, so a mouse move never re-renders the
 *     picture: draw the marks from cocovid_tab_marks() over the cached image;
 *   - pixels are 0xFFRRGGBB (opaque), whatever the toolkit does with alpha;
 *   - one table of overlays and one env-var parser, so the frontends cannot
 *     disagree.
 *
 * The VDG and the GIME draw a line from whatever mode registers hold at that
 * moment, so a program can change mode, palette or display address in the
 * middle of a frame. Everything per-line therefore comes from the capture:
 * the mode, the start address of the line's fetch (the SAM's address counter
 * on a CoCo 1/2, the GIME's 19-bit RAM address on a CoCo 3), the bytes and the
 * palette. Lines are numbered as the video output counts them, line 0 being
 * the first one after the vertical sync.
 *
 * Views: Display is the last published frame, border and all; Decoded redraws
 * the display area from each line's own mode, bytes and palette (so a split
 * screen shows both halves); Beam is the live frame of a stopped machine, this
 * frame above the beam and the last one dimmed below it; Values shows each
 * pixel as a flat false colour of its colour code (a VDG colour or a GIME
 * palette slot) so equal pixels read as equal whatever the palette says.
 *
 * UI thread only. Not thread-safe.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef COCOVIDTAB_H
#define COCOVIDTAB_H

#include <stdint.h>

#include "cocodebug.h"
#include "cocovidcap.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cocovid_tab cocovid_tab;

enum cocovid_view {
    COCOVID_V_DISPLAY = 0,      /* the last published frame, borders included */
    COCOVID_V_DECODED,          /* the display area rebuilt from each line's own mode, bytes and palette */
    COCOVID_V_BEAM,             /* the live frame (stopped): this frame above the beam, the last dimmed below */
    COCOVID_V_VALUES,           /* Decoded, each pixel a flat false colour of its colour code */
    COCOVID_V_COUNT
};

/* Overlays: one table, bit per entry. */
enum {
    COCOVID_OV_GRID    = 1u << 0,   /* byte cells in Decoded/Values; single pixels when zoomed in */
    COCOVID_OV_BEAM    = 1u << 1,   /* where the beam is (stopped) */
    COCOVID_OV_ADDRESS = 1u << 2,   /* lines whose fetch address is not the one before's continuation (a bar at the left) */
    COCOVID_OV_MODES   = 1u << 3,   /* lines where the mode or palette changed (a bar at the right and a hairline) */
    COCOVID_OV_SOURCE  = 1u << 4,   /* tint every line by the 512-byte page its fetch starts in */
    COCOVID_OV_ACTIVE  = 1u << 5,   /* outline of the display area (Display, Beam) */
    COCOVID_OV_ALL     = 0x3Fu
};
typedef struct {
    unsigned bit;
    const char *name;           /* the env name: grid, beam, ... */
    const char *label;          /* the checkbox text */
} cocovid_overlay_info;
const cocovid_overlay_info *cocovid_overlays(int *count);
const char *cocovid_view_name(int view);            /* "Display", ... */
const char *cocovid_view_key(int view);             /* "display", ... (env) */

/* Right-hand sections. */
enum { COCOVID_S_PIXEL = 0, COCOVID_S_REGISTERS, COCOVID_S_COLOURS,
       COCOVID_S_LINES, COCOVID_S_TIMING, COCOVID_S_COUNT };
const char *cocovid_section_name(int section);
const char *cocovid_section_key(int section);

/* d is the debugger the tab asks whether the machine is stopped; NULL (tests)
 * makes the machine count as running and every Break on... return -1. The tab
 * asks the host to capture frames until it is freed. */
cocovid_tab *cocovid_tab_new(cocodebug *dbg);
void         cocovid_tab_free(cocovid_tab *tab);

/* ---- what to show (they mark the picture stale) ------------------------------------ */
void     cocovid_tab_set_view(cocovid_tab *tab, int view);
int      cocovid_tab_view(const cocovid_tab *tab);
void     cocovid_tab_set_overlays(cocovid_tab *tab, unsigned ovl);
unsigned cocovid_tab_overlays(const cocovid_tab *tab);
/* 1, 2, 3, 4, 6, 8, 12, 16, or 0 for "fit the viewport" (the largest of those
 * that fits both ways) */
void     cocovid_tab_set_zoom(cocovid_tab *tab, int zoom);
int      cocovid_tab_zoom(const cocovid_tab *tab);
/* the room the picture has, for Fit; call when the widget resizes */
void     cocovid_tab_set_viewport(cocovid_tab *tab, int wide, int high);
/* the zoom steps the chooser offers (0 = Fit first) */
const int *cocovid_zooms(int *count);

/*
 * The scripted-capture variables, read once by cocovid_tab_new:
 *   COCO_DEBUGGER_VIDEO_VIEW      display decoded beam values
 *   COCO_DEBUGGER_VIDEO_OVERLAYS  grid,beam,address,modes,source,active | all | none
 *   COCO_DEBUGGER_VIDEO_ZOOM      1..16 or fit
 *   COCO_DEBUGGER_VIDEO_SECTION   pixel registers colours lines timing   (names, never indexes)
 * Returns the starting section, COCOVID_S_PIXEL when unset.
 */
int cocovid_tab_env_section(const cocovid_tab *tab);

/* ---- refresh ------------------------------------------------------------------------ */
#define COCOVID_TAB_PICTURE 1u  /* repaint the picture (cocovid_tab_picture) */
#define COCOVID_TAB_TABLES  2u  /* refill the status line and the sections */
#define COCOVID_TAB_OFF     4u  /* no machine: show cocovid_tab_why() instead of a picture */

/* Take the machine's state, update the trackers, and say what to redraw.
 * force: redraw everything. */
unsigned cocovid_tab_refresh(cocovid_tab *tab, int force);

/* For tests: use these captures instead of the machine. `finished` is a
 * finished-frame capture (what a running machine shows); `live` is the stopped
 * machine's, or NULL to make the machine count as running. */
void cocovid_tab_feed(cocovid_tab *tab, const cocovid_capture *finished,
                      const cocovid_capture *live);

/* ---- the picture ------------------------------------------------------------------- */
/*
 * The rendered picture, 0xFFRRGGBB, valid until the next refresh or set_*. NULL
 * when this view cannot be drawn: cocovid_tab_why() says why, and the tab
 * should show that text.
 */
const uint32_t *cocovid_tab_picture(cocovid_tab *tab, int *wide, int *high);
const char     *cocovid_tab_why(const cocovid_tab *tab);
int             cocovid_tab_effective_zoom(const cocovid_tab *tab);

/* ---- the tables -------------------------------------------------------------------- */
const char *cocovid_tab_status(const cocovid_tab *tab);        /* two lines, '\n' between */

typedef struct {
    const char *name;           /* "MODE", "PIA1 B", "VMODE", ... */
    char value[32];             /* "$F8" */
    char meaning[112];
    int age;                    /* refreshes since it last changed (COCOVID_REG_FADE+: not recent) */
} cocovid_reg;
#define COCOVID_REG_FADE 12
int cocovid_tab_regs(const cocovid_tab *tab, const cocovid_reg **out);

/* The colours the display can use: the VDG's twelve, or the GIME's sixteen
 * palette slots (as the reference line has them: the last display line). */
typedef struct {
    int index;                  /* the colour code: a VDG colour or a palette slot */
    char name[24];              /* "Green" or "Slot 5" */
    char detail[56];            /* the GIME's 6-bit palette value, decoded; empty for the VDG */
    uint32_t rgb;               /* 0xFFRRGGBB */
    long uses;                  /* pixels of the display area with this code */
    int inspected;              /* the inspected pixel has it */
} cocovid_swatch;
int cocovid_tab_swatches(const cocovid_tab *tab, const cocovid_swatch **out);

/* The per-line table: one entry for every line that has a record (the frame's
 * or the last one's) and every line of the display area. */
enum { COCOVID_LC_ADDR = 1,     /* the fetch did not start where the line above's continuation would */
       COCOVID_LC_MODE = 2,     /* a different mode */
       COCOVID_LC_PALETTE = 4,  /* a different GIME palette */
       COCOVID_LC_SIZE = 8,     /* a different byte count */
       COCOVID_LC_MID = 16 };   /* the mode changed during the line itself */
typedef struct {
    int row;                    /* video line, 0 = first after the vertical sync */
    int chip_line;              /* the chip's own scanline counter */
    int display;                /* a display line (the chip fetched bytes) */
    int valid;                  /* a record exists */
    int fresh;                  /* from this frame (else the previous one's) */
    char mode[64];              /* "RG6 256 x 192, 2 colours", "border" */
    unsigned addr;              /* where the fetch started (SAM address, or GIME physical RAM address) */
    int nbytes;                 /* bytes fetched */
    uint8_t data[8];            /* the first of them */
    int changed_mask;           /* COCOVID_LC_* against the previous row listed */
} cocovid_line_row;
int cocovid_tab_lines(const cocovid_tab *tab, const cocovid_line_row **out);

/* the timing section, as ready-made "label\tvalue" lines */
int  cocovid_tab_timing(const cocovid_tab *tab, const char *const **lines);
/* where the beam is in the whole frame, 0..1000 (0 when unknown) */
int  cocovid_tab_beam_permille(const cocovid_tab *tab);

/* A section as plain text (the console's `video [section]` shares it). */
int  cocovid_tab_text(cocovid_tab *tab, int section, char *dst, int dstsz);
/* The debugger console's `video [section]` (see cocodebug_set_video_text). */
int  cocovid_console_text(cocodebug *dbg, const char *section, char *dst, int dstsz);
/* A section by name or prefix ("pixel", "reg", ...); -1 if none. */
int  cocovid_section_from_name(const char *name);

/* ---- hover, pin ---------------------------------------------------------------------- */
typedef struct {
    int valid;
    int picx, picy;             /* picture pixel (at 1:1, before zoom) */
    int line;                   /* video line, -1 if the picture has none there */
    int chip_line;
    int column;                 /* x in the decoded line (0..width-1), -1 when the pixel is not in a display area */
    int has_byte;               /* a fetched byte drew this pixel */
    int index;                  /* which byte of the line's fetch */
    int sub;                    /* the pixel's position inside that byte's span, 0 = leftmost */
    int phys;                   /* addr is a GIME physical RAM address (CoCo 3); else a CPU address */
    unsigned addr;              /* where the byte came from */
    uint8_t value;              /* the byte */
    int code;                   /* the pixel's colour code, -1 if unknown */
    int fresh;                  /* from this frame, not the last one's */
    uint32_t rgb;               /* 0xFFRRGGBB as drawn in the picture */
    char mode[64];
    char text[640];             /* ready-made inspector text */
} cocovid_pixel;

/* x, y in picture pixels at the current zoom; -1,-1 when the pointer left.
 * Returns 1 when the inspector changed. */
int  cocovid_tab_hover(cocovid_tab *tab, int posx, int posy);
void cocovid_tab_pin(cocovid_tab *tab, int posx, int posy);      /* a click; the same spot unpins */
void cocovid_tab_unpin(cocovid_tab *tab);
/* the pinned pixel if any, else the hovered one, else NULL */
const cocovid_pixel *cocovid_tab_pixel(const cocovid_tab *tab);
/* rectangles x,y,w,h in picture pixels for the marks drawn over the picture;
 * bit0 of the result: hover valid, bit1: pin valid */
int  cocovid_tab_marks(const cocovid_tab *tab, int hover[4], int pin[4]);

/*
 * "Break on writes": a write breakpoint on the RAM byte the inspected pixel
 * was fetched from (the CPU address on a CoCo 1/2, the physical RAM address
 * on a CoCo 3: a program may reach the same byte through a different MMU
 * mapping), through cocodebug_breakpoint_add(COCODEBUG_BP_WRITE [|
 * COCODEBUG_BP_PHYS]) unless a handler is set. A write breakpoint that already
 * covers the byte is returned instead of a second one. Returns the
 * breakpoint's id (>= 0), or -1 (no pixel, no byte under it, no debugger, or
 * the debugger refused).
 */
int  cocovid_tab_break_on_pixel(cocovid_tab *tab);
/* 1 when "Break on writes" has a byte to break on now */
int  cocovid_tab_can_break(const cocovid_tab *tab);
/* Replace the default: fn(ctx, phys, address) adds the breakpoint and returns
 * >= 0, or -1. Tests use it to see what would be asked for. */
typedef int (*cocovid_break_fn)(void *ctx, int phys, unsigned address);
void cocovid_tab_set_break_handler(cocovid_tab *tab, cocovid_break_fn fn, void *ctx);

#ifdef __cplusplus
}
#endif

#endif
