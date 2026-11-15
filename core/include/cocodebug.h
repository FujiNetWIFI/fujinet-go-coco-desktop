/*
 * cocodebug -- the 6809 debugger engine.
 *
 * Built on XRoar's own hooks rather than a stepped core path of its own:
 * struct debug_cpu (xroar src/debug_cpu.h) offers an instruction_hook called
 * before each fetch, and that is where this parks the emulator thread when
 * the machine is paused. XRoar uses the same hook itself (bp_add/bp_remove:
 * the printer-ROM hook re-added at every reset, tape and auto-keyboard traps,
 * -trap), so ours CHAINS to whatever was installed before it and installs
 * itself again whenever something else replaced it. Accesses (read, write,
 * access breakpoints, old values, the sound history) come from the machine's
 * bus tap (core/include/cocoxroar.h); memory is read side-effect free
 * (peek_byte), never through a bus cycle.
 *
 * THREADING. Everything here is called from a UI thread except where noted.
 * While the machine is paused the emulator thread is parked inside the hook,
 * so the machine is quiescent and reading its memory and registers from
 * another thread is safe; that is why those calls are only valid while
 * paused, and say so. Nothing here is safe to call while it is running.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef COCODEBUG_H
#define COCODEBUG_H

#include <stdint.h>

#include "cocosession.h"
#include "m6809dasm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Enough for a UI that toggles them by clicking a disassembly line. */
#define COCODEBUG_MAX_BREAKPOINTS 128

/* The two address spaces the memory calls take. CPU is the 64K the CPU sees
 * through the SAM or the GIME's MMU. PHYS is the CoCo 3's physical RAM
 * (512K or 2M, GIME RAM address = MMU bank << 13 | offset, plus 512K per
 * extra bank); a CoCo 1/2 has none (size 0). */
#define COCODEBUG_SPACE_CPU  0
#define COCODEBUG_SPACE_PHYS 1

/* The 6809's programmer-visible state. D is A:B, so it is not carried
 * separately. */
typedef struct {
    uint16_t pc, x, y, u, s;
    uint8_t a, b, dp, cc;
    /* HD6309 only (is6309 set): W is E:F; MD is the mode register. All zero
     * on a 6809. */
    int is6309;
    uint16_t w, v;
    uint8_t e, f, md;
} cocodebug_regs;

/* CC bits, for cocodebug_flag_set. */
enum {
    COCODEBUG_CC_C = 0x01, COCODEBUG_CC_V = 0x02, COCODEBUG_CC_Z = 0x04,
    COCODEBUG_CC_N = 0x08, COCODEBUG_CC_I = 0x10, COCODEBUG_CC_H = 0x20,
    COCODEBUG_CC_F = 0x40, COCODEBUG_CC_E = 0x80
};

typedef enum {
    COCODEBUG_REG_PC = 0, COCODEBUG_REG_A, COCODEBUG_REG_B, COCODEBUG_REG_D,
    COCODEBUG_REG_DP, COCODEBUG_REG_X, COCODEBUG_REG_Y, COCODEBUG_REG_U,
    COCODEBUG_REG_S, COCODEBUG_REG_CC,
    /* HD6309 only: the setter refuses them (returns 0) on a 6809. */
    COCODEBUG_REG_E, COCODEBUG_REG_F, COCODEBUG_REG_W, COCODEBUG_REG_V,
    COCODEBUG_REG_MD,
    COCODEBUG_REG_COUNT
} cocodebug_reg;

/* Created by cocosession_start (or cocosession_debugger before it); owned
 * by the session. */

/* Engage or release the debugger. Engaging does not pause: it switches the
 * emulator loop to the path that honours breakpoints, which costs one hook
 * call per instruction, so a session that never opens the debugger pays
 * nothing. Releasing resumes first if paused. */
void cocodebug_set_engaged(cocodebug *d, int engaged);
int  cocodebug_is_engaged(const cocodebug *d);

/* Pause at the next instruction boundary, or resume. cocodebug_pause returns
 * immediately -- the machine stops shortly after, and the stop serial below
 * is how a UI notices. */
void cocodebug_pause(cocodebug *d);
void cocodebug_resume(cocodebug *d);
int  cocodebug_is_paused(const cocodebug *d);

/* Run exactly one instruction, then pause again. */
void cocodebug_step(cocodebug *d);

/* Run until PC reaches addr (a one-shot breakpoint), then pause. */
void cocodebug_run_to(cocodebug *d, uint16_t addr);

/* Step over a subroutine call: if the instruction at PC is a BSR/LBSR/JSR,
 * run until it returns (a one-shot breakpoint on the following instruction);
 * otherwise this is cocodebug_step. Only meaningful while paused. */
void cocodebug_step_over(cocodebug *d);

/* Run until control leaves the current subroutine. Approximate in the way
 * every debugger's is -- it breaks on the instruction after the return, and a
 * routine that manipulates its own stack can defeat it. */
void cocodebug_step_out(cocodebug *d);

/* Bumped every time the machine stops. A UI polls this on its frame clock and
 * refreshes when it changes, rather than being called back on the emulator
 * thread. */
uint64_t cocodebug_stop_serial(const cocodebug *d);

/* ---- inspection: valid only while paused --------------------------------
 * Each returns 0 and touches nothing when the machine is running. */
int cocodebug_regs_get(cocodebug *d, cocodebug_regs *out);
/* Reads n bytes of the CPU's 64K as the SAM/GIME map it now, WITHOUT a bus
 * cycle (no FujiNet byte consumed, no PIA flag cleared; I/O reads as latched
 * values). Same as cocodebug_mem_read(d, COCODEBUG_SPACE_CPU, ...). Returns
 * the number read. */
int cocodebug_read(cocodebug *d, uint16_t addr, uint8_t *dst, int n);

/* Disassembles up to `max` instructions starting at addr, walking forward by
 * each one's length. Returns how many were produced -- 0 while running, since
 * it reads the machine's memory. This is what a frontend fills its
 * disassembly list from; it never has to know the encoding. */
int cocodebug_disassemble(cocodebug *d, uint16_t addr, coco_dasm_line *out,
                          int max);

/* ---- symbols --------------------------------------------------------------
 * A small built-in table of well-known Color BASIC ROM entry points (see
 * core/debugger/symtab.c for exactly which, and why only those), plus
 * whatever the caller loads on top. Available regardless of pause state --
 * this is just a name<->address mapping, not machine state. */

/* Loads a symbol file on top of the built-in table (and anything already
 * loaded): LWASM's `--symbol-dump` format ("NAME EQU $ADDR" per line) or a
 * plain "NAME ADDR" (hex with an optional $/0x prefix, or decimal).
 * Returns the number of symbols loaded, or -1 if the file could not be
 * opened. */
int cocodebug_symbols_load(cocodebug *d, const char *path);
/* Drops everything cocodebug_symbols_load added; the built-in table stays. */
void cocodebug_symbols_clear(cocodebug *d);
/* Exact-address lookup, e.g. to annotate a disassembly line. Returns 1 and
 * fills dst (truncated to dstsz) when addr has a symbol, 0 otherwise. */
int cocodebug_symbol_at(cocodebug *d, uint16_t addr, char *dst, int dstsz);
/* Name -> address, case-sensitive exact match. For a "Go to address" field
 * that also accepts a symbol name. */
int cocodebug_symbol_find(cocodebug *d, const char *name, uint16_t *addr_out);

/* ---- breakpoints -------------------------------------------------------- */
/* Returns 1 when the breakpoint is now set, 0 when it was cleared, -1 when
 * the table is full. Safe while running. */
int cocodebug_breakpoint_toggle(cocodebug *d, uint16_t addr);
int cocodebug_breakpoint_is_set(cocodebug *d, uint16_t addr);
void cocodebug_breakpoint_clear_all(cocodebug *d);
/* Copies the set addresses into out (ascending); returns how many. */
int cocodebug_breakpoint_list(cocodebug *d, uint16_t *out, int max);

/* ---- C source info (from a CMOC build) ------------------------------------
 * `cmoc -i` (--intermediate) keeps the assembler listing and linker map that
 * together map machine addresses back to C source lines, function extents,
 * and each function's parameters/locals (see core/debugger/csrcinfo.h for
 * exactly what CMOC emits and how it is parsed). Everything below is
 * available regardless of pause state, like the symbol table -- it is a
 * static description of the program, not the running machine -- except
 * cocodebug_locals_get, which reads live memory the same as
 * cocodebug_read. */

/* Parses lst_path and its linker map -- both only kept by `cmoc -i`.
 * Function/data symbols the map resolves are also loaded into the symbol
 * table, so the disassembly view picks up C names for free.
 *
 * ADDS to whatever source info is already loaded rather than replacing it
 * (see cocosrcinfo_load in csrcinfo.h for exactly how the .map is found,
 * including the multi-module case): call this once per .lst to debug a
 * program built from several C files, and cocodebug_source_clear() first
 * for a clean slate instead of accumulating.
 *
 * Best called while paused: a C-line step in flight targets the table
 * being changed, and is cancelled by this call either way. Returns the
 * number of source lines with code on them known after this call
 * (cumulative, not just this one), or -1 if the listing or a .map for it
 * could not be found. */
int cocodebug_source_load(cocodebug *d, const char *lst_path);
/* Drops everything cocodebug_source_load loaded (every module, not just
 * the most recent). */
void cocodebug_source_clear(cocodebug *d);
int cocodebug_source_available(const cocodebug *d);

/* Exact-address lookup, e.g. to find which source line PC is on. Returns 1
 * and fills *file_id_out and *line_out on a hit. */
int cocodebug_source_line_at(cocodebug *d, uint16_t addr, int *file_id_out,
                             int *line_out);
int cocodebug_source_file_count(cocodebug *d);
/* Full path, resolved against the listing's own directory at load time.
 * Valid for the table's lifetime (until the next _source_load or
 * _source_clear). NULL if file_id is out of range. */
const char *cocodebug_source_file_path(cocodebug *d, int file_id);

/* Resolves file_id:line to an address and toggles a normal breakpoint there
 * -- source and disassembly breakpoints are the same table underneath.
 * Same return convention as cocodebug_breakpoint_toggle, plus -2 when the
 * line has no code (a blank line, a declaration with no initializer, one
 * optimized away entirely). */
int cocodebug_source_breakpoint_toggle(cocodebug *d, int file_id, int line);
int cocodebug_source_breakpoint_is_set(cocodebug *d, int file_id, int line);

/* Run until control reaches a different source line, in a different
 * function or the same one. Falls back to cocodebug_step when there is no
 * source info loaded, or PC is not itself a mapped statement (mid-prologue,
 * or a line optimized away entirely). Only meaningful while paused. */
void cocodebug_step_line(cocodebug *d);
/* Like cocodebug_step_line, but a call on the current line is run to
 * completion rather than stepped into -- the source-level equivalent of
 * cocodebug_step_over. Falls back to cocodebug_step_over under the same
 * conditions cocodebug_step_line falls back to cocodebug_step. */
void cocodebug_step_over_line(cocodebug *d);

typedef struct {
    char     name[32];
    char     type[32];  /* CMOC's own type string, verbatim */
    int16_t  frame_off; /* signed displacement from U; >=0 is a parameter */
    uint8_t  size;
    int      is_param;
    int      has_value; /* 0 when size is >4 bytes (not decoded) */
    uint32_t value;      /* meaningful only when has_value is set */
} cocodebug_local;

/* The current function's parameters and locals, in declaration order, read
 * at U + frame_off through the machine's own memory (like cocodebug_read).
 * Returns 0 while running, when no source info is loaded, or when PC is
 * before the function's first C statement -- U is still the caller's frame
 * there, so there is nothing correct to report. */
int cocodebug_locals_get(cocodebug *d, cocodebug_local *out, int max);

/* ---- why it stopped: one locked snapshot ----------------------------------
 * The reason, the PC, the stop serial and the paused flag are set together
 * under the engine's lock, so a poller that sees `paused` also sees the
 * reason and serial of THAT stop. Use this (or cocodebug_stop_reason) rather
 * than cocodebug_is_paused + cocodebug_stop_serial pairs. */
typedef struct {
    int      paused;       /* the emulator thread is parked */
    uint64_t serial;       /* cocodebug_stop_serial at this stop */
    int      pc;           /* PC at the stop, -1 while running */
    int      reason_addr;  /* an address the reason names (the PC, or the
                            * breakpoint's), -1 while running */
    char     reason[160];  /* "stopped at $0412", "breakpoint: exec $0412",
                            * "wrote $0400 = $41 (was $20) at $A1CB",
                            * "step at $0412", "running" */
} cocodebug_stop;
void cocodebug_stop_snapshot(cocodebug *d, cocodebug_stop *out);
/* The reason text into dst, the address into *address (-1 while running).
 * Returns the length. */
int  cocodebug_stop_reason(cocodebug *d, char *dst, int dstsz, int *address);
/* Bumped whenever a command ran or the machine stopped or resumed: a window
 * refreshes when it changes. */
unsigned cocodebug_generation(cocodebug *d);

/* Edit a register while paused. Returns 1, 0 when running or when the
 * register does not exist on this CPU (E F W V MD on a 6809). D sets A:B;
 * CC takes all eight bits. A PC edit takes effect at the next fetch. */
int cocodebug_regs_set(cocodebug *d, int reg, unsigned value);
/* Set or clear CC bits (COCODEBUG_CC_*). Same rules. */
int cocodebug_flag_set(cocodebug *d, unsigned mask, int on);

/* ---- memory: side effects none ---------------------------------------------
 * Both spaces are read without a bus cycle: nothing is consumed from the
 * FujiNet, no PIA flag is cleared, no GIME interrupt is acknowledged. I/O
 * pages read as their latched values (cartridge I/O $FF40-$FF5F and the SAM
 * read $FF). Valid while paused only. */
uint32_t cocodebug_space_size(cocodebug *d, int space);  /* 0x10000, or the physical RAM size */
/* Returns the bytes read (clamped to the space), 0 while running. */
int cocodebug_mem_read(cocodebug *d, int space, uint32_t addr, uint8_t *dst, int n);
/* Stores into RAM only (ROM and I/O are skipped). Returns the bytes
 * stored; 0 while running. */
int cocodebug_mem_write(cocodebug *d, int space, uint32_t addr, const uint8_t *src, int n);

/* One region of the CPU's 64K as the SAM/GIME map it now. */
enum { COCODEBUG_REGION_UNMAPPED = 0, COCODEBUG_REGION_RAM, COCODEBUG_REGION_ROM,
       COCODEBUG_REGION_CART_ROM, COCODEBUG_REGION_IO };
typedef struct {
    uint16_t lo, hi;         /* inclusive */
    int      kind;           /* COCODEBUG_REGION_* */
    int      writable;
    int32_t  physical_base;  /* RAM on a CoCo 3: the PHYS address of lo, else -1 */
    char     name[32];       /* "RAM", "BASIC ROM", "PIA0", "GIME", "VECTORS" */
} cocodebug_region;
/* Ordered, gap-free, covering $0000-$FFFF. Valid while paused; while running
 * the map of the last stop (0 before the first). Returns the count. */
int cocodebug_memory_map(cocodebug *d, cocodebug_region *out, int max);

#define COCODEBUG_SEARCH_NOCASE 1
/* Stopped only (-1 while running). Finds up to `max` start addresses in
 * from..to of `space` (to bounds the START address; a match may run past it)
 * where (mem & mask) == (pat & mask) for all patlen bytes. Returns the
 * count. */
int cocodebug_search(cocodebug *d, int space, const uint8_t *pat, int patlen,
                     uint8_t mask, int flags, uint32_t from, uint32_t to,
                     uint32_t *out, int max);
/* The next match after `from` (before it if backward), wrapping once.
 * Returns 1 and *found, 0 for none, -1 while running. */
int cocodebug_search_next(cocodebug *d, int space, const uint8_t *pat, int patlen,
                          uint8_t mask, int flags, uint32_t from, int backward,
                          uint32_t *found);

/* ---- disassembly with the window's decorations ---------------------------- */
typedef struct {
    uint16_t address;
    int  is_pc;
    int  has_breakpoint;   /* an enabled execute breakpoint covers it */
    int  bp_disabled;      /* only a disabled one does (the grey dot) */
    int  length;
    char bytes[24];        /* "B6 04 00" */
    char label[48];        /* the symbol at this address, "" if none */
    char disasm[48];       /* "LDA   $0400" */
    char comment[48];      /* a branch target's symbol */
    unsigned flags;        /* COCO_DASM_* */
    int  flow;             /* COCO_FLOW_* */
    uint16_t target;       /* valid with COCO_DASM_BRANCH */
} cocodebug_line;
/* Up to `max` lines from addr. Returns the count (0 while running); *pc_line
 * is the PC's index in `out`, -1 if not among them. */
int cocodebug_disassemble_ex(cocodebug *d, uint16_t addr, cocodebug_line *out,
                             int max, int *pc_line);
/* The address `rows` instructions before (negative) or after `addr`: for
 * scrolling and Follow PC. Backwards is a best guess. 0 while running. */
int cocodebug_row_address(cocodebug *d, uint16_t addr, int rows);

/* ---- breakpoints, full form ------------------------------------------------
 * One table for every kind; the older cocodebug_breakpoint_toggle/_is_set/
 * _list/_clear_all work on its enabled execute entries. */
enum {
    COCODEBUG_BP_EXEC = 1, COCODEBUG_BP_READ = 2, COCODEBUG_BP_WRITE = 4,
    COCODEBUG_BP_ACCESS = 6,    /* READ | WRITE: one entry, "Access" */
    /* OR'd in: start/end are CoCo 3 PHYSICAL addresses (a bus access whose
     * RAM element is in the range). READ/WRITE/ACCESS only; refused on a
     * CoCo 1/2 (no physical space) and with EXEC. */
    COCODEBUG_BP_PHYS = 0x100
};
typedef struct {
    int      id;
    int      type;            /* COCODEBUG_BP_* bits */
    uint32_t start, end;      /* inclusive; CPU addresses unless PHYS */
    int      enabled;
    unsigned hits;
    char     condition[128];
} cocodebug_breakpoint;
/* Returns the new id, or -1 (full, bad type, or the condition does not
 * parse). Safe while running. */
int  cocodebug_breakpoint_add(cocodebug *d, int type, uint32_t start, uint32_t end,
                              const char *condition);
void cocodebug_breakpoint_remove(cocodebug *d, int id);
void cocodebug_breakpoint_enable(cocodebug *d, int id, int enabled);
/* Every entry, in creation order. Returns the count. */
int  cocodebug_breakpoints_list(cocodebug *d, cocodebug_breakpoint *out, int max);
/* Zero every hit count. */
void cocodebug_breakpoint_clear_hits(cocodebug *d);

/* ---- symbols tab -------------------------------------------------------------- */
int  cocodebug_symbols_count(cocodebug *d);
/* Entry `index` (0 .. count-1): returns 1, 0 past the end. Order is the
 * table's and changes when symbols are loaded or cleared. */
int  cocodebug_symbols_get(cocodebug *d, int index, uint16_t *addr, char *name,
                           int namesz);
/* Indices whose name contains `text` (case-insensitive; empty matches all),
 * ordered by address then name. Returns the count written. */
int  cocodebug_symbols_filter(cocodebug *d, const char *text, int *out, int max);
/* Adds one symbol (a user label). Returns 1. */
int  cocodebug_symbol_add(cocodebug *d, uint16_t addr, const char *name);

/* ---- the instruction trace ---------------------------------------------------
 * A 65536-entry ring, allocated on the first enable, under its own lock. It
 * fills from the instruction hook, so it records only while the debugger is
 * engaged. */
typedef struct {
    uint16_t pc;
    uint8_t  len;              /* instruction length (decoded on read) */
    uint8_t  bytes[6];         /* the instruction's bytes, peeked (LDQ # is 6) */
    uint8_t  a, b, dp, cc;
    uint16_t x, y, u, s;
    uint8_t  is6309;
    uint16_t w, v;
    uint8_t  md;
    uint64_t cycles;           /* CPU cycles since the debugger engaged */
    int16_t  line, hpos;       /* the beam: scanline, and colour clocks into it */
    uint32_t frame;
} cocodebug_trace_entry;
void cocodebug_trace_enable(cocodebug *d, int on);
int  cocodebug_trace_enabled(cocodebug *d);
/* Newest first. Returns the entries copied. */
int  cocodebug_trace_read(cocodebug *d, cocodebug_trace_entry *out, int max);
void cocodebug_trace_clear(cocodebug *d);
int  cocodebug_trace_count(cocodebug *d);
/* "A1CB  LDA   $0400    A=41 B=00 DP=00 X=0400 Y=0000 U=0000 S=7F00 CC=50". */
int  cocodebug_trace_format(const cocodebug_trace_entry *e, char *dst, int dstsz);
enum { COCODEBUG_TRACE_ALL = 0, COCODEBUG_TRACE_BRANCHES, COCODEBUG_TRACE_RANGE };
/* 1 if the entry passes (BRANCHES: COCO_FLOW_* other than NONE; RANGE: pc in
 * lo..hi). */
int  cocodebug_trace_match(const cocodebug_trace_entry *e, int filter, uint16_t lo,
                           uint16_t hi);

/* ---- the prompt -------------------------------------------------------------- */
/* Runs one command ("help" lists them). Text into dst; returns its length. */
int  cocodebug_command(cocodebug *d, const char *command, char *dst, int dstsz);
/* Completions for a prefix (commands, registers, symbols), one per line.
 * Returns the count. */
int  cocodebug_completions(cocodebug *d, const char *prefix, char *dst, int dstsz);
/* Evaluate an expression (registers, flags, [addr], {addr}, symbols, vpos hpos
 * frame cycles, C operators). 0 and *out, or -1 with a message. Needs the
 * machine parked for register and memory terms (they read 0 otherwise). */
int  cocodebug_eval(cocodebug *d, const char *text, long *out, char *err, int errsz);
/* The text the prompt's `video [section]` prints: section is NULL or a name;
 * returns the length, or <= 0 for "nothing yet". Set by whoever owns the
 * Video tab controller; until then `video` says it is not available. */
typedef int (*cocodebug_video_text_fn)(cocodebug *d, const char *section, char *dst,
                                       int dstsz);
void cocodebug_set_video_text(cocodebug_video_text_fn fn);

/* ---- Cart & I/O ------------------------------------------------------------
 * Valid while paused; each returns 0 and zeroes `out` while running. */
typedef struct {
    uint8_t  cra, crb;          /* control registers */
    uint8_t  ddra, ddrb;        /* direction registers */
    uint8_t  ora, orb;          /* output registers */
    uint8_t  pina, pinb;        /* the pin levels the CPU would read */
    int      ca1, ca2, cb1, cb2;
    int      irqa, irqb;        /* IRQ lines to the CPU */
    uint8_t  irq_flags_a, irq_flags_b;   /* the two flag bits as latched (bit 7 CRx) */
} cocodebug_pia;
typedef struct {
    int      is_coco3;
    int      is6309;
    cocodebug_pia pia0, pia1;
    /* the SAM */
    int      sam_valid;
    unsigned sam_v, sam_f, sam_p, sam_r, sam_m, sam_ty;
    unsigned sam_video_address;
    /* the GIME (CoCo 3 only) */
    int      gime_valid;
    uint8_t  gime_reg[16];      /* $FF90-$FF9F */
    uint8_t  gime_palette[16];  /* $FFB0-$FFBF, 6-bit */
    uint8_t  gime_mmu[16];      /* $FFA0-$FFAF */
    unsigned gime_scanline, gime_row;
    int      gime_irq, gime_firq;
    /* the joysticks (0 right, 1 left): axis 0..65535, buttons 0/1 */
    int      joy_axis[2][2];
    int      joy_button[2][2];
    /* the Becker/FujiNet link */
    int      becker_link_up;
    int      becker_rx_pending;
} cocodebug_io;
int cocodebug_io_get(cocodebug *d, cocodebug_io *out);

/* The cartridge as text: "Cartridge: HDB-DOS DriveWire (Becker) 'hdbdw3bck'" */
int cocodebug_cart_info(cocodebug *d, char *dst, int dstsz);

/* ---- sound ---------------------------------------------------------------- */
typedef struct {
    int dac;            /* the 6-bit DAC level, 0..63 (PIA1 A bits 2-7) */
    int single_bit;     /* PIA1 B bit 1 */
    int sound_enable;   /* PIA1 CB2: the mux output reaches the speaker */
    int motor;          /* PIA1 CA2: the cassette motor relay */
    int mux_source;     /* PIA0 CB2:CA2: 0 DAC, 1 cassette, 2 cartridge, 3 none */
    const char *mux_name;
    int cassette_in;    /* PIA1 A bit 0: the cassette data comparator */
} cocodebug_sound;
int cocodebug_sound_get(cocodebug *d, cocodebug_sound *out);
/* Turn the recording of writes to the PIA1 DAC ($FF20) and single-bit ($FF22)
 * registers on or off. Off by default: it needs the machine's bus tap, which
 * a debugger without access breakpoints or a Sound tab leaves alone. Call it
 * (on) when a Sound tab opens and (off) when it closes. */
void cocodebug_sound_watch(cocodebug *d, int on);
/* Those writes, newest first, a small ring of 4096. Needs the debugger
 * engaged and cocodebug_sound_watch on. */
typedef struct {
    uint32_t tick;      /* event tick (14.318 MHz clocks) of the write */
    uint8_t  port;      /* 0: $FF20 (DAC), 2: $FF22 (single bit) */
    uint8_t  value;
    uint8_t  dac;       /* the DAC level after the write (0..63) */
} cocodebug_sound_write;
int  cocodebug_sound_history(cocodebug *d, cocodebug_sound_write *out, int max);
void cocodebug_sound_history_clear(cocodebug *d);

/* ---- FujiNet: the DriveWire traffic on the Becker port --------------------
 * Recorded from session start whether or not the debugger is engaged, so it
 * shows what happened before anyone looked. Safe from any thread. See
 * core/debugger/dwtrace.h for how the bytes are cut into entries. */
typedef struct {
    uint64_t serial;   /* new whenever the entry changes */
    int      to_host;  /* 1: FujiNet -> CoCo with no request before it */
    int      device;   /* the DriveWire opcode (E2 FUJI, D2 READEX...), -1 */
    int      command;  /* the FUJI/NET/CPM/CLOCK command byte, else -1 */
    int      length;   /* bytes FujiNet sent back, -1 for none */
    int      nbytes;   /* bytes in the exchange, both ways */
    uint8_t  data[64]; /* its first bytes: the request's, then the reply's */
    char     text[96]; /* "→ FUJI E8 GET ADAPTERCONFIG ← 140 B" */
    uint32_t repeat;   /* identical exchanges in a row folded into this one */
} coco_fuji_packet;

/* The newest max entries, oldest first. Returns how many. */
int  cocodebug_fujinet_trace(cocodebug *d, coco_fuji_packet *out, int max);
void cocodebug_fujinet_trace_clear(cocodebug *d);
/* A bus_ready probe (FUJI 00 DEVICE READY): what "Hide READY probes"
 * hides. */
int  cocodebug_fujinet_is_ready(const coco_fuji_packet *p);
/* The Becker port's state as text lines, for the pane beside the list. */
int  cocodebug_fujinet_state(cocodebug *d, char *dst, int dstsz);

#ifdef __cplusplus
}
#endif

#endif /* COCODEBUG_H */
