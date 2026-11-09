/*
 * gamepad_test -- the gamepad layer's pure helpers (no hardware): the sticks'
 * hysteresis (0.5 in, 0.35 out), a stick or D-pad as the joystick's four
 * directions, port assignment and the SDL button tables; and that the SDL
 * gamepad subsystem starts and stops cleanly with nothing attached, with the
 * generation counter carried across the restart.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gamepad_sdl.h"
#include "session_internal.h"
#include "test_input.h"

int main(void)
{
    int assign[4], btn, pass;

    /* stick hysteresis: in at half deflection, out below 0.35 */
    check(COCO_STICK_ENTER == 0.5f && COCO_STICK_EXIT == 0.35f, "the thresholds are 0.5 and 0.35");
#define STICK_LIMITS COCO_STICK_ENTER, COCO_STICK_EXIT
    check(coco_dir_from_stick(0.0f, 0.0f, 0, STICK_LIMITS) == 0, "a centred stick is no direction");
    check(coco_dir_from_stick(0.6f, 0.0f, 0, STICK_LIMITS) == COCO_DIR_RIGHT, "past half right engages right");
    check(coco_dir_from_stick(0.4f, 0.0f, 0, STICK_LIMITS) == 0, "two fifths right does not engage");
    check(coco_dir_from_stick(0.4f, 0.0f, COCO_DIR_RIGHT, STICK_LIMITS) == COCO_DIR_RIGHT, "but holds once engaged");
    check(coco_dir_from_stick(0.3f, 0.0f, COCO_DIR_RIGHT, STICK_LIMITS) == 0, "and releases below the exit");
    check(coco_dir_from_stick(-0.6f, -0.6f, 0, STICK_LIMITS) == (COCO_DIR_LEFT | COCO_DIR_UP), "diagonals are two bits");
    check(coco_dir_from_stick(0.6f, 0.05f, 0, STICK_LIMITS) == COCO_DIR_RIGHT,
          "a pure horizontal picks up no vertical noise");
    check(coco_dir_from_stick(0.0f, 0.9f, COCO_DIR_RIGHT, STICK_LIMITS) == COCO_DIR_DOWN,
          "an axis is judged on its own: a stick that left right is down");

    /* directions as the joystick's directions */
#define ACT_BIT(act) (1u << (act))
    check(coco_acts_from_dir(COCO_DIR_UP) == ACT_BIT(COCO_ACT_UP) &&
          coco_acts_from_dir(COCO_DIR_DOWN) == ACT_BIT(COCO_ACT_DOWN) &&
          coco_acts_from_dir(COCO_DIR_LEFT) == ACT_BIT(COCO_ACT_LEFT) &&
          coco_acts_from_dir(COCO_DIR_RIGHT) == ACT_BIT(COCO_ACT_RIGHT),
          "up, down, left and right are the joystick's four directions");
    check(coco_acts_from_dir(COCO_DIR_UP | COCO_DIR_LEFT) == (ACT_BIT(COCO_ACT_UP) | ACT_BIT(COCO_ACT_LEFT)) &&
          coco_acts_from_dir(COCO_DIR_DOWN | COCO_DIR_RIGHT) == (ACT_BIT(COCO_ACT_DOWN) | ACT_BIT(COCO_ACT_RIGHT)),
          "a diagonal is two directions");
    check(coco_acts_from_dir(COCO_DIR_UP | COCO_DIR_DOWN) == 0 && coco_acts_from_dir(0) == 0 &&
          coco_acts_from_dir(COCO_DIR_LEFT | COCO_DIR_RIGHT) == 0, "opposites cancel");
    check(coco_acts_from_dir(COCO_DIR_UP | COCO_DIR_DOWN | COCO_DIR_LEFT) == ACT_BIT(COCO_ACT_LEFT),
          "a cancelled axis leaves the other one alone");

    /* port assignment: the first pad is the RIGHT joystick, port 0 */
    assign[0] = -1; assign[1] = -1; assign[2] = -1;
    check(coco_pad_for_port(assign, 2, 0) == 0 && coco_pad_for_port(assign, 2, 1) == 1,
          "connection order: the first pad the right joystick, the second the left");
    check(coco_pad_for_port(assign, 1, 1) == -1, "one pad leaves the left joystick alone");
    check(coco_pad_for_port(assign, 3, 2) == -1, "a third pad has no port");
    assign[0] = 1; assign[1] = -1;
    check(coco_pad_for_port(assign, 2, 1) == 0 && coco_pad_for_port(assign, 2, 0) == 1,
          "an explicit assignment wins and the other pad fills the gap");
    assign[0] = 1; assign[1] = 1;
    check(coco_pad_for_port(assign, 2, 1) == 0 && coco_pad_for_port(assign, 2, 0) == -1,
          "two pads on one port: the first wins, the other port stays empty");

    /* SDL button tables round-trip */
    pass = 1;
    for (btn = 0; btn < COCO_PAD_BTN_LEFT_TRIGGER; btn++) {
        const int sdl = coco_sdl_button_from_pad(btn);

        if (sdl < 0 || coco_pad_button_from_sdl((uint8_t)sdl) != btn) pass = 0;
    }
    check(pass, "every named button round-trips through the SDL tables");
    check(coco_pad_button_from_sdl(200) == COCO_PAD_BTN_NONE, "an unknown SDL button is NONE");
    check(coco_sdl_button_from_pad(COCO_PAD_BTN_LEFT_TRIGGER) == -1, "a trigger is not an SDL button");

    /* the subsystem with no hardware: a session with gamepads enabled starts, reports no
     * pads, and stops; its generation never goes back */
    {
        char config[512], data[512];
        cocosession *sess;
        unsigned before;

        if (test_dirs("gamepad_test", config, sizeof config, data, sizeof data) != 0)
            return 1;
        sess = test_session(config, data);
        if (!sess) return 1;
        check(cocosession_gamepad_count(sess) == 0 && cocosession_gamepad_generation(sess) == 0,
              "without the thread there are no pads and generation 0");
        check(gamepad_start(sess) == 0, "the gamepad thread starts");
        check(cocosession_gamepad_count(sess) >= 0, "gamepad count answers");
        check(cocosession_gamepad_effective_port(sess, 99) == -1, "an absent pad has no port");
        check(cocosession_gamepad_assignment(sess, 99) == -1, "or an assignment");
        cocosession_gamepad_assign(sess, 99, 1);
        cocosession_gamepad_capture_begin(sess);
        check(cocosession_gamepad_capture_poll(sess, NULL) == 0, "nothing captured with no pad");
        cocosession_gamepad_capture_cancel(sess);
        before = cocosession_gamepad_generation(sess);
        gamepad_stop(sess);
        check(cocosession_gamepad_generation(sess) >= before && cocosession_gamepad_count(sess) == 0,
              "after the stop the generation is kept");
        before = cocosession_gamepad_generation(sess);
        check(gamepad_start(sess) == 0, "and the thread starts again");
        check(cocosession_gamepad_generation(sess) >= before, "the generation carries on from where it was");
        gamepad_stop(sess);
        check(1, "a session with gamepads stops cleanly");
        cocosession_free(sess);
    }

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
