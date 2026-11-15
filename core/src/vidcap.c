/*
 * vidcap -- per-line capture of the video chip for the debugger's Video tab
 * (core/include/cocovidcap.h).
 *
 * While a user is enabled, the emulator thread keeps the machine's video taps
 * (core/include/cocoxroar.h) on the VDG or GIME and chains the delegate the
 * chip fetches its display bytes through. Every finished scanline becomes a
 * record in s_work (the chip registers as they stood, the bytes fetched), so
 * s_work always holds this frame's lines above the beam and the previous
 * frame's below it. cocohost_emit_frame hands each published frame to
 * cocovid_host_frame(), which copies s_work and the frame into a double
 * buffer; readers copy the front one. A parked machine is read straight from
 * s_work with the partial frame the video output holds.
 *
 * Threading: the taps and cocovid_host_frame() run on the emulator thread;
 * s_lock only ever guards memcpys, so no tap waits on a UI thread that waits
 * on the emulator. Everything touching the chips is emulator-thread or
 * parked-machine only (see cocoxroar.h).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "top-config.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "cocoxroar.h"
#include "cocovidcap.h"

#define MAX_SUBSCRIBERS 4

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int s_users;
static cocovid_line s_work[COCOVID_MAX_LINES];
static cocovid_capture *s_back[2];      /* allocated by the first enable */
static int s_front;
static uint64_t s_serial;
static uint32_t s_frame_no = 1;         /* the frame in progress; a fresh line carries it */

/* emulator thread (or a parked machine) */
static struct machine *s_mach;
static struct MC6847 *s_vdg;
static struct TCC1014 *s_gime;
static struct MC6821 *s_pia1;
static struct MC6883 *s_sam;
static int s_installed;
static int s_synced;                    /* a vertical sync has been seen since the taps went on */
static int s_vline;                     /* lines finished since the vertical sync */
static int s_frame_lines;               /* lines in the last finished frame */
static uint16_t s_mode0;
static uint32_t s_last_tick, s_prev_tick;
static uint8_t s_cur_data[COCOVID_MAX_BYTES];
static unsigned s_cur_n;
static uint32_t s_cur_addr;
static int s_cur_first;
static struct machine_video_tap s_subs[MAX_SUBSCRIBERS];
static int s_nsubs;

static DELEGATE_T3(void, uint16, int, uint16p) s_saved_vdg_fetch;
static DELEGATE_T1(uint16, uint32) s_saved_gime_fetch;

/* ---- the fetch wrappers ----------------------------------------------------- */

static void cur_reset(void)
{
    s_cur_n = 0;
    s_cur_first = 0;
}

static void cur_append(uint8_t value)
{
    if (s_cur_n < COCOVID_MAX_BYTES)
        s_cur_data[s_cur_n] = value;
    s_cur_n++;
}

/* The VDG asks for `nbytes` display bytes; the dragon's handler walks the SAM's
 * address counter, so the counter is the first byte's address. */
static void vdg_fetch(void *sptr, uint16_t addr, int nbytes, uint16_t *dest)
{
    int idx;

    (void)sptr;
    if (!s_cur_first && s_sam) {
        struct mc6883_debug_state sam_state;

        s_cur_addr = mc6883_get_debug_state(s_sam, &sam_state) ? sam_state.video_address : 0;
        s_cur_first = 1;
    }
    DELEGATE_SAFE_CALL(s_saved_vdg_fetch, addr, nbytes, dest);
    if (dest)
        for (idx = 0; idx < nbytes; idx++)
            cur_append((uint8_t)dest[idx]);
}

/* The GIME fetches 16 bits at a time; both bytes belong to the line even when
 * the renderer drops one. */
static uint16_t gime_fetch(void *sptr, uint32_t addr)
{
    uint16_t word;

    (void)sptr;
    word = DELEGATE_CALL(s_saved_gime_fetch, addr);
    if (!s_cur_first) {
        s_cur_addr = addr;
        s_cur_first = 1;
    }
    cur_append((uint8_t)(word >> 8));
    cur_append((uint8_t)word);
    return word;
}

static void chain_fetch(void)
{
    if (s_vdg && s_vdg->fetch_data.func != vdg_fetch) {
        s_saved_vdg_fetch = s_vdg->fetch_data;
        s_vdg->fetch_data = DELEGATE_AS3(void, uint16, int, uint16p, vdg_fetch, NULL);
    }
    if (s_gime && s_gime->fetch_vram.func != gime_fetch) {
        s_saved_gime_fetch = s_gime->fetch_vram;
        s_gime->fetch_vram = DELEGATE_AS1(uint16, uint32, gime_fetch, NULL);
    }
}

static void unchain_fetch(void)
{
    if (s_vdg && s_vdg->fetch_data.func == vdg_fetch)
        s_vdg->fetch_data = s_saved_vdg_fetch;
    if (s_gime && s_gime->fetch_vram.func == gime_fetch)
        s_gime->fetch_vram = s_saved_gime_fetch;
}

/* ---- the taps ----------------------------------------------------------------- */

static uint8_t pia_mode(void)
{
    return s_pia1 ? (uint8_t)((s_pia1->b.out_source & s_pia1->b.out_sink) & 0xf8u) : 0;
}

/* What identifies the mode a line is drawn in, to tell a mid-line change. */
static uint16_t mode_signature(void)
{
    if (s_gime) {
        struct tcc1014_debug_state gime_state;

        tcc1014_get_debug_state(s_gime, &gime_state);
        if (gime_state.COCO)
            return (uint16_t)(pia_mode() | (gime_state.SAM_V & 7u) << 8);
        return (uint16_t)(gime_state.registers[8] << 8 | gime_state.registers[9]);
    }
    return pia_mode();
}

static void record_line(const struct machine_video_line *video_line)
{
    cocovid_line rec;
    int active = s_cur_n > 0;

    if (!s_synced || s_vline < 0 || s_vline >= COCOVID_MAX_LINES)
        goto done;
    memset(&rec, 0, sizeof rec);
    rec.valid = 1;
    rec.gime = s_gime != NULL;
    rec.chip_line = (uint16_t)video_line->scanline;
    rec.stamp = s_frame_no;
    rec.tick = video_line->tick;
    rec.mode0 = s_mode0;
    rec.mode1 = mode_signature();
    rec.flags = (uint8_t)((active ? COCOVID_LF_ACTIVE : 0) |
                          (rec.mode0 != rec.mode1 ? COCOVID_LF_MIDLINE : 0));
    rec.pia_mode = pia_mode();
    if (s_vdg) {
        /* the row counter has already stepped past an active line */
        rec.vdg_row = (uint8_t)(active ? (s_vdg->row + 11u) % 12u : s_vdg->row % 12u);
    }
    if (s_sam) {
        struct mc6883_debug_state sam_state;

        if (mc6883_get_debug_state(s_sam, &sam_state)) {
            rec.sam_v = (uint8_t)sam_state.V;
            rec.sam_ty = (uint8_t)sam_state.TY;
            rec.sam_f = (uint16_t)sam_state.F;
            rec.sam_reg = (uint16_t)sam_state.reg;
        }
    }
    if (s_gime) {
        struct tcc1014_debug_state gime_state;

        tcc1014_get_debug_state(s_gime, &gime_state);
        rec.g_coco = gime_state.COCO;
        rec.g_bp = gime_state.BP;
        rec.g_moch = gime_state.MOCH;
        rec.g_lpr = (uint8_t)gime_state.LPR;
        rec.g_lpf = (uint8_t)gime_state.LPF;
        rec.g_hres = (uint8_t)gime_state.HRES;
        rec.g_cres = (uint8_t)gime_state.CRES;
        rec.g_hven = gime_state.HVEN;
        rec.g_vsc = (uint8_t)gime_state.VSC;
        rec.g_row = (uint8_t)gime_state.row;
        rec.g_brdr = gime_state.BRDR;
        rec.g_sam_v = gime_state.SAM_V;
        rec.g_sam_f = gime_state.SAM_F;
        memcpy(rec.g_regs, gime_state.registers, sizeof rec.g_regs);
        memcpy(rec.g_pal, gime_state.palette_reg, sizeof rec.g_pal);
        rec.g_y = gime_state.Y;
        rec.g_b = gime_state.B;
        rec.g_x = (uint8_t)gime_state.X;
    }
    rec.addr = s_cur_addr;
    rec.nbytes = (uint16_t)(s_cur_n < COCOVID_MAX_BYTES ? s_cur_n : COCOVID_MAX_BYTES);
    memcpy(rec.data, s_cur_data, rec.nbytes);

    pthread_mutex_lock(&s_lock);
    s_work[s_vline] = rec;
    pthread_mutex_unlock(&s_lock);
done:
    s_prev_tick = s_last_tick;
    s_last_tick = video_line->tick;
    if (s_synced)
        s_vline++;
    cur_reset();
}

static void tap_render_line(void *sptr, const struct machine_video_line *video_line)
{
    int idx;

    (void)sptr;
    record_line(video_line);
    for (idx = 0; idx < s_nsubs; idx++)
        if (s_subs[idx].render_line)
            s_subs[idx].render_line(s_subs[idx].ctx, video_line);
}

static void tap_hsync(void *sptr, _Bool level, unsigned scanline, uint32_t tick)
{
    int idx;

    (void)sptr;
    if (!level)
        s_mode0 = mode_signature();
    for (idx = 0; idx < s_nsubs; idx++)
        if (s_subs[idx].hsync)
            s_subs[idx].hsync(s_subs[idx].ctx, level, scanline, tick);
}

static void tap_fsync(void *sptr, _Bool level, unsigned scanline, uint32_t tick)
{
    int idx;

    (void)sptr;
    if (level) {
        if (s_synced && s_vline > 0)
            s_frame_lines = s_vline;
        s_vline = 0;
        s_synced = 1;
    }
    for (idx = 0; idx < s_nsubs; idx++)
        if (s_subs[idx].fsync)
            s_subs[idx].fsync(s_subs[idx].ctx, level, scanline, tick);
}

static const struct machine_video_tap s_tap = {
    NULL, tap_render_line, tap_hsync, tap_fsync
};

/* ---- keeping the taps on the machine ------------------------------------------ */

static void clear_state(void)
{
    pthread_mutex_lock(&s_lock);
    memset(s_work, 0, sizeof s_work);
    pthread_mutex_unlock(&s_lock);
    s_synced = 0;
    s_vline = 0;
    s_frame_lines = 0;
    s_last_tick = s_prev_tick = 0;
    cur_reset();
}

/* Emulator thread, or a parked machine. */
static void ensure_installed(struct machine *mach)
{
    struct MC6847 *vdg = (struct MC6847 *)part_component_by_id_is_a(&mach->part, "VDG", "MC6847");
    struct TCC1014 *gime = vdg ? NULL
        : (struct TCC1014 *)part_component_by_id_is_a(&mach->part, "GIME", "TCC1014");
    int same = mach == s_mach && vdg == s_vdg && gime == s_gime;

    if (!vdg && !gime)
        return;
    if (!same || !machine_debug_video_tap_active(mach)) {
        clear_state();
        s_mach = mach;
        s_vdg = vdg;
        s_gime = gime;
        s_pia1 = (struct MC6821 *)part_component_by_id_is_a(&mach->part, "PIA1", "MC6821");
        s_sam = (struct MC6883 *)part_component_by_id_is_a(&mach->part, "SAM", "SN74LS783");
        if (!machine_debug_video_tap_install(mach, &s_tap))
            return;
    }
    chain_fetch();
    s_installed = 1;
}

/* Take our wrappers off, if they are on the machine that is running now. */
static void uninstall(void)
{
    struct machine *mach = xroar.machine;

    if (s_installed && mach && mach == s_mach)
        unchain_fetch();
    if (s_installed && mach && mach == s_mach)
        machine_debug_video_tap_remove(mach);
    s_installed = 0;
    s_mach = NULL;
    s_vdg = NULL;
    s_gime = NULL;
    s_pia1 = NULL;
    s_sam = NULL;
}

static int wanted(void)
{
    return atomic_load(&s_users) > 0 || s_nsubs > 0;
}

int cocovid_host_tap_add(const struct machine_video_tap *tap)
{
    if (!tap || s_nsubs >= MAX_SUBSCRIBERS)
        return 0;
    s_subs[s_nsubs++] = *tap;
    return 1;
}

void cocovid_host_tap_remove(const struct machine_video_tap *tap)
{
    int idx;

    for (idx = 0; tap && idx < s_nsubs; idx++)
        if (s_subs[idx].ctx == tap->ctx && s_subs[idx].render_line == tap->render_line &&
            s_subs[idx].hsync == tap->hsync && s_subs[idx].fsync == tap->fsync) {
            memmove(&s_subs[idx], &s_subs[idx + 1],
                    (size_t)(s_nsubs - idx - 1) * sizeof s_subs[0]);
            s_nsubs--;
            break;
        }
}

/* ---- the capture --------------------------------------------------------------- */

void cocovid_host_capture_enable(int on)
{
    if (on) {
        int slot;

        pthread_mutex_lock(&s_lock);
        for (slot = 0; slot < 2; slot++)
            if (!s_back[slot])
                s_back[slot] = calloc(1, sizeof *s_back[slot]);
        pthread_mutex_unlock(&s_lock);
        atomic_fetch_add(&s_users, 1);
    } else if (atomic_load(&s_users) > 0) {
        atomic_fetch_sub(&s_users, 1);
    }
}

/* The video output's geometry, as the frame sees it. */
static void fill_geometry(cocovid_capture *dst)
{
    const struct vo_interface *vo = xroar.ui_interface ? xroar.ui_interface->vo_interface : NULL;
    const struct vo_render *renderer = vo ? vo->renderer : NULL;

    if (!renderer)
        return;
    dst->first_line = renderer->viewport.y;
    dst->act_x = renderer->active_area.x - renderer->viewport.x;
    dst->act_y = renderer->active_area.y;
    dst->act_w = renderer->active_area.w;
    dst->act_h = renderer->active_area.h;
}

/* Lines out of the working array, with `fresh` against the frame number. */
static void copy_lines(cocovid_capture *dst, uint32_t fresh_stamp)
{
    int idx;

    memcpy(dst->lines, s_work, sizeof dst->lines);
    for (idx = 0; idx < COCOVID_MAX_LINES; idx++)
        dst->lines[idx].fresh = dst->lines[idx].valid && dst->lines[idx].stamp == fresh_stamp;
}

void cocovid_host_frame(const uint32_t *xrgb8888, int width, int height)
{
    struct machine *mach = xroar.machine;
    cocovid_capture *back;

    if (!mach)
        return;
    if (!wanted()) {
        if (s_installed)
            uninstall();
        return;
    }
    ensure_installed(mach);
    if (atomic_load(&s_users) > 0 && s_synced && s_vline > 0 && xrgb8888 && width > 0 &&
        height > 0 && width <= COCOSESSION_FB_MAX_WIDTH && height <= COCOSESSION_FB_MAX_HEIGHT) {
        pthread_mutex_lock(&s_lock);
        back = s_back[1 - s_front];
        pthread_mutex_unlock(&s_lock);
        if (back) {
            back->frame = s_frame_no;
            back->gime = s_gime != NULL;
            back->nlines = s_vline < COCOVID_MAX_LINES ? s_vline : COCOVID_MAX_LINES;
            back->stopped = 0;
            back->has_pixels = 1;
            back->frame_w = width;
            back->frame_h = height;
            memcpy(back->pixels, xrgb8888, (size_t)width * (size_t)height * sizeof *back->pixels);
            fill_geometry(back);
            back->beam_line = back->nlines;
            back->rows_done = height;
            back->tick = (uint32_t)event_current_tick;
            back->line_ticks = s_last_tick - s_prev_tick;
            back->beam_ticks = 0;
            copy_lines(back, s_frame_no);
            pthread_mutex_lock(&s_lock);
            back->serial = ++s_serial;
            s_front = 1 - s_front;
            pthread_mutex_unlock(&s_lock);
        }
    }
    s_frame_no++;
}

int cocovid_host_capture_get(cocovid_capture *dst, uint64_t *serial_inout)
{
    int changed = 0;

    pthread_mutex_lock(&s_lock);
    if (s_back[s_front] && s_back[s_front]->serial != 0 &&
        s_back[s_front]->serial != *serial_inout) {
        memcpy(dst, s_back[s_front], sizeof *dst);
        *serial_inout = s_back[s_front]->serial;
        changed = 1;
    }
    pthread_mutex_unlock(&s_lock);
    return changed;
}

int cocovid_host_capture_live(cocovid_capture *dst)
{
    struct machine *mach = xroar.machine;
    struct cocoxroar_partial_frame partial;
    int idx, any = 0;

    if (!dst || !mach)
        return 0;
    if (wanted() && (!s_installed || mach != s_mach))
        ensure_installed(mach);

    pthread_mutex_lock(&s_lock);
    memcpy(dst->lines, s_work, sizeof dst->lines);
    pthread_mutex_unlock(&s_lock);
    for (idx = 0; idx < COCOVID_MAX_LINES; idx++) {
        dst->lines[idx].fresh = dst->lines[idx].valid && dst->lines[idx].stamp == s_frame_no;
        any |= dst->lines[idx].valid;
    }
    dst->frame = s_frame_no;
    dst->gime = s_gime != NULL || part_component_by_id_is_a(&mach->part, "GIME", "TCC1014") != NULL;
    dst->stopped = 1;
    dst->has_pixels = 0;
    dst->frame_w = dst->frame_h = 0;
    dst->nlines = s_frame_lines > 0 ? s_frame_lines : COCOVID_MAX_LINES;
    dst->tick = (uint32_t)event_current_tick;
    dst->line_ticks = s_last_tick - s_prev_tick;
    dst->beam_ticks = s_last_tick ? dst->tick - s_last_tick : 0;
    dst->beam_line = s_synced ? s_vline : 0;
    dst->rows_done = 0;
    fill_geometry(dst);
    if (cocoxroar_partial_frame(&partial) && partial.width > 0 && partial.height > 0 &&
        partial.width <= COCOSESSION_FB_MAX_WIDTH && partial.height <= COCOSESSION_FB_MAX_HEIGHT) {
        int row;

        for (row = 0; row < partial.height; row++)
            memcpy(&dst->pixels[(size_t)row * (size_t)partial.width],
                   &partial.pixels[(size_t)row * (size_t)partial.pitch],
                   (size_t)partial.width * sizeof *dst->pixels);
        dst->has_pixels = 1;
        dst->frame_w = partial.width;
        dst->frame_h = partial.height;
        dst->first_line = partial.first_line;
        dst->rows_done = partial.rows_done;
        if (!s_synced)
            dst->beam_line = partial.scanline;
    }
    dst->serial = 0;
    return any || dst->has_pixels;
}
