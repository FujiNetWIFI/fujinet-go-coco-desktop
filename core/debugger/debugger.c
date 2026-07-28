/*
 * cocodebug -- the 6809 debugger engine.
 *
 * The whole design is one idea: XRoar calls instruction_hook before every
 * instruction fetch, on the emulator thread, so that is where the machine is
 * stopped. When the engine wants to pause, the hook snapshots the registers
 * and then blocks on a condition variable until a UI thread resumes it. The
 * emulator thread is parked inside xroar_run for as long as that lasts, which
 * is exactly what "paused" should mean -- no stepped core path of our own, no
 * second copy of the CPU's timing.
 *
 * That parking is also why cocodebug_regs_get and cocodebug_read are only
 * valid while paused: the machine is quiescent then, so reading it from a UI
 * thread is safe without a lock XRoar does not have.
 *
 * See cocodebug.h for the contract, and COMPLIANCE.md for why the engine
 * builds on upstream's hooks rather than reimplementing them.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "top-config.h"
#include "delegate.h"
#include "machine.h"
#include "mc6809/mc6809.h"
#include "part.h"
#include "xroar.h"

#include "coco_host.h"
#include "cocodebug.h"
#include "compat.h"
#include "m6809dasm.h"
#include "session_internal.h"

struct cocodebug {
    cocosession *s;

    pthread_mutex_t mtx;
    pthread_cond_t cv;

    int engaged;
    int paused;          /* the emulator thread is parked in the hook */
    int pause_request;   /* stop at the next instruction boundary */
    int stop_all;        /* the session is going away; never park again */

    /* >0 while stepping: instructions still to run before pausing. */
    int step_remaining;
    /* A one-shot breakpoint for step-over / step-out, or -1. */
    long transient_bp;

    uint16_t bp[COCODEBUG_MAX_BREAKPOINTS];
    int nbp;

    cocodebug_regs regs;
    int regs_valid;
    uint64_t stop_serial;

    /* The CPU our hook is installed on. The machine is rebuilt on a CoCo 2 /
     * CoCo 3 switch and the hook goes with it, so this is re-checked every
     * frame rather than installed once. */
    struct MC6809 *hooked_cpu;
};

/* ---- reaching the machine ----------------------------------------------
 * Emulator thread only. Returns NULL before the machine is built. */
static struct MC6809 *current_cpu(void)
{
    if (!xroar.machine)
        return NULL;
    return (struct MC6809 *)part_component_by_id_is_a(&xroar.machine->part,
                                                      "CPU", "MC6809");
}

static void snapshot_regs(struct cocodebug *d, struct MC6809 *cpu)
{
    d->regs.pc = cpu->reg_pc;
    d->regs.x = cpu->reg_x;
    d->regs.y = cpu->reg_y;
    d->regs.u = cpu->reg_u;
    d->regs.s = cpu->reg_s;
    d->regs.a = MC6809_REG_A(cpu);
    d->regs.b = MC6809_REG_B(cpu);
    d->regs.dp = cpu->reg_dp;
    d->regs.cc = cpu->reg_cc;
    d->regs_valid = 1;
}

/* Caller holds d->mtx. */
static int breakpoint_hit_locked(struct cocodebug *d, uint16_t pc)
{
    int i;
    if (d->transient_bp >= 0 && (uint16_t)d->transient_bp == pc)
        return 1;
    for (i = 0; i < d->nbp; i++)
        if (d->bp[i] == pc)
            return 1;
    return 0;
}

/* ---- the hook -----------------------------------------------------------
 * Emulator thread, called before every instruction fetch while engaged. */
static void instruction_hook(void *sptr)
{
    struct cocodebug *d = sptr;
    struct MC6809 *cpu = d->hooked_cpu;
    uint16_t pc;
    int stop = 0;

    if (!cpu || d->stop_all)
        return;
    pc = cpu->reg_pc;

    pthread_mutex_lock(&d->mtx);

    if (d->pause_request) {
        d->pause_request = 0;
        stop = 1;
    } else if (d->step_remaining > 0) {
        /* The step counter is decremented on the way IN, so a step of one
         * runs the instruction we are about to fetch and stops before the
         * next -- which is what a user watching the PC expects. */
        if (--d->step_remaining == 0)
            stop = 1;
    }
    if (breakpoint_hit_locked(d, pc)) {
        /* A one-shot breakpoint is spent whether or not it is what stopped
         * us, so step-over cannot leave one behind to fire later. */
        d->transient_bp = -1;
        d->step_remaining = 0;
        stop = 1;
    }

    if (!stop) {
        pthread_mutex_unlock(&d->mtx);
        return;
    }

    snapshot_regs(d, cpu);
    d->paused = 1;
    d->stop_serial++;
    /* Park here. The session's stop path calls cocodebug_resume_for_stop, so
     * a shutdown cannot deadlock behind a user who walked away. */
    while (d->paused && !d->stop_all)
        pthread_cond_wait(&d->cv, &d->mtx);
    d->paused = 0;
    d->regs_valid = 0;
    pthread_mutex_unlock(&d->mtx);
}

/* ---- lifecycle ---------------------------------------------------------- */

cocodebug *cocodebug_create(cocosession *s)
{
    struct cocodebug *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->s = s;
    d->transient_bp = -1;
    pthread_mutex_init(&d->mtx, NULL);
    coco_cond_init_monotonic(&d->cv);
    return d;
}

void cocodebug_destroy(cocodebug *d)
{
    if (!d)
        return;
    pthread_mutex_destroy(&d->mtx);
    pthread_cond_destroy(&d->cv);
    free(d);
}

void cocodebug_resume_for_stop(cocodebug *d)
{
    if (!d)
        return;
    pthread_mutex_lock(&d->mtx);
    d->stop_all = 1;
    d->paused = 0;
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->mtx);
}

/* ---- the emulator-thread frame ------------------------------------------
 * The session calls this instead of cocohost_core_run_frame while engaged. */
int cocodebug_session_frame(cocosession *s)
{
    struct cocodebug *d = s->debugger;
    struct MC6809 *cpu;

    if (!d) {
        cocohost_core_run_frame();
        return 1;
    }

    /* Re-attach every frame rather than once: switching between a CoCo 2 and
     * a CoCo 3 rebuilds the machine, and the new CPU has no hook on it. One
     * pointer compare per frame buys not having to notice the switch. */
    cpu = current_cpu();
    if (cpu != d->hooked_cpu) {
        d->hooked_cpu = cpu;
        if (cpu)
            cpu->debug_cpu.instruction_hook =
                DELEGATE_AS0(void, instruction_hook, d);
    }

    cocohost_core_run_frame();
    return 1;
}

/* ---- engage / pause / step ---------------------------------------------- */

void cocodebug_set_engaged(cocodebug *d, int engaged)
{
    if (!d)
        return;
    engaged = engaged ? 1 : 0;
    if (!engaged)
        cocodebug_resume(d);

    pthread_mutex_lock(&d->mtx);
    d->engaged = engaged;
    pthread_mutex_unlock(&d->mtx);

    /* The session's loop reads this to decide which frame path to run. It is
     * set after the engine's own state so the loop cannot arrive first. */
    d->s->dbg_engaged = engaged;

    if (!engaged && d->hooked_cpu) {
        /* Leave the machine as we found it: the hook is per-instruction, and
         * a released debugger should cost nothing at all. Safe to clear from
         * here because the loop has already stopped calling us. */
        d->hooked_cpu->debug_cpu.instruction_hook.func = NULL;
        d->hooked_cpu = NULL;
    }
}

int cocodebug_is_engaged(const cocodebug *d)
{
    return d ? d->engaged : 0;
}

void cocodebug_pause(cocodebug *d)
{
    if (!d)
        return;
    pthread_mutex_lock(&d->mtx);
    d->pause_request = 1;
    d->step_remaining = 0;
    pthread_mutex_unlock(&d->mtx);
}

void cocodebug_resume(cocodebug *d)
{
    if (!d)
        return;
    pthread_mutex_lock(&d->mtx);
    d->pause_request = 0;
    d->step_remaining = 0;
    d->transient_bp = -1;
    d->paused = 0;
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->mtx);
}

int cocodebug_is_paused(const cocodebug *d)
{
    return d ? d->paused : 0;
}

void cocodebug_step(cocodebug *d)
{
    if (!d)
        return;
    pthread_mutex_lock(&d->mtx);
    d->step_remaining = 1;
    d->transient_bp = -1;
    d->paused = 0;
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->mtx);
}

uint64_t cocodebug_stop_serial(const cocodebug *d)
{
    return d ? d->stop_serial : 0;
}

/* ---- inspection --------------------------------------------------------- */

int cocodebug_regs_get(cocodebug *d, cocodebug_regs *out)
{
    int ok = 0;
    if (!d || !out)
        return 0;
    pthread_mutex_lock(&d->mtx);
    if (d->paused && d->regs_valid) {
        *out = d->regs;
        ok = 1;
    }
    pthread_mutex_unlock(&d->mtx);
    return ok;
}

int cocodebug_read(cocodebug *d, uint16_t addr, uint8_t *dst, int n)
{
    int i;

    if (!d || !dst || n <= 0)
        return 0;
    /* Only while parked: the emulator thread is inside the hook, so the
     * machine cannot be halfway through a bus cycle. */
    if (!d->paused || !xroar.machine || !xroar.machine->read_byte)
        return 0;

    for (i = 0; i < n; i++)
        dst[i] = xroar.machine->read_byte(xroar.machine,
                                          (unsigned)(uint16_t)(addr + i), 0);
    return n;
}

/* Read one byte for the disassembler, through the machine's own path. */
static uint8_t dasm_read(uint16_t addr, void *user)
{
    (void)user;
    if (!xroar.machine || !xroar.machine->read_byte)
        return 0;
    return xroar.machine->read_byte(xroar.machine, addr, 0);
}

/* Which opcode table to disassemble with. An HD6309 part_is_a()'s "MC6809"
 * too (it really is one, extended), so the specific query has to come
 * first: this asks "is it actually the 6309", not "is it 6809-compatible",
 * which every CPU here would answer yes to. */
static coco_dasm_cpu current_dasm_cpu(void)
{
    if (xroar.machine &&
        part_component_by_id_is_a(&xroar.machine->part, "CPU", "HD6309"))
        return COCO_DASM_CPU_HD6309;
    return COCO_DASM_CPU_MC6809;
}

int cocodebug_disassemble(cocodebug *d, uint16_t addr, coco_dasm_line *out,
                          int max)
{
    int n = 0;
    coco_dasm_cpu cpu;

    if (!d || !out || max <= 0)
        return 0;
    /* Same rule as cocodebug_read, for the same reason: this walks the
     * machine's memory, which is only still while it is parked. */
    if (!d->paused)
        return 0;

    cpu = current_dasm_cpu();
    while (n < max) {
        coco_dasm_one_cpu(addr, dasm_read, NULL, cpu, &out[n]);
        addr = (uint16_t)(addr + out[n].len);
        n++;
    }
    return n;
}

void cocodebug_step_over(cocodebug *d)
{
    coco_dasm_line line;

    if (!d)
        return;
    if (!d->paused) {
        cocodebug_step(d);
        return;
    }

    coco_dasm_one_cpu(d->regs.pc, dasm_read, NULL, current_dasm_cpu(), &line);
    if (!(line.flags & COCO_DASM_CALL)) {
        /* Not a call: stepping over it is just stepping. */
        cocodebug_step(d);
        return;
    }

    pthread_mutex_lock(&d->mtx);
    /* Break where the call will return to. This is the whole reason the
     * disassembler reports a length. */
    d->transient_bp = (uint16_t)(d->regs.pc + line.len);
    d->step_remaining = 0;
    d->paused = 0;
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->mtx);
}

void cocodebug_step_out(cocodebug *d)
{
    uint8_t ret[2];

    if (!d)
        return;
    if (!d->paused) {
        cocodebug_step(d);
        return;
    }

    /* The 6809 pushes the return address last, so it is at the top of the
     * stack -- as long as the routine has not pushed anything since entry.
     * Every debugger's step-out is approximate in this way. */
    if (cocodebug_read(d, d->regs.s, ret, 2) != 2) {
        cocodebug_step(d);
        return;
    }

    pthread_mutex_lock(&d->mtx);
    d->transient_bp = (uint16_t)((ret[0] << 8) | ret[1]);
    d->step_remaining = 0;
    d->paused = 0;
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->mtx);
}

/* ---- breakpoints -------------------------------------------------------- */

int cocodebug_breakpoint_toggle(cocodebug *d, uint16_t addr)
{
    int i, rc;

    if (!d)
        return -1;
    pthread_mutex_lock(&d->mtx);
    for (i = 0; i < d->nbp; i++) {
        if (d->bp[i] != addr)
            continue;
        d->bp[i] = d->bp[--d->nbp];
        pthread_mutex_unlock(&d->mtx);
        return 0;
    }
    if (d->nbp >= COCODEBUG_MAX_BREAKPOINTS) {
        rc = -1;
    } else {
        d->bp[d->nbp++] = addr;
        rc = 1;
    }
    pthread_mutex_unlock(&d->mtx);
    return rc;
}

int cocodebug_breakpoint_is_set(cocodebug *d, uint16_t addr)
{
    int i, found = 0;
    if (!d)
        return 0;
    pthread_mutex_lock(&d->mtx);
    for (i = 0; i < d->nbp; i++)
        if (d->bp[i] == addr)
            found = 1;
    pthread_mutex_unlock(&d->mtx);
    return found;
}

void cocodebug_breakpoint_clear_all(cocodebug *d)
{
    if (!d)
        return;
    pthread_mutex_lock(&d->mtx);
    d->nbp = 0;
    pthread_mutex_unlock(&d->mtx);
}

static int cmp_u16(const void *a, const void *b)
{
    uint16_t x = *(const uint16_t *)a, y = *(const uint16_t *)b;
    return (x > y) - (x < y);
}

int cocodebug_breakpoint_list(cocodebug *d, uint16_t *out, int max)
{
    int n;
    if (!d || !out || max <= 0)
        return 0;
    pthread_mutex_lock(&d->mtx);
    n = d->nbp < max ? d->nbp : max;
    memcpy(out, d->bp, (size_t)n * sizeof(uint16_t));
    pthread_mutex_unlock(&d->mtx);
    /* Toggling removes by swapping with the last entry, so the table is not
     * ordered; a UI listing them wants it to be. */
    qsort(out, (size_t)n, sizeof(uint16_t), cmp_u16);
    return n;
}
