/*
 * cocosymtab -- name<->address table for the debugger's disassembly view.
 *
 * XRoar ships nothing equivalent to AppleWin's .SYM loading, so this is two
 * sources merged into one flat table: a small built-in list of well-known
 * Color BASIC ROM entry points, and whatever a user loads on top from their
 * own assembled program.
 *
 * The built-in entries ($A000-$A00A) are Color BASIC's documented machine-
 * language entry points -- POLCAT/CHROUT/CSRDON/BLKIN/BLKOUT/JOYIN, per
 * Wikipedia's "Color BASIC" article's "Documented ROM subroutines" section
 * (cross-checked against computerarcheology.com/CoCo/Hardware.html, which
 * independently names the same six routines in the same $A000-$A00D range).
 * These are the routines Radio Shack itself documented for ML programmers to
 * call, so -- unlike most of a given ROM revision's internal layout -- they
 * are the addresses actually meant to stay stable across Color BASIC, and
 * are also reachable in the disk/HDB-DOS and CoCo 3 SECB configurations this
 * project boots, since none of those replace the low BASIC ROM bank. HDB-DOS
 * and Super ECB's own additional entry points are deliberately NOT included
 * here: no equally authoritative primary source (ROM disassembly or vendor
 * documentation, not a secondary summary) was in hand for those, and a wrong
 * address in a debugger is worse than a missing one.
 *
 * User symbols load in LWASM's own --symbol-dump format, verified directly
 * against a real `lwasm --symbol-dump=file.sym` run rather than assumed:
 * one "NAME EQU $ADDR" per line. A plainer "NAME ADDR" is also accepted
 * (hex with an optional $ or 0x prefix, or decimal) for hand-written files.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "symtab.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SYM_NAME_MAX 32

typedef struct {
    uint16_t addr;
    char name[SYM_NAME_MAX];
} sym_entry;

struct cocosymtab {
    sym_entry *entries;
    int count;
    int cap;
    int builtin_count; /* entries[0..builtin_count) survive clear_user */
};

static const struct { uint16_t addr; const char *name; } k_builtin[] = {
    { 0xA000, "POLCAT" },  /* poll keyboard for a character */
    { 0xA002, "CHROUT" },  /* output a character to screen or device */
    { 0xA004, "CSRDON" },  /* start cassette, prepare for reading */
    { 0xA006, "BLKIN" },   /* read a block from cassette */
    { 0xA008, "BLKOUT" },  /* write a block to cassette */
    { 0xA00A, "JOYIN" },   /* read joystick values */
};

static int ensure_cap(cocosymtab *t, int need)
{
    if (need <= t->cap)
        return 1;
    int newcap = t->cap ? t->cap * 2 : 16;
    sym_entry *e;
    while (newcap < need) newcap *= 2;
    e = realloc(t->entries, (size_t)newcap * sizeof(*e));
    if (!e) return 0;
    t->entries = e;
    t->cap = newcap;
    return 1;
}

static int append(cocosymtab *t, uint16_t addr, const char *name)
{
    if (!ensure_cap(t, t->count + 1))
        return 0;
    t->entries[t->count].addr = addr;
    snprintf(t->entries[t->count].name, SYM_NAME_MAX, "%s", name);
    t->count++;
    return 1;
}

cocosymtab *cocosymtab_create(void)
{
    cocosymtab *t = calloc(1, sizeof(*t));
    size_t i;
    if (!t) return NULL;
    for (i = 0; i < sizeof(k_builtin) / sizeof(k_builtin[0]); i++)
        append(t, k_builtin[i].addr, k_builtin[i].name);
    t->builtin_count = t->count;
    return t;
}

void cocosymtab_destroy(cocosymtab *t)
{
    if (!t) return;
    free(t->entries);
    free(t);
}

void cocosymtab_clear_user(cocosymtab *t)
{
    if (!t) return;
    t->count = t->builtin_count;
}

/* Parses one line into (name, addr). Accepts "NAME EQU $ADDR" (lwasm's
 * --symbol-dump) and "NAME ADDR" (hex with $/0x, or decimal). Returns 1 on
 * a successful parse. */
static int parse_line(const char *line, char *name_out, uint16_t *addr_out)
{
    char name[SYM_NAME_MAX];
    char rest[128];
    const char *p = line;
    int n;

    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0' || *p == ';' || *p == '\n' || *p == '\r')
        return 0;

    if (sscanf(p, "%31s%n", name, &n) != 1)
        return 0;
    p += n;
    while (*p == ' ' || *p == '\t') p++;

    /* Optional "EQU" keyword between the name and the address. */
    if ((p[0] == 'E' || p[0] == 'e') && (p[1] == 'Q' || p[1] == 'q') &&
        (p[2] == 'U' || p[2] == 'u') && (p[3] == '\0' || isspace((unsigned char)p[3]))) {
        p += 3;
        while (*p == ' ' || *p == '\t') p++;
    }

    if (sscanf(p, "%127s", rest) != 1)
        return 0;

    {
        const char *num = rest;
        int base = 10;
        char *end;
        unsigned long v;

        if (num[0] == '$') { num++; base = 16; }
        else if (num[0] == '0' && (num[1] == 'x' || num[1] == 'X')) { num += 2; base = 16; }

        v = strtoul(num, &end, base);
        if (end == num || *end != '\0')
            return 0;
        if (v > 0xFFFF)
            return 0;
        *addr_out = (uint16_t)v;
    }
    snprintf(name_out, SYM_NAME_MAX, "%s", name);
    return 1;
}

int cocosymtab_load(cocosymtab *t, const char *path)
{
    FILE *f = fopen(path, "r");
    char line[256];
    int loaded = 0;

    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) {
        char name[SYM_NAME_MAX];
        uint16_t addr;
        if (parse_line(line, name, &addr) && append(t, addr, name))
            loaded++;
    }
    fclose(f);
    return loaded;
}

int cocosymtab_lookup_addr(const cocosymtab *t, uint16_t addr, char *dst,
                           int dstsz)
{
    int i;
    if (!t) return 0;
    for (i = t->count - 1; i >= 0; i--) {
        if (t->entries[i].addr == addr) {
            snprintf(dst, (size_t)dstsz, "%s", t->entries[i].name);
            return 1;
        }
    }
    return 0;
}

int cocosymtab_lookup_name(const cocosymtab *t, const char *name,
                           uint16_t *addr_out)
{
    int i;
    if (!t) return 0;
    for (i = t->count - 1; i >= 0; i--) {
        if (strcmp(t->entries[i].name, name) == 0) {
            *addr_out = t->entries[i].addr;
            return 1;
        }
    }
    return 0;
}
