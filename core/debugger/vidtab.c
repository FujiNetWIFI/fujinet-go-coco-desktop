/*
 * vidtab.c -- the Video tab's controller (core/include/cocovidtab.h).
 *
 * Everything here works from cocovid_capture records: per scanline, the video
 * chip's registers and the bytes it fetched. The Decoded and Values views
 * redraw the display area one line at a time through cocovideo_line_decode(),
 * so a mode, palette or address change in the middle of a frame shows.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "cocovidtab.h"

#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cocovideo.h"

#define STRIDE COCOVIDEO_MAX_WIDTH      /* codes per decoded row */
#define MAXPIX 6000000L                 /* the largest picture we will build */
#define MAX_REGS 40
#define MAX_TIMING 16
#define BAR_PX 3

static const int k_zooms[] = { 0, 1, 2, 3, 4, 6, 8, 12, 16 };

enum { BK_FRAME = 0, BK_CANVAS };       /* what the base picture is */

#define COL_BLACK 0xff000000u

struct cocovid_tab {
    cocodebug *dbg;
    int view, zoom, vieww, viewh;
    unsigned ovl;
    int env_section, capture_on;
    cocovid_break_fn break_fn;
    void *break_ctx;

    /* the machine's state as presented */
    cocovid_capture *last;      /* last finished frame, from the machine thread */
    cocovid_capture *live;      /* a stopped machine, read while parked */
    cocovid_capture *fed_last, *fed_live;
    uint64_t last_serial;
    int have, has_last, has_live, stopped, fed;
    uint64_t sig;
    const cocovid_capture *lcap;    /* where Decoded, Values and the tables take their lines */
    const cocovid_capture *bcap;    /* where the picture on screen takes its lines */

    /* the display area, decoded line by line */
    uint8_t *rows;              /* canvh rows of STRIDE colour codes */
    cocovideo_line_info *linfo; /* one per row; width 0: not a display line */
    int first_act, canvw, canvh;
    long uses[16];
    int ref_line;               /* video line the registers and colours are read from, -1 none */
    int mode_changes, jumps;

    /* the picture */
    uint32_t *base, *pic;
    size_t base_cap, pic_cap;
    int basew, baseh, picw, pich, effzoom, kind;
    int dirty;
    char why[200];

    /* tables */
    cocovid_reg regs[MAX_REGS];
    char prev_val[MAX_REGS][32];
    int nregs, regs_primed;
    cocovid_swatch swat[16];
    int nswat;
    cocovid_line_row rows_out[COCOVID_MAX_LINES];
    int nrows_out;
    char status[400];
    char timing_buf[MAX_TIMING][112];
    const char *timing_ptr[MAX_TIMING];
    int ntiming;

    /* pixels */
    int hovx, hovy, pinx, piny;         /* -1: none */
    cocovid_pixel hover, pin;
};

/* ---- tables of names ----------------------------------------------------------------- */

static const cocovid_overlay_info k_overlays[] = {
    { COCOVID_OV_GRID,    "grid",    "Grid" },
    { COCOVID_OV_BEAM,    "beam",    "Beam" },
    { COCOVID_OV_ADDRESS, "address", "Fetch address jumps" },
    { COCOVID_OV_MODES,   "modes",   "Mode and palette changes" },
    { COCOVID_OV_SOURCE,  "source",  "Memory page tint" },
    { COCOVID_OV_ACTIVE,  "active",  "Display area outline" },
};
#define N_OVERLAYS ((int)(sizeof k_overlays / sizeof *k_overlays))

const cocovid_overlay_info *cocovid_overlays(int *count)
{
    if (count) *count = N_OVERLAYS;
    return k_overlays;
}

static const char *const k_view_names[] = { "Display", "Decoded", "Beam", "Values" };
static const char *const k_view_keys[] = { "display", "decoded", "beam", "values" };
static const char *const k_sec_names[] = { "Pixel", "Registers", "Colours", "Lines", "Timing" };
static const char *const k_sec_keys[] = { "pixel", "registers", "colours", "lines", "timing" };
static const char *const k_vdg_names[12] = {
    "Green", "Yellow", "Blue", "Red", "White", "Cyan", "Magenta", "Orange",
    "Black", "Dark green", "Dark orange", "Bright orange" };

const char *cocovid_view_name(int val) { return val >= 0 && val < COCOVID_V_COUNT ? k_view_names[val] : ""; }
const char *cocovid_view_key(int val) { return val >= 0 && val < COCOVID_V_COUNT ? k_view_keys[val] : ""; }
const char *cocovid_section_name(int str) { return str >= 0 && str < COCOVID_S_COUNT ? k_sec_names[str] : ""; }
const char *cocovid_section_key(int str) { return str >= 0 && str < COCOVID_S_COUNT ? k_sec_keys[str] : ""; }

int cocovid_section_from_name(const char *name)
{
    if (!name || !*name) return -1;
    for (int sec = 0; sec < COCOVID_S_COUNT; sec++)
        if (!strncasecmp(name, k_sec_keys[sec], strlen(name))) return sec;
    return -1;
}

const int *cocovid_zooms(int *count)
{
    if (count) *count = (int)(sizeof k_zooms / sizeof *k_zooms);
    return k_zooms;
}

/* ---- small helpers ---------------------------------------------------------------------- */

static uint64_t fnv(uint64_t high, const void *pixp, size_t num)
{
    const uint8_t *blu = pixp;
    while (num--) { high ^= *blu++; high *= 1099511628211ull; }
    return high;
}

static uint32_t blend(uint32_t dst, uint32_t rgb, int a256)
{
    const uint32_t dstr = (dst >> 16) & 255, dstg = (dst >> 8) & 255, dstb = dst & 255;
    const uint32_t srcr = (rgb >> 16) & 255, srcg = (rgb >> 8) & 255, srcb = rgb & 255;
    const uint32_t ret = (dstr * (uint32_t)(256 - a256) + srcr * (uint32_t)a256) >> 8;
    const uint32_t grn = (dstg * (uint32_t)(256 - a256) + srcg * (uint32_t)a256) >> 8;
    const uint32_t blu = (dstb * (uint32_t)(256 - a256) + srcb * (uint32_t)a256) >> 8;
    return 0xff000000u | ret << 16 | grn << 8 | blu;
}

static int fits_zoom(int wide, int high, int scale)
{
    return (long)wide * scale * (long)high * scale <= MAXPIX;
}

static void why_set(cocovid_tab *tab, const char *str)
{
    snprintf(tab->why, sizeof tab->why, "%s", str);
}

static int ensure(uint32_t **buf, size_t *cap, size_t need)
{
    if (need > *cap) {
        uint32_t *grown = realloc(*buf, need * sizeof **buf);
        if (!grown) return 0;
        *buf = grown;
        *cap = need;
    }
    return 1;
}

/* sixteen well-separated colours for the Values view: equal codes read as equal */
static uint32_t value_colour(int code)
{
    static const uint32_t table[16] = {
        0xff2e7d32, 0xfff9a825, 0xff1565c0, 0xffc62828,
        0xffeeeeee, 0xff26c6da, 0xffab47bc, 0xffef6c00,
        0xff101010, 0xff66bb6a, 0xff8d6e63, 0xffffd54f,
        0xff5c6bc0, 0xfff48fb1, 0xff9e9e9e, 0xff80cbc4,
    };
    return table[code & 15];
}

static const cocovid_line *line_at(const cocovid_capture *cap, int vline)
{
    if (!cap || vline < 0 || vline >= COCOVID_MAX_LINES || !cap->lines[vline].valid)
        return NULL;
    return &cap->lines[vline];
}

static int is_display(const cocovid_line *rec)
{
    return rec && rec->valid && (rec->flags & COCOVID_LF_ACTIVE);
}

/* The address of fetched byte `index`: the SAM's counter simply counts; the
 * GIME adds its horizontal offset to the line's base, wrapping inside 256. */
static unsigned byte_address(const cocovid_line *rec, int index)
{
    if (!rec->gime)
        return (rec->addr + (unsigned)index) & 0xffffu;
    if (rec->addr >= rec->g_b && rec->addr - rec->g_b < 256u) {
        const unsigned off0 = rec->addr - rec->g_b;
        return rec->g_b + ((off0 + ((unsigned)index & ~1u)) & 0xffu) + ((unsigned)index & 1u);
    }
    return rec->addr + (unsigned)index;
}

/* ---- new / free / settings -------------------------------------------------------------- */

cocovid_tab *cocovid_tab_new(cocodebug *dbg)
{
    cocovid_tab *tab = calloc(1, sizeof *tab);
    const char *envv;
    if (!tab) return NULL;
    tab->dbg = dbg;
    tab->last = calloc(1, sizeof *tab->last);
    tab->live = calloc(1, sizeof *tab->live);
    tab->rows = calloc((size_t)COCOVID_MAX_LINES, STRIDE);
    tab->linfo = calloc((size_t)COCOVID_MAX_LINES, sizeof *tab->linfo);
    if (!tab->last || !tab->live || !tab->rows || !tab->linfo) {
        cocovid_tab_free(tab);
        return NULL;
    }
    tab->view = COCOVID_V_DISPLAY;
    tab->ovl = COCOVID_OV_BEAM | COCOVID_OV_ACTIVE;
    tab->zoom = 0;
    tab->vieww = 640;
    tab->viewh = 480;
    tab->hovx = tab->hovy = tab->pinx = tab->piny = -1;
    tab->ref_line = -1;
    tab->dirty = 1;

    if ((envv = getenv("COCO_DEBUGGER_VIDEO_VIEW")) && *envv) {
        for (int i = 0; i < COCOVID_V_COUNT; i++)
            if (!strncasecmp(envv, k_view_keys[i], strlen(envv))) { tab->view = i; break; }
    }
    if ((envv = getenv("COCO_DEBUGGER_VIDEO_OVERLAYS"))) {
        char buf[200], *tok, *save;
        unsigned ovl = 0;
        snprintf(buf, sizeof buf, "%s", envv);
        for (tok = strtok_r(buf, ", ", &save); tok; tok = strtok_r(NULL, ", ", &save)) {
            if (!strcasecmp(tok, "all")) ovl |= COCOVID_OV_ALL;
            else if (!strcasecmp(tok, "none")) ovl = 0;
            else
                for (int i = 0; i < N_OVERLAYS; i++)
                    if (!strcasecmp(tok, k_overlays[i].name)) ovl |= k_overlays[i].bit;
        }
        tab->ovl = ovl;
    }
    if ((envv = getenv("COCO_DEBUGGER_VIDEO_ZOOM")) && *envv) {
        if (!strcasecmp(envv, "fit")) tab->zoom = 0;
        else {
            const int scale = atoi(envv);
            for (size_t i = 1; i < sizeof k_zooms / sizeof *k_zooms; i++)
                if (k_zooms[i] == scale) tab->zoom = scale;
        }
    }
    tab->env_section = COCOVID_S_PIXEL;
    if ((envv = getenv("COCO_DEBUGGER_VIDEO_SECTION")) && *envv) {
        const int sec = cocovid_section_from_name(envv);
        if (sec >= 0) tab->env_section = sec;
    }
    cocovid_host_capture_enable(1);
    tab->capture_on = 1;
    return tab;
}

void cocovid_tab_free(cocovid_tab *tab)
{
    if (!tab) return;
    if (tab->capture_on)
        cocovid_host_capture_enable(0);
    free(tab->last);
    free(tab->live);
    free(tab->fed_last);
    free(tab->fed_live);
    free(tab->rows);
    free(tab->linfo);
    free(tab->base);
    free(tab->pic);
    free(tab);
}

void cocovid_tab_set_view(cocovid_tab *tab, int val)
{
    if (tab && val >= 0 && val < COCOVID_V_COUNT && val != tab->view) { tab->view = val; tab->dirty = 1; }
}
int cocovid_tab_view(const cocovid_tab *tab) { return tab ? tab->view : 0; }
void cocovid_tab_set_overlays(cocovid_tab *tab, unsigned ovl)
{
    if (tab && (ovl & COCOVID_OV_ALL) != tab->ovl) { tab->ovl = ovl & COCOVID_OV_ALL; tab->dirty = 1; }
}
unsigned cocovid_tab_overlays(const cocovid_tab *tab) { return tab ? tab->ovl : 0; }
void cocovid_tab_set_zoom(cocovid_tab *tab, int scale)
{
    if (tab && scale >= 0 && scale <= 16 && scale != tab->zoom) { tab->zoom = scale; tab->dirty = 1; }
}
int cocovid_tab_zoom(const cocovid_tab *tab) { return tab ? tab->zoom : 0; }
void cocovid_tab_set_viewport(cocovid_tab *tab, int wide, int high)
{
    if (!tab) return;
    if (wide < 1) wide = 1;
    if (high < 1) high = 1;
    if (wide != tab->vieww || high != tab->viewh) {
        tab->vieww = wide; tab->viewh = high;
        if (tab->zoom == 0) tab->dirty = 1;
    }
}
int cocovid_tab_env_section(const cocovid_tab *tab) { return tab ? tab->env_section : 0; }
void cocovid_tab_set_break_handler(cocovid_tab *tab, cocovid_break_fn fn, void *ctx)
{
    if (!tab) return;
    tab->break_fn = fn;
    tab->break_ctx = ctx;
}

/* ---- feeding ----------------------------------------------------------------------------- */

void cocovid_tab_feed(cocovid_tab *tab, const cocovid_capture *finished, const cocovid_capture *live)
{
    if (!tab) return;
    if (!tab->fed_last) tab->fed_last = calloc(1, sizeof *tab->fed_last);
    if (!tab->fed_live) tab->fed_live = calloc(1, sizeof *tab->fed_live);
    if (!tab->fed_last || !tab->fed_live) return;
    tab->fed = 1;
    if (finished) memcpy(tab->fed_last, finished, sizeof *tab->fed_last);
    else memset(tab->fed_last, 0, sizeof *tab->fed_last);
    tab->has_last = tab->fed_last->serial != 0;
    tab->stopped = live != NULL;
    if (live) {
        memcpy(tab->fed_live, live, sizeof *tab->fed_live);
        tab->fed_live->stopped = 1;
    }
    tab->has_live = live != NULL;
    tab->have = tab->has_last || tab->has_live;
    tab->dirty = 1;
}

/* Take what the machine has. 0 if there is nothing to show at all. */
static int acquire(cocovid_tab *tab)
{
    const cocovid_capture *lastp, *livep;

    if (tab->fed) {
        lastp = tab->fed_last;
        livep = tab->fed_live;
    } else {
        cocodebug_stop stop_state;

        stop_state.paused = 0;
        if (tab->dbg)
            cocodebug_stop_snapshot(tab->dbg, &stop_state);
        tab->stopped = stop_state.paused != 0;
        cocovid_host_capture_get(tab->last, &tab->last_serial);
        tab->has_last = tab->last->serial != 0;
        tab->has_live = 0;
        if (tab->stopped)
            tab->has_live = cocovid_host_capture_live(tab->live);
        lastp = tab->last;
        livep = tab->live;
        tab->have = tab->has_last || tab->has_live;
    }
    if (!tab->have)
        return 0;
    /* a stopped machine's own lines when it has any, else the last frame's */
    tab->lcap = tab->stopped && tab->has_live ? livep : (tab->has_last ? lastp : livep);
    switch (tab->view) {
    case COCOVID_V_DISPLAY:
        tab->bcap = tab->has_last ? lastp : livep;
        break;
    case COCOVID_V_BEAM:
        tab->bcap = livep;
        break;
    default:
        tab->bcap = tab->lcap;
        break;
    }
    return 1;
}

static uint64_t signature(const cocovid_tab *tab)
{
    const cocovid_capture *lines = tab->lcap, *frame = tab->bcap;
    uint64_t high = 1469598103934665603ull;
    if (!lines || !frame)
        return high;
    high = fnv(high, lines->lines, sizeof lines->lines);
    high = fnv(high, &lines->serial, sizeof lines->serial);
    high = fnv(high, &lines->tick, sizeof lines->tick);
    high = fnv(high, &lines->beam_line, sizeof lines->beam_line);
    high = fnv(high, &lines->rows_done, sizeof lines->rows_done);
    high = fnv(high, &frame->serial, sizeof frame->serial);
    high = fnv(high, &frame->tick, sizeof frame->tick);
    high = fnv(high, &frame->rows_done, sizeof frame->rows_done);
    high = fnv(high, &frame->frame_w, sizeof frame->frame_w);
    high = fnv(high, &frame->frame_h, sizeof frame->frame_h);
    high = fnv(high, &tab->stopped, sizeof tab->stopped);
    high = fnv(high, &tab->has_last, sizeof tab->has_last);
    high = fnv(high, &tab->has_live, sizeof tab->has_live);
    return high;
}

/* ---- decoding the display area -------------------------------------------------------- */

static void decode_all(cocovid_tab *tab)
{
    const cocovid_capture *cap = tab->lcap;
    int first = -1, last = -1;

    tab->first_act = -1;
    tab->canvw = tab->canvh = 0;
    tab->ref_line = -1;
    tab->mode_changes = tab->jumps = 0;
    memset(tab->uses, 0, sizeof tab->uses);
    for (int vline = 0; vline < COCOVID_MAX_LINES; vline++)
        if (is_display(&cap->lines[vline])) {
            if (first < 0) first = vline;
            last = vline;
        }
    if (first < 0)
        return;
    tab->first_act = first;
    tab->canvh = last - first + 1;
    for (int row = 0; row < tab->canvh; row++) {
        const cocovid_line *rec = &cap->lines[first + row];
        cocovideo_line_info *info = &tab->linfo[row];
        int wide = 0;
        if (is_display(rec))
            wide = cocovideo_line_decode(rec, tab->rows + (size_t)row * STRIDE, STRIDE, info);
        if (!wide) {
            memset(info, 0, sizeof *info);
            continue;
        }
        if (wide > tab->canvw) tab->canvw = wide;
        {
            const uint8_t *codes = tab->rows + (size_t)row * STRIDE;
            for (int posx = 0; posx < wide; posx++)
                tab->uses[codes[posx] & 15]++;
        }
        tab->ref_line = first + row;
    }
    if (tab->stopped && cap->beam_line > 0) {
        /* registers as of the last display line the beam has finished */
        for (int vline = cap->beam_line - 1; vline >= first; vline--)
            if (is_display(&cap->lines[vline])) { tab->ref_line = vline; break; }
    }
}

/* ---- the base picture (1x) -------------------------------------------------------------------- */

static uint32_t canvas_px(const cocovid_tab *tab, int row, int col, int values)
{
    const cocovideo_line_info *info = &tab->linfo[row];
    int src;
    if (info->width <= 0)
        return COL_BLACK;
    src = (int)((long)col * info->width / tab->canvw);
    if (src >= info->width) src = info->width - 1;
    {
        const int code = tab->rows[(size_t)row * STRIDE + (size_t)src] & 15;
        return values ? value_colour(code) : info->pal[code];
    }
}

static int build_base(cocovid_tab *tab)
{
    const cocovid_capture *cap = tab->bcap;

    switch (tab->view) {
    case COCOVID_V_DISPLAY:
    case COCOVID_V_BEAM: {
        if (tab->view == COCOVID_V_BEAM && !tab->has_live) {
            why_set(tab, "The Beam view needs the machine stopped: it shows this frame as far as the beam has drawn it, and the last one dimmed below.");
            return 0;
        }
        if (!cap || !cap->has_pixels || cap->frame_w <= 0 || cap->frame_h <= 0) {
            why_set(tab, "No frame has finished yet. Let the machine run for a moment.");
            return 0;
        }
        if (!ensure(&tab->base, &tab->base_cap, (size_t)cap->frame_w * (size_t)cap->frame_h)) return 0;
        for (int posy = 0; posy < cap->frame_h; posy++)
            for (int posx = 0; posx < cap->frame_w; posx++) {
                uint32_t pixel = cap->pixels[(size_t)posy * (size_t)cap->frame_w + (size_t)posx] | COL_BLACK;
                if (tab->view == COCOVID_V_BEAM && posy >= cap->rows_done)
                    pixel = blend(pixel, 0x000000, 150);
                tab->base[(size_t)posy * (size_t)cap->frame_w + (size_t)posx] = pixel;
            }
        tab->basew = cap->frame_w; tab->baseh = cap->frame_h;
        tab->kind = BK_FRAME;
        return 1;
    }
    default:
        if (!cap || tab->canvw <= 0 || tab->canvh <= 0) {
            why_set(tab, !cap ? "No frame has finished yet. Let the machine run for a moment."
                              : "No display lines have been captured yet. Let the machine run for a moment.");
            return 0;
        }
        if (!ensure(&tab->base, &tab->base_cap, (size_t)tab->canvw * (size_t)tab->canvh)) return 0;
        for (int row = 0; row < tab->canvh; row++)
            for (int col = 0; col < tab->canvw; col++)
                tab->base[(size_t)row * (size_t)tab->canvw + (size_t)col] =
                    canvas_px(tab, row, col, tab->view == COCOVID_V_VALUES);
        tab->basew = tab->canvw; tab->baseh = tab->canvh;
        tab->kind = BK_CANVAS;
        return 1;
    }
}

/* ---- overlays ---------------------------------------------------------------------------- */

typedef struct { uint32_t *pixp; int wide, high; } canvas;

static void px_blend(canvas *cnv, int posx, int posy, uint32_t rgb, int alpha)
{
    if (posx >= 0 && posy >= 0 && posx < cnv->wide && posy < cnv->high)
        cnv->pixp[(size_t)posy * (size_t)cnv->wide + (size_t)posx] = blend(cnv->pixp[(size_t)posy * (size_t)cnv->wide + (size_t)posx], rgb, alpha);
}
static void draw_hline(canvas *cnv, int posy, int minx, int maxx, uint32_t rgb, int alpha)
{
    for (int posx = minx; posx < maxx; posx++) px_blend(cnv, posx, posy, rgb, alpha);
}
static void draw_vline(canvas *cnv, int posx, int starty, int endy, uint32_t rgb, int alpha)
{
    for (int posy = starty; posy < endy; posy++) px_blend(cnv, posx, posy, rgb, alpha);
}
static void fill_rect(canvas *cnv, int posx, int posy, int wide, int high, uint32_t rgb, int alpha)
{
    for (int offy = 0; offy < high; offy++)
        for (int offx = 0; offx < wide; offx++) px_blend(cnv, posx + offx, posy + offy, rgb, alpha);
}

/* the video line a row of the base picture shows */
static int row_to_line(const cocovid_tab *tab, int row)
{
    return tab->kind == BK_FRAME ? tab->bcap->first_line + row : tab->first_act + row;
}

/* does the fetch of `rec` start where the line `prev` would leave it? */
static int address_continues(const cocovid_line *prev, const cocovid_line *rec)
{
    const unsigned stride = (unsigned)cocovideo_line_stride(prev);
    if (rec->addr == prev->addr)
        return 1;
    return rec->addr == ((prev->addr + stride) & (prev->gime ? 0xfffffu : 0xffffu));
}

static int changed_mode(const cocovid_line *prev, const cocovid_line *rec)
{
    return prev->mode1 != rec->mode1 || prev->pia_mode != rec->pia_mode ||
           (rec->gime && memcmp(prev->g_pal, rec->g_pal, sizeof rec->g_pal) != 0) ||
           (rec->flags & COCOVID_LF_MIDLINE);
}

static const uint32_t k_page_tints[8] = {
    0xff4caf50, 0xff29b6f6, 0xffffca28, 0xffec407a, 0xff9575cd, 0xff26a69a, 0xffff7043, 0xffd4e157 };

static void draw_overlays(cocovid_tab *tab, canvas *cnv, int scale)
{
    const cocovid_capture *cap = tab->bcap;
    const int frame = tab->kind == BK_FRAME;
    const cocovid_line *prev = NULL;

    for (int posy = 0; posy < tab->baseh; posy++) {
        const int vline = row_to_line(tab, posy);
        const cocovid_line *rec = line_at(cap, vline);
        const int show = is_display(rec);

        if (rec && show) {
            if (tab->ovl & COCOVID_OV_SOURCE)
                fill_rect(cnv, 0, posy * scale, tab->basew * scale, scale,
                          k_page_tints[(rec->addr >> 9) & 7], 60);
            if (prev && (tab->ovl & COCOVID_OV_ADDRESS) &&
                !address_continues(prev, rec))
                fill_rect(cnv, 0, posy * scale, scale < BAR_PX ? scale : BAR_PX, scale, 0xffff9020, 230);
            if (prev && (tab->ovl & COCOVID_OV_MODES) && changed_mode(prev, rec)) {
                draw_hline(cnv, posy * scale, 0, tab->basew * scale, 0xff40e0ff, 120);
                fill_rect(cnv, tab->basew * scale - (scale < BAR_PX ? scale : BAR_PX), posy * scale,
                          scale < BAR_PX ? scale : BAR_PX, scale, 0xff40e0ff, 230);
            }
            prev = rec;
        }
        if ((tab->ovl & COCOVID_OV_GRID) && scale >= 2 && !frame) {
            const cocovideo_line_info *info = &tab->linfo[posy];
            if (info->width > 0 && info->nbytes > 0) {
                const int per_byte = info->attr ? info->width * 2 / info->nbytes : info->width / info->nbytes;
                for (int idx = 0; per_byte > 0 && idx * per_byte <= info->width; idx++) {
                    const int posx = (int)((long)idx * per_byte * tab->canvw / info->width);
                    draw_vline(cnv, posx * scale, posy * scale, (posy + 1) * scale, 0xffffffff, 80);
                }
            }
            if (scale >= 8)
                for (int posx = 0; posx < tab->basew; posx++)
                    draw_vline(cnv, posx * scale, posy * scale, (posy + 1) * scale, 0xffffffff, 25);
        }
    }
    if ((tab->ovl & COCOVID_OV_ACTIVE) && frame && cap->act_w > 0 && cap->act_h > 0) {
        int top = -1, bottom = -1;
        for (int posy = 0; posy < tab->baseh; posy++)
            if (is_display(line_at(cap, row_to_line(tab, posy)))) {
                if (top < 0) top = posy;
                bottom = posy + 1;
            }
        if (top >= 0) {
            const int left = cap->act_x * scale, right = (cap->act_x + cap->act_w) * scale;
            draw_hline(cnv, top * scale, left, right, 0xffffffff, 170);
            draw_hline(cnv, bottom * scale - 1, left, right, 0xffffffff, 170);
            draw_vline(cnv, left, top * scale, bottom * scale, 0xffffffff, 170);
            draw_vline(cnv, right - 1, top * scale, bottom * scale, 0xffffffff, 170);
        }
    }
    if ((tab->ovl & COCOVID_OV_BEAM) && tab->stopped && tab->has_live && cap->stopped) {
        const int posy = tab->kind == BK_FRAME ? cap->rows_done : cap->beam_line - tab->first_act;
        if (posy >= 0 && posy < tab->baseh)
            draw_hline(cnv, posy * scale, 0, tab->basew * scale, 0xffff4040, 230);
    }
}

static int pick_zoom(const cocovid_tab *tab)
{
    int scale = tab->zoom;
    if (scale == 0) {
        scale = 1;
        for (size_t i = 1; i < sizeof k_zooms / sizeof *k_zooms; i++)
            if (tab->basew * k_zooms[i] <= tab->vieww && tab->baseh * k_zooms[i] <= tab->viewh)
                scale = k_zooms[i];
    }
    while (scale > 1 && !fits_zoom(tab->basew, tab->baseh, scale)) {
        int lower = 1;
        for (size_t i = 1; i < sizeof k_zooms / sizeof *k_zooms; i++)
            if (k_zooms[i] < scale) lower = k_zooms[i];
        scale = lower;
    }
    return scale;
}

static void build_picture(cocovid_tab *tab)
{
    int scale;
    canvas cnv;

    tab->why[0] = '\0';
    tab->picw = tab->pich = 0;
    if (!build_base(tab))
        return;
    scale = pick_zoom(tab);
    tab->effzoom = scale;
    tab->picw = tab->basew * scale;
    tab->pich = tab->baseh * scale;
    if (!ensure(&tab->pic, &tab->pic_cap, (size_t)tab->picw * (size_t)tab->pich)) {
        tab->picw = tab->pich = 0;
        why_set(tab, "Out of memory.");
        return;
    }
    if (scale == 1)
        memcpy(tab->pic, tab->base, (size_t)tab->basew * (size_t)tab->baseh * sizeof *tab->pic);
    else
        for (int posy = 0; posy < tab->pich; posy++) {
            const uint32_t *src = &tab->base[(size_t)(posy / scale) * (size_t)tab->basew];
            uint32_t *dst = &tab->pic[(size_t)posy * (size_t)tab->picw];
            for (int posx = 0; posx < tab->picw; posx++)
                dst[posx] = src[posx / scale];
        }
    cnv.pixp = tab->pic; cnv.wide = tab->picw; cnv.high = tab->pich;
    draw_overlays(tab, &cnv, scale);
}

/* ---- the inspector ----------------------------------------------------------------------- */

static void locate(cocovid_tab *tab, int posx, int posy, cocovid_pixel *outp)
{
    const cocovid_capture *cap = tab->bcap;
    const int scale = tab->effzoom ? tab->effzoom : 1;
    const cocovid_line *rec;
    cocovideo_line_info info;
    uint8_t codes[STRIDE];
    int picx, picy, vline, column = -1, index = -1, sub = 0, code = -1, num;

    memset(outp, 0, sizeof *outp);
    outp->line = -1; outp->chip_line = -1; outp->column = -1; outp->index = -1; outp->code = -1;
    if (!tab->picw || !cap || posx < 0 || posy < 0 || posx >= tab->picw || posy >= tab->pich)
        return;
    picx = posx / scale; picy = posy / scale;
    outp->valid = 1;
    outp->picx = picx; outp->picy = picy;
    outp->rgb = tab->base ? tab->base[(size_t)picy * (size_t)tab->basew + (size_t)picx] : COL_BLACK;

    vline = row_to_line(tab, picy);
    rec = line_at(cap, vline);
    outp->line = vline;
    memset(&info, 0, sizeof info);
    if (is_display(rec) && cocovideo_line_decode(rec, codes, STRIDE, &info)) {
        if (tab->kind == BK_CANVAS) {
            column = (int)((long)picx * info.width / tab->canvw);
        } else if (cap->act_w > 0 && picx >= cap->act_x && picx < cap->act_x + cap->act_w) {
            column = (int)((long)(picx - cap->act_x) * info.width / cap->act_w);
        }
        if (column >= info.width) column = info.width - 1;
        if (column >= 0) {
            index = cocovideo_line_byte_at(rec, &info, column, &sub);
            code = codes[column] & 15;
        }
    }
    if (rec) {
        outp->chip_line = rec->chip_line;
        outp->fresh = rec->fresh;
        snprintf(outp->mode, sizeof outp->mode, "%s", info.mode);
    }
    outp->column = column;
    if (index >= 0) {
        outp->has_byte = 1;
        outp->index = index;
        outp->sub = sub;
        outp->phys = rec->gime;
        outp->addr = byte_address(rec, index);
        outp->value = index < (int)rec->nbytes ? rec->data[index] : 0;
        outp->code = code;
    }

    num = snprintf(outp->text, sizeof outp->text, "Pixel %d,%d   line %d%s", picx, picy, vline,
                   column >= 0 ? "" : ", outside the display area");
    if (column >= 0)
        num += snprintf(outp->text + num, sizeof outp->text - (size_t)num, ", x %d of %d", column, info.width);
    if (rec && info.width > 0) {
        num += snprintf(outp->text + num, sizeof outp->text - (size_t)num, "\nMode: %s", info.mode);
        if (rec->flags & COCOVID_LF_MIDLINE)
            num += snprintf(outp->text + num, sizeof outp->text - (size_t)num, " (changed during this line)");
    } else if (rec) {
        num += snprintf(outp->text + num, sizeof outp->text - (size_t)num,
                        "\nNot a display line: the chip fetched no bytes (border or blanking)");
    } else {
        num += snprintf(outp->text + num, sizeof outp->text - (size_t)num, "\nNo record for this line");
    }
    if (outp->has_byte) {
        num += snprintf(outp->text + num, sizeof outp->text - (size_t)num,
                        "\nByte %d of %d at %s $%0*X = $%02X, pixel %d in it",
                        index, (int)rec->nbytes, rec->gime ? "RAM" : "CPU", rec->gime ? 5 : 4,
                        outp->addr, outp->value, sub);
        if (rec->gime && info.attr && (index & 1))
            num += snprintf(outp->text + num, sizeof outp->text - (size_t)num, " (attribute byte)");
        if (code >= 0) {
            if (info.pal_n == 12)
                num += snprintf(outp->text + num, sizeof outp->text - (size_t)num,
                                "\nColour: %s (VDG colour %d) #%06X", k_vdg_names[code % 12], code,
                                info.pal[code] & 0xffffff);
            else
                num += snprintf(outp->text + num, sizeof outp->text - (size_t)num,
                                "\nColour: palette slot %d = $%02X, #%06X", code, rec->g_pal[code] & 0x3f,
                                info.pal[code] & 0xffffff);
        }
        snprintf(outp->text + num, sizeof outp->text - (size_t)num, "\n%s",
                 rec->fresh ? "This frame" : "The previous frame's line");
    } else if (rec) {
        snprintf(outp->text + num, sizeof outp->text - (size_t)num, "\n%s",
                 rec->fresh ? "This frame" : "The previous frame's line");
    }
}

int cocovid_tab_hover(cocovid_tab *tab, int posx, int posy)
{
    char old[sizeof tab->hover.text];
    if (!tab) return 0;
    snprintf(old, sizeof old, "%s", tab->hover.valid ? tab->hover.text : "");
    tab->hovx = posx; tab->hovy = posy;
    locate(tab, posx, posy, &tab->hover);
    return strcmp(old, tab->hover.valid ? tab->hover.text : "") != 0;
}

void cocovid_tab_pin(cocovid_tab *tab, int posx, int posy)
{
    int scale;
    if (!tab) return;
    scale = tab->effzoom ? tab->effzoom : 1;
    if (tab->pinx >= 0 && posx / scale == tab->pinx / scale && posy / scale == tab->piny / scale) {
        cocovid_tab_unpin(tab);
        return;
    }
    tab->pinx = posx; tab->piny = posy;
    locate(tab, posx, posy, &tab->pin);
    if (!tab->pin.valid) cocovid_tab_unpin(tab);
}

void cocovid_tab_unpin(cocovid_tab *tab)
{
    if (!tab) return;
    tab->pinx = tab->piny = -1;
    memset(&tab->pin, 0, sizeof tab->pin);
}

const cocovid_pixel *cocovid_tab_pixel(const cocovid_tab *tab)
{
    if (!tab) return NULL;
    if (tab->pin.valid) return &tab->pin;
    if (tab->hover.valid) return &tab->hover;
    return NULL;
}

int cocovid_tab_marks(const cocovid_tab *tab, int hover[4], int pin[4])
{
    int ret = 0, scale;
    if (!tab) return 0;
    scale = tab->effzoom ? tab->effzoom : 1;
    if (tab->hover.valid) {
        hover[0] = tab->hover.picx * scale; hover[1] = tab->hover.picy * scale; hover[2] = scale; hover[3] = scale;
        ret |= 1;
    }
    if (tab->pin.valid) {
        pin[0] = tab->pin.picx * scale; pin[1] = tab->pin.picy * scale; pin[2] = scale; pin[3] = scale;
        ret |= 2;
    }
    return ret;
}

int cocovid_tab_can_break(const cocovid_tab *tab)
{
    const cocovid_pixel *pixp = cocovid_tab_pixel(tab);
    return tab && (tab->break_fn || tab->dbg) && pixp && pixp->valid && pixp->has_byte;
}

/* The default: a write breakpoint through the debugger, unless one already
 * covers the byte. Returns its id, or -1. */
static int debugger_break(cocovid_tab *tab, int phys, unsigned address)
{
    cocodebug_breakpoint list[COCODEBUG_MAX_BREAKPOINTS];
    const int wanted = COCODEBUG_BP_WRITE | (phys ? COCODEBUG_BP_PHYS : 0);
    int count;

    if (!tab->dbg) return -1;
    count = cocodebug_breakpoints_list(tab->dbg, list, COCODEBUG_MAX_BREAKPOINTS);
    for (int i = 0; i < count; i++)
        if ((list[i].type & COCODEBUG_BP_WRITE) && !!(list[i].type & COCODEBUG_BP_PHYS) == !!phys &&
            list[i].start <= address && list[i].end >= address)
            return list[i].id;
    return cocodebug_breakpoint_add(tab->dbg, wanted, address, address, NULL);
}

int cocovid_tab_break_on_pixel(cocovid_tab *tab)
{
    const cocovid_pixel *pixp = cocovid_tab_pixel(tab);

    if (!cocovid_tab_can_break(tab)) return -1;
    return tab->break_fn ? tab->break_fn(tab->break_ctx, pixp->phys, pixp->addr)
                         : debugger_break(tab, pixp->phys, pixp->addr);
}

/* ---- the tables ---------------------------------------------------------------------------- */

static void reg_set(cocovid_tab *tab, int *num, const char *name, const char *value,
                    const char *fmt, ...) __attribute__((format(printf, 5, 6)));
static void reg_set(cocovid_tab *tab, int *num, const char *name, const char *value,
                    const char *fmt, ...)
{
    cocovid_reg *reg;
    va_list args;
    int prev_age;
    if (*num >= MAX_REGS) return;
    reg = &tab->regs[*num];
    prev_age = reg->age;
    reg->name = name;
    snprintf(reg->value, sizeof reg->value, "%s", value);
    va_start(args, fmt);
    vsnprintf(reg->meaning, sizeof reg->meaning, fmt, args);
    va_end(args);
    if (!tab->regs_primed || strcmp(tab->prev_val[*num], reg->value) != 0)
        reg->age = tab->regs_primed ? 0 : 99;
    else
        reg->age = prev_age < 99 ? prev_age + 1 : 99;
    memcpy(tab->prev_val[*num], reg->value, sizeof tab->prev_val[*num]);
    (*num)++;
}

static const char *const k_sam_v[8] = {
    "alpha / semigraphics: 12 lines a row, 32 bytes",
    "CG1 / RG1: each row repeats 3 lines",
    "CG2: each row repeats 3 lines",
    "RG2: each row repeats 2 lines",
    "CG3: each row repeats 2 lines",
    "RG3: no repeat",
    "CG6 / RG6: no repeat",
    "DMA mode: reserved" };

static void build_regs(cocovid_tab *tab)
{
    const cocovid_line *rec = line_at(tab->lcap, tab->ref_line);
    cocovideo_line_info info;
    uint8_t codes[STRIDE];
    int num = 0;
    char val[32];

    memset(&info, 0, sizeof info);
    if (rec && is_display(rec))
        cocovideo_line_decode(rec, codes, STRIDE, &info);
    if (!rec) {
        tab->nregs = 0;
        return;
    }
    snprintf(val, sizeof val, "%d", tab->ref_line);
    reg_set(tab, &num, "LINE", val, "registers are those of video line %d (%s)", tab->ref_line,
            tab->stopped ? "the last display line the beam has finished" : "the last display line of the frame");
    reg_set(tab, &num, "MODE", info.mode[0] ? info.mode : "-", "%s",
            info.mode[0] ? "what the chip drew on that line" : "that line is not a display line");
    snprintf(val, sizeof val, "$%02X", rec->pia_mode);
    reg_set(tab, &num, "PIA1 B", val, "VDG mode bits ($FF22 bits 7-3): A/G=%d GM=%d CSS=%d",
            (rec->pia_mode >> 7) & 1, (rec->pia_mode >> 4) & 7, (rec->pia_mode >> 3) & 1);
    if (!rec->gime) {
        snprintf(val, sizeof val, "%d", rec->sam_v);
        reg_set(tab, &num, "SAM V", val, "$FFC0-C5: %s", k_sam_v[rec->sam_v & 7]);
        snprintf(val, sizeof val, "$%04X", (rec->sam_f & 0x7f) << 9);
        reg_set(tab, &num, "SAM F", val, "$FFC6-D3: display base address (F = %d)", rec->sam_f & 0x7f);
        snprintf(val, sizeof val, "$%04X", rec->addr & 0xffff);
        reg_set(tab, &num, "ADDR", val, "address counter at the first fetch of the line");
        snprintf(val, sizeof val, "$%04X", rec->sam_reg);
        reg_set(tab, &num, "SAM", val, "all the SAM's bits packed (TY %d)", rec->sam_ty);
        snprintf(val, sizeof val, "%d", rec->vdg_row);
        reg_set(tab, &num, "VDG ROW", val, "the VDG's text row counter during the line");
    } else {
        snprintf(val, sizeof val, "$%02X", rec->g_regs[0]);
        reg_set(tab, &num, "INIT0", val, "$FF90: %s mode, MMU %s", rec->g_coco ? "CoCo 1/2 compatible" : "GIME native",
                (rec->g_regs[0] & 0x40) ? "on" : "off");
        snprintf(val, sizeof val, "$%02X", rec->g_regs[8]);
        reg_set(tab, &num, "VMODE", val, "$FF98: %s, LPR %d%s", rec->g_bp ? "graphics" : "text", rec->g_lpr,
                rec->g_moch ? ", monochrome" : "");
        snprintf(val, sizeof val, "$%02X", rec->g_regs[9]);
        reg_set(tab, &num, "VRES", val, "$FF99: LPF %d, HRES %d, CRES %d", rec->g_lpf, rec->g_hres, rec->g_cres);
        snprintf(val, sizeof val, "$%02X", rec->g_brdr & 0x3f);
        reg_set(tab, &num, "BORDER", val, "$FF9A: border palette value");
        snprintf(val, sizeof val, "%d", rec->g_vsc);
        reg_set(tab, &num, "VSC", val, "$FF9C: vertical scroll");
        snprintf(val, sizeof val, "$%05X", rec->g_y);
        reg_set(tab, &num, "VOFF", val, "$FF9D-9E: video base address (19 bits)");
        snprintf(val, sizeof val, "$%02X", rec->g_x);
        reg_set(tab, &num, "HOFF", val, "$FF9F: horizontal offset%s", rec->g_hven ? ", virtual screen on (256-byte rows)" : "");
        snprintf(val, sizeof val, "$%05X", rec->addr);
        reg_set(tab, &num, "ADDR", val, "RAM address of the line's first fetch");
        snprintf(val, sizeof val, "%d", rec->g_sam_v);
        reg_set(tab, &num, "SAM V", val, "compatible mode addressing: %s", k_sam_v[rec->g_sam_v & 7]);
        snprintf(val, sizeof val, "$%04X", (rec->g_sam_f & 0x7f) << 9);
        reg_set(tab, &num, "SAM F", val, "compatible mode display base");
    }
    snprintf(val, sizeof val, "%d", tab->mode_changes);
    reg_set(tab, &num, "MODE CHG", val, "display lines of the frame whose mode or palette differs from the line above");
    snprintf(val, sizeof val, "%d", tab->jumps);
    reg_set(tab, &num, "JUMPS", val, "display lines whose fetch did not continue from the line above");
    tab->nregs = num;
    tab->regs_primed = 1;
}

static void gime_level_text(unsigned value, char *dst, size_t size)
{
    snprintf(dst, size, "$%02X  R%u G%u B%u", value & 0x3f, ((value >> 4) & 2) | ((value >> 2) & 1),
             ((value >> 3) & 2) | ((value >> 1) & 1), ((value >> 2) & 2) | (value & 1));
}

static void build_swatches(cocovid_tab *tab)
{
    const cocovid_line *rec = line_at(tab->lcap, tab->ref_line);
    cocovideo_line_info info;
    uint8_t codes[STRIDE];

    tab->nswat = 0;
    if (!rec || !is_display(rec) || !cocovideo_line_decode(rec, codes, STRIDE, &info))
        return;
    for (int idx = 0; idx < info.pal_n && idx < 16; idx++) {
        cocovid_swatch *swat = &tab->swat[idx];
        memset(swat, 0, sizeof *swat);
        swat->index = idx;
        swat->rgb = info.pal[idx];
        swat->uses = tab->uses[idx];
        if (info.pal_n == 12) {
            snprintf(swat->name, sizeof swat->name, "%s", k_vdg_names[idx]);
        } else {
            snprintf(swat->name, sizeof swat->name, "Slot %d", idx);
            gime_level_text(rec->g_pal[idx], swat->detail, sizeof swat->detail);
        }
    }
    tab->nswat = info.pal_n < 16 ? info.pal_n : 16;
}

static void build_line_rows(cocovid_tab *tab)
{
    const cocovid_capture *cap = tab->lcap;
    const cocovid_line *prev = NULL, *prev_display = NULL;
    int nlines = cap->nlines > 0 && cap->nlines <= COCOVID_MAX_LINES ? cap->nlines : COCOVID_MAX_LINES;

    tab->nrows_out = 0;
    tab->mode_changes = tab->jumps = 0;
    for (int vline = 0; vline < nlines; vline++) {
        const cocovid_line *rec = &cap->lines[vline];
        cocovid_line_row *out;
        int mask = 0, show = is_display(rec);

        if (!rec->valid) continue;
        if (prev) {
            if (rec->mode1 != prev->mode1 || rec->pia_mode != prev->pia_mode) mask |= COCOVID_LC_MODE;
            if (rec->gime && memcmp(prev->g_pal, rec->g_pal, sizeof rec->g_pal)) mask |= COCOVID_LC_PALETTE;
        }
        if (show) {
            if (!prev_display) {
                mask |= COCOVID_LC_ADDR;        /* the first display line starts somewhere */
            } else {
                if (!address_continues(prev_display, rec)) {
                    mask |= COCOVID_LC_ADDR;
                    tab->jumps++;
                }
                if (rec->nbytes != prev_display->nbytes) mask |= COCOVID_LC_SIZE;
            }
        }
        if (rec->flags & COCOVID_LF_MIDLINE) mask |= COCOVID_LC_MID;
        prev = rec;
        if (show) prev_display = rec;
        if (show && (mask & (COCOVID_LC_MODE | COCOVID_LC_PALETTE | COCOVID_LC_MID))) tab->mode_changes++;
        if (!show && !(mask & (COCOVID_LC_MODE | COCOVID_LC_PALETTE | COCOVID_LC_MID))) continue;
        if (tab->nrows_out >= COCOVID_MAX_LINES) break;
        out = &tab->rows_out[tab->nrows_out++];
        memset(out, 0, sizeof *out);
        out->row = vline;
        out->chip_line = rec->chip_line;
        out->display = show;
        out->valid = 1;
        out->fresh = rec->fresh;
        out->addr = rec->addr;
        out->nbytes = rec->nbytes;
        out->changed_mask = mask;
        memcpy(out->data, rec->data, sizeof out->data);
        if (show) {
            cocovideo_line_info info;
            uint8_t codes[STRIDE];
            if (cocovideo_line_decode(rec, codes, STRIDE, &info))
                snprintf(out->mode, sizeof out->mode, "%s", info.mode);
        } else {
            snprintf(out->mode, sizeof out->mode, "border / blanking");
        }
    }
}

static void build_tables(cocovid_tab *tab)
{
    const cocovid_capture *cap = tab->lcap;
    int shown = 0;
    const int nlines = cap->nlines > 0 ? cap->nlines : COCOVID_MAX_LINES;

    build_line_rows(tab);
    build_regs(tab);
    build_swatches(tab);
    for (int vline = 0; vline < COCOVID_MAX_LINES; vline++)
        shown += is_display(&cap->lines[vline]);

    tab->ntiming = 0;
#define TLINE(...) do { if (tab->ntiming < MAX_TIMING) { \
        snprintf(tab->timing_buf[tab->ntiming], sizeof tab->timing_buf[0], __VA_ARGS__); \
        tab->timing_ptr[tab->ntiming] = tab->timing_buf[tab->ntiming]; tab->ntiming++; } } while (0)
    if (tab->stopped && cap->stopped)
        TLINE("Beam\tline %d of %d, %u ticks into the line", cap->beam_line, nlines, cap->beam_ticks);
    else
        TLINE("Beam\tframe finished (%d lines)", nlines);
    TLINE("Frame\t%u", cap->frame);
    TLINE("Chip\t%s", cap->gime ? "GIME (CoCo 3)" : "VDG + SAM (CoCo 1/2)");
    if (cap->line_ticks > 0)
        TLINE("Line time\t%u ticks = %.2f us (%.2f kHz)", cap->line_ticks,
              (double)cap->line_ticks * 1e6 / 14318180.0, 14318180.0 / (double)cap->line_ticks / 1000.0);
    if (cap->line_ticks > 0 && nlines > 0)
        TLINE("Frame rate\t%.2f Hz (%d lines)", 14318180.0 / ((double)cap->line_ticks * (double)nlines), nlines);
    if (tab->first_act >= 0)
        TLINE("Display lines\t%d-%d (%d captured)", tab->first_act, tab->first_act + tab->canvh - 1, shown);
    else
        TLINE("Display lines\tnone captured");
    if (tab->canvw > 0)
        TLINE("Decoded size\t%d x %d", tab->canvw, tab->canvh);
    if (cap->frame_w > 0)
        TLINE("Frame\t%d x %d pixels, row 0 is video line %d", cap->frame_w, cap->frame_h, cap->first_line);
    if (cap->act_w > 0)
        TLINE("Active area\tx %d, %d wide; lines from %d, %d tall", cap->act_x, cap->act_w, cap->act_y, cap->act_h);
    TLINE("Mode changes\t%d line%s", tab->mode_changes, tab->mode_changes == 1 ? "" : "s");
    TLINE("Address jumps\t%d line%s", tab->jumps, tab->jumps == 1 ? "" : "s");
    TLINE("Rows listed\t%d", tab->nrows_out);
    TLINE("Machine\t%s", tab->stopped ? "stopped" : "running");
#undef TLINE

    snprintf(tab->status, sizeof tab->status,
             "%s view, %s, %d display line%s, %d mode change%s\n%s%s",
             k_view_names[tab->view], cap->gime ? "GIME" : "VDG", shown, shown == 1 ? "" : "s",
             tab->mode_changes, tab->mode_changes == 1 ? "" : "s",
             tab->stopped ? "Stopped" : "Running: showing the last finished frame",
             tab->stopped ? (tab->has_last ? "" : " (no frame has finished yet)") : "");
}

/* ---- refresh -------------------------------------------------------------------------------- */

unsigned cocovid_tab_refresh(cocovid_tab *tab, int force)
{
    uint64_t sig;
    unsigned ret = 0;
    int fresh;

    if (!tab) return 0;
    if (!acquire(tab)) {
        why_set(tab, "No frame has been captured yet. Let the machine run for a moment, or start a session.");
        tab->picw = tab->pich = 0;
        tab->nregs = tab->nswat = tab->nrows_out = 0;
        return COCOVID_TAB_OFF | COCOVID_TAB_PICTURE | COCOVID_TAB_TABLES;
    }
    sig = signature(tab);
    fresh = force || sig != tab->sig;
    if (fresh) {
        tab->sig = sig;
        decode_all(tab);
    }
    if (fresh || tab->dirty) {
        build_picture(tab);
        tab->dirty = 0;
        ret |= COCOVID_TAB_PICTURE;
    }
    if (fresh) {
        if (tab->hovx >= 0) locate(tab, tab->hovx, tab->hovy, &tab->hover);
        if (tab->pinx >= 0) locate(tab, tab->pinx, tab->piny, &tab->pin);
        build_tables(tab);
        ret |= COCOVID_TAB_TABLES;
    }
    return ret;
}

const uint32_t *cocovid_tab_picture(cocovid_tab *tab, int *wide, int *high)
{
    if (!tab || !tab->picw) return NULL;
    if (wide) *wide = tab->picw;
    if (high) *high = tab->pich;
    return tab->pic;
}
const char *cocovid_tab_why(const cocovid_tab *tab) { return tab ? tab->why : ""; }
int cocovid_tab_effective_zoom(const cocovid_tab *tab) { return tab ? tab->effzoom : 1; }
const char *cocovid_tab_status(const cocovid_tab *tab) { return tab ? tab->status : ""; }
int cocovid_tab_regs(const cocovid_tab *tab, const cocovid_reg **out)
{
    if (out) *out = tab->regs;
    return tab->nregs;
}
int cocovid_tab_swatches(const cocovid_tab *tab, const cocovid_swatch **out)
{
    /* the inspected pixel moves between refreshes */
    const cocovid_pixel *pixp = cocovid_tab_pixel(tab);
    cocovid_swatch *swat = ((cocovid_tab *)tab)->swat;
    for (int i = 0; i < tab->nswat; i++)
        swat[i].inspected = pixp && pixp->valid && pixp->code == swat[i].index;
    if (out) *out = tab->swat;
    return tab->nswat;
}
int cocovid_tab_lines(const cocovid_tab *tab, const cocovid_line_row **out)
{
    if (out) *out = tab->rows_out;
    return tab->nrows_out;
}
int cocovid_tab_timing(const cocovid_tab *tab, const char *const **lines)
{
    if (lines) *lines = (const char *const *)tab->timing_ptr;
    return tab->ntiming;
}
int cocovid_tab_beam_permille(const cocovid_tab *tab)
{
    const cocovid_capture *cap;
    if (!tab || !tab->have || !tab->stopped) return 0;
    cap = tab->lcap;
    return cap && cap->nlines > 0 ? cap->beam_line * 1000 / cap->nlines : 0;
}

int cocovid_tab_text(cocovid_tab *tab, int section, char *dst, int dstsz)
{
    int num = 0;
#define PUT(...) do { if (num < dstsz) num += snprintf(dst + num, (size_t)(dstsz - num), __VA_ARGS__); } while (0)
    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!tab || !tab->have) { PUT("No frame has been captured yet.\n"); return num < dstsz ? num : dstsz - 1; }
    switch (section) {
    case COCOVID_S_REGISTERS:
        for (int i = 0; i < tab->nregs; i++)
            PUT("%-9s %-9s %s\n", tab->regs[i].name, tab->regs[i].value, tab->regs[i].meaning);
        break;
    case COCOVID_S_COLOURS:
        for (int i = 0; i < tab->nswat; i++)
            PUT("%-14s #%06X  %7ld pixels  %s\n", tab->swat[i].name, tab->swat[i].rgb & 0xffffff,
                tab->swat[i].uses, tab->swat[i].detail);
        break;
    case COCOVID_S_LINES:
        for (int i = 0; i < tab->nrows_out; i++) {
            const cocovid_line_row *ret = &tab->rows_out[i];
            PUT("line %3d  $%05X x%-3d %02X %02X %02X %02X  %s%s%s%s%s%s  %s\n", ret->row, ret->addr, ret->nbytes,
                ret->data[0], ret->data[1], ret->data[2], ret->data[3], ret->fresh ? "" : "old ",
                (ret->changed_mask & COCOVID_LC_ADDR) ? "addr-jump " : "",
                (ret->changed_mask & COCOVID_LC_MODE) ? "mode " : "",
                (ret->changed_mask & COCOVID_LC_PALETTE) ? "palette " : "",
                (ret->changed_mask & COCOVID_LC_SIZE) ? "size " : "",
                (ret->changed_mask & COCOVID_LC_MID) ? "mid-line " : "", ret->mode);
        }
        break;
    case COCOVID_S_TIMING:
        for (int i = 0; i < tab->ntiming; i++) PUT("%s\n", tab->timing_buf[i]);
        break;
    default:
        PUT("%s\n", tab->status);
        if (cocovid_tab_pixel(tab)) PUT("%s\n", cocovid_tab_pixel(tab)->text);
        break;
    }
#undef PUT
    return num < dstsz ? num : dstsz - 1;
}

/* The console's `video [section]`: a short-lived tab (it asks for captures only
 * while it exists), registered with the debugger at session start. */
int cocovid_console_text(cocodebug *dbg, const char *section, char *dst, int dstsz)
{
    int which = COCOVID_S_REGISTERS;
    cocovid_tab *tab;
    int len;

    if (section && *section) {
        which = cocovid_section_from_name(section);
        if (which < 0)
            return snprintf(dst, (size_t)dstsz, "video: pixel, registers, colours, lines or timing");
    }
    tab = cocovid_tab_new(dbg);
    if (!tab) return -1;
    cocovid_tab_refresh(tab, 1);
    len = cocovid_tab_text(tab, which, dst, dstsz);
    cocovid_tab_free(tab);
    return len;
}
