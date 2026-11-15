/*
 * machine_debug.h -- engine-side hooks for the desktop port's debugger.
 *
 * Not part of XRoar. tools/xroar/patch-staged-tree.py copies this file into
 * the staged tree (core/xroar-generated/src/) and patches the machine,
 * SAM, GIME, VDG and PIA code to implement it. Include it through
 * core/include/cocoxroar.h, which carries the full contract and the threading
 * rules; the comments here are the short form.
 *
 * Every hook runs on the emulator thread, inside a CPU cycle or a video
 * event: a tap must be fast and must neither block nor call back into the
 * debugger or the machine.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef XROAR_MACHINE_DEBUG_H_
#define XROAR_MACHINE_DEBUG_H_

#include <stdint.h>

struct machine;

/* ---- 1. bus tap -------------------------------------------------------- */

/* One CPU bus cycle, as reported to the bus tap. */
struct machine_bus_cycle {
	uint16_t address;   /* CPU address on the bus */
	uint8_t  value;     /* reads: the byte the CPU received; writes: the byte it drove */
	uint8_t  old_value; /* writes to RAM only: the byte there BEFORE the store */
	_Bool    is_write;
	_Bool    has_old;   /* old_value is valid: a write that lands in RAM */
	int32_t  physical;  /* RAM element index touched by this cycle, or -1 when
	                     * the cycle did not select RAM (ROM, I/O, vectors).
	                     * Linear over all RAM banks: bank * bank size + the
	                     * index inside the bank. On the CoCo 3 that is the
	                     * 19-bit GIME RAM address (bank << 13 | offset), plus
	                     * 512K per extra bank of a 1M/2M machine. On the
	                     * CoCo 1/2 it is the DRAM row/column index, which is
	                     * NOT the CPU address (the SAM scrambles it). */
};

/* Called once per CPU bus cycle that advances the clock. Dummy cycles (the
 * 6809's $FFFF reads, repeated PC reads) are passed through: filter them.
 * Debug accesses that do not advance the clock (machine->read_byte /
 * write_byte, trap code) never reach it. */
typedef void (*machine_bus_tap_func)(void *ctx, const struct machine_bus_cycle *cycle);

/* Install (func != NULL) or remove (func == NULL) the tap. Call it from the
 * emulator thread or while the emulator thread is parked. The tap is a field
 * of the machine, so a machine rebuild (CoCo 2 <-> CoCo 3) drops it: re-check
 * `m->bus_tap == func` when the machine changes. */
void machine_debug_set_bus_tap(struct machine *m, machine_bus_tap_func func, void *ctx);

/* ---- 2. side-effect-free memory access --------------------------------- */

enum machine_region_kind {
	MACHINE_REGION_UNMAPPED = 0,
	MACHINE_REGION_RAM,
	MACHINE_REGION_ROM,      /* internal ROM, incl. the interrupt vectors */
	MACHINE_REGION_CART_ROM,
	MACHINE_REGION_IO        /* PIAs, cartridge I/O, GIME, MMU, palette, SAM */
};

#define MACHINE_MEMORY_MAP_MAX_REGIONS (48)

struct machine_memory_region {
	uint16_t first, last;    /* CPU addresses, inclusive */
	uint8_t  kind;           /* enum machine_region_kind */
	_Bool    writable;       /* a CPU store lands here (RAM, or an I/O register) */
	int32_t  physical_base;  /* RAM only: `physical` of `first`, contiguous to
	                          * `last`; -1 when not meaningful (always -1 on the
	                          * CoCo 1/2) */
	const char *name;        /* static string: "RAM", "BASIC ROM", "PIA0", ... */
};

/* The CPU's current view of the 64K, plus the state it was decoded from. */
struct machine_memory_map {
	_Bool    is_coco3;
	uint32_t ram_size;       /* bytes of RAM fitted */

	/* CoCo 1/2: SAM */
	unsigned sam_map_type;   /* TY: 0 = 32K RAM + 32K ROM, 1 = 64K RAM */
	unsigned sam_memory_size;/* M */
	unsigned sam_page;       /* P */

	/* CoCo 3: GIME and its MMU */
	_Bool    gime_coco_mode; /* INIT0 bit 7 */
	_Bool    mmu_enabled;    /* INIT0 MMUEN */
	_Bool    constant_ram;   /* INIT0 MC3: $FE00-$FEFF is the same RAM in both tasks */
	_Bool    mc2;            /* INIT0 MC2: $FF40-$FF5F external */
	unsigned rom_mode;       /* INIT0 MC1:MC0 */
	unsigned task;           /* INIT1 TR: 0 = task 1, 8 = task 2 */
	_Bool    timer_fast;     /* INIT1 TINS */
	uint8_t  mmu_bank[16];   /* $FFA0-$FFAF (6 bits each) */

	unsigned nregions;
	_Bool    truncated;      /* more regions than fit (never seen) */
	struct machine_memory_region regions[MACHINE_MEMORY_MAP_MAX_REGIONS];
};

/* Where machine->peek_byte reads. All of these are views of the same
 * machine; none of them runs a bus cycle:
 *
 *   machine->peek_byte(m, addr)        the byte the CPU would read at addr.
 *       RAM through the SAM map type / the GIME MMU, internal ROM, cartridge
 *       ROM. I/O returns latched values and acknowledges nothing: PIA
 *       registers, GIME registers (last value written; $FF92/$FF93 give the
 *       pending bits without clearing them), MMU, palette. Cartridge I/O
 *       ($FF40-$FF5F: Becker, floppy) and the SAM read back $FF. Unmapped
 *       reads give $FF.
 *   machine->poke_byte(m, addr, value) store into RAM only; returns whether
 *       addr is RAM at present. ROM and I/O are never written.
 *   machine->memory_map(m, &map)       fill the map above.
 *
 * machine_debug_ram_{size,peek,poke} address RAM by `physical` (same index as
 * machine_bus_cycle.physical). peek returns -1 out of range; poke returns 0.
 *
 * Call all of them from the emulator thread or while it is parked. */
uint32_t machine_debug_ram_size(struct machine *m);
int      machine_debug_ram_peek(struct machine *m, uint32_t physical);
_Bool    machine_debug_ram_poke(struct machine *m, uint32_t physical, uint8_t value);

/* ---- 3. chip state ------------------------------------------------------ */

/* The SAM (MC6883) state that is private to mc6883.c. */
struct mc6883_debug_state {
	unsigned V;             /* VDG addressing mode, 0-7 */
	unsigned F;             /* display base: bits 15..9 of the address */
	unsigned P;             /* page */
	unsigned R;             /* MPU rate */
	unsigned M;             /* memory size */
	unsigned TY;            /* map type */
	unsigned reg;           /* all of the above packed, as get_register() */
	unsigned video_address; /* the VDG address counter: (B15_5 << 5) | (B4 << 4) | B3_0 */
};

/* Returns 0 (and leaves *out untouched) when the machine's SAM is not an
 * MC6883/74LS785 (the SAMx8 has no such registers). */
struct MC6883;
_Bool mc6883_get_debug_state(struct MC6883 *, struct mc6883_debug_state *out);

/* The VDG (MC6847) state that is private to mc6847.c. */
struct mc6847_debug_state {
	unsigned scanline;      /* the line being drawn; during a render_line
	                         * callback, the line that has just finished */
	unsigned GM;            /* graphics mode bits */
	_Bool    nA_S, nA_G, EXT, CSS;
};

struct MC6847;
void mc6847_get_debug_state(struct MC6847 *, struct mc6847_debug_state *out);

/* The GIME (TCC1014) state is tcc1014_get_debug_state() in tcc1014.h
 * (struct tcc1014_debug_state), extended with B, row, scanline, the sixteen
 * registers, palette, MMU banks, SAM mirror, TY, R1, MC0-3, TR, TINS. */

/* ---- 4. video taps ------------------------------------------------------ */

/* One finished scanline, as the VDG or GIME hands it to the video output. */
struct machine_video_line {
	unsigned scanline;      /* index of the line just finished (0 = first of the frame) */
	uint32_t tick;          /* event_current_tick when it finished */
	unsigned burst;         /* colour burst index */
	unsigned npixels;       /* pixels in the line (VDG: half-pixels of the 228 us line; GIME: 912) */
	const uint8_t *data;    /* palettised pixels, NULL for a dummy line; valid only during the call */
};

struct machine_video_tap {
	void *ctx;
	void (*render_line)(void *ctx, const struct machine_video_line *line);
	/* HS / FS level changes (level 0 = the falling edge that starts the line
	 * or vertical sync). `scanline` is the line counter at that moment. */
	void (*hsync)(void *ctx, _Bool level, unsigned scanline, uint32_t tick);
	void (*fsync)(void *ctx, _Bool level, unsigned scanline, uint32_t tick);
};

/* Chain `tap` behind the VDG's (CoCo 1/2) or GIME's (CoCo 3) render_line,
 * signal_hs and signal_fs delegates: the original delegate runs first, then
 * the tap, so the pixels are already in the frame when it fires. Replaces any
 * previous tap of ours on the same machine (the struct is copied). Returns 0
 * when the machine has neither chip. Emulator thread, or parked. */
_Bool machine_debug_video_tap_install(struct machine *m, const struct machine_video_tap *tap);

/* Put the original delegates back. A no-op when our wrappers are not on `m`. */
void machine_debug_video_tap_remove(struct machine *m);

/* 1 when our wrappers are installed on `m`'s current chip. After a machine
 * rebuild (or if anything replaced a delegate) it is 0: install again. */
_Bool machine_debug_video_tap_active(struct machine *m);

/* Drop all state kept here; xroar_shutdown() calls it. */
void machine_debug_forget(void);

/* ---- helpers for the machine implementations ---------------------------- */

struct ram;
int32_t machine_debug_ram_physical(struct ram *ram, unsigned bank, unsigned row, unsigned col);

struct machine_memory_region;
typedef void (*machine_debug_describe_func)(struct machine *m, unsigned address,
                                            struct machine_memory_region *out);
void machine_debug_build_memory_map(struct machine *m, machine_debug_describe_func describe,
                                    struct machine_memory_map *map);

#endif
