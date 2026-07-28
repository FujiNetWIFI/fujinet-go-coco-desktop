/*
 * cocosession FujiNet runtime control: dlopen libfujinet.so and drive the
 * fujinet_desktop_* entry points (the desktop build of fujinet-pc-coco plus
 * the in-process entry wrapper, tools/fujinet/support/fujinet_desktop_entry.cpp).
 * Port of the Android fujinet_android.cpp wrapper.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "compat.h"
#include "dynlib.h"
#include "session_internal.h"

typedef int (*start_runtime_fn)(const char *root, const char *config,
                                const char *sd, const char *data,
                                int listen_port);
typedef void (*stop_runtime_fn)(void);
typedef const char *(*last_error_fn)(void);
typedef int (*read_audio_fn)(int16_t *out, int max_samples, int rate);
typedef void (*clear_audio_fn)(void);
typedef int (*copy_log_fn)(char *out, int max_bytes);

/* One runtime per process: the library owns background threads (web admin,
 * network listeners) that live inside its mapping, so it is loaded once and
 * NEVER dlclose'd -- unmapping it while any such thread still runs executes
 * freed code and crashes. A stopped runtime is restarted through the same
 * handle. Same pattern (and reasoning) as the Android wrapper. */
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static coco_dynlib g_handle;
static start_runtime_fn g_start;
static stop_runtime_fn g_stop;
static last_error_fn g_last_error;
static read_audio_fn g_read_audio;
static clear_audio_fn g_clear_audio;
static copy_log_fn g_copy_log;

static int load_library_locked(cocosession *s)
{
    char errbuf[256];
    if (g_handle) return 0;

    g_handle = coco_dynlib_open(s->fujinet_lib);
    if (!g_handle) {
        session_set_error(s, "FujiNet library load failed: %s",
                          coco_dynlib_error(errbuf, sizeof(errbuf)));
        return -1;
    }
    g_start = (start_runtime_fn)coco_dynlib_sym(
        g_handle, "fujinet_desktop_start_runtime");
    g_stop = (stop_runtime_fn)coco_dynlib_sym(
        g_handle, "fujinet_desktop_stop_runtime");
    g_last_error = (last_error_fn)coco_dynlib_sym(
        g_handle, "fujinet_desktop_last_error_message");
    g_read_audio = (read_audio_fn)coco_dynlib_sym(
        g_handle, "fujinet_desktop_read_audio");
    g_clear_audio = (clear_audio_fn)coco_dynlib_sym(
        g_handle, "fujinet_desktop_clear_audio");
    g_copy_log = (copy_log_fn)coco_dynlib_sym(
        g_handle, "fujinet_desktop_copy_recent_log");

    if (!g_start || !g_stop || !g_last_error) {
        session_set_error(s, "%s is missing the desktop runtime contract",
                          s->fujinet_lib);
        /* Leave the handle mapped (never dlclose); just mark it unusable. */
        g_start = NULL;
        return -1;
    }
    return 0;
}

int fujinet_start(cocosession *s)
{
    pthread_mutex_lock(&g_mtx);
    if (s->fujinet_running) {
        pthread_mutex_unlock(&g_mtx);
        return 0;
    }
    if (paths_provision_fujinet(s) != 0) {
        pthread_mutex_unlock(&g_mtx);
        fprintf(stderr, "cocosession: FujiNet runtime unavailable; "
                        "continuing without it\n");
        return -1;
    }
    if (load_library_locked(s) != 0) {
        pthread_mutex_unlock(&g_mtx);
        return -1;
    }
    /* Two of the firmware patches (fnFsSPIFFS's flash base and mgHttpClient's
     * CA bundle -- see tools/fujinet/build-fujinet-desktop.sh) root themselves
     * from this rather than from the CWD, so that FujiNet keeps working if
     * anything ever moves the process's working directory out from under it.
     * Both fall back to a relative path when it is unset, which is why leaving
     * it unset fails quietly rather than loudly: the CA store simply loads
     * empty and every https:// fetch starts failing like a network fault. */
    coco_setenv("FUJINET_RUNTIME_ROOT", s->fujinet_root);
    if (!g_start(s->fujinet_root, s->fujinet_config, s->fujinet_sd,
                 s->fujinet_data, COCOSESSION_BECKER_PORT)) {
        const char *err = g_last_error ? g_last_error() : NULL;
        session_set_error(s, "FujiNet runtime failed to start: %s",
                          err && *err ? err : "(unknown)");
        pthread_mutex_unlock(&g_mtx);
        return -1;
    }
    s->fujinet_running = 1;
    pthread_mutex_unlock(&g_mtx);
    return 0;
}

void fujinet_stop(cocosession *s)
{
    stop_runtime_fn stop = NULL;
    clear_audio_fn clear = NULL;

    pthread_mutex_lock(&g_mtx);
    if (s->fujinet_running) {
        stop = g_stop;
        clear = g_clear_audio;
        s->fujinet_running = 0;
    }
    pthread_mutex_unlock(&g_mtx);

    if (stop) stop();
    if (clear) clear();
}

int fujinet_copy_log(cocosession *s, char *dst, int max)
{
    copy_log_fn fn;
    (void)s;
    if (!dst || max <= 0) return 0;
    dst[0] = '\0';
    pthread_mutex_lock(&g_mtx);
    fn = g_copy_log;
    pthread_mutex_unlock(&g_mtx);
    return fn ? fn(dst, max) : 0;
}

/* Overlay FujiNet (SAM speech) audio onto the emulator mix, saturating.
 *
 * buf is interleaved stereo (nsamples counts int16 samples, so nsamples/2
 * frames) but the runtime produces MONO -- so each sample it hands back is
 * mixed into both channels of one frame. XRoar's sound interface is stereo
 * (see core/xroar/desktop/ao_desktop.c), so the same widening applies here as
 * on the Apple II target. */
void fujinet_mix_audio(cocosession *s, int16_t *buf, int nsamples, int rate)
{
    static int16_t overlay[2048];
    read_audio_fn fn;
    int frames_left;

    if (!s->fujinet_running || nsamples <= 0) return;
    pthread_mutex_lock(&g_mtx);
    fn = g_read_audio;
    pthread_mutex_unlock(&g_mtx);
    if (!fn) return;

    frames_left = nsamples / 2;

    while (frames_left > 0) {
        int chunk = frames_left > (int)(sizeof(overlay) / sizeof(overlay[0]))
                        ? (int)(sizeof(overlay) / sizeof(overlay[0]))
                        : frames_left;
        int produced = fn(overlay, chunk, rate);
        int i;
        if (produced <= 0) return;
        for (i = 0; i < produced; i++) {
            int c;
            for (c = 0; c < 2; c++) {
                int mixed = (int)buf[i * 2 + c] + (int)overlay[i];
                if (mixed > INT16_MAX) mixed = INT16_MAX;
                if (mixed < INT16_MIN) mixed = INT16_MIN;
                buf[i * 2 + c] = (int16_t)mixed;
            }
        }
        if (produced < chunk) return;
        buf += (size_t)produced * 2;
        frames_left -= produced;
    }
}
