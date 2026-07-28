/*
 * cocosession -- toolkit-agnostic desktop session for FujiNet Go CoCo.
 *
 * Owns the headless XRoar core (driven on its own paced thread through
 * core/xroar/coco_host.h), the SDL audio and gamepad backends, the in-process
 * FujiNet runtime (dlopen'd libfujinet.so, joined to the core over the Becker
 * port -- DriveWire-over-TCP -- on loopback 65504), the shared settings store,
 * and the media path layout. Frontends (GTK, Qt, AppKit, Win32) drive this API
 * and only do windowing, painting, and event translation.
 *
 * Note the transport direction: unlike the ADAM and Apple II ports, here
 * *FujiNet listens* and the emulator connects out to it. cocosession_start
 * therefore starts FujiNet first.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef COCOSESSION_H
#define COCOSESSION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Frame geometry is NOT fixed: XRoar sizes the viewport from what the machine
 * actually renders, and the CoCo 3's GIME modes are wider than the CoCo 2's
 * VDG ones. Frontends allocate for the maximum and paint whatever
 * cocosession_copy_frame reports. The maxima match XRoar's own SDL frontend
 * (src/sdl2/vo_sdl2.c), which covers the widest "underscan" viewport. */
#define COCOSESSION_FB_MAX_WIDTH  800
#define COCOSESSION_FB_MAX_HEIGHT 300

/* Becker-port (DriveWire-over-TCP) loopback port -- FujiNet listens here and
 * the emulator connects out -- fixed to match the Android app.
 *
 * The FujiNet web admin port is fixed too, but deliberately NOT the Android
 * app's 8002: a standalone fujinet-pc-coco uses that, and anyone developing
 * FujiNet has one running. The desktop targets each take a high port instead
 * (ADAM 65214, Apple II 64001), so all three can run at once and none of them
 * fights a standalone build. */
/* The CoCo has two joystick ports: 0 = right, 1 = left. The right one is what
 * nearly every game reads. */
#define COCOSESSION_JOYSTICK_PORTS 2

#define COCOSESSION_BECKER_PORT 65504
#define COCOSESSION_WEBUI_PORT  64002

typedef struct cocosession cocosession;
typedef struct cocodebug cocodebug;

/* All members optional (NULL = default).
 *  config_dir: default $XDG_CONFIG_HOME/fujinet-go-coco
 *  data_dir:   default $XDG_DATA_HOME/fujinet-go-coco
 *  fujinet_lib: path to libfujinet.so/.dylib/.dll; default searches
 *              $FUJINET_LIB, the executable's directory, the install libdir,
 *              then tools/fujinet/work/out. "" disables.
 *  fujinet_runtime_src: directory holding the pristine fnconfig.ini +
 *              data/ + SD/ used to provision the user's runtime tree on
 *              first start (a macOS app passes its bundle's
 *              Resources/fujinet here); default searches the install
 *              share dir, then tools/fujinet/work/out. */
typedef struct {
    const char *config_dir;
    const char *data_dir;
    const char *fujinet_lib;
    const char *fujinet_runtime_src;
} cocosession_paths;

/* Creates the session, provisions the config/data directories, materialises
 * the embedded ROMs into the ROM directory, and loads the shared settings
 * store. Does not start emulation. Returns NULL only on out-of-memory /
 * unusable directories. */
cocosession *cocosession_new(const cocosession_paths *paths);
void cocosession_free(cocosession *s);

/* ---- settings (shared INI; one store for all frontends/platforms) ------- */
int         cocosession_get_int(cocosession *s, const char *key, int def);
void        cocosession_set_int(cocosession *s, const char *key, int value);
const char *cocosession_get_str(cocosession *s, const char *key,
                                const char *def);
void        cocosession_set_str(cocosession *s, const char *key,
                                const char *value);
void        cocosession_settings_flush(cocosession *s);

/* ---- machine / video enumerations ---------------------------------------
 * These are the session's own values; core/xroar/coco_host.h maps them to
 * XRoar's. Frontends build their menus from the *_name() accessors so a new
 * entry does not have to be added in four places. */
enum {
    COCOSESSION_MACHINE_COCO3 = 0, /* Color Computer 3 (GIME) */
    COCOSESSION_MACHINE_COCO2 = 1, /* Color Computer 2 (VDG) */
};

/* TV input, i.e. how the machine's video reaches the display. This is what
 * decides whether artifact colour happens at all, and in which phase. */
enum {
    COCOSESSION_TV_SVIDEO = 0,       /* S-Video: clean, no artifact colours */
    COCOSESSION_TV_COMPOSITE_BR = 1, /* Composite, blue-red artifact phase */
    COCOSESSION_TV_COMPOSITE_RB = 2, /* Composite, red-blue artifact phase */
    COCOSESSION_TV_RGB = 3,          /* RGB -- CoCo 3 only */
};

/* Composite cross-colour renderer: how faithfully the artifact colours in the
 * composite TV inputs are simulated. Ignored for S-Video and RGB. */
enum {
    COCOSESSION_CCR_NONE = 0,      /* palette only, no cross-colour */
    COCOSESSION_CCR_SIMPLE = 1,    /* simple 2-bit */
    COCOSESSION_CCR_5BIT = 2,      /* 5-bit (the default) */
    COCOSESSION_CCR_PARTIAL = 3,   /* partial NTSC simulation */
    COCOSESSION_CCR_SIMULATED = 4, /* full NTSC simulation (expensive) */
};

/* Menu labels, NULL past the end so callers can walk idx = 0.. until NULL. */
const char *cocosession_machine_name(int idx);
const char *cocosession_tv_input_name(int idx);
const char *cocosession_ccr_name(int idx);

/* 1 when the TV input is selectable on that machine. RGB exists only on the
 * CoCo 3, so a frontend showing the CoCo 2's menu must filter it out. */
int cocosession_tv_input_supported(int machine, int tv_input);

/* ---- lifecycle ---------------------------------------------------------- */
typedef struct {
    int machine;   /* COCOSESSION_MACHINE_* */
    int tv_input;  /* COCOSESSION_TV_* */
    int ccr;       /* COCOSESSION_CCR_* */
    int enable_fujinet; /* start the in-process FujiNet runtime */
    int enable_audio;   /* open the SDL audio device */
    int enable_gamepad; /* start the SDL gamepad thread */
} cocosession_start_opts;

/* Fills opts from the settings store (defaults: CoCo 3, composite blue-red,
 * 5-bit cross-colour, everything enabled). The values are plain ints, so
 * unlike the other ports nothing here points back into the session. */
void cocosession_default_opts(cocosession *s, cocosession_start_opts *opts);

/* Starts FujiNet (if enabled) and then the emulator thread. Returns 0 on
 * success, -1 with cocosession_last_error() set.
 *
 * Machine, TV input and cross-colour can all be changed while running (see
 * below), so a restart is only needed to change what FujiNet, audio or the
 * gamepad backend are doing. */
int  cocosession_start(cocosession *s, const cocosession_start_opts *opts);
void cocosession_stop(cocosession *s);
int  cocosession_is_running(const cocosession *s);
const char *cocosession_last_error(const cocosession *s);

/* ---- live machine / video switching --------------------------------------
 * All three take effect on the emulator thread within a frame, without
 * restarting the session: FujiNet stays up and the Becker link reconnects.
 * Switching the machine hard-resets it (and swaps in the matching HDB-DOS
 * Becker ROM), so it is destructive to whatever was running; the TV input and
 * cross-colour renderer are not. */
void cocosession_set_machine(cocosession *s, int machine);
void cocosession_set_tv_input(cocosession *s, int tv_input);
void cocosession_set_ccr(cocosession *s, int ccr);
int  cocosession_machine(const cocosession *s);
int  cocosession_tv_input(const cocosession *s);
int  cocosession_ccr(const cocosession *s);

/* ---- video ---------------------------------------------------------------
 * The emulator thread stores the latest changed frame; the UI thread pulls it
 * on its own vsync/frame-clock tick. copy_frame copies the latest frame into
 * dst (XRGB8888, tightly packed, at most FB_MAX_WIDTH*FB_MAX_HEIGHT uint32)
 * iff its serial differs from *serial_inout, updates *serial_inout, writes
 * the frame's dimensions through w_out and h_out, and returns 1; returns 0
 * when the frame is unchanged (dst, w_out and h_out untouched). Pass
 * *serial_inout = 0 to force a copy (e.g. first paint after a window map).
 *
 * The dimensions change at runtime with the video mode, so paint what is
 * reported rather than caching it. */
int  cocosession_copy_frame(cocosession *s, uint32_t *dst,
                            uint64_t *serial_inout, int *w_out, int *h_out);

/* Feed the UI's vsync ticks (frame-clock time, CLOCK_MONOTONIC ns). While a
 * steady ~60Hz tick stream arrives *and the machine is running at 60Hz*, the
 * emulator phase-locks one emulated frame per tick; otherwise it paces on the
 * wall clock at the machine's declared rate. A PAL machine runs at 50Hz, which
 * no desktop display ticks at, so the phase lock declines to engage there. */
void cocosession_notify_vsync(cocosession *s, int64_t frame_time_ns);

/* The machine's frame rate in milli-Hz (59940 for NTSC, 50000 for PAL), as
 * XRoar reports it. 0 before the first frame. */
int  cocosession_frame_rate_mhz(const cocosession *s);

/* ---- input ---------------------------------------------------------------
 * Frontends translate their toolkit's key events to X11/xkb keysyms (GDK
 * keyvals already are; Qt, AppKit and Win32 map through a small table), then
 * call cocosession_key, so one mapping table serves every frontend. */
void cocosession_key(cocosession *s, int down, uint32_t keysym,
                     uint32_t unicode, int ctrl_down, int shift_down);

/* Analog joystick. port 0 = right, 1 = left; x and y in -1.0..1.0, centred at
 * 0 (the session rescales to the CoCo's 0..65535 DAC range). button 0 is the
 * primary fire button; the CoCo 3 has a second. */
void cocosession_joystick(cocosession *s, int port, float x, float y);
void cocosession_joystick_button(cocosession *s, int port, int button,
                                 int pressed);

/* Hard reset (the CoCo's reset button). There is no warm/cold distinction to
 * expose the way the Apple II has one: RESET on a CoCo is this. */
void cocosession_reset(cocosession *s);

/* Translate a desktop key event to the XRoar hkbd scancode the core expects.
 * keysym is an X11/xkb keysym, unicode the translated character (0 if none),
 * ctrl_down/shift_down the modifier state. Writes the scancode -- a USB HID
 * usage code, see XRoar's src/hkbd.h -- to *scancode_out and returns 1 when
 * the key is forwarded, 0 when it is not.
 *
 * F10, F11 and F12 are reserved for the frontends (menu, fullscreen,
 * debugger) and are never forwarded.
 *
 * Pure function, shared by every frontend and unit-tested on its own. */
int coco_key_from_event(uint32_t keysym, uint32_t unicode, int ctrl_down,
                        int shift_down, unsigned *scancode_out);

/* Which connected pad drives `port`?
 *
 * bindings[i] is what pad i is bound to: a port number, or -1 for automatic.
 * Returns the pad index, or -1 when no pad drives that port -- including when
 * `port` is not one the machine has. An explicit
 * binding wins; the rest fall into the free ports in order, so the first
 * connected pad takes the right port (which is the one nearly every game
 * reads) and the second takes the left.
 *
 * Pure function, split out of the SDL backend so the assignment rules can be
 * tested without any hardware attached. */
int coco_pad_for_port(const int *bindings, int npads, int port);

/* Apply a gamepad stick reading to a joystick axis: applies the deadzone,
 * rescales the remainder to the full -1..1 range so the joystick still reaches
 * its stops, then maps onto the CoCo's 0..65535 DAC range with 32767 at
 * centre. Pure function; shared with the SDL gamepad backend. */
int coco_axis_from_stick(float v, float deadzone);

/* ---- gamepads (SDL, hotplug; started by cocosession_start) -------------- */
int  cocosession_gamepad_count(cocosession *s);
/* Name of connected pad idx into dst; returns length or 0. */
int  cocosession_gamepad_name(cocosession *s, int idx, char *dst, int dstsz);
/* Bind pad idx to a joystick port (0 = right, 1 = left); -1 restores
 * automatic assignment (first pad to the right port, second to the left). */
void cocosession_gamepad_assign(cocosession *s, int idx, int port);

/* ---- audio ---------------------------------------------------------------
 * Owned by the session (SDL) when opts.enable_audio was set. A frontend that
 * wants to own the device instead can pull mixed emulator+FujiNet interleaved
 * stereo S16 samples at 44100 Hz. nsamples counts int16 samples, so one
 * stereo frame is two. */
int  cocosession_render_audio(cocosession *s, int16_t *out, int nsamples);

/* ---- FujiNet -------------------------------------------------------------*/
int         cocosession_fujinet_running(const cocosession *s);
const char *cocosession_fujinet_webui_url(const cocosession *s);
/* Copies the most recent FujiNet console output (NUL-terminated) into dst;
 * returns the number of bytes written (excluding the NUL). */
int         cocosession_fujinet_copy_log(cocosession *s, char *dst, int max);

/* Import media for the emulated machine. Disk and tape images
 * (.dsk .os9 .vdk .cas .wav) go to the FujiNet SD directory, to be mounted
 * from the web UI; cartridge images (.rom .ccc) go to the cartridge
 * directory. Returns 0 on success and writes the destination path into
 * dest_out. */
int cocosession_import_media(cocosession *s, const char *src_path,
                             char *dest_out, int dest_sz);

/* Import a CoCo system ROM. The file is copied into the session's ROM
 * directory under its own name -- XRoar resolves ROMs by exact basename
 * against its -rompath (bas13.rom, extbas11.rom, coco3.rom, ...), so the name
 * must be preserved. Takes effect on the next start. Returns 0 on success and
 * writes the destination into dest_out.
 *
 * A build configured with WITH_COCO_ROMS=ON has already materialised its own
 * copies here, but an imported file always wins: the session never overwrites
 * a name that is already present. See COMPLIANCE.md. */
int cocosession_import_rom(cocosession *s, const char *src_path,
                           char *dest_out, int dest_sz);

/* 1 when the ROM directory holds enough to boot the given machine: the CoCo 2
 * needs bas13 + extbas11, the CoCo 3 needs coco3. A frontend that gets 0 here
 * should prompt for Import System ROMs... rather than start a machine that
 * would come up to a blank screen. */
int cocosession_has_system_roms(const cocosession *s, int machine);

/* Directory paths (valid for the session's lifetime). */
const char *cocosession_roms_path(const cocosession *s);
const char *cocosession_carts_path(const cocosession *s);
const char *cocosession_config_path(const cocosession *s);
const char *cocosession_data_path(const cocosession *s);
const char *cocosession_sd_path(const cocosession *s);

/* ---- debugger ------------------------------------------------------------
 * Lazily created; engaging it (breakpoints/pause) switches the emulator loop
 * to the instruction-stepped core path. See cocodebug.h. */
cocodebug *cocosession_debugger(cocosession *s);

#ifdef __cplusplus
}
#endif

#endif /* COCOSESSION_H */
