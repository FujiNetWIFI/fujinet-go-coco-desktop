/*
 * cocodebug -- the 6809 debugger engine.
 *
 * Built on XRoar's own hooks rather than a stepped core path of its own:
 * struct debug_cpu (xroar src/debug_cpu.h) offers an instruction_hook called
 * before each fetch, and that is where this parks the emulator thread when
 * the machine is paused. Neither the Dragon nor the CoCo 3 machine uses that
 * hook -- both use instruction_posthook, for traps -- so nothing has to be
 * chained.
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
#define COCODEBUG_MAX_BREAKPOINTS 64

/* The 6809's programmer-visible state. D is A:B, so it is not carried
 * separately. */
typedef struct {
    uint16_t pc, x, y, u, s;
    uint8_t a, b, dp, cc;
} cocodebug_regs;

/* Lazily created by cocosession_debugger(); owned by the session. */

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
/* Reads n bytes from the machine's address space (through its own read path,
 * so memory-mapped hardware and the CoCo 3's MMU behave as the CPU sees
 * them). Returns the number read. */
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

/* Parses lst_path and its sibling .map (same directory and basename,
 * extension ".map") -- both only kept by `cmoc -i`. Function/data symbols
 * the map resolves are also loaded into the symbol table, so the
 * disassembly view picks up C names for free. Replaces anything already
 * loaded. Best called while paused: a C-line step in flight targets the
 * table being replaced, and is cancelled by this call either way. Returns
 * the number of source lines with code on them, or -1 if the listing or its
 * .map could not be read. */
int cocodebug_source_load(cocodebug *d, const char *lst_path);
/* Drops everything cocodebug_source_load loaded. */
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

#ifdef __cplusplus
}
#endif

#endif /* COCODEBUG_H */
