/*
 * bindings -- the remappable key/pad-button table, the key router, and every
 * name the bindings editor and the settings store need.
 *
 * One flat target table (cocosession.h's COCO_TARGET_* indices): per joystick
 * (port 0, the RIGHT one, then port 1) the four directions and two fire
 * buttons, then RESET, then the session's own action: 14 in all. Each target
 * holds at most one keysym and one pad button. Rebinding STEALS -- a key
 * drives exactly one target, because one keystroke doing two things is worse
 * than losing the old binding: the second effect is invisible until it
 * matters.
 *
 * Every key of the host keyboard is a CoCo key, so no target has a default
 * key; Map can add some, and a bound key is consumed (it stops going to the
 * machine). The pad defaults are the same on both joysticks: the D-pad
 * steers, South is button 1, East button 2.
 *
 * Seeds its defaults LAZILY so the table is usable with no session behind it:
 * a unit test with no settings store gets the documented default map.
 * Persisted as one packed "bindings" key holding only the entries that differ
 * from the defaults ("<target>.k:<keysym>" / "<target>.b:<button>").
 *
 * Keysyms are X11's, in the canonical form cocosession_bindable_key gives
 * (lower case, un-shifted), so a binding made with 'z' still fires while Shift
 * is held for something else.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "session_internal.h"

typedef struct {
    uint32_t keysym;
    int button;
} bind_slot;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static bind_slot s_table[COCO_TARGET_COUNT];
static bind_slot s_defaults[COCO_TARGET_COUNT];
static int s_seeded;
static cocosession *s_store;   /* whose settings hold "bindings" */

/* ---- defaults ------------------------------------------------------------- */

/* No key has a default (see the file comment). The pad: the D-pad steers
 * (digital, a full deflection; the left stick is the analog source and is not
 * a binding), South presses button 1, East button 2. RESET and the debugger
 * stop are deliberate: a stray Back press must not wipe a running program. */
static void compute_defaults_locked(void)
{
    static const struct { int act; int button; } pad_map[] = {
        { COCO_ACT_RIGHT,   COCO_PAD_BTN_DPAD_RIGHT },
        { COCO_ACT_LEFT,    COCO_PAD_BTN_DPAD_LEFT },
        { COCO_ACT_UP,      COCO_PAD_BTN_DPAD_UP },
        { COCO_ACT_DOWN,    COCO_PAD_BTN_DPAD_DOWN },
        { COCO_ACT_BUTTON1, COCO_PAD_BTN_SOUTH },
        { COCO_ACT_BUTTON2, COCO_PAD_BTN_EAST },
    };
    int tgt, port, idx;

    for (tgt = 0; tgt < COCO_TARGET_COUNT; tgt++) {
        s_defaults[tgt].keysym = 0;
        s_defaults[tgt].button = COCO_PAD_BTN_NONE;
    }
    for (port = 0; port < COCOSESSION_JOYSTICK_PORTS; port++)
        for (idx = 0; idx < COCO_ACT_PER_PORT; idx++)
            s_defaults[COCO_TARGET_PORT(port, pad_map[idx].act)].button = pad_map[idx].button;
}

/* ---- persistence ---------------------------------------------------------- */

static void apply_persisted_locked(const char *packed)
{
    const char *cur = packed;

    while (cur && *cur) {
        int target = -1;
        char kind = 0;
        long value = 0;
        const char *end = strchr(cur, ' ');

        if (sscanf(cur, "%d.%c:%ld", &target, &kind, &value) == 3
            && target >= 0 && target < COCO_TARGET_COUNT) {
            if (kind == 'k' && value >= 0)
                s_table[target].keysym = cocosession_bindable_key((uint32_t)value, 0);
            else if (kind == 'b')
                s_table[target].button = (int)value;
        }
        if (!end)
            break;
        cur = end + 1;
    }
}

static void pack_locked(char *out, size_t outsz)
{
    size_t len = 0;
    int tgt;

    out[0] = '\0';
    for (tgt = 0; tgt < COCO_TARGET_COUNT; tgt++) {
        if (s_table[tgt].keysym != s_defaults[tgt].keysym)
            len += (size_t)snprintf(out + len, outsz - len, "%s%d.k:%u",
                                    len ? " " : "", tgt, s_table[tgt].keysym);
        if (len >= outsz)
            break;
        if (s_table[tgt].button != s_defaults[tgt].button)
            len += (size_t)snprintf(out + len, outsz - len, "%s%d.b:%d",
                                    len ? " " : "", tgt, s_table[tgt].button);
        if (len >= outsz)
            break;
    }
}

static void persist_locked(void)
{
    char packed[2048];

    if (!s_store)
        return;
    pack_locked(packed, sizeof packed);
    cocosession_set_str(s_store, "bindings", packed);
}

static void ensure_seeded_locked(void)
{
    if (s_seeded)
        return;
    compute_defaults_locked();
    memcpy(s_table, s_defaults, sizeof s_table);
    s_seeded = 1;
}

void bindings_init(cocosession *sess)
{
    pthread_mutex_lock(&s_lock);
    compute_defaults_locked();
    memcpy(s_table, s_defaults, sizeof s_table);
    s_seeded = 1;
    s_store = sess;
    if (sess)
        apply_persisted_locked(cocosession_get_str(sess, "bindings", ""));
    pthread_mutex_unlock(&s_lock);
}

/* The table is process-wide: a freed session must not stay its store. */
void bindings_detach(cocosession *sess)
{
    pthread_mutex_lock(&s_lock);
    if (s_store == sess)
        s_store = NULL;
    pthread_mutex_unlock(&s_lock);
}

/* ---- lookups -------------------------------------------------------------- */

static int target_for_key_locked(uint32_t keysym)
{
    int tgt;

    if (!keysym)
        return -1;
    for (tgt = 0; tgt < COCO_TARGET_COUNT; tgt++)
        if (s_table[tgt].keysym == keysym)
            return tgt;
    return -1;
}

int cocosession_target_for_key(cocosession *sess, uint32_t keysym)
{
    int tgt;

    (void)sess;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    tgt = target_for_key_locked(cocosession_bindable_key(keysym, 0));
    pthread_mutex_unlock(&s_lock);
    return tgt;
}

/* Pad buttons are scoped to the pad's port: the same button is free to mean
 * different things on different joysticks, so only that port's targets and the
 * machine-wide ones (RESET, the actions) are searched. */
int cocosession_target_for_button(cocosession *sess, int port, int button)
{
    int tgt, found = -1;

    (void)sess;
    if (button == COCO_PAD_BTN_NONE)
        return -1;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    if (port < 0 || port >= COCOSESSION_JOYSTICK_PORTS)
        port = 0;
    for (tgt = 0; tgt < COCO_ACT_PER_PORT; tgt++)
        if (s_table[COCO_TARGET_PORT(port, tgt)].button == button) {
            found = COCO_TARGET_PORT(port, tgt);
            break;
        }
    if (found < 0)
        for (tgt = COCO_TARGET_SWITCH(0); tgt < COCO_TARGET_COUNT; tgt++)
            if (s_table[tgt].button == button) {
                found = tgt;
                break;
            }
    pthread_mutex_unlock(&s_lock);
    return found;
}

coco_binding cocosession_binding_get(cocosession *sess, int target)
{
    coco_binding bind = { 0, COCO_PAD_BTN_NONE };

    (void)sess;
    if (target < 0 || target >= COCO_TARGET_COUNT)
        return bind;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    bind.keysym = s_table[target].keysym;
    bind.button = s_table[target].button;
    pthread_mutex_unlock(&s_lock);
    return bind;
}

/* A key may legitimately drive several targets (hand-edited settings). An
 * explicit rebinding is the user saying "this key does exactly this", so it
 * displaces every holder -- and names them all, comma-separated, so nothing
 * is lost silently. */
static void describe_stolen(const int *victims, int nvictims, char *stolen, int stolensz)
{
    int idx, len = 0;

    if (!stolen || stolensz <= 0)
        return;
    stolen[0] = '\0';
    for (idx = 0; idx < nvictims && len < stolensz; idx++)
        len += snprintf(stolen + len, (size_t)(stolensz - len), "%s%s",
                        idx ? ", " : "", coco_target_name(victims[idx]));
}

void cocosession_binding_set_key(cocosession *sess, int target, uint32_t keysym,
                                 char *stolen, int stolensz)
{
    int victims[COCO_TARGET_COUNT], nvictims = 0, tgt;
    uint32_t canon;

    (void)sess;
    if (stolen && stolensz > 0)
        stolen[0] = '\0';
    if (target < 0 || target >= COCO_TARGET_COUNT)
        return;
    canon = cocosession_bindable_key(keysym, 0);
    if (keysym && !canon)
        return;                      /* a key that cannot be bound */
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    if (canon) {
        for (tgt = 0; tgt < COCO_TARGET_COUNT; tgt++)
            if (tgt != target && s_table[tgt].keysym == canon) {
                s_table[tgt].keysym = 0;
                victims[nvictims++] = tgt;
            }
    }
    s_table[target].keysym = canon;
    persist_locked();
    pthread_mutex_unlock(&s_lock);
    describe_stolen(victims, nvictims, stolen, stolensz);
}

void cocosession_binding_set_button(cocosession *sess, int target, int button,
                                    char *stolen, int stolensz)
{
    int victims[COCO_TARGET_COUNT], nvictims = 0, tgt;
    int port;   /* COCOSESSION_JOYSTICK_PORTS and up: RESET, the actions */

    (void)sess;
    if (stolen && stolensz > 0)
        stolen[0] = '\0';
    if (target < 0 || target >= COCO_TARGET_COUNT)
        return;
    port = target / COCO_ACT_PER_PORT;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    if (button != COCO_PAD_BTN_NONE) {
        for (tgt = 0; tgt < COCO_TARGET_COUNT; tgt++) {
            const int tport = tgt / COCO_ACT_PER_PORT;

            /* steal only within the same scope: this port, or the
             * machine-wide targets, which every pad reaches */
            if (tgt != target && s_table[tgt].button == button
                && (tport == port || tport >= COCOSESSION_JOYSTICK_PORTS
                    || port >= COCOSESSION_JOYSTICK_PORTS)) {
                s_table[tgt].button = COCO_PAD_BTN_NONE;
                victims[nvictims++] = tgt;
            }
        }
    }
    s_table[target].button = button;
    persist_locked();
    pthread_mutex_unlock(&s_lock);
    describe_stolen(victims, nvictims, stolen, stolensz);
}

void cocosession_bindings_reset(cocosession *sess)
{
    (void)sess;
    pthread_mutex_lock(&s_lock);
    compute_defaults_locked();
    memcpy(s_table, s_defaults, sizeof s_table);
    s_seeded = 1;
    if (s_store)
        cocosession_set_str(s_store, "bindings", "");
    pthread_mutex_unlock(&s_lock);
}

/* ---- names ---------------------------------------------------------------- */

static const char *const act_names[COCO_ACT_PER_PORT] = {
    "Right", "Left", "Up", "Down", "Button 1", "Button 2",
};
static const char *const port_names[COCOSESSION_JOYSTICK_PORTS] = {
    "Right joystick", "Left joystick",
};
static const char *const switch_names[COCO_SW_COUNT] = {
    "RESET",
};
static const char *const sysact_names[COCO_SYSACT_COUNT] = {
    "Debugger Stop",
};

const char *coco_target_name(int target)
{
    static char buf[64];

    if (target < 0 || target >= COCO_TARGET_COUNT)
        return "";
    if (target < COCO_TARGET_SWITCH(0)) {
        snprintf(buf, sizeof buf, "%s: %s", port_names[target / COCO_ACT_PER_PORT],
                 act_names[target % COCO_ACT_PER_PORT]);
        return buf;
    }
    if (target < COCO_TARGET_SYSACT(0))
        return switch_names[target - COCO_TARGET_SWITCH(0)];
    return sysact_names[target - COCO_TARGET_SYSACT(0)];
}

const char *coco_target_short_name(int target)
{
    if (target < 0 || target >= COCO_TARGET_COUNT)
        return "";
    if (target < COCO_TARGET_SWITCH(0))
        return act_names[target % COCO_ACT_PER_PORT];
    if (target < COCO_TARGET_SYSACT(0))
        return switch_names[target - COCO_TARGET_SWITCH(0)];
    return sysact_names[target - COCO_TARGET_SYSACT(0)];
}

static const char *const pad_button_names[COCO_PAD_BTN_COUNT] = {
    "A", "B", "X", "Y", "Back", "Guide", "Start", "Left Stick", "Right Stick",
    "Left Shoulder", "Right Shoulder", "D-pad Up", "D-pad Down", "D-pad Left",
    "D-pad Right", "Left Trigger", "Right Trigger",
};

const char *coco_pad_button_name(int button)
{
    static char buf[32];

    if (COCO_PAD_BTN_IS_NAMED(button))
        return pad_button_names[button];
    if (COCO_PAD_BTN_IS_RAW(button)) {
        snprintf(buf, sizeof buf, "Button %d", button - COCO_PAD_BTN_RAW_BASE);
        return buf;
    }
    if (COCO_PAD_BTN_IS_HAT(button)) {
        static const char *const dirs[COCO_PAD_HAT_DIRS] =
            { "Up", "Up-Right", "Right", "Down-Right", "Down", "Down-Left",
              "Left", "Up-Left" };
        const int hat = (button - COCO_PAD_HAT_BASE) / COCO_PAD_HAT_DIRS;
        const int dir = (button - COCO_PAD_HAT_BASE) % COCO_PAD_HAT_DIRS;

        snprintf(buf, sizeof buf, "Hat %d %s", hat, dirs[dir]);
        return buf;
    }
    return "";
}

/* ---- the key router ---------------------------------------------------------- */

/* The first stage of every host key: a bound key drives its targets and is
 * consumed; anything else goes to the machine. */
int cocosession_key(cocosession *sess, int down, uint32_t keysym, uint32_t unicode,
                    int ctrl_down, int shift_down)
{
    const uint32_t bound = cocosession_bindable_key(keysym, unicode);
    int hit = 0, tgt;

    if (!sess)
        return 0;
    if (bound) {
        pthread_mutex_lock(&s_lock);
        ensure_seeded_locked();
        for (tgt = 0; tgt < COCO_TARGET_COUNT; tgt++) {
            if (down) {
                if (s_table[tgt].keysym != bound)
                    continue;
                hit = 1;
                if (sess->held_keysym[tgt] == bound)
                    continue;                 /* auto-repeat */
                sess->held_keysym[tgt] = bound;
            } else {
                /* remembered by the key, so a release clears exactly what its
                 * press asserted even if the binding changed in between */
                if (sess->held_keysym[tgt] != bound)
                    continue;
                hit = 1;
                sess->held_keysym[tgt] = 0;
            }
            pthread_mutex_unlock(&s_lock);
            if (tgt >= COCO_TARGET_SYSACT(0)) {
                /* an action is the UI thread's to run, from its timer */
                if (down)
                    cocosession_sysaction_post(sess, tgt - COCO_TARGET_SYSACT(0));
            } else {
                cocosession_press(sess, tgt, down);
            }
            pthread_mutex_lock(&s_lock);
        }
        pthread_mutex_unlock(&s_lock);
    }
    if (hit)
        return 1;
    /* a key pressed before it was bound and released after comes here too,
     * so the machine does not keep it */
    if (!sess->running)
        return 0;
    session_physical_key(sess, down, keysym, unicode, ctrl_down, shift_down);
    return 0;
}
