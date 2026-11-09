/*
 * gamepad_sdl -- private interface: the pure helpers unit-tested without
 * hardware, and the start/stop the session calls.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef COCO_GAMEPAD_SDL_H
#define COCO_GAMEPAD_SDL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Direction bits a stick or D-pad resolves to. */
#define COCO_DIR_UP    1
#define COCO_DIR_DOWN  2
#define COCO_DIR_LEFT  4
#define COCO_DIR_RIGHT 8

/* With the analog stick off, a pad's stick is an eight-way switch: it engages
 * a direction at half deflection and lets go below 0.35, wide enough that a
 * thumb resting near the threshold cannot flutter. */
#define COCO_STICK_ENTER 0.5f
#define COCO_STICK_EXIT  0.35f

/* The stick's (or the D-pad's, or a hat's) direction bits as the joystick's
 * four directions, a mask over coco_action (1 << COCO_ACT_*): UP is -Y, DOWN
 * +Y. Opposite directions cancel, so a stick pushed both ways holds neither.
 * Pure. */
unsigned coco_acts_from_dir(int dir);

/* An analog stick as an eight-way switch with hysteresis: an axis engages
 * past `enter` and releases below `leave` (both 0..1 of full deflection), so a
 * thumb resting near the threshold cannot flutter. xval and yval are SDL's
 * (+x right, +y down); `prev` is the previous result. Pure. */
int coco_dir_from_stick(float xval, float yval, int prev, float enter, float leave);

/* SDL3's SDL_GamepadButton (a uint8_t) to the public numbering, and back.
 * Explicit tables, never a cast, so an SDL reordering cannot desync them. */
int coco_pad_button_from_sdl(uint8_t button);
int coco_sdl_button_from_pad(int button);

#ifdef __cplusplus
}
#endif

#endif /* COCO_GAMEPAD_SDL_H */
