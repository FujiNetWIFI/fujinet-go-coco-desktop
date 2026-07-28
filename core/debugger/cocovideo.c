/*
 * cocovideo -- see core/include/cocovideo.h for the design rationale.
 *
 * Everything here is transcribed directly from XRoar's own hardware
 * emulation rather than from general CoCo documentation, since getting a
 * bit wrong means silently-wrong debugger output with nothing to catch it.
 *
 * ---- CoCo 1/2 (VDG + SAM) --------------------------------------------------
 *   - Mode bits (nA/G, GM2-0, CSS) come from PIA1 side B exactly the way
 *     src/dragon/dragon.c's dragon_update_vdg_mode() derives them for the
 *     real VDG: vmode = (PIA1->b.out_source & PIA1->b.out_sink) & 0xf8.
 *   - The video RAM base address comes from the SAM's F register, read back
 *     via struct MC6883's public get_register(): base = (reg<<6) & 0xfe00,
 *     the inverse of mc6883_set_register()'s packing.
 *   - The per-byte pixel decode (alpha/SG4/SG6/CG/RG, and the resolution
 *     each GM value implies) is transcribed from src/mc6847/mc6847.c's
 *     render_scanline() and mc6847_set_mode(): GM_nLPR[], the is_32byte
 *     formula, and the SG4/SG6 bit-pair-per-zone pattern all come straight
 *     off that function.
 *
 * ---- CoCo 3 (GIME) ---------------------------------------------------------
 * A CoCo 3 has no separate SAM/VDG parts -- the GIME (TCC1014) absorbs both.
 * Unlike the SAM and PIA1, GIME's video mode state is not reachable any
 * other way: it lives in a private struct with no getter, and per real GIME
 * hardware almost none of it is even readable back through the CPU bus
 * (confirmed against src/tcc1014/tcc1014.c's own read-path comment). So
 * tools/xroar/patch-staged-tree.py adds one small, read-only
 * tcc1014_get_debug_state() to the vendored source -- see that file for why,
 * and COMPLIANCE.md for this project's general policy on touching XRoar.
 *
 * GIME can run in two quite different ways, both handled below:
 *   - COCO=1: a VDG/SAM-compatible mode, using the SAME per-byte pixel
 *     decode shape as the CoCo 1/2 case, but its OWN geometry rule (BPR
 *     from SAM_V's parity, not from VDG.GM the way real hardware does --
 *     see decode_gime_coco()) and colours resolved through the live
 *     16-entry palette rather than a fixed 12-colour table.
 *   - COCO=0: GIME's native text/graphics modes, entirely its own register
 *     set (VMODE/VRES) and a from-scratch per-byte decode.
 * Deliberately out of scope (documented at each site rather than silently
 * guessed): the RG2 "half resolution" compat-mode special case, which
 * XRoar's own author flags as an unconfirmed guess; the monochrome/
 * composite pixel mask; the HRES==0&&CRES>=2 zeroing quirk; and GIME's
 * `resolution` field, which is a display-timing oversampling factor, not
 * decoded content -- this module produces its own canonical canvas instead
 * (see below), which already reflects every mode at its real proportions.
 *
 * ---- canvas -----------------------------------------------------------
 * Every VDG mode's native resolution divides 256x192 evenly (real CoCo
 * pixels are not square -- eg CG1 is 64x64 stretched 4x horizontally and
 * 3x vertically), so the CoCo 1/2 path and GIME's COCO-compat path both
 * decode once at native resolution and integer-replicate to fill a
 * canonical 256x192 canvas. GIME's native modes vary far more (up to
 * 640x225) and are returned at their own native size instead -- out_w/
 * out_h always report what was actually written.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "top-config.h"

#include <stdint.h>

#include "machine.h"
#include "mc6821.h"
#include "mc6847/font-6847.h"
#include "mc6847/mc6847.h"
#include "mc6883.h"
#include "part.h"
#include "ram.h"
#include "tcc1014/font-gime.h"
#include "tcc1014/tcc1014.h"
#include "xroar.h"

#include "cocodebug.h"
#include "cocovideo.h"

#define VDG_CANVAS_W 256
#define VDG_CANVAS_H 192

struct rgb { uint8_t r, g, b; };

static uint32_t rgb_pixel(uint8_t r, uint8_t g, uint8_t b)
{
    return 0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/* ---- CoCo 1/2 (VDG + SAM) colours -----------------------------------------
 * A simplified flat RGB approximation of the 12 VDG colours -- see
 * core/include/cocovideo.h for why this isn't XRoar's NTSC-composite
 * palette (vdg_palette.h). Indexed by enum vdg_colour (mc6847.h). */
static const struct rgb vdg_rgb[12] = {
    [VDG_GREEN]         = { 0x1d, 0xd1, 0x2b },
    [VDG_YELLOW]         = { 0xe6, 0xe6, 0x1d },
    [VDG_BLUE]           = { 0x1d, 0x30, 0xe6 },
    [VDG_RED]            = { 0xd1, 0x1d, 0x1d },
    [VDG_WHITE]          = { 0xe6, 0xe6, 0xe6 },
    [VDG_CYAN]           = { 0x1d, 0xe6, 0xc4 },
    [VDG_MAGENTA]        = { 0xc4, 0x1d, 0xc4 },
    [VDG_ORANGE]         = { 0xe6, 0x7a, 0x1d },
    [VDG_BLACK]          = { 0x00, 0x00, 0x00 },
    [VDG_DARK_GREEN]     = { 0x0e, 0x69, 0x16 },
    [VDG_DARK_ORANGE]    = { 0x73, 0x3d, 0x0e },
    [VDG_BRIGHT_ORANGE]  = { 0xff, 0x8c, 0x1d },
};

static uint32_t vdg_pixel(int colour_idx)
{
    const struct rgb *c = &vdg_rgb[colour_idx];
    return rgb_pixel(c->r, c->g, c->b);
}

/* ---- CoCo 1/2 per-byte decode, transcribed from mc6847.c's
 * render_scanline() ---------------------------------------------------- */

/* Alpha (text): byte bit 7 clear. 8 native pixels from the internal font,
 * one glyph row (0-11) per physical scanline -- unlike every other mode
 * here, alpha genuinely has 192 distinct rows of detail, not a replicated
 * block. Bit 6 (INV) swaps fg/bg, which is equivalent to mc6847.c's
 * "~vram_g_data" and simpler to express here. */
static void decode_alpha(uint8_t byte, int subrow, int css, uint32_t out[8])
{
    uint8_t glyph = font_6847[(byte & 0x3f) * 12 + subrow];
    int inv = (byte & 0x40) != 0;
    uint32_t fg = vdg_pixel(css ? VDG_BRIGHT_ORANGE : VDG_GREEN);
    uint32_t bg = vdg_pixel(css ? VDG_DARK_ORANGE : VDG_DARK_GREEN);
    int i;

    if (inv) { uint32_t t = fg; fg = bg; bg = t; }
    for (i = 0; i < 8; i++)
        out[i] = (glyph & (0x80 >> i)) ? fg : bg;
}

/* SG4 (byte bit 7 set, GM0 clear i.e. ¬EXT): one of 8 colours (bits 6-4)
 * against black, 2x2 blocks per byte -- top zone (subrow<6) from bits 3-2,
 * bottom zone from bits 1-0. */
static void decode_sg4(uint8_t byte, int subrow, uint32_t out[2])
{
    uint32_t fg = vdg_pixel((byte >> 4) & 7);
    uint32_t bg = vdg_pixel(VDG_BLACK);
    int bits = (subrow < 6) ? ((byte >> 2) & 3) : (byte & 3);

    out[0] = (bits & 2) ? fg : bg;
    out[1] = (bits & 1) ? fg : bg;
}

/* SG6 (byte bit 7 set, GM0 set i.e. EXT): one of 4 colours from a
 * CSS-selected set (bits 7-6 index into it), 2x3 blocks per byte across
 * three 4-scanline zones (bits 5-4 / 3-2 / 1-0). */
static void decode_sg6(uint8_t byte, int subrow, int css, uint32_t out[2])
{
    int base = css ? VDG_WHITE : VDG_GREEN;
    uint32_t fg = vdg_pixel(base + ((byte >> 6) & 3));
    uint32_t bg = vdg_pixel(VDG_BLACK);
    int bits = (subrow < 4) ? ((byte >> 4) & 3)
             : (subrow < 8) ? ((byte >> 2) & 3)
                            : (byte & 3);

    out[0] = (bits & 2) ? fg : bg;
    out[1] = (bits & 1) ? fg : bg;
}

/* CG (4-colour graphics, GM0 clear): 4 native pixels per byte, 2 bits each,
 * from one of two CSS-selected 4-colour sets. */
static void decode_cg(uint8_t byte, int css, uint32_t out[4])
{
    int base = css ? VDG_WHITE : VDG_GREEN;
    int i;

    for (i = 0; i < 4; i++)
        out[i] = vdg_pixel(base + ((byte >> (6 - i * 2)) & 3));
}

/* RG (2-colour graphics, GM0 set): 8 native pixels per byte, 1 bit each. */
static void decode_rg(uint8_t byte, int css, uint32_t out[8])
{
    uint32_t fg = vdg_pixel(css ? VDG_WHITE : VDG_GREEN);
    uint32_t bg = vdg_pixel(css ? VDG_BLACK : VDG_DARK_GREEN);
    int i;

    for (i = 0; i < 8; i++)
        out[i] = (byte & (0x80 >> i)) ? fg : bg;
}

/* ---- geometry: nA_G=0 (alpha/SG4/SG6) ------------------------------------
 * All three share the same addressing (32 bytes/row, 16 character rows of
 * 12 scanlines each -- SAM V is not involved, mc6847.c derives nLPR=12 for
 * alpha/semigraphics purely from nA_G), differing only in per-byte decode
 * and in how many of a byte's bits change per scanline vs. per zone. */
static void decode_alpha_sg(cocodebug *d, uint16_t base, int GM0, int css,
                            uint32_t *pixels)
{
    uint8_t bytes[16][32];
    int y;

    for (y = 0; y < 16; y++)
        cocodebug_read(d, (uint16_t)(base + y * 32), bytes[y], 32);

    for (y = 0; y < VDG_CANVAS_H; y++) {
        int char_row = y / 12;
        int subrow = y % 12;
        int col;

        for (col = 0; col < 32; col++) {
            uint8_t byte = bytes[char_row][col];
            uint32_t out[8];
            int n, i, px_per;

            if (!(byte & 0x80)) {
                decode_alpha(byte, subrow, css, out);
                n = 8;
            } else if (!GM0) {
                decode_sg4(byte, subrow, out);
                n = 2;
            } else {
                decode_sg6(byte, subrow, css, out);
                n = 2;
            }

            px_per = 8 / n;
            for (i = 0; i < n; i++) {
                int x0 = col * 8 + i * px_per;
                int k;
                for (k = 0; k < px_per; k++)
                    pixels[y * VDG_CANVAS_W + x0 + k] = out[i];
            }
        }
    }
}

/* ---- geometry: nA_G=1 (CG/RG graphics) -----------------------------------
 * Bytes/row and lines-per-row (nLPR) per GM value, transcribed from
 * mc6847.c: GM_nLPR[8] = {3,3,3,2,2,1,1,1}, and is_32byte = !(GM==0 ||
 * (GM0 && GM!=7)) -- which resolves to bytes_per_row=32 exactly for
 * GM in {2,4,6,7} and 16 otherwise. Native width is bytes_per_row times 4
 * (CG, 2 bits/pixel) or 8 (RG, 1 bit/pixel); this table's values match the
 * well-known CG1/RG1/CG2/RG2/CG3/RG3/CG6/RG6 resolutions, which is the
 * cross-check that the transcription above is right. */
static const struct { unsigned char bytes_per_row, nLPR; } gfx_geom[8] = {
    { 16, 3 }, { 16, 3 }, { 32, 3 }, { 16, 2 },
    { 32, 2 }, { 16, 1 }, { 32, 1 }, { 32, 1 },
};

static void decode_graphics(cocodebug *d, uint16_t base, int GM, int GM0,
                            int css, uint32_t *pixels)
{
    int bytes_per_row = gfx_geom[GM].bytes_per_row;
    int nLPR = gfx_geom[GM].nLPR;
    int total_rows = VDG_CANVAS_H / nLPR;
    int native_w = bytes_per_row * (GM0 ? 8 : 4);
    int scale_x = VDG_CANVAS_W / native_w;
    int scale_y = nLPR;
    int row;

    for (row = 0; row < total_rows; row++) {
        uint8_t rowbuf[32];
        int col;

        cocodebug_read(d, (uint16_t)(base + row * bytes_per_row), rowbuf,
                       bytes_per_row);

        for (col = 0; col < bytes_per_row; col++) {
            uint32_t out[8];
            int n, i;

            if (GM0) { decode_rg(rowbuf[col], css, out); n = 8; }
            else     { decode_cg(rowbuf[col], css, out); n = 4; }

            for (i = 0; i < n; i++) {
                int x0 = (col * n + i) * scale_x;
                int y0 = row * scale_y;
                int sx, sy;
                for (sy = 0; sy < scale_y; sy++)
                    for (sx = 0; sx < scale_x; sx++)
                        pixels[(y0 + sy) * VDG_CANVAS_W + x0 + sx] = out[i];
            }
        }
    }
}

static int decode_vdg(cocodebug *d, struct MC6883 *sam, struct MC6821 *pia1,
                      uint32_t *pixels, int *out_w, int *out_h)
{
    unsigned vmode = (unsigned)(pia1->b.out_source & pia1->b.out_sink) &
                     0xf8u;
    int nA_G = (vmode & 0x80) != 0;
    int GM = (int)(vmode >> 4) & 7;
    int GM0 = GM & 1;
    int css = (vmode & 0x08) != 0;
    unsigned reg = sam->get_register(sam);
    uint16_t base = (uint16_t)((reg << 6) & 0xfe00u);

    if (!nA_G)
        decode_alpha_sg(d, base, GM0, css, pixels);
    else
        decode_graphics(d, base, GM, GM0, css, pixels);

    *out_w = VDG_CANVAS_W;
    *out_h = VDG_CANVAS_H;
    return 1;
}

/* ---- CoCo 3 (GIME) --------------------------------------------------------
 * See the module comment for the shape of this and what is deliberately
 * out of scope. */

/* GIME's 64-colour RGB conversion, transcribed from coco3.c's
 * coco3_finish(): a 6-bit palette register value is NOT grouped RRGGBB --
 * it is interleaved R1 G1 B1 R0 G0 B0 (bit5=R-hi, bit4=G-hi, bit3=B-hi,
 * bit2=R-lo, bit1=G-lo, bit0=B-lo), each 2-bit pair indexing measured
 * peak-to-peak voltages {0.000, 0.460, 0.736, 0.920}. This is a compile-
 * time-derivable constant, not a live one, so it needs none of the debug
 * accessor -- just the formula. */
static uint32_t gime_rgb_from_index(unsigned j)
{
    static const float intensity[4] = { 0.000f, 0.460f, 0.736f, 0.920f };
    unsigned r_idx = ((j >> 4) & 2) | ((j >> 2) & 1);
    unsigned g_idx = ((j >> 3) & 2) | ((j >> 1) & 1);
    unsigned b_idx = ((j >> 2) & 2) | (j & 1);

    return rgb_pixel((uint8_t)(intensity[r_idx] * 255.0f + 0.5f),
                     (uint8_t)(intensity[g_idx] * 255.0f + 0.5f),
                     (uint8_t)(intensity[b_idx] * 255.0f + 0.5f));
}

/* The 16 live palette registers ($FFB0-$FFBF) resolved to RGB, indexed by
 * enum tcc1014_colour (tcc1014.h) -- the register set is ordinary CPU-
 * readable memory (masked to 6 bits, per tcc1014.c), so this needs no
 * accessor, unlike everything else in this section. */
static void resolve_gime_palette(cocodebug *d, uint32_t pal[16])
{
    uint8_t reg[16];
    int i;

    cocodebug_read(d, 0xffb0, reg, 16);
    for (i = 0; i < 16; i++)
        pal[i] = gime_rgb_from_index(reg[i] & 0x3f);
}

/* Screen content, UNLIKE the palette/MMU registers above, cannot be read
 * through cocodebug_read(): the GIME fetches video RAM directly from
 * physical memory, bypassing the CPU's own MMU entirely, so a 19-bit video
 * address is not a CPU-visible 16-bit one -- confirmed from coco3.c's own
 * fetch_vram(), reproduced here:
 *
 *   unsigned Zrow = A & ~1, Zcol = A >> 9;
 *   uint8_t *Vp = ram_a8(mcc3->RAM, bank, Zrow, Zcol);
 *
 * struct ram is fully public (ram.h) with a public ram_a8(), so this reaches
 * physical memory directly the same way the machine's own video fetch does,
 * with no XRoar patch needed. bank is always 0: the DAT board that would
 * make it otherwise only exists for >512K configurations, which this port
 * does not offer. */
static void gime_vram_read(struct ram *ram, uint32_t addr, uint8_t *dst,
                           int n)
{
    int i;

    for (i = 0; i < n; i++) {
        uint32_t a = addr + (uint32_t)i;
        uint8_t *vp = ram_a8(ram, 0, a & ~1u, a >> 9);
        dst[i] = vp ? vp[a & 1u] : 0;
    }
}

/* Simulates GIME's per-scanline row/address-advance state machine
 * (tcc1014.c's do_hb_irq/do_vb_irq): "row" is a 4-bit counter that wraps
 * unconditionally, and the video address advances by row_stride whenever
 * the counter satisfies (row & rowmask) == rowmask -- not a fixed divisor,
 * which is why this is simulated rather than computed in closed form.
 * Writes one base address and one row value (the font/zone row to decode
 * that scanline's bytes with) per output scanline. */
static void gime_row_addresses(uint32_t base, unsigned rowmask,
                               unsigned row_stride, unsigned start_row,
                               int total_rows, uint32_t *addr_out,
                               unsigned *row_out)
{
    unsigned row = start_row;
    uint32_t addr = base;
    int i;

    for (i = 0; i < total_rows; i++) {
        addr_out[i] = addr;
        row_out[i] = row;
        row = (row + 1) & 15;
        if ((row & rowmask) == rowmask)
            addr += row_stride;
    }
}

static const unsigned sam_v_rowmask[8] = { 3, 3, 3, 2, 2, 1, 1, 1 };
static const unsigned vsc_rowmask[16] = {
    11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 4, 3, 2, 1, 12,
};

/* GIME's own VDG-compatible-mode decode (COCO=1). Same per-byte pixel
 * shapes as real VDG (SG4/SG6/CG/RG bit patterns), but:
 *   - geometry (BPR) comes from GIME's SAM_V parity, per
 *     update_from_gime_registers()'s COCO branch in tcc1014.c, NOT from
 *     VDG.GM the way mc6847.c's is_32byte does -- these mostly but do not
 *     always agree (GM 0 and 7 differ), so gfx_geom above must not be
 *     reused here.
 *   - colours resolve through the live palette (resolve_gime_palette())
 *     rather than the flat vdg_rgb table, and RG mode indexes the
 *     RGCSS0/1_{0,1} palette slots rather than GREEN/WHITE/BLACK/
 *     DARK_GREEN the way real VDG does (confirmed in tcc1014.c's render
 *     loop -- a real, deliberate hardware difference, not an oversight).
 * The RG2 "half resolution" variant tcc1014.c takes for one GM0=1
 * configuration is skipped -- XRoar's own comment on it calls it an
 * unconfirmed guess ("bodge to make BOINK work"), so this always renders
 * plain RG there instead of guessing further. */
static int decode_gime_coco(struct TCC1014 *gime, struct MC6821 *pia1,
                            struct ram *ram, cocodebug *d, uint32_t *pixels,
                            int *out_w, int *out_h)
{
    struct tcc1014_debug_state st;
    uint32_t pal[16];
    unsigned vmode;
    int nA_G, GM, GM0, css;
    unsigned bpr, rowmask;
    uint32_t base;
    uint32_t addr[VDG_CANVAS_H];
    unsigned rowv[VDG_CANVAS_H];
    int native_w, scale_x, y;
    uint32_t last_addr = 0xffffffffu;
    uint8_t rowbuf[32];

    tcc1014_get_debug_state(gime, &st);
    resolve_gime_palette(d, pal);

    vmode = (unsigned)(pia1->b.out_source & pia1->b.out_sink) & 0xf8u;
    nA_G = (vmode & 0x80) != 0;
    GM = (int)(vmode >> 4) & 7;
    GM0 = GM & 1;
    css = (vmode & 0x08) != 0;

    bpr = (!nA_G || !(st.SAM_V & 1)) ? 32 : 16;
    rowmask = nA_G ? sam_v_rowmask[st.SAM_V & 7] : vsc_rowmask[st.VSC & 15];
    base = (st.Y & 0x701ffu) | ((uint32_t)st.SAM_F << 9);

    gime_row_addresses(base, rowmask, bpr, 0, VDG_CANVAS_H, addr, rowv);

    native_w = bpr * (nA_G ? (GM0 ? 8 : 4) : 8);
    scale_x = VDG_CANVAS_W / native_w;

    for (y = 0; y < VDG_CANVAS_H; y++) {
        int subrow = (int)(rowv[y] & 15);
        int col;

        if (addr[y] != last_addr) {
            gime_vram_read(ram, addr[y], rowbuf, (int)bpr);
            last_addr = addr[y];
        }

        for (col = 0; col < (int)bpr; col++) {
            uint8_t byte = rowbuf[col];
            uint32_t out[8];
            uint32_t fg, bg;
            int n, i;

            if (!nA_G && !(byte & 0x80)) {
                /* Alpha. GIME reimplements this with its OWN font
                 * (font_gime, not font_6847) and a 7-bit code with a
                 * low/high-range remap plus a GM1-based invert -- this
                 * is confirmed from tcc1014.c's COCO-mode alphanumeric
                 * branch, not assumed from real standalone-VDG behaviour
                 * (which differs: a plain 6-bit code into font_6847). */
                int GM1 = (GM >> 1) & 1;
                int inv = ((byte & 0x40) != 0) ^ GM1;
                int c = byte & 0x7f;
                uint8_t glyph;

                if (c < 0x20) {
                    c |= GM0 ? 0x60 : 0x40;
                    inv ^= GM0;
                } else if (c >= 0x60) {
                    c ^= 0x40;
                }
                glyph = font_gime[c * 12 + (subrow % 12)];
                fg = css ? pal[TCC1014_BRIGHT_ORANGE] : pal[TCC1014_BRIGHT_GREEN];
                bg = css ? pal[TCC1014_DARK_ORANGE] : pal[TCC1014_DARK_GREEN];
                if (inv) { uint32_t t = fg; fg = bg; bg = t; }
                for (i = 0; i < 8; i++)
                    out[i] = (glyph & (0x80 >> i)) ? fg : bg;
                n = 8;
            } else if (!nA_G) {
                /* Semigraphics. GIME's COCO-mode emulation has only this
                 * ONE path -- confirmed from tcc1014.c, which does not
                 * branch on GM0/EXT here the way real standalone VDG's
                 * SG4-vs-SG6 split does. Always 8 colours from bits 6-4,
                 * 2 zones (row<6 / row>=6) from bits 3-2 / 1-0,
                 * background fixed to RGCSS0_0 regardless of CSS. */
                int sr = subrow % 12;
                int bits = (sr < 6) ? ((byte >> 2) & 3) : (byte & 3);
                fg = pal[(byte >> 4) & 7];
                bg = pal[TCC1014_RGCSS0_0];
                out[0] = (bits & 2) ? fg : bg;
                out[1] = (bits & 1) ? fg : bg;
                n = 2;
            } else if (GM0) {
                /* RG: fg/bg from the RGCSS palette slots, not GREEN/WHITE/
                 * BLACK/DARK_GREEN -- see the function comment. */
                fg = pal[css ? TCC1014_RGCSS1_1 : TCC1014_RGCSS0_1];
                bg = pal[css ? TCC1014_RGCSS1_0 : TCC1014_RGCSS0_0];
                for (i = 0; i < 8; i++)
                    out[i] = (byte & (0x80 >> i)) ? fg : bg;
                n = 8;
            } else {
                /* CG: 4 colours from one of two CSS-selected sets. */
                int base_c = css ? TCC1014_WHITE : TCC1014_GREEN;
                for (i = 0; i < 4; i++)
                    out[i] = pal[base_c + ((byte >> (6 - i * 2)) & 3)];
                n = 4;
            }

            {
                int px_per = 8 / n;
                int k;
                for (i = 0; i < n; i++) {
                    int x0 = (col * 8 + i * px_per) * scale_x;
                    for (k = 0; k < px_per * scale_x; k++)
                        pixels[y * VDG_CANVAS_W + x0 + k] = out[i];
                }
            }
        }
    }

    *out_w = VDG_CANVAS_W;
    *out_h = VDG_CANVAS_H;
    return 1;
}

/* GIME native mode (COCO=0): its own VMODE/VRES register set, entirely
 * independent of the VDG/SAM concepts above. Bytes-per-row per HRES
 * (0-7), transcribed from tcc1014.c's VRES_HRES_BPR[]/VRES_HRES_BPR_TEXT[];
 * lines-per-field per LPF (0-3) from VRES_LPF_lAA[]. LPF==2 ("infinite",
 * no vertical blanking) is clamped to the canvas height bound rather than
 * treated as literally unbounded. */
static const unsigned hres_bpr_gfx[8]  = { 16, 20, 32, 40, 64, 80, 128, 160 };
static const unsigned hres_bpr_text[8] = { 32, 40, 32, 40, 64, 80,  64,  80 };
static const unsigned lpr_rowmask[8] = { 0, 1, 2, 8, 9, 10, 11, 16 };

static int decode_gime_native(struct TCC1014 *gime, struct ram *ram,
                              cocodebug *d, uint32_t *pixels, int *out_w,
                              int *out_h)
{
    struct tcc1014_debug_state st;
    uint32_t pal[16];
    unsigned bpr, rowmask, row_stride;
    unsigned start_row;
    int total_rows, native_w, bits_per_px, px_per_byte;
    uint32_t addr[COCOVIDEO_MAX_HEIGHT];
    unsigned rowv[COCOVIDEO_MAX_HEIGHT];
    int y;
    uint32_t last_addr = 0xffffffffu;
    uint8_t rowbuf[160];

    tcc1014_get_debug_state(gime, &st);
    resolve_gime_palette(d, pal);

    bpr = st.BP ? hres_bpr_gfx[st.HRES & 7] : hres_bpr_text[st.HRES & 7];
    rowmask = lpr_rowmask[st.LPR & 7];
    row_stride = st.BP ? bpr : (bpr << (st.CRES & 1));

    /* VRES_LPF_lAA[4] in tcc1014.c: {192, 199, 65535 ("infinite", no
     * vertical blanking), 225}. The infinite case is clamped to the
     * canvas bound rather than treated as literally unbounded. */
    {
        static const int lpf_lines[4] = { 192, 199, COCOVIDEO_MAX_HEIGHT, 225 };
        total_rows = lpf_lines[st.LPF & 3];
    }
    if (total_rows > COCOVIDEO_MAX_HEIGHT)
        total_rows = COCOVIDEO_MAX_HEIGHT;

    start_row = st.VSC & 15;
    if ((start_row & rowmask) == rowmask)
        start_row = 0;

    if (bpr > sizeof(rowbuf))
        bpr = (unsigned)sizeof(rowbuf);

    gime_row_addresses(st.Y, rowmask, row_stride, start_row, total_rows,
                       addr, rowv);

    if (st.BP) {
        switch (st.CRES) {
        case 0: default: bits_per_px = 1; break;
        case 1:          bits_per_px = 2; break;
        case 2: case 3:  bits_per_px = 4; break;
        }
        px_per_byte = 8 / bits_per_px;
    } else {
        bits_per_px = 0; /* text: handled separately below */
        px_per_byte = 8;
    }

    native_w = (int)bpr * px_per_byte;
    if (native_w > COCOVIDEO_MAX_WIDTH)
        native_w = COCOVIDEO_MAX_WIDTH;

    for (y = 0; y < total_rows; y++) {
        int font_row = (int)((rowv[y] + 1) & 0x0f);
        int col;
        unsigned fetch_bpr = st.BP ? bpr : (bpr << (st.CRES & 1));

        if (font_row > 11)
            font_row = 0;

        if (fetch_bpr > sizeof(rowbuf))
            fetch_bpr = (unsigned)sizeof(rowbuf);

        if (addr[y] != last_addr) {
            gime_vram_read(ram, addr[y], rowbuf, (int)fetch_bpr);
            last_addr = addr[y];
        }

        if (st.BP) {
            /* Graphics: 1/2/4bpp, palette-indexed. */
            for (col = 0; col < (int)bpr && col * px_per_byte < native_w;
                col++) {
                uint8_t byte = rowbuf[col];
                int px;
                for (px = 0; px < px_per_byte; px++) {
                    unsigned bits =
                        (byte >> (8 - bits_per_px * (px + 1))) &
                        ((1u << bits_per_px) - 1);
                    int x0 = col * px_per_byte + px;
                    if (x0 >= native_w)
                        break;
                    pixels[y * native_w + x0] = pal[bits & 15];
                }
            }
        } else {
            /* Text: character (+ optional attribute) byte pairs. bpr is
             * already a CELL count (hres_bpr_text[]), unaffected by
             * whether attributes double the byte count -- fetch_bpr is
             * what doubles, not the cell count. */
            int cell;
            int cells = (int)bpr;
            const uint8_t *p = rowbuf;

            for (cell = 0; cell < cells && cell * 8 < native_w; cell++) {
                int c = *p++ & 0x7f;
                uint8_t glyph = font_gime[c * 12 + font_row];
                uint32_t fg, bg;
                int px;

                if (st.CRES & 1) {
                    uint8_t attr = *p++;
                    fg = pal[8 | ((attr >> 3) & 7)];
                    bg = pal[attr & 7];
                    if ((attr & 0x40) &&
                        ((unsigned)font_row & rowmask) == rowmask)
                        glyph = 0xff;
                    /* Blink (attr bit 7) needs the live blink phase,
                     * which is animation state this static decode has
                     * no notion of; always shown "on" rather than
                     * guessed. */
                } else {
                    /* No attribute byte: fg/bg hardwired to palette_reg[1]
                     * and palette_reg[0], per tcc1014.c's render loop. */
                    fg = pal[1];
                    bg = pal[0];
                }

                for (px = 0; px < 8; px++) {
                    int x0 = cell * 8 + px;
                    if (x0 >= native_w)
                        break;
                    pixels[y * native_w + x0] =
                        (glyph & (0x80 >> px)) ? fg : bg;
                }
            }
        }
    }

    *out_w = native_w;
    *out_h = total_rows;
    return 1;
}

static int decode_gime(struct TCC1014 *gime, struct MC6821 *pia1,
                       struct ram *ram, cocodebug *d, uint32_t *pixels,
                       int *out_w, int *out_h)
{
    struct tcc1014_debug_state st;

    tcc1014_get_debug_state(gime, &st);
    if (st.COCO)
        return decode_gime_coco(gime, pia1, ram, d, pixels, out_w, out_h);
    return decode_gime_native(gime, ram, d, pixels, out_w, out_h);
}

/* ---- entry point ---------------------------------------------------------
 * Identifies which video hardware the running machine has (a CoCo 3's GIME
 * supersedes the SAM/VDG entirely) the same way debugger.c reaches the CPU:
 * part_component_by_id_is_a() against the live machine's component tree. */
int cocodebug_video_decode(cocodebug *d, uint32_t *pixels, int *out_w,
                           int *out_h)
{
    struct part *mp;
    struct TCC1014 *gime;
    struct MC6883 *sam;
    struct MC6821 *pia1;
    struct ram *ram;

    if (!d || !pixels || !out_w || !out_h)
        return 0;
    if (!cocodebug_is_paused(d))
        return 0;
    if (!xroar.machine)
        return 0;
    mp = &xroar.machine->part;

    pia1 = (struct MC6821 *)part_component_by_id_is_a(mp, "PIA1", "MC6821");
    gime = (struct TCC1014 *)part_component_by_id_is_a(mp, "GIME", "TCC1014");
    if (gime && pia1) {
        ram = (struct ram *)part_component_by_id_is_a(mp, "RAM", "ram");
        if (!ram)
            return 0;
        return decode_gime(gime, pia1, ram, d, pixels, out_w, out_h);
    }

    sam = (struct MC6883 *)part_component_by_id_is_a(mp, "SAM", "SN74LS783");
    if (sam && pia1)
        return decode_vdg(d, sam, pia1, pixels, out_w, out_h);

    return 0;
}
