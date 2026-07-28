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

/* ---- breakpoints -------------------------------------------------------- */
/* Returns 1 when the breakpoint is now set, 0 when it was cleared, -1 when
 * the table is full. Safe while running. */
int cocodebug_breakpoint_toggle(cocodebug *d, uint16_t addr);
int cocodebug_breakpoint_is_set(cocodebug *d, uint16_t addr);
void cocodebug_breakpoint_clear_all(cocodebug *d);
/* Copies the set addresses into out (ascending); returns how many. */
int cocodebug_breakpoint_list(cocodebug *d, uint16_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* COCODEBUG_H */
