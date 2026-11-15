/*
 * vidtab_live_test -- the Video tab against a real machine (system ROMs
 * needed: skips with 77 without them). One machine per PROCESS, chosen by
 * argv[1] ("coco2" or "coco3"); ctest registers the binary once per machine.
 *
 * Running: the host captures a finished frame whose 192 display lines carry
 * the registers and bytes the chip fetched. Stopped, several times over:
 * Display is exactly the frame the session published, Decoded (rebuilt from
 * each line's own mode and bytes) agrees with the whole-screen decode the
 * debugger has always had, Beam is available, and Break on writes puts a
 * write breakpoint on the byte the pixel was fetched from. The machine runs on
 * afterwards.
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
#include "cocovideo.h"
#include "cocovidcap.h"
#include "cocovidtab.h"
#include "roms_embedded.h"

#define SKIP 77

static int failures;

static void check(int good, const char *what)
{
    printf("%s: %s\n", good ? "ok" : "FAIL", what);
    if (!good) failures++;
}

static void sleep_ms(int millis)
{
    struct timespec nap;
    nap.tv_sec = millis / 1000;
    nap.tv_nsec = (long)(millis % 1000) * 1000000L;
    nanosleep(&nap, NULL);
}

static int wait_state(cocodebug *dbg, int want_paused, int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited += 5) {
        cocodebug_stop stop_state;
        cocodebug_stop_snapshot(dbg, &stop_state);
        if (!!stop_state.paused == want_paused) return 1;
        sleep_ms(5);
    }
    return 0;
}


/* A real split screen: a 6809 program waits for the field sync, counts N
 * horizontal syncs, switches the VDG to RG6 (CSS 1) and switches back at the
 * next field sync, with interrupts masked so BASIC leaves the PIAs alone. The
 * capture must see the mode change on one line of the display, and the
 * published frame must change from text to graphics on that same line. */
static const uint8_t k_split_code[] = {
    0x1A, 0x50,                         /* 3000 ORCC #$50 */
    0xB6, 0xFF, 0x02,                   /* 3002 LDA $FF02 */
    0xB6, 0xFF, 0x03,                   /* 3005 w1: LDA $FF03 */
    0x2A, 0xFB,                         /* 3008 BPL w1 */
    0xB6, 0xFF, 0x02,                   /* 300A LDA $FF02 */
    0xB6, 0xFF, 0x00,                   /* 300D LDA $FF00 */
    0xC6, 0x00,                         /* 3010 LDB #N   (3011) */
    0xB6, 0xFF, 0x01,                   /* 3012 w2: LDA $FF01 */
    0x2A, 0xFB,                         /* 3015 BPL w2 */
    0xB6, 0xFF, 0x00,                   /* 3017 LDA $FF00 */
    0x5A,                               /* 301A DECB */
    0x26, 0xF5,                         /* 301B BNE w2 */
    0xB6, 0xFF, 0x22,                   /* 301D LDA $FF22 */
    0x84, 0x07,                         /* 3020 ANDA #7 */
    0x8A, 0xF8,                         /* 3022 ORA #$F8 */
    0xB7, 0xFF, 0x22,                   /* 3024 STA $FF22 */
    0xB6, 0xFF, 0x03,                   /* 3027 w3: LDA $FF03 */
    0x2A, 0xFB,                         /* 302A BPL w3 */
    0xB6, 0xFF, 0x02,                   /* 302C LDA $FF02 */
    0xB6, 0xFF, 0x22,                   /* 302F LDA $FF22 */
    0x84, 0x07,                         /* 3032 ANDA #7 */
    0xB7, 0xFF, 0x22,                   /* 3034 STA $FF22 */
    0x20, 0xD4                          /* 3037 BRA 300D */
};

static void test_real_split(cocosession *sess, cocodebug *dbg, cocovid_tab *tab)
{
    static const int counts[] = { 100, 130, 167, 200, 232 };
    uint8_t code[sizeof k_split_code];
    int found = 0;

    memcpy(code, k_split_code, sizeof code);
    for (size_t attempt = 0; attempt < sizeof counts / sizeof *counts && !found; attempt++) {
        const cocovid_line_row *rows;
        int nrows, split_row = -1, wide, high;
        const uint32_t *pic;

        cocodebug_set_engaged(dbg, 1);
        cocodebug_pause(dbg);
        wait_state(dbg, 1, 3000);
        code[0x11] = (uint8_t)counts[attempt];
        check(cocodebug_mem_write(dbg, COCODEBUG_SPACE_CPU, 0x3000, code, (int)sizeof code) == (int)sizeof code,
              "the split program is in RAM");
        check(cocodebug_regs_set(dbg, COCODEBUG_REG_PC, 0x3000), "PC set");
        cocodebug_resume(dbg);
        wait_state(dbg, 0, 3000);
        sleep_ms(700);                  /* many frames of the split */

        cocovid_tab_set_view(tab, COCOVID_V_DISPLAY);
        cocovid_tab_refresh(tab, 1);
        nrows = cocovid_tab_lines(tab, &rows);
        for (int i = 0; i < nrows; i++)
            if (rows[i].display && (rows[i].changed_mask & COCOVID_LC_MODE) && split_row < 0)
                split_row = rows[i].row;
        printf("  N=%d: mode change on display line %d\n", counts[attempt], split_row);
        if (split_row < 0)
            continue;
        found = 1;
        check(1, "a mode change inside the display shows in the Lines table");
        for (int i = 0; i < nrows; i++)
            if (rows[i].row == split_row)
                check(rows[i].changed_mask & COCOVID_LC_MID, "...flagged as changed during the line");
        {
            const cocovid_reg *regs;
            int nregs = cocovid_tab_regs(tab, &regs);
            check(nregs > 4, "registers while split");
        }
        /* the published frame changes from text to graphics on that line */
        pic = cocovid_tab_picture(tab, &wide, &high);
        if (pic) {
            uint64_t serial = 0;
            static cocovid_capture cap;
            int frame_split = -1;
            cocovid_host_capture_get(&cap, &serial);
            const int ref_row = split_row - 10 - cap.first_line;
            const uint32_t *ref = &cap.pixels[(size_t)ref_row * (size_t)cap.frame_w + (size_t)cap.act_x];
            for (int row = 0; ref_row >= 0 && row < cap.frame_h; row++) {
                const int vline = cap.first_line + row;
                const uint32_t *base = &cap.pixels[(size_t)row * (size_t)cap.frame_w + (size_t)cap.act_x];
                const int text = base[1] == ref[1] && base[3] == ref[1] && base[9] == ref[1];
                if (vline >= split_row - 4 && vline <= split_row + 4)
                    printf("   line %d: %06X %06X %06X %s\n", vline, base[1] & 0xffffff, base[3] & 0xffffff,
                           base[9] & 0xffffff, text ? "text" : "graphics");
                if (frame_split < 0 && !text && vline >= split_row - 6 && vline < split_row + 60) frame_split = vline;
            }
            printf("  published frame leaves text at video line %d (capture: %d)\n", frame_split, split_row);
            /* the program writes the mode a little after the line starts, so the
             * line itself is mixed: the capture marks it, the frame changes
             * on it or the next */
            check(frame_split >= 0 && frame_split - split_row >= 0 && frame_split - split_row <= 2,
                  "the published frame changes from text to graphics on the captured line or the next");
        }
        /* Decoded: text above, RG6 below, the mode named on each side */
        cocovid_tab_set_view(tab, COCOVID_V_DECODED);
        cocovid_tab_set_zoom(tab, 1);
        cocovid_tab_refresh(tab, 1);
        cocovid_tab_hover(tab, 20, 2);
        {
            const cocovid_pixel *top = cocovid_tab_pixel(tab);
            check(top && strstr(top->mode, "lpha"), "Decoded: the top of the screen is alpha");
        }
        cocovid_tab_hover(tab, 20, 190);
        {
            const cocovid_pixel *bottom = cocovid_tab_pixel(tab);
            check(bottom && strstr(bottom->mode, "RG"), "Decoded: the bottom of the screen is RG6");
        }
    }
    check(found, "a split was found for some line count");
    (void)sess;
}


/* The capture follows the machine through an in-place CoCo 2 <-> CoCo 3 switch
 * and through a stop and start: a new chip, a new set of taps. */
static void wait_frames(cocosession *sess, int count)
{
    const uint64_t start = cocosession_frame_count(sess);
    for (int waited = 0; waited < 12000 && cocosession_frame_count(sess) < start + (uint64_t)count; waited += 10)
        sleep_ms(10);
}

static int count_display(const cocovid_capture *cap)
{
    int count = 0;
    for (int vline = 0; vline < COCOVID_MAX_LINES; vline++)
        count += cap->lines[vline].valid && (cap->lines[vline].flags & COCOVID_LF_ACTIVE) && cap->lines[vline].fresh;
    return count;
}

static void test_machine_changes(cocosession *sess, cocovid_tab *tab, int coco3)
{
    static cocovid_capture cap;
    cocosession_start_opts opts;
    uint64_t serial = 0;

    cocosession_set_machine(sess, coco3 ? COCOSESSION_MACHINE_COCO2 : COCOSESSION_MACHINE_COCO3);
    wait_frames(sess, 200);
    cocovid_host_capture_get(&cap, &serial);
    printf("  after the switch: gime %d, %d fresh display lines\n", cap.gime, count_display(&cap));
    check(cap.gime == !coco3 && count_display(&cap) >= 192, "the capture follows an in-place machine switch");
    cocovid_tab_set_view(tab, COCOVID_V_DECODED);
    check((cocovid_tab_refresh(tab, 1) & COCOVID_TAB_PICTURE) != 0 && cocovid_tab_picture(tab, NULL, NULL) != NULL,
          "the tab draws the new machine");

    cocosession_stop(sess);
    cocosession_default_opts(sess, &opts);
    opts.enable_fujinet = 0;
    opts.enable_audio = 0;
    opts.enable_gamepad = 0;
    opts.machine = coco3 ? COCOSESSION_MACHINE_COCO3 : COCOSESSION_MACHINE_COCO2;
    check(cocosession_start(sess, &opts) == 0, "the session starts again");
    wait_frames(sess, 200);
    serial = 0;
    memset(&cap, 0, sizeof cap);
    cocovid_host_capture_get(&cap, &serial);
    printf("  after stop and start: gime %d, %d fresh display lines\n", cap.gime, count_display(&cap));
    check(cap.gime == coco3 && count_display(&cap) >= 192, "the capture follows a stop and start");
}

static uint32_t frame_copy[COCOSESSION_FB_MAX_WIDTH * COCOSESSION_FB_MAX_HEIGHT];
static uint32_t decode_copy[COCOVIDEO_MAX_WIDTH * COCOVIDEO_MAX_HEIGHT];
static cocovid_capture running_cap;

int main(int argc, char **argv)
{
    const int coco3 = argc > 1 && !strcmp(argv[1], "coco3");
    const char *config_dir = coco3 ? "vidtab_live-coco3-config" : "vidtab_live-coco2-config";
    const char *data_dir = coco3 ? "vidtab_live-coco3-data" : "vidtab_live-coco2-data";
    cocosession_paths paths;
    cocosession_start_opts opts;
    cocosession *sess;
    cocodebug *dbg;
    cocovid_tab *tab;
    const uint32_t *pic;
    int wide, high;

    if (!coco_embedded_system_roms) {
        printf("vidtab_live_test: no system ROMs in this build (WITH_COCO_ROMS=OFF); skipping\n");
        return SKIP;
    }
    memset(&paths, 0, sizeof paths);
    paths.config_dir = config_dir;
    paths.data_dir = data_dir;
    paths.fujinet_lib = "";
    sess = cocosession_new(&paths);
    if (!sess) return 1;
    cocosession_default_opts(sess, &opts);
    opts.enable_fujinet = 0;
    opts.enable_audio = 0;
    opts.enable_gamepad = 0;
    opts.machine = coco3 ? COCOSESSION_MACHINE_COCO3 : COCOSESSION_MACHINE_COCO2;
    if (cocosession_start(sess, &opts) != 0) {
        fprintf(stderr, "vidtab_live_test: session start failed: %s\n", cocosession_last_error(sess));
        return 1;
    }
    dbg = cocosession_debugger(sess);
    check(dbg != NULL, "the session hands out a debugger");
    tab = cocovid_tab_new(dbg);
    check(tab != NULL, "tab");
    {
        const uint64_t start = cocosession_frame_count(sess);
        for (int waited = 0; waited < 12000 && cocosession_frame_count(sess) < start + 150; waited += 10)
            sleep_ms(10);
    }

    /* running: a finished frame with display lines */
    cocovid_tab_set_zoom(tab, 1);
    cocovid_tab_set_overlays(tab, 0);
    check((cocovid_tab_refresh(tab, 1) & COCOVID_TAB_PICTURE) != 0, "refresh while running");
    pic = cocovid_tab_picture(tab, &wide, &high);
    check(pic && wide > 100 && high > 100, "Display (running) is the frame");
    {
        uint64_t serial = 0;
        int display = 0, fresh = 0, first = -1;
        check(cocovid_host_capture_get(&running_cap, &serial) == 1 && serial != 0, "the host captured a finished frame");
        check(cocovid_host_capture_get(&running_cap, &serial) == 0, "...and a second get with the same serial copies nothing");
        for (int vline = 0; vline < COCOVID_MAX_LINES; vline++) {
            const cocovid_line *rec = &running_cap.lines[vline];
            if (rec->valid && (rec->flags & COCOVID_LF_ACTIVE)) {
                display++;
                fresh += rec->fresh;
                if (first < 0) first = vline;
            }
        }
        printf("  %d display lines from %d, %d fresh, %d lines in the frame, gime %d\n", display, first, fresh,
               running_cap.nlines, running_cap.gime);
        check(running_cap.gime == coco3, "the capture knows the chip");
        check(display >= 192 && fresh >= 192, "192 or more display lines, all from this frame");
        check(running_cap.nlines >= 262 && running_cap.nlines <= 330, "a plausible frame length");
        check(running_cap.has_pixels && running_cap.frame_w == wide && running_cap.frame_h == high,
              "the capture carries the published frame");
    }
    {
        const cocovid_reg *regs;
        const cocovid_swatch *swat;
        char text[8192];
        check(cocovid_tab_regs(tab, &regs) >= 6, "registers");
        check(cocovid_tab_swatches(tab, &swat) == (coco3 ? 16 : 12), "colours");
        check(cocovid_tab_text(tab, COCOVID_S_TIMING, text, sizeof text) > 40 && strstr(text, "Line time"),
              "timing text with the line time");
        printf("%s", text);
        {
            const cocovid_line_row *rows;
            const int count = cocovid_tab_lines(tab, &rows);
            for (int i = 0; i < count; i++)
                if (rows[i].changed_mask)
                    printf("  line %d $%05X x%d mask %d  %s\n", rows[i].row, rows[i].addr, rows[i].nbytes,
                           rows[i].changed_mask, rows[i].mode);
        }
    }
    cocovid_tab_set_view(tab, COCOVID_V_BEAM);
    cocovid_tab_refresh(tab, 1);
    check(cocovid_tab_picture(tab, &wide, &high) == NULL && strlen(cocovid_tab_why(tab)) > 10,
          "Beam while running says why");
    cocovid_tab_set_view(tab, COCOVID_V_DISPLAY);

    /* stopped, several times over */
    for (int round = 0; round < 4; round++) {
        int diff = -1, tries, decode_diff = -1, dw = 0, dh = 0;
        uint64_t serial = 0;
        int fw = 0, fh = 0;

        cocodebug_set_engaged(dbg, 1);
        cocodebug_pause(dbg);
        check(wait_state(dbg, 1, 3000), "stopped");
        for (tries = 0; tries < 5 && diff != 0; tries++) {
            sleep_ms(30);
            cocovid_tab_set_view(tab, COCOVID_V_DISPLAY);
            cocovid_tab_refresh(tab, 1);
            pic = cocovid_tab_picture(tab, &wide, &high);
            serial = 0;
            cocosession_copy_frame(sess, frame_copy, &serial, &fw, &fh);
            diff = pic && wide == fw && high == fh ? 0 : -1;
            for (int pix = 0; diff == 0 && pix < wide * high; pix++)
                diff += pic[pix] != (frame_copy[pix] | 0xff000000u);
        }
        check(diff == 0, "stopped: Display is exactly the session's published frame");

        /* Decoded against the whole-screen decode */
        for (tries = 0; tries < 3 && decode_diff != 0; tries++) {
            int got;
            cocovid_tab_set_view(tab, COCOVID_V_DECODED);
            cocovid_tab_refresh(tab, 1);
            pic = cocovid_tab_picture(tab, &wide, &high);
            got = cocodebug_video_decode(dbg, decode_copy, &dw, &dh);
            decode_diff = pic && got && wide == dw && high == dh ? 0 : -1;
            for (int pix = 0; decode_diff >= 0 && pix < wide * high; pix++)
                decode_diff += pic[pix] != decode_copy[pix];
            if (decode_diff != 0) {
                cocodebug_resume(dbg);
                wait_state(dbg, 0, 3000);
                sleep_ms(80);
                cocodebug_pause(dbg);
                wait_state(dbg, 1, 3000);
                sleep_ms(30);
            }
        }
        printf("  Decoded %dx%d vs whole-screen %dx%d: %d differing pixels\n", wide, high, dw, dh, decode_diff);
        check(decode_diff == 0, "Decoded (per line) matches the whole-screen decode pixel for pixel");

        /* Beam, and the beam is somewhere */
        cocovid_tab_set_view(tab, COCOVID_V_BEAM);
        cocovid_tab_refresh(tab, 1);
        pic = cocovid_tab_picture(tab, &wide, &high);
        check(pic && wide == fw && high == fh, "Beam is available while stopped");
        check(cocovid_tab_beam_permille(tab) >= 0, "the beam is reported");

        /* Break on writes: the byte under a text pixel */
        if (round == 0) {
            cocodebug_breakpoint list[COCODEBUG_MAX_BREAKPOINTS];
            int count, found = 0, first, second;
            const cocovid_pixel *pixp;

            cocovid_tab_set_view(tab, COCOVID_V_DECODED);
            cocovid_tab_refresh(tab, 1);
            cocovid_tab_hover(tab, 40, 20);
            pixp = cocovid_tab_pixel(tab);
            check(pixp && pixp->has_byte && pixp->index == 5 && pixp->phys == coco3,
                  "hovering the Decoded text names a byte");
            if (pixp) printf("%s\n", pixp->text);
            check(cocovid_tab_can_break(tab), "that pixel can break");
            first = cocovid_tab_break_on_pixel(tab);
            second = cocovid_tab_break_on_pixel(tab);
            check(first >= 0 && first == second, "Break on writes adds one breakpoint, not two");
            count = cocodebug_breakpoints_list(dbg, list, COCODEBUG_MAX_BREAKPOINTS);
            for (int i = 0; pixp && i < count; i++)
                found |= list[i].id == first && (list[i].type & COCODEBUG_BP_WRITE) &&
                         list[i].start == pixp->addr && list[i].end == pixp->addr &&
                         !!(list[i].type & COCODEBUG_BP_PHYS) == !!pixp->phys;
            check(found, "...a write breakpoint on the byte's address");
            cocodebug_breakpoint_remove(dbg, first);
        }

        cocodebug_resume(dbg);
        check(wait_state(dbg, 0, 3000), "running again");
        sleep_ms(80);
    }
    {
        uint64_t before = 0, after = 0;
        cocovid_host_capture_get(&running_cap, &before);
        sleep_ms(300);
        cocovid_host_capture_get(&running_cap, &after);
        check(after != before, "frames are captured again after the stops");
    }

    test_real_split(sess, dbg, tab);
    test_machine_changes(sess, tab, coco3);

    cocodebug_set_engaged(dbg, 0);
    cocovid_tab_free(tab);
    cocosession_stop(sess);
    cocosession_free(sess);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
