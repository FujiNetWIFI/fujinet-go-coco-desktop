/*
 * restart_test -- stop and start the emulator several times in one process,
 * and import ROMs into a running machine. XRoar 1.11 is built to initialise
 * once; the patches in tools/xroar/patch-staged-tree.py make a second round
 * safe, and this is what holds them (run it under ASan: the fault they fix is
 * a realloc of freed memory).
 *
 * No Tandy ROM needed: when the build embeds none, same-sized filler stands in
 * (the machine runs garbage, which is fine for a lifecycle test), and the
 * import uses an image forged to Color BASIC's CRC32.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

#include "cocosession.h"
#include "rom_forge.h"

static int failures;

static void ok(const char *what, int cond)
{
    if (cond) {
        printf("ok   %s\n", what);
    } else {
        printf("FAIL %s\n", what);
        failures++;
    }
}

static void nap_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* Filler of the right size, unless the ROM is already there. */
static void ensure_rom(cocosession *s, const char *name, long size)
{
    char path[1024];
    FILE *f;
    snprintf(path, sizeof(path), "%s/%s", cocosession_roms_path(s), name);
    f = fopen(path, "rb");
    if (f) { fclose(f); return; }
    f = fopen(path, "wb");
    if (!f) return;
    while (size-- > 0) fputc(0xa5, f);
    fclose(f);
}

static int start(cocosession *s, int machine)
{
    cocosession_start_opts opts;
    cocosession_default_opts(s, &opts);
    opts.machine = machine;
    opts.enable_fujinet = 0;
    opts.enable_audio = 0;
    opts.enable_gamepad = 0;
    return cocosession_start(s, &opts);
}

int main(void)
{
    static uint8_t bas13[0x2000];
    cocosession_paths paths;
    cocosession *s;
    char msg[256];
    FILE *f;
    int i;

    memset(&paths, 0, sizeof(paths));
    paths.config_dir = "restart_test-config";
    paths.data_dir = "restart_test-data";
    paths.fujinet_lib = ""; /* no runtime: this test must not bind a port */

    s = cocosession_new(&paths);
    if (!s) {
        fprintf(stderr, "restart_test: could not create the session\n");
        return 1;
    }
    ensure_rom(s, "bas13.rom", 0x2000);
    ensure_rom(s, "extbas11.rom", 0x2000);
    ensure_rom(s, "coco3.rom", 0x8000);

    for (i = 0; i < 4; i++) {
        char what[64];
        snprintf(what, sizeof(what), "start %d of 4", i + 1);
        ok(what, start(s, i & 1 ? COCOSESSION_MACHINE_COCO3
                                : COCOSESSION_MACHINE_COCO2) == 0);
        nap_ms(150);
        ok("...and it is running", cocosession_is_running(s));
        cocosession_stop(s);
        ok("...and stops", !cocosession_is_running(s));
    }

    /* Import into a running machine: it is rebuilt in place, not restarted. */
    ok("starts again", start(s, COCOSESSION_MACHINE_COCO2) == 0);
    nap_ms(150);
    crc_tables();
    forge(bas13, 0x2000, 7, 0xd8f4d15eu);
    f = fopen("restart_test-bas13.bin", "wb");
    if (f) { fwrite(bas13, 1, sizeof(bas13), f); fclose(f); }
    ok("a ROM imports into a running machine",
       cocosession_rom_import(s, "restart_test-bas13.bin", msg, sizeof(msg)) == 0);
    nap_ms(300);
    ok("the machine runs on after the import", cocosession_is_running(s));
    cocosession_set_machine(s, COCOSESSION_MACHINE_COCO3);
    nap_ms(200);
    ok("and after a machine switch", cocosession_is_running(s));
    cocosession_stop(s);
    remove("restart_test-bas13.bin");
    cocosession_free(s);

    if (failures) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    printf("\nall restart tests passed\n");
    return 0;
}
