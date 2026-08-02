/*
 * boot_smoke -- does the staged XRoar core actually come up and produce
 * frames? This is the test that makes a macOS or Windows CI job meaningful
 * when you cannot run those platforms yourself: it exercises the whole host
 * path (xroar_init, xroar_run, the vo module's draw delegate) without a
 * window, a toolkit or FujiNet.
 *
 * The ROMs come from the compiled-in table, written to a scratch directory
 * that becomes XRoar's -rompath -- so this also proves the materialisation
 * the session does on first start produces images XRoar accepts.
 *
 * Exits 77 (ctest SKIP_RETURN_CODE) when this build has no system ROMs
 * compiled in, so a WITH_COCO_ROMS=OFF configuration still passes.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(_WIN32)
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#define make_dir(p) mkdir((p), 0755)
#endif

#include "coco_host.h"
#include "cocosession.h"
#include "roms_embedded.h"

static int g_frames;
static int g_width;
static int g_height;
static unsigned g_nonblack;
static int g_rate_mhz;

static void on_frame(const uint32_t *px, int w, int h, void *user)
{
    (void)user;
    g_frames++;
    g_width = w;
    g_height = h;
    /* The CoCo boot screen is a green text field with dark glyphs; counting
     * non-black pixels distinguishes "the core rendered something" from "the
     * core handed us a cleared buffer". */
    for (int i = 0; i < w * h; i++) {
        if ((px[i] & 0x00FFFFFFu) != 0) g_nonblack++;
    }
}

static void on_rate(int rate_mhz, void *user)
{
    (void)user;
    g_rate_mhz = rate_mhz;
}

static void on_log(int level, const char *msg, void *user)
{
    (void)user;
    if (level >= 2) fprintf(stderr, "core: %s\n", msg);
}

/* Write the compiled-in ROMs into dir. Returns 0 on success. */
static int write_roms(const char *dir)
{
    char path[1024];
    int i;

    make_dir(dir); /* may already exist; the writes below are the real test */
    for (i = 0; i < coco_embedded_rom_count; i++) {
        const coco_embedded_rom *r = &coco_embedded_roms[i];
        FILE *f;
        snprintf(path, sizeof(path), "%s/%s", dir, r->name);
        f = fopen(path, "wb");
        if (!f) {
            fprintf(stderr, "boot_smoke: cannot write %s\n", path);
            return -1;
        }
        if (fwrite(r->data, 1, r->size, f) != r->size) {
            fprintf(stderr, "boot_smoke: short write to %s\n", path);
            fclose(f);
            return -1;
        }
        fclose(f);
    }
    return 0;
}

int main(void)
{
    /* Relative to the test's working directory, which ctest sets to the build
     * tree -- so a failed run leaves the ROMs somewhere inspectable and a
     * clean tree has none of them. */
    const char *romdir = "boot_smoke-roms";
    int rc = 0;

    if (!coco_embedded_system_roms) {
        printf("boot_smoke: no system ROMs in this build "
               "(WITH_COCO_ROMS=OFF); skipping\n");
        return 77;
    }
    if (write_roms(romdir) != 0)
        return 1;

    cocohost_set_log_sink(on_log, NULL);
    cocohost_set_frame_sink(on_frame, NULL);
    cocohost_set_frame_rate_sink(on_rate, NULL);
    cocohost_set_rompath(romdir);
    /* Point the Becker port at a port nothing listens on: the patched
     * becker.c connects lazily and retries, so a refused connection is the
     * normal "no DriveWire server" case and must not affect the boot. */
    cocohost_set_becker_endpoint("127.0.0.1", 1);

    if (!cocohost_core_start(COCOSESSION_MACHINE_COCO3,
                             COCOSESSION_TV_COMPOSITE_BR,
                             COCOSESSION_CCR_5BIT,
                             COCOSESSION_CPU_MC6809)) {
        fprintf(stderr, "boot_smoke: core failed to start\n");
        return 1;
    }

    /* ~2 seconds of emulated time: enough for the CoCo 3 to clear the screen
     * and print its banner. */
    for (int i = 0; i < 120; i++) cocohost_core_run_frame();

    printf("frames=%d geometry=%dx%d nonblack=%u rate=%d mHz\n",
           g_frames, g_width, g_height, g_nonblack, g_rate_mhz);

    if (g_frames == 0) {
        fprintf(stderr, "boot_smoke: no frames were produced\n");
        rc = 1;
    }
    if (g_width <= 0 || g_width > COCOSESSION_FB_MAX_WIDTH ||
        g_height <= 0 || g_height > COCOSESSION_FB_MAX_HEIGHT) {
        fprintf(stderr, "boot_smoke: geometry %dx%d outside 0..%dx%d\n",
                g_width, g_height, COCOSESSION_FB_MAX_WIDTH,
                COCOSESSION_FB_MAX_HEIGHT);
        rc = 1;
    }
    if (g_nonblack == 0) {
        fprintf(stderr, "boot_smoke: every frame was blank -- the machine did "
                        "not boot\n");
        rc = 1;
    }
    /* XRoar has to have told us the rate, or the session's pacer would never
     * learn a PAL machine is not 60Hz. */
    if (g_rate_mhz == 0) {
        fprintf(stderr, "boot_smoke: the core never reported a frame rate\n");
        rc = 1;
    }

    cocohost_core_stop();
    return rc;
}
