/*
 * controls_test -- the on-screen joysticks and keyboard, with no machine:
 * both joystick layouts (a CoCo 3 has two fire buttons, a CoCo 2 one), that
 * every target of the controls is on exactly one control, that the exact-shape
 * hit test agrees with the shapes and the sectors change at 45 degrees, the
 * stick's gesture maths (a square range, +y down); and the keyboard: 53 caps
 * on a CoCo 2 and 57 on a CoCo 3, no two overlapping, every cap inject the key
 * the host keyboard would for the same key, CTRL being Caps Lock and never the
 * emulator-command Control, and the caps' press, hold, latch and release.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>

#include "coco_host.h"
#include "session_internal.h"
#include "test_input.h"

static const int k_machines[2] = { COCOSESSION_MACHINE_COCO3, COCOSESSION_MACHINE_COCO2 };

static int near_int(int got, int want, int tol)
{
    return got >= want - tol && got <= want + tol;
}

/* The control of kind `kind` on `port` (-1: any) whose target is `target`. */
static int find_control(const coco_control *list, int count, int kind, int port, int target)
{
    int idx;

    for (idx = 0; idx < count; idx++)
        if (list[idx].kind == kind && (port < 0 || list[idx].port == port)
            && (target < 0 || list[idx].target == target))
            return idx;
    return -1;
}

static int is_inside(const coco_control *ctl, float posx, float posy)
{
    return posx >= ctl->left && posx < ctl->left + ctl->wide
        && posy >= ctl->top && posy < ctl->top + ctl->high;
}

/* A control's exact shape, as the hit test promises it, by the control alone. */
static int in_shape(const coco_control *list, int idx, float posx, float posy)
{
    const coco_control *ctl = &list[idx];
    const float cenx = ctl->left + ctl->wide / 2, ceny = ctl->top + ctl->high / 2;

    if (ctl->kind == COCO_CTL_BUTTON || ctl->kind == COCO_CTL_STICK)
        return hypotf(posx - cenx, posy - ceny) <= ctl->wide / 2;
    return is_inside(ctl, posx, posy);
}

static void joystick_layout(int machine)
{
    int count = 0, idx, port, tgt, hits;
    float wide = 0, high = 0;
    const coco_control *list = cocosession_controls_layout(machine, &count, &wide, &high);
    const int coco2 = machine == COCOSESSION_MACHINE_COCO2;
    char what[96];

    snprintf(what, sizeof what, "%s: %d controls", coco2 ? "CoCo 2" : "CoCo 3", coco2 ? 19 : 21);
    check(list && count == (coco2 ? 19 : 21), what);
    check(near_int((int)(wide * 10), 120, 0) && near_int((int)(high * 10), 90, 0), "12.0 x 9.0 units");

    /* every control inside the extent */
    hits = 0;
    for (idx = 0; idx < count; idx++)
        if (list[idx].left < 0 || list[idx].top < 0 || list[idx].left + list[idx].wide > wide + 0.001f
            || list[idx].top + list[idx].high > high + 0.001f)
            hits++;
    check(hits == 0, "every control is inside the extent");

    /* every control's target appears exactly once */
    for (tgt = 0; tgt < COCO_TARGET_SYSACT(0); tgt++) {
        const int base_act = tgt % COCO_ACT_PER_PORT;
        const int expect = (tgt >= COCO_TARGET_SWITCH(0) || base_act != COCO_ACT_BUTTON2 || !coco2) ? 1 : 0;

        hits = 0;
        for (idx = 0; idx < count; idx++)
            if (list[idx].target == tgt) hits++;
        if (hits != expect) {
            snprintf(what, sizeof what, "target %d (%s) is on %d controls, expected %d", tgt, coco_target_name(tgt),
                     hits, expect);
            check(0, what);
        }
    }
    check(find_control(list, count, COCO_CTL_BUTTON, -1, COCO_TARGET_PORT(0, COCO_ACT_BUTTON2)) >= 0 ? !coco2 : coco2,
          "button 2 is there on a CoCo 3 and absent on a CoCo 2");
    check(find_control(list, count, COCO_CTL_RESET, -1, COCO_TARGET_SWITCH(COCO_SW_RESET)) >= 0, "RESET is there");
    check(find_control(list, count, COCO_CTL_BUTTON, -1, COCO_TARGET_SYSACT(0)) < 0
          && find_control(list, count, COCO_CTL_DIRECTION, -1, COCO_TARGET_SYSACT(0)) < 0,
          "and the debugger stop is not on the picture");

    /* the right joystick is at the right */
    idx = find_control(list, count, COCO_CTL_STICK, 0, -1);
    port = find_control(list, count, COCO_CTL_STICK, 1, -1);
    check(idx >= 0 && port >= 0 && list[idx].left > list[port].left, "port 0 (right) is drawn right of port 1 (left)");
    idx = find_control(list, count, COCO_CTL_CAPTION, 0, -1);
    check(idx >= 0 && !strcmp(list[idx].label, "Right joystick"), "and captioned");
    idx = find_control(list, count, COCO_CTL_CAPTION, 1, -1);
    check(idx >= 0 && !strcmp(list[idx].label, "Left joystick"), "the left one too");

    /* the hit test against the shapes: on a grid, at most one control claims a point (the plate
     * and the gate under the sectors excepted), and the hit test names it */
    {
        int bad = 0, covered = 0;
        float posx, posy;

        for (posy = 0.05f; posy < high; posy += 0.1f)
            for (posx = 0.05f; posx < wide; posx += 0.1f) {
                int claim = -1, claims = 0, got, want;

                for (idx = 0; idx < count; idx++) {
                    const int kind = list[idx].kind;

                    if (kind == COCO_CTL_BASE || kind == COCO_CTL_STICK || kind == COCO_CTL_DIRECTION)
                        continue;
                    if (in_shape(list, idx, posx, posy)) { claim = idx; claims++; }
                }
                for (idx = 0; idx < count; idx++) {
                    const coco_control *ctl = &list[idx];

                    if (ctl->kind != COCO_CTL_DIRECTION) continue;
                    {
                        int stick = find_control(list, count, COCO_CTL_STICK, ctl->port, -1);
                        const float cenx = list[stick].left + list[stick].wide / 2;
                        const float ceny = list[stick].top + list[stick].high / 2;
                        const float dist = hypotf(posx - cenx, posy - ceny), rad = list[stick].wide / 2;
                        float deg = atan2f(-(posy - ceny), posx - cenx) * 180.0f / 3.14159265f;
                        int sector;

                        if (deg < 0) deg += 360.0f;
                        sector = (int)floorf((deg + 45.0f) / 90.0f) & 3;
                        if (dist >= rad / 2 && dist <= rad && ctl->target % COCO_ACT_PER_PORT ==
                            (sector == 0 ? COCO_ACT_RIGHT : sector == 1 ? COCO_ACT_UP
                             : sector == 2 ? COCO_ACT_LEFT : COCO_ACT_DOWN)) {
                            claim = idx;
                            claims++;
                        }
                    }
                }
                if (claims > 1) bad++;
                got = cocosession_controls_hit(machine, posx, posy);
                if (claims == 1) {
                    want = claim;
                } else {
                    int stick_idx = -1, base_idx = -1;

                    for (idx = 0; idx < count; idx++) {
                        if (list[idx].kind == COCO_CTL_STICK && in_shape(list, idx, posx, posy)) stick_idx = idx;
                        if (list[idx].kind == COCO_CTL_BASE && is_inside(&list[idx], posx, posy)) base_idx = idx;
                    }
                    want = stick_idx >= 0 ? stick_idx : base_idx;
                }
                if (want >= 0) covered++;
                /* points within a hair of an edge may round either way: only
                 * the interior is held to it */
                if (got != want) {
                    int near_edge = 0;

                    for (idx = 0; idx < count; idx++) {
                        const coco_control *ctl = &list[idx];
                        const float cenx = ctl->left + ctl->wide / 2, ceny = ctl->top + ctl->high / 2;

                        if (fabsf(posx - ctl->left) < 0.06f || fabsf(posx - ctl->left - ctl->wide) < 0.06f
                            || fabsf(posy - ctl->top) < 0.06f || fabsf(posy - ctl->top - ctl->high) < 0.06f)
                            near_edge = 1;
                        if ((ctl->kind == COCO_CTL_STICK || ctl->kind == COCO_CTL_BUTTON || ctl->kind == COCO_CTL_DIRECTION)
                            && (fabsf(hypotf(posx - cenx, posy - ceny) - ctl->wide / 2) < 0.06f
                                || fabsf(hypotf(posx - cenx, posy - ceny) - ctl->wide / 4) < 0.06f))
                            near_edge = 1;
                        /* a sector's border is a ray from the gate's centre, which no
                         * bounding box shows: skip points near a diagonal inside a gate */
                        if (ctl->kind == COCO_CTL_STICK) {
                            const float delx = fabsf(posx - cenx), dely = fabsf(posy - ceny);

                            if (hypotf(delx, dely) < ctl->wide / 2 + 0.1f && fabsf(delx - dely) < 0.08f)
                                near_edge = 1;
                        }
                    }
                    if (!near_edge) bad++;
                }
            }
        check(bad == 0, "no two controls overlap, and the hit test returns what the shapes say");
        check(covered > 2000, "and the controls cover a good part of the extent");
    }
    check(cocosession_controls_hit(machine, -1.0f, -1.0f) == -1 && cocosession_controls_hit(machine, wide + 1, 0) == -1,
          "a point outside every control hits nothing");

    /* each control's centre hits itself (the plate, the gate under its sectors and the caption
     * and readout rectangles all included) */
    hits = 0;
    for (idx = 0; idx < count; idx++) {
        const coco_control *ctl = &list[idx];
        float posx = ctl->left + ctl->wide / 2, posy = ctl->top + ctl->high / 2;
        int got;

        if (ctl->kind == COCO_CTL_BASE) { posx = ctl->left + 0.2f; posy = ctl->top + ctl->high - 0.2f; }
        if (ctl->kind == COCO_CTL_DIRECTION) {
            /* the middle of the sector's ring, along its own axis */
            const int stick = find_control(list, count, COCO_CTL_STICK, ctl->port, -1);
            const float cenx = list[stick].left + list[stick].wide / 2, ceny = list[stick].top + list[stick].high / 2;
            const float rad = list[stick].wide / 2 * 0.75f;
            const int act = ctl->target % COCO_ACT_PER_PORT;

            posx = cenx + (act == COCO_ACT_RIGHT ? rad : act == COCO_ACT_LEFT ? -rad : 0);
            posy = ceny + (act == COCO_ACT_DOWN ? rad : act == COCO_ACT_UP ? -rad : 0);
        }
        got = cocosession_controls_hit(machine, posx, posy);
        if (got != idx) hits++;
    }
    check(hits == 0, "every control is found at its middle");

    /* the sectors change at 45 degrees */
    for (port = 0; port < 2; port++) {
        const int stick = find_control(list, count, COCO_CTL_STICK, port, -1);
        const float cenx = list[stick].left + list[stick].wide / 2, ceny = list[stick].top + list[stick].high / 2;
        const float rad = list[stick].wide / 2;
        static const struct { float deg; int act; } edges[] = {
            { 44, COCO_ACT_RIGHT }, { 46, COCO_ACT_UP }, { 134, COCO_ACT_UP }, { 136, COCO_ACT_LEFT },
            { 224, COCO_ACT_LEFT }, { 226, COCO_ACT_DOWN }, { 314, COCO_ACT_DOWN }, { 316, COCO_ACT_RIGHT },
            { 0, COCO_ACT_RIGHT }, { 90, COCO_ACT_UP }, { 180, COCO_ACT_LEFT }, { 270, COCO_ACT_DOWN },
        };
        int wrong = 0, which;

        for (which = 0; which < (int)(sizeof edges / sizeof edges[0]); which++) {
            const float ang = edges[which].deg * 3.14159265f / 180.0f;
            const int got = cocosession_controls_hit(machine, cenx + rad * 0.75f * cosf(ang),
                                                     ceny - rad * 0.75f * sinf(ang));

            if (got < 0 || list[got].kind != COCO_CTL_DIRECTION
                || list[got].target != COCO_TARGET_PORT(port, edges[which].act)) wrong++;
        }
        check(wrong == 0, "the ring's four sectors change at 45 degrees");
        check(list[cocosession_controls_hit(machine, cenx + rad * 0.3f, ceny)].kind == COCO_CTL_STICK
              && list[cocosession_controls_hit(machine, cenx, ceny)].kind == COCO_CTL_STICK,
              "inside the ring is the gate");
        check(list[cocosession_controls_hit(machine, cenx + rad * 1.1f, ceny)].kind != COCO_CTL_STICK
              && list[cocosession_controls_hit(machine, cenx + rad * 1.1f, ceny)].kind != COCO_CTL_DIRECTION,
              "outside the gate is the plate");
    }
}

static void stick_maths(void)
{
    int xaxis, yaxis;

    cocosession_stick_from_vector(0, 0, 1.25f, &xaxis, &yaxis);
    check(xaxis == 0 && yaxis == 0, "the stick at its centre");
    cocosession_stick_from_vector(0.0375f, -0.0375f, 1.25f, &xaxis, &yaxis);
    check(xaxis == 0 && yaxis == 0, "3% of the travel snaps to the centre");
    cocosession_stick_from_vector(0.0625f, -0.0625f, 1.25f, &xaxis, &yaxis);
    check(xaxis == 50 && yaxis == -50, "5% does not");
    cocosession_stick_from_vector(0.625f, 0, 1.25f, &xaxis, &yaxis);
    check(xaxis == 500 && yaxis == 0, "half the travel right is 500");
    cocosession_stick_from_vector(0, -1.25f, 1.25f, &xaxis, &yaxis);
    check(xaxis == 0 && yaxis == -1000, "up is -1000 (y is down)");
    cocosession_stick_from_vector(0, 5.0f, 1.25f, &xaxis, &yaxis);
    check(xaxis == 0 && yaxis == 1000, "past the travel clamps");
    cocosession_stick_from_vector(2.0f, 2.0f, 1.25f, &xaxis, &yaxis);
    check(xaxis == 1000 && yaxis == 1000, "a corner is a full diagonal: the range is a square");
    cocosession_stick_from_vector(-1.25f, 1.25f, 1.25f, &xaxis, &yaxis);
    check(xaxis == -1000 && yaxis == 1000, "down-left");
    cocosession_stick_from_vector(0.1f, 0.1f, 0.0f, &xaxis, &yaxis);
    check(xaxis == 0 && yaxis == 0, "no travel, no deflection");
    cocosession_stick_from_vector(NAN, 0.5f, 1.25f, &xaxis, &yaxis);
    check(xaxis == 0 && yaxis >= 0, "NaN is the centre");
}

/* ---- the keyboard ---------------------------------------------------------- */

static int find_cap(const coco_keycap *caps, int count, const char *label)
{
    int idx;

    for (idx = 0; idx < count; idx++)
        if (!strcmp(caps[idx].label, label)) return idx;
    return -1;
}

static void keyboard_layout(int machine)
{
    int count = 0, idx, jdx, bad;
    float wide = 0, high = 0;
    const coco_keycap *caps = cocosession_keyboard_layout(machine, &count, &wide, &high);
    const int coco2 = machine == COCOSESSION_MACHINE_COCO2;
    char what[96];
    unsigned scan;

    snprintf(what, sizeof what, "%s: %d keys", coco2 ? "CoCo 2" : "CoCo 3", coco2 ? 53 : 57);
    check(caps && count == (coco2 ? 53 : 57), what);
    check(near_int((int)(wide * 10), 175, 0) && near_int((int)(high * 10), 54, 0), "17.5 x 5.4 units");

    /* inside the extent, no two overlapping */
    bad = 0;
    for (idx = 0; idx < count; idx++) {
        if (caps[idx].left < 0 || caps[idx].top < 0 || caps[idx].left + caps[idx].wide > wide + 0.001f
            || caps[idx].top + caps[idx].high > high + 0.001f)
            bad++;
        for (jdx = idx + 1; jdx < count; jdx++)
            if (caps[idx].left < caps[jdx].left + caps[jdx].wide - 0.001f
                && caps[jdx].left < caps[idx].left + caps[idx].wide - 0.001f
                && caps[idx].top < caps[jdx].top + caps[jdx].high - 0.001f
                && caps[jdx].top < caps[idx].top + caps[idx].high - 0.001f) {
                printf("  %s overlaps %s\n", caps[idx].label, caps[jdx].label);
                bad++;
            }
    }
    check(bad == 0, "every key is inside the extent and none overlap");

    /* every usage once, and none of the host's Control keys (XRoar's own command modifier) */
    bad = 0;
    for (idx = 0; idx < count; idx++) {
        for (jdx = idx + 1; jdx < count; jdx++)
            if (caps[idx].hid == caps[jdx].hid) bad++;
        if (caps[idx].hid == 0xe0 || caps[idx].hid == 0xe4 || caps[idx].hid > 0xff) bad++;
    }
    check(bad == 0, "no two keys inject the same usage, and none is a host Control");

    /* letters and digits once each, and each is the key the host keyboard sends for it */
    bad = 0;
    for (idx = 'A'; idx <= 'Z'; idx++) {
        char label[2] = { (char)idx, 0 };
        const int cap = find_cap(caps, count, label);

        if (cap < 0 || !coco_key_from_event((uint32_t)idx + 32, 0, 0, 0, &scan) || caps[cap].hid != scan
            || caps[cap].kind != COCO_KEYCAP_NORMAL) bad++;
    }
    for (idx = '0'; idx <= '9'; idx++) {
        char label[2] = { (char)idx, 0 };
        const int cap = find_cap(caps, count, label);

        if (cap < 0 || !coco_key_from_event((uint32_t)idx, 0, 0, 0, &scan) || caps[cap].hid != scan) bad++;
    }
    check(bad == 0, "A-Z and 0-9 are there once, as the host key of the same name");

    {
        static const struct { const char *label; uint32_t keysym; int kind; } named[] = {
            { "SPACE", ' ', COCO_KEYCAP_SPACE }, { "ENTER", 0xFF0D, COCO_KEYCAP_SPECIAL },
            { "BREAK", 0xFF1B, COCO_KEYCAP_SPECIAL }, { "CLEAR", 0xFF50, COCO_KEYCAP_SPECIAL },
            { ";", ';', COCO_KEYCAP_NORMAL }, { ",", ',', COCO_KEYCAP_NORMAL },
            { ".", '.', COCO_KEYCAP_NORMAL }, { "/", '/', COCO_KEYCAP_NORMAL },
            { "\xE2\x86\x91", 0xFF52, COCO_KEYCAP_ARROW }, { "\xE2\x86\x93", 0xFF54, COCO_KEYCAP_ARROW },
            { "\xE2\x86\x90", 0xFF51, COCO_KEYCAP_ARROW }, { "\xE2\x86\x92", 0xFF53, COCO_KEYCAP_ARROW },
        };

        bad = 0;
        for (idx = 0; idx < (int)(sizeof named / sizeof named[0]); idx++) {
            const int cap = find_cap(caps, count, named[idx].label);

            if (cap < 0 || !coco_key_from_event(named[idx].keysym, 0, 0, 0, &scan) || caps[cap].hid != scan
                || caps[cap].kind != named[idx].kind) {
                printf("  the %s key does not match\n", named[idx].label);
                bad++;
            }
        }
        check(bad == 0, "SPACE, ENTER, BREAK, CLEAR, ; , . / and the arrows are the host key's usage");
    }

    /* the CoCo's own symbols sit on other host keys: XRoar's table is positional, so the cap printed
     * ":" is the host's "-" key, "-" the host's "=", and "@" the host's "[" */
    {
        static const struct { const char *label; uint32_t host; } symbols[] = {
            { ":", '-' }, { "-", '=' }, { "@", '[' },
        };

        bad = 0;
        for (idx = 0; idx < 3; idx++) {
            const int cap = find_cap(caps, count, symbols[idx].label);

            if (cap < 0 || !coco_key_from_event(symbols[idx].host, 0, 0, 0, &scan) || caps[cap].hid != scan) bad++;
        }
        check(bad == 0 && caps[find_cap(caps, count, ":")].hid == 0x2d && caps[find_cap(caps, count, "-")].hid == 0x2e
              && caps[find_cap(caps, count, "@")].hid == 0x2f,
              ": - @ inject the host keys XRoar maps to them (- = [), not the keys of the same symbol");
    }

    /* the shifted legends */
    check(!strcmp(caps[find_cap(caps, count, "1")].shifted, "!") && !strcmp(caps[find_cap(caps, count, ":")].shifted, "*")
          && !strcmp(caps[find_cap(caps, count, ";")].shifted, "+") && caps[find_cap(caps, count, "A")].shifted == NULL,
          "the shifted legends");

    /* the modifiers */
    idx = find_cap(caps, count, "SHIFT");
    check(idx >= 0 && caps[idx].kind == COCO_KEYCAP_MODIFIER && caps[idx].hid == 0xe1, "SHIFT is a modifier cap");
    {
        int shifts = 0;

        for (idx = 0; idx < count; idx++)
            if (!strcmp(caps[idx].label, "SHIFT")) shifts++;
        check(shifts == 2, "there are two");
    }
    idx = find_cap(caps, count, "CTRL");
    jdx = find_cap(caps, count, "ALT");
    if (coco2) {
        check(idx < 0 && jdx < 0 && find_cap(caps, count, "F1") < 0 && find_cap(caps, count, "F2") < 0,
              "a CoCo 2 has no CTRL, ALT, F1 or F2");
    } else {
        check(idx >= 0 && caps[idx].kind == COCO_KEYCAP_MODIFIER && caps[idx].hid == 0x39,
              "CTRL is Caps Lock (0x39), what XRoar's CoCo 3 keymap turns into CTRL");
        check(caps[idx].hid != 0xe0 && caps[idx].hid != 0xe4, "and never a host Control, XRoar's command modifier");
        check(jdx >= 0 && caps[jdx].kind == COCO_KEYCAP_MODIFIER && caps[jdx].hid == 0xe2, "ALT is Alt_L (0xe2)");
        check(find_cap(caps, count, "F1") >= 0 && caps[find_cap(caps, count, "F1")].hid == 0x3a
              && find_cap(caps, count, "F2") >= 0 && caps[find_cap(caps, count, "F2")].hid == 0x3b
              && caps[find_cap(caps, count, "F1")].kind == COCO_KEYCAP_FUNCTION, "F1 and F2");
        check(coco_key_from_event(0xFFE3, 0, 1, 0, &scan) && scan == 0xe0,
              "(the host Control still forwards as 0xe0, which the CTRL cap must not)");
    }
}

/* ---- the caps against a session --------------------------------------------- */

static void keyboard_state(cocosession *sess, int machine)
{
    int count = 0, shift_l, aaa, bbb, ctrl;
    const coco_keycap *caps;

    cocosession_set_machine(sess, machine);
    caps = cocosession_keyboard_layout(machine, &count, NULL, NULL);
    shift_l = find_cap(caps, count, "SHIFT");
    aaa = find_cap(caps, count, "A");
    bbb = find_cap(caps, count, "B");
    ctrl = find_cap(caps, count, "CTRL");

    check(cocosession_keyboard_held(sess, aaa) == 0 && cocosession_keyboard_held(sess, -1) == 0
          && cocosession_keyboard_held(sess, count) == 0, "nothing held at the start; a bad index is not held");

    cocosession_keyboard_press(sess, aaa, 1);
    check(cocosession_keyboard_held(sess, aaa) == 1, "pressing a cap lights it");
    cocosession_keyboard_press(sess, aaa, 0);
    check(cocosession_keyboard_held(sess, aaa) == 0, "and releasing clears it");

    /* a modifier latches on the press and ignores the release */
    cocosession_keyboard_press(sess, shift_l, 1);
    cocosession_keyboard_press(sess, shift_l, 0);
    check(cocosession_keyboard_held(sess, shift_l) == 3, "SHIFT is held and latched after a click");
    cocosession_keyboard_press(sess, aaa, 1);
    check(cocosession_keyboard_held(sess, shift_l) == 3 && cocosession_keyboard_held(sess, aaa) == 1, "A with the SHIFT still down");
    cocosession_keyboard_press(sess, aaa, 0);
    check(cocosession_keyboard_held(sess, shift_l) == 0 && cocosession_keyboard_held(sess, aaa) == 0,
          "an ordinary key's release lets the latched SHIFT go");

    cocosession_keyboard_press(sess, shift_l, 1);
    cocosession_keyboard_press(sess, shift_l, 0);
    cocosession_keyboard_press(sess, shift_l, 1);
    cocosession_keyboard_press(sess, shift_l, 0);
    check(cocosession_keyboard_held(sess, shift_l) == 0, "clicking a latched SHIFT again unlatches it");

    /* release_all (the host keyboard's focus loss) leaves the window's keyboard alone */
    cocosession_keyboard_press(sess, shift_l, 1);
    cocosession_keyboard_press(sess, bbb, 1);
    cocosession_release_all(sess);
    check(cocosession_keyboard_held(sess, shift_l) == 3 && cocosession_keyboard_held(sess, bbb) == 1,
          "the host keyboard's release_all leaves caps and latches alone");
    cocosession_keyboard_release(sess);
    check(cocosession_keyboard_held(sess, shift_l) == 0 && cocosession_keyboard_held(sess, bbb) == 0,
          "keyboard_release lets go of every cap and latch");

    if (ctrl >= 0) {
        cocosession_keyboard_press(sess, ctrl, 1);
        check(cocosession_keyboard_held(sess, ctrl) == 3, "CTRL latches too");
        cocosession_keyboard_release(sess);
    }

    /* the host keyboard lights the cap too (the window doubles as a keyboard tester) */
    sess->running = 1;
    cocosession_key(sess, 1, 'a', 'a', 0, 0);
    check(cocosession_keyboard_held(sess, aaa) == 1, "a host key held lights its cap");
    cocosession_keyboard_press(sess, aaa, 1);
    cocosession_keyboard_press(sess, aaa, 0);
    check(cocosession_keyboard_held(sess, aaa) == 1, "and the cap letting go does not release the host's hold");
    cocosession_key(sess, 0, 'a', 'a', 0, 0);
    check(cocosession_keyboard_held(sess, aaa) == 0, "released by the host key");
    sess->running = 0;
    cocohost_release_all_keys();
}

int main(void)
{
    char config[512], data[512];
    cocosession *sess;
    int which;

    for (which = 0; which < 2; which++) {
        printf("-- joysticks, %s\n", which ? "CoCo 2" : "CoCo 3");
        joystick_layout(k_machines[which]);
        printf("-- keyboard, %s\n", which ? "CoCo 2" : "CoCo 3");
        keyboard_layout(k_machines[which]);
    }
    stick_maths();

    if (test_dirs("controls_test", config, sizeof config, data, sizeof data) != 0)
        return 1;
    sess = test_session(config, data);
    if (!sess) return 1;
    for (which = 0; which < 2; which++) {
        printf("-- caps in a session, %s\n", which ? "CoCo 2" : "CoCo 3");
        keyboard_state(sess, k_machines[which]);
    }
    cocosession_free(sess);

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
