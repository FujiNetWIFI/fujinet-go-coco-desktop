/*
 * drivewire_smoke -- does the whole thing actually work end to end?
 *
 * boot_smoke proves the emulator runs. This proves the two halves are
 * *joined*, and it checks two things the rest of the suite cannot:
 *
 *   1. DriveWire traffic. HDB-DOS DW3 auto-boots from drive 0, so FujiNet's
 *      own console log grows an "OP_READ: DRIVE 0 - SECTOR n" line within a
 *      second of the machine starting. That line can only appear if bytes
 *      crossed the loopback socket in BOTH directions -- the emulator asked
 *      for a sector and the runtime answered. A screen-changed check would
 *      pass on "?IO ERROR" too.
 *
 *   2. The keyboard. Typing at the machine and watching the framebuffer
 *      change is the only test that covers coco_key_from_event ->
 *      cocohost_inject_key -> hk_scan_press -> the emulated keyboard as one
 *      path. input_test covers the mapping alone, which is not the same
 *      thing: an early version of this file passed its mapping assertions
 *      while the machine was hung and receiving nothing.
 *
 * Skips (ctest 77) rather than failing when the pieces are not there: no
 * system ROMs (WITH_COCO_ROMS=OFF), no libfujinet (WITH_FUJINET=OFF), or the
 * Becker port already taken -- which is nearly always a standalone
 * fujinet-pc-coco left running, and is worth saying out loud because it is
 * the single most confusing way for this to look broken.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "cocosession.h"
#include "roms_embedded.h"

#define SKIP 77

static void nap_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* 1 when nothing else is listening on the Becker port. */
static int becker_port_free(void)
{
    int rc = 0;
#if defined(_WIN32)
    WSADATA wsa;
    SOCKET s;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return 1; /* cannot tell; let the run decide */
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s != INVALID_SOCKET) {
#else
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s >= 0) {
#endif
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(COCOSESSION_BECKER_PORT);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        rc = bind(s, (struct sockaddr *)&addr, sizeof(addr)) == 0;
#if defined(_WIN32)
        closesocket(s);
        WSACleanup();
#else
        close(s);
#endif
    }
    return rc;
}

static unsigned long long hash_frame(const uint32_t *px, int w, int h)
{
    unsigned long long v = 1469598103934665603ULL;
    for (int i = 0; i < w * h; i++) {
        v ^= px[i];
        v *= 1099511628211ULL;
    }
    return v;
}

/* Type a string on the emulated machine. XRoar's hkbd is positional and
 * holds a key until it is released, so each character is a press/release
 * pair with a gap the emulator thread can actually see -- it drains the
 * queue once per frame. */
static void type_line(cocosession *s, const char *text)
{
    for (const char *p = text; *p; p++) {
        cocosession_key(s, 1, (uint32_t)*p, (uint32_t)*p, 0, 0);
        nap_ms(40);
        cocosession_key(s, 0, (uint32_t)*p, (uint32_t)*p, 0, 0);
        nap_ms(40);
    }
    cocosession_key(s, 1, 0xFF0D, 0, 0, 0); /* Return */
    nap_ms(40);
    cocosession_key(s, 0, 0xFF0D, 0, 0, 0);
}

/* Type at the machine and report whether the screen reacted. */
static int screen_responds_to_typing(cocosession *s)
{
    uint32_t *fb = malloc((size_t)COCOSESSION_FB_MAX_WIDTH *
                          COCOSESSION_FB_MAX_HEIGHT * sizeof(uint32_t));
    uint64_t serial = 0;
    unsigned long long before = 0, after = 0;
    int w = 0, h = 0, i, ok = 0;

    if (!fb)
        return 0;

    /* Settle first: the boot is still painting, so a hash taken mid-scroll
     * would differ from the next one no matter what we type. */
    nap_ms(1500);
    if (cocosession_copy_frame(s, fb, &serial, &w, &h))
        before = hash_frame(fb, w, h);

    for (i = 0; i < 6; i++) {
        type_line(s, "PRINT");
        nap_ms(500);
        serial = 0; /* force a copy */
        if (cocosession_copy_frame(s, fb, &serial, &w, &h)) {
            after = hash_frame(fb, w, h);
            if (after != before) {
                ok = 1;
                break;
            }
        }
    }

    if (ok)
        printf("drivewire_smoke: typing reached the machine (the screen "
               "changed)\n");
    else
        fprintf(stderr, "drivewire_smoke: typing did not reach the machine -- "
                        "the screen never changed.\n");
    free(fb);
    return ok;
}

int main(void)
{
    cocosession_paths paths;
    cocosession_start_opts opts;
    cocosession *s;
    char *log;
    const int log_max = 256 * 1024;
    int rc = 0, i, found = 0;

    if (!coco_embedded_system_roms) {
        printf("drivewire_smoke: no system ROMs in this build "
               "(WITH_COCO_ROMS=OFF); skipping\n");
        return SKIP;
    }
    if (!becker_port_free()) {
        printf("drivewire_smoke: something is already listening on port %d.\n"
               "This is almost always a standalone fujinet-pc-coco; stop it "
               "and re-run.\n  ss -tlnp | grep %d\nSkipping.\n",
               COCOSESSION_BECKER_PORT, COCOSESSION_BECKER_PORT);
        return SKIP;
    }

    /* Keep the test out of the user's real config and data directories: it
     * provisions a FujiNet runtime tree and materialises ROMs, and neither
     * belongs in ~/.local/share during a build. Relative to the working
     * directory, which ctest sets to the build tree. */
    memset(&paths, 0, sizeof(paths));
    paths.config_dir = "drivewire_smoke-config";
    paths.data_dir = "drivewire_smoke-data";

    s = cocosession_new(&paths);
    if (!s) {
        fprintf(stderr, "drivewire_smoke: could not create the session\n");
        return 1;
    }

    cocosession_default_opts(s, &opts);
    opts.machine = COCOSESSION_MACHINE_COCO3;
    opts.enable_fujinet = 1;
    opts.enable_audio = 0;   /* no audio device in CI */
    opts.enable_gamepad = 0; /* no input devices in CI */
    if (cocosession_start(s, &opts) != 0) {
        fprintf(stderr, "drivewire_smoke: session start failed: %s\n",
                cocosession_last_error(s));
        cocosession_free(s);
        return 1;
    }

    if (!cocosession_fujinet_running(s)) {
        printf("drivewire_smoke: the FujiNet runtime is not available "
               "(WITH_FUJINET=OFF?); skipping\n");
        cocosession_stop(s);
        cocosession_free(s);
        return SKIP;
    }

    /* --- 1. DriveWire ---------------------------------------------------
     * HDB-DOS DW3 boots from drive 0 by itself; nothing has to be typed. */
    log = malloc((size_t)log_max);
    if (!log) {
        cocosession_stop(s);
        cocosession_free(s);
        return 1;
    }
    for (i = 0; i < 150 && !found; i++) {
        nap_ms(100);
        /* OP_READ matches OP_READEX too, which is the named-object boot
         * path; either one means the link is carrying data. */
        if (cocosession_fujinet_copy_log(s, log, log_max) > 0 &&
            strstr(log, "OP_READ") != NULL)
            found = 1;
    }

    if (found) {
        printf("drivewire_smoke: HDB-DOS read a sector over the Becker port "
               "from the in-process runtime\n");
    } else {
        fprintf(stderr,
                "drivewire_smoke: no OP_READ in FujiNet's log -- the machine "
                "and the runtime are not talking.\nFujiNet's log follows:\n"
                "%s\n", log);
        rc = 1;
    }
    free(log);

    /* --- 2. the keyboard ------------------------------------------------ */
    if (rc == 0 && !screen_responds_to_typing(s))
        rc = 1;
    cocosession_stop(s);
    cocosession_free(s);
    return rc;
}
