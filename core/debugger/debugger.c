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
#include <stdio.h>
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
#include "csrcinfo.h"
#include "dwtrace.h"
#include "m6809dasm.h"
#include "session_internal.h"
#include "symtab.h"

/* Bound on instructions run during a C-line step before giving up. Covers
 * stepping onto a line whose statement calls into code with no debug info
 * (a ROM routine, a hand-written .s helper) and never returns to a mapped
 * address on its own -- generous enough to survive a real printf() many
 * times over, but not so large that a genuine runaway hangs the UI. */
#define SRC_STEP_BUDGET 200000

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

    /* C-line stepping: 0 off, 1 step-into, 2 step-over. Checked in the hook
     * alongside step_remaining/transient_bp above, and mutually exclusive
     * with them -- entering one clears the others. */
    int      src_step;
    int      src_step_file, src_step_line;
    uint16_t src_step_s;       /* S when the step began, for step-over's
                                 * call-depth check. */
    long     src_step_budget;  /* counts down; 0 gives up and stops anyway. */

    uint16_t bp[COCODEBUG_MAX_BREAKPOINTS];
    int nbp;

    cocodebug_regs regs;
    int regs_valid;
    uint64_t stop_serial;

    /* The CPU our hook is installed on. The machine is rebuilt on a CoCo 2 /
     * CoCo 3 switch and the hook goes with it, so this is re-checked every
     * frame rather than installed once. */
    struct MC6809 *hooked_cpu;

    /* Name<->address table for annotating the disassembly view. Not tied to
     * pause state or the machine, so it needs none of the locking above. */
    cocosymtab *symtab;

    /* C line/variable info from a CMOC build (see csrcinfo.h). Unlike
     * symtab, this IS read from the hook while a C-line step is in flight,
     * so swapping it (cocodebug_source_load/_clear) takes mtx. */
    cocosrcinfo *srcinfo;

    /* The FujiNet tab's DriveWire trace: fed from the Becker port on the
     * emulator thread, read from the UI, so under a lock of its own. */
    pthread_mutex_t trace_mtx;
    dwtrace *trace;
};

/* The debugger the Becker port's tap feeds. */
static cocodebug *g_trace_dbg;

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
    if (d->src_step && cocosrcinfo_is_stmt(d->srcinfo, pc)) {
        int f, l;
        cocosrcinfo_line_at(d->srcinfo, pc, &f, &l);
        /* Step-over ignores anything deeper than where it started: S grows
         * downward on the 6809, so a called routine's frame sits below the
         * caller's, and at a statement boundary the callee's own frame is
         * always already set up -- that is what makes this comparison
         * stable within a function rather than just at entry. Step-into
         * skips the check entirely, since any new statement is the goal. */
        if ((f != d->src_step_file || l != d->src_step_line) &&
            (d->src_step == 1 || cpu->reg_s >= d->src_step_s)) {
            d->src_step = 0;
            stop = 1;
        }
    }
    if (d->src_step && --d->src_step_budget <= 0) {
        /* Ran on for SRC_STEP_BUDGET instructions without reaching a new
         * mapped statement -- most likely a call into code with no line
         * info that never returns to a mapped address on its own. Stop
         * where we are rather than hang. */
        d->src_step = 0;
        stop = 1;
    }
    if (breakpoint_hit_locked(d, pc)) {
        /* A one-shot breakpoint is spent whether or not it is what stopped
         * us, so step-over cannot leave one behind to fire later. */
        d->transient_bp = -1;
        d->step_remaining = 0;
        d->src_step = 0;
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
    d->symtab = cocosymtab_create();
    d->srcinfo = cocosrcinfo_create();
    pthread_mutex_init(&d->trace_mtx, NULL);
    d->trace = dwtrace_create();
    return d;
}

void cocodebug_destroy(cocodebug *d)
{
    if (!d)
        return;
    if (g_trace_dbg == d) {
        cocohost_set_becker_trace(NULL);
        g_trace_dbg = NULL;
    }
    cocosymtab_destroy(d->symtab);
    cocosrcinfo_destroy(d->srcinfo);
    dwtrace_destroy(d->trace);
    pthread_mutex_destroy(&d->trace_mtx);
    pthread_mutex_destroy(&d->mtx);
    pthread_cond_destroy(&d->cv);
    free(d);
}

/* ---- symbols -------------------------------------------------------------
 * UI-thread only, like everything else that is not marked otherwise; the
 * table itself has nothing to do with the paused/running machine. */

int cocodebug_symbols_load(cocodebug *d, const char *path)
{
    return d ? cocosymtab_load(d->symtab, path) : -1;
}

void cocodebug_symbols_clear(cocodebug *d)
{
    if (d) cocosymtab_clear_user(d->symtab);
}

int cocodebug_symbol_at(cocodebug *d, uint16_t addr, char *dst, int dstsz)
{
    return d ? cocosymtab_lookup_addr(d->symtab, addr, dst, dstsz) : 0;
}

int cocodebug_symbol_find(cocodebug *d, const char *name, uint16_t *addr_out)
{
    return d ? cocosymtab_lookup_name(d->symtab, name, addr_out) : 0;
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
    d->src_step = 0;
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
    d->src_step = 0;
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
    d->src_step = 0;
    d->paused = 0;
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->mtx);
}

/* Common tail for cocodebug_step_line / cocodebug_step_over_line: install the
 * (file, line) the step should escape, arm the hook, and let the emulator
 * thread go. Caller holds nothing; this takes d->mtx itself. */
static void arm_src_step(cocodebug *d, int kind, int file, int line,
                         uint16_t s_at_start)
{
    pthread_mutex_lock(&d->mtx);
    d->src_step = kind;
    d->src_step_file = file;
    d->src_step_line = line;
    d->src_step_s = s_at_start;
    d->src_step_budget = SRC_STEP_BUDGET;
    d->step_remaining = 0;
    d->transient_bp = -1;
    d->paused = 0;
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->mtx);
}

void cocodebug_step_line(cocodebug *d)
{
    int file, line;

    if (!d)
        return;
    /* Falls back to a plain instruction step when there is nothing to
     * target: no source info loaded, or PC is not itself a mapped statement
     * (mid-prologue, or an address optimized out of the line table) -- in
     * either case there is no (file, line) to compare against. */
    if (!d->paused || cocosrcinfo_empty(d->srcinfo) ||
        !cocosrcinfo_line_at(d->srcinfo, d->regs.pc, &file, &line)) {
        cocodebug_step(d);
        return;
    }
    arm_src_step(d, 1, file, line, 0);
}

void cocodebug_step_over_line(cocodebug *d)
{
    int file, line;

    if (!d)
        return;
    if (!d->paused || cocosrcinfo_empty(d->srcinfo) ||
        !cocosrcinfo_line_at(d->srcinfo, d->regs.pc, &file, &line)) {
        cocodebug_step_over(d);
        return;
    }
    arm_src_step(d, 2, file, line, d->regs.s);
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

/* ---- C source info (from a CMOC build) ----------------------------------- */

int cocodebug_source_load(cocodebug *d, const char *lst_path)
{
    int n;
    if (!d)
        return -1;
    /* Takes mtx because, unlike symtab, this table IS read from the hook
     * while a C-line step is in flight (see instruction_hook). Safe to call
     * while running -- the hook will simply block for the duration of the
     * file read, the same brief stall an emulator instruction takes on any
     * other contended lock -- but callers should still prefer to do this
     * while paused, since a step in flight is aimed at the table being
     * replaced out from under it. */
    pthread_mutex_lock(&d->mtx);
    n = cocosrcinfo_load(d->srcinfo, lst_path, d->symtab);
    d->src_step = 0;
    pthread_mutex_unlock(&d->mtx);
    return n;
}

void cocodebug_source_clear(cocodebug *d)
{
    if (!d)
        return;
    pthread_mutex_lock(&d->mtx);
    cocosrcinfo_clear(d->srcinfo);
    d->src_step = 0;
    pthread_mutex_unlock(&d->mtx);
}

int cocodebug_source_available(const cocodebug *d)
{
    return d && !cocosrcinfo_empty(d->srcinfo);
}

int cocodebug_source_line_at(cocodebug *d, uint16_t addr, int *file_id_out,
                             int *line_out)
{
    return d ? cocosrcinfo_line_at(d->srcinfo, addr, file_id_out, line_out) : 0;
}

int cocodebug_source_file_count(cocodebug *d)
{
    return d ? cocosrcinfo_file_count(d->srcinfo) : 0;
}

const char *cocodebug_source_file_path(cocodebug *d, int file_id)
{
    return d ? cocosrcinfo_file_path(d->srcinfo, file_id) : NULL;
}

int cocodebug_source_breakpoint_toggle(cocodebug *d, int file_id, int line)
{
    uint16_t addr;
    if (!d)
        return -1;
    if (!cocosrcinfo_addr_for_line(d->srcinfo, file_id, line, &addr))
        return -2;
    return cocodebug_breakpoint_toggle(d, addr);
}

int cocodebug_source_breakpoint_is_set(cocodebug *d, int file_id, int line)
{
    uint16_t addr;
    if (!d)
        return 0;
    if (!cocosrcinfo_addr_for_line(d->srcinfo, file_id, line, &addr))
        return 0;
    return cocodebug_breakpoint_is_set(d, addr);
}

int cocodebug_locals_get(cocodebug *d, cocodebug_local *out, int max)
{
    const cocosrc_func *f;
    const cocosrc_var *vars;
    cocodebug_regs regs;
    int nv, n, i;

    if (!d || !out || max <= 0)
        return 0;
    /* A consistent snapshot of PC and U, the same way any other inspection
     * call sees the paused machine's state. */
    if (!cocodebug_regs_get(d, &regs))
        return 0;

    f = cocosrcinfo_func_at(d->srcinfo, regs.pc);
    /* Before body_start, U is still whichever frame the caller left it in
     * (see cocosrc_func's comment in csrcinfo.h): reporting locals there
     * would show someone else's stack under this function's variable
     * names. */
    if (!f || regs.pc < f->body_start)
        return 0;

    vars = cocosrcinfo_func_vars(d->srcinfo, f, &nv);
    n = nv < max ? nv : max;
    for (i = 0; i < n; i++) {
        uint16_t addr = (uint16_t)(regs.u + vars[i].frame_off);
        int sz = vars[i].size;
        uint8_t buf[4];

        snprintf(out[i].name, sizeof(out[i].name), "%s", vars[i].name);
        snprintf(out[i].type, sizeof(out[i].type), "%s", vars[i].type);
        out[i].frame_off = vars[i].frame_off;
        out[i].size = vars[i].size;
        out[i].is_param = vars[i].is_param;
        out[i].value = 0;
        out[i].has_value = 0;

        if (sz >= 1 && sz <= 4 && cocodebug_read(d, addr, buf, sz) == sz) {
            uint32_t v = 0;
            int b;
            /* The 6809 is big-endian, so the first byte read is the most
             * significant -- the natural order to fold into a uint32_t. */
            for (b = 0; b < sz; b++)
                v = (v << 8) | buf[b];
            out[i].value = v;
            out[i].has_value = 1;
        }
    }
    return n;
}

/* ---- FujiNet trace ------------------------------------------------------ */

/* Emulator thread, from becker.c. */
static void becker_trace(int dir, const uint8_t *buf, size_t len)
{
    cocodebug *d = g_trace_dbg;
    if (!d)
        return;
    pthread_mutex_lock(&d->trace_mtx);
    dwtrace_feed(d->trace, dir, buf, len);
    pthread_mutex_unlock(&d->trace_mtx);
}

void cocodebug_trace_install(cocodebug *d)
{
    g_trace_dbg = d;
    cocohost_set_becker_trace(d ? becker_trace : NULL);
}

int cocodebug_fujinet_trace(cocodebug *d, coco_fuji_packet *out, int max)
{
    int n;
    if (!d)
        return 0;
    pthread_mutex_lock(&d->trace_mtx);
    n = dwtrace_get(d->trace, out, max);
    pthread_mutex_unlock(&d->trace_mtx);
    return n;
}

void cocodebug_fujinet_trace_clear(cocodebug *d)
{
    if (!d)
        return;
    pthread_mutex_lock(&d->trace_mtx);
    dwtrace_clear(d->trace);
    pthread_mutex_unlock(&d->trace_mtx);
}

int cocodebug_fujinet_is_ready(const coco_fuji_packet *p)
{
    return p && p->device == 0xE2 && p->command == 0x00;
}

int cocodebug_fujinet_state(cocodebug *d, char *dst, int dstsz)
{
    unsigned long long out, in, ex;
    char pending[96];
    int n;

    if (!d || !dst || dstsz <= 0)
        return 0;
    pthread_mutex_lock(&d->trace_mtx);
    out = dwtrace_bytes(d->trace, 1);
    in = dwtrace_bytes(d->trace, 0);
    ex = dwtrace_exchanges(d->trace);
    dwtrace_pending(d->trace, pending, sizeof(pending));
    pthread_mutex_unlock(&d->trace_mtx);

    n = snprintf(dst, (size_t)dstsz,
                 "Becker port     127.0.0.1:%d\n"
                 "FujiNet         %s\n"
                 "Link            %s\n"
                 "Unread          %d B\n"
                 "\n"
                 "CoCo \xe2\x86\x92 FujiNet  %llu B\n"
                 "FujiNet \xe2\x86\x92 CoCo  %llu B\n"
                 "Exchanges       %llu\n"
                 "\n"
                 "In progress     %s\n",
                 COCOSESSION_BECKER_PORT,
                 d->s->fujinet_running ? "listening" : "off",
                 cocohost_becker_link_up() ? "connected" : "not connected",
                 cocohost_becker_rx_pending(), out, in, ex, pending);
    return n < dstsz ? n : dstsz - 1;
}
