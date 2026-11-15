/*
 * cocoxroar -- the engine-side hooks the debugger builds on.
 *
 * XRoar 1.11 is staged and patched at build time (cmake/StageXRoar.cmake,
 * tools/xroar/patch-staged-tree.py). The patch adds everything declared in
 * the staged header "machine_debug.h" (source: tools/xroar/support/
 * machine_debug.h; the C bodies are the .inc files in tools/xroar/support/fragments).
 * Including this header brings it in, with the XRoar headers it needs, and
 * adds one inline helper (the partial frame). This file is the contract.
 *
 * THREADING. Every tap runs on the emulator thread, inside a CPU bus cycle or
 * a video event, with the machine in the middle of its work. A tap must be
 * fast, must not block, must not take a lock a UI thread can hold while it
 * waits for the emulator, and must not call back into the machine or the
 * debugger (no peek, no pause, no read_byte). Copy what you need into a ring
 * and return. The functions that install or remove a tap, and every peek,
 * poke, memory_map and accessor below, are for the emulator thread or for a
 * moment when it is parked (inside instruction_hook, as cocodebug does):
 * XRoar has no lock to take, so they are not safe against a running machine.
 *
 * WHEN THE MACHINE CHANGES. A CoCo 2 <-> CoCo 3 switch, or a session
 * stop/start, frees the machine and builds another. The bus tap is a field of
 * the machine and goes with it; the video tap wrappers sit on the chip's
 * delegates and go with the chip. Check once per frame, from the emulator
 * thread, and install again:
 *
 *     struct machine *mach = xroar.machine;
 *     if (mach && mach->bus_tap != my_bus_tap)
 *         machine_debug_set_bus_tap(mach, my_bus_tap, my_ctx);
 *     if (mach && !machine_debug_video_tap_active(mach))
 *         machine_debug_video_tap_install(mach, &my_video_tap);
 *
 * xroar_shutdown() calls machine_debug_forget(), so a stop/start leaves no
 * stale state behind on our side.
 *
 * THE HOOKS (prototypes are in machine_debug.h; this is what they promise)
 *
 * 1. Bus tap
 *      void machine_debug_set_bus_tap(struct machine *, machine_bus_tap_func,
 *                                     void *ctx);
 *      void func(void *ctx, const struct machine_bus_cycle *cycle);
 *    Called once per CPU bus cycle that advances the clock, for CoCo 1/2
 *    (dragon_cpu_cycle(), skipped while clock_inhibit is set, i.e. never for
 *    machine->read_byte / write_byte or trap code) and CoCo 3 (cpu_cycle(),
 *    never cpu_cycle_noclock()). `address` is the CPU address, `value` the
 *    byte on the data bus (the byte read, or the byte the CPU drives on a
 *    write), `physical` the RAM element index the cycle selected or -1,
 *    `has_old`/`old_value` the byte that was in RAM BEFORE a write's store.
 *    Writes to ROM or I/O have physical = -1 and has_old = 0. The 6809's
 *    dummy cycles ($FFFF reads, repeated PC reads) are not filtered: the
 *    consumer decides which cycles are real accesses.
 *
 * 2. Side-effect-free memory
 *      mach->peek_byte(mach, address)         CPU space
 *      mach->poke_byte(mach, address, value)  RAM only, returns stored?
 *      mach->memory_map(mach, &map)           regions + SAM/GIME state
 *      machine_debug_ram_size / _peek / _poke by physical index
 *    peek_byte never runs a bus cycle: PIA, GIME and MMU registers read as
 *    latched values; $FF92/$FF93 report the pending bits without clearing
 *    them; cartridge I/O (Becker $FF41/$FF42, floppy $FF48-$FF4F) and the
 *    SAM read back $FF; nothing fires dwtrace or consumes a FujiNet byte.
 *    `physical` is a linear index over the RAM banks: on the CoCo 3 it is
 *    the GIME's 19-bit RAM address (MMU bank << 13 | offset), plus 512K for
 *    each extra bank on a 1M/2M machine; on the CoCo 1/2 it is the DRAM
 *    row/column index, which the SAM scrambles, so it is not the CPU address.
 *
 * 3. Chip state
 *      mc6883_get_debug_state(sam, &state)    V F P R M TY, packed register,
 *                                             current VDG address counter
 *      mc6847_get_debug_state(vdg, &state)    scanline, GM, A/S, A/G, INT/EXT, CSS
 *      tcc1014_get_debug_state(gime, &state)  the existing fields, plus B,
 *          row, scanline, registers[16] ($FF90-$FF9F), palette_reg[16],
 *          mmu_bank[16], SAM_register, TY, R1, MC0-MC3, MMUEN, TR, TINS,
 *          irq_state, firq_state
 *      mc6883_debug_ram_address / tcc1014_debug_decode  the address decodes
 *    The SAM and VDG are reached with
 *      part_component_by_id_is_a(&mach->part, "SAM", "SN74LS783")
 *      part_component_by_id_is_a(&mach->part, "VDG", "MC6847")
 *      part_component_by_id_is_a(&mach->part, "GIME", "TCC1014")
 *    mc6883_get_debug_state returns 0 when the SAM is not an MC6883.
 *
 * 4. Video taps
 *      machine_debug_video_tap_install(mach, &tap)   0 if no VDG/GIME
 *      machine_debug_video_tap_remove(mach)
 *      machine_debug_video_tap_active(mach)
 *    Chains `tap` behind the chip's render_line, signal_hs and signal_fs
 *    delegates (the original runs first, so the line is already in the video
 *    output when the tap sees it). The line carries the scanline number
 *    (the line that just finished), the tick, burst, npixels and a pixel
 *    pointer valid only during the call. hsync / fsync carry level, the
 *    chip's scanline counter and the tick. One render_line per scanline, 262
 *    (or 263 on a GIME) per 60 Hz frame; one fsync(1) per frame.
 *
 * 5. The partial frame (below): while the emulator is parked the video
 *    output holds the rows drawn so far; cocoxroar_partial_frame() hands out
 *    the buffer, how many rows are done and the raw scanline.
 */

#ifndef COCOXROAR_H
#define COCOXROAR_H

#include "top-config.h"
#include "delegate.h"
#include "cart.h"
#include "machine.h"
#include "mc6809/mc6809.h"
#include "mc6821.h"
#include "mc6847/mc6847.h"
#include "mc6883.h"
#include "part.h"
#include "ram.h"
#include "tcc1014/tcc1014.h"
#include "ui.h"
#include "vo.h"
#include "vo_render.h"
#include "xroar.h"

#include "machine_debug.h"

struct cocoxroar_partial_frame {
    const uint32_t *pixels;  /* 32-bit pixels, row 0 first, `pitch` per row */
    int width;               /* the viewport, i.e. the published frame's size */
    int height;
    int pitch;               /* pixels from one row to the next */
    int rows_done;           /* rows of this frame already drawn, 0..height */
    int scanline;            /* raw scanline counter of the video output */
    int first_line;          /* raw scanline that is row 0 of the frame */
};

/* Valid only while the emulator thread is parked (or on it). Returns 0 when
 * there is no video output yet. Rows at or past rows_done still hold the
 * previous frame. */
static inline int cocoxroar_partial_frame(struct cocoxroar_partial_frame *out)
{
    struct vo_interface *vo;
    struct vo_render *renderer;
    long done;

    if (!xroar.ui_interface)
        return 0;
    vo = xroar.ui_interface->vo_interface;
    if (!vo || !vo->renderer)
        return 0;
    renderer = vo->renderer;
    if (!renderer->buffer || renderer->buffer_pitch <= 0)
        return 0;
    done = ((const uint32_t *)renderer->pixel -
            (const uint32_t *)renderer->buffer) / renderer->buffer_pitch;
    if (done < 0)
        done = 0;
    if (done > renderer->viewport.h)
        done = renderer->viewport.h;
    out->pixels = (const uint32_t *)renderer->buffer;
    out->width = renderer->viewport.w;
    out->height = renderer->viewport.h;
    out->pitch = renderer->buffer_pitch;
    out->rows_done = (int)done;
    out->scanline = renderer->scanline;
    out->first_line = renderer->viewport.y;
    return 1;
}

#endif
