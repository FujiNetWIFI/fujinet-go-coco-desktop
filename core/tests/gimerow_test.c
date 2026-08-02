/*
 * gimerow_test -- cocovideo.c's GIME row/address state machine against a
 * literal transcription of the hardware it is modelling.
 *
 * This covers the one piece of cocovideo.c that is pure arithmetic over
 * register values: gime_row_addresses(), which decides -- for every output
 * scanline -- which video address to fetch that row's bytes from and which
 * font/zone row to decode them with. Everything else in that file needs a
 * live machine and is covered by video_test; this needs none, so it runs in
 * milliseconds and always runs (no ROM skip).
 *
 * It exists because a bug here is invisible to video_test: a wrong row
 * stride still decodes to a busy, many-coloured frame, which is all
 * video_test can check without a reference image. The specific regression
 * guarded is a real one -- the counter used to free-run 0-15 instead of
 * restarting at each character row, so with rowmask 8 the video address
 * advanced on eight scanlines out of every sixteen rather than once every
 * eight, and the decode raced through screen RAM roughly four times too
 * fast. On a CoCo 3 in FujiNet's CONFIG that produced a visibly shredded
 * picture; CoCo 2 goes through decode_vdg() and never touches this code.
 *
 * gime_row_addresses() is static, and deliberately so -- it is an internal
 * detail of one decode, not API. Including the translation unit is how this
 * suite reaches it without widening cocovideo.c's surface just for a test.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>

#include "cocovideo.c"

#define MAX_ROWS 225

static int failures;

static void ok(const char *what, int cond)
{
    if (cond) {
        printf("ok   %s\n", what);
    } else {
        printf("FAIL %s\n", what);
        failures++;
    }
}

/* tcc1014.c's do_hb_irq() and do_vb_irq(), transcribed in the order and
 * shape the hardware runs them rather than folded into the tighter form
 * gime_row_addresses() uses -- the point of this reference is to be
 * obviously the same as the emulation it came from, not to be concise.
 *
 * Per scanline the GIME does: do_vb_irq() near the start of the left border,
 * then renders the active area with whatever row/B it now holds, then
 * do_hb_irq() at the right border. row_advance is carried between the two.
 *
 * do_vb_irq()'s "!COCO && LPR == 7" case (XRoar's BOINK bodge, which forces
 * row_advance back to 0) is not reproduced: LPR 7 maps to rowmask 16, which
 * a 4-bit row can never match, so nothing sets row_advance in the first
 * place and the special case is already subsumed. test_lpr7() pins that. */
static void ref_row_addresses(uint32_t base, unsigned rowmask,
                              unsigned row_stride, unsigned start_row,
                              int total_rows, uint32_t *addr_out,
                              unsigned *row_out)
{
    unsigned row = start_row;
    uint32_t B = base;
    int row_advance = 0;
    int i;

    for (i = 0; i < total_rows; i++) {
        /* do_vb_irq(), active-area branch. Skipped on the first scanline:
         * the vb_irq that starts the active area is the one that seeds row
         * (from VSC, or 0 in COCO mode) and takes the border branch, so it
         * cannot also advance B. */
        if (i > 0) {
            if ((row & rowmask) == rowmask) {
                row = 0;
                row_advance = 1;
            }
            if (row_advance)
                B += row_stride;
        }

        /* render_scanline() -- uses row and B as they now stand. */
        addr_out[i] = B;
        row_out[i] = row;

        /* do_hb_irq(). */
        row_advance = 0;
        row = (row + 1) & 15;
        if ((row & rowmask) == rowmask)
            row_advance = 1;
    }
}

/* Every rowmask the three GIME tables can select, from tcc1014.c's
 * LPR_rowmask[], SAM_V_rowmask[] and VSC_rowmask[]. */
static const unsigned all_rowmasks[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 16,
};

static void test_matches_hardware(void)
{
    uint32_t got_a[MAX_ROWS], want_a[MAX_ROWS];
    unsigned got_r[MAX_ROWS], want_r[MAX_ROWS];
    unsigned mi, start_row;
    int bad = 0;

    for (mi = 0; mi < sizeof(all_rowmasks) / sizeof(all_rowmasks[0]); mi++) {
        unsigned rowmask = all_rowmasks[mi];

        for (start_row = 0; start_row < 16; start_row++) {
            /* 40-byte text and 160-byte graphics rows, plus HVEN's fixed
             * 256, are the strides the callers actually pass. */
            static const unsigned strides[] = { 32, 40, 160, 256 };
            unsigned si;

            for (si = 0; si < 4; si++) {
                int i;

                gime_row_addresses(0x60000u, rowmask, strides[si], start_row,
                                   MAX_ROWS, got_a, got_r);
                ref_row_addresses(0x60000u, rowmask, strides[si], start_row,
                                  MAX_ROWS, want_a, want_r);

                for (i = 0; i < MAX_ROWS; i++) {
                    if (got_a[i] != want_a[i] || got_r[i] != want_r[i]) {
                        if (!bad)
                            printf("     first mismatch: rowmask=%u "
                                   "start_row=%u stride=%u scanline=%d: "
                                   "addr %05x/%05x row %u/%u\n",
                                   rowmask, start_row, strides[si], i,
                                   got_a[i], want_a[i], got_r[i], want_r[i]);
                        bad++;
                    }
                }
            }
        }
    }

    ok("matches do_hb_irq/do_vb_irq for every rowmask, start row and stride",
       bad == 0);
}

/* The property the free-running counter destroyed: rowmask is a
 * lines-per-character-row selector, and GIME's LPR register documents
 * exactly which. Derived from the address-advance pattern, so it tests the
 * behaviour a decode depends on rather than restating the implementation. */
static void test_lines_per_row(void)
{
    /* LPR 0-6 against LPR_rowmask[]; LPR 7 ("infinite") is test_lpr7(). */
    static const unsigned lpr_rowmask_tbl[7] = { 0, 1, 2, 8, 9, 10, 11 };
    static const int documented_lines[7] = { 1, 1, 2, 8, 9, 10, 11 };
    uint32_t addr[MAX_ROWS];
    unsigned rowv[MAX_ROWS];
    int lpr;

    for (lpr = 0; lpr < 7; lpr++) {
        char what[96];
        int advances = 0, first_advance = -1, i, regular = 1;

        gime_row_addresses(0x60000u, lpr_rowmask_tbl[lpr], 100, 0, MAX_ROWS,
                           addr, rowv);

        for (i = 1; i < MAX_ROWS; i++) {
            if (addr[i] == addr[i - 1])
                continue;
            if (first_advance < 0)
                first_advance = i;
            /* Every advance must be one stride, evenly spaced. */
            if (addr[i] != addr[i - 1] + 100 || i % first_advance != 0)
                regular = 0;
            advances++;
        }

        snprintf(what, sizeof(what),
                 "LPR=%d (rowmask %u) gives %d scanlines per character row",
                 lpr, lpr_rowmask_tbl[lpr], documented_lines[lpr]);
        ok(what, first_advance == documented_lines[lpr] && regular &&
                 advances == (MAX_ROWS - 1) / documented_lines[lpr]);
    }
}

/* LPR 7: no vertical blanking, so the address never advances at all. */
static void test_lpr7(void)
{
    uint32_t addr[MAX_ROWS];
    unsigned rowv[MAX_ROWS];
    int i, moved = 0;

    gime_row_addresses(0x60000u, 16, 40, 0, MAX_ROWS, addr, rowv);
    for (i = 0; i < MAX_ROWS; i++)
        if (addr[i] != 0x60000u)
            moved++;
    ok("LPR=7 (rowmask 16) never advances the video address", moved == 0);
}

/* The decode indexes a 12-row glyph with the row this hands back, so a row
 * that runs past the character cell is what put nonexistent font rows on
 * screen. Within a character row the counter must stay below that row's
 * height. */
static void test_row_stays_in_cell(void)
{
    static const unsigned tbl[7] = { 0, 1, 2, 8, 9, 10, 11 };
    static const int lines[7] = { 1, 1, 2, 8, 9, 10, 11 };
    uint32_t addr[MAX_ROWS];
    unsigned rowv[MAX_ROWS];
    int lpr, bad = 0;

    for (lpr = 0; lpr < 7; lpr++) {
        int i;
        gime_row_addresses(0x60000u, tbl[lpr], 40, 0, MAX_ROWS, addr, rowv);
        for (i = 0; i < MAX_ROWS; i++)
            if (rowv[i] >= (unsigned)lines[lpr] ||
                rowv[i] != (unsigned)(i % lines[lpr]))
                bad++;
    }
    ok("the row counter restarts each character row and stays inside it",
       bad == 0);
}

int main(void)
{
    test_matches_hardware();
    test_lines_per_row();
    test_lpr7();
    test_row_stays_in_cell();

    if (failures) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    printf("\nall GIME row state machine tests passed\n");
    return 0;
}
