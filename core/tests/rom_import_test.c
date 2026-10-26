/*
 * rom_import_test -- BIOS import from a file, a zip and a folder, without a
 * Tandy ROM: each image is filler of the real ROM's size whose last four
 * bytes are forged so its CRC32 is the real ROM's (CRC32 is linear, so four
 * bytes reach any value). zlib's own crc32() checks the forgery.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "cocosession.h"
#include "rom_forge.h"
#include "test_tmpdir.h"


static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while (0)

/* index-aligned with roms.c's file table */
static const struct { const char *name; uint32_t size, crc; } k_rom[] = {
    { "bas13.rom",    0x2000, 0xd8f4d15eu },
    { "bas12.rom",    0x2000, 0x54368805u },
    { "bas11.rom",    0x2000, 0x6270955au },
    { "bas10.rom",    0x2000, 0x00b50aaau },
    { "extbas11.rom", 0x2000, 0xa82a6254u },
    { "extbas10.rom", 0x2000, 0x6111a086u },
    { "coco3.rom",    0x8000, 0xb4c88d6cu },
};
#define N_ROM 7
#define MAX_SIZE 0x8000

static void write_file(const char *path, const uint8_t *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fwrite(data, 1, n, f);
    fclose(f);
}

static void make_dir(const char *path)
{
#if defined(_WIN32)
    mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

static void put16(FILE *f, unsigned v) { fputc(v & 0xff, f); fputc((v >> 8) & 0xff, f); }
static void put32(FILE *f, uint32_t v) { put16(f, v & 0xffff); put16(f, v >> 16); }

typedef struct { const char *name; const uint8_t *data; uint32_t size; int deflate; } zentry;

/* A zip of stored and raw-deflated entries, the way MAME's sets come. */
static void write_zip(const char *path, const zentry *e, int n)
{
    FILE *f = fopen(path, "wb");
    uint32_t off[8], csz[8], crc[8];
    uint8_t *comp[8];
    if (!f || n > 8) { perror(path); exit(1); }
    for (int i = 0; i < n; i++) {
        crc[i] = (uint32_t)crc32(0, e[i].data, e[i].size);
        if (e[i].deflate) {
            z_stream st;
            memset(&st, 0, sizeof st);
            comp[i] = malloc(e[i].size + 1024);
            deflateInit2(&st, 9, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
            st.next_in = (Bytef *)e[i].data;
            st.avail_in = e[i].size;
            st.next_out = comp[i];
            st.avail_out = e[i].size + 1024;
            deflate(&st, Z_FINISH);
            csz[i] = (uint32_t)st.total_out;
            deflateEnd(&st);
        } else {
            comp[i] = malloc(e[i].size);
            memcpy(comp[i], e[i].data, e[i].size);
            csz[i] = e[i].size;
        }
        off[i] = (uint32_t)ftell(f);
        put32(f, 0x04034b50); put16(f, 20); put16(f, 0); put16(f, e[i].deflate ? 8 : 0);
        put16(f, 0); put16(f, 0); put32(f, crc[i]); put32(f, csz[i]); put32(f, e[i].size);
        put16(f, (unsigned)strlen(e[i].name)); put16(f, 0);
        fputs(e[i].name, f);
        fwrite(comp[i], 1, csz[i], f);
    }
    uint32_t cd = (uint32_t)ftell(f);
    for (int i = 0; i < n; i++) {
        put32(f, 0x02014b50); put16(f, 20); put16(f, 20); put16(f, 0);
        put16(f, e[i].deflate ? 8 : 0); put16(f, 0); put16(f, 0);
        put32(f, crc[i]); put32(f, csz[i]); put32(f, e[i].size);
        put16(f, (unsigned)strlen(e[i].name)); put16(f, 0); put16(f, 0);
        put16(f, 0); put16(f, 0); put32(f, 0); put32(f, off[i]);
        fputs(e[i].name, f);
        free(comp[i]);
    }
    uint32_t cd_end = (uint32_t)ftell(f);
    put32(f, 0x06054b50); put16(f, 0); put16(f, 0); put16(f, (unsigned)n); put16(f, (unsigned)n);
    put32(f, cd_end - cd); put32(f, cd); put16(f, 0);
    fclose(f);
}

static cocosession *open_session(const char *dir, const char *name)
{
    char cfg[600], data[600];
    cocosession_paths paths;
    cocosession *s;
    snprintf(cfg, sizeof cfg, "%s/%s-config", dir, name);
    snprintf(data, sizeof data, "%s/%s-data", dir, name);
    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;
    paths.fujinet_lib = "";     /* no FujiNet: this is file work */
    s = cocosession_new(&paths);
    if (!s) { fprintf(stderr, "cocosession_new failed\n"); exit(1); }
    /* A WITH_COCO_ROMS=ON build lays its own Tandy ROMs down: clear them, so
     * the same expectations hold in every build configuration. */
    for (int i = 0; i < N_ROM; i++) {
        char p[1200];
        snprintf(p, sizeof p, "%s/%s", cocosession_roms_path(s), k_rom[i].name);
        remove(p);
    }
    return s;
}

/* The file the import wrote: the right name, size and CRC. */
static int stored_ok(cocosession *s, int rom)
{
    char path[1200];
    static uint8_t buf[MAX_SIZE + 1];
    snprintf(path, sizeof path, "%s/%s", cocosession_roms_path(s), k_rom[rom].name);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    return n == k_rom[rom].size && (uint32_t)crc32(0, buf, (uInt)n) == k_rom[rom].crc;
}

int main(void)
{
    static uint8_t rom[N_ROM][MAX_SIZE], junk[0x2000];
    char tmpl[512], path[1200], msg[512];
    crc_tables();

    test_tmpl(tmpl, sizeof tmpl, "rom_import_test");
    char *dir = mkdtemp(tmpl);
    if (!dir) { perror("mkdtemp"); return 1; }

    for (int i = 0; i < N_ROM; i++) {
        forge(rom[i], k_rom[i].size, 0x1234u + (uint32_t)i, k_rom[i].crc);
        CHECK((uint32_t)crc32(0, rom[i], k_rom[i].size) == k_rom[i].crc, "forged CRC");
    }
    forge(junk, 0x2000, 99, 0x12345678u);       /* 8 KB, but no ROM */

    cocosession *s = open_session(dir, "a");

    CHECK(cocosession_rom_item_count() == 3, "three items");
    CHECK(cocosession_rom_item(3) == NULL, "item past the end");
    CHECK(strcmp(cocosession_rom_item(2)->group, "CoCo 3") == 0, "group");
    CHECK(!cocosession_has_system_roms(s, COCOSESSION_MACHINE_COCO2), "no CoCo 2 BIOS yet");
    CHECK(!cocosession_has_system_roms(s, COCOSESSION_MACHINE_COCO3), "no CoCo 3 BIOS yet");

    /* not a ROM: nothing written, and the message says so */
    snprintf(path, sizeof path, "%s/junk.bin", dir);
    write_file(path, junk, 0x2000);
    CHECK(cocosession_rom_import(s, path, msg, sizeof msg) == -1, "junk rejected");
    CHECK(strncmp(msg, "No CoCo ROM was found in ", 25) == 0, msg);

    /* a short copy of a ROM is not one either */
    snprintf(path, sizeof path, "%s/short.bin", dir);
    write_file(path, rom[0], 0x2000 - 1);
    CHECK(cocosession_rom_import(s, path, msg, sizeof msg) == -1, "short file rejected");

    /* a single file, whatever it is called (XRoar finds only bas13.rom) */
    snprintf(path, sizeof path, "%s/COLOR BASIC.BIN", dir);
    write_file(path, rom[0], 0x2000);
    CHECK(cocosession_rom_import(s, path, msg, sizeof msg) == 0, "file imported");
    CHECK(stored_ok(s, 0), "bas13.rom stored");
    CHECK(strcmp(msg, "Now available: Color BASIC") == 0, msg);
    CHECK(cocosession_rom_item_available(s, 0), "item 0 available");
    CHECK(!cocosession_rom_item_available(s, 1), "item 1 still missing");
    CHECK(!cocosession_has_system_roms(s, COCOSESSION_MACHINE_COCO2),
          "a CoCo 2 needs Extended BASIC too");

    /* again: written, but nothing new */
    CHECK(cocosession_rom_import(s, path, msg, sizeof msg) == 0, "re-import");
    CHECK(strcmp(msg, "Imported 1 file; nothing new is complete yet") == 0, msg);

    /* importing a file that already is the stored ROM must not empty it */
    snprintf(path, sizeof path, "%s/bas13.rom", cocosession_roms_path(s));
    CHECK(cocosession_rom_import(s, path, msg, sizeof msg) == 0, "import from the ROM folder");
    CHECK(stored_ok(s, 0), "bas13.rom intact");

    /* a zip: stored and deflated entries, in a subfolder, beside junk */
    {
        zentry e[] = {
            { "extbas11.rom", rom[4], 0x2000, 0 },
            { "coco3/whatever.bin", rom[6], 0x8000, 1 },
            { "readme.txt", (const uint8_t *)"hello", 5, 1 },
            { "coco/junk.bin", junk, 0x2000, 1 },
        };
        snprintf(path, sizeof path, "%s/coco3.zip", dir);
        write_zip(path, e, 4);
        CHECK(cocosession_rom_import(s, path, msg, sizeof msg) == 0, "zip imported");
        CHECK(stored_ok(s, 4) && stored_ok(s, 6), "zip entries stored");
        CHECK(strcmp(msg, "Now available: Extended Color BASIC, "
                          "Super Extended Color BASIC (CoCo 3)") == 0, msg);
        for (int i = 0; i < 3; i++)
            CHECK(cocosession_rom_item_available(s, i), "all three available");
        CHECK(cocosession_has_system_roms(s, COCOSESSION_MACHINE_COCO2), "CoCo 2 boots");
        CHECK(cocosession_has_system_roms(s, COCOSESSION_MACHINE_COCO3), "CoCo 3 boots");
    }
    cocosession_free(s);

    /* a folder, nested, in a fresh session; an older BASIC counts too */
    s = open_session(dir, "b");
    {
        char sub[1200];
        snprintf(sub, sizeof sub, "%s/set", dir);
        make_dir(sub);
        snprintf(sub, sizeof sub, "%s/set/deep", dir);
        make_dir(sub);
        snprintf(path, sizeof path, "%s/set/deep/basic11", dir);
        write_file(path, rom[2], 0x2000);
        snprintf(path, sizeof path, "%s/set/other.bin", dir);
        write_file(path, junk, 0x2000);
        snprintf(path, sizeof path, "%s/set", dir);
        CHECK(cocosession_rom_import(s, path, msg, sizeof msg) == 0, "folder imported");
        CHECK(stored_ok(s, 2), "folder BIOS stored as bas11.rom");
        CHECK(strcmp(msg, "Now available: Color BASIC") == 0, msg);
        CHECK(!cocosession_rom_item_available(s, 2), "folder: no CoCo 3");
        /* an empty folder: nothing */
        snprintf(sub, sizeof sub, "%s/empty", dir);
        make_dir(sub);
        CHECK(cocosession_rom_import(s, sub, msg, sizeof msg) == -1, "empty folder");
    }
    cocosession_free(s);

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd)) { /* ignore */ }

    fprintf(stderr, "rom_import_test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
