/*
 * input -- every way a joystick direction, a fire button, RESET or one of the
 * machine's own keys gets held, and the one result the machine sees.
 *
 * Joysticks. A direction or button is held by sources (session_internal.h
 * SRC_*: the host keyboard and the Controllers window's buttons, the pointer's
 * drag, and for each gamepad its buttons and its stick), one bit per source
 * per contact, so letting go of one source never releases another's. A
 * joystick axis is merged per axis: the digital sources push a full
 * deflection (RIGHT and LEFT held together cancel), the analog sources (a
 * pad's stick in analog mode, the pointer) push a proportional one in
 * milli-units, and the strongest deflection wins. It is per axis rather than
 * per vector so that a held arrow key on X and an analog stick on Y combine.
 * Ties keep the earlier candidate (digital, then the pads in owner order, then
 * the pointer). Fire buttons OR together. After every change the result goes
 * to the host (cocohost_set_joystick_axis / _button), which the emulator reads
 * whenever the machine samples the joystick.
 *
 * Machine keys. The host keyboard and the Controllers window's keyboard both
 * hold a machine key by its HID usage (key_src), and the key goes to the
 * machine on the 0 <-> held transitions only, so one source letting go of a
 * key another still holds does nothing. A modifier cap in the window latches
 * (sticky); it lets go after the next ordinary key does.
 *
 * Nothing here needs a running machine except the final hand-off of a machine
 * key and RESET, so the merge can be tested without one.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdlib.h>
#include <string.h>

#include "coco_host.h"
#include "cocodebug.h"
#include "session_internal.h"

#define FULL_TRAVEL 1000

void input_init(cocosession *sess)
{
    (void)sess;
}

/* ---- what the machine sees of a joystick ------------------------------------ */

static int clamp_milli(int value)
{
    return value < -FULL_TRAVEL ? -FULL_TRAVEL : value > FULL_TRAVEL ? FULL_TRAVEL : value;
}

/* input_mtx held. */
static void merged_locked(const cocosession *sess, int port, int *xaxis, int *yaxis)
{
    int axis;

    for (axis = 0; axis < 2; axis++) {
        const int low = axis ? COCO_ACT_UP : COCO_ACT_LEFT;
        const int high = axis ? COCO_ACT_DOWN : COCO_ACT_RIGHT;
        int best = (sess->dir_src[port][high] ? FULL_TRAVEL : 0)
                 - (sess->dir_src[port][low] ? FULL_TRAVEL : 0);
        int owner;

        for (owner = 0; owner <= COCO_MAX_PADS; owner++) {
            const int value = axis ? sess->an_y[owner][port] : sess->an_x[owner][port];
            if (abs(value) > abs(best))
                best = value;
        }
        if (axis)
            *yaxis = best;
        else
            *xaxis = best;
    }
}

static void push_joystick_locked(cocosession *sess, int port)
{
    int xaxis, yaxis, btn;

    merged_locked(sess, port, &xaxis, &yaxis);
    cocohost_set_joystick_axis(port, 0, coco_axis_from_deflection(xaxis));
    cocohost_set_joystick_axis(port, 1, coco_axis_from_deflection(yaxis));
    for (btn = 0; btn < 2; btn++)
        cocohost_set_joystick_button(port, btn, sess->btn_src[port][btn] != 0);
}

static void reset_machine(cocosession *sess)
{
    if (sess->running)
        cocohost_core_reset();
}

/* ---- the machine's keys --------------------------------------------------------- */

static int usage_is_modifier(unsigned usage)
{
    return (usage >= 0xe0 && usage <= 0xe7) || usage == 0x39;   /* 0x39: CoCo 3 CTRL */
}

static int latched_locked(const cocosession *sess, unsigned usage)
{
    return (sess->cap_latch[usage >> 3] >> (usage & 7)) & 1;
}

static void hid_set_locked(cocosession *sess, unsigned usage, unsigned source, int down, int oneshot);

/* The next ordinary key to go up lets go of every latched modifier. */
static void release_latches_locked(cocosession *sess)
{
    unsigned usage;

    for (usage = 0; usage < 256; usage++) {
        if (!latched_locked(sess, usage))
            continue;
        sess->cap_latch[usage >> 3] &= (uint8_t)~(1u << (usage & 7));
        hid_set_locked(sess, usage, KEYSRC_CAP, 0, 0);
    }
}

static void hid_set_locked(cocosession *sess, unsigned usage, unsigned source, int down, int oneshot)
{
    const uint8_t before = sess->key_src[usage];

    if (down)
        sess->key_src[usage] |= (uint8_t)source;
    else
        sess->key_src[usage] &= (uint8_t)~source;
    if (!before && sess->key_src[usage]) {
        if (sess->running)
            cocohost_inject_key(1, (int)usage);
    } else if (before && !sess->key_src[usage]) {
        if (sess->running)
            cocohost_inject_key(0, (int)usage);
        if (oneshot && !usage_is_modifier(usage))
            release_latches_locked(sess);
    }
}

/* The host keyboard's key, after bindings declined it. */
void session_physical_key(cocosession *sess, int down, uint32_t keysym, uint32_t unicode,
                          int ctrl_down, int shift_down)
{
    unsigned scancode = 0;

    if (!coco_key_from_event(keysym, unicode, ctrl_down, shift_down, &scancode))
        return;
    pthread_mutex_lock(&sess->input_mtx);
    hid_set_locked(sess, scancode, KEYSRC_PHYSICAL, down, 1);
    pthread_mutex_unlock(&sess->input_mtx);
}

void cocosession_keyboard_press(cocosession *sess, int index, int down)
{
    const coco_keycap *caps;
    int count = 0;
    unsigned usage;

    caps = cocosession_keyboard_layout(sess->cur_machine, &count, NULL, NULL);
    if (index < 0 || index >= count)
        return;
    usage = caps[index].hid & 0xff;
    pthread_mutex_lock(&sess->input_mtx);
    if (caps[index].kind == COCO_KEYCAP_MODIFIER) {
        /* a modifier cap toggles on each press and ignores the release */
        if (down) {
            if (latched_locked(sess, usage)) {
                sess->cap_latch[usage >> 3] &= (uint8_t)~(1u << (usage & 7));
                hid_set_locked(sess, usage, KEYSRC_CAP, 0, 1);
            } else {
                sess->cap_latch[usage >> 3] |= (uint8_t)(1u << (usage & 7));
                hid_set_locked(sess, usage, KEYSRC_CAP, 1, 1);
            }
        }
    } else {
        hid_set_locked(sess, usage, KEYSRC_CAP, down, 1);
    }
    pthread_mutex_unlock(&sess->input_mtx);
}

int cocosession_keyboard_held(cocosession *sess, int index)
{
    const coco_keycap *caps;
    int count = 0, held;
    unsigned usage;

    caps = cocosession_keyboard_layout(sess->cur_machine, &count, NULL, NULL);
    if (index < 0 || index >= count)
        return 0;
    usage = caps[index].hid & 0xff;
    pthread_mutex_lock(&sess->input_mtx);
    held = (sess->key_src[usage] != 0) | (latched_locked(sess, usage) << 1);
    pthread_mutex_unlock(&sess->input_mtx);
    return held;
}

void cocosession_keyboard_release(cocosession *sess)
{
    unsigned usage;

    pthread_mutex_lock(&sess->input_mtx);
    memset(sess->cap_latch, 0, sizeof sess->cap_latch);
    for (usage = 0; usage < 256; usage++)
        if (sess->key_src[usage] & KEYSRC_CAP)
            hid_set_locked(sess, usage, KEYSRC_CAP, 0, 0);
    pthread_mutex_unlock(&sess->input_mtx);
}

/* ---- a start ---------------------------------------------------------------------- */

void input_clear(cocosession *sess)
{
    int port;

    pthread_mutex_lock(&sess->input_mtx);
    memset(sess->dir_src, 0, sizeof sess->dir_src);
    memset(sess->btn_src, 0, sizeof sess->btn_src);
    memset(sess->sw_src, 0, sizeof sess->sw_src);
    memset(sess->an_x, 0, sizeof sess->an_x);
    memset(sess->an_y, 0, sizeof sess->an_y);
    memset(sess->key_src, 0, sizeof sess->key_src);
    memset(sess->cap_latch, 0, sizeof sess->cap_latch);
    memset(sess->held_keysym, 0, sizeof sess->held_keysym);
    /* the host initialises the joystick atomics once, and a machine that
     * stopped with the stick off-centre must not start that way */
    for (port = 0; port < COCOSESSION_JOYSTICK_PORTS; port++)
        push_joystick_locked(sess, port);
    cocohost_release_all_keys();
    pthread_mutex_unlock(&sess->input_mtx);
}

/* ---- the public input API ------------------------------------------------------------ */

static void set_src(uint32_t *word, uint32_t source, int down)
{
    if (down)
        *word |= source;
    else
        *word &= ~source;
}

/* One source of one target; system actions are not held inputs and are left
 * to the caller. */
static void source_press(cocosession *sess, int target, uint32_t source, int down)
{
    int fire_reset = 0;

    pthread_mutex_lock(&sess->input_mtx);
    if (target >= 0 && target < COCO_TARGET_SWITCH(0)) {
        const int port = target / COCO_ACT_PER_PORT;
        const int act = target % COCO_ACT_PER_PORT;

        if (act < COCO_ACT_BUTTON1)
            set_src(&sess->dir_src[port][act], source, down);
        else
            set_src(&sess->btn_src[port][act - COCO_ACT_BUTTON1], source, down);
        push_joystick_locked(sess, port);
    } else if (target >= COCO_TARGET_SWITCH(0) && target < COCO_TARGET_SYSACT(0)) {
        uint32_t *word = &sess->sw_src[target - COCO_TARGET_SWITCH(0)];
        const int was = *word != 0;

        set_src(word, source, down);
        /* RESET acts on the press, once, whoever holds it */
        fire_reset = !was && *word != 0;
    }
    pthread_mutex_unlock(&sess->input_mtx);
    if (fire_reset)
        reset_machine(sess);
}

void cocosession_press(cocosession *sess, int target, int down)
{
    if (!sess || target < 0 || target >= COCO_TARGET_COUNT)
        return;
    if (target >= COCO_TARGET_SYSACT(0)) {
        if (down)
            cocosession_sysaction(sess, target - COCO_TARGET_SYSACT(0));
        return;
    }
    source_press(sess, target, SRC_KEYBOARD, down);
}

void cocosession_sysaction(cocosession *sess, int sysact)
{
    switch (sysact) {
    case COCO_SYSACT_DEBUG_STOP:
        if (sess->running) {
            cocodebug *dbg = cocosession_debugger(sess);

            cocodebug_set_engaged(dbg, 1);
            cocodebug_pause(dbg);
        }
        break;
    default:
        break;
    }
}

void cocosession_sysaction_post(cocosession *sess, int sysact)
{
    if (sysact < 0 || sysact >= COCO_SYSACT_COUNT)
        return;
    pthread_mutex_lock(&sess->sysact_mtx);
    sess->sysact_pending |= 1u << sysact;
    pthread_mutex_unlock(&sess->sysact_mtx);
}

int cocosession_sysaction_take(cocosession *sess, int *out)
{
    int found = 0, act;

    pthread_mutex_lock(&sess->sysact_mtx);
    for (act = 0; act < COCO_SYSACT_COUNT; act++) {
        if (sess->sysact_pending & (1u << act)) {
            sess->sysact_pending &= ~(1u << act);
            if (out)
                *out = act;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&sess->sysact_mtx);
    return found;
}

void cocosession_release_all(cocosession *sess)
{
    int port, swi;
    unsigned usage;

    if (!sess)
        return;
    memset(sess->held_keysym, 0, sizeof sess->held_keysym);
    pthread_mutex_lock(&sess->input_mtx);
    for (port = 0; port < COCOSESSION_JOYSTICK_PORTS; port++) {
        int act;

        for (act = 0; act < 4; act++)
            sess->dir_src[port][act] &= ~SRC_KEYBOARD;
        for (act = 0; act < 2; act++)
            sess->btn_src[port][act] &= ~SRC_KEYBOARD;
        push_joystick_locked(sess, port);
    }
    for (swi = 0; swi < COCO_SW_COUNT; swi++)
        sess->sw_src[swi] &= ~SRC_KEYBOARD;
    /* the host keyboard's machine keys; the window's keyboard stays */
    for (usage = 0; usage < 256; usage++)
        if (sess->key_src[usage] & KEYSRC_PHYSICAL)
            hid_set_locked(sess, usage, KEYSRC_PHYSICAL, 0, 0);
    pthread_mutex_unlock(&sess->input_mtx);
}

void cocosession_pointer_set(cocosession *sess, int port, int xaxis, int yaxis, unsigned buttons)
{
    int btn;

    if (!sess || port < 0 || port >= COCOSESSION_JOYSTICK_PORTS)
        return;
    pthread_mutex_lock(&sess->input_mtx);
    sess->an_x[COCO_MAX_PADS][port] = (int16_t)clamp_milli(xaxis);
    sess->an_y[COCO_MAX_PADS][port] = (int16_t)clamp_milli(yaxis);
    for (btn = 0; btn < 2; btn++)
        set_src(&sess->btn_src[port][btn], SRC_POINTER, (buttons >> btn) & 1u);
    push_joystick_locked(sess, port);
    pthread_mutex_unlock(&sess->input_mtx);
}

void cocosession_joystick_held(cocosession *sess, int port, int *xaxis, int *yaxis)
{
    int xval = 0, yval = 0;

    if (sess && port >= 0 && port < COCOSESSION_JOYSTICK_PORTS) {
        pthread_mutex_lock(&sess->input_mtx);
        merged_locked(sess, port, &xval, &yval);
        pthread_mutex_unlock(&sess->input_mtx);
    }
    if (xaxis)
        *xaxis = xval;
    if (yaxis)
        *yaxis = yval;
}

unsigned cocosession_buttons_held(cocosession *sess, int port)
{
    unsigned held = 0;
    int btn;

    if (!sess || port < 0 || port >= COCOSESSION_JOYSTICK_PORTS)
        return 0;
    pthread_mutex_lock(&sess->input_mtx);
    for (btn = 0; btn < 2; btn++)
        if (sess->btn_src[port][btn])
            held |= 1u << btn;
    pthread_mutex_unlock(&sess->input_mtx);
    return held;
}

int cocosession_switch_held(cocosession *sess, int swi)
{
    int held;

    if (!sess || swi < 0 || swi >= COCO_SW_COUNT)
        return 0;
    pthread_mutex_lock(&sess->input_mtx);
    held = sess->sw_src[swi] != 0;
    pthread_mutex_unlock(&sess->input_mtx);
    return held;
}

/* The gamepad thread's entry points: the same routing, from the pad's own
 * source bits, never touching the settings or the debugger. */
void session_gamepad_press(cocosession *sess, int owner, int target, int down)
{
    if (owner < 0 || owner >= COCO_MAX_PADS || target < 0 || target >= COCO_TARGET_SYSACT(0))
        return;
    source_press(sess, target, SRC_PAD(owner), down);
}

void session_gamepad_stick(cocosession *sess, int owner, int port, int act, int down)
{
    if (owner < 0 || owner >= COCO_MAX_PADS || port < 0 || port >= COCOSESSION_JOYSTICK_PORTS
        || act < 0 || act > COCO_ACT_DOWN)
        return;
    source_press(sess, COCO_TARGET_PORT(port, act), SRC_STICK(owner), down);
}

void session_gamepad_analog(cocosession *sess, int owner, int port, int xaxis, int yaxis)
{
    if (owner < 0 || owner >= COCO_MAX_PADS || port < 0 || port >= COCOSESSION_JOYSTICK_PORTS)
        return;
    pthread_mutex_lock(&sess->input_mtx);
    sess->an_x[owner][port] = (int16_t)clamp_milli(xaxis);
    sess->an_y[owner][port] = (int16_t)clamp_milli(yaxis);
    push_joystick_locked(sess, port);
    pthread_mutex_unlock(&sess->input_mtx);
}

void cocosession_set_analog(cocosession *sess, int enable)
{
    sess->opts.analog_joystick = enable ? 1 : 0;
    cocosession_set_int(sess, "analog_joystick", sess->opts.analog_joystick);
}
