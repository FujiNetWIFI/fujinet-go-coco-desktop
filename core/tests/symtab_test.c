/*
 * symtab_test -- the debugger's name<->address table, standalone.
 *
 * No XRoar, no session, no ROMs: this is pure data (the built-in Color BASIC
 * entry points) plus a text parser (LWASM's --symbol-dump format and the
 * plain "NAME ADDR" fallback), so it runs in milliseconds.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "symtab.h"

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

static const char *write_fixture(const char *name, const char *contents)
{
    static char path[512];
    FILE *f;
    snprintf(path, sizeof(path), "symtab_test-%s", name);
    f = fopen(path, "w");
    if (!f) return NULL;
    fputs(contents, f);
    fclose(f);
    return path;
}

int main(void)
{
    cocosymtab *t = cocosymtab_create();
    char name[64];
    uint16_t addr;
    const char *path;

    ok("a symtab can be created", t != NULL);
    if (!t) return 1;

    /* --- built-in Color BASIC entry points ------------------------------- */
    ok("POLCAT is built in", cocosymtab_lookup_addr(t, 0xA000, name, sizeof(name)) &&
                             strcmp(name, "POLCAT") == 0);
    ok("CHROUT is built in", cocosymtab_lookup_addr(t, 0xA002, name, sizeof(name)) &&
                             strcmp(name, "CHROUT") == 0);
    ok("an address with no symbol misses",
       !cocosymtab_lookup_addr(t, 0x1234, name, sizeof(name)));
    ok("name -> address works for a built-in",
       cocosymtab_lookup_name(t, "JOYIN", &addr) && addr == 0xA00A);
    ok("an unknown name misses", !cocosymtab_lookup_name(t, "NOSUCH", &addr));

    /* --- lwasm's real --symbol-dump format (verified against actual lwasm
     * output, not assumed) ------------------------------------------------ */
    path = write_fixture("lwasm.sym",
                         "COUNT EQU $300A\nLOOP EQU $3005\nSTART EQU $3000\n");
    ok("the lwasm fixture writes", path != NULL);
    if (path) {
        int n = cocosymtab_load(t, path);
        ok("all three lwasm symbols load", n == 3);
        ok("START resolves", cocosymtab_lookup_name(t, "START", &addr) &&
                             addr == 0x3000);
        ok("COUNT resolves", cocosymtab_lookup_addr(t, 0x300A, name, sizeof(name)) &&
                             strcmp(name, "COUNT") == 0);
    }

    /* --- the plain fallback format, plus comments and blank lines -------- */
    path = write_fixture("plain.sym",
                         "; a comment\n\nHELLO 0x4000\nWORLD 16384\nHEXY $C000\n");
    if (path) {
        int n = cocosymtab_load(t, path);
        ok("the plain-format fixture loads three symbols", n == 3);
        ok("a 0x-prefixed address parses",
           cocosymtab_lookup_name(t, "HELLO", &addr) && addr == 0x4000);
        ok("a decimal address parses",
           cocosymtab_lookup_name(t, "WORLD", &addr) && addr == 0x4000);
        ok("a $-prefixed address parses",
           cocosymtab_lookup_name(t, "HEXY", &addr) && addr == 0xC000);
    }

    /* --- a loaded symbol at the same address as a built-in wins ---------- */
    path = write_fixture("shadow.sym", "MYPOLCAT EQU $A000\n");
    if (path) {
        cocosymtab_load(t, path);
        ok("the most recently loaded symbol at an address wins the lookup",
           cocosymtab_lookup_addr(t, 0xA000, name, sizeof(name)) &&
           strcmp(name, "MYPOLCAT") == 0);
    }

    /* --- a missing file is reported, not silently empty ------------------ */
    ok("loading a nonexistent file returns -1",
       cocosymtab_load(t, "symtab_test-does-not-exist.sym") == -1);

    /* --- clearing drops loaded symbols but keeps the built-ins ----------- */
    cocosymtab_clear_user(t);
    ok("a loaded symbol is gone after clear",
       !cocosymtab_lookup_name(t, "START", &addr));
    ok("a built-in symbol survives clear",
       cocosymtab_lookup_addr(t, 0xA000, name, sizeof(name)) &&
       strcmp(name, "POLCAT") == 0);

    cocosymtab_destroy(t);

    if (failures) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    printf("\nall symtab tests passed\n");
    return 0;
}
