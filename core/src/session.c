/*
 * cocosession -- session lifecycle and the paced emulator loop.
 *
 * Port of the Android host layer (coco_host.c + SessionController.kt): the
 * emulator thread free-runs cocohost_core_run_frame (one frame's worth of
 * XRoar ticks) at the machine's declared rate on a wall-clock sleeper,
 * phase-locking to the UI's vsync ticks whenever a steady ~60 Hz stream of
 * them arrives (cocosession_notify_vsync) and the machine is itself running at
 * 60 Hz. Frames are published to a latest-frame store the UI thread pulls on
 * its own tick, so a stalled paint can never freeze the 6809 -- and therefore
 * never stall the DriveWire traffic FujiNet depends on.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "coco_host.h"
#include "compat.h"
#include "session_internal.h"

/****************************************************************************/
/** Small helpers                                                          **/
/****************************************************************************/

void session_set_error(cocosession *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->last_error, sizeof(s->last_error), fmt, ap);
    va_end(ap);
    fprintf(stderr, "cocosession: %s\n", s->last_error);
}

static long read_timer_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000000L + ts.tv_nsec / 1000L);
}

/* Absolute-deadline sleeping and monotonic condvars live in compat.h so
 * the same code paces correctly on Linux, macOS and Windows. */

static void core_log_sink(int level, const char *msg, void *user)
{
    (void)user;
    if (level >= 2) fprintf(stderr, "cococore: %s\n", msg);
}

/****************************************************************************/
/** Machine / video option tables                                          **/
/****************************************************************************/

/* Indexed by COCOSESSION_MACHINE_*, COCOSESSION_TV_* and COCOSESSION_CCR_*.
 * These are menu labels, not core arguments -- coco_host.c holds the mapping
 * to XRoar's own names -- so they are the one place to translate. */
static const char *const k_machines[] = {
    "Color Computer 3", "Color Computer 2", NULL
};
static const char *const k_tv_inputs[] = {
    "S-Video", "Composite (blue-red)", "Composite (red-blue)", "RGB", NULL
};
static const char *const k_ccrs[] = {
    "None", "Simple (2-bit)", "5-bit", "Partial NTSC", "Simulated NTSC", NULL
};

static const char *table_at(const char *const *tab, int idx)
{
    int n = 0;
    if (idx < 0) return NULL;
    while (tab[n]) n++;
    return idx < n ? tab[idx] : NULL;
}

const char *cocosession_machine_name(int idx)  { return table_at(k_machines, idx); }
const char *cocosession_tv_input_name(int idx) { return table_at(k_tv_inputs, idx); }
const char *cocosession_ccr_name(int idx)      { return table_at(k_ccrs, idx); }

int cocosession_tv_input_supported(int machine, int tv_input)
{
    if (tv_input < 0 || !table_at(k_tv_inputs, tv_input)) return 0;
    /* The RGB connector is a CoCo 3 feature; on a CoCo 2 there is no signal to
     * carry, and XRoar renders a blank screen if you ask for it anyway. */
    if (tv_input == COCOSESSION_TV_RGB)
        return machine == COCOSESSION_MACHINE_COCO3;
    return 1;
}

/* Clamp a stored or caller-supplied enum to something the core will accept.
 * A hand-edited INI is the usual source of a bad value. */
static int clamp_opt(int v, const char *const *tab, int def)
{
    return table_at(tab, v) ? v : def;
}

/****************************************************************************/
/** Vsync phase-lock (ticks fed by the frontends)                          **/
/****************************************************************************/

void cocosession_notify_vsync(cocosession *s, int64_t frame_time_ns)
{
    if (!s) return;
    pthread_mutex_lock(&s->vs_mtx);
    if (s->vs_ns)
        s->vs_iv_us = (long)((frame_time_ns - s->vs_ns) / 1000);
    s->vs_ns = frame_time_ns;
    pthread_cond_broadcast(&s->vs_cv);
    pthread_mutex_unlock(&s->vs_mtx);
}

/* XRoar reports the machine's frame rate through the vo module; the host
 * forwards it here. Emulator thread. */
static void core_frame_rate_sink(int rate_mhz, void *user)
{
    cocosession *s = user;
    s->frame_rate_mhz = rate_mhz;
}

int cocosession_frame_rate_mhz(const cocosession *s)
{
    return s ? s->frame_rate_mhz : 0;
}

/* True while ~60Hz ticks are arriving (window visible on a ~60Hz display) AND
 * the machine is itself running at ~60Hz.
 *
 * That second condition is what the Apple II and ADAM ports do not need. A PAL
 * CoCo runs at 50 Hz, and no desktop display ticks at 50 Hz: phase-locking one
 * emulated frame to each 60 Hz tick would run the machine 20% fast, which is
 * audible long before it is visible. So at 50 Hz we decline to engage and pace
 * on the wall clock instead, which is correct if slightly less smooth. */
static int vsync_fresh(cocosession *s, long now_us)
{
    int fresh;
    int rate = s->frame_rate_mhz;

    /* Unknown rate (no frame yet) is treated as 60Hz: that is the default for
     * every machine this build ships, and the check re-runs every frame. */
    if (rate != 0 && (rate < 55000 || rate > 66000))
        return 0;

    pthread_mutex_lock(&s->vs_mtx);
    fresh = s->vs_ns != 0 &&
            (now_us - (long)(s->vs_ns / 1000)) < 100000 && /* fresh (<100ms) */
            s->vs_iv_us >= 15000 && s->vs_iv_us <= 18500;  /* ~55-66Hz */
    pthread_mutex_unlock(&s->vs_mtx);
    return fresh;
}

/* Block until the next UI tick after the one we last paced on and return its
 * time (us). Self-correcting: if a frame's work overran a tick, the next call
 * returns at once. Bails on a short timeout so it can never hang if the ticks
 * stop mid-wait (window hidden). */
static long wait_next_vsync(cocosession *s)
{
    long v;
    pthread_mutex_lock(&s->vs_mtx);
    while (!s->stop_flag && s->vs_ns <= s->vs_seen_ns) {
        /* 40ms safety timeout so this can never hang if the ticks stop. */
        if (coco_cond_timedwait_ms(&s->vs_cv, &s->vs_mtx, 40) == ETIMEDOUT)
            break;
    }
    s->vs_seen_ns = s->vs_ns;
    v = (long)(s->vs_ns / 1000);
    pthread_mutex_unlock(&s->vs_mtx);
    return v;
}

/****************************************************************************/
/** Frame store                                                            **/
/****************************************************************************/

/* Called by the XRoar vo module from inside xroar_run, on the emulator thread,
 * once per completed frame. */
static void on_core_frame(const uint32_t *px, int w, int h, void *user)
{
    cocosession *s = user;
    size_t n;

    if (w <= 0 || h <= 0) return;
    if (w > COCOSESSION_FB_MAX_WIDTH || h > COCOSESSION_FB_MAX_HEIGHT) {
        /* ui_desktop.c clamps the viewport to exactly these maxima, so this
         * cannot fire today; it stays because the two limits live in different
         * files and dropping the frame beats scribbling past the buffer if
         * they ever drift apart. */
        fprintf(stderr, "cocosession: dropping oversized %dx%d frame "
                        "(max %dx%d)\n",
                w, h, COCOSESSION_FB_MAX_WIDTH, COCOSESSION_FB_MAX_HEIGHT);
        return;
    }

    n = (size_t)w * (size_t)h;
    pthread_mutex_lock(&s->frame_mtx);
    memcpy(s->frame, px, n * sizeof(uint32_t));
    s->frame_w = w;
    s->frame_h = h;
    s->frame_serial++;
    pthread_mutex_unlock(&s->frame_mtx);
}

int cocosession_copy_frame(cocosession *s, uint32_t *dst,
                           uint64_t *serial_inout, int *w_out, int *h_out)
{
    int copied = 0;
    pthread_mutex_lock(&s->frame_mtx);
    if (s->frame_serial != *serial_inout && s->frame_w > 0) {
        memcpy(dst, s->frame,
               (size_t)s->frame_w * (size_t)s->frame_h * sizeof(uint32_t));
        *serial_inout = s->frame_serial;
        if (w_out) *w_out = s->frame_w;
        if (h_out) *h_out = s->frame_h;
        copied = 1;
    }
    pthread_mutex_unlock(&s->frame_mtx);
    return copied;
}

/****************************************************************************/
/** Emulator thread                                                        **/
/****************************************************************************/

static void *emu_thread_main(void *arg)
{
    cocosession *s = arg;
    long next_us;
    const int pace_log = getenv("COCO_PACE_LOG") != NULL;

    coco_thread_setname("coco-emu");

    /* XRoar resolves every machine and cartridge ROM by basename against this
     * path, so it must be set before the core starts. */
    cocohost_set_rompath(s->roms_dir);
    cocohost_set_becker_endpoint("127.0.0.1", COCOSESSION_BECKER_PORT);

    cocohost_set_log_sink(core_log_sink, s);
    cocohost_set_frame_sink(on_core_frame, s);
    cocohost_set_frame_rate_sink(core_frame_rate_sink, s);

    {
        int ok = cocohost_core_start(s->opts.machine, s->opts.tv_input,
                                     s->opts.ccr);
        if (!ok) {
            if (!roms_present_for_machine(s->roms_dir, s->opts.machine))
                session_set_error(s,
                    "No CoCo system ROMs found in %s.\n\n"
                    "Use Media > Import System ROMs... to supply your own "
                    "(or set COCO_ROM_DIR to point at a folder that already "
                    "has them).", s->roms_dir);
            else
                session_set_error(s, "the XRoar core failed to start");
            cocohost_set_frame_sink(NULL, NULL);
            cocohost_set_frame_rate_sink(NULL, NULL);
        }
        /* Publish the result either way: cocosession_start is blocked on this,
         * and a failed core must not leave it waiting. */
        pthread_mutex_lock(&s->start_mtx);
        s->core_started = ok ? 1 : -1;
        pthread_cond_broadcast(&s->start_cv);
        pthread_mutex_unlock(&s->start_mtx);
        if (!ok) {
            s->running = 0;
            return NULL;
        }
    }

    next_us = read_timer_us();
    while (!s->stop_flag) {
        /* Pace from what the machine actually declares rather than a
         * hard-coded constant, re-read each frame so a PAL machine (or a
         * machine switch to one) is picked up without a restart. */
        int rate = s->frame_rate_mhz;
        long frame_us = (rate > 1000) ? (long)(1000000000L / rate) : 16683;

        if (s->dbg_engaged)
            cocodebug_session_frame(s);
        else
            cocohost_core_run_frame();

        /* Pacing: one emulated frame per UI vsync tick while ticks arrive,
         * else the wall clock. Once-per-second diagnostics with
         * COCO_PACE_LOG=1. */
        {
            long now = read_timer_us();
            int vfresh = vsync_fresh(s, now);
            static long dbg_t0;
            static int dbg_frames, dbg_behind, dbg_vsync;

            if (dbg_t0 == 0) dbg_t0 = now;
            dbg_frames++;
            if (vfresh) dbg_vsync++;

            next_us += frame_us;
            if (now >= next_us) dbg_behind++;

            if (now - dbg_t0 >= 1000000L) {
                if (pace_log)
                    fprintf(stderr,
                            "cocosession pace: %d fps over %ld ms (%d behind, "
                            "%d on vsync, machine %d mHz)\n",
                            dbg_frames, (now - dbg_t0) / 1000, dbg_behind,
                            dbg_vsync, rate);
                dbg_t0 = now;
                dbg_frames = dbg_behind = dbg_vsync = 0;
            }

            if (vfresh) {
                /* Snap the wall clock so a later fallback resumes from real
                 * time rather than a stale schedule. */
                next_us = wait_next_vsync(s);
            } else if (now < next_us) {
                coco_sleep_until_us(next_us);
            } else {
                next_us = now; /* behind: don't accumulate debt */
            }
        }
    }

    cocohost_core_stop();
    /* Drop the sinks before the session's frame buffer can go away. */
    cocohost_set_frame_sink(NULL, NULL);
    cocohost_set_frame_rate_sink(NULL, NULL);
    s->running = 0;
    return NULL;
}

/****************************************************************************/
/** Lifecycle                                                              **/
/****************************************************************************/

cocosession *cocosession_new(const cocosession_paths *paths)
{
    cocosession *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    /* ~1MB, so heap rather than inline in the struct. */
    s->frame = calloc(COCO_FB_MAX_PIXELS, sizeof(uint32_t));
    if (!s->frame) {
        free(s);
        return NULL;
    }

    if (paths_init(s, paths) != 0) {
        free(s->frame);
        free(s);
        return NULL;
    }

    pthread_mutex_init(&s->lifecycle_mtx, NULL);
    pthread_mutex_init(&s->frame_mtx, NULL);
    pthread_mutex_init(&s->vs_mtx, NULL);
    pthread_mutex_init(&s->start_mtx, NULL);
    coco_cond_init_monotonic(&s->vs_cv);
    coco_cond_init_monotonic(&s->start_cv);

    settings_init(s);

    /* Publish the persisted machine/video selection immediately, so the
     * accessors mean something before (and without) a start. The frontends
     * build their radio menus from these, and a start that failed -- no ROMs
     * imported yet, say -- would otherwise leave every menu showing the
     * defaults rather than what the user actually has selected. */
    {
        cocosession_start_opts opts;
        cocosession_default_opts(s, &opts);
        s->cur_machine = opts.machine;
        s->cur_tv_input = opts.tv_input;
        s->cur_ccr = opts.ccr;
    }
    return s;
}

void cocosession_free(cocosession *s)
{
    if (!s) return;
    cocosession_stop(s);
    cocosession_settings_flush(s);
    settings_free_all(s);
    cocodebug_destroy(s->debugger);
    pthread_mutex_destroy(&s->lifecycle_mtx);
    pthread_mutex_destroy(&s->frame_mtx);
    pthread_mutex_destroy(&s->vs_mtx);
    pthread_mutex_destroy(&s->start_mtx);
    pthread_cond_destroy(&s->vs_cv);
    pthread_cond_destroy(&s->start_cv);
    free(s->frame);
    free(s);
}

void cocosession_default_opts(cocosession *s, cocosession_start_opts *opts)
{
    memset(opts, 0, sizeof(*opts));
    opts->machine = clamp_opt(cocosession_get_int(s, "machine",
                                                  COCOSESSION_MACHINE_COCO3),
                              k_machines, COCOSESSION_MACHINE_COCO3);
    /* Composite blue-red by default: a real CoCo went to a television, and the
     * artifact colours that hi-res games rely on only exist on that signal. */
    opts->tv_input = clamp_opt(cocosession_get_int(s, "tv_input",
                                                   COCOSESSION_TV_COMPOSITE_BR),
                               k_tv_inputs, COCOSESSION_TV_COMPOSITE_BR);
    opts->ccr = clamp_opt(cocosession_get_int(s, "ccr", COCOSESSION_CCR_5BIT),
                          k_ccrs, COCOSESSION_CCR_5BIT);
    if (!cocosession_tv_input_supported(opts->machine, opts->tv_input))
        opts->tv_input = COCOSESSION_TV_COMPOSITE_BR;
    opts->enable_fujinet = cocosession_get_int(s, "enable_fujinet", 1) ? 1 : 0;
    opts->enable_audio = cocosession_get_int(s, "enable_audio", 1) ? 1 : 0;
    opts->enable_gamepad = cocosession_get_int(s, "enable_gamepad", 1) ? 1 : 0;
}

int cocosession_start(cocosession *s, const cocosession_start_opts *opts)
{
    pthread_mutex_lock(&s->lifecycle_mtx);
    if (s->running) {
        pthread_mutex_unlock(&s->lifecycle_mtx);
        return 0;
    }

    s->opts = *opts;
    s->opts.machine = clamp_opt(opts->machine, k_machines,
                                COCOSESSION_MACHINE_COCO3);
    s->opts.ccr = clamp_opt(opts->ccr, k_ccrs, COCOSESSION_CCR_5BIT);
    s->opts.tv_input = clamp_opt(opts->tv_input, k_tv_inputs,
                                 COCOSESSION_TV_COMPOSITE_BR);
    if (!cocosession_tv_input_supported(s->opts.machine, s->opts.tv_input))
        s->opts.tv_input = COCOSESSION_TV_COMPOSITE_BR;
    s->cur_machine = s->opts.machine;
    s->cur_tv_input = s->opts.tv_input;
    s->cur_ccr = s->opts.ccr;

    /* FUJINET MUST BE UP BEFORE THE EMULATOR.
     *
     * This is the opposite of the ADAM and Apple II targets, and the reason is
     * the direction of the transport. There the emulator listens and FujiNet
     * connects in; here FujiNet is the DriveWire server that listens on the
     * Becker port and XRoar connects *out* to it.
     *
     * And it is not enough to start it first: fujinet_start() returns as soon
     * as the devices are constructed, but the DriveWire bus then suspends
     * itself for a full second before it begins listening. HDB-DOS boots in
     * well under that, asks for its boot sector, gets nothing, and HANGS on
     * the banner with no OK prompt -- which looks exactly like a working
     * emulator until you try to type. So wait for the listener to actually
     * accept before starting the machine.
     *
     * A missing or broken runtime is not fatal: the machine still boots, just
     * with nothing on the other end of the Becker port. Nor is a timeout --
     * the patched becker.c keeps retrying, so the link comes up late rather
     * than never. */
    if (opts->enable_fujinet && fujinet_start(s) == 0)
        fujinet_wait_for_becker(s, 10000);

    s->stop_flag = 0;
    s->frame_serial = 0;
    s->frame_w = 0;
    s->frame_h = 0;
    s->frame_rate_mhz = 0;
    s->vs_ns = 0;
    s->vs_seen_ns = 0;
    s->vs_iv_us = 0;
    s->core_started = 0;
    s->running = 1;
    if (pthread_create(&s->emu_thread, NULL, emu_thread_main, s) != 0) {
        s->running = 0;
        session_set_error(s, "could not start the emulator thread");
        fujinet_stop(s);
        pthread_mutex_unlock(&s->lifecycle_mtx);
        return -1;
    }
    s->emu_thread_started = 1;

    /* Wait for the core to report itself up so the caller learns about a
     * failed xroar_init rather than being handed a black window and no error.
     * The wait is bounded so a wedged core cannot hang the frontend. */
    {
        int up;
        pthread_mutex_lock(&s->start_mtx);
        while (s->core_started == 0) {
            if (coco_cond_timedwait_ms(&s->start_cv, &s->start_mtx, 15000)
                == ETIMEDOUT)
                break;
        }
        up = s->core_started;
        pthread_mutex_unlock(&s->start_mtx);

        if (up != 1) {
            /* The emulator thread has already exited (or is wedged); reap it
             * so a later stop() does not join a thread twice. last_error was
             * set by the thread itself. */
            s->stop_flag = 1;
            pthread_join(s->emu_thread, NULL);
            s->emu_thread_started = 0;
            s->running = 0;
            fujinet_stop(s);
            if (!s->last_error[0])
                session_set_error(s, "the emulator core did not come up");
            pthread_mutex_unlock(&s->lifecycle_mtx);
            return -1;
        }
    }

    if (opts->enable_audio)
        audio_start(s);
    if (opts->enable_gamepad)
        gamepad_start(s);

    pthread_mutex_unlock(&s->lifecycle_mtx);
    return 0;
}

void cocosession_stop(cocosession *s)
{
    pthread_mutex_lock(&s->lifecycle_mtx);
    if (!s->emu_thread_started) {
        pthread_mutex_unlock(&s->lifecycle_mtx);
        return;
    }
    s->stop_flag = 1;
    if (s->debugger)
        cocodebug_resume_for_stop(s->debugger);
    pthread_mutex_lock(&s->start_mtx);
    if (s->core_started == 0) s->core_started = -1;
    pthread_cond_broadcast(&s->start_cv);
    pthread_mutex_unlock(&s->start_mtx);
    pthread_mutex_lock(&s->vs_mtx);
    pthread_cond_broadcast(&s->vs_cv);
    pthread_mutex_unlock(&s->vs_mtx);
    /* Unblock a fill_audio parked waiting for samples the stopped emulator
     * will never produce. */
    cocohost_audio_set_active(0);
    pthread_join(s->emu_thread, NULL);
    s->emu_thread_started = 0;
    audio_stop(s);
    gamepad_stop(s);
    fujinet_stop(s);
    cocohost_audio_set_active(1);
    s->running = 0;
    pthread_mutex_unlock(&s->lifecycle_mtx);
}

int cocosession_is_running(const cocosession *s)
{
    return s && s->running;
}

const char *cocosession_last_error(const cocosession *s)
{
    return s->last_error;
}

/****************************************************************************/
/** Live machine / video switching                                         **/
/****************************************************************************/

void cocosession_set_machine(cocosession *s, int machine)
{
    if (!s) return;
    machine = clamp_opt(machine, k_machines, COCOSESSION_MACHINE_COCO3);
    /* Switching to a CoCo 2 with RGB selected would leave a blank screen, so
     * carry the TV input to something that machine can actually produce. */
    if (!cocosession_tv_input_supported(machine, s->cur_tv_input))
        s->cur_tv_input = COCOSESSION_TV_COMPOSITE_BR;
    s->cur_machine = machine;
    cocosession_set_int(s, "machine", machine);
    cocosession_set_int(s, "tv_input", s->cur_tv_input);
    /* The host applies both together: selecting a machine resets its TV input,
     * so the two cannot be queued independently. */
    if (s->running)
        cocohost_set_machine(machine, s->cur_tv_input);
}

void cocosession_set_tv_input(cocosession *s, int tv_input)
{
    if (!s) return;
    tv_input = clamp_opt(tv_input, k_tv_inputs, COCOSESSION_TV_COMPOSITE_BR);
    if (!cocosession_tv_input_supported(s->cur_machine, tv_input))
        return;
    s->cur_tv_input = tv_input;
    cocosession_set_int(s, "tv_input", tv_input);
    if (s->running)
        cocohost_set_tv_input(tv_input);
}

void cocosession_set_ccr(cocosession *s, int ccr)
{
    if (!s) return;
    ccr = clamp_opt(ccr, k_ccrs, COCOSESSION_CCR_5BIT);
    s->cur_ccr = ccr;
    cocosession_set_int(s, "ccr", ccr);
    if (s->running)
        cocohost_set_ccr(ccr);
}

int cocosession_machine(const cocosession *s)  { return s ? s->cur_machine : 0; }
int cocosession_tv_input(const cocosession *s) { return s ? s->cur_tv_input : 0; }
int cocosession_ccr(const cocosession *s)      { return s ? s->cur_ccr : 0; }

/****************************************************************************/
/** Input / audio / misc accessors                                         **/
/****************************************************************************/

void cocosession_key(cocosession *s, int down, uint32_t keysym,
                     uint32_t unicode, int ctrl_down, int shift_down)
{
    unsigned scancode = 0;

    if (!s->running) return;
    if (!coco_key_from_event(keysym, unicode, ctrl_down, shift_down, &scancode))
        return;
    cocohost_inject_key(down, (int)scancode);
}

void cocosession_joystick(cocosession *s, int port, float x, float y)
{
    if (!s->running) return;
    /* The deadzone is applied by whoever produced these -- the SDL gamepad
     * backend, or a frontend driving the joystick from the mouse -- so pass
     * the reading straight through the range conversion. */
    cocohost_set_joystick_axis(port, 0, coco_axis_from_stick(x, 0.0f));
    cocohost_set_joystick_axis(port, 1, coco_axis_from_stick(y, 0.0f));
}

void cocosession_joystick_button(cocosession *s, int port, int button,
                                 int pressed)
{
    if (!s->running) return;
    cocohost_set_joystick_button(port, button, pressed);
}

void cocosession_reset(cocosession *s)
{
    /* Latched by the host and applied on the emulator thread between frames. */
    if (s->running) cocohost_core_reset();
}

int cocosession_render_audio(cocosession *s, int16_t *out, int nsamples)
{
    if (!s->running || s->audio_mute) {
        memset(out, 0, (size_t)nsamples * sizeof(int16_t));
        return nsamples;
    }
    cocohost_fill_audio(out, nsamples);
    fujinet_mix_audio(s, out, nsamples, 44100);
    return nsamples;
}

const char *cocosession_roms_path(const cocosession *s)  { return s->roms_dir; }
const char *cocosession_carts_path(const cocosession *s) { return s->carts_dir; }
const char *cocosession_config_path(const cocosession *s) { return s->config_dir; }
const char *cocosession_data_path(const cocosession *s)  { return s->data_dir; }
const char *cocosession_sd_path(const cocosession *s)    { return s->fujinet_sd; }

int cocosession_has_system_roms(const cocosession *s, int machine)
{
    return s ? roms_present_for_machine(s->roms_dir, machine) : 0;
}

const char *cocosession_fujinet_webui_url(const cocosession *s)
{
    return s->webui_url;
}

int cocosession_fujinet_running(const cocosession *s)
{
    return s->fujinet_running;
}

int cocosession_fujinet_copy_log(cocosession *s, char *dst, int max)
{
    return fujinet_copy_log(s, dst, max);
}

cocodebug *cocosession_debugger(cocosession *s)
{
    /* Created lazily: a session that never opens the debugger pays nothing,
     * and while it is disengaged the emulator loop costs one volatile read
     * per frame. */
    pthread_mutex_lock(&s->lifecycle_mtx);
    if (!s->debugger)
        s->debugger = cocodebug_create(s);
    pthread_mutex_unlock(&s->lifecycle_mtx);
    return s->debugger;
}

/****************************************************************************/
/** Media import                                                           **/
/****************************************************************************/

static const char *path_ext(const char *path)
{
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    if (!dot || (slash && dot < slash)) return "";
    return dot + 1;
}

static int ext_is(const char *ext, const char *want)
{
    for (; *ext && *want; ext++, want++)
        if ((*ext | 0x20) != *want) return 0;
    return *ext == '\0' && *want == '\0';
}

int cocosession_import_rom(cocosession *s, const char *src_path,
                           char *dest_out, int dest_sz)
{
    const char *base = strrchr(src_path, '/');
    char dst[COCO_PATH_MAX];

    base = base ? base + 1 : src_path;
    if (!*base) {
        session_set_error(s, "Not a file: %s", src_path);
        return -1;
    }

    mkdir_p(s->roms_dir);
    snprintf(dst, sizeof(dst), "%s/%s", s->roms_dir, base);
    if (copy_file(src_path, dst) != 0) {
        session_set_error(s, "Could not copy %s to %s", src_path, dst);
        return -1;
    }
    if (dest_out && dest_sz > 0)
        snprintf(dest_out, (size_t)dest_sz, "%s", dst);
    return 0;
}

int cocosession_import_media(cocosession *s, const char *src_path,
                             char *dest_out, int dest_sz)
{
    /* Disk and tape images go to FujiNet's SD directory, to be mounted from
     * the web UI. Cartridge images are XRoar's business, not FujiNet's, so
     * they go to the session's own cartridge directory. */
    static const char *const k_disk_exts[] = {
        "dsk", "os9", "vdk", "cas", "wav", NULL
    };
    static const char *const k_cart_exts[] = { "rom", "ccc", NULL };
    const char *ext = path_ext(src_path);
    const char *base = strrchr(src_path, '/');
    const char *dir = NULL;
    char dst[COCO_PATH_MAX];
    int i;

    base = base ? base + 1 : src_path;
    for (i = 0; k_disk_exts[i] && !dir; i++)
        if (ext_is(ext, k_disk_exts[i])) dir = s->fujinet_sd;
    for (i = 0; k_cart_exts[i] && !dir; i++)
        if (ext_is(ext, k_cart_exts[i])) dir = s->carts_dir;

    if (!dir) {
        session_set_error(s, "Unsupported media type \".%s\" (expected a disk "
                          "or tape image: .dsk .os9 .vdk .cas .wav, or a "
                          "cartridge: .rom .ccc)", ext);
        return -1;
    }
    if (!dir[0]) {
        session_set_error(s, "No FujiNet SD directory (FujiNet runtime "
                          "unavailable)");
        return -1;
    }

    mkdir_p(dir);
    snprintf(dst, sizeof(dst), "%s/%s", dir, base);
    if (copy_file(src_path, dst) != 0) {
        session_set_error(s, "Could not copy %s to %s", src_path, dst);
        return -1;
    }
    if (dest_out && dest_sz > 0)
        snprintf(dest_out, (size_t)dest_sz, "%s", dst);
    return 0;
}
