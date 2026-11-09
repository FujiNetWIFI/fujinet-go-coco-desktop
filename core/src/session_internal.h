/*
 * cocosession internals shared between the session, audio, gamepad, FujiNet
 * and debugger translation units. Nothing here is API; frontends must only
 * include cocosession.h / cocodebug.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef COCOSESSION_INTERNAL_H
#define COCOSESSION_INTERNAL_H

#include <pthread.h>
#include <stdint.h>

#include "cocosession.h"

#define COCO_PATH_MAX 1024

/* Sources of a held input (see struct cocosession): the host keyboard and the
 * Controllers window's buttons, each pad's buttons and its stick (by the
 * stable owner number its slot was given, 0 to COCO_MAX_PADS - 1), and the
 * pointer's drag. */
#define COCO_MAX_PADS 8
#define SRC_KEYBOARD     1u
#define SRC_PAD(owner)   (1u << (1 + (owner)))
#define SRC_STICK(owner) (1u << (1 + COCO_MAX_PADS + (owner)))
#define SRC_POINTER      (1u << (1 + 2 * COCO_MAX_PADS))

/* key_src bits: who holds a machine key down */
#define KEYSRC_PHYSICAL 1u    /* the host keyboard */
#define KEYSRC_CAP      2u    /* the Controllers window's keyboard */

#define COCO_FB_MAX_PIXELS \
    (COCOSESSION_FB_MAX_WIDTH * COCOSESSION_FB_MAX_HEIGHT)

typedef struct setting_kv {
    char *key;
    char *val;
    struct setting_kv *next;
} setting_kv;

struct cocosession {
    /* ---- resolved paths ---- */
    char config_dir[COCO_PATH_MAX];
    char data_dir[COCO_PATH_MAX];
    char roms_dir[COCO_PATH_MAX];  /* XRoar's -rompath */
    char carts_dir[COCO_PATH_MAX]; /* imported cartridge images */
    char settings_file[COCO_PATH_MAX];
    char fujinet_lib[COCO_PATH_MAX];  /* "" = unavailable/disabled */
    char fujinet_src[COCO_PATH_MAX];  /* provisioning source override */
    char fujinet_root[COCO_PATH_MAX];
    char fujinet_config[COCO_PATH_MAX];
    char fujinet_sd[COCO_PATH_MAX];
    char fujinet_data[COCO_PATH_MAX];
    char webui_url[64];

    /* ---- settings store ---- */
    pthread_mutex_t settings_mtx;
    setting_kv *settings;
    int settings_dirty;

    /* ---- lifecycle ---- */
    pthread_mutex_t lifecycle_mtx;
    pthread_t emu_thread;
    int emu_thread_started;
    volatile int stop_flag;
    volatile int running;
    /* The options this session was started with. Unlike the other ports these
     * are all plain ints, so nothing here owns a string on opts' behalf. */
    cocosession_start_opts opts;

    /* Live machine/video state. cocohost owns the pending-command queue that
     * actually applies these on the emulator thread; these are what the UI
     * asked for, so a frontend can read back what it last set without racing
     * the emulator. */
    volatile int cur_machine;
    volatile int cur_tv_input;
    volatile int cur_ccr;
    volatile int cur_cpu;

    /* Handshake: the emulator thread publishes whether the core came up, so
     * cocosession_start can report a failed xroar_init to its caller rather
     * than returning success and going quiet. 0 = pending, 1 = up,
     * -1 = failed. */
    pthread_mutex_t start_mtx;
    pthread_cond_t start_cv;
    int core_started;

    /* ---- latest-frame store (emulator thread -> UI thread) ----
     * Sized for the largest viewport XRoar can produce; frame_w/frame_h say
     * how much of it is live, and change at runtime with the video mode. */
    pthread_mutex_t frame_mtx;
    uint32_t *frame; /* COCO_FB_MAX_PIXELS XRGB8888, heap: ~1MB */
    int frame_w;
    int frame_h;
    uint64_t frame_serial;
    /* frame-ready callback; cb_mtx is held across the call so set(NULL)
     * waits for one in flight */
    pthread_mutex_t cb_mtx;
    cocosession_frame_cb frame_cb;
    void *frame_cb_ctx;
    volatile uint64_t frame_count;   /* frames the machine has run */

    /* ---- vsync phase-lock (UI tick -> emulator pacing) ---- */
    pthread_mutex_t vs_mtx;
    pthread_cond_t vs_cv; /* CLOCK_MONOTONIC */
    int64_t vs_ns;        /* latest tick time */
    long vs_iv_us;        /* most recent tick interval */
    int64_t vs_seen_ns;   /* last tick the emulator paced on */
    /* The machine's frame rate in milli-Hz, as XRoar reports it (59940 NTSC,
     * 50000 PAL); 0 until the first frame. A 50Hz machine cannot phase-lock to
     * a 60Hz display, so the pacer checks this before engaging. */
    volatile int frame_rate_mhz;

    /* ---- backends ---- */
    void *audio;   /* audio_sdl.c state */
    void *gamepad; /* gamepad_sdl.c state */
    volatile int audio_mute;

    /* ---- input: what drives the joysticks, RESET and the machine's keys ----
     * One bit per source (SRC_*) for every digital contact, and one analog
     * deflection per source, so letting go of one source never releases
     * another's. input_mtx covers all of it, and the calls that hand the
     * merged result to the host. held_keysym is the UI thread's alone. */
    pthread_mutex_t input_mtx;
    uint32_t dir_src[COCOSESSION_JOYSTICK_PORTS][4];  /* RIGHT LEFT UP DOWN */
    uint32_t btn_src[COCOSESSION_JOYSTICK_PORTS][2];  /* BUTTON1 BUTTON2 */
    uint32_t sw_src[COCO_SW_COUNT];
    /* analog deflection (milli-units) per source; [COCO_MAX_PADS] is the
     * pointer */
    int16_t an_x[COCO_MAX_PADS + 1][COCOSESSION_JOYSTICK_PORTS];
    int16_t an_y[COCO_MAX_PADS + 1][COCOSESSION_JOYSTICK_PORTS];
    /* the machine's keys by HID usage: KEYSRC_* bits; and the modifier caps
     * latched on in the Controllers window */
    uint8_t key_src[256];
    uint8_t cap_latch[256 / 8];
    /* keys the keyboard currently holds, by target, so a release clears
     * exactly what its press asserted even if the binding changed meanwhile */
    uint32_t held_keysym[COCO_TARGET_COUNT];

    /* cross-thread system-action latch (see cocosession_sysaction_post) */
    pthread_mutex_t sysact_mtx;
    unsigned sysact_pending;
    /* the last gamepad hot-plug event, for a frontend toast */
    pthread_mutex_t pad_event_mtx;
    char pad_event[160];
    /* the gamepad generation, carried across session restarts so it only
     * ever increases */
    unsigned pad_gen_base;

    /* ---- FujiNet runtime (fujinet_runtime.c) ---- */
    int fujinet_loaded;  /* dlopen'd (kept for process lifetime) */
    int fujinet_running;
    int fujinet_external;      /* connecting to a FujiNet that isn't ours */
    char becker_host[64];      /* where the Becker connection goes */
    int becker_port;
    char becker_endpoint[80];  /* "host:port" for display */

    /* ---- debugger ---- */
    cocodebug *debugger;
    volatile int dbg_engaged; /* emulator loop runs the stepped path */

    char last_error[512];
};

/* session.c */
void session_set_error(cocosession *s, const char *fmt, ...);

/* settings.c */
void settings_init(cocosession *s);
void settings_free_all(cocosession *s);

/* paths.c */
int  paths_init(cocosession *s, const cocosession_paths *p);
int  paths_provision_fujinet(cocosession *s); /* fills fujinet_* or "" */
void paths_materialise_roms(cocosession *s);  /* embedded ROMs -> roms_dir */
int  roms_present_for_machine(const char *dir, int machine);
int  mkdir_p(const char *path);
int  copy_file(const char *src, const char *dst);

/* audio_sdl.c */
int  audio_start(cocosession *s);
void audio_stop(cocosession *s);

/* gamepad_sdl.c */
int  gamepad_start(cocosession *s);
void gamepad_stop(cocosession *s);
/* Called by the gamepad thread for the pad that owns `owner`. press: a pad
 * button drives a target (a direction, a fire button, RESET) -- the pad's
 * SRC_PAD source. stick: a digital stick or hat drives one direction -- its
 * SRC_STICK source. analog: the pad's analog stick deflection on a port,
 * milli-units (0, 0 releases it). */
void session_gamepad_press(cocosession *sess, int owner, int target, int down);
void session_gamepad_stick(cocosession *sess, int owner, int port, int act, int down);
void session_gamepad_analog(cocosession *sess, int owner, int port, int xaxis, int yaxis);
/* Called by the gamepad thread on a connect/disconnect. */
void session_gamepad_event(cocosession *sess, const char *text);

/* input.c */
void input_init(cocosession *sess);
/* A start: nothing is held, the joysticks are centred and the machine told. */
void input_clear(cocosession *sess);
/* The host keyboard's key, after bindings declined it, onto the machine. */
void session_physical_key(cocosession *sess, int down, uint32_t keysym, uint32_t unicode,
                          int ctrl_down, int shift_down);
/* bindings.c: reloads the table from s's settings; unhooks s on free */
void bindings_init(cocosession *sess);
void bindings_detach(cocosession *sess);

/* debugger.c -- all emulator-thread side except create/destroy and
 * trace_install. */
cocodebug *cocodebug_create(cocosession *s);
void cocodebug_destroy(cocodebug *d);
/* One debugger-controlled frame slice; returns 1 when a frame's worth of
 * cycles ran. May block while paused. */
int  cocodebug_session_frame(cocosession *s);
/* Unpark a paused debugger so a stopping emulator thread can exit. */
void cocodebug_resume_for_stop(cocodebug *d);
/* Point the Becker port's tap at d's FujiNet trace. UI thread, before the
 * emulator thread starts. */
void cocodebug_trace_install(cocodebug *d);

/* fujinet_runtime.c */
int  fujinet_start(cocosession *s);
/* Block until FujiNet is actually accepting on the Becker port. Returns 0
   when it is, -1 on timeout. See the definition for why start is not
   enough. */
int  fujinet_wait_for_becker(cocosession *s, int timeout_ms);
void fujinet_stop(cocosession *s);
int  fujinet_copy_log(cocosession *s, char *dst, int max);
void fujinet_mix_audio(cocosession *s, int16_t *buf, int nsamples, int rate);

#endif /* COCOSESSION_INTERNAL_H */
