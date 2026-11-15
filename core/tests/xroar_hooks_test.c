/*
 * xroar_hooks_test -- the engine-side hooks the debugger builds on
 * (core/include/cocoxroar.h), against a real running machine.
 *
 * The hooks are patched into the staged XRoar tree (tools/xroar/patch-staged-
 * tree.py), so only a machine that actually executes can show they work:
 * the bus tap sees the CPU's accesses, the old byte of a write is what the
 * RAM held, peeking I/O consumes nothing, the SAM and GIME accessors read
 * the live chips, the video taps fire once per scanline, and all of it
 * survives a machine switch and a session stop/start.
 *
 * One machine per PROCESS, chosen by argv[1] ("coco3" or "coco2", default
 * coco2); ctest registers the binary once per machine. Skips (ctest 77) when
 * the build has no system ROMs to boot.
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
#include "cocoxroar.h"
#include "roms_embedded.h"

#define SKIP 77
#define RING_SIZE (4u << 20)

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

static int wait_paused(cocodebug *debugger)
{
    int tries;
    for (tries = 0; tries < 300; tries++) {
        if (cocodebug_is_paused(debugger))
            return 1;
        nap_ms(10);
    }
    return 0;
}

/* ---- the taps ----------------------------------------------------------- */

struct tap_state {
    struct machine_bus_cycle *ring;
    volatile unsigned count;    /* cycles seen, including those past the ring */
    volatile unsigned lines;
    volatile unsigned dummy_lines;
    volatile unsigned hs_edges;
    volatile unsigned fs_rises;
    volatile unsigned lines_at_first_rise;
    volatile unsigned lines_at_last_rise;
    volatile unsigned fs_falls;
    volatile unsigned bad_scanlines;
    volatile unsigned last_scanline;
};

static struct tap_state g_tap;

static void bus_tap(void *ctx, const struct machine_bus_cycle *cycle)
{
    struct tap_state *state = ctx;
    unsigned slot = state->count++;
    if (slot < RING_SIZE)
        state->ring[slot] = *cycle;
}

static void on_line(void *ctx, const struct machine_video_line *line)
{
    struct tap_state *state = ctx;
    state->lines++;
    if (!line->data)
        state->dummy_lines++;
    if (line->scanline >= 320)
        state->bad_scanlines++;
    state->last_scanline = line->scanline;
}

static void on_hsync(void *ctx, _Bool level, unsigned scanline, uint32_t tick)
{
    struct tap_state *state = ctx;
    (void)scanline;
    (void)tick;
    if (!level)
        state->hs_edges++;
}

static void on_fsync(void *ctx, _Bool level, unsigned scanline, uint32_t tick)
{
    struct tap_state *state = ctx;
    (void)scanline;
    (void)tick;
    if (level) {
        if (!state->fs_rises)
            state->lines_at_first_rise = state->lines;
        state->lines_at_last_rise = state->lines;
        state->fs_rises++;
    } else
        state->fs_falls++;
}

static const struct machine_video_tap g_video_tap = {
    &g_tap, on_line, on_hsync, on_fsync
};

static void install_taps(struct machine *mach)
{
    machine_debug_set_bus_tap(mach, bus_tap, &g_tap);
    machine_debug_video_tap_install(mach, &g_video_tap);
}

static void reset_counts(void)
{
    g_tap.count = 0;
    g_tap.lines = g_tap.dummy_lines = g_tap.hs_edges = 0;
    g_tap.fs_rises = g_tap.fs_falls = g_tap.bad_scanlines = 0;
    g_tap.lines_at_first_rise = g_tap.lines_at_last_rise = 0;
}

/* ---- observing that peeks consume nothing ------------------------------- */

static unsigned g_cart_reads;
static uint8_t (*g_cart_read_real)(struct cart *, uint16_t, _Bool, _Bool, uint8_t);

static uint8_t counting_cart_read(struct cart *cart, uint16_t address, _Bool p2,
                                  _Bool r2, uint8_t data)
{
    g_cart_reads++;
    return g_cart_read_real(cart, address, p2, r2, data);
}

/* ---- checks done while the emulator is parked --------------------------- */

static void check_peek_matches_bus(struct machine *mach)
{
    unsigned address, mismatches = 0, checked = 0;
    unsigned first_bad = 0;

    /* Everywhere that is RAM or ROM, the side-effect-free view must agree
     * with what a (debug) bus cycle returns. I/O is left out on purpose. */
    for (address = 0; address < 0xff00; address += 5) {
        uint8_t viaPeek = mach->peek_byte(mach, address);
        uint8_t viaBus = mach->read_byte(mach, address, 0);
        checked++;
        if (viaPeek != viaBus) {
            if (!mismatches)
                first_bad = address;
            mismatches++;
        }
    }
    for (address = 0xffe0; address < 0x10000; address++) {
        uint8_t viaPeek = mach->peek_byte(mach, address);
        uint8_t viaBus = mach->read_byte(mach, address, 0);
        checked++;
        if (viaPeek != viaBus) {
            if (!mismatches)
                first_bad = address;
            mismatches++;
        }
    }
    if (mismatches)
        printf("     %u of %u addresses differ; first at $%04X\n", mismatches,
               checked, first_bad);
    ok("peek_byte agrees with a bus read across RAM, ROM and the vectors",
       mismatches == 0);
}

static void check_peek_has_no_side_effects(struct machine *mach)
{
    struct cart *cart =
        (struct cart *)part_component_by_id_is_a(&mach->part, "cart", "dragon-cart");
    struct MC6821 *pia0 =
        (struct MC6821 *)part_component_by_id_is_a(&mach->part, "PIA0", "MC6821");
    unsigned address;
    uint8_t before;

    ok("a cartridge (the Becker port) is fitted", cart != NULL);
    if (cart) {
        g_cart_read_real = cart->read;
        g_cart_reads = 0;
        cart->read = counting_cart_read;

        for (address = 0xff40; address < 0xff60; address++)
            (void)mach->peek_byte(mach, address);
        ok("peeking $FF40-$FF5F (Becker, floppy) reaches no cartridge", g_cart_reads == 0);

        (void)mach->read_byte(mach, 0xff42, 0);
        ok("the control: a bus read of $FF42 does reach the cartridge", g_cart_reads > 0);
        cart->read = g_cart_read_real;
    }

    /* A PIA interrupt flag: a peek leaves it, a bus read clears it. */
    ok("PIA0 found", pia0 != NULL);
    if (pia0) {
        pia0->a.irq1_received = 0x80;
        before = mach->peek_byte(mach, 0xff01);
        ok("PIA0 control register shows the flag", (before & 0x80) != 0);
        (void)mach->peek_byte(mach, 0xff00);
        (void)mach->peek_byte(mach, 0xff01);
        ok("peeking PIA0 leaves the flag set", pia0->a.irq1_received == 0x80);
        if (pia0->a.control_register & 0x04) {
            (void)mach->read_byte(mach, 0xff00, 0);
            ok("the control: a bus read of the data register clears it",
               pia0->a.irq1_received == 0);
        }
    }
}

static void check_ram_and_map(struct machine *mach)
{
    struct machine_memory_map map;
    unsigned index, covered = 0;
    int found_ram = 0, found_pia = 0, found_vectors = 0, contiguous = 1;
    uint32_t size = machine_debug_ram_size(mach);

    ok("peek_byte, poke_byte and memory_map are set",
       mach->peek_byte && mach->poke_byte && mach->memory_map);
    ok("the RAM size is 64K or more", size >= 0x10000);
    ok("physical RAM reads back inside and fails outside",
       machine_debug_ram_peek(mach, 0) >= 0 && machine_debug_ram_peek(mach, size) == -1);

    memset(&map, 0xaa, sizeof(map));
    mach->memory_map(mach, &map);
    ok("the map has regions and no truncation", map.nregions > 2 && !map.truncated);
    ok("the map reports the RAM size", map.ram_size == size);
    for (index = 0; index < map.nregions; index++) {
        const struct machine_memory_region *region = &map.regions[index];
        if (index == 0 && region->first != 0)
            contiguous = 0;
        if (index > 0 && region->first != map.regions[index - 1].last + 1)
            contiguous = 0;
        covered += region->last - region->first + 1;
        if (region->kind == MACHINE_REGION_RAM && region->first == 0)
            found_ram = 1;
        if (region->name && !strcmp(region->name, "PIA0") && region->first == 0xff00)
            found_pia = 1;
        if (region->name && !strcmp(region->name, "VECTORS") && region->last == 0xffff)
            found_vectors = 1;
    }
    ok("the regions tile $0000-$FFFF", contiguous && covered == 0x10000 &&
       map.regions[map.nregions - 1].last == 0xffff);
    ok("RAM at $0000", found_ram);
    ok("PIA0 at $FF00", found_pia);
    ok("the vectors end at $FFFF", found_vectors);

    if (map.is_coco3) {
        /* The MMU registers in the map are the GIME's. */
        struct TCC1014 *gime =
            (struct TCC1014 *)part_component_by_id_is_a(&mach->part, "GIME", "TCC1014");
        struct tcc1014_debug_state gimes;
        int same = 1;

        ok("GIME found", gime != NULL);
        if (gime) {
            tcc1014_get_debug_state(gime, &gimes);
            for (index = 0; index < 16; index++)
                if (map.mmu_bank[index] != (gimes.mmu_bank[index] & 0x3f))
                    same = 0;
            ok("the map's MMU banks are the GIME's", same);
            ok("the map's task and MMU enable are the GIME's",
               map.task == gimes.TR && map.mmu_enabled == gimes.MMUEN);
            ok("scanline, row and registers are sane",
               gimes.scanline < 320 && gimes.row < 16 &&
               gimes.registers[0] == mach->peek_byte(mach, 0xff90));
        }
        /* RAM through the MMU: the CPU view equals the physical view. */
        for (index = 0; index < map.nregions; index++) {
            const struct machine_memory_region *region = &map.regions[index];
            if (region->kind == MACHINE_REGION_RAM && region->physical_base >= 0) {
                uint32_t address = region->first + 0x10;
                int viaPhys;
                if (address > region->last)
                    address = region->first;
                viaPhys = machine_debug_ram_peek(
                    mach, (uint32_t)region->physical_base + (address - region->first));
                ok("a RAM region's physical base reads the same byte as the CPU view",
                   viaPhys == mach->peek_byte(mach, address));
                break;
            }
        }
    } else {
        struct MC6883 *sam =
            (struct MC6883 *)part_component_by_id_is_a(&mach->part, "SAM", "SN74LS783");
        struct mc6883_debug_state state;

        ok("SAM found", sam != NULL);
        if (sam) {
            ok("the SAM accessor answers", mc6883_get_debug_state(sam, &state));
            ok("BASIC runs with the 32K map and a $0400 display",
               state.TY == 0 && state.F == 0x0400 && state.V == 0);
            ok("the packed register matches get_register()", state.reg == sam->get_register(sam));
            ok("the video address counter is inside the 64K",
               state.video_address < 0x10000);
            ok("the map carries the SAM map type", map.sam_map_type == state.TY);
        }
    }

    {
        uint8_t was = mach->peek_byte(mach, 0x0300);
        uint8_t rom = 0;
        uint32_t rom_address = 0;
        unsigned region;
        int have_rom = 0;
        for (region = 0; region < map.nregions && !have_rom; region++) {
            if (map.regions[region].kind == MACHINE_REGION_ROM) {
                rom_address = map.regions[region].first;
                have_rom = 1;
            }
        }
        rom = have_rom ? mach->peek_byte(mach, rom_address) : 0;
        ok("poke_byte stores into RAM", mach->poke_byte(mach, 0x0300, (uint8_t)~was) &&
           mach->peek_byte(mach, 0x0300) == (uint8_t)~was);
        mach->poke_byte(mach, 0x0300, was);
        ok("the map has internal ROM", have_rom);
        ok("poke_byte refuses ROM and leaves it alone",
           have_rom && !mach->poke_byte(mach, rom_address, (uint8_t)~rom) &&
           mach->peek_byte(mach, rom_address) == rom);
    }
}

/* The shadow check: every write that reports an old byte must report what
 * the last write to that RAM element stored, and every RAM read must return
 * the last write's byte. Elements never written are unknown, not assumed. */
static void check_bus_ring(struct machine *mach)
{
    uint32_t size = machine_debug_ram_size(mach);
    uint8_t *shadow = calloc(size ? size : 1, 1);
    uint8_t *known = calloc(size ? size : 1, 1);
    unsigned total = g_tap.count < RING_SIZE ? g_tap.count : RING_SIZE;
    unsigned index;
    unsigned reads = 0, writes = 0, writes_old = 0, wrong_old = 0, wrong_read = 0;
    unsigned ram_reads_checked = 0, dummy = 0, io_writes = 0;

    ok("the tap saw bus cycles", total > 100000);
    for (index = 0; index < total; index++) {
        const struct machine_bus_cycle *cycle = &g_tap.ring[index];
        if (cycle->is_write) {
            writes++;
            if (cycle->physical < 0 || (uint32_t)cycle->physical >= size) {
                if (cycle->has_old)
                    wrong_old++;
                io_writes++;
                continue;
            }
            if (cycle->has_old) {
                writes_old++;
                if (known[cycle->physical] && shadow[cycle->physical] != cycle->old_value)
                    wrong_old++;
            }
            shadow[cycle->physical] = cycle->value;
            known[cycle->physical] = 1;
        } else {
            reads++;
            if (cycle->address == 0xffff)
                dummy++;
            if (cycle->physical >= 0 && (uint32_t)cycle->physical < size) {
                if (cycle->has_old)
                    wrong_old++;
                if (known[cycle->physical]) {
                    ram_reads_checked++;
                    if (shadow[cycle->physical] != cycle->value)
                        wrong_read++;
                }
            }
        }
    }
    printf("     %u cycles: %u reads, %u writes (%u to RAM, %u elsewhere), %u dummy $FFFF reads\n",
           total, reads, writes, writes_old, io_writes, dummy);
    ok("both reads and writes were seen", reads > 1000 && writes > 100);
    ok("writes to RAM carry the old byte", writes_old > 100);
    ok("writes to I/O are seen, without a physical address or an old byte", io_writes > 10);
    ok("every old byte is what the previous write stored", wrong_old == 0);
    ok("every RAM read returns the previous write's byte",
       wrong_read == 0 && ram_reads_checked > 100);
    ok("the 6809's dummy cycles are passed through", dummy > 0);
    free(shadow);
    free(known);
}

static void check_video_counts(void)
{
    double per_frame;

    printf("     %u lines, %u HS falls, %u FS rises, %u dummy lines\n", g_tap.lines,
           g_tap.hs_edges, g_tap.fs_rises, g_tap.dummy_lines);
    ok("frames went by", g_tap.fs_rises >= 20);
    /* Whole frames only: the lines between the first and the last FS rise. */
    per_frame = (double)(g_tap.lines_at_last_rise - g_tap.lines_at_first_rise) /
                (double)(g_tap.fs_rises - 1);
    printf("     %.2f lines per frame\n", per_frame);
    ok("render_line fires 262 or 263 times per frame", per_frame > 261.5 && per_frame < 263.5);
    ok("an HS edge for every line", g_tap.hs_edges + 2 >= g_tap.lines &&
       g_tap.hs_edges <= g_tap.lines + 2);
    ok("one FS fall per FS rise", g_tap.fs_falls + 1 >= g_tap.fs_rises &&
       g_tap.fs_falls <= g_tap.fs_rises + 1);
    ok("scanline numbers stay in range", g_tap.bad_scanlines == 0);
}

static void check_partial_frame(void)
{
    struct cocoxroar_partial_frame frame;

    ok("the partial frame is reachable", cocoxroar_partial_frame(&frame));
    ok("...with a plausible size and progress",
       frame.pixels && frame.width >= 256 && frame.height >= 192 &&
       frame.pitch >= frame.width && frame.rows_done >= 0 &&
       frame.rows_done <= frame.height);
    ok("...and rows are readable", frame.pixels[(frame.height - 1) * frame.pitch +
                                                (frame.width - 1)] <= 0xffffffffu);
}

/* The machine is idle in the cartridge's DriveWire wait loop, which only
 * reads. Give it something that reads and writes RAM: copy a counter over the
 * text screen, forever. Parked, so PC and CC can be set directly. */
static void inject_program(struct machine *mach)
{
    static const uint8_t program[] = {
        0x8e, 0x04, 0x00,       /* LDX #$0400 */
        0xa6, 0x84,             /* loop: LDA ,X */
        0x4c,                   /* INCA */
        0xa7, 0x80,             /* STA ,X+ */
        0x8c, 0x05, 0xff,       /* CMPX #$05FF */
        0x26, 0xf6,             /* BNE loop */
        0x7f, 0xff, 0x02,       /* CLR $FF02: a write that is not RAM */
        0x20, 0xee              /* BRA $0600 */
    };
    struct MC6809 *cpu =
        (struct MC6809 *)part_component_by_id_is_a(&mach->part, "CPU", "MC6809");
    unsigned index;

    for (index = 0; index < sizeof(program); index++)
        mach->poke_byte(mach, 0x0600 + index, program[index]);
    cpu->reg_pc = 0x0600;
    cpu->reg_cc |= 0x50; /* no IRQ or FIRQ */
}

/* ---- after a session restart, taps installed with no debugger ------------ */

struct restart_state {
    volatile int installed;
};

static void install_from_frame(void *ctx)
{
    struct restart_state *state = ctx;
    struct machine *mach = xroar.machine;

    if (!mach)
        return;
    if (mach->bus_tap != bus_tap)
        machine_debug_set_bus_tap(mach, bus_tap, &g_tap);
    if (!machine_debug_video_tap_active(mach))
        machine_debug_video_tap_install(mach, &g_video_tap);
    state->installed = 1;
}

static int start_session(cocosession *session, int machine)
{
    cocosession_start_opts opts;
    cocosession_default_opts(session, &opts);
    opts.machine = machine;
    opts.enable_fujinet = 0;
    opts.enable_audio = 0;
    opts.enable_gamepad = 0;
    return cocosession_start(session, &opts);
}

int main(int argc, char **argv)
{
    const char *which = argc > 1 ? argv[1] : "coco2";
    const int machine_id = !strcmp(which, "coco3") ? COCOSESSION_MACHINE_COCO3
                                                   : COCOSESSION_MACHINE_COCO2;
    const int other_id = machine_id == COCOSESSION_MACHINE_COCO3
                             ? COCOSESSION_MACHINE_COCO2 : COCOSESSION_MACHINE_COCO3;
    cocosession_paths paths;
    cocosession *session;
    cocodebug *debugger;
    struct machine *mach;
    struct restart_state restart;
    unsigned before, tries;

    if (!coco_embedded_system_roms) {
        printf("xroar_hooks_test: no system ROMs in this build "
               "(WITH_COCO_ROMS=OFF); skipping\n");
        return SKIP;
    }

    memset(&paths, 0, sizeof(paths));
    paths.config_dir = "xroar_hooks_test-config";
    paths.data_dir = "xroar_hooks_test-data";
    paths.fujinet_lib = ""; /* no runtime: this test must not bind a port */

    session = cocosession_new(&paths);
    if (!session) {
        fprintf(stderr, "xroar_hooks_test: could not create the session\n");
        return 1;
    }
    g_tap.ring = malloc(sizeof(*g_tap.ring) * RING_SIZE);
    if (!g_tap.ring) {
        fprintf(stderr, "xroar_hooks_test: out of memory\n");
        return 1;
    }

    printf("== %s ==\n", which);
    if (start_session(session, machine_id) != 0) {
        fprintf(stderr, "xroar_hooks_test: session start failed: %s\n",
                cocosession_last_error(session));
        cocosession_free(session);
        return 1;
    }
    debugger = cocosession_debugger(session);
    ok("the session hands out a debugger", debugger != NULL);
    if (!debugger) {
        cocosession_stop(session);
        cocosession_free(session);
        return 1;
    }

    /* Let BASIC reach its prompt, then park the machine. */
    nap_ms(2500);
    cocodebug_set_engaged(debugger, 1);
    cocodebug_pause(debugger);
    ok("the machine parks", wait_paused(debugger));
    mach = xroar.machine;
    ok("there is a machine", mach != NULL);
    if (!mach || !cocodebug_is_paused(debugger)) {
        cocosession_stop(session);
        cocosession_free(session);
        return 1;
    }

    /* -- static checks, machine parked -- */
    check_peek_matches_bus(mach);
    check_peek_has_no_side_effects(mach);
    check_ram_and_map(mach);
    check_partial_frame();

    /* -- run with the taps in -- */
    memset(&g_tap.ring[0], 0, sizeof(g_tap.ring[0]));
    reset_counts();
    inject_program(mach);
    install_taps(mach);
    ok("the video tap reports installed", machine_debug_video_tap_active(mach));
    ok("the bus tap is set on the machine", mach->bus_tap == bus_tap);
    cocodebug_resume(debugger);
    nap_ms(1500);
    cocodebug_pause(debugger);
    ok("the machine parks again", wait_paused(debugger));

    check_bus_ring(mach);
    check_video_counts();
    check_partial_frame();

    /* A pause stops the taps: nothing counts while parked. */
    before = g_tap.count;
    nap_ms(100);
    ok("nothing is counted while parked", g_tap.count == before);

    /* -- remove the taps -- */
    machine_debug_video_tap_remove(mach);
    machine_debug_set_bus_tap(mach, NULL, NULL);
    ok("the video tap reports removed", !machine_debug_video_tap_active(mach));
    reset_counts();
    cocodebug_resume(debugger);
    nap_ms(400);
    cocodebug_pause(debugger);
    ok("the machine parks a third time", wait_paused(debugger));
    ok("removed taps count nothing", g_tap.count == 0 && g_tap.lines == 0);

    /* Installing twice is harmless and chains once. */
    install_taps(mach);
    install_taps(mach);
    reset_counts();
    cocodebug_resume(debugger);
    nap_ms(1000);
    cocodebug_pause(debugger);
    ok("the machine parks a fourth time", wait_paused(debugger));
    printf("     after a double install: %u lines, %u FS rises\n", g_tap.lines,
           g_tap.fs_rises);
    ok("a second install does not double the callbacks",
       g_tap.fs_rises > 0 && g_tap.lines < g_tap.fs_rises * 300u);
    machine_debug_video_tap_remove(mach);
    machine_debug_set_bus_tap(mach, NULL, NULL);
    cocodebug_resume(debugger);

    /* -- switch machine in place: the new machine has no taps -- */
    cocodebug_set_engaged(debugger, 0);
    cocosession_set_machine(session, other_id);
    nap_ms(500);
    cocodebug_set_engaged(debugger, 1);
    cocodebug_pause(debugger);
    ok("the new machine parks", wait_paused(debugger));
    mach = xroar.machine;
    ok("the other machine has no bus tap", mach && mach->bus_tap == NULL);
    ok("...and no video tap", mach && !machine_debug_video_tap_active(mach));
    ok("...but has the hooks", mach && mach->peek_byte && mach->poke_byte && mach->memory_map);
    if (mach) {
        struct machine_memory_map map;
        mach->memory_map(mach, &map);
        ok("...and its map is of the other kind", map.is_coco3 == (other_id == COCOSESSION_MACHINE_COCO3));
        install_taps(mach);
        ok("the taps install on it", machine_debug_video_tap_active(mach));
        reset_counts();
        cocodebug_resume(debugger);
        nap_ms(1000);
        cocodebug_pause(debugger);
        ok("it parks once more", wait_paused(debugger));
        ok("the taps fire on the new machine", g_tap.count > 1000 && g_tap.lines > 100);
        machine_debug_video_tap_remove(mach);
        machine_debug_set_bus_tap(mach, NULL, NULL);
    }
    cocodebug_set_engaged(debugger, 0);
    cocodebug_resume(debugger);

    /* -- stop and start: nothing stale, taps installable again -- */
    cocosession_stop(session);
    memset(&restart, 0, sizeof(restart));
    reset_counts();
    ok("starts again", start_session(session, machine_id) == 0);
    cocosession_set_frame_callback(session, install_from_frame, &restart);
    for (tries = 0; tries < 300 && !restart.installed; tries++)
        nap_ms(10);
    ok("a tap can be installed on the restarted machine from the frame callback",
       restart.installed);
    nap_ms(1500);
    cocosession_set_frame_callback(session, NULL, NULL);
    printf("     after the restart: %u cycles, %u lines\n", g_tap.count, g_tap.lines);
    ok("the taps fire after a restart", g_tap.count > 100000 && g_tap.lines > 100);

    cocosession_stop(session);
    ok("it stops with taps installed", !cocosession_is_running(session));
    ok("and the shutdown forgot the video tap", !machine_debug_video_tap_active(xroar.machine));

    /* One more round, to be sure the second init is as good as the first. */
    reset_counts();
    ok("starts a third time", start_session(session, other_id) == 0);
    memset(&restart, 0, sizeof(restart));
    cocosession_set_frame_callback(session, install_from_frame, &restart);
    for (tries = 0; tries < 300 && !restart.installed; tries++)
        nap_ms(10);
    nap_ms(500);
    cocosession_set_frame_callback(session, NULL, NULL);
    ok("the taps fire on the third start", g_tap.count > 10000 && g_tap.lines > 30);
    cocosession_stop(session);

    cocosession_free(session);
    free(g_tap.ring);
    if (failures) {
        printf("xroar_hooks_test (%s): %d FAILED\n", which, failures);
        return 1;
    }
    printf("xroar_hooks_test (%s): all passed\n", which);
    return 0;
}
