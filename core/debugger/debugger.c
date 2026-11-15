/*
 * cocodebug -- the 6809 debugger engine.
 *
 * XRoar calls instruction_hook before every instruction fetch, on the
 * emulator thread, so that is where the machine is stopped. When the engine
 * wants to pause, the hook snapshots the registers and then blocks on a
 * condition variable until a UI thread resumes it. The emulator thread is
 * parked inside xroar_run for as long as that lasts, which is exactly what
 * "paused" should mean -- no stepped core path of our own, no second copy of
 * the CPU's timing.
 *
 * That parking is also why register and memory inspection is only valid
 * while paused: the machine is quiescent then, so reading it from a UI thread
 * is safe without a lock XRoar does not have. Memory is read with the
 * machine's side-effect-free peek_byte, never a bus cycle.
 *
 * The same hook is XRoar's own (bp_add/bp_remove), so ours chains to the
 * delegate it found and puts itself back whenever something replaces it.
 * Read, write and access breakpoints, the write "old" byte and the sound
 * history come from the machine's bus tap (cocoxroar.h), which runs inside a
 * CPU cycle: it only notes a pending stop, and the stop itself happens at the
 * next instruction boundary, in the hook.
 *
 * Lock order: d->mtx; the trace and sound rings have locks of their own that
 * are never held while taking d->mtx. The bus tap takes d->mtx only when an
 * access matches a breakpoint.
 *
 * See cocodebug.h for the contract, and COMPLIANCE.md for why the engine
 * builds on upstream's hooks rather than reimplementing them.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cocoxroar.h"
#include "cocovidcap.h"
#include "mc6809/hd6309.h"

#include "coco_host.h"
#include "cocodebug.h"
#include "compat.h"
#include "csrcinfo.h"
#include "debugger_internal.h"
#include "dwtrace.h"
#include "expr.h"
#include "m6809dasm.h"
#include "session_internal.h"
#include "symtab.h"

/* Bound on instructions run during a C-line step before giving up. Covers
 * stepping onto a line whose statement calls into code with no debug info
 * (a ROM routine, a hand-written .s helper) and never returns to a mapped
 * address on its own -- generous enough to survive a real printf() many
 * times over, but not so large that a genuine runaway hangs the UI. */
#define SRC_STEP_BUDGET 200000

#define MAX_BP     COCODEBUG_MAX_BREAKPOINTS
#define TRACE_N    65536
#define SOUND_N    4096
#define SPACE_CPU_SIZE 0x10000u

struct cocodebug {
    cocosession *s;

    pthread_mutex_t mtx;
    pthread_cond_t cv;

    atomic_int engaged;
    int paused;          /* the emulator thread is parked in the hook */
    int stop_all;        /* the session is going away; never park again */
    /* Read without the lock by the hook's fast path (a stale value costs one
     * instruction), written under it. */
    volatile int pause_request;   /* stop at the next instruction boundary */
    volatile int step_remaining;  /* >0 while stepping: instructions still to run */
    volatile long transient_bp;   /* one-shot breakpoint for step-over/out, or -1 */

    /* C-line stepping: 0 off, 1 step-into, 2 step-over. Checked in the hook
     * alongside step_remaining/transient_bp above, and mutually exclusive
     * with them -- entering one clears the others. */
    volatile int src_step;
    /* Step out: run until a return-class instruction has executed and S is
     * above out_s (S when the command was given). */
    volatile int out_active;
    int      out_pending;      /* the instruction just run was return-class */
    uint16_t out_s;
    int      src_step_file, src_step_line;
    uint16_t src_step_s;       /* S when the step began, for step-over's
                                 * call-depth check. */
    long     src_step_budget;  /* counts down; 0 gives up and stops anyway. */

    /* The breakpoint table, and what the hook and the bus tap read of it. */
    cocodebug_breakpoint bp[MAX_BP];
    int nbp, next_id;
    uint8_t *map_exec;         /* 64K: nonzero where an enabled exec bp covers */
    uint8_t *map_mem;          /* 64K: COCODEBUG_BP_READ / _WRITE bits */
    int nphys;                 /* enabled PHYS breakpoints */
    atomic_int break_pending;  /* the tap saw a hit; the hook stops */
    char break_reason[160];
    int  break_has_pc;         /* append " at $PC" to it */
    int  break_hit_id;

    /* The access a condition sees as value / addr / old. */
    long acc_value, acc_addr, acc_old;
    int  in_access;

    cocodebug_regs regs;
    int regs_valid;
    uint64_t stop_serial;
    char reason[160];
    int  reason_addr;
    int  stop_pc;
    atomic_uint generation;

    /* The CPU our hook is installed on, and the delegate that was there. The
     * machine is rebuilt on a CoCo 2 / CoCo 3 switch and the hook goes with
     * it, so this is re-checked every frame rather than installed once. */
    struct MC6809 *hooked_cpu;
    DELEGATE_T0(void) prev_hook;
    int hooked_6309;

    /* What the frame check learned about the machine (emulator thread
     * writes, any thread reads). */
    atomic_int machine_known;
    atomic_int is_coco3;
    atomic_uint phys_size;
    atomic_int fast_rate;      /* SAM R1: the CPU runs at 1.79 MHz */

    /* Where the beam is and how many CPU cycles have gone by, kept in the
     * hook and the video tap (emulator thread). */
    uint32_t last_tick;
    uint64_t tick_rem;
    uint64_t cycles;
    uint32_t line_tick;
    int      line_no;
    uint32_t frame_no;
    int      video_subscribed;
    atomic_int tap_wanted;     /* a bus-tap consumer exists: install the tap */
    atomic_int want_beam;      /* subscribe to the video capture for the beam */
    atomic_int sound_watch;    /* record the DAC / single-bit writes */

    /* The instruction trace: its own lock, so a window reading it never
     * waits for a frame; the ring is allocated on the first enable. */
    atomic_int trace_on;
    pthread_mutex_t trace_ring_mtx;
    cocodebug_trace_entry *ring;
    int ring_head, ring_count;

    /* Writes to the PIA1 DAC and single-bit registers. */
    pthread_mutex_t sound_mtx;
    cocodebug_sound_write *sound_ring;
    int sound_head, sound_count;

    /* The CPU memory map of the last stop. */
    struct machine_memory_map map;
    uint64_t map_serial;
    int map_valid;

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

static cocodebug_video_text_fn g_video_text;

static void instruction_hook(void *sptr);
static void sync_taps(struct cocodebug *d);
static uint8_t dasm_read(uint16_t addr, void *user);

static void bump(struct cocodebug *d)
{
    atomic_fetch_add(&d->generation, 1);
}

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
    d->regs.is6309 = d->hooked_6309;
    if (d->hooked_6309) {
        struct HD6309 *hcpu = (struct HD6309 *)cpu;
        d->regs.w = hcpu->reg_w;
        d->regs.e = HD6309_REG_E(hcpu);
        d->regs.f = HD6309_REG_F(hcpu);
        d->regs.v = hcpu->reg_v;
        d->regs.md = hcpu->reg_md;
    } else {
        d->regs.w = d->regs.v = 0;
        d->regs.e = d->regs.f = d->regs.md = 0;
    }
    d->regs_valid = 1;
}

/* Registers as the hook or the tap sees them: the live CPU. */
static void live_regs(struct cocodebug *d, cocodebug_regs *out)
{
    struct MC6809 *cpu = d->hooked_cpu;
    memset(out, 0, sizeof(*out));
    if (!cpu)
        return;
    out->pc = cpu->reg_pc;
    out->x = cpu->reg_x;
    out->y = cpu->reg_y;
    out->u = cpu->reg_u;
    out->s = cpu->reg_s;
    out->a = MC6809_REG_A(cpu);
    out->b = MC6809_REG_B(cpu);
    out->dp = cpu->reg_dp;
    out->cc = cpu->reg_cc;
    out->is6309 = d->hooked_6309;
    if (d->hooked_6309) {
        struct HD6309 *hcpu = (struct HD6309 *)cpu;
        out->w = hcpu->reg_w;
        out->e = HD6309_REG_E(hcpu);
        out->f = HD6309_REG_F(hcpu);
        out->v = hcpu->reg_v;
        out->md = hcpu->reg_md;
    }
}

static uint8_t machine_peek(uint16_t addr)
{
    struct machine *mach = xroar.machine;
    return mach && mach->peek_byte ? mach->peek_byte(mach, addr) : 0xff;
}

/* ---- expressions ---------------------------------------------------------
 * A context is one evaluation: the registers it sees (the live CPU from the
 * hook or the tap, the stop's snapshot from a UI thread), the access a
 * breakpoint condition describes, and whether memory may be read. */
typedef struct {
    struct cocodebug *d;
    cocodebug_regs regs;
    int have_regs;
    int can_peek;
    int in_access;
    long value, addr, old;
} evalctx;

const char *const cocodebug_reg_names[] = {
    "pc", "a", "b", "d", "dp", "x", "y", "u", "s", "cc",
    "e", "f", "w", "v", "md",
    "cf", "vf", "zf", "nf", "if", "hf", "ff", "ef",
    "vpos", "hpos", "frame", "cycles", "value", "addr", "old", NULL
};

static uint8_t eval_read8(void *user, uint16_t addr)
{
    evalctx *ctx = user;
    return ctx->can_peek ? machine_peek(addr) : 0;
}

/* The raster line from the chip itself, when nothing subscribed to the video
 * capture yet. Parked or on the emulator thread. */
static unsigned chip_scanline(void)
{
    struct machine *mach = xroar.machine;
    struct TCC1014 *gime;
    struct MC6847 *vdg;

    if (!mach)
        return 0;
    gime = (struct TCC1014 *)part_component_by_id_is_a(&mach->part, "GIME", "TCC1014");
    if (gime) {
        struct tcc1014_debug_state gs;
        tcc1014_get_debug_state(gime, &gs);
        return gs.scanline;
    }
    vdg = (struct MC6847 *)part_component_by_id_is_a(&mach->part, "VDG", "MC6847");
    if (vdg) {
        struct mc6847_debug_state vs;
        mc6847_get_debug_state(vdg, &vs);
        return vs.scanline;
    }
    return 0;
}

static int eval_name(void *user, const char *name, long *out)
{
    evalctx *ctx = user;
    struct cocodebug *d = ctx->d;
    const cocodebug_regs *r = &ctx->regs;
    uint16_t addr;
    char low[16];
    size_t pos;

    for (pos = 0; name[pos] && pos < sizeof low - 1; pos++)
        low[pos] = (char)tolower((unsigned char)name[pos]);
    low[pos] = '\0';
    if (name[pos])
        low[0] = '\0';   /* too long for a register name */

    if (!strcmp(low, "pc"))   { *out = r->pc; return 1; }
    if (!strcmp(low, "a"))    { *out = r->a; return 1; }
    if (!strcmp(low, "b"))    { *out = r->b; return 1; }
    if (!strcmp(low, "d"))    { *out = (r->a << 8) | r->b; return 1; }
    if (!strcmp(low, "dp"))   { *out = r->dp; return 1; }
    if (!strcmp(low, "x"))    { *out = r->x; return 1; }
    if (!strcmp(low, "y"))    { *out = r->y; return 1; }
    if (!strcmp(low, "u"))    { *out = r->u; return 1; }
    if (!strcmp(low, "s"))    { *out = r->s; return 1; }
    if (!strcmp(low, "cc"))   { *out = r->cc; return 1; }
    if (r->is6309) {
        if (!strcmp(low, "e"))  { *out = r->e; return 1; }
        if (!strcmp(low, "f"))  { *out = r->f; return 1; }
        if (!strcmp(low, "w"))  { *out = r->w; return 1; }
        if (!strcmp(low, "v"))  { *out = r->v; return 1; }
        if (!strcmp(low, "md")) { *out = r->md; return 1; }
    }
    if (!strcmp(low, "cf")) { *out = (r->cc & COCODEBUG_CC_C) != 0; return 1; }
    if (!strcmp(low, "vf")) { *out = (r->cc & COCODEBUG_CC_V) != 0; return 1; }
    if (!strcmp(low, "zf")) { *out = (r->cc & COCODEBUG_CC_Z) != 0; return 1; }
    if (!strcmp(low, "nf")) { *out = (r->cc & COCODEBUG_CC_N) != 0; return 1; }
    if (!strcmp(low, "if")) { *out = (r->cc & COCODEBUG_CC_I) != 0; return 1; }
    if (!strcmp(low, "hf")) { *out = (r->cc & COCODEBUG_CC_H) != 0; return 1; }
    if (!strcmp(low, "ff")) { *out = (r->cc & COCODEBUG_CC_F) != 0; return 1; }
    if (!strcmp(low, "ef")) { *out = (r->cc & COCODEBUG_CC_E) != 0; return 1; }
    if (!strcmp(low, "vpos") || !strcmp(low, "hpos") || !strcmp(low, "frame")) {
        /* the beam needs the video capture's subscription: ask for it */
        atomic_store(&d->want_beam, 1);
    }
    if (!strcmp(low, "vpos"))   { *out = d->video_subscribed ? d->line_no : (long)chip_scanline(); return 1; }
    if (!strcmp(low, "hpos"))   {
        long dots = (long)((uint32_t)(event_current_tick - d->line_tick) / 4u);
        *out = dots > 227 ? 227 : dots;
        return 1;
    }
    if (!strcmp(low, "frame"))  { *out = (long)d->frame_no; return 1; }
    if (!strcmp(low, "cycles")) { *out = (long)d->cycles; return 1; }
    if (ctx->in_access) {
        if (!strcmp(low, "value")) { *out = ctx->value; return 1; }
        if (!strcmp(low, "addr"))  { *out = ctx->addr; return 1; }
        if (!strcmp(low, "old"))   { *out = ctx->old; return 1; }
    }
    if (cocosymtab_lookup_name(d->symtab, name, &addr)) {
        *out = addr;
        return 1;
    }
    return 0;
}

/* From a UI thread: registers are the stop's snapshot. */
static void ui_context(struct cocodebug *d, evalctx *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->d = d;
    pthread_mutex_lock(&d->mtx);
    if (d->paused && d->regs_valid) {
        ctx->regs = d->regs;
        ctx->have_regs = 1;
        ctx->can_peek = 1;
    }
    pthread_mutex_unlock(&d->mtx);
}

int cocodebug_eval(cocodebug *d, const char *text, long *out, char *err, int errsz)
{
    evalctx ctx;
    expr_ctx ex;

    if (!d)
        return -1;
    ui_context(d, &ctx);
    ex.name = eval_name;
    ex.read8 = eval_read8;
    ex.user = &ctx;
    return expr_eval(text, &ex, out, err, errsz);
}

int cocodebug_eval_plain(cocodebug *d, const char *text, long *out, char *err,
                         int errsz)
{
    return cocodebug_eval(d, text, out, err, errsz);
}

cocosymtab *cocodebug_symtab(cocodebug *d)
{
    return d->symtab;
}

void cocodebug_hook_state(cocodebug *d, int *ours, int *chained)
{
    struct MC6809 *cpu = current_cpu();

    if (ours)
        *ours = cpu && cpu->debug_cpu.instruction_hook.func == instruction_hook;
    if (chained)
        *chained = d && d->prev_hook.func != NULL;
}

int cocodebug_cpu_is_6309(cocodebug *d)
{
    return d ? d->hooked_6309 : 0;
}

/* Caller holds d->mtx, on the emulator thread (hook or tap). */
static int cond_true(struct cocodebug *d, const cocodebug_breakpoint *bp)
{
    evalctx ctx;
    expr_ctx ex;
    long v = 0;
    char err[64];

    if (!bp->condition[0])
        return 1;
    memset(&ctx, 0, sizeof(ctx));
    ctx.d = d;
    live_regs(d, &ctx.regs);
    ctx.have_regs = 1;
    ctx.can_peek = 1;
    ctx.in_access = d->in_access;
    ctx.value = d->acc_value;
    ctx.addr = d->acc_addr;
    ctx.old = d->acc_old;
    ex.name = eval_name;
    ex.read8 = eval_read8;
    ex.user = &ctx;
    if (expr_eval(bp->condition, &ex, &v, err, sizeof(err)) != 0)
        return 1;   /* a condition that no longer evaluates stops, rather than hides */
    return v != 0;
}

/* Rebuild what the hook and the tap read from the table. Caller holds mtx. */
static void rebuild_locked(struct cocodebug *d)
{
    int pos;
    uint32_t addr;

    int wants_tap = atomic_load(&d->sound_watch);

    memset(d->map_exec, 0, SPACE_CPU_SIZE);
    memset(d->map_mem, 0, SPACE_CPU_SIZE);
    d->nphys = 0;
    for (pos = 0; pos < d->nbp; pos++) {
        const cocodebug_breakpoint *bp = &d->bp[pos];
        if (!bp->enabled)
            continue;
        if (bp->type & COCODEBUG_BP_ACCESS)
            wants_tap = 1;
        if (bp->type & COCODEBUG_BP_PHYS) {
            d->nphys++;
            continue;
        }
        for (addr = bp->start; addr <= bp->end && addr < SPACE_CPU_SIZE; addr++) {
            if (bp->type & COCODEBUG_BP_EXEC)
                d->map_exec[addr] = 1;
            d->map_mem[addr] |= (uint8_t)(bp->type & COCODEBUG_BP_ACCESS);
        }
    }
    atomic_store(&d->tap_wanted, wants_tap);
}

/* ---- parking --------------------------------------------------------------
 * Caller holds d->mtx on the emulator thread, inside the hook. The reason,
 * the PC, the serial and the paused flag are set together, so a poller that
 * sees the machine paused sees this stop's reason. */
static void park_locked(struct cocodebug *d, struct MC6809 *cpu, uint16_t pc,
                        const char *reason, int append_pc)
{
    snapshot_regs(d, cpu);
    if (append_pc)
        snprintf(d->reason, sizeof(d->reason), "%s at $%04X", reason, pc);
    else
        snprintf(d->reason, sizeof(d->reason), "%s", reason);
    d->reason_addr = pc;
    d->stop_pc = pc;
    d->stop_serial++;
    d->paused = 1;
    bump(d);
    /* Park here. The session's stop path calls cocodebug_resume_for_stop, so
     * a shutdown cannot deadlock behind a user who walked away. */
    while (d->paused && !d->stop_all)
        pthread_cond_wait(&d->cv, &d->mtx);
    d->paused = 0;
    d->regs_valid = 0;
    bump(d);
    sync_taps(d);
}

/* ---- the trace ring --------------------------------------------------------- */

static void trace_record(struct cocodebug *d, struct MC6809 *cpu, uint16_t pc)
{
    cocodebug_trace_entry ent;
    cocodebug_regs live;
    int pos;
    long dots;

    memset(&ent, 0, sizeof(ent));
    live_regs(d, &live);
    ent.pc = pc;
    for (pos = 0; pos < 6; pos++)
        ent.bytes[pos] = machine_peek((uint16_t)(pc + pos));
    ent.a = live.a;
    ent.b = live.b;
    ent.dp = live.dp;
    ent.cc = live.cc;
    ent.x = live.x;
    ent.y = live.y;
    ent.u = live.u;
    ent.s = live.s;
    ent.is6309 = (uint8_t)live.is6309;
    ent.w = live.w;
    ent.v = live.v;
    ent.md = live.md;
    ent.cycles = d->cycles;
    ent.line = (int16_t)d->line_no;
    dots = (long)((uint32_t)(event_current_tick - d->line_tick) / 4u);
    ent.hpos = (int16_t)(dots > 227 ? 227 : dots);
    ent.frame = d->frame_no;
    (void)cpu;

    pthread_mutex_lock(&d->trace_ring_mtx);
    if (d->ring) {
        d->ring[d->ring_head] = ent;
        d->ring_head = (d->ring_head + 1) % TRACE_N;
        if (d->ring_count < TRACE_N)
            d->ring_count++;
    }
    pthread_mutex_unlock(&d->trace_ring_mtx);
}

/* CPU cycles: the event tick delta, over 16 (0.89 MHz) or 8 (1.79 MHz). */
static void count_cycles(struct cocodebug *d)
{
    uint32_t now = event_current_tick;
    uint32_t delta = now - d->last_tick;
    unsigned divisor = atomic_load(&d->fast_rate) ? 8u : 16u;

    d->last_tick = now;
    if (delta > 0x10000000u)
        delta = 0;   /* a new machine's clock restarted */
    d->tick_rem += delta;
    d->cycles += d->tick_rem / divisor;
    d->tick_rem %= divisor;
}

/* ---- the hook -----------------------------------------------------------
 * Emulator thread, called before every instruction fetch while engaged. */
static void instruction_hook(void *sptr)
{
    struct cocodebug *d = sptr;
    struct MC6809 *cpu = d->hooked_cpu;
    uint16_t pc;
    int stop = 0, append_pc = 1;
    const char *reason = "stopped";
    char text[160];

    if (!cpu || d->stop_all || !atomic_load(&d->engaged))
        goto chain;
    pc = cpu->reg_pc;

    count_cycles(d);
    if (atomic_load_explicit(&d->trace_on, memory_order_relaxed))
        trace_record(d, cpu, pc);

    if (atomic_load(&d->break_pending)) {
        pthread_mutex_lock(&d->mtx);
        atomic_store(&d->break_pending, 0);
        d->pause_request = 0;
        d->step_remaining = 0;
        d->src_step = 0;
        d->out_active = 0;
        d->transient_bp = -1;
        snprintf(text, sizeof(text), "%s", d->break_reason);
        park_locked(d, cpu, pc, text, d->break_has_pc);
        pthread_mutex_unlock(&d->mtx);
        goto chain;
    }
    if (!(d->pause_request || d->step_remaining > 0 || d->src_step || d->out_active ||
          d->transient_bp >= 0 || d->map_exec[pc]))
        goto chain;

    pthread_mutex_lock(&d->mtx);

    if (d->pause_request) {
        d->pause_request = 0;
        stop = 1;
        reason = "stopped";
    } else if (d->step_remaining > 0) {
        /* The step counter is decremented on the way IN, so a step of one
         * runs the instruction we are about to fetch and stops before the
         * next -- which is what a user watching the PC expects. */
        if (--d->step_remaining == 0) {
            stop = 1;
            reason = "step";
        }
    }
    if (d->out_active) {
        if (d->out_pending) {
            d->out_pending = 0;
            if (cpu->reg_s > d->out_s) {
                d->out_active = 0;
                stop = 1;
                reason = "returned";
            }
        }
        if (d->out_active) {
            coco_dasm_line next;
            coco_dasm_one_cpu(pc, dasm_read, NULL,
                              d->hooked_6309 ? COCO_DASM_CPU_HD6309 : COCO_DASM_CPU_MC6809,
                              &next);
            d->out_pending = next.flow == COCO_FLOW_RETURN || next.flow == COCO_FLOW_PULL_PC ||
                             next.flow == COCO_FLOW_TRANSFER_PC;
        }
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
            d->out_active = 0;
            stop = 1;
            reason = "line step";
        }
    }
    if (d->src_step && --d->src_step_budget <= 0) {
        /* Ran on for SRC_STEP_BUDGET instructions without reaching a new
         * mapped statement -- most likely a call into code with no line
         * info that never returns to a mapped address on its own. Stop
         * where we are rather than hang. */
        d->src_step = 0;
        d->out_active = 0;
        stop = 1;
        reason = "line step gave up";
    }
    if (d->transient_bp >= 0 && (uint16_t)d->transient_bp == pc) {
        /* A one-shot breakpoint is spent whether or not it is what stopped
         * us, so step-over cannot leave one behind to fire later. */
        d->transient_bp = -1;
        d->step_remaining = 0;
        d->src_step = 0;
        d->out_active = 0;
        stop = 1;
        reason = "returned";
    }
    if (d->map_exec[pc]) {
        int pos;
        d->acc_addr = pc;
        d->acc_value = machine_peek(pc);
        d->acc_old = d->acc_value;
        d->in_access = 1;
        for (pos = 0; pos < d->nbp; pos++) {
            cocodebug_breakpoint *bp = &d->bp[pos];
            if (bp->enabled && (bp->type & COCODEBUG_BP_EXEC) &&
                !(bp->type & COCODEBUG_BP_PHYS) && pc >= bp->start &&
                pc <= bp->end && cond_true(d, bp)) {
                bp->hits++;
                d->step_remaining = 0;
                d->src_step = 0;
                d->out_active = 0;
                d->transient_bp = -1;
                stop = 1;
                append_pc = 0;
                snprintf(text, sizeof(text), "breakpoint: exec $%04X", pc);
                reason = text;
                break;
            }
        }
        d->in_access = 0;
    }

    if (stop)
        park_locked(d, cpu, pc, reason, append_pc);
    pthread_mutex_unlock(&d->mtx);

chain:
    DELEGATE_SAFE_CALL(d->prev_hook);
}

/* Install our hook on `cpu`, remembering what was there. Emulator thread. */
static void relink_hook(struct cocodebug *d, struct MC6809 *cpu)
{
    if (cpu->debug_cpu.instruction_hook.func == instruction_hook)
        return;
    d->prev_hook = cpu->debug_cpu.instruction_hook;
    cpu->debug_cpu.instruction_hook = DELEGATE_AS0(void, instruction_hook, d);
}

/* ---- the bus tap ------------------------------------------------------------ */

static void sound_note(struct cocodebug *d, const struct machine_bus_cycle *cyc)
{
    cocodebug_sound_write ent;
    unsigned port = cyc->address & 3u;

    if (port != 0 && port != 2)
        return;
    ent.tick = event_current_tick;
    ent.port = (uint8_t)port;
    ent.value = cyc->value;
    ent.dac = (uint8_t)(port == 0 ? cyc->value >> 2 : 0);
    pthread_mutex_lock(&d->sound_mtx);
    if (d->sound_ring) {
        if (port == 2 && d->sound_count) {
            /* carry the level over from the last DAC write */
            int last = (d->sound_head + SOUND_N - 1) % SOUND_N;
            ent.dac = d->sound_ring[last].dac;
        }
        d->sound_ring[d->sound_head] = ent;
        d->sound_head = (d->sound_head + 1) % SOUND_N;
        if (d->sound_count < SOUND_N)
            d->sound_count++;
    }
    pthread_mutex_unlock(&d->sound_mtx);
}

static void note_hit(struct cocodebug *d, cocodebug_breakpoint *bp,
                     const struct machine_bus_cycle *cyc, int phys, int write)
{
    long old = cyc->has_old ? cyc->old_value : cyc->value;
    const char *where = phys ? "phys " : "";
    unsigned shown = phys ? (unsigned)cyc->physical : cyc->address;
    const int digits = phys ? 6 : 4;

    bp->hits++;
    d->break_hit_id = bp->id;
    if (write)
        snprintf(d->break_reason, sizeof(d->break_reason),
                 "wrote %s$%0*X = $%02X (was $%02lX)", where, digits, shown,
                 cyc->value, old & 0xff);
    else
        snprintf(d->break_reason, sizeof(d->break_reason), "read %s$%0*X = $%02X",
                 where, digits, shown, cyc->value);
    d->break_has_pc = 1;
    atomic_store(&d->break_pending, 1);
}

static void bus_tap(void *ctx, const struct machine_bus_cycle *cyc)
{
    struct cocodebug *d = ctx;
    uint16_t addr = cyc->address;
    int want, pos, write = cyc->is_write;

    if (!atomic_load_explicit(&d->engaged, memory_order_relaxed))
        return;
    /* Something else (a reset, XRoar's bp_add/bp_remove) took the hook: put
     * ours back at once rather than at the next frame. */
    if (d->hooked_cpu && d->hooked_cpu->debug_cpu.instruction_hook.func != instruction_hook)
        relink_hook(d, d->hooked_cpu);

    if (write) {
        if (addr >= 0xFF20 && addr <= 0xFF3F && atomic_load_explicit(&d->sound_watch, memory_order_relaxed))
            sound_note(d, cyc);
        else if (addr == 0xFFD8)
            atomic_store(&d->fast_rate, 0);
        else if (addr == 0xFFD9)
            atomic_store(&d->fast_rate, 1);
    } else if (addr == 0xFFFF) {
        return;   /* the 6809's dummy cycle */
    }

    want = write ? COCODEBUG_BP_WRITE : COCODEBUG_BP_READ;
    if (!(d->map_mem[addr] & want) &&
        !(d->nphys && cyc->physical >= 0 && atomic_load(&d->is_coco3)))
        return;
    if (atomic_load(&d->break_pending))
        return;

    pthread_mutex_lock(&d->mtx);
    d->acc_value = cyc->value;
    d->acc_addr = addr;
    d->acc_old = cyc->has_old ? cyc->old_value : cyc->value;
    d->in_access = 1;
    for (pos = 0; pos < d->nbp; pos++) {
        cocodebug_breakpoint *bp = &d->bp[pos];
        int phys = (bp->type & COCODEBUG_BP_PHYS) != 0;

        if (!bp->enabled || !(bp->type & want))
            continue;
        if (phys) {
            if (cyc->physical < 0 || !atomic_load(&d->is_coco3) ||
                (uint32_t)cyc->physical < bp->start || (uint32_t)cyc->physical > bp->end)
                continue;
        } else if (addr < bp->start || addr > bp->end) {
            continue;
        }
        if (!cond_true(d, bp))
            continue;
        note_hit(d, bp, cyc, phys, write);
        break;
    }
    d->in_access = 0;
    pthread_mutex_unlock(&d->mtx);
}

/* ---- the video tap: where the beam is ------------------------------------------ */

static void video_hsync(void *ctx, _Bool level, unsigned scanline, uint32_t tick)
{
    struct cocodebug *d = ctx;
    if (!level) {
        d->line_tick = tick;
        d->line_no = (int)scanline;
    }
}

static void video_fsync(void *ctx, _Bool level, unsigned scanline, uint32_t tick)
{
    struct cocodebug *d = ctx;
    (void)scanline;
    (void)tick;
    if (level)
        d->frame_no++;
}

/* The video capture (src/vidcap.c) owns the machine's one video tap slot and
 * passes every event to its subscribers; we subscribe for the beam position. */
static void video_line(void *ctx, const struct machine_video_line *line)
{
    (void)ctx;
    (void)line;
}

/* Put the taps on the machine that are wanted, and take ours off when they
 * are not: a debugger that needs no access breakpoint and no sound history
 * leaves the bus tap slot alone, and one that never reads the beam does not
 * subscribe to the video capture. Emulator thread (the frame, or the hook
 * waking from a park, so a breakpoint added while stopped is live at the
 * first instruction after the resume). */
static void sync_taps(struct cocodebug *d)
{
    struct machine *mach = xroar.machine;

    if (mach) {
        if (atomic_load(&d->tap_wanted)) {
            if (!mach->bus_tap)
                machine_debug_set_bus_tap(mach, bus_tap, d);
        } else if (mach->bus_tap == bus_tap) {
            machine_debug_set_bus_tap(mach, NULL, NULL);
        }
    }
    if (atomic_load(&d->want_beam) && !d->video_subscribed) {
        struct machine_video_tap tap = { d, video_line, video_hsync, video_fsync };
        d->video_subscribed = cocovid_host_tap_add(&tap) != 0;
    }
}

/* ---- lifecycle ---------------------------------------------------------- */

cocodebug *cocodebug_create(cocosession *s)
{
    struct cocodebug *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->map_exec = calloc(SPACE_CPU_SIZE, 1);
    d->map_mem = calloc(SPACE_CPU_SIZE, 1);
    if (!d->map_exec || !d->map_mem) {
        free(d->map_exec);
        free(d->map_mem);
        free(d);
        return NULL;
    }
    d->sound_ring = calloc(SOUND_N, sizeof(*d->sound_ring));
    if (!d->sound_ring) {
        free(d->map_exec);
        free(d->map_mem);
        free(d);
        return NULL;
    }
    d->s = s;
    d->transient_bp = -1;
    d->next_id = 1;
    pthread_mutex_init(&d->mtx, NULL);
    coco_cond_init_monotonic(&d->cv);
    pthread_mutex_init(&d->trace_ring_mtx, NULL);
    pthread_mutex_init(&d->sound_mtx, NULL);
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
    if (d->video_subscribed) {
        struct machine_video_tap tap = { d, video_line, video_hsync, video_fsync };
        cocovid_host_tap_remove(&tap);
    }
    cocosymtab_destroy(d->symtab);
    cocosrcinfo_destroy(d->srcinfo);
    dwtrace_destroy(d->trace);
    free(d->ring);
    free(d->sound_ring);
    free(d->map_exec);
    free(d->map_mem);
    pthread_mutex_destroy(&d->trace_mtx);
    pthread_mutex_destroy(&d->trace_ring_mtx);
    pthread_mutex_destroy(&d->sound_mtx);
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

/* A session is starting: the debugger object outlives the previous run, so
 * everything the stop path or the old machine left behind is cleared. UI
 * thread, before the emulator thread starts. */
static void session_begin(cocodebug *d)
{
    if (!d)
        return;
    pthread_mutex_lock(&d->mtx);
    d->stop_all = 0;
    d->paused = 0;
    d->regs_valid = 0;
    d->pause_request = 0;
    d->step_remaining = 0;
    d->transient_bp = -1;
    d->src_step = 0;
    d->out_active = 0;
    atomic_store(&d->break_pending, 0);
    d->hooked_cpu = NULL;
    memset(&d->prev_hook, 0, sizeof(d->prev_hook));
    d->hooked_6309 = 0;
    if (d->video_subscribed) {
        /* the emulator thread is not running: safe to drop, and re-added by
         * the first frame */
        struct machine_video_tap tap = { d, video_line, video_hsync, video_fsync };
        cocovid_host_tap_remove(&tap);
        d->video_subscribed = 0;
    }
    d->map_valid = 0;
    atomic_store(&d->machine_known, 0);
    atomic_store(&d->is_coco3, 0);
    atomic_store(&d->phys_size, 0);
    atomic_store(&d->fast_rate, 0);
    d->cycles = d->tick_rem = 0;
    d->frame_no = 0;
    d->line_no = 0;
    d->line_tick = 0;
    pthread_mutex_unlock(&d->mtx);
    bump(d);
}

/* ---- the emulator-thread frame ------------------------------------------
 * The session calls this instead of cocohost_core_run_frame while engaged. */
int cocodebug_session_frame(cocosession *s)
{
    struct cocodebug *d = s->debugger;
    struct MC6809 *cpu;
    struct machine *mach;

    if (!d) {
        cocohost_core_run_frame();
        return 1;
    }

    /* Re-attach every frame rather than once: switching between a CoCo 2 and
     * a CoCo 3 rebuilds the machine, and the new CPU has no hook on it; a
     * reset or XRoar's own bp_add/bp_remove can replace the hook on the same
     * CPU. One pointer compare per frame buys not having to notice. */
    mach = xroar.machine;
    cpu = current_cpu();
    if (cpu != d->hooked_cpu) {
        d->hooked_cpu = cpu;
        memset(&d->prev_hook, 0, sizeof(d->prev_hook));
        d->hooked_6309 = cpu && part_component_by_id_is_a(&mach->part, "CPU", "HD6309");
        d->last_tick = event_current_tick;
    }
    if (cpu)
        relink_hook(d, cpu);

    if (mach) {
        struct TCC1014 *gime =
            (struct TCC1014 *)part_component_by_id_is_a(&mach->part, "GIME", "TCC1014");

        sync_taps(d);
        atomic_store(&d->is_coco3, gime != NULL);
        atomic_store(&d->phys_size, gime ? machine_debug_ram_size(mach) : 0);
        atomic_store(&d->machine_known, 1);
        if (gime) {
            struct tcc1014_debug_state gs;
            tcc1014_get_debug_state(gime, &gs);
            atomic_store(&d->fast_rate, gs.R1 ? 1 : 0);
        } else {
            struct MC6883 *sam =
                (struct MC6883 *)part_component_by_id_is_a(&mach->part, "SAM", "SN74LS783");
            struct mc6883_debug_state ss;
            if (sam && mc6883_get_debug_state(sam, &ss))
                atomic_store(&d->fast_rate, (ss.R & 2u) ? 1 : 0);
        }
    }

    cocohost_core_run_frame();
    return 1;
}

/* ---- engage / pause / step ---------------------------------------------- */

void cocodebug_set_engaged(cocodebug *d, int engaged)
{
    struct MC6809 *cpu;

    if (!d)
        return;
    engaged = engaged ? 1 : 0;
    if (!engaged) {
        cocodebug_resume(d);
        atomic_store(&d->break_pending, 0);
    }

    atomic_store(&d->engaged, engaged);

    /* The session's loop reads this to decide which frame path to run. It is
     * set after the engine's own state so the loop cannot arrive first. */
    d->s->dbg_engaged = engaged;

    cpu = d->hooked_cpu;
    if (!engaged && cpu) {
        /* Leave the machine as we found it: the hook is per-instruction, and
         * a released debugger should cost nothing at all. Safe to clear from
         * here because the loop has already stopped calling us. Whatever was
         * on the hook before ours goes back. The beam subscription stays (it
         * goes with the debugger): it only counts lines. */
        if (cpu->debug_cpu.instruction_hook.func == instruction_hook)
            cpu->debug_cpu.instruction_hook = d->prev_hook;
        d->hooked_cpu = NULL;
        memset(&d->prev_hook, 0, sizeof(d->prev_hook));
        if (xroar.machine && xroar.machine->bus_tap == bus_tap)
            machine_debug_set_bus_tap(xroar.machine, NULL, NULL);
    }
    bump(d);
}

int cocodebug_is_engaged(const cocodebug *d)
{
    return d ? atomic_load(&((struct cocodebug *)d)->engaged) : 0;
}

void cocodebug_pause(cocodebug *d)
{
    if (!d)
        return;
    pthread_mutex_lock(&d->mtx);
    if (!d->paused) {
        d->pause_request = 1;
        d->step_remaining = 0;
        d->src_step = 0;
        d->out_active = 0;
    }
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
    d->out_active = 0;
    d->paused = 0;
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->mtx);
    bump(d);
}

int cocodebug_is_paused(const cocodebug *d)
{
    int paused;
    struct cocodebug *mut = (struct cocodebug *)d;

    if (!d)
        return 0;
    pthread_mutex_lock(&mut->mtx);
    paused = mut->paused;
    pthread_mutex_unlock(&mut->mtx);
    return paused;
}

void cocodebug_step(cocodebug *d)
{
    if (!d)
        return;
    pthread_mutex_lock(&d->mtx);
    d->step_remaining = 1;
    d->transient_bp = -1;
    d->src_step = 0;
    d->out_active = 0;
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
    d->out_active = 0;
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
    uint64_t serial;
    struct cocodebug *mut = (struct cocodebug *)d;

    if (!d)
        return 0;
    pthread_mutex_lock(&mut->mtx);
    serial = mut->stop_serial;
    pthread_mutex_unlock(&mut->mtx);
    return serial;
}

void cocodebug_stop_snapshot(cocodebug *d, cocodebug_stop *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->pc = out->reason_addr = -1;
    snprintf(out->reason, sizeof(out->reason), "running");
    if (!d)
        return;
    pthread_mutex_lock(&d->mtx);
    out->serial = d->stop_serial;
    if (d->paused) {
        out->paused = 1;
        out->pc = d->stop_pc;
        out->reason_addr = d->reason_addr;
        snprintf(out->reason, sizeof(out->reason), "%s", d->reason);
    }
    pthread_mutex_unlock(&d->mtx);
}

int cocodebug_stop_reason(cocodebug *d, char *dst, int dstsz, int *address)
{
    cocodebug_stop stop;
    int n;

    if (!dst || dstsz <= 0)
        return 0;
    cocodebug_stop_snapshot(d, &stop);
    n = snprintf(dst, (size_t)dstsz, "%s", stop.reason);
    if (address)
        *address = stop.reason_addr;
    return n < dstsz ? n : dstsz - 1;
}

unsigned cocodebug_generation(cocodebug *d)
{
    return d ? atomic_load(&d->generation) : 0;
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

int cocodebug_regs_set(cocodebug *d, int reg, unsigned value)
{
    struct MC6809 *cpu;
    struct HD6309 *hcpu;
    int ok = 1;

    if (!d || reg < 0 || reg >= COCODEBUG_REG_COUNT)
        return 0;
    pthread_mutex_lock(&d->mtx);
    cpu = d->hooked_cpu;
    if (!d->paused || !d->regs_valid || !cpu) {
        pthread_mutex_unlock(&d->mtx);
        return 0;
    }
    hcpu = (struct HD6309 *)cpu;
    switch (reg) {
    case COCODEBUG_REG_PC: cpu->reg_pc = (uint16_t)value; break;
    case COCODEBUG_REG_A:  MC6809_REG_A(cpu) = (uint8_t)value; break;
    case COCODEBUG_REG_B:  MC6809_REG_B(cpu) = (uint8_t)value; break;
    case COCODEBUG_REG_D:  cpu->reg_d = (uint16_t)value; break;
    case COCODEBUG_REG_DP: cpu->reg_dp = (uint8_t)value; break;
    case COCODEBUG_REG_X:  cpu->reg_x = (uint16_t)value; break;
    case COCODEBUG_REG_Y:  cpu->reg_y = (uint16_t)value; break;
    case COCODEBUG_REG_U:  cpu->reg_u = (uint16_t)value; break;
    case COCODEBUG_REG_S:  cpu->reg_s = (uint16_t)value; break;
    case COCODEBUG_REG_CC: cpu->reg_cc = (uint8_t)value; break;
    default:
        if (!d->hooked_6309) {
            ok = 0;
            break;
        }
        if (reg == COCODEBUG_REG_E)       HD6309_REG_E(hcpu) = (uint8_t)value;
        else if (reg == COCODEBUG_REG_F)  HD6309_REG_F(hcpu) = (uint8_t)value;
        else if (reg == COCODEBUG_REG_W)  hcpu->reg_w = (uint16_t)value;
        else if (reg == COCODEBUG_REG_V)  hcpu->reg_v = (uint16_t)value;
        else if (reg == COCODEBUG_REG_MD) hcpu->reg_md = (uint8_t)value;
        break;
    }
    if (ok) {
        snapshot_regs(d, cpu);
        d->stop_pc = d->regs.pc;
        d->map_valid = 0;
    }
    pthread_mutex_unlock(&d->mtx);
    if (ok)
        bump(d);
    return ok;
}

int cocodebug_flag_set(cocodebug *d, unsigned mask, int on)
{
    cocodebug_regs now;

    if (!cocodebug_regs_get(d, &now))
        return 0;
    return cocodebug_regs_set(d, COCODEBUG_REG_CC,
                              on ? (now.cc | mask) : (now.cc & ~mask & 0xffu));
}

/* ---- memory -------------------------------------------------------------- */

uint32_t cocodebug_space_size(cocodebug *d, int space)
{
    if (!d)
        return 0;
    return space == COCODEBUG_SPACE_PHYS ? atomic_load(&d->phys_size) : SPACE_CPU_SIZE;
}

/* The machine may be read: parked, with its peek hooks. */
static struct machine *parked_machine(struct cocodebug *d)
{
    struct machine *mach = xroar.machine;

    if (!cocodebug_is_paused(d) || !mach || !mach->peek_byte)
        return NULL;
    return mach;
}

int cocodebug_mem_read(cocodebug *d, int space, uint32_t addr, uint8_t *dst, int n)
{
    struct machine *mach;
    int pos;

    if (!d || !dst || n <= 0)
        return 0;
    mach = parked_machine(d);
    if (!mach || (space == COCODEBUG_SPACE_PHYS && !atomic_load(&d->phys_size)))
        return 0;
    if (space == COCODEBUG_SPACE_PHYS) {
        for (pos = 0; pos < n; pos++) {
            int value = machine_debug_ram_peek(mach, addr + (uint32_t)pos);
            if (value < 0)
                break;
            dst[pos] = (uint8_t)value;
        }
        return pos;
    }
    for (pos = 0; pos < n && addr + (uint32_t)pos < SPACE_CPU_SIZE; pos++)
        dst[pos] = mach->peek_byte(mach, (unsigned)(addr + (uint32_t)pos));
    return pos;
}

int cocodebug_mem_write(cocodebug *d, int space, uint32_t addr, const uint8_t *src, int n)
{
    struct machine *mach;
    int pos, stored = 0;

    if (!d || !src || n <= 0)
        return 0;
    mach = parked_machine(d);
    if (!mach || (space == COCODEBUG_SPACE_PHYS && !atomic_load(&d->phys_size)))
        return 0;
    if (space == COCODEBUG_SPACE_PHYS) {
        for (pos = 0; pos < n; pos++)
            if (machine_debug_ram_poke(mach, addr + (uint32_t)pos, src[pos]))
                stored++;
    } else {
        for (pos = 0; pos < n && addr + (uint32_t)pos < SPACE_CPU_SIZE; pos++)
            if (mach->poke_byte(mach, (unsigned)(addr + (uint32_t)pos), src[pos]))
                stored++;
    }
    if (stored)
        bump(d);
    return stored;
}

int cocodebug_read(cocodebug *d, uint16_t addr, uint8_t *dst, int n)
{
    struct machine *mach;
    int pos;

    if (!d || !dst || n <= 0)
        return 0;
    /* Only while parked: the emulator thread is inside the hook, so the
     * machine cannot be halfway through a bus cycle. */
    mach = parked_machine(d);
    if (!mach)
        return 0;
    for (pos = 0; pos < n; pos++)
        dst[pos] = mach->peek_byte(mach, (unsigned)(uint16_t)(addr + pos));
    return n;
}

int cocodebug_memory_map(cocodebug *d, cocodebug_region *out, int max)
{
    struct machine *mach;
    struct machine_memory_map *map;
    unsigned pos;
    int count;

    if (!d || !out || max <= 0)
        return 0;
    mach = parked_machine(d);
    pthread_mutex_lock(&d->mtx);
    if (mach && mach->memory_map && (!d->map_valid || d->map_serial != d->stop_serial)) {
        mach->memory_map(mach, &d->map);
        d->map_serial = d->stop_serial;
        d->map_valid = 1;
    }
    map = &d->map;
    count = 0;
    if (d->map_valid) {
        for (pos = 0; pos < map->nregions && count < max; pos++) {
            const struct machine_memory_region *region = &map->regions[pos];
            out[count].lo = region->first;
            out[count].hi = region->last;
            out[count].kind = region->kind;
            out[count].writable = region->writable;
            out[count].physical_base = region->physical_base;
            snprintf(out[count].name, sizeof(out[count].name), "%s",
                     region->name ? region->name : "");
            count++;
        }
    }
    pthread_mutex_unlock(&d->mtx);
    return count;
}

/* ---- search ----------------------------------------------------------------- */

static unsigned fold(unsigned ch, int nocase)
{
    return nocase && ch >= 'a' && ch <= 'z' ? ch - 'a' + 'A' : ch;
}

static int match_at(const uint8_t *mem, uint32_t size, uint32_t at, const uint8_t *pat,
                    int patlen, uint8_t mask, int flags)
{
    int pos;
    const int nocase = (flags & COCODEBUG_SEARCH_NOCASE) != 0;

    if (at + (uint32_t)patlen > size)
        return 0;
    for (pos = 0; pos < patlen; pos++)
        if ((fold(mem[at + (uint32_t)pos], nocase) & mask) != (fold(pat[pos], nocase) & mask))
            return 0;
    return 1;
}

/* A copy of the whole space; the caller frees it. NULL when running. */
static uint8_t *snapshot_space(struct cocodebug *d, int space, uint32_t *size)
{
    uint32_t total = cocodebug_space_size(d, space);
    uint8_t *buf;
    int got;

    if (!total || !parked_machine(d))
        return NULL;
    buf = malloc(total);
    if (!buf)
        return NULL;
    got = cocodebug_mem_read(d, space, 0, buf, (int)total);
    if (got != (int)total) {
        free(buf);
        return NULL;
    }
    *size = total;
    return buf;
}

int cocodebug_search(cocodebug *d, int space, const uint8_t *pat, int patlen,
                     uint8_t mask, int flags, uint32_t from, uint32_t to,
                     uint32_t *out, int max)
{
    uint32_t size = 0, at;
    uint8_t *mem;
    int count = 0;

    if (!d || !pat || patlen <= 0 || !out || max <= 0)
        return 0;
    mem = snapshot_space(d, space, &size);
    if (!mem)
        return -1;
    if (to >= size)
        to = size - 1;
    for (at = from; at <= to && count < max; at++)
        if (match_at(mem, size, at, pat, patlen, mask, flags))
            out[count++] = at;
    free(mem);
    return count;
}

int cocodebug_search_next(cocodebug *d, int space, const uint8_t *pat, int patlen,
                          uint8_t mask, int flags, uint32_t from, int backward,
                          uint32_t *found)
{
    uint32_t size = 0, step;
    uint8_t *mem;
    int hit = 0;

    if (!d || !pat || patlen <= 0 || !found)
        return 0;
    mem = snapshot_space(d, space, &size);
    if (!mem)
        return -1;
    /* every address once, starting next to `from`, wrapping */
    for (step = 1; step <= size && !hit; step++) {
        uint32_t at = backward ? (from + size - step) % size : (from + step) % size;
        if (match_at(mem, size, at, pat, patlen, mask, flags)) {
            *found = at;
            hit = 1;
        }
    }
    free(mem);
    return hit;
}

/* ---- disassembly -------------------------------------------------------- */

/* Read one byte for the disassembler, side-effect free. */
static uint8_t dasm_read(uint16_t addr, void *user)
{
    (void)user;
    return machine_peek(addr);
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
    if (!parked_machine(d))
        return 0;

    cpu = current_dasm_cpu();
    while (n < max) {
        coco_dasm_one_cpu(addr, dasm_read, NULL, cpu, &out[n]);
        addr = (uint16_t)(addr + out[n].len);
        n++;
    }
    return n;
}

int cocodebug_disassemble_ex(cocodebug *d, uint16_t addr, cocodebug_line *out,
                             int max, int *pc_line)
{
    coco_dasm_cpu cpu;
    cocodebug_regs regs;
    int count = 0, have_regs, pos;

    if (pc_line)
        *pc_line = -1;
    if (!d || !out || max <= 0 || !parked_machine(d))
        return 0;
    have_regs = cocodebug_regs_get(d, &regs);
    cpu = current_dasm_cpu();
    pthread_mutex_lock(&d->mtx);   /* the breakpoint table is read below */
    while (count < max) {
        coco_dasm_line raw;
        cocodebug_line *line = &out[count];
        int byte;

        coco_dasm_one_cpu(addr, dasm_read, NULL, cpu, &raw);
        memset(line, 0, sizeof(*line));
        line->address = addr;
        line->length = raw.len;
        line->is_pc = have_regs && regs.pc == addr;
        if (line->is_pc && pc_line)
            *pc_line = count;
        {
            size_t used = 0;
            for (byte = 0; byte < raw.len && byte < COCO_DASM_MAX_BYTES && used < sizeof(line->bytes); byte++)
                used += (size_t)snprintf(line->bytes + used, sizeof(line->bytes) - used,
                                         "%s%02X", byte ? " " : "", raw.bytes[byte]);
        }
        snprintf(line->disasm, sizeof(line->disasm), "%s", raw.text);
        line->flags = raw.flags;
        line->flow = raw.flow;
        line->target = raw.target;
        cocosymtab_lookup_addr(d->symtab, addr, line->label, sizeof(line->label));
        if (raw.flags & COCO_DASM_BRANCH)
            cocosymtab_lookup_addr(d->symtab, raw.target, line->comment,
                                   sizeof(line->comment));
        for (pos = 0; pos < d->nbp; pos++) {
            const cocodebug_breakpoint *bp = &d->bp[pos];
            if ((bp->type & COCODEBUG_BP_EXEC) && !(bp->type & COCODEBUG_BP_PHYS) &&
                addr >= bp->start && addr <= bp->end) {
                if (bp->enabled)
                    line->has_breakpoint = 1;
                else
                    line->bp_disabled = 1;
            }
        }
        if (line->has_breakpoint)
            line->bp_disabled = 0;
        addr = (uint16_t)(addr + raw.len);
        count++;
    }
    pthread_mutex_unlock(&d->mtx);
    return count;
}

int cocodebug_row_address(cocodebug *d, uint16_t addr, int rows)
{
    coco_dasm_cpu cpu;
    coco_dasm_line raw;
    int pos, extra, want;
    static const int max_rows = 200;

    if (!d || !parked_machine(d))
        return addr;
    cpu = current_dasm_cpu();
    if (rows >= 0) {
        for (pos = 0; pos < rows; pos++) {
            coco_dasm_one_cpu(addr, dasm_read, NULL, cpu, &raw);
            addr = (uint16_t)(addr + raw.len);
        }
        return addr;
    }
    want = -rows;
    if (want > max_rows)
        want = max_rows;
    /* Variable-length code cannot be walked backwards. Decode forward from a
     * few starting points until one lands exactly on addr, and take the
     * instruction `want` before it. */
    for (extra = 0; extra < 24; extra++) {
        int dist = want * 3 + extra, offset = 0, count = 0;
        uint16_t starts[3 * 200 + 24 + 8];

        while (offset < dist && count < (int)(sizeof(starts) / sizeof(starts[0])) - 1) {
            starts[count++] = (uint16_t)(addr - dist + offset);
            coco_dasm_one_cpu((uint16_t)(addr - dist + offset), dasm_read, NULL, cpu, &raw);
            offset += raw.len;
        }
        if (offset == dist && count >= want)
            return starts[count - want];
    }
    return (uint16_t)(addr - want);
}

void cocodebug_step_over(cocodebug *d)
{
    coco_dasm_line line;

    if (!d)
        return;
    if (!cocodebug_is_paused(d)) {
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
    d->out_active = 0;
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
    if (!d)
        return;
    if (!cocodebug_is_paused(d)) {
        cocodebug_step(d);
        return;
    }

    /* Depth, not a guess at the return address: run until a RETURN-class
     * instruction has executed and S is above where it is now. A nested
     * call's RTS comes back to this S or lower, so it does not stop us; a
     * routine that pushed registers, locals or a frame pointer is no
     * different from one that did not. If it never returns the machine just
     * keeps running and Pause still works. */
    pthread_mutex_lock(&d->mtx);
    d->out_s = d->regs.s;
    d->out_pending = 0;
    d->out_active = 1;
    d->transient_bp = -1;
    d->step_remaining = 0;
    d->src_step = 0;
    d->out_active = 1;
    d->paused = 0;
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->mtx);
}

void cocodebug_run_to(cocodebug *d, uint16_t addr)
{
    if (!d)
        return;
    pthread_mutex_lock(&d->mtx);
    d->transient_bp = addr;
    d->step_remaining = 0;
    d->src_step = 0;
    d->out_active = 0;
    d->paused = 0;
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->mtx);
}

/* ---- breakpoints -------------------------------------------------------- */

int cocodebug_breakpoint_add(cocodebug *d, int type, uint32_t start, uint32_t end,
                             const char *condition)
{
    cocodebug_breakpoint *bp;
    char err[96];
    int id;
    const int phys = (type & COCODEBUG_BP_PHYS) != 0;
    const int kinds = type & (COCODEBUG_BP_EXEC | COCODEBUG_BP_ACCESS);

    if (!d || !kinds)
        return -1;
    if (phys && ((type & COCODEBUG_BP_EXEC) ||
                 (atomic_load(&d->machine_known) && !atomic_load(&d->is_coco3))))
        return -1;
    if (condition && *condition && expr_eval(condition, NULL, NULL, err, sizeof(err)) != 0)
        return -1;
    if (condition && (strstr(condition, "vpos") || strstr(condition, "hpos") ||
                      strstr(condition, "frame")))
        atomic_store(&d->want_beam, 1);
    if (end < start) {
        uint32_t swap = start;
        start = end;
        end = swap;
    }
    if (end >= (phys ? 0x200000u : SPACE_CPU_SIZE))
        return -1;
    pthread_mutex_lock(&d->mtx);
    if (d->nbp >= MAX_BP) {
        pthread_mutex_unlock(&d->mtx);
        return -1;
    }
    bp = &d->bp[d->nbp++];
    memset(bp, 0, sizeof(*bp));
    bp->id = id = d->next_id++;
    bp->type = type & (COCODEBUG_BP_EXEC | COCODEBUG_BP_ACCESS | COCODEBUG_BP_PHYS);
    bp->start = start;
    bp->end = end;
    bp->enabled = 1;
    snprintf(bp->condition, sizeof(bp->condition), "%s", condition ? condition : "");
    rebuild_locked(d);
    pthread_mutex_unlock(&d->mtx);
    bump(d);
    return id;
}

void cocodebug_breakpoint_remove(cocodebug *d, int id)
{
    int pos;

    if (!d)
        return;
    pthread_mutex_lock(&d->mtx);
    for (pos = 0; pos < d->nbp; pos++) {
        if (d->bp[pos].id == id) {
            memmove(&d->bp[pos], &d->bp[pos + 1],
                    (size_t)(d->nbp - pos - 1) * sizeof(d->bp[0]));
            d->nbp--;
            break;
        }
    }
    rebuild_locked(d);
    pthread_mutex_unlock(&d->mtx);
    bump(d);
}

void cocodebug_breakpoint_enable(cocodebug *d, int id, int enabled)
{
    int pos;

    if (!d)
        return;
    pthread_mutex_lock(&d->mtx);
    for (pos = 0; pos < d->nbp; pos++)
        if (d->bp[pos].id == id)
            d->bp[pos].enabled = enabled != 0;
    rebuild_locked(d);
    pthread_mutex_unlock(&d->mtx);
    bump(d);
}

int cocodebug_breakpoints_list(cocodebug *d, cocodebug_breakpoint *out, int max)
{
    int count;

    if (!d || !out || max <= 0)
        return 0;
    pthread_mutex_lock(&d->mtx);
    count = d->nbp < max ? d->nbp : max;
    memcpy(out, d->bp, (size_t)count * sizeof(*out));
    pthread_mutex_unlock(&d->mtx);
    return count;
}

void cocodebug_breakpoint_clear_hits(cocodebug *d)
{
    int pos;

    if (!d)
        return;
    pthread_mutex_lock(&d->mtx);
    for (pos = 0; pos < d->nbp; pos++)
        d->bp[pos].hits = 0;
    pthread_mutex_unlock(&d->mtx);
    bump(d);
}

/* The single-address execute breakpoint toggle the disassembly click uses. */
int cocodebug_breakpoint_toggle(cocodebug *d, uint16_t addr)
{
    int pos, found = -1;

    if (!d)
        return -1;
    pthread_mutex_lock(&d->mtx);
    for (pos = 0; pos < d->nbp; pos++)
        if (d->bp[pos].type == COCODEBUG_BP_EXEC && d->bp[pos].start == addr &&
            d->bp[pos].end == addr) {
            found = d->bp[pos].id;
            break;
        }
    pthread_mutex_unlock(&d->mtx);
    if (found >= 0) {
        cocodebug_breakpoint_remove(d, found);
        return 0;
    }
    return cocodebug_breakpoint_add(d, COCODEBUG_BP_EXEC, addr, addr, NULL) >= 0 ? 1 : -1;
}

int cocodebug_breakpoint_is_set(cocodebug *d, uint16_t addr)
{
    int pos, found = 0;

    if (!d)
        return 0;
    pthread_mutex_lock(&d->mtx);
    for (pos = 0; pos < d->nbp; pos++)
        if ((d->bp[pos].type & COCODEBUG_BP_EXEC) && d->bp[pos].start <= addr &&
            addr <= d->bp[pos].end)
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
    rebuild_locked(d);
    pthread_mutex_unlock(&d->mtx);
    bump(d);
}

static int cmp_u16(const void *a, const void *b)
{
    uint16_t x = *(const uint16_t *)a, y = *(const uint16_t *)b;
    return (x > y) - (x < y);
}

int cocodebug_breakpoint_list(cocodebug *d, uint16_t *out, int max)
{
    int pos, count = 0;

    if (!d || !out || max <= 0)
        return 0;
    pthread_mutex_lock(&d->mtx);
    for (pos = 0; pos < d->nbp && count < max; pos++)
        if (d->bp[pos].type == COCODEBUG_BP_EXEC && d->bp[pos].start == d->bp[pos].end)
            out[count++] = (uint16_t)d->bp[pos].start;
    pthread_mutex_unlock(&d->mtx);
    /* A UI listing them wants them in order. */
    qsort(out, (size_t)count, sizeof(uint16_t), cmp_u16);
    return count;
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

int cocodebug_symbol_add(cocodebug *d, uint16_t addr, const char *name)
{
    return d ? cocosymtab_add(d->symtab, addr, name) : 0;
}

int cocodebug_symbols_count(cocodebug *d)
{
    return d ? cocosymtab_count(d->symtab) : 0;
}

int cocodebug_symbols_get(cocodebug *d, int index, uint16_t *addr, char *name,
                          int namesz)
{
    return d ? cocosymtab_get(d->symtab, index, addr, name, namesz, NULL) : 0;
}

typedef struct {
    uint16_t addr;
    char name[32];
    int index;
} sym_pick;

static int sym_pick_cmp(const void *left, const void *right)
{
    const sym_pick *a = left, *b = right;
    if (a->addr != b->addr)
        return a->addr < b->addr ? -1 : 1;
    return strcmp(a->name, b->name);
}

static int contains_nocase(const char *hay, const char *needle)
{
    size_t len = strlen(needle);
    if (!len)
        return 1;
    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, len))
            return 1;
    return 0;
}

int cocodebug_symbols_filter(cocodebug *d, const char *text, int *out, int max)
{
    int total, index, count = 0, pos;
    sym_pick *pick;

    if (!d || !out || max <= 0)
        return 0;
    if (!text)
        text = "";
    total = cocosymtab_count(d->symtab);
    pick = malloc(sizeof(*pick) * (size_t)(total ? total : 1));
    if (!pick)
        return 0;
    for (index = 0; index < total; index++) {
        sym_pick *slot = &pick[count];
        if (!cocosymtab_get(d->symtab, index, &slot->addr, slot->name,
                            (int)sizeof(slot->name), NULL))
            continue;
        if (!contains_nocase(slot->name, text))
            continue;
        slot->index = index;
        count++;
    }
    qsort(pick, (size_t)count, sizeof(*pick), sym_pick_cmp);
    if (count > max)
        count = max;
    for (pos = 0; pos < count; pos++)
        out[pos] = pick[pos].index;
    free(pick);
    return count;
}

/* ---- the trace --------------------------------------------------------------- */

void cocodebug_trace_enable(cocodebug *d, int on)
{
    if (!d)
        return;
    if (on) {
        pthread_mutex_lock(&d->trace_ring_mtx);
        if (!d->ring)
            d->ring = calloc(TRACE_N, sizeof(*d->ring));
        if (!d->ring) {
            pthread_mutex_unlock(&d->trace_ring_mtx);
            return;
        }
        pthread_mutex_unlock(&d->trace_ring_mtx);
    }
    if (on)
        atomic_store(&d->want_beam, 1);
    atomic_store(&d->trace_on, on ? 1 : 0);
    bump(d);
}

int cocodebug_trace_enabled(cocodebug *d)
{
    return d ? atomic_load(&d->trace_on) : 0;
}

void cocodebug_trace_clear(cocodebug *d)
{
    if (!d)
        return;
    pthread_mutex_lock(&d->trace_ring_mtx);
    d->ring_head = d->ring_count = 0;
    pthread_mutex_unlock(&d->trace_ring_mtx);
    bump(d);
}

int cocodebug_trace_count(cocodebug *d)
{
    int count;

    if (!d)
        return 0;
    pthread_mutex_lock(&d->trace_ring_mtx);
    count = d->ring_count;
    pthread_mutex_unlock(&d->trace_ring_mtx);
    return count;
}

static uint8_t entry_read(uint16_t addr, void *user)
{
    const cocodebug_trace_entry *ent = user;
    unsigned off = (uint16_t)(addr - ent->pc);
    return off < sizeof(ent->bytes) ? ent->bytes[off] : 0;
}

static void entry_decode(const cocodebug_trace_entry *ent, coco_dasm_line *line)
{
    coco_dasm_one_cpu(ent->pc, entry_read, (void *)ent,
                      ent->is6309 ? COCO_DASM_CPU_HD6309 : COCO_DASM_CPU_MC6809, line);
}

int cocodebug_trace_read(cocodebug *d, cocodebug_trace_entry *out, int max)
{
    int count, pos;

    if (!d || !out || max <= 0)
        return 0;
    pthread_mutex_lock(&d->trace_ring_mtx);
    count = d->ring_count < max ? d->ring_count : max;
    for (pos = 0; pos < count; pos++)
        out[pos] = d->ring[(d->ring_head - 1 - pos + TRACE_N * 2) % TRACE_N];
    pthread_mutex_unlock(&d->trace_ring_mtx);
    for (pos = 0; pos < count; pos++) {
        coco_dasm_line line;
        entry_decode(&out[pos], &line);
        out[pos].len = line.len;
    }
    return count;
}

int cocodebug_trace_format(const cocodebug_trace_entry *ent, char *dst, int dstsz)
{
    static const char flag_names[] = "EFHINZVC";
    coco_dasm_line line;
    char flags[9];
    int n, bit;

    if (!ent || !dst || dstsz <= 0)
        return 0;
    entry_decode(ent, &line);
    for (bit = 0; bit < 8; bit++)
        flags[bit] = (ent->cc & (0x80 >> bit)) ? flag_names[bit] : '.';
    flags[8] = '\0';
    n = snprintf(dst, (size_t)dstsz,
                 "%04X  %-18s A=%02X B=%02X DP=%02X X=%04X Y=%04X U=%04X S=%04X CC=%02X %s",
                 ent->pc, line.text, ent->a, ent->b, ent->dp, ent->x, ent->y, ent->u,
                 ent->s, ent->cc, flags);
    if (ent->is6309 && n < dstsz)
        n += snprintf(dst + n, (size_t)(dstsz - n), " E=%02X F=%02X W=%04X V=%04X MD=%02X",
                      ent->w >> 8, ent->w & 0xff, ent->w, ent->v, ent->md);
    return n < dstsz ? n : dstsz - 1;
}

int cocodebug_trace_match(const cocodebug_trace_entry *ent, int filter, uint16_t lo,
                          uint16_t hi)
{
    coco_dasm_line line;

    if (!ent)
        return 0;
    switch (filter) {
    case COCODEBUG_TRACE_BRANCHES:
        entry_decode(ent, &line);
        return line.flow != COCO_FLOW_NONE;
    case COCODEBUG_TRACE_RANGE:
        if (lo > hi) {
            uint16_t swap = lo;
            lo = hi;
            hi = swap;
        }
        return ent->pc >= lo && ent->pc <= hi;
    default:
        return 1;
    }
}

/* ---- Cart & I/O and sound --------------------------------------------------- */

static void pia_fill(const struct MC6821 *pia, cocodebug_pia *out)
{
    out->cra = pia->a.control_register;
    out->crb = pia->b.control_register;
    out->ddra = pia->a.direction_register;
    out->ddrb = pia->b.direction_register;
    out->ora = pia->a.output_register;
    out->orb = pia->b.output_register;
    out->pina = PIA_VALUE_A(pia);
    out->pinb = PIA_VALUE_B(pia);
    out->ca1 = pia->a.cx1;
    out->ca2 = PIA_VALUE_CA2(pia) != 0;
    out->cb1 = pia->b.cx1;
    out->cb2 = PIA_VALUE_CB2(pia) != 0;
    out->irqa = pia->a.irq;
    out->irqb = pia->b.irq;
    out->irq_flags_a = (uint8_t)(pia->a.irq1_received | pia->a.irq2_received);
    out->irq_flags_b = (uint8_t)(pia->b.irq1_received | pia->b.irq2_received);
}

int cocodebug_io_get(cocodebug *d, cocodebug_io *out)
{
    struct machine *mach;
    struct MC6821 *pia;
    struct MC6883 *sam;
    struct TCC1014 *gime;
    int port, axis;

    if (!out)
        return 0;
    memset(out, 0, sizeof(*out));
    mach = d ? parked_machine(d) : NULL;
    if (!mach)
        return 0;
    out->is6309 = d->hooked_6309;
    pia = (struct MC6821 *)part_component_by_id_is_a(&mach->part, "PIA0", "MC6821");
    if (pia)
        pia_fill(pia, &out->pia0);
    pia = (struct MC6821 *)part_component_by_id_is_a(&mach->part, "PIA1", "MC6821");
    if (pia)
        pia_fill(pia, &out->pia1);
    sam = (struct MC6883 *)part_component_by_id_is_a(&mach->part, "SAM", "SN74LS783");
    if (sam) {
        struct mc6883_debug_state ss;
        if (mc6883_get_debug_state(sam, &ss)) {
            out->sam_valid = 1;
            out->sam_v = ss.V;
            out->sam_f = ss.F;
            out->sam_p = ss.P;
            out->sam_r = ss.R;
            out->sam_m = ss.M;
            out->sam_ty = ss.TY;
            out->sam_video_address = ss.video_address;
        }
    }
    gime = (struct TCC1014 *)part_component_by_id_is_a(&mach->part, "GIME", "TCC1014");
    if (gime) {
        struct tcc1014_debug_state gs;
        tcc1014_get_debug_state(gime, &gs);
        out->is_coco3 = 1;
        out->gime_valid = 1;
        memcpy(out->gime_reg, gs.registers, sizeof(out->gime_reg));
        memcpy(out->gime_palette, gs.palette_reg, sizeof(out->gime_palette));
        memcpy(out->gime_mmu, gs.mmu_bank, sizeof(out->gime_mmu));
        out->gime_scanline = gs.scanline;
        out->gime_row = gs.row;
        out->gime_irq = (int)gs.irq_state;
        out->gime_firq = (int)gs.firq_state;
    }
    for (port = 0; port < 2; port++)
        for (axis = 0; axis < 2; axis++) {
            out->joy_axis[port][axis] = cocohost_get_joystick_axis(port, axis);
            out->joy_button[port][axis] = cocohost_get_joystick_button(port, axis);
        }
    out->becker_link_up = cocohost_becker_link_up();
    out->becker_rx_pending = cocohost_becker_rx_pending();
    return 1;
}

int cocodebug_cart_info(cocodebug *d, char *dst, int dstsz)
{
    struct machine *mach;
    struct cart *cart;
    int n;

    if (!dst || dstsz <= 0)
        return 0;
    dst[0] = '\0';
    mach = d ? xroar.machine : NULL;
    if (!mach)
        return snprintf(dst, (size_t)dstsz, "Cartridge: no machine");
    cart = (struct cart *)part_component_by_id_is_a(&mach->part, "cart", "dragon-cart");
    if (!cart || !cart->config)
        return snprintf(dst, (size_t)dstsz, "Cartridge: none");
    n = snprintf(dst, (size_t)dstsz, "Cartridge: %s",
                 cart->config->description ? cart->config->description
                                           : cart->config->name ? cart->config->name : "?");
    if (n < dstsz && cart->config->name)
        n += snprintf(dst + n, (size_t)(dstsz - n), " '%s'", cart->config->name);
    if (n < dstsz && cart->config->rom)
        n += snprintf(dst + n, (size_t)(dstsz - n), ", ROM %s", cart->config->rom);
    if (n < dstsz)
        n += snprintf(dst + n, (size_t)(dstsz - n), "%s",
                      cart->config->becker_port ? ", Becker port" : "");
    return n < dstsz ? n : dstsz - 1;
}

int cocodebug_sound_get(cocodebug *d, cocodebug_sound *out)
{
    static const char *const mux_names[4] = { "DAC", "cassette", "cartridge", "none" };
    struct machine *mach;
    struct MC6821 *pia0, *pia1;

    if (!out)
        return 0;
    memset(out, 0, sizeof(*out));
    out->mux_name = mux_names[0];
    mach = d ? parked_machine(d) : NULL;
    if (!mach)
        return 0;
    pia0 = (struct MC6821 *)part_component_by_id_is_a(&mach->part, "PIA0", "MC6821");
    pia1 = (struct MC6821 *)part_component_by_id_is_a(&mach->part, "PIA1", "MC6821");
    if (!pia0 || !pia1)
        return 0;
    out->dac = (PIA_VALUE_A(pia1) & 0xFC) >> 2;
    out->single_bit = (PIA_VALUE_B(pia1) >> 1) & 1;
    out->sound_enable = PIA_VALUE_CB2(pia1) != 0;
    out->motor = PIA_VALUE_CA2(pia1) != 0;
    out->mux_source = ((PIA_VALUE_CB2(pia0) != 0) << 1) | (PIA_VALUE_CA2(pia0) != 0);
    out->mux_name = mux_names[out->mux_source];
    out->cassette_in = PIA_VALUE_A(pia1) & 1;
    return 1;
}

void cocodebug_sound_watch(cocodebug *d, int on)
{
    if (!d)
        return;
    pthread_mutex_lock(&d->mtx);
    atomic_store(&d->sound_watch, on ? 1 : 0);
    rebuild_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

int cocodebug_sound_history(cocodebug *d, cocodebug_sound_write *out, int max)
{
    int count, pos;

    if (!d || !out || max <= 0)
        return 0;
    pthread_mutex_lock(&d->sound_mtx);
    count = d->sound_count < max ? d->sound_count : max;
    for (pos = 0; pos < count; pos++)
        out[pos] = d->sound_ring[(d->sound_head - 1 - pos + SOUND_N * 2) % SOUND_N];
    pthread_mutex_unlock(&d->sound_mtx);
    return count;
}

void cocodebug_sound_history_clear(cocodebug *d)
{
    if (!d)
        return;
    pthread_mutex_lock(&d->sound_mtx);
    d->sound_head = d->sound_count = 0;
    pthread_mutex_unlock(&d->sound_mtx);
}

void cocodebug_set_video_text(cocodebug_video_text_fn fn)
{
    g_video_text = fn;
}

cocodebug_video_text_fn cocodebug_video_text_get(void)
{
    return g_video_text;
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
    d->out_active = 0;
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
    d->out_active = 0;
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
    /* A session is starting: re-arm the engine (see session_begin). */
    session_begin(d);
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
                 "Becker port     %s\n"
                 "FujiNet         %s\n"
                 "Link            %s\n"
                 "Unread          %d B\n"
                 "\n"
                 "CoCo \xe2\x86\x92 FujiNet  %llu B\n"
                 "FujiNet \xe2\x86\x92 CoCo  %llu B\n"
                 "Exchanges       %llu\n"
                 "\n"
                 "In progress     %s\n",
                 cocosession_becker_endpoint(d->s),
                 d->s->fujinet_external ? "external" :
                 d->s->fujinet_running ? "listening" : "off",
                 cocohost_becker_link_up() ? "connected" : "not connected",
                 cocohost_becker_rx_pending(), out, in, ex, pending);
    return n < dstsz ? n : dstsz - 1;
}
