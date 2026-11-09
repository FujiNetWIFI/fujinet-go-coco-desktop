/*
 * cocosession gamepad backend: SDL3 gamepads (and unmapped joysticks) as CoCo
 * joysticks, on their own thread.
 *
 * A polling thread rather than SDL's event queue on the UI thread: this app's
 * video belongs to GTK/Qt/AppKit/Win32, not SDL, and only the gamepad
 * subsystem is initialised here -- never SDL video. Devices are added and
 * removed as they appear on the system: SDL posts the JOYSTICK_ and GAMEPAD_
 * add/remove pairs and both are taken, with idempotent open/close, so neither
 * kind of device can be missed. A device SDL has no gamepad mapping for (a
 * plain HID adapter) is opened as a raw joystick instead: its buttons and hats
 * are bindable in the raw bands, just unnamed.
 *
 * What a pad drives is one analog joystick (the first pad the RIGHT one, the
 * second the LEFT). Every button goes through the bindings table: by default
 * the D-pad steers (a full deflection), South presses button 1 and East
 * button 2, and a button may be bound to RESET, the debugger stop or another
 * joystick control instead. The left analog stick is the joystick's stick:
 * proportional (a radial dead zone of 0.15, the rest rescaled to full travel),
 * the way a real CoCo joystick is; with "analog_joystick" off an eight-way
 * switch with hysteresis (in at half deflection, out at 0.35). Hat 0 of a raw
 * joystick is digital either way. The ways of holding an input -- a pad
 * button, the sticks, the keyboard -- are separate sources in the session, so
 * letting go of one never releases another; of the analog ones the strongest
 * wins. Ported from the Channel F sibling's gamepad_sdl.c.
 *
 * Pads are assigned to ports in connection order unless assigned explicitly.
 * An explicit assignment is remembered (settings key "pad_assign", see
 * pad_assign.h): a pad that is unplugged and plugged back in, or is there on
 * the next run, gets it back -- matched by its SDL GUID, else by its name. Two
 * identical pads share one entry; the first to connect gets it. Every
 * connect/disconnect is reported to the session for the frontend's toast.
 *
 * The thread is joined by gamepad_stop, which cocosession_stop runs after the
 * emulator thread has gone and before the next xroar_init: nothing here
 * touches XRoar, only the session's input state.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "compat.h"
#include "gamepad_sdl.h"
#include "pad_assign.h"
#include "session_internal.h"

#define MAX_PADS COCO_MAX_PADS
#define MAX_HATS 4

#define TRIGGER_ON  0.5f

typedef struct {
    SDL_JoystickID devid;
    int owner;                /* the session's source number for this pad: stable
                               * while it is connected, whatever its slot */
    SDL_Gamepad *handle;      /* mapped gamepad, or NULL */
    SDL_Joystick *joy;        /* raw joystick when not a gamepad */
    int assign;               /* explicit port, -1 = automatic */
    int by_name;              /* assign was restored from a name-only match
                               * (the entry's GUID differs): an exact-GUID
                               * pad may take it over */
    int port;                 /* the port it drove on the last poll, or -1 */
    int stick_dir;            /* the stick's four-way state (hysteresis) */
    uint8_t sent[4];          /* directions the digital stick / hat hold */
    int xaxis, yaxis;         /* the analog deflection last sent */
    /* What each button's press asserted (target + 1, 0 = nothing), so its
     * release lets go of exactly that even if the binding changed meanwhile,
     * and a pad that goes away drops what it held. */
    int16_t btn_target[256];
    SDL_GUID guid;
    char guidstr[COCO_PAD_GUID_MAX];   /* guid as hex, for the pad_assign table */
    char name[COCO_PAD_NAME_MAX];
    int trigger_down[2];
    Uint8 last_hat[MAX_HATS];
} pad_slot;

typedef struct {
    cocosession *session;
    pthread_t thread;
    pthread_mutex_t lock;
    atomic_int stop_requested;
    pad_slot pads[MAX_PADS];
    int npads;
    unsigned generation;
    /* Map-mode capture */
    int cap_active, cap_have, cap_result;
    /* the remembered explicit assignments (the "pad_assign" setting), for
     * pads that are away as well as those here; guarded by lock */
    coco_pad_table table;
} gamepad_state;

/* ---- pure helpers ----------------------------------------------------------- */

unsigned coco_acts_from_dir(int dir)
{
    unsigned acts = 0;

    if ((dir & COCO_DIR_UP) && !(dir & COCO_DIR_DOWN)) acts |= 1u << COCO_ACT_UP;
    if ((dir & COCO_DIR_DOWN) && !(dir & COCO_DIR_UP)) acts |= 1u << COCO_ACT_DOWN;
    if ((dir & COCO_DIR_LEFT) && !(dir & COCO_DIR_RIGHT)) acts |= 1u << COCO_ACT_LEFT;
    if ((dir & COCO_DIR_RIGHT) && !(dir & COCO_DIR_LEFT)) acts |= 1u << COCO_ACT_RIGHT;
    return acts;
}

int coco_dir_from_stick(float xval, float yval, int prev, float enter, float leave)
{
    int dir = 0;
    /* each axis independently, so diagonals are two bits and a stick that
     * is exactly horizontal never picks up a vertical bit from noise */
    if (xval <= -enter || (xval <= -leave && (prev & COCO_DIR_LEFT))) dir |= COCO_DIR_LEFT;
    if (xval >=  enter || (xval >=  leave && (prev & COCO_DIR_RIGHT))) dir |= COCO_DIR_RIGHT;
    if (yval <= -enter || (yval <= -leave && (prev & COCO_DIR_UP))) dir |= COCO_DIR_UP;
    if (yval >=  enter || (yval >=  leave && (prev & COCO_DIR_DOWN))) dir |= COCO_DIR_DOWN;
    return dir;
}

int coco_pad_button_from_sdl(uint8_t button)
{
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return COCO_PAD_BTN_SOUTH;
    case SDL_GAMEPAD_BUTTON_EAST: return COCO_PAD_BTN_EAST;
    case SDL_GAMEPAD_BUTTON_WEST: return COCO_PAD_BTN_WEST;
    case SDL_GAMEPAD_BUTTON_NORTH: return COCO_PAD_BTN_NORTH;
    case SDL_GAMEPAD_BUTTON_BACK: return COCO_PAD_BTN_BACK;
    case SDL_GAMEPAD_BUTTON_GUIDE: return COCO_PAD_BTN_GUIDE;
    case SDL_GAMEPAD_BUTTON_START: return COCO_PAD_BTN_START;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return COCO_PAD_BTN_LEFT_STICK;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return COCO_PAD_BTN_RIGHT_STICK;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return COCO_PAD_BTN_LEFT_SHOULDER;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return COCO_PAD_BTN_RIGHT_SHOULDER;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return COCO_PAD_BTN_DPAD_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return COCO_PAD_BTN_DPAD_DOWN;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return COCO_PAD_BTN_DPAD_LEFT;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return COCO_PAD_BTN_DPAD_RIGHT;
    default: return COCO_PAD_BTN_NONE;
    }
}

int coco_sdl_button_from_pad(int button)
{
    switch (button) {
    case COCO_PAD_BTN_SOUTH: return SDL_GAMEPAD_BUTTON_SOUTH;
    case COCO_PAD_BTN_EAST: return SDL_GAMEPAD_BUTTON_EAST;
    case COCO_PAD_BTN_WEST: return SDL_GAMEPAD_BUTTON_WEST;
    case COCO_PAD_BTN_NORTH: return SDL_GAMEPAD_BUTTON_NORTH;
    case COCO_PAD_BTN_BACK: return SDL_GAMEPAD_BUTTON_BACK;
    case COCO_PAD_BTN_GUIDE: return SDL_GAMEPAD_BUTTON_GUIDE;
    case COCO_PAD_BTN_START: return SDL_GAMEPAD_BUTTON_START;
    case COCO_PAD_BTN_LEFT_STICK: return SDL_GAMEPAD_BUTTON_LEFT_STICK;
    case COCO_PAD_BTN_RIGHT_STICK: return SDL_GAMEPAD_BUTTON_RIGHT_STICK;
    case COCO_PAD_BTN_LEFT_SHOULDER: return SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
    case COCO_PAD_BTN_RIGHT_SHOULDER: return SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    case COCO_PAD_BTN_DPAD_UP: return SDL_GAMEPAD_BUTTON_DPAD_UP;
    case COCO_PAD_BTN_DPAD_DOWN: return SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    case COCO_PAD_BTN_DPAD_LEFT: return SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    case COCO_PAD_BTN_DPAD_RIGHT: return SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    default: return -1;
    }
}

/* ---- slots ------------------------------------------------------------------ */

static int slot_for_id(gamepad_state *gst, SDL_JoystickID devid)
{
    int idx;

    for (idx = 0; idx < gst->npads; idx++)
        if (gst->pads[idx].devid == devid) return idx;
    return -1;
}

static const char *slot_name(const pad_slot *slot)
{
    const char *name = slot->handle ? SDL_GetGamepadName(slot->handle)
                                    : SDL_GetJoystickName(slot->joy);

    return name ? name : "Gamepad";
}

static void slot_release_all(gamepad_state *gst, pad_slot *slot)
{
    int act, btn;

    if (slot->port >= 0) {
        for (act = 0; act < 4; act++)
            if (slot->sent[act]) {
                slot->sent[act] = 0;
                session_gamepad_stick(gst->session, slot->owner, slot->port, act, 0);
            }
        if (slot->xaxis || slot->yaxis) {
            slot->xaxis = slot->yaxis = 0;
            session_gamepad_analog(gst->session, slot->owner, slot->port, 0, 0);
        }
    }
    for (btn = 0; btn < (int)(sizeof slot->btn_target / sizeof slot->btn_target[0]); btn++)
        if (slot->btn_target[btn]) {
            session_gamepad_press(gst->session, slot->owner, slot->btn_target[btn] - 1, 0);
            slot->btn_target[btn] = 0;
        }
}

static void slot_close(gamepad_state *gst, pad_slot *slot)
{
    slot_release_all(gst, slot);
    if (slot->handle) SDL_CloseGamepad(slot->handle);
    if (slot->joy) SDL_CloseJoystick(slot->joy);
    slot->handle = NULL;
    slot->joy = NULL;
}

static int effective_port_locked(gamepad_state *gst, int idx);

/* Gives the pad in slot idx its remembered port. An entry with the pad's own
 * GUID is the pad's: it takes it from a pad that got it only through the name
 * fallback. A pad known only by name takes the entry if no other pad carries
 * it (two identical pads, or one pad known under two GUIDs): then this one
 * stays automatic. Locked by the caller; only reads settings' copy in
 * gst->table, never writes. */
static void restore_assignment(gamepad_state *gst, int idx)
{
    pad_slot *slot = &gst->pads[idx];
    int ent = coco_pad_table_find(&gst->table, slot->guidstr, slot->name), kix;
    int exact;

    if (ent < 0 || gst->table.entry[ent].port >= COCOSESSION_JOYSTICK_PORTS) return;
    exact = !strcmp(gst->table.entry[ent].guid, slot->guidstr);
    for (kix = 0; kix < gst->npads; kix++) {
        pad_slot *other = &gst->pads[kix];
        if (kix == idx || other->assign < 0 ||
            coco_pad_table_find(&gst->table, other->guidstr, other->name) != ent)
            continue;
        if (!exact || !other->by_name) return;
        other->assign = -1;          /* the entry's owner is here: it wins */
        other->by_name = 0;
    }
    slot->assign = gst->table.entry[ent].port;
    slot->by_name = !exact;
}

/* "Connected: <pad> (right joystick)": the joystick the pad drives, if any. */
static void connect_event(gamepad_state *gst, int idx)
{
    static const char *const joysticks[COCOSESSION_JOYSTICK_PORTS] =
        { "right joystick", "left joystick" };
    pad_slot *slot = &gst->pads[idx];
    char msg[160];
    const int port = effective_port_locked(gst, idx);

    if (port >= 0)
        snprintf(msg, sizeof msg, "Connected: %s (%s)", slot->name, joysticks[port]);
    else
        snprintf(msg, sizeof msg, "Connected: %s", slot->name);
    session_gamepad_event(gst->session, msg);
}

/* Idempotent, and the single place the gamepad/joystick decision is made:
 * both SDL_EVENT_JOYSTICK_ADDED and SDL_EVENT_GAMEPAD_ADDED land here (SDL
 * posts both for a mapped device) and the slot_for_id guard makes the second
 * one a no-op. */
static void open_pad(gamepad_state *gst, SDL_JoystickID devid)
{
    pthread_mutex_lock(&gst->lock);
    if (gst->npads < MAX_PADS && slot_for_id(gst, devid) < 0) {
        /* Not an else-if on SDL_IsGamepad: a device SDL calls a gamepad can
         * still fail to open as one. Falling through to the raw joystick
         * keeps it usable -- every button is still bindable, just unnamed. */
        SDL_Gamepad *gamepad = SDL_IsGamepad(devid) ? SDL_OpenGamepad(devid) : NULL;
        SDL_Joystick *joy = gamepad ? NULL : SDL_OpenJoystick(devid);
        if (gamepad || joy) {
            pad_slot *slot = &gst->pads[gst->npads++];
            int hat, used = 0, kix;
            for (kix = 0; kix < gst->npads - 1; kix++)
                used |= 1 << gst->pads[kix].owner;
            memset(slot, 0, sizeof *slot);
            for (kix = 0; kix < MAX_PADS && (used & (1 << kix)); kix++) {}
            slot->owner = kix;
            slot->devid = devid;
            slot->handle = gamepad;
            slot->joy = joy;
            slot->assign = -1;
            slot->port = -1;
            for (hat = 0; hat < MAX_HATS; hat++) slot->last_hat[hat] = SDL_HAT_CENTERED;
            slot->guid = SDL_GetJoystickGUIDForID(devid);
            snprintf(slot->name, sizeof slot->name, "%s", slot_name(slot));
            SDL_GUIDToString(slot->guid, slot->guidstr, sizeof slot->guidstr);
            restore_assignment(gst, gst->npads - 1);
            gst->generation++;
            fprintf(stderr, "coco gamepad: %s connected (%s)\n", slot->name,
                    gamepad ? "gamepad" : "raw joystick");
            connect_event(gst, gst->npads - 1);
        }
    }
    pthread_mutex_unlock(&gst->lock);
}

static void close_pad(gamepad_state *gst, SDL_JoystickID devid)
{
    int idx, mov;

    pthread_mutex_lock(&gst->lock);
    idx = slot_for_id(gst, devid);
    if (idx >= 0) {
        pad_slot *slot = &gst->pads[idx];
        char msg[160];
        fprintf(stderr, "coco gamepad: %s disconnected\n", slot->name);
        snprintf(msg, sizeof msg, "Disconnected: %s", slot->name);
        slot_close(gst, slot);
        session_gamepad_event(gst->session, msg);
        for (mov = idx; mov < gst->npads - 1; mov++) gst->pads[mov] = gst->pads[mov + 1];
        gst->npads--;
        gst->generation++;
    }
    pthread_mutex_unlock(&gst->lock);
}

/* ---- buttons ---------------------------------------------------------------- */

/* The port a slot drives right now. Locked by the caller. */
static int effective_port_locked(gamepad_state *gst, int idx)
{
    int assign[MAX_PADS], pdx, port;

    for (pdx = 0; pdx < MAX_PADS; pdx++) assign[pdx] = -1;
    for (pdx = 0; pdx < gst->npads; pdx++) assign[pdx] = gst->pads[pdx].assign;
    for (port = 0; port < COCOSESSION_JOYSTICK_PORTS; port++)
        if (coco_pad_for_port(assign, gst->npads, port) == idx) return port;
    return -1;
}

static void dispatch_button(gamepad_state *gst, SDL_JoystickID which, int btn, int down)
{
    int idx, port, owner = 0, target = -1;

    if (btn == COCO_PAD_BTN_NONE) return;

    pthread_mutex_lock(&gst->lock);
    if (gst->cap_active) {
        /* Map mode: every button event is swallowed while armed, not just
         * the captured one, so the press that maps a button does not also
         * fire whatever it used to do. */
        if (down && !gst->cap_have) { gst->cap_have = 1; gst->cap_result = btn; }
        pthread_mutex_unlock(&gst->lock);
        return;
    }
    idx = slot_for_id(gst, which);
    port = idx >= 0 ? effective_port_locked(gst, idx) : -1;
    if (idx >= 0 && port >= 0 && btn >= 0 && btn < 256) {
        pad_slot *slot = &gst->pads[idx];
        owner = slot->owner;
        if (down) {
            target = cocosession_target_for_button(gst->session, port, btn);
            if (target >= 0 && target < COCO_TARGET_SYSACT(0))
                slot->btn_target[btn] = (int16_t)(target + 1);
        } else if (slot->btn_target[btn]) {
            /* what the press asserted, whatever the binding says now */
            target = slot->btn_target[btn] - 1;
            slot->btn_target[btn] = 0;
        }
    }
    pthread_mutex_unlock(&gst->lock);
    if (target < 0) return;

    if (target >= COCO_TARGET_SYSACT(0)) {
        if (down) cocosession_sysaction_post(gst->session, target - COCO_TARGET_SYSACT(0));
        return;
    }
    session_gamepad_press(gst->session, owner, target, down);
}

static int hat_dir_from_mask(Uint8 mask)
{
    switch (mask) {
    case SDL_HAT_UP: return 0;
    case SDL_HAT_RIGHTUP: return 1;
    case SDL_HAT_RIGHT: return 2;
    case SDL_HAT_RIGHTDOWN: return 3;
    case SDL_HAT_DOWN: return 4;
    case SDL_HAT_LEFTDOWN: return 5;
    case SDL_HAT_LEFT: return 6;
    case SDL_HAT_LEFTUP: return 7;
    default: return -1;
    }
}

static void handle_hat_motion(gamepad_state *gst, SDL_JoystickID which, int hat, Uint8 mask)
{
    int idx, prev_dir, dir;

    if (hat < 0 || hat >= MAX_HATS) return;
    pthread_mutex_lock(&gst->lock);
    idx = slot_for_id(gst, which);
    if (idx < 0 || gst->pads[idx].handle) { pthread_mutex_unlock(&gst->lock); return; }
    prev_dir = hat_dir_from_mask(gst->pads[idx].last_hat[hat]);
    gst->pads[idx].last_hat[hat] = mask;
    pthread_mutex_unlock(&gst->lock);
    dir = hat_dir_from_mask(mask);
    /* hat positions are bindable buttons in their own band; hat 0's four
     * cardinal points also feed the digital stick in poll_sticks */
    if (prev_dir >= 0)
        dispatch_button(gst, which, COCO_PAD_HAT_BASE + hat * COCO_PAD_HAT_DIRS + prev_dir, 0);
    if (dir >= 0)
        dispatch_button(gst, which, COCO_PAD_HAT_BASE + hat * COCO_PAD_HAT_DIRS + dir, 1);
}

/* ---- the sticks -------------------------------------------------------------- */

static float axis_norm(Sint16 value)
{
    return value < 0 ? (float)value / 32768.0f : (float)value / 32767.0f;
}

static void slot_stick(const pad_slot *slot, float *xval, float *yval)
{
    if (slot->handle) {
        *xval = axis_norm(SDL_GetGamepadAxis(slot->handle, SDL_GAMEPAD_AXIS_LEFTX));
        *yval = axis_norm(SDL_GetGamepadAxis(slot->handle, SDL_GAMEPAD_AXIS_LEFTY));
    } else {
        *xval = SDL_GetNumJoystickAxes(slot->joy) > 0 ? axis_norm(SDL_GetJoystickAxis(slot->joy, 0)) : 0.0f;
        *yval = SDL_GetNumJoystickAxes(slot->joy) > 1 ? axis_norm(SDL_GetJoystickAxis(slot->joy, 1)) : 0.0f;
    }
}

/* The raw joystick's hat 0 as directions (a mapped gamepad's D-pad is a
 * button, bound like any other). */
static int slot_hat_dir(const pad_slot *slot)
{
    int dir = 0;

    if (!slot->handle && slot->joy && SDL_GetNumJoystickHats(slot->joy) > 0) {
        Uint8 mask = SDL_GetJoystickHat(slot->joy, 0);
        if (mask & SDL_HAT_UP) dir |= COCO_DIR_UP;
        if (mask & SDL_HAT_DOWN) dir |= COCO_DIR_DOWN;
        if (mask & SDL_HAT_LEFT) dir |= COCO_DIR_LEFT;
        if (mask & SDL_HAT_RIGHT) dir |= COCO_DIR_RIGHT;
    }
    return dir;
}

/* A trigger crossing its threshold. Dispatched by poll_sticks once it has
 * let go of the lock: dispatch_button takes it too, and it is not recursive. */
typedef struct {
    SDL_JoystickID devid;
    int button;
    int down;
} trigger_edge;

#define MAX_EDGES (MAX_PADS * 2)

static void poll_one(gamepad_state *gst, pad_slot *slot, int port,
                     trigger_edge *edges, int *nedges)
{
    const cocosession *sess = gst->session;
    uint8_t want[4];
    unsigned acts;
    int dir, act, xaxis = 0, yaxis = 0;
    float leftx, lefty;

    memset(want, 0, sizeof want);
    if (port != slot->port) {
        /* moved to another port (or none): release what it held there */
        slot_release_all(gst, slot);
        slot->port = port;
        slot->stick_dir = 0;
        if (port < 0) return;
    }

    /* a raw joystick's hat 0 is always the digital stick */
    dir = slot_hat_dir(slot);
    if (sess->opts.analog_joystick) {
        /* the stick, as it is: the analog source */
        slot->stick_dir = 0;
        if (slot->handle || slot->joy) {
            slot_stick(slot, &leftx, &lefty);
            coco_deflect_from_stick(leftx, lefty, &xaxis, &yaxis);
        }
    } else if (slot->handle || slot->joy) {
        slot_stick(slot, &leftx, &lefty);
        slot->stick_dir = coco_dir_from_stick(leftx, lefty, slot->stick_dir,
                                              COCO_STICK_ENTER, COCO_STICK_EXIT);
        dir |= slot->stick_dir;
    }
    if (xaxis != slot->xaxis || yaxis != slot->yaxis) {
        slot->xaxis = xaxis;
        slot->yaxis = yaxis;
        session_gamepad_analog(gst->session, slot->owner, port, xaxis, yaxis);
    }
    acts = coco_acts_from_dir(dir);

    for (act = 0; act < 4; act++)
        want[act] = (uint8_t)((acts >> act) & 1u);
    for (act = 0; act < 4; act++) {
        if (want[act] != slot->sent[act]) {
            slot->sent[act] = want[act];
            session_gamepad_stick(gst->session, slot->owner, port, act, want[act]);
        }
    }

    /* the triggers, as buttons */
    if (slot->handle) {
        int trig;
        for (trig = 0; trig < 2; trig++) {
            float value = axis_norm(SDL_GetGamepadAxis(slot->handle,
                trig ? SDL_GAMEPAD_AXIS_RIGHT_TRIGGER : SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
            int down = value >= TRIGGER_ON;
            if (down != slot->trigger_down[trig]) {
                slot->trigger_down[trig] = down;
                if (*nedges < MAX_EDGES) {
                    edges[*nedges].devid = slot->devid;
                    edges[*nedges].button = trig ? COCO_PAD_BTN_RIGHT_TRIGGER : COCO_PAD_BTN_LEFT_TRIGGER;
                    edges[*nedges].down = down;
                    (*nedges)++;
                }
            }
        }
    }
}

static void poll_sticks(gamepad_state *gst)
{
    int idx, nedges = 0;
    trigger_edge edges[MAX_EDGES];

    pthread_mutex_lock(&gst->lock);
    for (idx = 0; idx < gst->npads; idx++)
        poll_one(gst, &gst->pads[idx], effective_port_locked(gst, idx), edges, &nedges);
    pthread_mutex_unlock(&gst->lock);
    for (idx = 0; idx < nedges; idx++)
        dispatch_button(gst, edges[idx].devid, edges[idx].button, edges[idx].down);
}

/* ---- the thread ------------------------------------------------------------- */

static void *thread_main(void *arg)
{
    gamepad_state *gst = arg;
    int count = 0, idx;
    /* Every joystick, not just the ones SDL has a gamepad mapping for:
     * SDL_GetGamepads would silently omit a plain HID adapter entirely. */
    SDL_JoystickID *ids = SDL_GetJoysticks(&count);

    coco_thread_setname("coco-gamepad");
    if (ids) {
        for (idx = 0; idx < count; idx++) open_pad(gst, ids[idx]);
        SDL_free(ids);
    }

    while (!atomic_load(&gst->stop_requested)) {
        SDL_Event event;
        int npads;

        pthread_mutex_lock(&gst->lock);
        npads = gst->npads;
        pthread_mutex_unlock(&gst->lock);
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_EVENT_JOYSTICK_ADDED: open_pad(gst, event.jdevice.which); break;
            case SDL_EVENT_JOYSTICK_REMOVED: close_pad(gst, event.jdevice.which); break;
            case SDL_EVENT_GAMEPAD_ADDED: open_pad(gst, event.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_REMOVED: close_pad(gst, event.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                dispatch_button(gst, event.gbutton.which,
                                coco_pad_button_from_sdl(event.gbutton.button),
                                event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
                break;
            case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
            case SDL_EVENT_JOYSTICK_BUTTON_UP: {
                /* a mapped gamepad also emits these for the same press it
                 * reported as a GAMEPAD_BUTTON event; the slot's kind decides */
                int gamepad, slot_idx;
                pthread_mutex_lock(&gst->lock);
                slot_idx = slot_for_id(gst, event.jbutton.which);
                gamepad = slot_idx >= 0 && gst->pads[slot_idx].handle != NULL;
                pthread_mutex_unlock(&gst->lock);
                if (gamepad || event.jbutton.button >= 64) break;
                dispatch_button(gst, event.jbutton.which,
                                COCO_PAD_BTN_RAW_BASE + event.jbutton.button,
                                event.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN);
                break;
            }
            case SDL_EVENT_JOYSTICK_HAT_MOTION:
                handle_hat_motion(gst, event.jhat.which, event.jhat.hat, event.jhat.value);
                break;
            default:
                break;
            }
        }
        poll_sticks(gst);
        /* ~60 Hz while a pad is attached (the machine's own sampling rate).
         * With none there are only hotplug events to notice, which can wait
         * half a second. (Not SDL_WaitEventTimeout: SDL polls inside it at
         * about 1 kHz.) */
        SDL_Delay(npads > 0 ? 16 : 500);
    }

    pthread_mutex_lock(&gst->lock);
    for (idx = 0; idx < gst->npads; idx++) slot_close(gst, &gst->pads[idx]);
    gst->npads = 0;
    pthread_mutex_unlock(&gst->lock);
    return NULL;
}

int gamepad_start(cocosession *sess)
{
    gamepad_state *gst;
    pthread_attr_t attr;

    if (sess->gamepad) return 0;
    /* The app owns its signals; SDL must not intercept SIGINT/SIGTERM. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    /* Bluetooth pads over hidapi and the joystick subsystem together. */
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        session_set_error(sess, "SDL gamepad init failed: %s", SDL_GetError());
        return -1;
    }
    gst = calloc(1, sizeof *gst);
    if (!gst) { SDL_QuitSubSystem(SDL_INIT_GAMEPAD); return -1; }
    gst->session = sess;
    /* the counter carries on from the last run, so a frontend that remembered
     * a generation never sees the same one again with different pads */
    gst->generation = sess->pad_gen_base;
    /* the remembered assignments, once: garbage or no key is an empty table */
    coco_pad_table_decode(&gst->table, cocosession_get_str(sess, "pad_assign", ""),
                          COCOSESSION_JOYSTICK_PORTS);
    pthread_mutex_init(&gst->lock, NULL);
    /* An explicit stack: the platform default is 512 KB on macOS, and SDL's
     * HID enumeration runs on this thread. */
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4u << 20);
    if (pthread_create(&gst->thread, &attr, thread_main, gst) != 0) {
        pthread_attr_destroy(&attr);
        session_set_error(sess, "gamepad thread could not start");
        pthread_mutex_destroy(&gst->lock);
        free(gst);
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        return -1;
    }
    pthread_attr_destroy(&attr);
    sess->gamepad = gst;
    return 0;
}

void gamepad_stop(cocosession *sess)
{
    gamepad_state *gst = sess->gamepad;

    if (!gst) return;
    atomic_store(&gst->stop_requested, 1);
    pthread_join(gst->thread, NULL);
    sess->pad_gen_base = gst->generation;
    pthread_mutex_destroy(&gst->lock);
    sess->gamepad = NULL;
    free(gst);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

/* ---- public API ------------------------------------------------------------- */

int cocosession_gamepad_count(cocosession *sess)
{
    gamepad_state *gst = sess->gamepad;
    int count;

    if (!gst) return 0;
    pthread_mutex_lock(&gst->lock);
    count = gst->npads;
    pthread_mutex_unlock(&gst->lock);
    return count;
}

int cocosession_gamepad_name(cocosession *sess, int idx, char *dst, int dstsz)
{
    gamepad_state *gst = sess->gamepad;
    int len = 0;

    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!gst) return 0;
    pthread_mutex_lock(&gst->lock);
    if (idx >= 0 && idx < gst->npads)
        len = snprintf(dst, (size_t)dstsz, "%s", slot_name(&gst->pads[idx]));
    pthread_mutex_unlock(&gst->lock);
    return len;
}

void cocosession_gamepad_assign(cocosession *sess, int idx, int port)
{
    gamepad_state *gst = sess->gamepad;
    char text[COCO_PAD_ASSIGN_TEXT_MAX];
    int changed = 0;

    if (!gst) return;
    pthread_mutex_lock(&gst->lock);
    if (idx >= 0 && idx < gst->npads) {
        pad_slot *slot = &gst->pads[idx];
        if (port >= 0 && port < COCOSESSION_JOYSTICK_PORTS) {
            slot->assign = port;
            slot->by_name = 0;
            changed = coco_pad_table_set(&gst->table, slot->guidstr, slot->name, port) > 0;
        } else {
            const char *keepguid[MAX_PADS], *keepname[MAX_PADS];
            int kix, nkeep = 0;
            slot->assign = -1;
            slot->by_name = 0;
            for (kix = 0; kix < gst->npads; kix++)
                if (kix != idx && gst->pads[kix].assign >= 0) {
                    keepguid[nkeep] = gst->pads[kix].guidstr;
                    keepname[nkeep++] = gst->pads[kix].name;
                }
            changed = coco_pad_table_remove(&gst->table, slot->guidstr, slot->name,
                                            nkeep, keepguid, keepname);
        }
        if (changed && coco_pad_table_encode(&gst->table, text, sizeof text) < 0)
            changed = 0;
    }
    pthread_mutex_unlock(&gst->lock);
    /* the settings file is written here, on the caller's thread, with the
     * pad lock let go: the pad thread must never wait on a file write */
    if (changed)
        cocosession_set_str(sess, "pad_assign", text);
}

int cocosession_gamepad_assignment(cocosession *sess, int idx)
{
    gamepad_state *gst = sess->gamepad;
    int result = -1;

    if (!gst) return -1;
    pthread_mutex_lock(&gst->lock);
    if (idx >= 0 && idx < gst->npads) result = gst->pads[idx].assign;
    pthread_mutex_unlock(&gst->lock);
    return result;
}

int cocosession_gamepad_effective_port(cocosession *sess, int idx)
{
    gamepad_state *gst = sess->gamepad;
    int result = -1;

    if (!gst) return -1;
    pthread_mutex_lock(&gst->lock);
    if (idx >= 0 && idx < gst->npads) result = effective_port_locked(gst, idx);
    pthread_mutex_unlock(&gst->lock);
    return result;
}

unsigned cocosession_gamepad_generation(cocosession *sess)
{
    gamepad_state *gst = sess->gamepad;
    unsigned result;

    if (!gst) return sess->pad_gen_base;
    pthread_mutex_lock(&gst->lock);
    result = gst->generation;
    pthread_mutex_unlock(&gst->lock);
    return result;
}

void cocosession_gamepad_capture_begin(cocosession *sess)
{
    gamepad_state *gst = sess->gamepad;

    if (!gst) return;
    pthread_mutex_lock(&gst->lock);
    gst->cap_active = 1; gst->cap_have = 0; gst->cap_result = COCO_PAD_BTN_NONE;
    pthread_mutex_unlock(&gst->lock);
}

void cocosession_gamepad_capture_cancel(cocosession *sess)
{
    gamepad_state *gst = sess->gamepad;

    if (!gst) return;
    pthread_mutex_lock(&gst->lock);
    gst->cap_active = 0; gst->cap_have = 0;
    pthread_mutex_unlock(&gst->lock);
}

int cocosession_gamepad_capture_poll(cocosession *sess, int *button)
{
    gamepad_state *gst = sess->gamepad;
    int got = 0;

    if (!gst) return 0;
    pthread_mutex_lock(&gst->lock);
    if (gst->cap_active && gst->cap_have) {
        if (button) *button = gst->cap_result;
        gst->cap_active = 0; gst->cap_have = 0;
        got = 1;
    }
    pthread_mutex_unlock(&gst->lock);
    return got;
}
