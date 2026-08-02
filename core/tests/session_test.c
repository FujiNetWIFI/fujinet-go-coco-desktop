/*
 * session_test -- the parts of the session API a frontend drives that do NOT
 * need the emulator running: media and ROM import routing, ROM detection, the
 * machine/TV/artifact enumerations, and the shared settings store.
 *
 * Deliberately separate from drivewire_smoke, which starts a real machine and
 * takes seconds. Everything here is file and string work, so it runs in
 * milliseconds and needs no ROMs, no FujiNet and no display.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "cocosession.h"

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

/* Create a file with recognisable contents, and return its path. */
static const char *make_file(const char *name)
{
    static char path[512];
    FILE *f;
    snprintf(path, sizeof(path), "session_test-in/%s", name);
    f = fopen(path, "wb");
    if (!f) return NULL;
    fwrite("fujinet-go-coco session_test fixture", 1, 36, f);
    fclose(f);
    return path;
}

static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* Does dest sit inside dir? Both are absolute by the time we see them. */
static int under(const char *dest, const char *dir)
{
    return dir && *dir && strncmp(dest, dir, strlen(dir)) == 0;
}

int main(void)
{
    cocosession_paths paths;
    cocosession *s;
    char dest[1024];
    const char *src;

#if defined(_WIN32)
    _mkdir("session_test-in");
#else
    mkdir("session_test-in", 0755);
#endif

    memset(&paths, 0, sizeof(paths));
    paths.config_dir = "session_test-config";
    paths.data_dir = "session_test-data";
    /* "" disables the runtime outright: this test must not start FujiNet, and
     * that is exactly the configuration in which the SD path used to come
     * back empty and disk import failed. */
    paths.fujinet_lib = "";

    s = cocosession_new(&paths);
    ok("a session can be created", s != NULL);
    if (!s) return 1;

    /* --- paths are usable before (and without) FujiNet ------------------ */
    ok("the SD path is known without starting FujiNet",
       cocosession_sd_path(s)[0] != '\0');
    ok("the ROM path is known", cocosession_roms_path(s)[0] != '\0');
    ok("the cartridge path is known", cocosession_carts_path(s)[0] != '\0');
    /* The chdir hazard: FujiNet's entry wrapper moves the CWD, so every path
     * the session hands out has to be absolute. */
    ok("the ROM path is absolute", cocosession_roms_path(s)[0] == '/' ||
                                   cocosession_roms_path(s)[1] == ':');
    ok("the SD path is absolute", cocosession_sd_path(s)[0] == '/' ||
                                  cocosession_sd_path(s)[1] == ':');

    /* --- media import routing ------------------------------------------- */
    src = make_file("game.dsk");
    ok("a .dsk imports",
       src && cocosession_import_media(s, src, dest, sizeof(dest)) == 0);
    ok("a .dsk lands in the FujiNet SD folder",
       under(dest, cocosession_sd_path(s)) && file_exists(dest));

    src = make_file("tape.cas");
    ok("a .cas imports",
       src && cocosession_import_media(s, src, dest, sizeof(dest)) == 0);
    ok("a .cas lands in the FujiNet SD folder",
       under(dest, cocosession_sd_path(s)));

    /* Cartridges are XRoar's business, not FujiNet's, so they go somewhere
     * else -- and must not be able to shadow a system ROM by basename. */
    src = make_file("game.ccc");
    ok("a .ccc imports",
       src && cocosession_import_media(s, src, dest, sizeof(dest)) == 0);
    ok("a .ccc lands in the cartridge folder",
       under(dest, cocosession_carts_path(s)) && file_exists(dest));

    src = make_file("cart.rom");
    ok("a .rom imports as a cartridge",
       src && cocosession_import_media(s, src, dest, sizeof(dest)) == 0);
    ok("a .rom lands in the cartridge folder, NOT the ROM folder",
       under(dest, cocosession_carts_path(s)));

    src = make_file("notes.txt");
    ok("an unsupported type is refused",
       src && cocosession_import_media(s, src, dest, sizeof(dest)) != 0);
    ok("...with an error that names the extension",
       strstr(cocosession_last_error(s), "txt") != NULL);

    /* --- system ROM import goes to the ROM path, by exact name ---------- */
    src = make_file("bas13.rom");
    ok("a system ROM imports",
       src && cocosession_import_rom(s, src, dest, sizeof(dest)) == 0);
    ok("a system ROM lands in the ROM folder",
       under(dest, cocosession_roms_path(s)));
    ok("...keeping its exact basename, which is how XRoar finds it",
       strstr(dest, "bas13.rom") != NULL);

    /* --- ROM detection per machine --------------------------------------
     * A WITH_COCO_ROMS=ON build has already materialised all five into the
     * ROM directory, so clear the two we are about to test for. Doing it
     * explicitly makes this deterministic in BOTH build configurations
     * rather than only the one that happens to be configured. */
    snprintf(dest, sizeof(dest), "%s/extbas11.rom", cocosession_roms_path(s));
    remove(dest);
    snprintf(dest, sizeof(dest), "%s/coco3.rom", cocosession_roms_path(s));
    remove(dest);

    /* bas13 alone is not enough for a CoCo 2: it also needs extbas11. */
    ok("a CoCo 2 needs Extended BASIC too",
       !cocosession_has_system_roms(s, COCOSESSION_MACHINE_COCO2));
    ok("a CoCo 3 needs its own ROM",
       !cocosession_has_system_roms(s, COCOSESSION_MACHINE_COCO3));
    src = make_file("extbas11.rom");
    cocosession_import_rom(s, src, dest, sizeof(dest));
    ok("a CoCo 2 boots with bas13 + extbas11",
       cocosession_has_system_roms(s, COCOSESSION_MACHINE_COCO2));
    /* The CoCo 3 wants its own ROM and does not care about the other two. */
    src = make_file("coco3.rom");
    cocosession_import_rom(s, src, dest, sizeof(dest));
    ok("a CoCo 3 boots with coco3.rom",
       cocosession_has_system_roms(s, COCOSESSION_MACHINE_COCO3));

    /* --- enumerations the frontends build their menus from -------------- */
    ok("the machine table ends", cocosession_machine_name(2) == NULL);
    ok("the TV input table ends", cocosession_tv_input_name(4) == NULL);
    ok("the artifact table ends", cocosession_ccr_name(5) == NULL);
    ok("the cpu table ends", cocosession_cpu_name(2) == NULL);
    ok("a CoCo 3 has an RGB connector",
       cocosession_tv_input_supported(COCOSESSION_MACHINE_COCO3,
                                      COCOSESSION_TV_RGB));
    ok("a CoCo 2 does not",
       !cocosession_tv_input_supported(COCOSESSION_MACHINE_COCO2,
                                       COCOSESSION_TV_RGB));
    ok("both have composite",
       cocosession_tv_input_supported(COCOSESSION_MACHINE_COCO2,
                                      COCOSESSION_TV_COMPOSITE_BR));
    ok("an out-of-range TV input is refused",
       !cocosession_tv_input_supported(COCOSESSION_MACHINE_COCO3, 99));

    /* --- live setters clamp, persist, and honour the machine ------------ */
    cocosession_set_machine(s, COCOSESSION_MACHINE_COCO3);
    cocosession_set_tv_input(s, COCOSESSION_TV_RGB);
    ok("RGB is accepted on a CoCo 3",
       cocosession_tv_input(s) == COCOSESSION_TV_RGB);
    cocosession_set_machine(s, COCOSESSION_MACHINE_COCO2);
    ok("switching to a CoCo 2 carries RGB to something it can produce",
       cocosession_tv_input(s) != COCOSESSION_TV_RGB);
    cocosession_set_tv_input(s, COCOSESSION_TV_RGB);
    ok("...and RGB cannot be selected there",
       cocosession_tv_input(s) != COCOSESSION_TV_RGB);
    cocosession_set_ccr(s, 99);
    ok("a nonsense artifact mode is clamped",
       cocosession_ccr_name(cocosession_ccr(s)) != NULL);

    /* CPU is independent of machine: unlike the TV input, nothing about it
     * gets carried or refused when the machine changes. */
    ok("the default cpu is 6809", cocosession_cpu(s) == COCOSESSION_CPU_MC6809);
    cocosession_set_cpu(s, COCOSESSION_CPU_HD6309);
    ok("6309 is accepted", cocosession_cpu(s) == COCOSESSION_CPU_HD6309);
    cocosession_set_machine(s, COCOSESSION_MACHINE_COCO3);
    ok("switching machine does not disturb the cpu selection",
       cocosession_cpu(s) == COCOSESSION_CPU_HD6309);
    /* Restore what the earlier RGB-carry checks left selected, so the
     * persistence check below (after reopening the session) still sees the
     * machine it expects. */
    cocosession_set_machine(s, COCOSESSION_MACHINE_COCO2);
    cocosession_set_cpu(s, 99);
    ok("a nonsense cpu is clamped",
       cocosession_cpu_name(cocosession_cpu(s)) != NULL);
    cocosession_set_cpu(s, COCOSESSION_CPU_MC6809);

    /* --- the shared settings store -------------------------------------- */
    cocosession_set_int(s, "session_test_int", 42);
    cocosession_set_str(s, "session_test_str", "hello");
    ok("an int round-trips", cocosession_get_int(s, "session_test_int", 0) == 42);
    ok("a string round-trips",
       strcmp(cocosession_get_str(s, "session_test_str", ""), "hello") == 0);
    ok("a missing key returns the default",
       cocosession_get_int(s, "session_test_absent", 7) == 7);
    cocosession_settings_flush(s);
    cocosession_free(s);

    /* Reopening is what the two frontends rely on to see each other's
     * configuration, so it has to survive the round trip to disk. */
    s = cocosession_new(&paths);
    ok("settings persist across sessions",
       s && cocosession_get_int(s, "session_test_int", 0) == 42);
    ok("...strings too",
       s && strcmp(cocosession_get_str(s, "session_test_str", ""), "hello") == 0);
    ok("...and so does the machine selection",
       s && cocosession_machine(s) == COCOSESSION_MACHINE_COCO2);
    ok("...and the cpu selection",
       s && cocosession_cpu(s) == COCOSESSION_CPU_MC6809);
    if (s) cocosession_free(s);

    if (failures) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    printf("\nall session tests passed\n");
    return 0;
}
