/*
 * gamepad_map_test -- the gamepad layer end to end with an SDL3 virtual
 * gamepad and no machine: hot-plug pickup and its toast, the default mapping
 * onto a joystick (the D-pad steers at full deflection, South and East are
 * the buttons, the left stick is proportional or, with "analog_joystick" off,
 * an eight-way switch with hysteresis), a plain HID joystick's hat and
 * buttons, buttons rebound to RESET and to the debugger stop, the sources not
 * stepping on each other, a trigger held while the session is asked for its
 * port (it once froze the pad thread), Map mode's capture, and hot-unplug
 * releasing everything the pad held. Then the pad's port assignment: written
 * to the settings ("pad_assign") when made, restored when a new session (a
 * restart) finds the pad again, found through a stale GUID by the name, kept
 * through garbage, removed by Automatic, and kept across a hot replug and a
 * stop/start of the pad thread; and the generation counter never goes back.
 *
 * What the pad holds is read back through cocosession_joystick_held,
 * cocosession_buttons_held and cocosession_switch_held, the same merged state
 * the machine is driven from. The pad thread is started directly
 * (gamepad_start), so no emulator and no ROMs are needed. SKIPs (77) where SDL
 * has no virtual joysticks or real pads leave no port free.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <SDL3/SDL.h>

#include "session_internal.h"
#include "test_input.h"

#define PAD_NAME "cocotest virtual pad"
#define RAW_NAME "cocotest raw joystick"
#define TWIN_NAME "cocotest second pad"

static int vport;        /* the joystick (port) the virtual pad was given */

/* Waits (polling, up to five seconds: a slow runner is slow) for the merged
 * state to be (xaxis, yaxis, buttons, reset). */
static void expect(cocosession *sess, int port, int xaxis, int yaxis, unsigned buttons, int reset,
                   const char *what)
{
    int gotx = 0, goty = 0, gotreset = 0, tries;
    unsigned gotbtn = 0;

    for (tries = 0; tries < 250; tries++) {
        cocosession_joystick_held(sess, port, &gotx, &goty);
        gotbtn = cocosession_buttons_held(sess, port);
        gotreset = cocosession_switch_held(sess, COCO_SW_RESET);
        if (gotx == xaxis && goty == yaxis && gotbtn == buttons && gotreset == reset) {
            printf("ok: %s\n", what);
            return;
        }
        nap_ms(20);
    }
    printf("FAIL: %s (x %d want %d, y %d want %d, buttons %u want %u, RESET %d want %d)\n", what,
           gotx, xaxis, goty, yaxis, gotbtn, buttons, gotreset, reset);
    failures++;
}

/* The stick's value for a raw SDL axis (-32768..32767) as the pad layer will compute it. */
static int stick_milli(int raw)
{
    int xaxis, yaxis;

    coco_deflect_from_stick(raw < 0 ? (float)raw / 32768.0f : (float)raw / 32767.0f, 0.0f, &xaxis, &yaxis);
    return xaxis;
}

/* The stick's value for a raw pair of SDL axes, both of them. */
static void stick_pair(int rawx, int rawy, int *xaxis, int *yaxis)
{
    coco_deflect_from_stick(rawx < 0 ? (float)rawx / 32768.0f : (float)rawx / 32767.0f,
                            rawy < 0 ? (float)rawy / 32768.0f : (float)rawy / 32767.0f, xaxis, yaxis);
}

/* A session on the given directories with only the pad thread. */
static cocosession *session_up(const char *config, const char *data)
{
    cocosession *sess = test_session(config, data);

    if (!sess) return NULL;
    /* what cocosession_start does with the setting before the thread runs */
    sess->opts.analog_joystick = cocosession_get_int(sess, "analog_joystick", 1) ? 1 : 0;
    if (gamepad_start(sess) != 0) {
        printf("FAIL: gamepad_start: %s\n", cocosession_last_error(sess));
        cocosession_free(sess);
        return NULL;
    }
    return sess;
}

static void session_down(cocosession *sess)
{
    gamepad_stop(sess);
    cocosession_free(sess);
}

/* The named virtual pad's slot now, or -1. */
static int pad_slot_now(cocosession *sess, const char *name)
{
    int idx;

    for (idx = 0; idx < cocosession_gamepad_count(sess); idx++) {
        char got[64];

        cocosession_gamepad_name(sess, idx, got, sizeof got);
        if (strcmp(got, name) == 0)
            return idx;
    }
    return -1;
}

/* The pad's slot once the pad thread has found it (polled, up to five
 * seconds), or -1. */
static int wait_for_pad(cocosession *sess, const char *name)
{
    int idx = pad_slot_now(sess, name), tries;

    for (tries = 0; tries < 250 && idx < 0; tries++) {
        nap_ms(20);
        idx = pad_slot_now(sess, name);
    }
    return idx;
}

/* Each pad gets a product id of its own: SDL names a mapped gamepad by its
 * GUID, so two with the same ids would both carry the first one's name. */
static SDL_JoystickID attach_gamepad(const char *name, Uint16 product)
{
    SDL_VirtualJoystickDesc desc;

    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = 0x1209;
    desc.product_id = product;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = name;
    return SDL_AttachVirtualJoystick(&desc);
}

/* The pad_assign setting with the pad's entry in it? */
static int has_entry(cocosession *sess, const char *guid, const char *name, int port)
{
    char want[160];

    snprintf(want, sizeof want, "%s|%s=%d", guid, name, port);
    return strstr(cocosession_get_str(sess, "pad_assign", ""), want) != NULL;
}

/* A pad's port assignment survives a restart. The virtual pad stays attached
 * across the sessions (SDL is held open by the caller). */
static void persistence(const char *config, const char *data)
{
    SDL_JoystickID vid;
    cocosession *sess;
    char guid[64];
    int idx, want, auto_port, tries;
    unsigned generation;

    vid = attach_gamepad(PAD_NAME, 0x4301);
    if (!vid) {
        printf("FAIL: the virtual pad would not attach again: %s\n", SDL_GetError());
        failures++;
        return;
    }
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(vid), guid, sizeof guid);

    sess = session_up(config, data);
    if (!sess) { failures++; SDL_DetachVirtualJoystick(vid); return; }
    idx = wait_for_pad(sess, PAD_NAME);
    check(idx >= 0, "persistence: the pad is picked up");
    if (idx < 0) { session_down(sess); SDL_DetachVirtualJoystick(vid); return; }
    check(cocosession_gamepad_assignment(sess, idx) == -1, "a pad nobody assigned is automatic");
    check(strstr(cocosession_get_str(sess, "pad_assign", ""), guid) == NULL, "and has no entry");
    auto_port = cocosession_gamepad_effective_port(sess, idx);
    want = auto_port == 0 ? 1 : 0;

    cocosession_gamepad_assign(sess, idx, want);
    check(cocosession_gamepad_assignment(sess, idx) == want
          && cocosession_gamepad_effective_port(sess, idx) == want, "assigning drives the other joystick");
    check(has_entry(sess, guid, PAD_NAME, want), "and writes <guid>|<name>=<port> to pad_assign");

    /* the file itself, not just the in-memory store */
    {
        char path[600], line[2048];
        int found = 0;
        FILE *file;

        snprintf(path, sizeof path, "%s/settings.ini", config);
        file = fopen(path, "r");
        if (file) {
            while (fgets(line, sizeof line, file))
                if (!strncmp(line, "pad_assign=", 11) && strstr(line, guid))
                    found = 1;
            fclose(file);
        }
        check(found, "the settings file on disk holds it");
    }

    /* a stop and start of the pad thread in the same session (a Preferences change) */
    generation = cocosession_gamepad_generation(sess);
    gamepad_stop(sess);
    check(cocosession_gamepad_generation(sess) >= generation, "stopped: the generation is not lost");
    check(gamepad_start(sess) == 0, "started again");
    idx = wait_for_pad(sess, PAD_NAME);
    check(idx >= 0 && cocosession_gamepad_assignment(sess, idx) == want, "the same session: the assignment is restored");
    check(cocosession_gamepad_generation(sess) > generation,
          "and the generation is above what a frontend remembered, though the pads are the same");

    /* restart: a new session on the same settings */
    session_down(sess);
    sess = session_up(config, data);
    if (!sess) { failures++; SDL_DetachVirtualJoystick(vid); return; }
    idx = wait_for_pad(sess, PAD_NAME);
    check(idx >= 0, "restart: the pad is picked up again");
    if (idx >= 0) {
        check(cocosession_gamepad_assignment(sess, idx) == want, "the assignment is restored on device add");
        check(cocosession_gamepad_effective_port(sess, idx) == want, "and the pad drives that joystick");
    }

    /* a stale GUID (another USB port): the name finds the entry. Rewrite the
     * stored GUID, restart. */
    {
        char text[300];

        snprintf(text, sizeof text, "ffffffffffffffffffffffffffffffff|%s=%d", PAD_NAME, want);
        cocosession_set_str(sess, "pad_assign", text);
    }
    session_down(sess);
    sess = session_up(config, data);
    if (!sess) { failures++; SDL_DetachVirtualJoystick(vid); return; }
    idx = wait_for_pad(sess, PAD_NAME);
    check(idx >= 0 && cocosession_gamepad_assignment(sess, idx) == want,
          "a different GUID with the same name gets the assignment too");

    /* Automatic removes the entry, and the next run starts automatic */
    cocosession_gamepad_assign(sess, idx, want);   /* back under its own GUID */
    check(has_entry(sess, guid, PAD_NAME, want), "assigned under the pad's own GUID again");
    cocosession_gamepad_assign(sess, idx, -1);
    check(cocosession_gamepad_assignment(sess, idx) == -1, "Automatic: no explicit port");
    check(strstr(cocosession_get_str(sess, "pad_assign", ""), PAD_NAME) == NULL,
          "and the entry is gone from pad_assign");
    session_down(sess);
    sess = session_up(config, data);
    if (!sess) { failures++; SDL_DetachVirtualJoystick(vid); return; }
    idx = wait_for_pad(sess, PAD_NAME);
    check(idx >= 0 && cocosession_gamepad_assignment(sess, idx) == -1
          && cocosession_gamepad_effective_port(sess, idx) == auto_port, "after a restart it is automatic again");

    /* garbage in the setting is ignored, silently */
    cocosession_set_str(sess, "pad_assign", ";;|=;zz|x=9;%%%;" "00000000000000000000000000000000|" PAD_NAME "=7");
    session_down(sess);
    sess = session_up(config, data);
    if (!sess) { failures++; SDL_DetachVirtualJoystick(vid); return; }
    idx = wait_for_pad(sess, PAD_NAME);
    check(idx >= 0 && cocosession_gamepad_assignment(sess, idx) == -1,
          "a malformed or out-of-range pad_assign leaves the pad automatic");
    cocosession_gamepad_assign(sess, idx, want);
    check(has_entry(sess, guid, PAD_NAME, want), "and a new assignment still writes");
    cocosession_gamepad_assign(sess, idx, -1);

    /* a hot replug within a run restores it too */
    cocosession_gamepad_assign(sess, idx, want);
    SDL_DetachVirtualJoystick(vid);
    for (tries = 0; tries < 250 && pad_slot_now(sess, PAD_NAME) >= 0; tries++)
        nap_ms(20);
    check(pad_slot_now(sess, PAD_NAME) < 0, "unplugged: the pad is gone");
    vid = attach_gamepad(PAD_NAME, 0x4301);
    idx = vid ? wait_for_pad(sess, PAD_NAME) : -1;
    check(idx >= 0 && cocosession_gamepad_assignment(sess, idx) == want, "unplugged and plugged back in: the port comes back");
    cocosession_gamepad_assign(sess, idx, -1);

    session_down(sess);
    if (vid)
        SDL_DetachVirtualJoystick(vid);
}

int main(void)
{
    char config[512], data[512];
    cocosession *sess;
    SDL_JoystickID vid, rawid = 0, twinid = 0;
    SDL_Joystick *vjoy, *rawjoy = NULL;
    int idx, baseline, tries, diagx, diagy;
    char event[160];

    /* held open for the whole run: a session stopping must not take the
     * virtual pads with it (the persistence tests restart sessions) */
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
        return 1;
    if (test_dirs("gamepad_map_test", config, sizeof config, data, sizeof data) != 0)
        return 1;
    sess = session_up(config, data);
    if (!sess)
        return 1;

    /* Real pads may already be attached: let the first device scan find
     * them, so the virtual one is known by the slot it adds. */
    nap_ms(700);
    baseline = cocosession_gamepad_count(sess);

    vid = attach_gamepad(PAD_NAME, 0x4301);
    if (!vid) {
        printf("SKIP: no virtual joysticks here: %s\n", SDL_GetError());
        session_down(sess);
        return 77;
    }
    vjoy = SDL_OpenJoystick(vid);
    if (!vjoy)
        return 1;

    for (tries = 0; tries < 250 && cocosession_gamepad_count(sess) <= baseline; tries++)
        nap_ms(20);
    vport = -1;
    for (idx = 0; idx < cocosession_gamepad_count(sess); idx++) {
        char name[64];

        cocosession_gamepad_name(sess, idx, name, sizeof name);
        if (strcmp(name, PAD_NAME) == 0)
            vport = cocosession_gamepad_effective_port(sess, idx);
    }
    if (vport < 0 || vport > 1) {
        const int none_new = cocosession_gamepad_count(sess) <= baseline;

        printf("%s: the virtual pad got no joystick (%d; real pads attached?)\n", none_new ? "FAIL" : "SKIP", vport);
        SDL_CloseJoystick(vjoy);
        SDL_DetachVirtualJoystick(vid);
        session_down(sess);
        return none_new ? 1 : 77;
    }
    printf("ok: the virtual pad was picked up as the %s joystick\n", vport ? "left" : "right");

    /* the toast */
    cocosession_gamepad_last_event(sess, event, sizeof event);
    check(strstr(event, "Connected: " PAD_NAME) && strstr(event, vport ? "left joystick" : "right joystick"),
          "the connect toast names the pad and the joystick it drives");
    check(cocosession_gamepad_generation(sess) > 0, "and the generation moved");

    SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_SOUTH, true);
    expect(sess, vport, 0, 0, 1, 0, "South is button 1");
    SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_SOUTH, false);
    expect(sess, vport, 0, 0, 0, 0, "released");
    SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_EAST, true);
    expect(sess, vport, 0, 0, 2, 0, "East is button 2");
    SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_SOUTH, true);
    expect(sess, vport, 0, 0, 3, 0, "both together");
    SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_EAST, false);
    SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_SOUTH, false);
    SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_WEST, true);
    SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_NORTH, true);
    nap_ms(200);
    expect(sess, vport, 0, 0, 0, 0, "West and North fire nothing (the old layer fired a button from them)");
    SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_WEST, false);
    SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_NORTH, false);

    SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true);
    expect(sess, vport, 1000, 0, 0, 0, "D-pad right is full deflection");
    SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
    expect(sess, vport, 1000, -1000, 0, 0, "and up as well: a diagonal");
    SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_DPAD_DOWN, true);
    expect(sess, vport, 1000, 0, 0, 0, "up and down together cancel");
    SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_DPAD_UP, false);
    expect(sess, vport, 1000, 1000, 0, 0, "and down is back once up lets go");
    SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_DPAD_DOWN, false);
    SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, false);
    expect(sess, vport, 0, 0, 0, 0, "released");
    check(cocosession_buttons_held(sess, 1 - vport) == 0, "the other joystick was left alone");

    /* the left stick is proportional: a radial dead zone, the rest rescaled */
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFTX, 16000);
    expect(sess, vport, stick_milli(16000), 0, 0, 0, "half-way is a proportional deflection");
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFTY, -32768);
    stick_pair(16000, -32768, &diagx, &diagy);
    expect(sess, vport, diagx, diagy, 0, 0, "and the other axis to its stop (the dead zone is radial)");
    SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_DPAD_LEFT, true);
    expect(sess, vport, -1000, diagy, 0, 0, "a held D-pad direction is a stronger source than the stick's half deflection");
    SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_DPAD_LEFT, false);
    expect(sess, vport, diagx, diagy, 0, 0, "and the stick is back when it lets go");
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFTX, 0);
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFTY, 0);
    expect(sess, vport, 0, 0, 0, 0, "the stick centred");
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFTX, 3000);          /* 0.09: inside the dead zone */
    nap_ms(200);
    expect(sess, vport, 0, 0, 0, 0, "inside the dead zone it is exactly centred");
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFTX, 0);

    /* with the analog stick off it is an eight-way switch with hysteresis */
    cocosession_set_analog(sess, 0);
    check(cocosession_get_int(sess, "analog_joystick", 1) == 0, "the setting is kept");
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFTX, 20000);
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFTY, -32768);
    expect(sess, vport, 1000, -1000, 0, 0, "analog off: right and up are full deflections");
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFTX, 0);
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFTY, 0);
    expect(sess, vport, 0, 0, 0, 0, "centred");
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFTX, 12000);         /* 0.37: under half */
    nap_ms(200);
    expect(sess, vport, 0, 0, 0, 0, "a third of the way is not enough");
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFTX, 17000);         /* 0.52 */
    expect(sess, vport, 1000, 0, 0, 0, "past half it is");
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFTX, 13000);         /* 0.40: between exit and enter */
    nap_ms(200);
    expect(sess, vport, 1000, 0, 0, 0, "and it holds until it falls below 0.35");
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFTX, 9000);          /* 0.27 */
    expect(sess, vport, 0, 0, 0, 0, "then lets go");
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFTX, 30000);
    expect(sess, vport, 1000, 0, 0, 0, "held right again");
    cocosession_set_analog(sess, 1);
    expect(sess, vport, stick_milli(30000), 0, 0, 0, "switched back to analog while held: proportional at once, nothing left held");
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFTX, 0);
    expect(sess, vport, 0, 0, 0, 0, "centred");

    /* RESET and the debugger stop on pad buttons */
    check(cocosession_binding_get(sess, COCO_TARGET_SWITCH(COCO_SW_RESET)).button == COCO_PAD_BTN_NONE,
          "RESET has no pad button until the user picks one");
    cocosession_binding_set_button(sess, COCO_TARGET_SWITCH(COCO_SW_RESET), COCO_PAD_BTN_START, NULL, 0);
    SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_START, true);
    expect(sess, vport, 0, 0, 0, 1, "Start, bound to RESET, holds it");
    SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_START, false);
    expect(sess, vport, 0, 0, 0, 0, "released");
    cocosession_binding_set_button(sess, COCO_TARGET_SYSACT(COCO_SYSACT_DEBUG_STOP), COCO_PAD_BTN_BACK, NULL, 0);
    SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_BACK, true);
    {
        int act = -1, got = 0, tries2;

        for (tries2 = 0; tries2 < 250 && !got; tries2++) {
            got = cocosession_sysaction_take(sess, &act);
            if (!got) nap_ms(20);
        }
        check(got && act == COCO_SYSACT_DEBUG_STOP, "Back, bound to the debugger stop, posts the action for the UI thread");
    }
    SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_BACK, false);
    nap_ms(100);
    {
        int act = -1;

        check(cocosession_sysaction_take(sess, &act) == 0, "once");
    }
    expect(sess, vport, 0, 0, 0, 0, "and it holds nothing");

    /* a D-pad direction rebound elsewhere stops steering */
    cocosession_binding_set_button(sess, COCO_TARGET_SWITCH(COCO_SW_RESET), COCO_PAD_BTN_DPAD_UP, NULL, 0);
    SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
    expect(sess, vport, 0, 0, 0, 1, "D-pad up, bound to RESET, is RESET and not up any more");
    SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_DPAD_UP, false);
    expect(sess, vport, 0, 0, 0, 0, "released");
    cocosession_bindings_reset(sess);

    /* A trigger past half travel once risked the gamepad thread taking its
     * own lock twice; a hang here is a ctest timeout. */
    cocosession_binding_set_button(sess, COCO_TARGET_PORT(vport, COCO_ACT_BUTTON1), COCO_PAD_BTN_LEFT_TRIGGER, NULL, 0);
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
    expect(sess, vport, 0, 0, 1, 0, "the left trigger presses its binding");
    (void)cocosession_gamepad_generation(sess);
    (void)cocosession_gamepad_effective_port(sess, 0);
    (void)cocosession_gamepad_assignment(sess, 0);
    check(1, "the pad lock is still free with a trigger held");
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0);
    expect(sess, vport, 0, 0, 0, 0, "the left trigger released");
    cocosession_gamepad_capture_begin(sess);
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
    {
        int btn = -1, got = 0, tries2;

        for (tries2 = 0; tries2 < 250 && !got; tries2++) {
            got = cocosession_gamepad_capture_poll(sess, &btn);
            if (!got) nap_ms(20);
        }
        check(got && btn == COCO_PAD_BTN_RIGHT_TRIGGER, "Map mode captures a trigger");
    }
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0);
    cocosession_bindings_reset(sess);

    /* Map mode swallows the press it captures: South maps, and does not also press button 1 */
    cocosession_gamepad_capture_begin(sess);
    SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_SOUTH, true);
    {
        int btn = -1, got = 0, tries2;

        for (tries2 = 0; tries2 < 250 && !got; tries2++) {
            got = cocosession_gamepad_capture_poll(sess, &btn);
            if (!got) nap_ms(20);
        }
        check(got && btn == COCO_PAD_BTN_SOUTH, "Map mode captures a button");
    }
    nap_ms(100);
    expect(sess, vport, 0, 0, 0, 0, "and that press did nothing else");
    SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_SOUTH, false);
    nap_ms(100);
    cocosession_gamepad_capture_begin(sess);
    cocosession_gamepad_capture_cancel(sess);
    check(cocosession_gamepad_capture_poll(sess, NULL) == 0, "a cancelled capture reports nothing");
    SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_SOUTH, true);
    expect(sess, vport, 0, 0, 1, 0, "and the button works again");
    SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_SOUTH, false);
    expect(sess, vport, 0, 0, 0, 0, "released");

    /* a second pad takes the other joystick; assigning crosses them over */
    twinid = attach_gamepad(TWIN_NAME, 0x4302);
    if (twinid) {
        const int twin = wait_for_pad(sess, TWIN_NAME);
        const int first = pad_slot_now(sess, PAD_NAME);

        check(twin >= 0 && first >= 0, "a second pad is picked up");
        if (twin < 0 || first < 0) {
            int dbg;
            for (dbg = 0; dbg < cocosession_gamepad_count(sess); dbg++) {
                char got[64];
                cocosession_gamepad_name(sess, dbg, got, sizeof got);
                printf("   pad %d: %s\n", dbg, got);
            }
        }
        if (twin >= 0 && first >= 0) {
            const int other = cocosession_gamepad_effective_port(sess, twin);

            check(other >= 0 && other != vport, "and drives the other joystick");
            cocosession_gamepad_last_event(sess, event, sizeof event);
            check(strstr(event, "Connected: " TWIN_NAME) != NULL, "with its own toast");
            cocosession_gamepad_assign(sess, first, other);
            check(cocosession_gamepad_effective_port(sess, first) == other
                  && cocosession_gamepad_effective_port(sess, twin) == vport,
                  "assigning the first pad to the second's joystick crosses them over");
            cocosession_gamepad_assign(sess, first, -1);
            check(cocosession_gamepad_effective_port(sess, first) == vport, "and Automatic puts them back");
        }
        SDL_DetachVirtualJoystick(twinid);
        for (tries = 0; tries < 250 && pad_slot_now(sess, TWIN_NAME) >= 0; tries++)
            nap_ms(20);
        check(pad_slot_now(sess, TWIN_NAME) < 0, "the second pad leaves");
        cocosession_gamepad_last_event(sess, event, sizeof event);
        check(strstr(event, "Disconnected: " TWIN_NAME) != NULL, "with a toast");
    }

    /* a plain HID joystick (no gamepad mapping): its hat and buttons */
    {
        SDL_VirtualJoystickDesc desc;

        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_UNKNOWN;
        desc.naxes = 2;
        desc.nbuttons = 4;
        desc.nhats = 1;
        desc.name = RAW_NAME;
        rawid = SDL_AttachVirtualJoystick(&desc);
    }
    if (rawid && !SDL_IsGamepad(rawid)) {
        const int raw = wait_for_pad(sess, RAW_NAME);

        rawjoy = SDL_OpenJoystick(rawid);
        check(raw >= 0 && rawjoy != NULL, "a plain joystick is picked up");
        if (raw >= 0 && rawjoy) {
            const int rport = cocosession_gamepad_effective_port(sess, raw);

            if (rport >= 0) {
                SDL_SetJoystickVirtualHat(rawjoy, 0, SDL_HAT_UP);
                expect(sess, rport, 0, -1000, 0, 0, "its hat 0 steers, digitally");
                SDL_SetJoystickVirtualHat(rawjoy, 0, SDL_HAT_RIGHTDOWN);
                expect(sess, rport, 1000, 1000, 0, 0, "diagonally");
                SDL_SetJoystickVirtualHat(rawjoy, 0, SDL_HAT_CENTERED);
                expect(sess, rport, 0, 0, 0, 0, "and lets go");
                cocosession_binding_set_button(sess, COCO_TARGET_PORT(rport, COCO_ACT_BUTTON2),
                                               COCO_PAD_BTN_RAW_BASE + 2, NULL, 0);
                SDL_SetJoystickVirtualButton(rawjoy, 2, true);
                expect(sess, rport, 0, 0, 2, 0, "button 2 of the raw band presses the button bound to it");
                SDL_SetJoystickVirtualButton(rawjoy, 2, false);
                expect(sess, rport, 0, 0, 0, 0, "released");
                SDL_SetJoystickVirtualAxis(rawjoy, 0, 32767);
                expect(sess, rport, 1000, 0, 0, 0, "its first two axes are the stick");
                SDL_SetJoystickVirtualAxis(rawjoy, 0, 0);
                expect(sess, rport, 0, 0, 0, 0, "centred");
                cocosession_binding_set_button(sess, COCO_TARGET_PORT(rport, COCO_ACT_BUTTON2), COCO_PAD_BTN_EAST, NULL, 0);
            } else {
                printf("ok: (the plain joystick got no joystick: more pads than ports)\n");
            }
        }
        if (rawjoy) SDL_CloseJoystick(rawjoy);
        SDL_DetachVirtualJoystick(rawid);
        for (tries = 0; tries < 250 && pad_slot_now(sess, RAW_NAME) >= 0; tries++)
            nap_ms(20);
    } else if (rawid) {
        SDL_DetachVirtualJoystick(rawid);
    }
    cocosession_bindings_reset(sess);

    /* hot-unplug while held: the stick, a button and RESET are released, not left holding forever */
    cocosession_binding_set_button(sess, COCO_TARGET_SWITCH(COCO_SW_RESET), COCO_PAD_BTN_START, NULL, 0);
    SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true);
    SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_SOUTH, true);
    SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_START, true);
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFTY, -30000);
    expect(sess, vport, 1000, stick_milli(-30000), 1, 1, "held before the unplug");
    SDL_CloseJoystick(vjoy);
    SDL_DetachVirtualJoystick(vid);
    for (tries = 0; tries < 250 && cocosession_gamepad_count(sess) != baseline; tries++)
        nap_ms(20);
    check(cocosession_gamepad_count(sess) == baseline, "the unplugged pad is gone");
    expect(sess, vport, 0, 0, 0, 0, "and its direction, button, analog stick and RESET are released");
    cocosession_gamepad_last_event(sess, event, sizeof event);
    check(strstr(event, "Disconnected: " PAD_NAME) != NULL, "with a toast");
    cocosession_bindings_reset(sess);

    session_down(sess);

    persistence(config, data);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
