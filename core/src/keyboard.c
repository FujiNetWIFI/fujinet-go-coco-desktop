/*
 * keyboard -- the on-screen keyboard: the key caps as they are printed on the
 * CoCo's keyboard.
 *
 * The CoCo 2 has 53 keys: the digit row (1-9, 0, :, -, BREAK), the Q row (CLEAR,
 * Q-P, @), the A row (A-L, ;, ENTER), the Z row (SHIFT, Z-M, comma, period,
 * slash, SHIFT), SPACE and the four arrows. The CoCo 3 adds F1 and F2, CTRL
 * (beside A) and ALT (below the left SHIFT): 57. There is no RESET key.
 *
 * A cap does not name a CoCo key, it names the USB HID usage XRoar's keyboard
 * layer maps onto the CoCo's key matrix (XRoar's hkbd tables are positional):
 * the same usage the host keyboard path (input_map.c) sends for the same key,
 * so a cap and a physical key are the same thing to the machine. That is why
 * the cap printed ":" holds the usage of the host's "-" key (0x2d), the cap
 * "-" the host's "=" (0x2e), and "@" the host's "[" (0x2f): the host symbols
 * are the US layout's, the CoCo's own legends are elsewhere. CTRL is the usage
 * of Caps Lock (0x39), which is what XRoar's CoCo 3 keymap turns into CTRL;
 * the host's Control keys (0xe0, 0xe4) are XRoar's own emulator-command
 * modifier and must not be used.
 *
 * Geometry: units are key caps (a key is 1.0 x 1.0, the gap between caps 0.1
 * already in the coordinates, so the pitch is 1.1). It follows the typewriter
 * layout of the CoCo 2 and 3 with the four arrows as a cluster at the right,
 * and it is approximate: the key set and what each key does are exact.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stddef.h>

#include "cocosession.h"

#define GAP   0.1f
#define PITCH (1.0f + GAP)

/* A cap `wid` pitches wide at column `col` of row `row` (both in pitches). */
#define KEY(lbl, shf, hidcode, col, row, wid, kind) \
    { lbl, shf, hidcode, (col) * PITCH, (row) * PITCH, (wid) * PITCH - GAP, 1.0f, kind }
#define NRM COCO_KEYCAP_NORMAL
#define SPC COCO_KEYCAP_SPECIAL
#define MOD COCO_KEYCAP_MODIFIER
#define ARW COCO_KEYCAP_ARROW
#define SPB COCO_KEYCAP_SPACE
#define FUN COCO_KEYCAP_FUNCTION

/* The keys the two machines share, bar the A row and what sits beside it. */
#define COMMON_TOP \
    KEY("1", "!", 0x1e, 0, 0, 1, NRM), KEY("2", "\"", 0x1f, 1, 0, 1, NRM), \
    KEY("3", "#", 0x20, 2, 0, 1, NRM), KEY("4", "$", 0x21, 3, 0, 1, NRM), \
    KEY("5", "%", 0x22, 4, 0, 1, NRM), KEY("6", "&", 0x23, 5, 0, 1, NRM), \
    KEY("7", "'", 0x24, 6, 0, 1, NRM), KEY("8", "(", 0x25, 7, 0, 1, NRM), \
    KEY("9", ")", 0x26, 8, 0, 1, NRM), KEY("0", NULL, 0x27, 9, 0, 1, NRM), \
    KEY(":", "*", 0x2d, 10, 0, 1, NRM), KEY("-", "=", 0x2e, 11, 0, 1, NRM), \
    KEY("BREAK", NULL, 0x29, 12, 0, 1.5f, SPC), \
    KEY("CLEAR", NULL, 0x4a, 0, 1, 1.5f, SPC), \
    KEY("Q", NULL, 0x14, 1.5f, 1, 1, NRM), KEY("W", NULL, 0x1a, 2.5f, 1, 1, NRM), \
    KEY("E", NULL, 0x08, 3.5f, 1, 1, NRM), KEY("R", NULL, 0x15, 4.5f, 1, 1, NRM), \
    KEY("T", NULL, 0x17, 5.5f, 1, 1, NRM), KEY("Y", NULL, 0x1c, 6.5f, 1, 1, NRM), \
    KEY("U", NULL, 0x18, 7.5f, 1, 1, NRM), KEY("I", NULL, 0x0c, 8.5f, 1, 1, NRM), \
    KEY("O", NULL, 0x12, 9.5f, 1, 1, NRM), KEY("P", NULL, 0x13, 10.5f, 1, 1, NRM), \
    KEY("@", NULL, 0x2f, 11.5f, 1, 1, NRM)

#define COMMON_BOTTOM \
    KEY("SHIFT", NULL, 0xe1, 0, 3, 1.5f, MOD), \
    KEY("Z", NULL, 0x1d, 1.5f, 3, 1, NRM), KEY("X", NULL, 0x1b, 2.5f, 3, 1, NRM), \
    KEY("C", NULL, 0x06, 3.5f, 3, 1, NRM), KEY("V", NULL, 0x19, 4.5f, 3, 1, NRM), \
    KEY("B", NULL, 0x05, 5.5f, 3, 1, NRM), KEY("N", NULL, 0x11, 6.5f, 3, 1, NRM), \
    KEY("M", NULL, 0x10, 7.5f, 3, 1, NRM), \
    KEY(",", "<", 0x36, 8.5f, 3, 1, NRM), KEY(".", ">", 0x37, 9.5f, 3, 1, NRM), \
    KEY("/", "?", 0x38, 10.5f, 3, 1, NRM), \
    KEY("SHIFT", NULL, 0xe5, 11.5f, 3, 1, MOD), \
    KEY("\xE2\x86\x91", NULL, 0x52, 14, 1, 1, ARW), \
    KEY("\xE2\x86\x90", NULL, 0x50, 13, 2, 1, ARW), \
    KEY("\xE2\x86\x93", NULL, 0x51, 14, 2, 1, ARW), \
    KEY("\xE2\x86\x92", NULL, 0x4f, 15, 2, 1, ARW)

static const coco_keycap k_coco2[] = {
    COMMON_TOP,
    /* the A row */
    KEY("A", NULL, 0x04, 0.5f, 2, 1, NRM), KEY("S", NULL, 0x16, 1.5f, 2, 1, NRM),
    KEY("D", NULL, 0x07, 2.5f, 2, 1, NRM), KEY("F", NULL, 0x09, 3.5f, 2, 1, NRM),
    KEY("G", NULL, 0x0a, 4.5f, 2, 1, NRM), KEY("H", NULL, 0x0b, 5.5f, 2, 1, NRM),
    KEY("J", NULL, 0x0d, 6.5f, 2, 1, NRM), KEY("K", NULL, 0x0e, 7.5f, 2, 1, NRM),
    KEY("L", NULL, 0x0f, 8.5f, 2, 1, NRM),
    KEY(";", "+", 0x33, 9.5f, 2, 1, NRM),
    KEY("ENTER", NULL, 0x28, 10.5f, 2, 2, SPC),
    COMMON_BOTTOM,
    KEY("SPACE", NULL, 0x2c, 2.5f, 4, 7.5f, SPB),
};

static const coco_keycap k_coco3[] = {
    COMMON_TOP,
    /* the F keys above the arrows */
    KEY("F1", NULL, 0x3a, 14, 0, 1, FUN), KEY("F2", NULL, 0x3b, 15, 0, 1, FUN),
    /* the A row, with CTRL at its left */
    KEY("CTRL", NULL, 0x39, 0, 2, 1.25f, MOD),
    KEY("A", NULL, 0x04, 1.25f, 2, 1, NRM), KEY("S", NULL, 0x16, 2.25f, 2, 1, NRM),
    KEY("D", NULL, 0x07, 3.25f, 2, 1, NRM), KEY("F", NULL, 0x09, 4.25f, 2, 1, NRM),
    KEY("G", NULL, 0x0a, 5.25f, 2, 1, NRM), KEY("H", NULL, 0x0b, 6.25f, 2, 1, NRM),
    KEY("J", NULL, 0x0d, 7.25f, 2, 1, NRM), KEY("K", NULL, 0x0e, 8.25f, 2, 1, NRM),
    KEY("L", NULL, 0x0f, 9.25f, 2, 1, NRM),
    KEY(";", "+", 0x33, 10.25f, 2, 1, NRM),
    KEY("ENTER", NULL, 0x28, 11.25f, 2, 1.25f, SPC),
    COMMON_BOTTOM,
    KEY("ALT", NULL, 0xe2, 0, 4, 2.4f, MOD),
    KEY("SPACE", NULL, 0x2c, 2.5f, 4, 7.5f, SPB),
};

/* 16 pitches wide (the arrows' last column ends there), 5 rows high. */
#define KEYS_WIDE (16 * PITCH - GAP)
#define KEYS_HIGH (4 * PITCH + 1.0f)

const coco_keycap *cocosession_keyboard_layout(int machine, int *count, float *wide, float *high)
{
    const int coco2 = machine == COCOSESSION_MACHINE_COCO2;

    if (count) *count = (int)(coco2 ? sizeof k_coco2 / sizeof k_coco2[0]
                                    : sizeof k_coco3 / sizeof k_coco3[0]);
    if (wide) *wide = KEYS_WIDE;
    if (high) *high = KEYS_HIGH;
    return coco2 ? k_coco2 : k_coco3;
}
