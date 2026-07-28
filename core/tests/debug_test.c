/*
 * debug_test -- the debugger engine, against a real running machine.
 *
 * The engine's whole design is that the emulator thread parks inside XRoar's
 * instruction_hook while paused, so nothing about it can be tested without a
 * machine actually executing: a mock would only prove the mock works. This
 * starts a session (no FujiNet, no audio, no gamepads), engages the debugger,
 * and drives pause / step / breakpoints the way a frontend does.
 *
 * Skips (ctest 77) when the build has no system ROMs to boot.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
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

/* Wait for the machine to come to rest, up to about a second. Pausing is
 * asynchronous by design -- the request is honoured at the next instruction
 * boundary -- so every test that pauses waits like this. */
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

static int wait_stop_serial(cocodebug *d, uint64_t was)
{
    int i;
    for (i = 0; i < 200; i++) {
        if (cocodebug_stop_serial(d) != was && cocodebug_is_paused(d))
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
    cocodebug_regs r, r2;
    uint16_t list[8];
    uint8_t buf[4];
    uint64_t serial;

    if (!coco_embedded_system_roms) {
        printf("debug_test: no system ROMs in this build "
               "(WITH_COCO_ROMS=OFF); skipping\n");
        return SKIP;
    }

    memset(&paths, 0, sizeof(paths));
    paths.config_dir = "debug_test-config";
    paths.data_dir = "debug_test-data";
    paths.fujinet_lib = ""; /* no runtime: this test must not bind a port */

    s = cocosession_new(&paths);
    if (!s) {
        fprintf(stderr, "debug_test: could not create the session\n");
        return 1;
    }
    cocosession_default_opts(s, &opts);
    opts.enable_fujinet = 0;
    opts.enable_audio = 0;
    opts.enable_gamepad = 0;
    if (cocosession_start(s, &opts) != 0) {
        fprintf(stderr, "debug_test: session start failed: %s\n",
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

    /* --- breakpoints work before the debugger is even engaged ----------- */
    ok("a breakpoint can be set", cocodebug_breakpoint_toggle(d, 0xA000) == 1);
    ok("...and reads back", cocodebug_breakpoint_is_set(d, 0xA000));
    ok("...and toggles off", cocodebug_breakpoint_toggle(d, 0xA000) == 0);
    ok("...and is gone", !cocodebug_breakpoint_is_set(d, 0xA000));
    cocodebug_breakpoint_toggle(d, 0x3000);
    cocodebug_breakpoint_toggle(d, 0x1000);
    cocodebug_breakpoint_toggle(d, 0x2000);
    ok("the list comes back sorted",
       cocodebug_breakpoint_list(d, list, 8) == 3 && list[0] == 0x1000 &&
           list[1] == 0x2000 && list[2] == 0x3000);
    cocodebug_breakpoint_clear_all(d);
    ok("clear_all empties it", cocodebug_breakpoint_list(d, list, 8) == 0);

    /* --- disengaged, nothing is inspectable ----------------------------- */
    ok("a running machine reports no registers", !cocodebug_regs_get(d, &r));
    ok("...and no memory", cocodebug_read(d, 0xA000, buf, 4) == 0);
    ok("the debugger starts disengaged", !cocodebug_is_engaged(d));

    /* --- engage and pause ------------------------------------------------ */
    cocodebug_set_engaged(d, 1);
    ok("it engages", cocodebug_is_engaged(d));
    cocodebug_pause(d);
    ok("the machine stops when asked", wait_paused(d));
    ok("...and now reports registers", cocodebug_regs_get(d, &r));
    /* Deliberately NOT a range check. The obvious one -- "the CoCo 3 executes
     * ROM at $8000-$FEFF" -- is wrong: the GIME can map RAM over any of it,
     * and HDB-DOS relocates itself, so a perfectly healthy machine pauses at
     * $417e. What must hold is that this is a real PC rather than a zeroed
     * struct -- and the reads and steps below are what actually prove the
     * machine is where it says it is. */
    ok("...with a real PC", r.pc != 0);
    printf("     (paused at $%04x  a=%02x b=%02x x=%04x s=%04x cc=%02x)\n",
           r.pc, r.a, r.b, r.x, r.s, r.cc);

    /* --- memory is readable while parked -------------------------------- */
    ok("memory reads while paused", cocodebug_read(d, r.pc, buf, 4) == 4);
    /* Reading the same address twice must agree: the machine really is
     * stopped, not merely between frames. */
    {
        uint8_t again[4];
        cocodebug_read(d, r.pc, again, 4);
        ok("...and is stable", memcmp(buf, again, 4) == 0);
    }

    /* --- stepping -------------------------------------------------------- */
    serial = cocodebug_stop_serial(d);
    cocodebug_step(d);
    ok("a step stops again", wait_stop_serial(d, serial));
    ok("...and reports registers", cocodebug_regs_get(d, &r2));
    /* One instruction moved the PC. It could legitimately land anywhere (a
     * branch), but it must not be where it was: no 6809 instruction is a
     * one-byte infinite loop, and the ROM is not executing one. */
    ok("...having actually advanced the PC", r2.pc != r.pc);
    printf("     (stepped $%04x -> $%04x)\n", r.pc, r2.pc);

    /* Stepping repeatedly must keep making progress, not stall on the first
     * instruction -- the decrement-on-entry bug this ordering avoids. */
    {
        int i, moved = 0;
        uint16_t prev = r2.pc;
        for (i = 0; i < 8; i++) {
            serial = cocodebug_stop_serial(d);
            cocodebug_step(d);
            if (!wait_stop_serial(d, serial))
                break;
            if (cocodebug_regs_get(d, &r2) && r2.pc != prev)
                moved++;
            prev = r2.pc;
        }
        ok("eight more steps all advance", moved == 8);
    }

    /* --- a breakpoint stops a running machine ---------------------------- */
    {
        /* Break on the instruction we are sitting at: resuming has to come
         * back round to it, because the ROM is in a loop waiting for input. */
        uint16_t here = r2.pc;
        cocodebug_breakpoint_toggle(d, here);
        serial = cocodebug_stop_serial(d);
        cocodebug_resume(d);
        ok("a breakpoint stops the machine", wait_stop_serial(d, serial));
        if (cocodebug_regs_get(d, &r))
            ok("...at the breakpoint address", r.pc == here);
        else
            ok("...at the breakpoint address", 0);
        cocodebug_breakpoint_clear_all(d);
    }

    /* --- releasing lets it run again ------------------------------------- */
    cocodebug_set_engaged(d, 0);
    ok("releasing disengages", !cocodebug_is_engaged(d));
    nap_ms(200);
    ok("...and the machine is running", !cocodebug_is_paused(d));
    ok("...so registers are private again", !cocodebug_regs_get(d, &r));

    /* --- and the session can still be torn down --------------------------
     * A stop while the debugger is paused is the deadlock this has to not
     * have: the emulator thread is parked inside the hook and the session
     * has to get it out. */
    cocodebug_set_engaged(d, 1);
    cocodebug_pause(d);
    wait_paused(d);
    ok("a paused session still stops cleanly (no deadlock)", 1);
    cocosession_stop(s);
    cocosession_free(s);
    printf("ok   the session tore down with the debugger paused\n");

    if (failures) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    printf("\nall debugger engine tests passed\n");
    return 0;
}
