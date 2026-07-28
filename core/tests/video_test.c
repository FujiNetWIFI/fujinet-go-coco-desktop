/*
 * video_test -- cocodebug_video_decode() against a real running machine.
 *
 * Like debug_test, this needs a real machine: the decode reads live hardware
 * component state (SAM/PIA1/GIME) and screen RAM, so a mock would only prove
 * the mock works.
 *
 * One machine per PROCESS, chosen by argv[1] ("coco3" or "coco2") -- not one
 * process running both sessions in turn. XRoar's own xroar_init()/
 * xroar_shutdown() cannot safely run twice in one process yet (see the TODO's
 * "Restarting a session" note); every other test in this suite only ever
 * creates one session for exactly that reason, and this one follows suit.
 * ctest registers this binary twice, once per machine, below.
 *
 * Skips (ctest 77) when the build has no system ROMs to boot.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cocodebug.h"
#include "cocosession.h"
#include "cocovideo.h"
#include "roms_embedded.h"

#define SKIP 77

static int failures;
static uint32_t g_pixels[COCOVIDEO_MAX_WIDTH * COCOVIDEO_MAX_HEIGHT];

static void ok(const char *what, int cond)
{
    if (cond) {
        printf("ok   %s\n", what);
    } else {
        printf("FAIL %s\n", what);
        failures++;
    }
}

static void nap_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static int wait_paused(cocodebug *d)
{
    int i;
    for (i = 0; i < 100; i++) {
        if (cocodebug_is_paused(d))
            return 1;
        nap_ms(10);
    }
    return 0;
}

/* How many distinct colours appear in the decoded frame -- a real booted
 * screen (banner text, or a coloured boot pattern) is never one flat
 * colour; a decode that silently returned zeroed/garbage memory would be. */
static int count_distinct(const uint32_t *px, int n)
{
    uint32_t seen[64];
    int nseen = 0, i, j;

    for (i = 0; i < n && nseen < 64; i++) {
        for (j = 0; j < nseen; j++)
            if (seen[j] == px[i])
                break;
        if (j == nseen)
            seen[nseen++] = px[i];
    }
    return nseen;
}

static int run_machine(const char *label, const char *config_dir,
                       const char *data_dir, int machine)
{
    cocosession_paths paths;
    cocosession_start_opts opts;
    cocosession *s;
    cocodebug *d;
    int out_w = 0, out_h = 0, n;
    int before = failures;

    memset(&paths, 0, sizeof(paths));
    paths.config_dir = config_dir;
    paths.data_dir = data_dir;
    paths.fujinet_lib = ""; /* no runtime: this test must not bind a port */

    s = cocosession_new(&paths);
    if (!s) {
        fprintf(stderr, "video_test: could not create the %s session\n",
                label);
        return 1;
    }
    cocosession_default_opts(s, &opts);
    opts.enable_fujinet = 0;
    opts.enable_audio = 0;
    opts.enable_gamepad = 0;
    opts.machine = machine;
    if (cocosession_start(s, &opts) != 0) {
        fprintf(stderr, "video_test: %s session start failed: %s\n", label,
                cocosession_last_error(s));
        cocosession_free(s);
        return 1;
    }

    d = cocosession_debugger(s);
    ok("the session hands out a debugger", d != NULL);
    if (!d) {
        cocosession_stop(s);
        cocosession_free(s);
        return 1;
    }

    /* Give the boot ROM time to reach its banner before parking it -- a
     * machine paused mid-reset has nothing interesting on screen yet. */
    nap_ms(2500);

    cocodebug_set_engaged(d, 1);
    cocodebug_pause(d);
    ok("the machine stops when asked", wait_paused(d));

    n = cocodebug_video_decode(d, g_pixels, &out_w, &out_h);
    ok("decodes while paused", n == 1);
    ok("...with a plausible width",
       out_w > 0 && out_w <= COCOVIDEO_MAX_WIDTH);
    ok("...with a plausible height",
       out_h > 0 && out_h <= COCOVIDEO_MAX_HEIGHT);
    if (n == 1 && out_w > 0 && out_h > 0) {
        int distinct = count_distinct(g_pixels, out_w * out_h);
        printf("     (%s: %dx%d, %d+ distinct colours)\n", label, out_w,
               out_h, distinct);
        ok("...and is not a single flat colour", distinct > 1);
    }

    cocodebug_set_engaged(d, 0);
    cocosession_stop(s);
    cocosession_free(s);

    return failures != before;
}

int main(int argc, char **argv)
{
    const char *which = (argc > 1) ? argv[1] : "coco3";
    int machine;
    const char *label, *config_dir, *data_dir;

    if (!coco_embedded_system_roms) {
        printf("video_test: no system ROMs in this build "
               "(WITH_COCO_ROMS=OFF); skipping\n");
        return SKIP;
    }

    if (strcmp(which, "coco2") == 0) {
        machine = COCOSESSION_MACHINE_COCO2;
        label = "CoCo 2 (VDG)";
        config_dir = "video_test-coco2-config";
        data_dir = "video_test-coco2-data";
    } else {
        machine = COCOSESSION_MACHINE_COCO3;
        label = "CoCo 3 (GIME)";
        config_dir = "video_test-coco3-config";
        data_dir = "video_test-coco3-data";
    }

    /* Running while not paused must report failure and touch nothing --
     * same contract as cocodebug_read/cocodebug_disassemble. */
    {
        int out_w = -1, out_h = -1;
        g_pixels[0] = 0xdeadbeefu;
        ok("no debugger, no decode",
           cocodebug_video_decode(NULL, g_pixels, &out_w, &out_h) == 0);
    }

    run_machine(label, config_dir, data_dir, machine);

    if (failures) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    printf("\nall video decode tests passed\n");
    return 0;
}
