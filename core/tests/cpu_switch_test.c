/*
 * cpu_switch_test -- the live CPU switch (cocosession_set_cpu / cocohost_
 * set_cpu), against a real running machine.
 *
 * Everything below the session API is exercised elsewhere without ROMs
 * (session_test's clamp/persist coverage), but the in-place hard-reset
 * mechanism itself -- mutating machine_config::cpu and resetting, the same
 * trick cocohost_set_machine uses -- only means something with a machine
 * actually running. This boots a CoCo 3, switches to HD6309 live, confirms
 * the debugger's disassembler picks it up (cocodebug_disassemble auto-
 * detects the live CPU part via part_component_by_id_is_a), switches back,
 * and confirms the machine survives both resets.
 *
 * Skips (ctest 77) when the build has no system ROMs to boot.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cocodebug.h"
#include "cocosession.h"
#include "roms_embedded.h"

#define SKIP 77

static int failures;

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

int main(void)
{
    cocosession_paths paths;
    cocosession_start_opts opts;
    cocosession *s;
    cocodebug *d;
    coco_dasm_line line;

    if (!coco_embedded_system_roms) {
        printf("cpu_switch_test: no system ROMs in this build "
               "(WITH_COCO_ROMS=OFF); skipping\n");
        return SKIP;
    }

    memset(&paths, 0, sizeof(paths));
    paths.config_dir = "cpu_switch_test-config";
    paths.data_dir = "cpu_switch_test-data";
    paths.fujinet_lib = ""; /* no runtime: this test must not bind a port */

    s = cocosession_new(&paths);
    if (!s) {
        fprintf(stderr, "cpu_switch_test: could not create the session\n");
        return 1;
    }
    cocosession_default_opts(s, &opts);
    opts.machine = COCOSESSION_MACHINE_COCO3;
    opts.cpu = COCOSESSION_CPU_MC6809;
    opts.enable_fujinet = 0;
    opts.enable_audio = 0;
    opts.enable_gamepad = 0;
    if (cocosession_start(s, &opts) != 0) {
        fprintf(stderr, "cpu_switch_test: session start failed: %s\n",
                cocosession_last_error(s));
        cocosession_free(s);
        return 1;
    }
    ok("boots with the default cpu (6809)",
       cocosession_cpu(s) == COCOSESSION_CPU_MC6809);

    d = cocosession_debugger(s);
    ok("the session hands out a debugger", d != NULL);
    if (!d) {
        cocosession_stop(s);
        cocosession_free(s);
        return 1;
    }

    cocodebug_set_engaged(d, 1);
    cocodebug_pause(d);
    ok("the machine pauses on 6809", wait_paused(d));
    ok("it disassembles at least one instruction",
       cocodebug_disassemble(d, 0xA000, &line, 1) == 1);
    cocodebug_set_engaged(d, 0);

    cocosession_set_cpu(s, COCOSESSION_CPU_HD6309);
    ok("the live setter reports 6309", cocosession_cpu(s) == COCOSESSION_CPU_HD6309);
    /* The switch hard-resets the machine in place, on the emulator thread;
     * give it a moment to land before re-engaging the debugger. */
    nap_ms(300);
    ok("the session is still running after the cpu switch",
       cocosession_is_running(s));

    cocodebug_set_engaged(d, 1);
    cocodebug_pause(d);
    ok("the machine pauses on 6309", wait_paused(d));
    ok("it still disassembles after switching to 6309",
       cocodebug_disassemble(d, 0xA000, &line, 1) == 1);
    cocodebug_set_engaged(d, 0);

    cocosession_set_cpu(s, COCOSESSION_CPU_MC6809);
    ok("switching back reports 6809", cocosession_cpu(s) == COCOSESSION_CPU_MC6809);
    nap_ms(300);
    ok("the session survives switching back too", cocosession_is_running(s));

    cocosession_stop(s);
    cocosession_free(s);

    if (failures) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    printf("\nall cpu_switch tests passed\n");
    return 0;
}
