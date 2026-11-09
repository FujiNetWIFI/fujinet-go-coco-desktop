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
#include <string.h>

#include "coco_host.h"
#include "cocosession.h"
#include "session_internal.h"
#include "test_tmpdir.h"

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

static void check(int pass, const char *what)
{
    printf("%s   %s\n", pass ? "ok" : "FAIL", what);
    if (!pass) failures++;
}

/* ---- deflection: the analog joystick in milli-units ------------------------ */

static void deflection_tests(void)
{
    int xaxis, yaxis;

    check(coco_axis_from_deflection(0) == 32767, "a centred deflection is 32767");
    check(coco_axis_from_deflection(-1000) == 0 && coco_axis_from_deflection(1000) == 65535, "the stops are 0 and 65535");
    check(coco_axis_from_deflection(-5000) == 0 && coco_axis_from_deflection(5000) == 65535, "beyond the stops clamps");
    check(coco_axis_from_deflection(250) == coco_axis_from_stick(0.25f, 0.0f)
          && coco_axis_from_deflection(-731) == coco_axis_from_stick(-0.731f, 0.0f),
          "it is coco_axis_from_stick without a dead zone");
    check(coco_axis_6bit(0) == 31 && coco_axis_6bit(-1000) == 0 && coco_axis_6bit(1000) == 63,
          "the 6-bit readout: 31 centred, 0 and 63 at the stops");
    check(coco_axis_6bit(-9999) == 0 && coco_axis_6bit(9999) == 63, "and it clamps");

    coco_deflect_from_stick(0.0f, 0.0f, &xaxis, &yaxis);
    check(xaxis == 0 && yaxis == 0, "a stick at rest is exactly centred");
    coco_deflect_from_stick(0.14f, -0.05f, &xaxis, &yaxis);
    check(xaxis == 0 && yaxis == 0, "and inside the dead zone");
    coco_deflect_from_stick(0.5f, 0.0f, &xaxis, &yaxis);
    check(xaxis >= 410 && xaxis <= 414 && yaxis == 0, "half deflection is 412 once the dead zone is taken out");
    coco_deflect_from_stick(1.0f, 0.0f, &xaxis, &yaxis);
    check(xaxis == 1000 && yaxis == 0, "full deflection reaches the stop");
    coco_deflect_from_stick(-1.0f, 1.0f, &xaxis, &yaxis);
    check(xaxis >= -1000 && xaxis < -600 && yaxis > 600 && yaxis <= 1000, "a corner of the pad's circle is rescaled, not past the stops");
    coco_deflect_from_stick(0.0f, -0.575f, &xaxis, &yaxis);
    check(xaxis == 0 && yaxis >= -503 && yaxis <= -497, "the vertical: up is -Y");
    coco_deflect_from_stick(0.7f, 0.7f, &xaxis, &yaxis);
    check(xaxis == yaxis && xaxis > 600 && xaxis < 800, "the diagonal is symmetric");
    coco_deflect_from_stick(1e9f, 0.0f, &xaxis, &yaxis);
    check(xaxis == 1000 && yaxis == 0, "an absurd reading is clamped");
}

/* ---- the merge of every source ---------------------------------------------- */

static int axis_x_of(cocosession *sess, int port)
{
    int xaxis = 0, yaxis = 0;

    cocosession_joystick_held(sess, port, &xaxis, &yaxis);
    return xaxis;
}

static int axis_y_of(cocosession *sess, int port)
{
    int xaxis = 0, yaxis = 0;

    cocosession_joystick_held(sess, port, &xaxis, &yaxis);
    return yaxis;
}

#define RIGHT_ACT(port) COCO_TARGET_PORT(port, COCO_ACT_RIGHT)
#define LEFT_ACT(port)  COCO_TARGET_PORT(port, COCO_ACT_LEFT)
#define DOWN_ACT(port)  COCO_TARGET_PORT(port, COCO_ACT_DOWN)
#define FIRE_ACT(port)  COCO_TARGET_PORT(port, COCO_ACT_BUTTON1)

static void merge_tests(cocosession *sess)
{
    int xaxis, yaxis;

    /* a start: the host's joystick state is initialised and centred */
    input_clear(sess);

    /* digital: a direction is a full deflection; the host sees it */
    cocosession_press(sess, RIGHT_ACT(0), 1);
    check(axis_x_of(sess, 0) == 1000 && cocohost_get_joystick_axis(0, 0) == 65535
          && cocohost_get_joystick_axis(0, 1) == 32767, "a held direction is a full deflection, on the host too");
    check(axis_x_of(sess, 1) == 0 && cocohost_get_joystick_axis(1, 0) == 32767, "and only on its own port");
    cocosession_press(sess, LEFT_ACT(0), 1);
    check(axis_x_of(sess, 0) == 0 && cocohost_get_joystick_axis(0, 0) == 32767, "opposed directions cancel");
    cocosession_press(sess, RIGHT_ACT(0), 0);
    check(axis_x_of(sess, 0) == -1000 && cocohost_get_joystick_axis(0, 0) == 0, "and the other is back when one lets go");
    cocosession_press(sess, LEFT_ACT(0), 0);
    check(axis_x_of(sess, 0) == 0, "centred");

    /* the family bug: letting go of one source must not release another's hold */
    cocosession_press(sess, RIGHT_ACT(0), 1);                 /* the keyboard / window */
    session_gamepad_press(sess, 0, RIGHT_ACT(0), 1);          /* a pad's button */
    session_gamepad_stick(sess, 1, 0, COCO_ACT_RIGHT, 1);     /* another pad's digital stick */
    cocosession_press(sess, RIGHT_ACT(0), 0);
    check(axis_x_of(sess, 0) == 1000, "the keyboard lets go of RIGHT; the pad still holds it");
    session_gamepad_press(sess, 0, RIGHT_ACT(0), 0);
    check(axis_x_of(sess, 0) == 1000, "the pad's button lets go; the other pad's stick still holds it");
    session_gamepad_stick(sess, 1, 0, COCO_ACT_RIGHT, 0);
    check(axis_x_of(sess, 0) == 0, "the last source lets go: centred");

    /* analog: proportional, the pad's stick or the pointer */
    session_gamepad_analog(sess, 0, 0, 400, -250);
    check(axis_x_of(sess, 0) == 400 && axis_y_of(sess, 0) == -250
          && cocohost_get_joystick_axis(0, 0) == coco_axis_from_deflection(400)
          && cocohost_get_joystick_axis(0, 1) == coco_axis_from_deflection(-250), "a pad's analog stick passes through");
    cocosession_pointer_set(sess, 0, -700, 0, 0);
    check(axis_x_of(sess, 0) == -700 && axis_y_of(sess, 0) == -250, "the stronger source wins per axis: the pointer on X, the pad on Y");
    session_gamepad_analog(sess, 3, 0, 900, 100);
    check(axis_x_of(sess, 0) == 900 && axis_y_of(sess, 0) == -250, "a third source stronger on X only");
    session_gamepad_analog(sess, 3, 0, 0, 0);
    cocosession_pointer_set(sess, 0, 0, 0, 0);
    check(axis_x_of(sess, 0) == 400 && axis_y_of(sess, 0) == -250, "the weaker ones are back when the stronger let go");
    cocosession_press(sess, DOWN_ACT(0), 1);
    check(axis_x_of(sess, 0) == 400 && axis_y_of(sess, 0) == 1000, "a held arrow on Y and the stick on X combine (per axis)");
    cocosession_press(sess, RIGHT_ACT(0), 1);
    check(axis_x_of(sess, 0) == 1000, "a digital direction beats a weaker analog deflection");
    cocosession_press(sess, DOWN_ACT(0), 0);
    cocosession_press(sess, RIGHT_ACT(0), 0);
    session_gamepad_analog(sess, 0, 0, 0, 0);
    check(axis_x_of(sess, 0) == 0 && axis_y_of(sess, 0) == 0, "all centred");
    cocosession_pointer_set(sess, 0, 5000, -5000, 0);
    cocosession_joystick_held(sess, 0, &xaxis, &yaxis);
    check(xaxis == 1000 && yaxis == -1000, "the pointer's deflection is clamped");
    cocosession_pointer_set(sess, 0, 0, 0, 0);
    cocosession_pointer_set(sess, 9, 500, 500, 3);
    cocosession_pointer_set(sess, -1, 500, 500, 3);
    session_gamepad_analog(sess, 99, 0, 500, 500);
    session_gamepad_analog(sess, 0, 9, 500, 500);
    check(axis_x_of(sess, 0) == 0 && axis_x_of(sess, 1) == 0 && cocosession_buttons_held(sess, 0) == 0,
          "a bad port or owner changes nothing");

    /* fire buttons simply OR */
    cocosession_press(sess, FIRE_ACT(1), 1);
    cocosession_pointer_set(sess, 1, 0, 0, 3);
    check(cocosession_buttons_held(sess, 1) == 3 && cocohost_get_joystick_button(1, 0) == 1
          && cocohost_get_joystick_button(1, 1) == 1 && cocosession_buttons_held(sess, 0) == 0,
          "the keyboard holds button 1, the pointer both");
    cocosession_pointer_set(sess, 1, 0, 0, 0);
    check(cocosession_buttons_held(sess, 1) == 1 && cocohost_get_joystick_button(1, 0) == 1
          && cocohost_get_joystick_button(1, 1) == 0, "the pointer lets go: button 1 is still held, button 2 is not");
    cocosession_press(sess, FIRE_ACT(1), 0);
    check(cocosession_buttons_held(sess, 1) == 0 && cocohost_get_joystick_button(1, 0) == 0, "released");

    /* release_all: the keyboard's holds, not the pad's or the pointer's */
    cocosession_press(sess, RIGHT_ACT(0), 1);
    cocosession_press(sess, FIRE_ACT(0), 1);
    session_gamepad_press(sess, 2, DOWN_ACT(0), 1);
    cocosession_pointer_set(sess, 0, 0, -300, 2);
    cocosession_release_all(sess);
    check(axis_x_of(sess, 0) == 0 && axis_y_of(sess, 0) == 1000 && cocosession_buttons_held(sess, 0) == 2,
          "focus loss lets go of the keyboard's holds only");
    session_gamepad_press(sess, 2, DOWN_ACT(0), 0);
    cocosession_pointer_set(sess, 0, 0, 0, 0);

    /* RESET acts once per press, on the edge, whoever holds it */
    sess->running = 1;
    while (cocohost_take_reset_request()) {}
    cocosession_press(sess, COCO_TARGET_SWITCH(COCO_SW_RESET), 1);
    check(cocohost_take_reset_request() == 1 && cocosession_switch_held(sess, COCO_SW_RESET) == 1,
          "RESET resets on the press");
    session_gamepad_press(sess, 0, COCO_TARGET_SWITCH(COCO_SW_RESET), 1);
    check(cocohost_take_reset_request() == 0, "a second source pressing it while held does not reset again");
    cocosession_press(sess, COCO_TARGET_SWITCH(COCO_SW_RESET), 0);
    check(cocosession_switch_held(sess, COCO_SW_RESET) == 1 && cocohost_take_reset_request() == 0,
          "the first lets go: still held by the pad");
    session_gamepad_press(sess, 0, COCO_TARGET_SWITCH(COCO_SW_RESET), 0);
    check(cocosession_switch_held(sess, COCO_SW_RESET) == 0 && cocohost_take_reset_request() == 0,
          "released: no reset");
    cocosession_press(sess, COCO_TARGET_SWITCH(COCO_SW_RESET), 1);
    cocosession_press(sess, COCO_TARGET_SWITCH(COCO_SW_RESET), 0);
    check(cocohost_take_reset_request() == 1, "a new press resets again");
    sess->running = 0;
    cocosession_press(sess, COCO_TARGET_SWITCH(COCO_SW_RESET), 1);
    cocosession_press(sess, COCO_TARGET_SWITCH(COCO_SW_RESET), 0);
    check(cocohost_take_reset_request() == 0, "with no machine running it resets nothing");

    /* the debugger-stop action is not a held input */
    cocosession_press(sess, COCO_TARGET_SYSACT(COCO_SYSACT_DEBUG_STOP), 1);
    cocosession_press(sess, COCO_TARGET_SYSACT(COCO_SYSACT_DEBUG_STOP), 0);
    cocosession_press(sess, 99, 1);
    cocosession_press(sess, -1, 1);
    session_gamepad_press(sess, 0, COCO_TARGET_SYSACT(0), 1);
    check(axis_x_of(sess, 0) == 0 && cocosession_switch_held(sess, 0) == 0, "actions and bad targets hold nothing");

    /* a start centres everything */
    cocosession_press(sess, RIGHT_ACT(1), 1);
    session_gamepad_analog(sess, 0, 0, 800, 800);
    cocosession_pointer_set(sess, 1, -400, 0, 1);
    input_clear(sess);
    check(axis_x_of(sess, 0) == 0 && axis_y_of(sess, 0) == 0 && axis_x_of(sess, 1) == 0
          && cocosession_buttons_held(sess, 1) == 0 && cocohost_get_joystick_axis(0, 0) == 32767
          && cocohost_get_joystick_axis(0, 1) == 32767 && cocohost_get_joystick_axis(1, 0) == 32767
          && cocohost_get_joystick_button(1, 0) == 0, "input_clear centres all four axes and lets go of every source");
}

/* ---- the key queue's per-frame drain ------------------------------------------ */

static int sunk_down[16], sunk_code[16], sunk_count;

static void record_sink(int down, int scancode)
{
    if (sunk_count < 16) {
        sunk_down[sunk_count] = down;
        sunk_code[sunk_count] = scancode;
        sunk_count++;
    }
}

static void drain_tests(void)
{
    int delivered;

    while (cocohost_drain_keys(record_sink)) {}
    sunk_count = 0;

    /* a press and its release queued together: the machine must see the key
     * held for a frame */
    cocohost_inject_key(1, 0x04);
    cocohost_inject_key(0, 0x04);
    delivered = cocohost_drain_keys(record_sink);
    check(delivered == 1 && sunk_count == 1 && sunk_down[0] == 1 && sunk_code[0] == 0x04,
          "a press and its release queued together: the first frame gets the press only");
    sunk_count = 0;
    delivered = cocohost_drain_keys(record_sink);
    check(delivered == 1 && sunk_count == 1 && sunk_down[0] == 0 && sunk_code[0] == 0x04,
          "and the next frame the release");
    check(cocohost_drain_keys(record_sink) == 0, "then the queue is empty");

    /* other keys are not held up by it, up to the held-back release */
    sunk_count = 0;
    cocohost_inject_key(1, 0x04);
    cocohost_inject_key(1, 0x05);
    cocohost_inject_key(0, 0x04);
    cocohost_inject_key(0, 0x05);
    delivered = cocohost_drain_keys(record_sink);
    check(delivered == 2 && sunk_code[0] == 0x04 && sunk_code[1] == 0x05 && sunk_down[0] && sunk_down[1],
          "two presses go in one frame; the release behind them waits");
    sunk_count = 0;
    delivered = cocohost_drain_keys(record_sink);
    check(delivered == 2 && !sunk_down[0] && !sunk_down[1] && sunk_code[0] == 0x04 && sunk_code[1] == 0x05,
          "and both go in the next, in order");

    /* a key pressed in an earlier frame is released at once */
    sunk_count = 0;
    cocohost_inject_key(1, 0x06);
    cocohost_drain_keys(record_sink);
    sunk_count = 0;
    cocohost_inject_key(0, 0x06);
    cocohost_inject_key(1, 0x07);
    delivered = cocohost_drain_keys(record_sink);
    check(delivered == 2 && !sunk_down[0] && sunk_code[0] == 0x06 && sunk_down[1] && sunk_code[1] == 0x07,
          "a key that was held from an earlier frame is released without waiting");
    cocohost_release_all_keys();
    while (cocohost_drain_keys(record_sink)) {}
    sunk_count = 0;
}

int main(void)
{
    deflection_tests();
    {
        char tmpl[512], config[600], data[600];
        cocosession_paths paths;
        cocosession *sess;
        char *base;

        test_tmpl(tmpl, sizeof tmpl, "input_test");
        base = mkdtemp(tmpl);
        if (!base) { perror("mkdtemp"); return 1; }
        snprintf(config, sizeof config, "%s/config", base);
        snprintf(data, sizeof data, "%s/data", base);
        memset(&paths, 0, sizeof paths);
        paths.config_dir = config;
        paths.data_dir = data;
        paths.fujinet_lib = "";
        sess = cocosession_new(&paths);
        if (!sess) { fprintf(stderr, "input_test: no session\n"); return 1; }
        merge_tests(sess);
        cocosession_free(sess);
    }
    drain_tests();

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
