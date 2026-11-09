/*
 * cocosession -- toolkit-agnostic desktop session for FujiNet Go CoCo.
 *
 * Owns the headless XRoar core (driven on its own paced thread through
 * core/xroar/coco_host.h), the SDL audio and gamepad backends, the remappable
 * controls (joysticks, pads, the on-screen keyboard), the in-process
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

/* CPU: both machine types support either core, since real CoCo 2s and 3s
 * both shipped with a socketed 6809 that some owners swapped for a Hitachi
 * 6309. HD6309 is XRoar's own upstream label ("Hitachi 6309 - UNVERIFIED" in
 * its enum help text) for its less-tested extended-mode emulation, not a
 * claim this port makes about its own disassembler support -- that part
 * (core/debugger/m6809dasm.c) is complete and independently tested. */
enum {
    COCOSESSION_CPU_MC6809 = 0, /* Motorola 6809 (default) */
    COCOSESSION_CPU_HD6309 = 1, /* Hitachi 6309, extended mode */
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
const char *cocosession_cpu_name(int idx);

/* 1 when the TV input is selectable on that machine. RGB exists only on the
 * CoCo 3, so a frontend showing the CoCo 2's menu must filter it out. */
int cocosession_tv_input_supported(int machine, int tv_input);

/* ---- lifecycle ---------------------------------------------------------- */
typedef struct {
    int machine;   /* COCOSESSION_MACHINE_* */
    int tv_input;  /* COCOSESSION_TV_* */
    int ccr;       /* COCOSESSION_CCR_* */
    int cpu;       /* COCOSESSION_CPU_* */
    int enable_fujinet; /* start the in-process FujiNet runtime */
    int external_fujinet; /* 1: don't start it; connect to another FujiNet
                           * instance's Becker port at fujinet_addr */
    char fujinet_addr[80]; /* "host:port" or "port" (external only); ""
                            * = 127.0.0.1:65504 */
    int enable_audio;   /* open the SDL audio device */
    int enable_gamepad; /* start the SDL gamepad thread */
    int analog_joystick; /* a pad's left stick drives the joystick
                          * proportionally (default); off, it is an eight-way
                          * switch. Live: cocosession_set_analog. */
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
/* Unlike the machine switch, this does not touch the TV input or cartridge
 * ROM -- it only swaps which CPU part the (unchanged) machine is built with,
 * then hard-resets in place. */
void cocosession_set_cpu(cocosession *s, int cpu);
int  cocosession_machine(const cocosession *s);
int  cocosession_tv_input(const cocosession *s);
int  cocosession_ccr(const cocosession *s);
int  cocosession_cpu(const cocosession *s);

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

/* Ask to be told when a new frame is ready, for a frontend with no frame
 * clock of its own: cb(ctx) runs on the machine thread after each frame is
 * published. Keep it quick (post an event and return) and do not call into
 * the session from it. Passing NULL stops the calls, and returns only after
 * any call in flight has finished, so ctx may be freed right after. A frame
 * identical to the previous one is not published again. */
typedef void (*cocosession_frame_cb)(void *ctx);
void cocosession_set_frame_callback(cocosession *s, cocosession_frame_cb cb,
                                    void *ctx);

/* Frames the machine has run since it started. Unlike copy_frame's serial it
 * counts every frame, including the ones identical to their predecessor
 * (which are not published again): use it to ask "is the machine running",
 * not "is the picture changing". */
uint64_t cocosession_frame_count(cocosession *s);

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
 * call cocosession_key, so one mapping table serves every frontend.
 *
 * Every key of the host keyboard belongs to the CoCo, so the joysticks' keys
 * start out unbound; a key the user binds (the Controllers window's Map mode)
 * stops going to the machine. Returns 1 when the key was consumed by a
 * binding (the caller does nothing more with it), 0 when it was an ordinary
 * key and went to the machine (or was dropped: no machine, or a key the CoCo
 * lacks). */
int cocosession_key(cocosession *sess, int down, uint32_t keysym,
                    uint32_t unicode, int ctrl_down, int shift_down);

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

/* Deflection in milli-units (-1000..1000, 0 centred; +x right, +y DOWN, as the
 * CoCo's Y is 0 at the top) onto the DAC range coco_axis_from_stick returns:
 * 0 -> 32767, -1000 -> 0, +1000 -> 65535. Pure. */
int coco_axis_from_deflection(int milli);
/* The same deflection as the 6-bit value a program reads (0..63, 31 centred),
 * for the Controllers window's readout. Pure. */
int coco_axis_6bit(int milli);
/* A pad stick reading (xval, yval in -1..1, SDL's convention: +x right, +y
 * down) as a deflection: a radial dead zone of 0.15, the remainder rescaled to
 * full travel, each axis clamped to -1000..1000. Exactly 0, 0 inside the dead
 * zone. Pure. */
void coco_deflect_from_stick(float xval, float yval, int *xaxis, int *yaxis);

/* ---- the controls: targets -------------------------------------------------
 * A CoCo has no console buttons: two analog joysticks (port 0 = RIGHT, port 1 =
 * LEFT, each an X and a Y axis and a fire button, the CoCo 3's with a second),
 * a RESET, and a keyboard that is the machine's own. The remappable controls
 * are the joysticks' four directions and two buttons, per port, then RESET,
 * then the session's own action (stop in the debugger): 14 targets in all.
 * Every target holds at most one keyboard key and one pad button (below). The
 * keyboard's keys are not targets: they are the machine's. */
typedef enum {
    COCO_ACT_RIGHT = 0,         /* +X deflection */
    COCO_ACT_LEFT,              /* -X */
    COCO_ACT_UP,                /* -Y: the stick pushed away from the player */
    COCO_ACT_DOWN,              /* +Y */
    COCO_ACT_BUTTON1,           /* the fire button */
    COCO_ACT_BUTTON2,           /* a CoCo 3 joystick's second button */
    COCO_ACT_PER_PORT
} coco_action;
typedef enum { COCO_SW_RESET = 0, COCO_SW_COUNT } coco_switch;
typedef enum { COCO_SYSACT_DEBUG_STOP = 0, COCO_SYSACT_COUNT } coco_sysaction;

#define COCO_TARGET_PORT(port, act) ((port) * COCO_ACT_PER_PORT + (act))
#define COCO_TARGET_SWITCH(swi) \
    (COCOSESSION_JOYSTICK_PORTS * COCO_ACT_PER_PORT + (swi))
#define COCO_TARGET_SYSACT(sac) \
    (COCOSESSION_JOYSTICK_PORTS * COCO_ACT_PER_PORT + COCO_SW_COUNT + (sac))
#define COCO_TARGET_COUNT \
    (COCOSESSION_JOYSTICK_PORTS * COCO_ACT_PER_PORT + COCO_SW_COUNT + COCO_SYSACT_COUNT)

/* The app's accent colour (the icon's green), for held controls. */
#define COCOSESSION_ACCENT_RGB 0x8BC34A

/* "Right joystick: Left", "RESET", "Debugger Stop"; the short form drops the
 * joystick: "Left", "Button 1". Static strings (the long form is rewritten by
 * the next call). */
const char *coco_target_name(int target);
const char *coco_target_short_name(int target);

/* ---- remappable bindings ---------------------------------------------------
 * Every target can be driven by one keyboard key and one pad button. The keys
 * all start unbound (every key belongs to the machine); the pad defaults are
 * the same on both joysticks: the D-pad steers, South is button 1, East button
 * 2. RESET and the debugger stop have none. Binding a key STEALS it from every
 * target that held it; a pad button is stolen only within its own joystick
 * and the machine-wide targets. Persisted in the settings store under
 * "bindings", as only the entries that differ from the defaults.
 *
 * A bound key is consumed by cocosession_key and no longer reaches the CoCo.
 * Keys are the X11 keysyms the frontends already pass, canonicalised by
 * cocosession_bindable_key: use it on the same keysym/unicode pair given to
 * cocosession_key when capturing a key in Map mode. */
typedef enum {
    COCO_PAD_BTN_NONE = -1,
    COCO_PAD_BTN_SOUTH = 0, COCO_PAD_BTN_EAST, COCO_PAD_BTN_WEST,
    COCO_PAD_BTN_NORTH, COCO_PAD_BTN_BACK, COCO_PAD_BTN_GUIDE,
    COCO_PAD_BTN_START, COCO_PAD_BTN_LEFT_STICK, COCO_PAD_BTN_RIGHT_STICK,
    COCO_PAD_BTN_LEFT_SHOULDER, COCO_PAD_BTN_RIGHT_SHOULDER,
    COCO_PAD_BTN_DPAD_UP, COCO_PAD_BTN_DPAD_DOWN, COCO_PAD_BTN_DPAD_LEFT,
    COCO_PAD_BTN_DPAD_RIGHT,
    COCO_PAD_BTN_LEFT_TRIGGER, COCO_PAD_BTN_RIGHT_TRIGGER,
    COCO_PAD_BTN_COUNT,
    /* Raw joystick bands for devices SDL has no gamepad mapping for (a plain
     * HID adapter): button index, and hat directions (hat * 8 + direction). */
    COCO_PAD_BTN_RAW_BASE = 64, COCO_PAD_BTN_RAW_LAST = 127,
    COCO_PAD_HAT_BASE = 192, COCO_PAD_HAT_LAST = 255
} coco_pad_button;
#define COCO_PAD_HAT_DIRS 8
#define COCO_PAD_BTN_IS_NAMED(btn) ((btn) >= 0 && (btn) < COCO_PAD_BTN_COUNT)
#define COCO_PAD_BTN_IS_RAW(btn) \
    ((btn) >= COCO_PAD_BTN_RAW_BASE && (btn) <= COCO_PAD_BTN_RAW_LAST)
#define COCO_PAD_BTN_IS_HAT(btn) \
    ((btn) >= COCO_PAD_HAT_BASE && (btn) <= COCO_PAD_HAT_LAST)

typedef struct {
    uint32_t keysym;     /* canonical (cocosession_bindable_key); 0 = no key */
    int      button;     /* coco_pad_button, NONE = no pad button */
} coco_binding;

/* The canonical key identity bindings use: keysym 0 falls back to unicode;
 * printable ASCII is lower-cased and un-shifted to the key's unshifted symbol
 * ('!' -> '1', 'A' -> 'a', ':' -> ';'); the keypad's Enter is Return; anything
 * else is unchanged. Returns 0 for what cannot be bound: nothing, F10-F12
 * (reserved by the frontends) and the pure modifier keys (Shift, Ctrl, Alt,
 * Meta/Super, Caps Lock). Pure. */
uint32_t cocosession_bindable_key(uint32_t keysym, uint32_t unicode);
/* Name for a canonical keysym ("F1", "Space", "A", "Left", "Keypad 5");
 * returns its length. */
int cocosession_keysym_name(uint32_t keysym, char *dst, int dstsz);
/* Name for a pad button ("A", "Left Trigger", "Button 3", "Hat 0 Up"). */
const char *coco_pad_button_name(int button);

coco_binding cocosession_binding_get(cocosession *sess, int target);
/* Bind; the previous holders of the key/button, if any, are described into
 * `stolen` (comma-separated target names; may be NULL). keysym 0 / button NONE
 * unbinds. A keysym cocosession_bindable_key refuses is ignored. */
void cocosession_binding_set_key(cocosession *sess, int target, uint32_t keysym,
                                 char *stolen, int stolensz);
void cocosession_binding_set_button(cocosession *sess, int target, int button,
                                    char *stolen, int stolensz);
void cocosession_bindings_reset(cocosession *sess);
/* The first target a canonical keysym / a pad button on `port` drives, or -1. */
int cocosession_target_for_key(cocosession *sess, uint32_t keysym);
int cocosession_target_for_button(cocosession *sess, int port, int button);

/* ---- driving the controls ---------------------------------------------------
 * What the machine sees of a joystick is merged from independent sources: the
 * host keyboard (bound keys), the Controllers window's buttons, the pointer
 * (its drag on the stick), and for each pad its buttons and its stick. Letting
 * go of one source never releases another's. Per axis: the digital sources
 * (directions held) push full deflection, opposed directions cancel, the
 * analog sources (a pad's stick in analog mode, the pointer) push a
 * proportional one, and the STRONGEST deflection wins; the fire buttons and
 * RESET simply OR. None of this needs a running machine, but nothing reaches
 * one until it runs. */
/* Hold (down 1) or release (down 0) a target from the window/keyboard source.
 * A direction pushes full deflection, a button is held, RESET is an edge (it
 * resets on press); the debugger stop runs on press. */
void cocosession_press(cocosession *sess, int target, int down);
/* Run a session action now, on the calling (UI) thread. */
void cocosession_sysaction(cocosession *sess, int sysact);
/* From a non-UI thread (the pad thread): post it for the UI thread's timer,
 * which takes it (returns 1 and the action) and runs cocosession_sysaction and
 * whatever window it opens. */
void cocosession_sysaction_post(cocosession *sess, int sysact);
int  cocosession_sysaction_take(cocosession *sess, int *out);
/* Release everything the host keyboard holds (focus loss): bound keys, and the
 * machine's keys held down. A pad's holds, the pointer's, and the
 * Controllers window's keyboard (including latched modifiers) are untouched. */
void cocosession_release_all(cocosession *sess);
/* The pointer's analog source on `port`: the stick dragged to (xaxis, yaxis),
 * -1000..1000 (+y down: see cocosession_stick_from_vector), and the fire
 * buttons held (bit 0 button 1, bit 1 button 2). 0, 0, 0 releases it. The
 * strongest source wins, so a window can keep it asserted ("Hold stick"). */
void cocosession_pointer_set(cocosession *sess, int port, int xaxis, int yaxis,
                             unsigned buttons);
/* What the machine sees held on `port` right now, merged from every source:
 * the deflection in milli-units (as pointer_set), and the buttons (bit 0
 * button 1, bit 1 button 2). For the window's highlights and readout. */
void     cocosession_joystick_held(cocosession *sess, int port, int *xaxis, int *yaxis);
unsigned cocosession_buttons_held(cocosession *sess, int port);
/* Whether a switch (coco_switch) is held, from any source. */
int      cocosession_switch_held(cocosession *sess, int swi);

/* ---- the on-screen joysticks -----------------------------------------------
 * One layout for every toolkit, in layout units (1.0 = a fire button's width)
 * from the top-left: the LEFT joystick (port 1) at the left, the RIGHT one
 * (port 0) at the right, RESET centred above them. Each joystick is a base
 * plate with a caption, its fire button(s), the stick's gate (a disc) and a
 * "X 31  Y 31" readout. Colours are the frontends'. A CoCo 2 layout has one
 * fire button per joystick, a CoCo 3 layout two; a frontend re-fetches the
 * layout when cocosession_machine() changes. Static; valid for the process. */
typedef enum {
    COCO_CTL_BASE = 0,      /* a joystick's base plate (drawn, never pressed) */
    COCO_CTL_CAPTION,       /* "Right joystick" / "Left joystick" */
    COCO_CTL_STICK,         /* the gate: a disc; drag = analog position */
    COCO_CTL_DIRECTION,     /* one of four sectors of the gate's outer ring */
    COCO_CTL_BUTTON,        /* a fire button (a circle) */
    COCO_CTL_RESET,         /* the console's RESET */
    COCO_CTL_READOUT        /* a text box: the stick's 6-bit position */
} coco_control_kind;

typedef struct {
    int kind;               /* coco_control_kind */
    const char *label;      /* "Right joystick", "UP", "1", "RESET", or NULL */
    const char *legend;     /* small text under a button, or NULL */
    int port;               /* 0 right, 1 left, -1 console */
    int target;             /* COCO_TARGET_*, or -1 (BASE, CAPTION, STICK, READOUT) */
    float left, top, wide, high;   /* the bounding box */
} coco_control;

/* The layout for a machine (COCOSESSION_MACHINE_*); *count is its entry count
 * and wide/high its extent. */
const coco_control *cocosession_controls_layout(int machine, int *count,
                                                float *wide, float *high);
/* The topmost control under a point in layout units (exact shapes: the gate is
 * a disc, a DIRECTION an annular sector, a BUTTON a disc), or -1. */
int cocosession_controls_hit(int machine, float posx, float posy);
/* A pointer at (delx, dely) from the gate's centre (+y down, in any unit) with
 * `radius` the knob's travel in the same unit, as a deflection: each axis
 * clamped independently to -1000..1000 (the CoCo's range is a square: a corner
 * is a full diagonal), and under 40 snaps to 0. */
void cocosession_stick_from_vector(float delx, float dely, float radius,
                                   int *xaxis, int *yaxis);

/* ---- the on-screen keyboard ---------------------------------------------------
 * The machine's keys as printed (CoCo 2: 53 keys; CoCo 3: 57, with CTRL, ALT,
 * F1 and F2), in key units: 1.0 = a key, pitch 1.1, from the top-left. Pressing
 * a cap injects the same machine key a host key would, so nothing is stored and
 * Map does not apply. The geometry is approximate, the key set and what each
 * key does are exact. */
typedef enum {
    COCO_KEYCAP_NORMAL = 0, COCO_KEYCAP_SPECIAL, COCO_KEYCAP_MODIFIER,
    COCO_KEYCAP_ARROW, COCO_KEYCAP_SPACE, COCO_KEYCAP_FUNCTION
} coco_keycap_kind;

typedef struct {
    const char *label;      /* "A", "1", "ENTER", "BREAK"; a UTF-8 arrow for ARROW */
    const char *shifted;    /* the legend printed above ("!" on 1), or NULL */
    unsigned hid;           /* the USB HID usage the cap injects */
    float left, top, wide, high;
    int kind;               /* coco_keycap_kind */
} coco_keycap;

const coco_keycap *cocosession_keyboard_layout(int machine, int *count,
                                               float *wide, float *high);
/* Press (down 1) or release (down 0) cap `index` of the CURRENT machine's
 * layout. A MODIFIER cap ignores the release and toggles on each press: it
 * latches (sticky), and lets go after the next ordinary key. */
void cocosession_keyboard_press(cocosession *sess, int index, int down);
/* Bit 0: the cap's machine key is held (from the cap or the host keyboard).
 * Bit 1: the cap is a latched modifier. */
int  cocosession_keyboard_held(cocosession *sess, int index);
/* Let go of every cap hold and latch (window hidden, machine switched). */
void cocosession_keyboard_release(cocosession *sess);

/* ---- gamepads (SDL, hotplug; started by cocosession_start) --------------
 * Pads (mapped gamepads and plain HID joysticks) are added and removed as they
 * appear. Each drives a joystick port: the first unassigned pad the RIGHT
 * joystick (port 0, the one nearly every game reads), the second the LEFT
 * (port 1), unless assigned explicitly. An explicit assignment is remembered
 * (setting "pad_assign", by the pad's GUID, else its name) and comes back when
 * the pad is plugged in again or on the next run. Idx is the pad's index in
 * the list, 0..count-1; it changes when a pad goes away, so a frontend
 * refreshes its rows when the generation changes. */
int  cocosession_gamepad_count(cocosession *sess);
/* Name of connected pad idx into dst; returns length or 0. */
int  cocosession_gamepad_name(cocosession *sess, int idx, char *dst, int dstsz);
/* Assign pad idx to a joystick port (0 = right, 1 = left); -1 returns it to
 * automatic. Remembered. */
void cocosession_gamepad_assign(cocosession *sess, int idx, int port);
/* The explicit port of pad idx, or -1 when automatic. */
int  cocosession_gamepad_assignment(cocosession *sess, int idx);
/* The port pad idx drives right now (automatic or explicit), or -1 for none. */
int  cocosession_gamepad_effective_port(cocosession *sess, int idx);
/* Bumped on every add/remove, and never reset across a session restart, so a
 * frontend can refresh its lists and toast cheaply. */
unsigned cocosession_gamepad_generation(cocosession *sess);
/* The last hot-plug event as text ("Connected: 8BitDo SN30 Pro (right
 * joystick)", "Disconnected: ..."), for a toast; returns length, 0 if none. */
int  cocosession_gamepad_last_event(cocosession *sess, char *dst, int dstsz);
/* Map mode: after begin(), the next pad button pressed on any pad is reported
 * by poll() (returns 1 and the button, a coco_pad_button) instead of acting.
 * cancel() disarms. */
void cocosession_gamepad_capture_begin(cocosession *sess);
void cocosession_gamepad_capture_cancel(cocosession *sess);
int  cocosession_gamepad_capture_poll(cocosession *sess, int *button);
/* A pad's left stick is the joystick's analog stick (radial dead zone 0.15,
 * the rest rescaled to full travel); off, it is an eight-way switch with
 * hysteresis (in at half deflection, out at 0.35). A raw joystick's hat is
 * always digital. Setting "analog_joystick", on by default; takes effect at
 * once. */
void cocosession_set_analog(cocosession *sess, int enable);

/* ---- audio ---------------------------------------------------------------
 * Owned by the session (SDL) when opts.enable_audio was set. A frontend that
 * wants to own the device instead can pull mixed emulator+FujiNet interleaved
 * stereo S16 samples at 44100 Hz. nsamples counts int16 samples, so one
 * stereo frame is two. */
int  cocosession_render_audio(cocosession *s, int16_t *out, int nsamples);

/* ---- FujiNet -------------------------------------------------------------*/
/* Parses the "Use external FujiNet" address field: "host:port" (host an IPv4
 * literal or a name the Becker connect resolves) or a bare "port". Empty text
 * means the default, 127.0.0.1:65504. Returns 0 and fills host/port, or -1
 * when the text is not an address:port (nothing is written then). */
int         cocosession_parse_hostport(const char *text, char *host,
                                       int hostsz, int *port);
int         cocosession_fujinet_running(const cocosession *s);
/* 1 when the session was started to connect to an external FujiNet. */
int         cocosession_fujinet_external(const cocosession *s);
/* "host:port" the Becker connection targets (the running session's, or the
 * default 127.0.0.1:65504 before the first start). */
const char *cocosession_becker_endpoint(const cocosession *s);
/* 1 while the emulator is connected to FujiNet over the Becker port. */
int         cocosession_fujinet_link(const cocosession *s);
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

/* ---- system ROMs ("BIOS") -------------------------------------------------
 * One row of the ROMs list: what a machine needs, and the machine it is for.
 * An item is available when any known dump of it is in the ROM directory. */
typedef struct {
    const char *name;
    const char *group;           /* "CoCo 2" or "CoCo 3" */
} coco_rom_item;

int cocosession_rom_item_count(void);
const coco_rom_item *cocosession_rom_item(int i);
int cocosession_rom_item_available(const cocosession *s, int i);

/* Import system ROMs from path: a file, a zip (MAME's coco and coco3 sets
 * work) or a folder (searched to a depth of 8). Each known dump is
 * recognised by size and CRC32, whatever the file is called, and written
 * under the name XRoar looks for (bas13.rom, extbas11.rom, coco3.rom, ...) in
 * the ROM directory. A running machine is rebuilt in place so the new ROMs
 * take effect; the session is not restarted. Writes a one-line result into
 * msg ("Now available: ...", "Imported N file(s); ...", or that nothing was
 * found). Returns 0 if any ROM was written, -1 if not.
 *
 * A build configured with WITH_COCO_ROMS=ON has already materialised its own
 * copies here; an imported dump replaces the file of the same name. See
 * COMPLIANCE.md. */
int cocosession_rom_import(cocosession *s, const char *path, char *msg, int msgsz);

/* 1 when the ROM directory holds enough to boot the given machine: the CoCo 2
 * needs bas13 + extbas11, the CoCo 3 needs coco3. A frontend that gets 0 here
 * should prompt for Import BIOS... rather than start a machine that
 * would come up to a blank screen. */
int cocosession_has_system_roms(const cocosession *s, int machine);

/* Directory paths (valid for the session's lifetime). */
const char *cocosession_roms_path(const cocosession *s);
const char *cocosession_carts_path(const cocosession *s);
const char *cocosession_config_path(const cocosession *s);
const char *cocosession_data_path(const cocosession *s);
const char *cocosession_sd_path(const cocosession *s);

/* ---- debugger ------------------------------------------------------------
 * Created when the session starts (its FujiNet trace runs from then), or
 * here if it has not; engaging it (breakpoints/pause) switches the emulator
 * loop to the instruction-stepped core path. See cocodebug.h. */
cocodebug *cocosession_debugger(cocosession *s);

#ifdef __cplusplus
}
#endif

#endif /* COCOSESSION_H */
