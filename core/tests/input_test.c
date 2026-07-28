/*
 * input_test -- the pure key and joystick mapping functions (core/src/input_map.c).
 *
 * These are what every frontend calls, so getting them wrong breaks all four
 * at once; they are pure, so they can be checked without a core, a window or a
 * session.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>

#include "cocosession.h"

static int failures;

static void expect_key(const char *what, uint32_t keysym, uint32_t unicode,
                       int ctrl, int shift, int want_ok, unsigned want_scan)
{
    unsigned scan = 0xFFFF;
    int ok = coco_key_from_event(keysym, unicode, ctrl, shift, &scan);

    if (ok != want_ok) {
        printf("FAIL %s: forwarded=%d, expected %d\n", what, ok, want_ok);
        failures++;
        return;
    }
    if (ok && scan != want_scan) {
        printf("FAIL %s: scancode 0x%02x, expected 0x%02x\n", what, scan,
               want_scan);
        failures++;
        return;
    }
    printf("ok   %s\n", what);
}

static void expect_axis(const char *what, float v, float deadzone, int want)
{
    int got = coco_axis_from_stick(v, deadzone);
    if (got != want) {
        printf("FAIL %s: %d, expected %d\n", what, got, want);
        failures++;
        return;
    }
    printf("ok   %s\n", what);
}

static void expect_axis_near(const char *what, float v, float deadzone,
                             int want, int tol)
{
    int got = coco_axis_from_stick(v, deadzone);
    int d = got - want;
    if (d < 0) d = -d;
    if (d > tol) {
        printf("FAIL %s: %d, expected %d +/- %d\n", what, got, want, tol);
        failures++;
        return;
    }
    printf("ok   %s\n", what);
}

static void expect_pad(const char *what, const int *bindings, int npads,
                       int port, int want)
{
    int got = coco_pad_for_port(bindings, npads, port);
    if (got != want) {
        printf("FAIL %s: pad %d, expected %d\n", what, got, want);
        failures++;
        return;
    }
    printf("ok   %s\n", what);
}

int main(void)
{
    /* --- letters and digits: HID positions ------------------------------ */
    expect_key("'a' -> hk_scan_a", 'a', 'a', 0, 0, 1, 0x04);
    expect_key("'z' -> hk_scan_z", 'z', 'z', 0, 0, 1, 0x1d);
    /* Shift-A reports the keysym 'A'; it is the same physical key as 'a'. */
    expect_key("'A' -> hk_scan_a", 'A', 'A', 0, 1, 1, 0x04);
    expect_key("'1' -> hk_scan_1", '1', '1', 0, 0, 1, 0x1e);
    expect_key("'9' -> hk_scan_9", '9', '9', 0, 0, 1, 0x26);
    /* HID puts 1..9 before 0, so '0' is NOT hk_scan_1 + 9. */
    expect_key("'0' -> hk_scan_0", '0', '0', 0, 0, 1, 0x27);

    /* --- shifted symbols resolve to the unshifted US key ---------------- */
    expect_key("'!' -> hk_scan_1", '!', '!', 0, 1, 1, 0x1e);
    /* A US keyboard's shift-2. */
    expect_key("'@' -> hk_scan_2", '@', '@', 0, 1, 1, 0x1f);
    /* A UK keyboard's shift-2 -- the same physical key, and the one that
     * types " on a CoCo too. */
    expect_key("'\"' -> hk_scan_apostrophe", '"', '"', 0, 1, 1, 0x34);
    expect_key("':' -> hk_scan_semicolon", ':', ':', 0, 1, 1, 0x33);
    expect_key("'?' -> hk_scan_slash", '?', '?', 0, 1, 1, 0x38);

    /* --- the CoCo's named keys ----------------------------------------- */
    expect_key("Return", 0xFF0D, 0, 0, 0, 1, 0x28);
    expect_key("Escape is BREAK", 0xFF1B, 0, 0, 0, 1, 0x29);
    expect_key("Home is CLEAR", 0xFF50, 0, 0, 0, 1, 0x4a);
    expect_key("space", ' ', ' ', 0, 0, 1, 0x2c);
    expect_key("Left arrow", 0xFF51, 0, 0, 0, 1, 0x50);
    expect_key("Up arrow", 0xFF52, 0, 0, 0, 1, 0x52);
    expect_key("Right arrow", 0xFF53, 0, 0, 0, 1, 0x4f);
    expect_key("Down arrow", 0xFF54, 0, 0, 0, 1, 0x51);
    expect_key("Shift_L", 0xFFE1, 0, 0, 1, 1, 0xe1);
    expect_key("Control_L (CoCo 3 CTRL)", 0xFFE3, 0, 1, 0, 1, 0xe0);
    expect_key("Alt_L (CoCo 3 ALT)", 0xFFE9, 0, 0, 0, 1, 0xe2);
    expect_key("F1 (CoCo 3)", 0xFFBE, 0, 0, 0, 1, 0x3a);
    expect_key("F2 (CoCo 3)", 0xFFBF, 0, 0, 0, 1, 0x3b);

    /* --- Ctrl combos still identify the key ----------------------------- */
    /* Some toolkits report the control character in `unicode`; the keysym is
     * preferred precisely so Ctrl-C is still the C key. */
    expect_key("Ctrl-C", 'c', 0x03, 1, 0, 1, 0x06);

    /* --- keys the frontends keep for themselves ------------------------- */
    expect_key("F10 is reserved (menu)", 0xFFC7, 0, 0, 0, 0, 0);
    expect_key("F11 is reserved (fullscreen)", 0xFFC8, 0, 0, 0, 0, 0);
    expect_key("F12 is reserved (debugger)", 0xFFC9, 0, 0, 0, 0, 0);

    /* --- anything with no CoCo key is dropped --------------------------- */
    expect_key("Print Screen is dropped", 0xFF61, 0, 0, 0, 0, 0);
    expect_key("a non-ASCII keysym is dropped", 0x00E9 /* eacute */, 0x00E9,
               0, 0, 0, 0);

    /* --- the analog joystick: 0..65535 with 32767 at centre ------------- */
    expect_axis("centre", 0.0f, 0.0f, 32767);
    expect_axis("full left/up", -1.0f, 0.0f, 0);
    expect_axis("full right/down", 1.0f, 0.0f, 65535);
    /* Out of range is clamped, not wrapped. */
    expect_axis("beyond full travel clamps", -2.0f, 0.0f, 0);
    expect_axis("beyond full travel clamps", 2.0f, 0.0f, 65535);
    /* Inside the deadzone reads as centred... */
    expect_axis("inside the deadzone", 0.1f, 0.18f, 32767);
    expect_axis("just inside the deadzone", -0.18f, 0.18f, 32767);
    /* ...and the live travel is rescaled, so the stops are still reachable
     * with a deadzone applied. */
    expect_axis("deadzone still reaches the stop", 1.0f, 0.18f, 65535);
    expect_axis("deadzone still reaches the stop", -1.0f, 0.18f, 0);
    /* Just past a 0.18 deadzone is just past centre, not a jump to the edge. */
    expect_axis_near("just past the deadzone", 0.19f, 0.18f, 33167, 400);

    /* --- gamepad -> joystick port assignment ---------------------------- */
    {
        /* -1 means "no explicit binding": the automatic case. */
        const int none[]  = { -1, -1, -1 };
        const int left0[] = { 1, -1 };      /* pad 0 pinned to the left port */
        const int both[]  = { 1, 0 };       /* deliberately crossed over */

        expect_pad("no pads, no assignment", NULL, 0, 0, -1);
        expect_pad("one pad takes the right port", none, 1, 0, 0);
        expect_pad("...and nothing drives the left", none, 1, 1, -1);
        expect_pad("a second pad takes the left port", none, 2, 1, 1);
        expect_pad("...while the first keeps the right", none, 2, 0, 0);
        expect_pad("a third pad drives nothing", none, 3, 2, -1);
        /* Pinning pad 0 to the left must hand the right port to pad 1, not
         * leave it dead -- the bug a naive "port'th unbound pad" loop has. */
        expect_pad("pinning pad 0 left frees the right for pad 1",
                   left0, 2, 0, 1);
        expect_pad("...and pad 0 still drives the left", left0, 2, 1, 0);
        expect_pad("explicit bindings can be crossed over", both, 2, 0, 1);
        expect_pad("...both ways", both, 2, 1, 0);
    }

    if (failures) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    printf("\nall input mapping tests passed\n");
    return 0;
}
