/*
 * vidtab_test -- the Video tab's controller (core/include/cocovidtab.h) against
 * synthetic captures: no machine, so the cases the real screen rarely shows
 * can be built exactly.
 *
 *   - a CoCo 1/2 frame split in two modes: text above, RG6 below. Decoded
 *     draws each half in its own mode, the lines table flags the change, the
 *     inspector names the mode, the byte and its address of whichever half is
 *     under the pointer;
 *   - a CoCo 3 frame whose palette changes halfway: the same colour code is
 *     two different colours, and the Colours section and Lines table say so;
 *   - lines of different widths (640 above, 320 below) share one canvas;
 *   - Display shows the published frame exactly; Beam needs a stopped machine
 *     and dims what the beam has not redrawn yet;
 *   - Break on writes asks for the byte's address (CPU on a CoCo 1/2,
 *     physical on a CoCo 3);
 *   - random captures never crash any view, section or inspector call.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cocovideo.h"
#include "cocovidcap.h"
#include "cocovidtab.h"

static int failures;

static void check(int good, const char *what)
{
    printf("%s: %s\n", good ? "ok" : "FAIL", what);
    if (!good) failures++;
}

/* big: tabs hold copies, the test holds the originals */
static cocovid_capture g_finished, g_live;

static uint32_t g_rand = 12345u;
static uint32_t rnd(void)
{
    g_rand = g_rand * 1664525u + 1013904223u;
    return g_rand >> 8;
}

#define VDG_FIRST 38
#define VDG_LINES 192
#define VDG_TOTAL 262

/* A display line of a CoCo 1/2: PIA1 B mode bits, the fetched bytes. */
static cocovid_line *set_vdg_line(cocovid_capture *cap, int vline, unsigned pia_mode,
                                  unsigned addr, int row, uint8_t fill)
{
    cocovid_line *rec = &cap->lines[vline];
    memset(rec, 0, sizeof *rec);
    rec->valid = rec->fresh = 1;
    rec->flags = COCOVID_LF_ACTIVE;
    rec->chip_line = (uint16_t)vline;
    rec->stamp = cap->frame;
    rec->mode0 = rec->mode1 = (uint16_t)pia_mode;
    rec->pia_mode = (uint8_t)pia_mode;
    rec->vdg_row = (uint8_t)row;
    rec->sam_v = 6;
    rec->addr = addr;
    rec->nbytes = 42;
    memset(rec->data, fill, 42);
    return rec;
}

static void blank_capture(cocovid_capture *cap, int gime)
{
    memset(cap, 0, sizeof *cap);
    cap->serial = 1;
    cap->frame = 5;
    cap->gime = gime;
    cap->nlines = VDG_TOTAL;
    cap->beam_line = VDG_TOTAL;
    cap->frame_w = 640;
    cap->frame_h = 240;
    cap->has_pixels = 1;
    cap->first_line = 32;
    cap->act_x = 64;
    cap->act_w = 512;
    cap->act_y = VDG_FIRST;
    cap->act_h = VDG_LINES;
    cap->rows_done = 240;
    for (int pix = 0; pix < 640 * 240; pix++)
        cap->pixels[pix] = 0xff000000u | (uint32_t)(pix * 2654435761u >> 8);
}

/* Text above (alpha, 32 bytes a row from $0400), RG6 below (32 bytes a row from
 * $0600 up): the screen mode changes after the 96th display line. */
static void split_vdg(cocovid_capture *cap)
{
    blank_capture(cap, 0);
    for (int idx = 0; idx < VDG_LINES; idx++) {
        const int vline = VDG_FIRST + idx;
        cocovid_line *rec;
        if (idx < 96)
            rec = set_vdg_line(cap, vline, 0x00, 0x0400u + (unsigned)(idx / 12) * 32u, idx % 12, 0x01);
        else
            rec = set_vdg_line(cap, vline, 0xF0, 0x0600u + (unsigned)(idx - 96) * 32u, 0, 0xAA);
        if (idx == 96)
            rec->flags |= COCOVID_LF_MIDLINE;
    }
}

/* RGB of the VDG colours the decoder uses (cocovideo.c vdg_rgb) */
#define RG6_ON  0xff1dd12bu     /* green */
#define RG6_OFF 0xff0e6916u     /* dark green */

static int g_break_calls, g_break_phys;
static unsigned g_break_addr;
static int on_break(void *ctx, int phys, unsigned address)
{
    (void)ctx;
    g_break_calls++;
    g_break_phys = phys;
    g_break_addr = address;
    return 7;
}

static void test_vdg_split(void)
{
    cocovid_tab *tab = cocovid_tab_new(NULL);
    const uint32_t *pic;
    int wide, high;
    const cocovid_pixel *pixp;
    const cocovid_line_row *rows;
    int nrows, found_change = 0;
    char text[8192];

    check(tab != NULL, "VDG: tab");
    split_vdg(&g_finished);
    cocovid_tab_feed(tab, &g_finished, NULL);
    cocovid_tab_set_view(tab, COCOVID_V_DECODED);
    cocovid_tab_set_zoom(tab, 1);
    cocovid_tab_set_overlays(tab, 0);
    check((cocovid_tab_refresh(tab, 1) & COCOVID_TAB_PICTURE) != 0, "VDG: refresh");
    pic = cocovid_tab_picture(tab, &wide, &high);
    check(pic && wide == 256 && high == 192, "VDG: Decoded is 256 x 192");
    if (pic && wide == 256 && high == 192) {
        int top_fg = 0, top_bg = 0, bottom_ok = 1;
        for (int posy = 0; posy < 96; posy++)
            for (int posx = 0; posx < 256; posx++) {
                top_fg += pic[posy * 256 + posx] == 0xff1dd12bu;      /* alpha: bright green glyph */
                top_bg += pic[posy * 256 + posx] == 0xff0e6916u;      /* ... on dark green */
            }
        check(top_fg > 1000 && top_bg > 1000, "VDG: the upper half is text (a glyph in two colours)");
        for (int posy = 96; posy < 192 && bottom_ok; posy++)
            for (int posx = 0; posx < 256; posx++)
                if (pic[posy * 256 + posx] != ((posx & 1) ? RG6_OFF : RG6_ON)) { bottom_ok = 0; break; }
        check(bottom_ok, "VDG: the lower half is RG6 ($AA: every other pixel lit)");
    }

    /* the lines table flags the change, the registers count it */
    nrows = cocovid_tab_lines(tab, &rows);
    for (int i = 0; i < nrows; i++)
        if (rows[i].row == VDG_FIRST + 96 && (rows[i].changed_mask & COCOVID_LC_MODE))
            found_change = 1;
    check(nrows >= 192 && found_change, "VDG: the Lines table marks the mode change at the 96th display line");
    check(cocovid_tab_text(tab, COCOVID_S_LINES, text, sizeof text) > 500 && strstr(text, "mode"),
          "VDG: Lines text names the change");
    check(cocovid_tab_text(tab, COCOVID_S_TIMING, text, sizeof text) > 20 && strstr(text, "Mode changes\t1 line"),
          "VDG: Timing counts one mode change");

    /* inspector: text half, then the graphics half */
    cocovid_tab_hover(tab, 8 * 3 + 2, 10);         /* the 4th character of the first row */
    pixp = cocovid_tab_pixel(tab);
    check(pixp && pixp->has_byte && pixp->index == 3 && pixp->addr == 0x0403 && !pixp->phys &&
          strstr(pixp->mode, "Alpha"), "VDG: hovering the text names byte 3 at $0403 in Alpha mode");
    cocovid_tab_hover(tab, 9, 100 + 3);            /* graphics row 4 of the lower half, byte 1 */
    pixp = cocovid_tab_pixel(tab);
    check(pixp && pixp->has_byte && pixp->index == 1 && pixp->addr == 0x0600u + 7u * 32u + 1u &&
          pixp->value == 0xAA && strstr(pixp->mode, "RG6"),
          "VDG: hovering the lower half names RG6, byte 1 at $06E1 = $AA");
    check(pixp && pixp->line == VDG_FIRST + 96 + 7, "VDG: ... on video line 141");

    /* Break on writes: the byte's CPU address */
    cocovid_tab_set_break_handler(tab, on_break, NULL);
    check(cocovid_tab_can_break(tab), "VDG: a pixel with a byte can break");
    check(cocovid_tab_break_on_pixel(tab) == 7 && g_break_calls == 1 && !g_break_phys &&
          g_break_addr == 0x06E1u, "VDG: Break on writes asks for CPU $06E1");
    cocovid_tab_hover(tab, -1, -1);
    cocovid_tab_unpin(tab);
    check(!cocovid_tab_can_break(tab) && cocovid_tab_break_on_pixel(tab) == -1,
          "VDG: no pixel, no breakpoint");

    /* Values: equal codes, equal colours; text and graphics both read */
    cocovid_tab_set_view(tab, COCOVID_V_VALUES);
    cocovid_tab_refresh(tab, 1);
    pic = cocovid_tab_picture(tab, &wide, &high);
    check(pic && wide == 256 && high == 192 && pic[96 * 256 + 0] != pic[96 * 256 + 1] &&
          pic[96 * 256 + 0] == pic[96 * 256 + 2] && pic[96 * 256 + 1] == pic[96 * 256 + 3],
          "VDG: Values paints the two RG6 codes in two flat colours");

    /* Colours: twelve VDG colours, the usage counts add up to the decoded pixels */
    {
        const cocovid_swatch *swat;
        long total = 0;
        const int count = cocovid_tab_swatches(tab, &swat);
        for (int i = 0; i < count; i++) total += swat[i].uses;
        check(count == 12 && total == 256L * 192L, "VDG: 12 colours whose uses sum to the 256 x 192 pixels");
    }
    cocovid_tab_free(tab);
}

/* CoCo 3, GIME native 320 x 16 colours: HRES 7, CRES 2 = 160 bytes a row, 4 bits
 * a pixel; every byte $11 so every pixel is palette slot 1. */
static cocovid_line *set_gime_line(cocovid_capture *cap, int vline, int hres, int cres, int bp,
                                   unsigned addr, uint8_t fill)
{
    cocovid_line *rec = &cap->lines[vline];
    memset(rec, 0, sizeof *rec);
    rec->valid = rec->fresh = 1;
    rec->gime = 1;
    rec->flags = COCOVID_LF_ACTIVE;
    rec->chip_line = (uint16_t)(vline + 4);
    rec->stamp = cap->frame;
    rec->g_bp = (uint8_t)bp;
    rec->g_hres = (uint8_t)hres;
    rec->g_cres = (uint8_t)cres;
    rec->mode0 = rec->mode1 = (uint16_t)(0x80 | hres << 2 | cres);
    rec->addr = addr;
    rec->g_b = addr;
    rec->nbytes = 160;
    memset(rec->data, fill, 160);
    for (int slot = 0; slot < 16; slot++)
        rec->g_pal[slot] = (uint8_t)slot;
    return rec;
}

static void test_gime_palette_and_widths(void)
{
    cocovid_tab *tab = cocovid_tab_new(NULL);
    const uint32_t *pic;
    int wide, high;
    const cocovid_line_row *rows;
    int nrows, palette_marked = 0;

    blank_capture(&g_finished, 1);
    g_finished.act_x = 0;
    g_finished.act_w = 640;
    for (int idx = 0; idx < 200; idx++) {
        const int vline = 30 + idx;
        cocovid_line *rec;
        if (idx < 100) {
            /* 640 pixels wide, two colours: HRES 5 = 80 bytes of 8 pixels */
            rec = set_gime_line(&g_finished, vline, 5, 0, 1, 0x10000u + (unsigned)idx * 80u, 0xF0);
            rec->nbytes = 80;
        } else {
            rec = set_gime_line(&g_finished, vline, 7, 2, 1, 0x20000u + (unsigned)idx * 160u, 0x11);
            if (idx >= 150)
                rec->g_pal[1] = 0x3f;           /* palette slot 1 turns white */
        }
    }
    cocovid_tab_feed(tab, &g_finished, NULL);
    cocovid_tab_set_view(tab, COCOVID_V_DECODED);
    cocovid_tab_set_zoom(tab, 1);
    cocovid_tab_set_overlays(tab, 0);
    cocovid_tab_refresh(tab, 1);
    pic = cocovid_tab_picture(tab, &wide, &high);
    check(pic && wide == 640 && high == 200, "GIME: Decoded takes the widest line's width (640 x 200)");
    if (pic && wide == 640 && high == 200) {
        /* $F0 at 1 bit a pixel: 4 lit, 4 dark; palette slots 1 and 0 */
        check(pic[10 * 640 + 0] != pic[10 * 640 + 4] && pic[10 * 640 + 0] == pic[10 * 640 + 1],
              "GIME: the 640-wide half shows $F0 as four lit, four dark pixels");
        check(pic[120 * 640 + 0] == pic[120 * 640 + 1] && pic[120 * 640 + 0] == pic[120 * 640 + 2] &&
              pic[120 * 640 + 0] == pic[120 * 640 + 639],
              "GIME: the 320-wide half is stretched over the canvas, a flat slot 1");
        check(pic[120 * 640] != pic[170 * 640] && (pic[170 * 640] & 0xffffff) == 0xebebeb,
              "GIME: after the palette change the same code is the palette's brightest");
    }
    nrows = cocovid_tab_lines(tab, &rows);
    for (int i = 0; i < nrows; i++)
        if (rows[i].row == 30 + 150 && (rows[i].changed_mask & COCOVID_LC_PALETTE)) palette_marked = 1;
    check(palette_marked, "GIME: the Lines table marks the palette change");

    /* the inspector in the stretched half: pixel 640 of 640 is byte 159 of 160 */
    cocovid_tab_hover(tab, 639, 120);
    {
        const cocovid_pixel *pixp = cocovid_tab_pixel(tab);
        check(pixp && pixp->has_byte && pixp->index == 159 && pixp->phys && pixp->code == 1 &&
              pixp->addr == 0x20000u + 120u * 160u + 159u, "GIME: last pixel is byte 159 at its physical RAM address, code 1");
    }
    cocovid_tab_set_break_handler(tab, on_break, NULL);
    g_break_calls = 0;
    check(cocovid_tab_break_on_pixel(tab) == 7 && g_break_phys == 1 && g_break_addr == 0x20000u + 120u * 160u + 159u,
          "GIME: Break on writes asks for the physical address");
    {
        const cocovid_swatch *swat;
        check(cocovid_tab_swatches(tab, &swat) == 16, "GIME: 16 palette slots in Colours");
    }

    /* Display view shows the published frame as it is */
    cocovid_tab_set_view(tab, COCOVID_V_DISPLAY);
    cocovid_tab_refresh(tab, 1);
    pic = cocovid_tab_picture(tab, &wide, &high);
    check(pic && wide == 640 && high == 240 && !memcmp(pic, g_finished.pixels, 640u * 240u * 4u),
          "Display is exactly the published frame");
    cocovid_tab_free(tab);
}

static void test_beam_and_display_hover(void)
{
    cocovid_tab *tab = cocovid_tab_new(NULL);
    const uint32_t *pic;
    int wide, high;
    const cocovid_pixel *pixp;

    split_vdg(&g_finished);
    split_vdg(&g_live);
    g_live.stopped = 1;
    g_live.beam_line = VDG_FIRST + 100;
    g_live.rows_done = 106 - g_live.first_line + 32;      /* a frame row */
    g_live.rows_done = 100;
    for (int idx = 100; idx < VDG_LINES; idx++)
        g_live.lines[VDG_FIRST + idx].fresh = 0;

    cocovid_tab_set_view(tab, COCOVID_V_BEAM);
    cocovid_tab_set_zoom(tab, 1);
    cocovid_tab_set_overlays(tab, 0);
    cocovid_tab_feed(tab, &g_finished, NULL);
    cocovid_tab_refresh(tab, 1);
    check(cocovid_tab_picture(tab, &wide, &high) == NULL && strlen(cocovid_tab_why(tab)) > 20,
          "Beam while running says why");

    cocovid_tab_feed(tab, &g_finished, &g_live);
    cocovid_tab_refresh(tab, 1);
    pic = cocovid_tab_picture(tab, &wide, &high);
    check(pic && wide == 640 && high == 240, "Beam while stopped is the live frame");
    if (pic && wide == 640 && high == 240) {
        check(pic[50 * 640 + 7] == (g_live.pixels[50 * 640 + 7] | 0xff000000u),
              "Beam: rows the beam has drawn are as they are");
        check(pic[150 * 640 + 7] != (g_live.pixels[150 * 640 + 7] | 0xff000000u),
              "Beam: rows below the beam are dimmed");
    }
    check(cocovid_tab_beam_permille(tab) > 300 && cocovid_tab_beam_permille(tab) < 600,
          "Beam position is reported");

    /* Display's inspector: frame row -> video line -> the line's own record */
    cocovid_tab_set_view(tab, COCOVID_V_DISPLAY);
    cocovid_tab_refresh(tab, 1);
    cocovid_tab_hover(tab, 64 + 2 * 8 * 2 + 3, VDG_FIRST + 10 - 32);    /* 3rd character cell, 2 units a pixel */
    pixp = cocovid_tab_pixel(tab);
    check(pixp && pixp->line == VDG_FIRST + 10 && pixp->has_byte && pixp->index == 2 && pixp->addr == 0x0402u,
          "Display: a frame pixel maps to its line and byte through the active area");
    cocovid_tab_hover(tab, 5, VDG_FIRST + 10 - 32);
    pixp = cocovid_tab_pixel(tab);
    check(pixp && pixp->valid && !pixp->has_byte && strstr(pixp->text, "outside"),
          "Display: the border has no byte");
    cocovid_tab_hover(tab, -1, -1);
    cocovid_tab_pin(tab, 64 + 8, 70);
    check(cocovid_tab_pixel(tab) && cocovid_tab_pixel(tab)->picx == 72, "pin holds the clicked pixel");
    cocovid_tab_pin(tab, 64 + 8, 70);
    check(cocovid_tab_pixel(tab) == NULL, "the same spot unpins");
    cocovid_tab_free(tab);
}

static void test_nothing_yet(void)
{
    cocovid_tab *tab = cocovid_tab_new(NULL);
    char text[512];
    int wide, high;

    cocovid_tab_set_view(tab, COCOVID_V_DECODED);
    cocovid_tab_feed(tab, NULL, NULL);
    check((cocovid_tab_refresh(tab, 1) & COCOVID_TAB_OFF) != 0, "no capture at all: the tab is off");
    check(cocovid_tab_picture(tab, &wide, &high) == NULL && strlen(cocovid_tab_why(tab)) > 10, "...with a reason");
    check(cocovid_tab_text(tab, COCOVID_S_LINES, text, sizeof text) > 0, "...and the console text still answers");
    cocovid_tab_free(tab);
}

/* random records: every field, in range or not */
static void fuzz_capture(cocovid_capture *cap, int gime)
{
    blank_capture(cap, gime);
    cap->frame_w = 1 + (int)(rnd() % 800);
    cap->frame_h = 1 + (int)(rnd() % 300);
    cap->act_x = (int)(rnd() % 900) - 50;
    cap->act_w = (int)(rnd() % 900);
    cap->first_line = (int)(rnd() % 400) - 20;
    cap->rows_done = (int)(rnd() % 400);
    cap->beam_line = (int)(rnd() % 400);
    cap->nlines = (int)(rnd() % 400);
    for (int vline = 0; vline < COCOVID_MAX_LINES; vline++) {
        cocovid_line *rec = &cap->lines[vline];
        uint8_t *raw = (uint8_t *)rec;
        if (rnd() % 5 == 0) {
            memset(rec, 0, sizeof *rec);
            continue;
        }
        for (size_t i = 0; i < sizeof *rec; i++) raw[i] = (uint8_t)rnd();
        rec->valid = rnd() % 8 != 0;
        rec->fresh = rnd() & 1;
        rec->gime = (uint8_t)gime;
        rec->flags = (uint8_t)(rnd() % 4);
        rec->nbytes = (uint16_t)(rnd() % 400);
        rec->g_hven &= 1;
        rec->g_coco &= 1;
        rec->g_bp &= 1;
    }
}

static void test_fuzz(void)
{
    cocovid_tab *tab = cocovid_tab_new(NULL);
    char text[16384];
    int survived = 0;

    for (int iter = 0; iter < 400; iter++) {
        static cocovid_capture live;
        const int gime = iter & 1;
        fuzz_capture(&g_finished, gime);
        fuzz_capture(&live, gime);
        live.stopped = 1;
        cocovid_tab_feed(tab, &g_finished, (iter % 3) ? &live : NULL);
        cocovid_tab_set_overlays(tab, rnd() & COCOVID_OV_ALL);
        cocovid_tab_set_zoom(tab, (int)(rnd() % 4) == 0 ? 0 : 1 + (int)(rnd() % 3));
        cocovid_tab_set_viewport(tab, 1 + (int)(rnd() % 900), 1 + (int)(rnd() % 700));
        for (int view = 0; view < COCOVID_V_COUNT; view++) {
            int wide, high;
            const uint32_t *pic;
            cocovid_tab_set_view(tab, view);
            cocovid_tab_refresh(tab, 1);
            pic = cocovid_tab_picture(tab, &wide, &high);
            if (pic) {
                int hits = 0;
                for (int probe = 0; probe < 20; probe++) {
                    cocovid_tab_hover(tab, (int)(rnd() % (unsigned)(wide + 40)) - 20,
                                      (int)(rnd() % (unsigned)(high + 40)) - 20);
                    hits += cocovid_tab_pixel(tab) != NULL;
                    if (probe == 5) cocovid_tab_pin(tab, (int)(rnd() % (unsigned)wide), (int)(rnd() % (unsigned)high));
                }
                (void)hits;
                cocovid_tab_break_on_pixel(tab);
            }
            for (int sec = 0; sec < COCOVID_S_COUNT; sec++)
                cocovid_tab_text(tab, sec, text, sizeof text);
            {
                const cocovid_reg *regs;
                const cocovid_swatch *swat;
                const cocovid_line_row *rows;
                const char *const *timing;
                cocovid_tab_regs(tab, &regs);
                cocovid_tab_swatches(tab, &swat);
                cocovid_tab_lines(tab, &rows);
                cocovid_tab_timing(tab, &timing);
            }
        }
        survived++;
    }
    check(survived == 400, "400 random captures through every view, section and inspector call");
    cocovid_tab_free(tab);
}

static void test_line_decoder_alone(void)
{
    cocovid_line rec;
    cocovideo_line_info info;
    uint8_t codes[COCOVIDEO_MAX_WIDTH];

    memset(&rec, 0, sizeof rec);
    check(cocovideo_line_decode(&rec, codes, sizeof codes, &info) == 0, "an empty record is not a display line");
    check(cocovideo_line_decode(NULL, codes, sizeof codes, &info) == 0, "NULL is not a display line");
    set_vdg_line(&g_finished, 50, 0xF0, 0x1000, 0, 0x80);
    check(cocovideo_line_decode(&g_finished.lines[50], codes, sizeof codes, &info) == 256 &&
          codes[0] == 0 && codes[1] == 9 && strstr(info.mode, "RG6"),
          "RG6 $80: pixel 0 green, pixel 1 dark green");
    check(cocovideo_line_stride(&g_finished.lines[50]) == 32, "RG6 advances 32 bytes a row");
    {
        int sub = -1;
        check(cocovideo_line_byte_at(&g_finished.lines[50], &info, 17, &sub) == 2 && sub == 1,
              "pixel 17 is byte 2, pixel 1 in it");
        check(cocovideo_line_byte_at(&g_finished.lines[50], &info, 256, &sub) == -1, "past the line: none");
    }
}

/* the scripted-capture variables (putenv: MinGW has no setenv) */
static void test_env(void)
{
    static char view[] = "COCO_DEBUGGER_VIDEO_VIEW=dec";
    static char overlays[] = "COCO_DEBUGGER_VIDEO_OVERLAYS=grid,modes";
    static char zoom[] = "COCO_DEBUGGER_VIDEO_ZOOM=4";
    static char section[] = "COCO_DEBUGGER_VIDEO_SECTION=col";
    cocovid_tab *tab;

    putenv(view);
    putenv(overlays);
    putenv(zoom);
    putenv(section);
    tab = cocovid_tab_new(NULL);
    check(cocovid_tab_view(tab) == COCOVID_V_DECODED, "env: view");
    check(cocovid_tab_overlays(tab) == (COCOVID_OV_GRID | COCOVID_OV_MODES), "env: overlays");
    check(cocovid_tab_zoom(tab) == 4, "env: zoom");
    check(cocovid_tab_env_section(tab) == COCOVID_S_COLOURS, "env: section");
    check(cocovid_section_from_name("lin") == COCOVID_S_LINES && cocovid_section_from_name("zzz") == -1,
          "section names by prefix");
    cocovid_tab_free(tab);
}

int main(void)
{
    test_line_decoder_alone();
    test_vdg_split();
    test_gime_palette_and_widths();
    test_beam_and_display_hover();
    test_nothing_yet();
    test_fuzz();
    test_env();
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
