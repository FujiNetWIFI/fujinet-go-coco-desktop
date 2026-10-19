/*
 * Desktop host for the XRoar Tandy Color Computer core.
 *
 * The session statically links a headless XRoar build. This host drives the
 * core via xroar_init()/xroar_run() on the emulator thread and owns the shared
 * state the custom XRoar modules talk to:
 *
 *   * ui_desktop / vo (core/xroar/desktop/ui_desktop.c) calls
 *     cocohost_emit_frame() once per emulated frame with the rendered
 *     XRGB8888 viewport.
 *   * ao_desktop (core/xroar/desktop/ao_desktop.c) calls cocohost_push_audio()
 *     with completed stereo S16 buffers; the session's audio backend drains
 *     them with cocohost_fill_audio().
 *   * joystick_desktop (core/xroar/desktop/joystick_desktop.c) reads the
 *     per-port analog axis / button state set by the session via
 *     cocohost_set_joystick_*.
 *
 * Keyboard, reset, machine and TV-input changes are queued and applied on the
 * emulator thread at the top of cocohost_core_run_frame(); XRoar is
 * single-threaded and none of it is safe to touch from a UI thread.
 *
 * This file is the Android app's coco_host.h with the Android surface and log
 * dependencies replaced by the frame sink and log sink below. Everything else
 * -- the command queue, the audio ring, the joystick atomics, the in-place
 * CoCo 2 <-> CoCo 3 switch -- is carried over unchanged, because none of it
 * was Android-specific in the first place.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef COCO_HOST_H
#define COCO_HOST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Machine selection (also picks the matching HDB-DOS Becker ROM). */
enum {
    COCO_MACHINE_COCO3 = 0,  /* xroar -machine coco3,    becker ROM hdbdw3bc3 */
    COCO_MACHINE_COCO2 = 1,  /* xroar -machine coco2bus, becker ROM hdbdw3bck */
};

/* CPU selection (xroar -machine-cpu). Independent of machine: both machine
 * configs support either core. */
enum {
    COCO_CPU_MC6809 = 0,
    COCO_CPU_HD6309 = 1,
};

/* TV input / artifact mode (values match XRoar TV_INPUT_*). */
enum {
    COCO_TV_SVIDEO = 0,        /* S-Video: clean, no artifact colours */
    COCO_TV_COMPOSITE_BR = 1,  /* Composite, blue-red artifact phase */
    COCO_TV_COMPOSITE_RB = 2,  /* Composite, red-blue artifact phase */
    COCO_TV_RGB = 3,           /* RGB (CoCo 3) */
};

/* Composite cross-colour (artifact) renderer (matches XRoar VO_CMP_CCR_*). */
enum {
    COCO_CCR_NONE = 0,
    COCO_CCR_SIMPLE = 1,
    COCO_CCR_5BIT = 2,
    COCO_CCR_PARTIAL = 3,
    COCO_CCR_SIMULATED = 4,
};

/* --- log sink --------------------------------------------------------------
 * Stands in for android/log.h. The session points this at its own logging;
 * unset, the host writes to stderr. level is 0 = info, 1 = warning,
 * 2 = error. Called on the emulator thread. */
typedef void (*CocoLogSink)(int level, const char *msg, void *user);
void cocohost_set_log_sink(CocoLogSink sink, void *user);

/* --- frame sink (set by the session; called on the emulator thread) -------
 * Pixels are XRGB8888 (uint32 0x00RRGGBB), tightly packed, width*height. */
typedef void (*CocoFrameSink)(const uint32_t *xrgb8888, int width, int height,
                              void *user);
void cocohost_set_frame_sink(CocoFrameSink sink, void *user);

/* --- frame-rate sink ------------------------------------------------------
 * XRoar tells its video output whether the machine is running at 60Hz; the
 * session needs that to decide whether the vsync phase lock can engage at all
 * (a 50Hz machine cannot phase-lock to a 60Hz display). rate_mhz is the frame
 * rate in milli-Hz: 59940 for NTSC, 50000 for PAL. */
typedef void (*CocoFrameRateSink)(int rate_mhz, void *user);
void cocohost_set_frame_rate_sink(CocoFrameRateSink sink, void *user);

/* Where XRoar should look for ROMs. Must be set before
 * cocohost_core_start(). Copied internally. */
void cocohost_set_rompath(const char *path);

/* Becker port (DriveWire-over-TCP) endpoint XRoar connects out to. FujiNet
 * listens here. Defaults to 127.0.0.1:65504. */
void cocohost_set_becker_endpoint(const char *ip, int port);

/* Every byte across the Becker port, for the debugger's FujiNet tab. dir 1 is
 * CoCo -> FujiNet (bytes actually sent), dir 0 FujiNet -> CoCo (bytes the
 * CoCo read); len 0 means the link (re)connected. Called on the emulator
 * thread; NULL turns it off. */
typedef void (*CocoBeckerTrace)(int dir, const uint8_t *buf, size_t len);
void cocohost_set_becker_trace(CocoBeckerTrace fn);
/* Any thread. 1 while XRoar is connected to FujiNet. */
int  cocohost_becker_link_up(void);
/* Any thread. Bytes received from FujiNet the CoCo has not read yet. */
int  cocohost_becker_rx_pending(void);

/* --- core lifecycle (call on the emulator thread) ------------------------ */
/* Builds argv and runs xroar_init()/xroar_init_finish(). machine is
 * COCO_MACHINE_*, tv_input is COCO_TV_*, ccr is COCO_CCR_*, cpu is
 * COCO_CPU_*. Returns 0 on failure. */
int  cocohost_core_start(int machine, int tv_input, int ccr, int cpu);
void cocohost_core_run_frame(void);  /* advances one ~60Hz frame */
void cocohost_core_stop(void);
void cocohost_core_reset(void);      /* queued hard reset */

/* Live TV-input / artifact-mode switch (COCO_TV_*); applied on the emulator
 * thread. */
void cocohost_set_tv_input(int tv_input);

/* Live composite cross-colour (artifact) renderer switch (COCO_CCR_*). */
void cocohost_set_ccr(int ccr);

/* In-place CoCo 2 <-> CoCo 3 switch: no XRoar re-init (that crashes), just
 * ui_update_state(machine/cartridge) + xroar_hard_reset on the emulator
 * thread, with the matching HDB-DOS Becker ROM. FujiNet stays up; becker
 * reconnects. */
void cocohost_set_machine(int machine, int tv_input);

/* Live CPU switch (COCO_CPU_*): mutates the running machine_config's cpu
 * field and hard-resets in place, the same trick as cocohost_set_machine but
 * without touching the machine, TV input or cartridge ROM. Applied on the
 * emulator thread. */
void cocohost_set_cpu(int cpu);

/* --- audio -----------------------------------------------------------------
 * ao_desktop pushes completed interleaved stereo S16 frames (44100 Hz). */
void cocohost_push_audio(const int16_t *interleaved, int nframes);
/* Drain: blocks (bounded, 50ms) until max_samples interleaved stereo S16
 * samples are available, silence-padding on underrun. Returns max_samples. */
int  cocohost_fill_audio(int16_t *out, int max_samples);
void cocohost_audio_set_active(int active);  /* 0 on shutdown unblocks fill */
void cocohost_clear_audio(void);

/* --- video emit (from vo_desktop, emulator thread) ----------------------- */
void cocohost_emit_frame(const uint32_t *xrgb8888, int width, int height);
/* --- frame-rate notify (from vo_desktop, emulator thread) ---------------- */
void cocohost_notify_frame_rate(int is_60hz);

/* --- input -----------------------------------------------------------------
 * Keyboard: XRoar hkbd scancode (a USB HID usage code, see XRoar's
 * src/hkbd.h). Queued, applied per frame. */
void cocohost_inject_key(int down, int scancode);
/* Release every key the host believes is held. The frontends call this when
 * the window loses focus, so a key held across a focus change does not stick
 * down inside the machine forever. */
void cocohost_release_all_keys(void);
/* Analog joystick: port 0=right 1=left; axis 0=X 1=Y; value 0..65535
 * (32767 = centre). */
void cocohost_set_joystick_axis(int port, int axis, int value);
void cocohost_set_joystick_button(int port, int button, int pressed);
/* Read sides used by joystick_desktop's control delegates (emulator thread). */
int  cocohost_get_joystick_axis(int port, int axis);
int  cocohost_get_joystick_button(int port, int button);

#ifdef __cplusplus
}
#endif

#endif  /* COCO_HOST_H */
