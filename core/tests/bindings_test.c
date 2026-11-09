/*
 * bindings_test -- the remappable table and the key router: the target space
 * (two joysticks' four directions and two buttons, RESET, the debugger stop:
 * 14), that no key is bound by default and the pad defaults are the same on
 * both joysticks, steal-and-describe, persistence through the settings store
 * (and a garbled setting), names, the canonical key identity, and that a bound
 * key is consumed and drives the merged joystick. No machine is started.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "session_internal.h"
#include "test_input.h"

#define RIGHT_OF(port) COCO_TARGET_PORT(port, COCO_ACT_RIGHT)
#define UP_OF(port) COCO_TARGET_PORT(port, COCO_ACT_UP)

static int axis_x(cocosession *sess, int port)
{
    int xaxis = 0, yaxis = 0;

    cocosession_joystick_held(sess, port, &xaxis, &yaxis);
    return xaxis;
}

static int axis_y(cocosession *sess, int port)
{
    int xaxis = 0, yaxis = 0;

    cocosession_joystick_held(sess, port, &xaxis, &yaxis);
    return yaxis;
}

int main(void)
{
    char config[512], data[512], stolen[160];
    cocosession *sess;
    coco_binding bind;
    int tgt, port, bad, clash;
    uint32_t keysym;

    if (test_dirs("bindings_test", config, sizeof config, data, sizeof data) != 0)
        return 1;
    sess = test_session(config, data);
    if (!sess) return 1;

    /* the table's shape */
    check(COCO_TARGET_COUNT == 14, "14 targets: 2 x (4 directions + 2 buttons), RESET, the debugger stop");
    check(COCO_TARGET_SWITCH(COCO_SW_RESET) == 12 && COCO_TARGET_SYSACT(0) == 13, "the flat target indices");
    check(COCO_ACT_RIGHT == 0 && COCO_ACT_LEFT == 1 && COCO_ACT_UP == 2 && COCO_ACT_DOWN == 3
          && COCO_ACT_BUTTON1 == 4 && COCO_ACT_BUTTON2 == 5, "the actions' order");
    check(COCOSESSION_JOYSTICK_PORTS == 2, "two ports: 0 right, 1 left");

    /* no key is bound by default: every key belongs to the CoCo */
    bad = 0;
    for (tgt = 0; tgt < COCO_TARGET_COUNT; tgt++)
        if (cocosession_binding_get(sess, tgt).keysym != 0) bad++;
    check(bad == 0, "no target has a default key");
    check(cocosession_target_for_key(sess, 'a') == -1 && cocosession_target_for_key(sess, 0xFF51) == -1,
          "so no key is a target");

    /* pad defaults: the same on both joysticks */
    for (port = 0; port < 2; port++) {
        check(cocosession_binding_get(sess, COCO_TARGET_PORT(port, COCO_ACT_UP)).button == COCO_PAD_BTN_DPAD_UP
              && cocosession_binding_get(sess, COCO_TARGET_PORT(port, COCO_ACT_DOWN)).button == COCO_PAD_BTN_DPAD_DOWN
              && cocosession_binding_get(sess, COCO_TARGET_PORT(port, COCO_ACT_LEFT)).button == COCO_PAD_BTN_DPAD_LEFT
              && cocosession_binding_get(sess, COCO_TARGET_PORT(port, COCO_ACT_RIGHT)).button == COCO_PAD_BTN_DPAD_RIGHT,
              "the D-pad steers");
        check(cocosession_binding_get(sess, COCO_TARGET_PORT(port, COCO_ACT_BUTTON1)).button == COCO_PAD_BTN_SOUTH
              && cocosession_binding_get(sess, COCO_TARGET_PORT(port, COCO_ACT_BUTTON2)).button == COCO_PAD_BTN_EAST,
              "South is button 1, East button 2");
    }
    check(cocosession_binding_get(sess, COCO_TARGET_SWITCH(COCO_SW_RESET)).button == COCO_PAD_BTN_NONE
          && cocosession_binding_get(sess, COCO_TARGET_SYSACT(COCO_SYSACT_DEBUG_STOP)).button == COCO_PAD_BTN_NONE,
          "RESET and the debugger stop have no pad default");
    check(cocosession_target_for_button(sess, 0, COCO_PAD_BTN_DPAD_UP) == UP_OF(0)
          && cocosession_target_for_button(sess, 1, COCO_PAD_BTN_DPAD_UP) == UP_OF(1)
          && cocosession_target_for_button(sess, 1, COCO_PAD_BTN_SOUTH) == COCO_TARGET_PORT(1, COCO_ACT_BUTTON1),
          "a pad button resolves on the pad's own joystick");
    check(cocosession_target_for_button(sess, 0, COCO_PAD_BTN_WEST) == -1
          && cocosession_target_for_button(sess, 0, COCO_PAD_BTN_NONE) == -1,
          "West is free (the old layer fired button 2 from it), and NONE is nothing");
    bind = cocosession_binding_get(sess, -1);
    check(bind.keysym == 0 && bind.button == COCO_PAD_BTN_NONE, "an out-of-range target is unbound");

    /* names */
    check(!strcmp(coco_target_name(RIGHT_OF(0)), "Right joystick: Right")
          && !strcmp(coco_target_name(UP_OF(1)), "Left joystick: Up")
          && !strcmp(coco_target_name(COCO_TARGET_PORT(0, COCO_ACT_BUTTON1)), "Right joystick: Button 1")
          && !strcmp(coco_target_name(COCO_TARGET_SWITCH(COCO_SW_RESET)), "RESET")
          && !strcmp(coco_target_name(COCO_TARGET_SYSACT(0)), "Debugger Stop")
          && !strcmp(coco_target_name(14), ""),
          "target names");
    check(!strcmp(coco_target_short_name(UP_OF(1)), "Up")
          && !strcmp(coco_target_short_name(COCO_TARGET_PORT(1, COCO_ACT_BUTTON2)), "Button 2")
          && !strcmp(coco_target_short_name(COCO_TARGET_SWITCH(0)), "RESET")
          && !strcmp(coco_target_short_name(COCO_TARGET_SYSACT(0)), "Debugger Stop"),
          "short names");
    check(!strcmp(coco_pad_button_name(COCO_PAD_BTN_SOUTH), "A")
          && !strcmp(coco_pad_button_name(COCO_PAD_BTN_RIGHT_TRIGGER), "Right Trigger")
          && !strcmp(coco_pad_button_name(COCO_PAD_BTN_RAW_BASE + 3), "Button 3")
          && !strcmp(coco_pad_button_name(COCO_PAD_HAT_BASE + 2), "Hat 0 Right")
          && !strcmp(coco_pad_button_name(COCO_PAD_BTN_NONE), ""),
          "pad button names");
    {
        char name[32];

        cocosession_keysym_name('z', name, sizeof name);
        check(!strcmp(name, "Z"), "a letter key is named in capitals");
        cocosession_keysym_name(0xFFBE + 8, name, sizeof name);
        check(!strcmp(name, "F9"), "F9");
        cocosession_keysym_name(0x20, name, sizeof name);
        check(!strcmp(name, "Space"), "Space");
        cocosession_keysym_name(0xFFB5, name, sizeof name);
        check(!strcmp(name, "Keypad 5"), "Keypad 5");
        cocosession_keysym_name(0xFF51, name, sizeof name);
        check(!strcmp(name, "Left"), "Left");
        cocosession_keysym_name(0x1234567, name, sizeof name);
        check(!strncmp(name, "Key 0x", 6), "an unknown keysym is hex");
    }

    /* the canonical key identity */
    check(cocosession_bindable_key('A', 0) == 'a' && cocosession_bindable_key('a', 0) == 'a',
          "Shift-A is the a key");
    check(cocosession_bindable_key('!', 0) == '1' && cocosession_bindable_key(':', 0) == ';'
          && cocosession_bindable_key('"', 0) == '\'' && cocosession_bindable_key('_', 0) == '-',
          "shifted symbols are their unshifted key");
    check(cocosession_bindable_key(0, 'Q') == 'q', "no keysym: the unicode");
    check(cocosession_bindable_key(0xFF8D, 0) == 0xFF0D, "keypad Enter is Return");
    check(cocosession_bindable_key(0xFFBE, 0) == 0xFFBE && cocosession_bindable_key(0xFF51, 0) == 0xFF51
          && cocosession_bindable_key(0xFFBE + 8, 0) == 0xFFBE + 8,
          "F1, the arrows and F9 are bindable as they are");
    check(cocosession_bindable_key(0xFFC7, 0) == 0 && cocosession_bindable_key(0xFFC8, 0) == 0
          && cocosession_bindable_key(0xFFC9, 0) == 0, "F10, F11 and F12 are the frontends'");
    check(cocosession_bindable_key(0xFFE1, 0) == 0 && cocosession_bindable_key(0xFFE3, 0) == 0
          && cocosession_bindable_key(0xFFE9, 0) == 0 && cocosession_bindable_key(0xFFE5, 0) == 0
          && cocosession_bindable_key(0xFFEB, 0) == 0, "the pure modifiers cannot be bound");
    check(cocosession_bindable_key(0, 0) == 0, "no key is no key");

    /* binding a key steals it from every holder, and names them */
    stolen[0] = 'x';
    cocosession_binding_set_key(sess, RIGHT_OF(0), 'd', stolen, sizeof stolen);
    check(stolen[0] == '\0' && cocosession_target_for_key(sess, 'd') == RIGHT_OF(0), "bind d to the right joystick's Right");
    check(cocosession_target_for_key(sess, 'D') == RIGHT_OF(0), "and Shift-D finds it");
    cocosession_binding_set_key(sess, RIGHT_OF(1), 'D', stolen, sizeof stolen);
    check(!strcmp(stolen, "Right joystick: Right") && cocosession_target_for_key(sess, 'd') == RIGHT_OF(1)
          && cocosession_binding_get(sess, RIGHT_OF(0)).keysym == 0,
          "binding it again steals it, and says from whom");
    cocosession_binding_set_key(sess, RIGHT_OF(1), 0, stolen, sizeof stolen);
    check(cocosession_target_for_key(sess, 'd') == -1, "keysym 0 unbinds");
    cocosession_binding_set_key(sess, RIGHT_OF(0), 0xFFC9, stolen, sizeof stolen);
    check(cocosession_binding_get(sess, RIGHT_OF(0)).keysym == 0, "F12 is refused");
    cocosession_binding_set_key(sess, RIGHT_OF(0), 'q', NULL, 0);
    cocosession_binding_set_key(sess, RIGHT_OF(0), 0xFFE1, stolen, sizeof stolen);
    check(cocosession_binding_get(sess, RIGHT_OF(0)).keysym == 'q', "and so is Shift: the binding stays");
    cocosession_binding_set_key(sess, 99, 'a', stolen, sizeof stolen);
    cocosession_binding_set_key(sess, -1, 'a', stolen, sizeof stolen);
    check(cocosession_target_for_key(sess, 'a') == -1, "a bad target binds nothing");

    /* a pad button is stolen only within its own joystick and the machine-wide targets */
    cocosession_binding_set_button(sess, UP_OF(0), COCO_PAD_BTN_NORTH, stolen, sizeof stolen);
    cocosession_binding_set_button(sess, UP_OF(1), COCO_PAD_BTN_NORTH, stolen, sizeof stolen);
    check(stolen[0] == '\0' && cocosession_binding_get(sess, UP_OF(0)).button == COCO_PAD_BTN_NORTH
          && cocosession_binding_get(sess, UP_OF(1)).button == COCO_PAD_BTN_NORTH,
          "the same pad button on both joysticks is legal");
    cocosession_binding_set_button(sess, COCO_TARGET_PORT(0, COCO_ACT_BUTTON1), COCO_PAD_BTN_NORTH,
                                   stolen, sizeof stolen);
    check(!strcmp(stolen, "Right joystick: Up") && cocosession_binding_get(sess, UP_OF(0)).button == COCO_PAD_BTN_NONE
          && cocosession_binding_get(sess, UP_OF(1)).button == COCO_PAD_BTN_NORTH,
          "within a joystick it steals, and the other joystick keeps it");
    cocosession_binding_set_button(sess, COCO_TARGET_SWITCH(COCO_SW_RESET), COCO_PAD_BTN_NORTH,
                                   stolen, sizeof stolen);
    check(strstr(stolen, "Right joystick: Button 1") && strstr(stolen, "Left joystick: Up")
          && cocosession_binding_get(sess, UP_OF(1)).button == COCO_PAD_BTN_NONE,
          "a machine-wide target takes the button from both joysticks, naming both");
    check(cocosession_target_for_button(sess, 0, COCO_PAD_BTN_NORTH) == COCO_TARGET_SWITCH(0)
          && cocosession_target_for_button(sess, 1, COCO_PAD_BTN_NORTH) == COCO_TARGET_SWITCH(0),
          "and it resolves from either pad");
    cocosession_binding_set_button(sess, COCO_TARGET_PORT(1, COCO_ACT_DOWN), COCO_PAD_BTN_NORTH,
                                   stolen, sizeof stolen);
    check(!strcmp(stolen, "RESET"), "a joystick control takes it back from a machine-wide one");

    /* persistence: only the differences, through a new session on the same settings */
    cocosession_bindings_reset(sess);
    check(!strcmp(cocosession_get_str(sess, "bindings", "x"), ""), "reset writes an empty diff");
    cocosession_binding_set_key(sess, UP_OF(0), 'w', NULL, 0);
    cocosession_binding_set_key(sess, COCO_TARGET_SYSACT(0), 0xFFBE, NULL, 0);
    cocosession_binding_set_button(sess, COCO_TARGET_PORT(1, COCO_ACT_LEFT), COCO_PAD_BTN_LEFT_SHOULDER, NULL, 0);
    cocosession_binding_set_button(sess, COCO_TARGET_PORT(1, COCO_ACT_BUTTON2), COCO_PAD_BTN_NONE, NULL, 0);
    {
        char diff[256];

        snprintf(diff, sizeof diff, "%s", cocosession_get_str(sess, "bindings", ""));
        check(strstr(diff, "2.k:119") && strstr(diff, "13.k:") && strstr(diff, "7.b:9") && strstr(diff, "11.b:-1"),
              "the settings hold the entries that differ");
        check(!strstr(diff, "0.b:") && !strstr(diff, "5.k:"), "and only those");
    }
    cocosession_free(sess);
    sess = test_session(config, data);
    if (!sess) return 1;
    check(cocosession_binding_get(sess, UP_OF(0)).keysym == 'w' && cocosession_target_for_key(sess, 'w') == UP_OF(0),
          "a new session finds the key");
    check(cocosession_binding_get(sess, COCO_TARGET_SYSACT(0)).keysym == 0xFFBE, "and the action's key");
    check(cocosession_binding_get(sess, COCO_TARGET_PORT(1, COCO_ACT_LEFT)).button == COCO_PAD_BTN_LEFT_SHOULDER
          && cocosession_binding_get(sess, COCO_TARGET_PORT(1, COCO_ACT_BUTTON2)).button == COCO_PAD_BTN_NONE
          && cocosession_binding_get(sess, COCO_TARGET_PORT(0, COCO_ACT_BUTTON2)).button == COCO_PAD_BTN_EAST,
          "and the buttons, including one unbound by hand; the rest stay default");

    /* garbage in the setting is skipped entry by entry */
    cocosession_set_str(sess, "bindings", "99.k:5 abc 3.k:97 -1.b:3 3.b:xx 4.k:65 5.k:70000000000 6.k:65481");
    cocosession_free(sess);
    sess = test_session(config, data);
    if (!sess) return 1;
    check(cocosession_binding_get(sess, COCO_TARGET_PORT(0, COCO_ACT_DOWN)).keysym == 'a',
          "a good entry among bad ones is kept");
    check(cocosession_binding_get(sess, COCO_TARGET_PORT(0, COCO_ACT_BUTTON1)).keysym == 'a',
          "a persisted capital is the same key");
    check(cocosession_binding_get(sess, 6).keysym == 0, "and a persisted F10 is dropped");
    cocosession_bindings_reset(sess);

    /* a bound key is consumed and drives the joystick */
    cocosession_binding_set_key(sess, RIGHT_OF(0), 'd', NULL, 0);
    cocosession_binding_set_key(sess, UP_OF(0), 0xFF52, NULL, 0);
    cocosession_binding_set_key(sess, COCO_TARGET_PORT(1, COCO_ACT_BUTTON1), ' ', NULL, 0);
    cocosession_binding_set_key(sess, COCO_TARGET_SYSACT(0), 'k', NULL, 0);
    check(cocosession_key(sess, 1, 'd', 'd', 0, 0) == 1, "a bound key press is consumed");
    check(axis_x(sess, 0) == 1000 && coco_axis_from_deflection(axis_x(sess, 0)) == 65535, "and the stick is at its stop");
    check(cocosession_key(sess, 1, 'd', 'd', 0, 0) == 1 && axis_x(sess, 0) == 1000, "auto-repeat is consumed and changes nothing");
    check(cocosession_key(sess, 1, 0xFF52, 0, 0, 0) == 1 && axis_y(sess, 0) == -1000, "Up pushes the stick away (-Y)");
    check(cocosession_key(sess, 0, 0xFF52, 0, 0, 0) == 1 && axis_y(sess, 0) == 0, "and its release lets go");
    check(cocosession_key(sess, 0, 'D', 'D', 0, 1) == 1 && axis_x(sess, 0) == 0,
          "Shift-D released finds the same key (the identity is unshifted)");
    check(cocosession_key(sess, 0, 'd', 'd', 0, 0) == 0, "a release with nothing held is not consumed");
    check(cocosession_key(sess, 1, ' ', ' ', 0, 0) == 1 && (cocosession_buttons_held(sess, 1) & 1u)
          && !(cocosession_buttons_held(sess, 0) & 1u), "Space holds the left joystick's button 1");
    check(cocosession_key(sess, 0, ' ', ' ', 0, 0) == 1 && cocosession_buttons_held(sess, 1) == 0, "released");

    /* a release undoes what the press asserted, even if the key was rebound meanwhile */
    cocosession_key(sess, 1, 'd', 'd', 0, 0);
    cocosession_binding_set_key(sess, RIGHT_OF(0), 0, NULL, 0);
    check(axis_x(sess, 0) == 1000, "unbinding does not let go by itself");
    check(cocosession_key(sess, 0, 'd', 'd', 0, 0) == 1 && axis_x(sess, 0) == 0, "its release does");

    /* an unbound key is not consumed (and, with no machine, goes nowhere) */
    check(cocosession_key(sess, 1, 'q', 'q', 0, 0) == 0 && cocosession_key(sess, 0, 'q', 'q', 0, 0) == 0,
          "an unbound key is not consumed");
    check(cocosession_key(sess, 1, 0xFFE1, 0, 0, 1) == 0, "Shift never is");

    /* an action key posts for the UI thread's timer, once per press */
    {
        int act = -1;

        check(cocosession_key(sess, 1, 'k', 'k', 0, 0) == 1, "the debugger-stop key is consumed");
        check(cocosession_sysaction_take(sess, &act) == 1 && act == COCO_SYSACT_DEBUG_STOP, "and posts the action");
        check(cocosession_sysaction_take(sess, &act) == 0, "once");
        cocosession_key(sess, 1, 'k', 'k', 0, 0);
        check(cocosession_sysaction_take(sess, &act) == 0, "auto-repeat does not post it again");
        cocosession_key(sess, 0, 'k', 'k', 0, 0);
        cocosession_key(sess, 1, 'k', 'k', 0, 0);
        check(cocosession_sysaction_take(sess, &act) == 1, "a new press does");
        cocosession_key(sess, 0, 'k', 'k', 0, 0);
    }

    /* release_all lets go of every key's hold */
    cocosession_binding_set_key(sess, RIGHT_OF(0), 'd', NULL, 0);
    cocosession_key(sess, 1, 'd', 'd', 0, 0);
    cocosession_key(sess, 1, 0xFF52, 0, 0, 0);
    cocosession_release_all(sess);
    check(axis_x(sess, 0) == 0 && axis_y(sess, 0) == 0, "release_all centres a stick the keyboard held");
    check(cocosession_key(sess, 0, 'd', 'd', 0, 0) == 0, "and forgets which key held it");

    /* a key bound to a pad-style steal target: two keys on one target is legal by hand */
    keysym = cocosession_bindable_key('x', 0);
    cocosession_binding_set_key(sess, RIGHT_OF(0), keysym, NULL, 0);
    clash = 0;
    for (tgt = 0; tgt < COCO_TARGET_COUNT; tgt++)
        if (cocosession_binding_get(sess, tgt).keysym == keysym) clash++;
    check(clash == 1, "a key sits on one target");

    cocosession_bindings_reset(sess);
    check(cocosession_target_for_key(sess, 'x') == -1 && cocosession_binding_get(sess, UP_OF(0)).button == COCO_PAD_BTN_DPAD_UP,
          "reset unbinds every key and restores the pad defaults");
    cocosession_free(sess);

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
