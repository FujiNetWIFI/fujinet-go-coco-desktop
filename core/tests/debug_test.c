/*
 * debug_test -- the debugger engine, against a real running machine.
 *
 * The engine's whole design is that the emulator thread parks inside XRoar's
 * instruction_hook while paused, so nothing about it can be tested without a
 * machine actually executing: a mock would only prove the mock works. This
 * starts a session (no FujiNet, no audio, no gamepads), engages the debugger,
 * and drives pause / step / breakpoints the way a frontend does, then covers
 * the bus-tap breakpoints, the memory spaces, the trace and the regressions
 * for the four latent bugs (a session restart, a machine reset replacing the
 * instruction hook, debugger reads with side effects, and the stop race).
 *
 * One machine per PROCESS: argv[1] is "coco2" or "coco3" (default coco3),
 * and ctest registers the binary once per machine. Skips (ctest 77) when the
 * build has no system ROMs to boot.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cocodebug.h"
#include "cocomemtools.h"
#include "cocosession.h"
#include "cocoxroar.h"
#include "debugger_internal.h"
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
    for (i = 0; i < 300; i++) {
        if (cocodebug_is_paused(d))
            return 1;
        nap_ms(10);
    }
    return 0;
}

static int wait_stop_serial(cocodebug *d, uint64_t was)
{
    int i;
    cocodebug_stop stop;
    for (i = 0; i < 300; i++) {
        cocodebug_stop_snapshot(d, &stop);
        if (stop.serial != was && stop.paused)
            return 1;
        nap_ms(10);
    }
    return 0;
}

/* ---- the session ---------------------------------------------------------- */

static int start_session(cocosession *s, int machine)
{
    cocosession_start_opts opts;
    cocosession_default_opts(s, &opts);
    opts.machine = machine;
    opts.enable_fujinet = 0;
    opts.enable_audio = 0;
    opts.enable_gamepad = 0;
    return cocosession_start(s, &opts);
}

static void pause_now(cocodebug *d)
{
    cocodebug_pause(d);
    wait_paused(d);
}

/* Park, put a program at $0700, point the PC at it with interrupts masked,
 * run it for a while and park again. The program loops, so it stays put. */
static void run_snippet(cocodebug *d, const uint8_t *code, int n, int run_ms)
{
    cocodebug_mem_write(d, COCODEBUG_SPACE_CPU, 0x0700, code, n);
    cocodebug_regs_set(d, COCODEBUG_REG_PC, 0x0700);
    cocodebug_flag_set(d, COCODEBUG_CC_I | COCODEBUG_CC_F, 1);
    cocodebug_resume(d);
    nap_ms(run_ms);
    pause_now(d);
}

/* LDA $0600 / INCA / STA $0600 / BRA loop: reads, then writes, one byte. */
static const uint8_t k_rw_loop[] = {
    0xB6, 0x06, 0x00, 0x4C, 0xB7, 0x06, 0x00, 0x20, 0xF7
};

static int map_gap_free(cocodebug *d, int *nregions)
{
    cocodebug_region map[64];
    int count = cocodebug_memory_map(d, map, 64), pos;
    unsigned next = 0;

    if (nregions)
        *nregions = count;
    if (count < 2)
        return 0;
    for (pos = 0; pos < count; pos++) {
        if (map[pos].lo != next || map[pos].hi < map[pos].lo)
            return 0;
        next = (unsigned)map[pos].hi + 1u;
    }
    return next == 0x10000u;
}

/* ---- observing that reads consume nothing ----------------------------------- */

static unsigned g_cart_reads;
static uint8_t (*g_cart_read_real)(struct cart *, uint16_t, _Bool, _Bool, uint8_t);

static uint8_t counting_cart_read(struct cart *cart, uint16_t address, _Bool p2,
                                  _Bool r2, uint8_t data)
{
    g_cart_reads++;
    return g_cart_read_real(cart, address, p2, r2, data);
}

/* ---- the sections ------------------------------------------------------------- */

static void check_before_engaging(cocodebug *d)
{
    cocodebug_regs r;
    uint16_t list[8];
    uint8_t buf[4];
    cocodebug_local locals[8];
    int fid, line;

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
    ok("...and cannot set a register", !cocodebug_regs_set(d, COCODEBUG_REG_A, 1));
    ok("the debugger starts disengaged", !cocodebug_is_engaged(d));

    /* --- no C source info loaded: every query is a safe miss ------------- */
    ok("no source info is available yet", !cocodebug_source_available(d));
    ok("line_at misses with nothing loaded", !cocodebug_source_line_at(d, 0xA000, &fid, &line));
    ok("file_count is 0", cocodebug_source_file_count(d) == 0);
    ok("file_path is NULL for any id", cocodebug_source_file_path(d, 0) == NULL);
    ok("a source breakpoint can't resolve a line that doesn't exist",
       cocodebug_source_breakpoint_toggle(d, 0, 1) == -2);
    ok("...and isn't reported set either", !cocodebug_source_breakpoint_is_set(d, 0, 1));
    ok("locals_get returns none on a running machine", cocodebug_locals_get(d, locals, 8) == 0);
    cocodebug_step_line(d);
    cocodebug_step_over_line(d);
    ok("stepping by line without pausing first doesn't crash", 1);
}

static void check_basics(cocodebug *d)
{
    cocodebug_regs r, r2;
    uint8_t buf[4], again[4];
    uint64_t serial;
    int moved = 0, i;
    uint16_t prev;
    cocodebug_local locals[8];
    uint16_t before, here;

    cocodebug_set_engaged(d, 1);
    ok("it engages", cocodebug_is_engaged(d));
    cocodebug_pause(d);
    ok("the machine stops when asked", wait_paused(d));
    ok("...and now reports registers", cocodebug_regs_get(d, &r));
    ok("...with a real PC", r.pc != 0);
    printf("     (paused at $%04x  a=%02x b=%02x x=%04x s=%04x cc=%02x%s)\n", r.pc, r.a, r.b,
           r.x, r.s, r.cc, r.is6309 ? " 6309" : "");

    ok("memory reads while paused", cocodebug_read(d, r.pc, buf, 4) == 4);
    cocodebug_read(d, r.pc, again, 4);
    ok("...and is stable", memcmp(buf, again, 4) == 0);

    serial = cocodebug_stop_serial(d);
    cocodebug_step(d);
    ok("a step stops again", wait_stop_serial(d, serial));
    ok("...and reports registers", cocodebug_regs_get(d, &r2));
    ok("...having actually advanced the PC", r2.pc != r.pc);
    prev = r2.pc;
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

    ok("locals_get misses while paused with no source loaded", cocodebug_locals_get(d, locals, 8) == 0);
    cocodebug_regs_get(d, &r);
    before = r.pc;
    serial = cocodebug_stop_serial(d);
    cocodebug_step_line(d);
    ok("step_line falls back to a real step and stops again", wait_stop_serial(d, serial));
    ok("...having actually advanced the PC", cocodebug_regs_get(d, &r2) && r2.pc != before);

    here = r2.pc;
    cocodebug_breakpoint_toggle(d, here);
    serial = cocodebug_stop_serial(d);
    cocodebug_resume(d);
    ok("a breakpoint stops the machine", wait_stop_serial(d, serial));
    ok("...at the breakpoint address", cocodebug_regs_get(d, &r) && r.pc == here);
    {
        char reason[160];
        int address = 0;
        cocodebug_stop_reason(d, reason, sizeof reason, &address);
        printf("     (%s)\n", reason);
        ok("...and says so", strstr(reason, "breakpoint: exec $") != NULL && address == here);
    }
    cocodebug_breakpoint_clear_all(d);
}

/* Bug 4: the reason, PC and serial are readable the instant the machine is
 * seen paused. */
static void check_stop_race(cocodebug *d)
{
    int round, bad = 0, behind = 0;
    cocodebug_stop stop;

    for (round = 0; round < 40; round++) {
        uint64_t was = cocodebug_stop_serial(d);
        char reason[160];
        int address = 0, spins = 0;

        cocodebug_resume(d);
        nap_ms(3);
        cocodebug_pause(d);
        while (!cocodebug_is_paused(d) && spins++ < 5000000)
            ;
        /* the instant paused flips, with nothing in between */
        cocodebug_stop_reason(d, reason, sizeof reason, &address);
        cocodebug_stop_snapshot(d, &stop);
        if (!strcmp(reason, "running") || address < 0)
            bad++;
        if (!stop.paused || stop.serial == was || stop.pc < 0)
            behind++;
    }
    ok("stop reason and address are readable the instant paused flips", bad == 0);
    ok("...and so are the snapshot's serial and PC", behind == 0);
    if (!cocodebug_is_paused(d))
        wait_paused(d);
}

/* Bug 3: the debugger's reads consume nothing. */
static void check_reads_have_no_side_effects(cocodebug *d)
{
    struct machine *mach = xroar.machine;
    struct cart *cart = (struct cart *)part_component_by_id_is_a(&mach->part, "cart", "dragon-cart");
    uint8_t buf[0x20];
    cocodebug_line lines[8];
    cocodebug_region map[64];
    uint32_t hits[4];
    static const uint8_t needle[] = { 0xFF, 0xFF };

    ok("a cartridge (the Becker port) is fitted", cart != NULL);
    if (!cart)
        return;
    g_cart_read_real = cart->read;
    g_cart_reads = 0;
    cart->read = counting_cart_read;

    cocodebug_read(d, 0xFF40, buf, sizeof buf);
    cocodebug_mem_read(d, COCODEBUG_SPACE_CPU, 0xFF40, buf, sizeof buf);
    cocodebug_disassemble_ex(d, 0xFF40, lines, 8, NULL);
    cocodebug_memory_map(d, map, 64);
    cocodebug_search(d, COCODEBUG_SPACE_CPU, needle, 2, 0xFF, 0, 0xFF00, 0xFF5F, hits, 4);
    ok("Memory and disassembly of $FF40-$FF5F reach no cartridge (no FujiNet byte consumed)",
       g_cart_reads == 0);
    (void)mach->read_byte(mach, 0xFF42, 0);
    ok("the control: a bus read of $FF42 does reach it", g_cart_reads > 0);
    cart->read = g_cart_read_real;
}

/* Bug 2: a reset takes the hook (XRoar's print breakpoint is re-added); ours
 * comes back, chained to it, and breakpoints still fire. */
static void reset_and_check(cocosession *s, cocodebug *d)
{
    int ours = 0, chained = 0;
    cocodebug_regs r;
    uint64_t serial;
    uint16_t here;

    cocodebug_hook_state(d, &ours, &chained);
    ok("before a reset our hook is in, chained to XRoar's", ours && chained);
    cocodebug_resume(d);
    nap_ms(100);
    cocosession_reset(s);
    nap_ms(700);
    pause_now(d);
    ok("a reset leaves the machine pausable", cocodebug_is_paused(d));
    cocodebug_hook_state(d, &ours, &chained);
    ok("after a reset our hook is back in", ours);
    ok("...and still chained to XRoar's (the printer hook survives)", chained);

    cocodebug_regs_get(d, &r);
    here = r.pc;
    cocodebug_breakpoint_toggle(d, here);
    serial = cocodebug_stop_serial(d);
    cocodebug_resume(d);
    ok("a breakpoint still fires after a reset", wait_stop_serial(d, serial));
    ok("...at its address", cocodebug_regs_get(d, &r) && r.pc == here);
    cocodebug_breakpoint_clear_all(d);
}

static void check_breakpoint_kinds(cocodebug *d)
{
    cocodebug_stop stop;
    cocodebug_breakpoint bps[8];
    cocodebug_line line[4];
    uint64_t serial;
    uint8_t value;
    int id, count;

    run_snippet(d, k_rw_loop, sizeof k_rw_loop, 200);
    ok("the snippet loop runs (the byte at $0600 counts)",
       cocodebug_mem_read(d, COCODEBUG_SPACE_CPU, 0x0600, &value, 1) == 1);

    /* write */
    id = cocodebug_breakpoint_add(d, COCODEBUG_BP_WRITE, 0x0600, 0x0600, "value != old");
    ok("a write breakpoint with a condition is accepted", id > 0);
    ok("a malformed condition is refused",
       cocodebug_breakpoint_add(d, COCODEBUG_BP_WRITE, 0x0600, 0x0600, "value ==") < 0);
    serial = cocodebug_stop_serial(d);
    cocodebug_resume(d);
    ok("a write breakpoint stops the machine", wait_stop_serial(d, serial));
    cocodebug_stop_snapshot(d, &stop);
    printf("     (%s)\n", stop.reason);
    ok("...with 'wrote $0600 = $X (was $Y) at $PC'",
       strstr(stop.reason, "wrote $0600 = $") && strstr(stop.reason, "(was $") &&
       strstr(stop.reason, ") at $0707"));
    ok("...PC is the instruction after the store", stop.pc == 0x0707);
    {
        unsigned new_value, old_value;
        ok("...value and old differ by one (INCA)",
           sscanf(strstr(stop.reason, "= $"), "= $%x (was $%x", &new_value, &old_value) == 2 &&
           ((old_value + 1) & 0xFF) == new_value);
    }
    count = cocodebug_breakpoints_list(d, bps, 8);
    ok("...and the hit is counted", count == 1 && bps[0].hits == 1);
    cocodebug_breakpoint_clear_hits(d);
    cocodebug_breakpoints_list(d, bps, 8);
    ok("Clear Hits zeroes it", bps[0].hits == 0);
    cocodebug_breakpoint_remove(d, id);

    /* a false condition never stops */
    id = cocodebug_breakpoint_add(d, COCODEBUG_BP_WRITE, 0x0600, 0x0600, "value == old");
    serial = cocodebug_stop_serial(d);
    cocodebug_resume(d);
    nap_ms(150);
    ok("a false condition does not stop the machine", !cocodebug_is_paused(d));
    pause_now(d);
    cocodebug_breakpoint_remove(d, id);

    /* read */
    id = cocodebug_breakpoint_add(d, COCODEBUG_BP_READ, 0x0600, 0x0600, NULL);
    serial = cocodebug_stop_serial(d);
    cocodebug_resume(d);
    ok("a read breakpoint stops the machine", wait_stop_serial(d, serial));
    cocodebug_stop_snapshot(d, &stop);
    ok("...with 'read $0600 = $X at $0703'", strstr(stop.reason, "read $0600 = $") &&
       stop.pc == 0x0703 && strstr(stop.reason, " at $0703"));
    cocodebug_breakpoint_remove(d, id);

    /* access: whichever comes first, the read */
    cocodebug_regs_set(d, COCODEBUG_REG_PC, 0x0700);
    id = cocodebug_breakpoint_add(d, COCODEBUG_BP_ACCESS, 0x0600, 0x0600, NULL);
    serial = cocodebug_stop_serial(d);
    cocodebug_resume(d);
    ok("an access breakpoint stops the machine", wait_stop_serial(d, serial));
    cocodebug_stop_snapshot(d, &stop);
    ok("...on the read", strstr(stop.reason, "read $0600") != NULL);
    serial = cocodebug_stop_serial(d);
    cocodebug_resume(d);
    ok("...and again on the next access", wait_stop_serial(d, serial));
    cocodebug_stop_snapshot(d, &stop);
    ok("...which is the write", strstr(stop.reason, "wrote $0600") != NULL);
    cocodebug_breakpoint_remove(d, id);

    /* exec with a range, disabled and enabled */
    id = cocodebug_breakpoint_add(d, COCODEBUG_BP_EXEC, 0x0704, 0x0704, NULL);
    cocodebug_breakpoint_enable(d, id, 0);
    cocodebug_disassemble_ex(d, 0x0704, line, 1, NULL);
    ok("a disabled execute breakpoint is the grey dot", !line[0].has_breakpoint && line[0].bp_disabled);
    serial = cocodebug_stop_serial(d);
    cocodebug_resume(d);
    nap_ms(100);
    ok("...and does not stop the machine", !cocodebug_is_paused(d));
    pause_now(d);
    cocodebug_breakpoint_enable(d, id, 1);
    cocodebug_disassemble_ex(d, 0x0704, line, 1, NULL);
    ok("an enabled one is the red dot", line[0].has_breakpoint && !line[0].bp_disabled);
    ok("the disassembly lists every opcode byte (EC A4, not EC)",
       line[0].length > 0 && (int)strlen(line[0].bytes) == line[0].length * 3 - 1);
    serial = cocodebug_stop_serial(d);
    cocodebug_resume(d);
    ok("...and stops the machine", wait_stop_serial(d, serial));
    cocodebug_stop_snapshot(d, &stop);
    ok("...at $0704 'breakpoint: exec $0704'", stop.pc == 0x0704 &&
       !strcmp(stop.reason, "breakpoint: exec $0704"));
    cocodebug_breakpoint_remove(d, id);
    count = cocodebug_breakpoints_list(d, bps, 8);
    ok("the table is empty again", count == 0);

    /* a PHYS write breakpoint, CoCo 3 only */
    {
        cocodebug_region map[64];
        int regions = cocodebug_memory_map(d, map, 64), pos;
        int32_t base = -1;
        for (pos = 0; pos < regions; pos++)
            if (map[pos].kind == COCODEBUG_REGION_RAM && map[pos].lo == 0 && map[pos].physical_base >= 0)
                base = map[pos].physical_base;
        if (cocodebug_space_size(d, COCODEBUG_SPACE_PHYS)) {
            ok("the CoCo 3 RAM region reports its physical base", base >= 0);
            id = cocodebug_breakpoint_add(d, COCODEBUG_BP_WRITE | COCODEBUG_BP_PHYS,
                                          (uint32_t)base + 0x600, (uint32_t)base + 0x600, NULL);
            ok("a physical write breakpoint is accepted", id > 0);
            ok("...but not an execute one",
               cocodebug_breakpoint_add(d, COCODEBUG_BP_EXEC | COCODEBUG_BP_PHYS, 0, 0, NULL) < 0);
            serial = cocodebug_stop_serial(d);
            cocodebug_resume(d);
            ok("it stops on the store to that RAM element", wait_stop_serial(d, serial));
            cocodebug_stop_snapshot(d, &stop);
            printf("     (%s)\n", stop.reason);
            ok("...'wrote phys $...'", strstr(stop.reason, "wrote phys $") != NULL);
            cocodebug_breakpoint_remove(d, id);
        } else {
            ok("a physical breakpoint is refused on a CoCo 1/2",
               cocodebug_breakpoint_add(d, COCODEBUG_BP_WRITE | COCODEBUG_BP_PHYS, 0, 0, NULL) < 0);
            ok("...which has no physical space", cocodebug_space_size(d, COCODEBUG_SPACE_PHYS) == 0);
        }
    }
}

static void check_registers(cocodebug *d)
{
    cocodebug_regs r;
    const int is6309 = cocodebug_cpu_is_6309(d);

    pause_now(d);
    ok("set A", cocodebug_regs_set(d, COCODEBUG_REG_A, 0x12) && cocodebug_regs_get(d, &r) && r.a == 0x12);
    ok("set B", cocodebug_regs_set(d, COCODEBUG_REG_B, 0x34) && cocodebug_regs_get(d, &r) && r.b == 0x34);
    ok("set D sets A:B", cocodebug_regs_set(d, COCODEBUG_REG_D, 0xABCD) && cocodebug_regs_get(d, &r) &&
       r.a == 0xAB && r.b == 0xCD);
    ok("set X Y U S DP",
       cocodebug_regs_set(d, COCODEBUG_REG_X, 0x1111) && cocodebug_regs_set(d, COCODEBUG_REG_Y, 0x2222) &&
       cocodebug_regs_set(d, COCODEBUG_REG_U, 0x3333) && cocodebug_regs_set(d, COCODEBUG_REG_S, 0x4444) &&
       cocodebug_regs_set(d, COCODEBUG_REG_DP, 0x55) && cocodebug_regs_get(d, &r) && r.x == 0x1111 &&
       r.y == 0x2222 && r.u == 0x3333 && r.s == 0x4444 && r.dp == 0x55);
    cocodebug_regs_set(d, COCODEBUG_REG_CC, 0x00);
    ok("set one CC flag", cocodebug_flag_set(d, COCODEBUG_CC_Z, 1) && cocodebug_regs_get(d, &r) &&
       r.cc == COCODEBUG_CC_Z);
    ok("clear it", cocodebug_flag_set(d, COCODEBUG_CC_Z, 0) && cocodebug_regs_get(d, &r) && r.cc == 0);
    ok("set the whole CC", cocodebug_regs_set(d, COCODEBUG_REG_CC, 0xD5) && cocodebug_regs_get(d, &r) &&
       r.cc == 0xD5);
    ok("E F W V MD exist only on a 6309",
       cocodebug_regs_set(d, COCODEBUG_REG_E, 0x77) == is6309 &&
       cocodebug_regs_set(d, COCODEBUG_REG_MD, 0x01) == is6309);
    if (is6309) {
        cocodebug_regs_set(d, COCODEBUG_REG_W, 0xBEEF);
        cocodebug_regs_get(d, &r);
        ok("6309: W is E:F", r.w == 0xBEEF && r.e == 0xBE && r.f == 0xEF && r.is6309);
        cocodebug_regs_set(d, COCODEBUG_REG_MD, 0x00);
    }
    ok("a PC edit is read back", cocodebug_regs_set(d, COCODEBUG_REG_PC, 0x0700) &&
       cocodebug_regs_get(d, &r) && r.pc == 0x0700);
}

static void check_memory_and_search(cocodebug *d)
{
    uint8_t buf[16];
    static const uint8_t text[] = { 'F', 'U', 'J', 'I', 'Q', 'Z', 'X' };
    uint32_t hits[8], found = 0;
    uint8_t pat[COCOSEARCH_MAX], mask;
    int count, flags;
    uint32_t size;

    pause_now(d);
    ok("Poke stores into RAM", cocodebug_mem_write(d, COCODEBUG_SPACE_CPU, 0x0900, text, sizeof text) == 7);
    cocodebug_mem_read(d, COCODEBUG_SPACE_CPU, 0x0900, buf, 7);
    ok("...and reads back", !memcmp(buf, text, 7));
    {
        cocodebug_region map[64];
        int regions = cocodebug_memory_map(d, map, 64), pos;
        for (pos = 0; pos < regions; pos++)
            if (map[pos].kind == COCODEBUG_REGION_ROM && map[pos].hi < 0xFF00) {
                uint8_t was = 0, now = 0;
                cocodebug_mem_read(d, COCODEBUG_SPACE_CPU, map[pos].lo, &was, 1);
                ok("Poke leaves ROM alone", cocodebug_mem_write(d, COCODEBUG_SPACE_CPU, map[pos].lo, text, 1) == 0 &&
                   cocodebug_mem_read(d, COCODEBUG_SPACE_CPU, map[pos].lo, &now, 1) == 1 && now == was);
                break;
            }
    }
    ok("the CPU space is 64K", cocodebug_space_size(d, COCODEBUG_SPACE_CPU) == 0x10000);
    ok("a read past the end is clamped", cocodebug_mem_read(d, COCODEBUG_SPACE_CPU, 0xFFFE, buf, 8) == 2);

    cocosearch_parse(COCOSEARCH_TEXT, "fujiqzx", pat, &count, &mask, &flags);
    ok("search finds a text, ignoring case",
       cocodebug_search(d, COCODEBUG_SPACE_CPU, pat, count, mask, flags, 0x0800, 0x0A00, hits, 8) == 1 &&
       hits[0] == 0x0900);
    cocosearch_parse(COCOSEARCH_HEX, "46 55 4A 49 51 5A 58", pat, &count, &mask, &flags);
    ok("search finds hex bytes", cocodebug_search(d, COCODEBUG_SPACE_CPU, pat, count, mask, flags, 0x0800,
                                                  0x0A00, hits, 8) == 1);
    ok("'to' bounds the start address, not the end",
       cocodebug_search(d, COCODEBUG_SPACE_CPU, pat, count, mask, flags, 0x0900, 0x0900, hits, 8) == 1 &&
       cocodebug_search(d, COCODEBUG_SPACE_CPU, pat, count, mask, flags, 0x0901, 0x0A00, hits, 8) == 0);
    ok("search_next from before it finds it",
       cocodebug_search_next(d, COCODEBUG_SPACE_CPU, pat, count, mask, flags, 0x0100, 0, &found) == 1 &&
       found == 0x0900);
    ok("...wrapping round from after it",
       cocodebug_search_next(d, COCODEBUG_SPACE_CPU, pat, count, mask, flags, 0x0900, 0, &found) == 1 &&
       found == 0x0900);
    ok("...and backwards",
       cocodebug_search_next(d, COCODEBUG_SPACE_CPU, pat, count, mask, flags, 0x0A00, 1, &found) == 1 &&
       found == 0x0900);
    {
        static const uint8_t none[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03 };
        cocodebug_mem_write(d, COCODEBUG_SPACE_CPU, 0x0A00, none, 7);
        cocodebug_mem_write(d, COCODEBUG_SPACE_CPU, 0x0A00, none, 0);
        ok("a masked search matches under the mask",
           cocodebug_search(d, COCODEBUG_SPACE_CPU, none, 1, 0xF0, 0, 0x0A00, 0x0A00, hits, 8) == 1);
    }

    size = cocodebug_space_size(d, COCODEBUG_SPACE_PHYS);
    if (size) {
        uint8_t high[8];
        ok("PHYS is 512K or 2M", size == 0x80000 || size == 0x200000);
        ok("PHYS poke and read", cocodebug_mem_write(d, COCODEBUG_SPACE_PHYS, size - 7, text, 7) == 7 &&
           cocodebug_mem_read(d, COCODEBUG_SPACE_PHYS, size - 7, high, 7) == 7 && !memcmp(high, text, 7));
        cocosearch_parse(COCOSEARCH_TEXT, "fujiqzx", pat, &count, &mask, &flags);
        ok("PHYS search finds it at the top of RAM",
           cocodebug_search(d, COCODEBUG_SPACE_PHYS, pat, count, mask, flags, 0, size - 1, hits, 8) >= 1);
        ok("PHYS reads past the end are clamped",
           cocodebug_mem_read(d, COCODEBUG_SPACE_PHYS, size - 2, high, 8) == 2);
    } else {
        ok("a CoCo 1/2 has no PHYS space", cocodebug_mem_read(d, COCODEBUG_SPACE_PHYS, 0, buf, 4) == 0);
    }
}

static void check_memory_map(cocodebug *d)
{
    int count = 0, pos;
    cocodebug_region map[64];
    static const uint8_t set_type1[] = { 0xB7, 0xFF, 0xDF, 0x20, 0xFE };   /* STA $FFDF; BRA * */
    static const uint8_t set_type0[] = { 0xB7, 0xFF, 0xDE, 0x20, 0xFE };   /* STA $FFDE; BRA * */

    pause_now(d);
    ok("the CPU map is gap-free over $0000-$FFFF", map_gap_free(d, &count));
    printf("     (%d regions)\n", count);
    {
        int regions = cocodebug_memory_map(d, map, 64), have_ram = 0, have_pia = 0;
        for (pos = 0; pos < regions; pos++) {
            if (map[pos].kind == COCODEBUG_REGION_RAM && map[pos].lo == 0)
                have_ram = 1;
            if (!strcmp(map[pos].name, "PIA0"))
                have_pia = 1;
        }
        ok("...with RAM at $0000 and PIA0", have_ram && have_pia);
    }

    if (cocodebug_space_size(d, COCODEBUG_SPACE_PHYS) == 0) {
        /* SAM map type 1: 64K RAM, no BASIC ROM */
        int rom_before = 0, rom_after = 0, regions;
        regions = cocodebug_memory_map(d, map, 64);
        for (pos = 0; pos < regions; pos++)
            rom_before += map[pos].kind == COCODEBUG_REGION_ROM;
        run_snippet(d, set_type1, sizeof set_type1, 150);
        ok("SAM map type 1: still gap-free", map_gap_free(d, &count));
        regions = cocodebug_memory_map(d, map, 64);
        for (pos = 0; pos < regions; pos++) {
            rom_after += map[pos].kind == COCODEBUG_REGION_ROM && strcmp(map[pos].name, "VECTORS");
            if (map[pos].lo <= 0x8000 && map[pos].hi >= 0x8000)
                ok("...and $8000 is RAM there", map[pos].kind == COCODEBUG_REGION_RAM);
        }
        ok("...BASIC's ROM is gone", rom_before > 0 && rom_after == 0);
        run_snippet(d, set_type0, sizeof set_type0, 150);
        ok("map type 0 again: gap-free", map_gap_free(d, &count));
    } else {
        /* the GIME: INIT0 ROM map bits and the constant-RAM bit */
        uint8_t init0 = 0;
        unsigned variant;
        cocodebug_mem_read(d, COCODEBUG_SPACE_CPU, 0xFF90, &init0, 1);
        for (variant = 0; variant < 4; variant++) {
            uint8_t code[] = { 0x86, 0, 0xB7, 0xFF, 0x90, 0x20, 0xFE };   /* LDA #n; STA $FF90; BRA * */
            char what[64];
            code[1] = (uint8_t)((init0 & ~0x0Bu) | variant | (variant == 3 ? 0x08 : 0));
            run_snippet(d, code, sizeof code, 150);
            snprintf(what, sizeof what, "GIME INIT0 $%02X: the map is gap-free", code[1]);
            ok(what, map_gap_free(d, &count));
        }
        {
            uint8_t restore[] = { 0x86, 0, 0xB7, 0xFF, 0x90, 0x20, 0xFE };
            restore[1] = init0;
            run_snippet(d, restore, sizeof restore, 150);
            ok("INIT0 restored: gap-free", map_gap_free(d, &count));
        }
    }
}

static void check_trace(cocodebug *d)
{
    cocodebug_trace_entry ring[64];
    uint8_t value = 0;
    int count, pos, branches = 0, descending = 1;
    char text[256];
    uint64_t latest;

    pause_now(d);
    ok("the trace starts off and empty", !cocodebug_trace_enabled(d) && cocodebug_trace_count(d) == 0);
    cocodebug_trace_enable(d, 1);
    ok("it enables", cocodebug_trace_enabled(d));
    run_snippet(d, k_rw_loop, sizeof k_rw_loop, 250);
    count = cocodebug_trace_count(d);
    printf("     (%d entries)\n", count);
    ok("instructions were recorded", count > 500);
    count = cocodebug_trace_read(d, ring, 64);
    ok("the newest 64 come back", count == 64);
    latest = ring[0].cycles;
    for (pos = 1; pos < count; pos++)
        if (ring[pos].cycles > ring[pos - 1].cycles)
            descending = 0;
    ok("newest first, cycles never go backwards", descending && latest > 0);
    ok("an entry is the snippet's instruction",
       ring[0].pc == 0x0700 || ring[0].pc == 0x0703 || ring[0].pc == 0x0704 || ring[0].pc == 0x0707);
    for (pos = 0; pos < count; pos++) {
        if (cocodebug_trace_match(&ring[pos], COCODEBUG_TRACE_BRANCHES, 0, 0)) {
            branches++;
            if (ring[pos].pc != 0x0707)
                descending = 0;
        }
    }
    ok("BRANCHES keeps only the BRA", branches > 10 && branches < 30 && descending);
    cocodebug_trace_format(&ring[0], text, sizeof text);
    printf("     (%s)\n", text);
    ok("an entry formats with its mnemonic and registers",
       strstr(text, "A=") && strstr(text, "CC=") && (strstr(text, "LDA") || strstr(text, "INCA") ||
                                                    strstr(text, "STA") || strstr(text, "BRA")));
    ok("an entry carries its bytes", ring[0].len >= 1 && ring[0].bytes[0] != 0);
    ok("the beam position is sane", ring[0].line >= 0 && ring[0].line < 320 && ring[0].hpos >= 0 &&
       ring[0].hpos < 230);
    ok("RANGE keeps only a range",
       cocodebug_trace_match(&ring[0], COCODEBUG_TRACE_RANGE, 0x0700, 0x0710) &&
       !cocodebug_trace_match(&ring[0], COCODEBUG_TRACE_RANGE, 0x8000, 0x9000));
    cocodebug_mem_read(d, COCODEBUG_SPACE_CPU, 0x0600, &value, 1);
    cocodebug_trace_clear(d);
    ok("Clear empties it", cocodebug_trace_count(d) == 0);
    cocodebug_trace_enable(d, 0);
    ok("it disables", !cocodebug_trace_enabled(d));
}

static void check_symbols(cocodebug *d)
{
    int picks[64], count, before = cocodebug_symbols_count(d);
    uint16_t addr = 0;
    char name[48];
    uint16_t found;

    ok("the built-in table has the BASIC vectors", before >= 6);
    cocodebug_symbol_add(d, 0x0900, "MyRoutine");
    cocodebug_symbol_add(d, 0x0901, "myother");
    ok("a symbol is added and found", cocodebug_symbol_find(d, "MyRoutine", &found) && found == 0x0900 &&
       cocodebug_symbols_count(d) == before + 2);
    count = cocodebug_symbols_filter(d, "MYR", picks, 64);
    ok("the filter is case-insensitive", count == 1 && cocodebug_symbols_get(d, picks[0], &addr, name, sizeof name) &&
       addr == 0x0900 && !strcmp(name, "MyRoutine"));
    count = cocodebug_symbols_filter(d, "my", picks, 64);
    ok("...ordered by address", count == 2 && picks[0] < picks[1]);
    ok("an empty filter matches all", cocodebug_symbols_filter(d, "", picks, 64) == before + 2);
    ok("the disassembly names an address",
       cocodebug_symbol_at(d, 0x0900, name, sizeof name) && !strcmp(name, "MyRoutine"));
    cocodebug_symbols_clear(d);
    ok("Clear drops the loaded ones, keeps the built-ins", cocodebug_symbols_count(d) == before);
}

static void check_io_and_sound(cocodebug *d)
{
    cocodebug_io io;
    cocodebug_sound snd;
    cocodebug_sound_write hist[16];
    char info[240];
    /* LDA #$FC; STA $FF20; CLRA; STA $FF20; BRA loop: a square on the DAC */
    static const uint8_t dac_loop[] = {
        0x86, 0xFC, 0xB7, 0xFF, 0x20, 0x4F, 0xB7, 0xFF, 0x20, 0x20, 0xF5
    };
    int count;

    pause_now(d);
    ok("Cart & I/O data while paused", cocodebug_io_get(d, &io));
    ok("...PIA0 and PIA1 have been set up by BASIC", io.pia0.cra != 0 && io.pia1.cra != 0);
    ok("...the SAM (CoCo 1/2) or the GIME (CoCo 3)", io.sam_valid || io.gime_valid);
    ok("...and the right machine", io.is_coco3 == (cocodebug_space_size(d, COCODEBUG_SPACE_PHYS) != 0));
    ok("...the joysticks centre", io.joy_axis[0][0] >= 0 && io.joy_axis[0][0] <= 65535);
    ok("the cartridge info names it", cocodebug_cart_info(d, info, sizeof info) > 10 &&
       strstr(info, "Cartridge:") != NULL);
    printf("     (%s)\n", info);
    ok("sound state while paused", cocodebug_sound_get(d, &snd));
    ok("...DAC and mux are plausible", snd.dac >= 0 && snd.dac < 64 && snd.mux_source >= 0 &&
       snd.mux_source < 4 && snd.mux_name != NULL);

    cocodebug_sound_watch(d, 1);
    cocodebug_sound_history_clear(d);
    run_snippet(d, dac_loop, sizeof dac_loop, 100);
    count = cocodebug_sound_history(d, hist, 16);
    ok("writes to $FF20 are recorded", count == 16);
    ok("...newest first, with the DAC level", hist[0].port == 0 &&
       (hist[0].value == 0xFC || hist[0].value == 0x00) && hist[0].dac == (hist[0].value >> 2) &&
       hist[0].value != hist[1].value);
    ok("...with ticks that do not go forward as we read back", (int32_t)(hist[0].tick - hist[1].tick) > 0);
    cocodebug_sound_watch(d, 0);
}

static void check_prompt(cocodebug *d)
{
    char out[4096];
    cocodebug_regs r;

    pause_now(d);
    cocodebug_command(d, "print 1+2*3", out, sizeof out);
    ok("print evaluates C precedence", strstr(out, "$0007  7") != NULL);
    cocodebug_command(d, "set a $5a", out, sizeof out);
    cocodebug_command(d, "print a", out, sizeof out);
    ok("a register name evaluates", strstr(out, "$005A") != NULL);
    cocodebug_command(d, "set x $1234", out, sizeof out);
    cocodebug_command(d, "print x+1", out, sizeof out);
    ok("an expression of registers", strstr(out, "$1235") != NULL);
    cocodebug_command(d, "set zf 1", out, sizeof out);
    cocodebug_command(d, "print zf", out, sizeof out);
    ok("a flag sets and reads", strstr(out, "$0001  1") != NULL);
    cocodebug_command(d, "print [$0900]", out, sizeof out);
    ok("[addr] reads a byte", strstr(out, "$0046") != NULL);    /* 'F' */
    cocodebug_command(d, "print {$0900}", out, sizeof out);
    ok("{addr} reads a big-endian word", strstr(out, "$4655") != NULL);   /* 'F''U' */
    cocodebug_command(d, "print POLCAT", out, sizeof out);
    ok("a built-in symbol evaluates", strstr(out, "$A000") != NULL);
    cocodebug_command(d, "print nosuchthing", out, sizeof out);
    ok("an unknown name is an error", strstr(out, "error") != NULL);
    cocodebug_command(d, "mem $0900 8", out, sizeof out);
    ok("mem dumps hex and text", strstr(out, "46 55 4A 49") && strstr(out, "FUJI"));
    cocodebug_command(d, "poke $0910 1 2 3", out, sizeof out);
    cocodebug_command(d, "mem $0910 4", out, sizeof out);
    ok("poke writes", strstr(out, "01 02 03") != NULL);
    cocodebug_command(d, "break $0704", out, sizeof out);
    cocodebug_command(d, "bpw $0600 value != old", out, sizeof out);
    cocodebug_command(d, "breaks", out, sizeof out);
    ok("breaks lists them with text", strstr(out, "Exec  $0704") && strstr(out, "Write  $0600  if value != old"));
    cocodebug_command(d, "clear", out, sizeof out);
    cocodebug_command(d, "watch $0900", out, sizeof out);
    ok("watch lists with the live value", strstr(out, "$0900") && strstr(out, "$46 (70)"));
    cocodebug_command(d, "unwatch all", out, sizeof out);
    cocodebug_command(d, "find text fuji", out, sizeof out);
    ok("find reports the address", strstr(out, "$0900") != NULL);
    cocodebug_command(d, "disasm $0700 4", out, sizeof out);
    ok("disasm lists code", strstr(out, "LDA") != NULL);
    cocodebug_command(d, "map", out, sizeof out);
    ok("map lists regions", strstr(out, "PIA0") != NULL);
    cocodebug_command(d, "io", out, sizeof out);
    ok("io lists the PIAs", strstr(out, "PIA0") && strstr(out, "PIA1"));
    cocodebug_command(d, "sound", out, sizeof out);
    ok("sound lists the DAC", strstr(out, "DAC") != NULL);
    cocodebug_command(d, "cart", out, sizeof out);
    ok("cart names the cartridge", strstr(out, "Cartridge") != NULL);
    cocodebug_command(d, "video", out, sizeof out);
    ok("video answers", out[0] != '\0' && !strstr(out, "not available"));
    cocodebug_command(d, "video timing", out, sizeof out);
    ok("video timing answers", out[0] != '\0' && !strstr(out, "not available"));
    cocodebug_command(d, "video bogus", out, sizeof out);
    ok("a bad video section is explained", strstr(out, "pixel, registers") != NULL);
    cocodebug_command(d, "frobnicate", out, sizeof out);
    ok("an unknown command says so", strstr(out, "unknown command") != NULL);
    cocodebug_command(d, "help", out, sizeof out);
    ok("help lists the commands", strstr(out, "bpa") && strstr(out, "watch") && strstr(out, "find") &&
       strstr(out, "overline"));
    cocodebug_completions(d, "wat", out, sizeof out);
    ok("completion finishes a command", strstr(out, "watch") != NULL);
    cocodebug_completions(d, "print PO", out, sizeof out);
    ok("...and a symbol", strstr(out, "POLCAT") != NULL);

    cocodebug_mem_write(d, COCODEBUG_SPACE_CPU, 0x0700, k_rw_loop, sizeof k_rw_loop);
    cocodebug_command(d, "set pc $0700", out, sizeof out);
    cocodebug_command(d, "step 3", out, sizeof out);
    ok("step n runs n instructions", cocodebug_regs_get(d, &r) && r.pc == 0x0707);
    cocodebug_command(d, "trace on", out, sizeof out);
    ok("trace on enables", cocodebug_trace_enabled(d));
    cocodebug_command(d, "trace off", out, sizeof out);
}

/* Step Out by depth: a routine with PSHS U,X, a local, a nested BSR and a
 * PULS ...,PC. */
static void check_step_out(cocodebug *d)
{
    static const uint8_t prog[] = { 0xBD, 0x07, 0x10, 0x20, 0xFE };
    static const uint8_t routine[] = {
        0x34, 0x50, 0x32, 0x7E, 0x8D, 0x0A, 0x86, 0x55, 0x32, 0x62, 0x35, 0xD0
    };
    static const uint8_t nested[] = { 0x86, 0x11, 0x39 };
    static const uint8_t forever[] = { 0x20, 0xFE };
    cocodebug_regs r, start;
    cocodebug_stop stop;
    uint64_t serial;
    int step;

    pause_now(d);
    cocodebug_mem_write(d, COCODEBUG_SPACE_CPU, 0x0700, prog, sizeof prog);
    cocodebug_mem_write(d, COCODEBUG_SPACE_CPU, 0x0710, routine, sizeof routine);
    cocodebug_mem_write(d, COCODEBUG_SPACE_CPU, 0x0720, nested, sizeof nested);
    cocodebug_mem_write(d, COCODEBUG_SPACE_CPU, 0x0740, forever, sizeof forever);
    cocodebug_flag_set(d, COCODEBUG_CC_I | COCODEBUG_CC_F, 1);
    cocodebug_regs_set(d, COCODEBUG_REG_PC, 0x0700);
    cocodebug_regs_get(d, &start);

    for (step = 0; step < 3; step++) {
        serial = cocodebug_stop_serial(d);
        cocodebug_step(d);
        wait_stop_serial(d, serial);
    }
    cocodebug_regs_get(d, &r);
    ok("stepped into the middle of the routine (before the nested call)", r.pc == 0x0714 && r.s < start.s);
    serial = cocodebug_stop_serial(d);
    cocodebug_step_out(d);
    ok("Step Out stops", wait_stop_serial(d, serial));
    cocodebug_stop_snapshot(d, &stop);
    ok("...after the nested call returned, at the instruction after the JSR",
       stop.pc == 0x0703 && !strcmp(stop.reason, "returned at $0703"));
    cocodebug_regs_get(d, &r);
    ok("...with S back where it was", r.s == start.s);

    /* from the first instruction of the routine */
    cocodebug_regs_set(d, COCODEBUG_REG_PC, 0x0700);
    serial = cocodebug_stop_serial(d);
    cocodebug_step(d);
    wait_stop_serial(d, serial);
    cocodebug_regs_get(d, &r);
    ok("at the routine's first instruction", r.pc == 0x0710);
    serial = cocodebug_stop_serial(d);
    cocodebug_step_out(d);
    ok("Step Out from the first instruction stops", wait_stop_serial(d, serial));
    cocodebug_regs_get(d, &r);
    ok("...after the JSR", r.pc == 0x0703 && r.s == start.s);

    /* a routine that never returns stays running, and Pause works */
    cocodebug_regs_set(d, COCODEBUG_REG_PC, 0x0740);
    cocodebug_step_out(d);
    nap_ms(150);
    ok("Step Out in a loop that never returns keeps running", !cocodebug_is_paused(d));
    pause_now(d);
    ok("...and Pause stops it", cocodebug_is_paused(d));
    serial = cocodebug_stop_serial(d);
    cocodebug_resume(d);
    nap_ms(50);
    ok("...with no step-out left armed", !cocodebug_is_paused(d));
    pause_now(d);
    (void)serial;
}

/* Bug 1: a session that stops and starts again leaves the engine alive. */
static void check_restart(cocosession *s, cocodebug *d, int machine)
{
    cocodebug_regs r;
    uint64_t serial;
    uint16_t here;

    cocodebug_resume(d);
    cocosession_stop(s);
    ok("the session stops while engaged", !cocosession_is_running(s));
    ok("it starts again", start_session(s, machine) == 0);
    nap_ms(1500);
    pause_now(d);
    ok("the engine parks again after a restart", cocodebug_is_paused(d));
    ok("...and reports registers", cocodebug_regs_get(d, &r));
    here = r.pc;
    cocodebug_breakpoint_toggle(d, here);
    serial = cocodebug_stop_serial(d);
    cocodebug_resume(d);
    ok("a breakpoint still fires after a restart", wait_stop_serial(d, serial));
    ok("...at its address", cocodebug_regs_get(d, &r) && r.pc == here);
    cocodebug_breakpoint_clear_all(d);
}

int main(int argc, char **argv)
{
    const char *which = argc > 1 ? argv[1] : "coco3";
    const int machine = !strcmp(which, "coco2") ? COCOSESSION_MACHINE_COCO2
                                                : COCOSESSION_MACHINE_COCO3;
    cocosession_paths paths;
    cocosession *s;
    cocodebug *d;

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
    printf("== %s ==\n", which);
    if (start_session(s, machine) != 0) {
        fprintf(stderr, "debug_test: session start failed: %s\n", cocosession_last_error(s));
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

    check_before_engaging(d);
    check_basics(d);
    check_stop_race(d);
    check_reads_have_no_side_effects(d);
    reset_and_check(s, d);
    check_breakpoint_kinds(d);
    check_registers(d);
    check_memory_and_search(d);
    check_memory_map(d);
    check_trace(d);
    check_symbols(d);
    check_io_and_sound(d);
    check_prompt(d);
    check_step_out(d);

    /* --- releasing lets it run again ------------------------------------- */
    {
        cocodebug_regs r;
        cocodebug_set_engaged(d, 0);
        ok("releasing disengages", !cocodebug_is_engaged(d));
        nap_ms(200);
        ok("...and the machine is running", !cocodebug_is_paused(d));
        ok("...so registers are private again", !cocodebug_regs_get(d, &r));
    }

    /* --- the restart regression, with the debugger engaged again ----------- */
    cocodebug_set_engaged(d, 1);
    pause_now(d);
    check_restart(s, d, machine);

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
