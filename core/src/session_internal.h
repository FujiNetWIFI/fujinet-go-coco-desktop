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

    /* ---- FujiNet runtime (fujinet_runtime.c) ---- */
    int fujinet_loaded;  /* dlopen'd (kept for process lifetime) */
    int fujinet_running;

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

/* debugger.c -- all emulator-thread side except create/destroy. */
cocodebug *cocodebug_create(cocosession *s);
void cocodebug_destroy(cocodebug *d);
/* One debugger-controlled frame slice; returns 1 when a frame's worth of
 * cycles ran. May block while paused. */
int  cocodebug_session_frame(cocosession *s);
/* Unpark a paused debugger so a stopping emulator thread can exit. */
void cocodebug_resume_for_stop(cocodebug *d);

/* fujinet_runtime.c */
int  fujinet_start(cocosession *s);
void fujinet_stop(cocosession *s);
int  fujinet_copy_log(cocosession *s, char *dst, int max);
void fujinet_mix_audio(cocosession *s, int16_t *buf, int nsamples, int rate);

#endif /* COCOSESSION_INTERNAL_H */
