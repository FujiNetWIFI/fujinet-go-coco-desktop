/*
 * cocosession gamepad backend: SDL3 gamepad subsystem on a dedicated
 * polling thread. Hotplug is automatic (SDL's udev monitor runs inside
 * SDL_UpdateGamepads); the mapping mirrors the Android GameControllerMapper:
 * left stick PROPORTIONAL with d-pad fallback, 0.18 deadzone rescaled to full
 * travel, A/X -> button 0, B/Y -> button 1 (the second button exists on a
 * CoCo 3 joystick).
 *
 * The CoCo has two analog joystick ports, so up to two pads drive the machine
 * at once: by default the first connected pad takes the right port (which is
 * the one nearly every game reads) and the second takes the left. A pad can be
 * bound to a port explicitly, and -1 puts it back under automatic assignment.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <SDL3/SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat.h"
#include "session_internal.h"

#define MAX_PADS 4
#define NUM_PORTS COCOSESSION_JOYSTICK_PORTS
/* Matches the Android GameControllerMapper. Applied by coco_axis_from_stick,
 * which rescales the live travel so the joystick can still reach its stops. */
#define DEADZONE 0.18f

typedef struct {
    SDL_Gamepad *pad;
    SDL_JoystickID id;
    int port;          /* explicit binding, or -1 for automatic */
} pad_slot;

typedef struct {
    float x, y;
    int btn0, btn1;
} pad_reading;

typedef struct {
    pthread_t thread;
    volatile int run;
    pthread_mutex_t mtx;
    pad_slot pads[MAX_PADS];
    int count;
    pad_reading last[NUM_PORTS]; /* last pushed, to skip redundant updates */
    int have_last[NUM_PORTS];
} gamepad_state;

static void read_pad(SDL_Gamepad *pad, pad_reading *out)
{
    float ax = (float)SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
    float ay = (float)SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;

    /* Deadzone only; the session converts -1..1 to the CoCo's DAC range, so
     * both paths through cocosession_joystick agree on the geometry. */
    out->x = ax;
    out->y = ay;
    if (out->x > -DEADZONE && out->x < DEADZONE) out->x = 0.0f;
    if (out->y > -DEADZONE && out->y < DEADZONE) out->y = 0.0f;

    /* D-pad fallback: only when the stick is centred, so a pad with both
     * cannot fight itself. Digital input drives the joystick to its stop. */
    if (out->x == 0.0f) {
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT))
            out->x = -1.0f;
        else if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT))
            out->x = 1.0f;
    }
    if (out->y == 0.0f) {
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP))
            out->y = -1.0f;
        else if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN))
            out->y = 1.0f;
    }

    out->btn0 = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH) ||
                SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_WEST);
    out->btn1 = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST) ||
                SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_NORTH);
}

/* Which pad drives `port`. The rules live in coco_pad_for_port (input_map.c)
 * so they can be tested without hardware; this just hands over the current
 * bindings. Caller holds g->mtx. */
static int pad_for_port(gamepad_state *g, int port)
{
    int bindings[MAX_PADS];
    int i;

    for (i = 0; i < g->count; i++)
        bindings[i] = g->pads[i].port;
    return coco_pad_for_port(bindings, g->count, port);
}

static void sync_devices(cocosession *s, gamepad_state *g)
{
    int i, n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);

    pthread_mutex_lock(&g->mtx);
    /* Drop pads that vanished. */
    for (i = 0; i < g->count;) {
        int present = 0, j;
        for (j = 0; j < n; j++)
            if (ids && ids[j] == g->pads[i].id) present = 1;
        if (!present) {
            SDL_CloseGamepad(g->pads[i].pad);
            memmove(&g->pads[i], &g->pads[i + 1],
                    (size_t)(g->count - i - 1) * sizeof(pad_slot));
            g->count--;
            /* The remaining pads may have shifted onto different ports, and
             * the vanished one may have been holding a direction. */
            g->have_last[0] = g->have_last[1] = 0;
        } else {
            i++;
        }
    }
    /* Open pads that appeared. */
    for (i = 0; ids && i < n && g->count < MAX_PADS; i++) {
        int j, known = 0;
        for (j = 0; j < g->count; j++)
            if (g->pads[j].id == ids[i]) known = 1;
        if (!known) {
            SDL_Gamepad *pad = SDL_OpenGamepad(ids[i]);
            if (pad) {
                g->pads[g->count].pad = pad;
                g->pads[g->count].id = ids[i];
                g->pads[g->count].port = -1;
                g->count++;
            }
        }
    }
    pthread_mutex_unlock(&g->mtx);
    SDL_free(ids);
    (void)s;
}

static void *gamepad_thread_main(void *arg)
{
    cocosession *s = arg;
    gamepad_state *g = s->gamepad;
    int tick = 0;

    coco_thread_setname("coco-gamepad");
    while (g->run) {
        int port;
        SDL_UpdateGamepads();
        /* Device add/remove is rare; re-enumerate at ~4Hz, poll state at
         * ~125Hz. Flush the (unused) event queue so it can't grow. */
        if (tick++ % 32 == 0)
            sync_devices(s, g);
        SDL_PumpEvents();
        SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);

        for (port = 0; port < NUM_PORTS; port++) {
            pad_reading r;
            int idx, changed, b0, b1, b0_was, b1_was;

            pthread_mutex_lock(&g->mtx);
            idx = pad_for_port(g, port);
            if (idx < 0) {
                pthread_mutex_unlock(&g->mtx);
                continue;
            }
            read_pad(g->pads[idx].pad, &r);
            changed = !g->have_last[port] ||
                      r.x != g->last[port].x || r.y != g->last[port].y ||
                      r.btn0 != g->last[port].btn0 ||
                      r.btn1 != g->last[port].btn1;
            b0 = r.btn0;
            b1 = r.btn1;
            b0_was = g->have_last[port] ? g->last[port].btn0 : 0;
            b1_was = g->have_last[port] ? g->last[port].btn1 : 0;
            if (changed) {
                g->last[port] = r;
                g->have_last[port] = 1;
            }
            pthread_mutex_unlock(&g->mtx);

            /* Pushed outside the lock: the session's input path is its own
             * concern and must not be serialised behind pad polling. */
            if (changed) {
                cocosession_joystick(s, port, r.x, r.y);
                if (b0 != b0_was) cocosession_joystick_button(s, port, 0, b0);
                if (b1 != b1_was) cocosession_joystick_button(s, port, 1, b1);
            }
        }
        SDL_Delay(8);
    }
    return NULL;
}

int gamepad_start(cocosession *s)
{
    gamepad_state *g;

    if (s->gamepad) return 0;
    /* The app owns its signals; SDL must not intercept SIGINT/SIGTERM. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        session_set_error(s, "SDL gamepad init failed: %s", SDL_GetError());
        return -1;
    }
    g = calloc(1, sizeof(*g));
    if (!g) return -1;
    pthread_mutex_init(&g->mtx, NULL);
    g->run = 1;
    s->gamepad = g;
    if (pthread_create(&g->thread, NULL, gamepad_thread_main, s) != 0) {
        s->gamepad = NULL;
        free(g);
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        return -1;
    }
    return 0;
}

void gamepad_stop(cocosession *s)
{
    gamepad_state *g = s->gamepad;
    int i;
    if (!g) return;
    g->run = 0;
    pthread_join(g->thread, NULL);
    for (i = 0; i < g->count; i++)
        SDL_CloseGamepad(g->pads[i].pad);
    pthread_mutex_destroy(&g->mtx);
    s->gamepad = NULL;
    free(g);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

int cocosession_gamepad_count(cocosession *s)
{
    gamepad_state *g = s->gamepad;
    int n;
    if (!g) return 0;
    pthread_mutex_lock(&g->mtx);
    n = g->count;
    pthread_mutex_unlock(&g->mtx);
    return n;
}

int cocosession_gamepad_name(cocosession *s, int idx, char *dst, int dstsz)
{
    gamepad_state *g = s->gamepad;
    const char *name = NULL;
    if (!g || dstsz <= 0) return 0;
    pthread_mutex_lock(&g->mtx);
    if (idx >= 0 && idx < g->count)
        name = SDL_GetGamepadName(g->pads[idx].pad);
    snprintf(dst, (size_t)dstsz, "%s", name ? name : "");
    pthread_mutex_unlock(&g->mtx);
    return (int)strlen(dst);
}

void cocosession_gamepad_assign(cocosession *s, int idx, int port)
{
    gamepad_state *g = s->gamepad;
    int i;
    if (!g) return;
    pthread_mutex_lock(&g->mtx);
    if (idx >= 0 && idx < g->count) {
        if (port >= 0 && port < NUM_PORTS) {
            /* One pad per port: whoever held it goes back to automatic. */
            for (i = 0; i < g->count; i++)
                if (g->pads[i].port == port) g->pads[i].port = -1;
            g->pads[idx].port = port;
        } else {
            g->pads[idx].port = -1;
        }
    }
    /* Force the next poll to push on both ports, so a newly selected pad takes
     * effect even if it happens to be reading exactly what the old one did. */
    g->have_last[0] = g->have_last[1] = 0;
    pthread_mutex_unlock(&g->mtx);
}
