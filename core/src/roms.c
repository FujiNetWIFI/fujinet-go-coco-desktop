/*
 * roms.c -- CoCo system ROM import and availability.
 *
 * XRoar finds a ROM by basename in its -rompath, trying only "", ".rom" and
 * ".ROM" (bas13 -> bas13.rom), so a dump called BAS13.ROM or color.bin is
 * invisible to it. Import therefore recognises each known dump by size and
 * CRC32, whatever the file is called, and writes it under the name XRoar
 * looks for. The input can be a file, a zip (MAME's coco/coco3 sets work) or
 * a folder, as in fujinet-go-msdos-desktop and fujinet-go-astrocade-desktop.
 * See COMPLIANCE.md.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "coco_host.h"
#include "session_internal.h"
#include "zipread.h"

#define MAX_ROM_SIZE 0x8000u

/* What a machine needs, as the ROMs list shows it. An item is available when
 * any one of its dumps is in the ROM directory. */
static const coco_rom_item k_items[] = {
    { "Color BASIC", "CoCo 2" },
    { "Extended Color BASIC", "CoCo 2" },
    { "Super Extended Color BASIC (CoCo 3)", "CoCo 3" },
};
#define N_ITEMS ((int)(sizeof k_items / sizeof k_items[0]))

static const struct {
    const char *name;           /* the file in the ROM directory */
    uint32_t size;
    uint32_t crc32;
    int item;
} k_files[] = {
    { "bas13.rom",    0x2000, 0xd8f4d15eu, 0 },
    { "bas12.rom",    0x2000, 0x54368805u, 0 },
    { "bas11.rom",    0x2000, 0x6270955au, 0 },
    { "bas10.rom",    0x2000, 0x00b50aaau, 0 },
    { "extbas11.rom", 0x2000, 0xa82a6254u, 1 },
    { "extbas10.rom", 0x2000, 0x6111a086u, 1 },
    { "coco3.rom",    0x8000, 0xb4c88d6cu, 2 },
};
#define N_FILES ((int)(sizeof k_files / sizeof k_files[0]))

/* CRC32 (IEEE, the zip/MAME polynomial), table-free. */
static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xffffffffu;
    size_t i;
    int b;
    for (i = 0; i < n; i++) {
        crc ^= p[i];
        for (b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xedb88320u & (~(crc & 1) + 1));
    }
    return ~crc;
}

static int size_wanted(uint32_t size)
{
    int i;
    for (i = 0; i < N_FILES; i++)
        if (k_files[i].size == size) return 1;
    return 0;
}

static int file_present(const char *dir, int f)
{
    char path[COCO_PATH_MAX + 32];
    struct stat st;
    snprintf(path, sizeof(path), "%s/%s", dir, k_files[f].name);
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
           (uint32_t)st.st_size == k_files[f].size;
}

static int item_present(const char *dir, int item)
{
    int f;
    for (f = 0; f < N_FILES; f++)
        if (k_files[f].item == item && file_present(dir, f)) return 1;
    return 0;
}

/* The CoCo 2 boots from Color BASIC plus Extended Color BASIC, the CoCo 3
 * from its one 32K image. XRoar will start without them but comes up to a
 * blank screen, so the frontends ask first. Cartridge ROMs are deliberately
 * not counted: HDB-DOS is always present, and a missing one costs the FujiNet
 * link, not the boot. */
int roms_present_for_machine(const char *dir, int machine)
{
    if (machine == COCOSESSION_MACHINE_COCO2)
        return item_present(dir, 0) && item_present(dir, 1);
    return item_present(dir, 2);
}

/* ---- the ROMs list ---- */

int cocosession_rom_item_count(void)
{
    return N_ITEMS;
}

const coco_rom_item *cocosession_rom_item(int i)
{
    return i >= 0 && i < N_ITEMS ? &k_items[i] : NULL;
}

int cocosession_rom_item_available(const cocosession *s, int i)
{
    return s && i >= 0 && i < N_ITEMS && item_present(s->roms_dir, i);
}

/* ---- import: a file, a zip or a folder ---- */

typedef struct {
    cocosession *s;
    int imported;               /* files written */
    int got[N_FILES];
} import_ctx;

/* One candidate image: each known dump it matches is written under its own
 * name. Written through a temporary so a failed write leaves the old ROM. */
static void take(import_ctx *ctx, const uint8_t *data, uint32_t size)
{
    uint32_t crc;
    int i;

    if (!size_wanted(size)) return;
    crc = crc32_of(data, size);
    for (i = 0; i < N_FILES; i++) {
        char path[COCO_PATH_MAX + 32], tmp[COCO_PATH_MAX + 40];
        FILE *f;
        int ok;
        if (k_files[i].size != size || k_files[i].crc32 != crc) continue;
        mkdir_p(ctx->s->roms_dir);
        snprintf(path, sizeof(path), "%s/%s", ctx->s->roms_dir, k_files[i].name);
        snprintf(tmp, sizeof(tmp), "%s.part", path);
        f = fopen(tmp, "wb");
        if (!f) continue;
        ok = fwrite(data, 1, size, f) == size;
        if (fclose(f) != 0) ok = 0;
        if (ok) {
#if defined(_WIN32)
            remove(path); /* rename() will not replace an existing file here */
#endif
            ok = rename(tmp, path) == 0;
        }
        if (!ok) {
            remove(tmp);
            continue;
        }
        if (!ctx->got[i]) {
            ctx->got[i] = 1;
            ctx->imported++;
        }
    }
}

static uint8_t *read_whole(const char *path, uint32_t want)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    uint8_t extra;
    size_t n;

    if (!f) return NULL;
    buf = malloc(want);
    if (!buf) { fclose(f); return NULL; }
    n = fread(buf, 1, want, f);
    if (n != want || fread(&extra, 1, 1, f) == 1) { /* shorter or longer */
        free(buf);
        buf = NULL;
    }
    fclose(f);
    return buf;
}

static void import_path(import_ctx *ctx, const char *path, int depth);

static void import_zip(import_ctx *ctx, zipread *z)
{
    int i;
    for (i = 0; i < zipread_count(z); i++) {
        uint8_t *data = NULL;
        uint32_t size = 0;
        if (!size_wanted(zipread_size(z, i))) continue;
        if (zipread_extract(z, i, &data, &size, MAX_ROM_SIZE) == 0) {
            take(ctx, data, size);
            free(data);
        }
    }
}

static void import_dir(import_ctx *ctx, const char *dir, int depth)
{
    char path[COCO_PATH_MAX * 2];
    DIR *d = opendir(dir);
    struct dirent *e;

    if (!d) return;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        import_path(ctx, path, depth + 1);
    }
    closedir(d);
}

static void import_path(import_ctx *ctx, const char *path, int depth)
{
    struct stat st;
    zipread *z;

    if (depth > 8 || stat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        import_dir(ctx, path, depth);
        return;
    }
    if (!S_ISREG(st.st_mode)) return;
    z = zipread_open(path);
    if (z) {
        import_zip(ctx, z);
        zipread_close(z);
        return;
    }
    if (size_wanted((uint32_t)st.st_size)) {
        uint8_t *data = read_whole(path, (uint32_t)st.st_size);
        if (data) {
            take(ctx, data, (uint32_t)st.st_size);
            free(data);
        }
    }
}

int cocosession_rom_import(cocosession *s, const char *path, char *msg, int msgsz)
{
    import_ctx ctx;
    int before[N_ITEMS];
    int i, len = 0;

    if (!s) return -1;
    memset(&ctx, 0, sizeof(ctx));
    ctx.s = s;
    for (i = 0; i < N_ITEMS; i++)
        before[i] = item_present(s->roms_dir, i);
    if (path && *path)
        import_path(&ctx, path, 0);

    if (msg && msgsz > 0) {
        msg[0] = '\0';
        for (i = 0; i < N_ITEMS; i++) {
            if (!before[i] && item_present(s->roms_dir, i) && len < msgsz)
                len += snprintf(msg + len, (size_t)(msgsz - len), "%s%s",
                                len ? ", " : "Now available: ", k_items[i].name);
        }
        if (!len) {
            if (ctx.imported)
                snprintf(msg, (size_t)msgsz,
                         "Imported %d file%s; nothing new is complete yet",
                         ctx.imported, ctx.imported == 1 ? "" : "s");
            else
                snprintf(msg, (size_t)msgsz,
                         "No CoCo ROM was found in %s", path ? path : "");
        }
    }
    if (ctx.imported > 0) {
        /* A running machine reads its ROMs when it is built; rebuild it in
         * place. XRoar itself is not re-initialised. */
        if (s->running) cocohost_set_machine(s->cur_machine, s->cur_tv_input);
        return 0;
    }
    return -1;
}
