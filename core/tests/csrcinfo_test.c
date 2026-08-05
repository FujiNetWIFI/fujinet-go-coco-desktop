/*
 * csrcinfo_test -- the CMOC .lst/.map parser, standalone.
 *
 * No XRoar, no session: this is pure text parsing over a fixture that came
 * from a real `cmoc -i --decb hello.c` build (core/tests/data/hello.{c,lst,
 * map}), not hand-written, the same way symtab_test verifies against real
 * lwasm output rather than an assumed format. Every expected address below
 * was cross-checked by hand against the fixture: e.g. _main's entry is the
 * code section's link base (0x2E6D, from hello.map) plus its listing offset
 * (0x0010), landing on exactly the address hello.map's own "Symbol: _main"
 * line gives.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "csrcinfo.h"
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

static const cocosrc_var *find_var(const cocosrc_var *vars, int n,
                                   const char *name)
{
    int i;
    for (i = 0; i < n; i++)
        if (strcmp(vars[i].name, name) == 0)
            return &vars[i];
    return NULL;
}

int main(void)
{
    cocosrcinfo *t = cocosrcinfo_create();
    cocosymtab *sym = cocosymtab_create();
    char lst_path[512];
    int n, fid, line;
    uint16_t addr;
    const cocosrc_func *f;
    const cocosrc_var *vars;
    int nvars;
    const cocosrc_var *v;

    ok("a srcinfo table can be created", t != NULL && sym != NULL);
    if (!t || !sym) return 1;

    snprintf(lst_path, sizeof(lst_path), "%s/hello.lst", CSRCINFO_TEST_DATA_DIR);
    n = cocosrcinfo_load(t, lst_path, sym);
    ok("the fixture loads", n > 0);
    ok("the table is non-empty", !cocosrcinfo_empty(t));

    /* --- map symbols landed in the symbol table, at the addresses the map
     * itself resolved (section load base + listing offset) ---------------- */
    ok("_main resolves via the symbol table",
       cocosymtab_lookup_name(sym, "_main", &addr) && addr == 0x2E7D);
    ok("_add resolves via the symbol table",
       cocosymtab_lookup_name(sym, "_add", &addr) && addr == 0x2E6D);

    /* --- function extents -------------------------------------------------- */
    f = cocosrcinfo_func_at(t, 0x2E7D);
    ok("func_at finds main at its entry point", f != NULL);
    if (f) {
        ok("main's name is right", strcmp(f->name, "main") == 0);
        ok("main's lo is its entry", f->lo == 0x2E7D);
        /* add ends exactly where main begins -- the two functions are
         * contiguous in this build, verified against the fixture by hand. */
        ok("main's hi is one past its last instruction", f->hi == 0x2EAC);
        ok("main's body starts after the prologue", f->body_start == 0x2E83);
        ok("main's body_start is after its entry", f->body_start > f->lo);
    }

    f = cocosrcinfo_func_at(t, 0x2E6D);
    ok("func_at finds add at its entry point", f != NULL);
    if (f) {
        ok("add's name is right", strcmp(f->name, "add") == 0);
        ok("add's lo is its entry", f->lo == 0x2E6D);
        ok("add ends where main begins (contiguous, per the fixture)",
           f->hi == 0x2E7D);
    }

    ok("an address in neither function misses",
       cocosrcinfo_func_at(t, 0x0000) == NULL);

    /* --- line table: exact address, and the reverse lookup ---------------- */
    ok("main's first statement resolves to a (file, line)",
       cocosrcinfo_line_at(t, 0x2E83, &fid, &line));
    ok("...and it is hello.c:12", line == 12);
    ok("the entry point itself is NOT a statement boundary (still the "
       "caller's frame -- the prologue guard this all exists for)",
       !cocosrcinfo_is_stmt(t, 0x2E7D));
    ok("but body_start IS one", cocosrcinfo_is_stmt(t, 0x2E83));

    ok("hello.c:12 resolves back to an address",
       cocosrcinfo_addr_for_line(t, fid, 12, &addr) && addr == 0x2E83);
    ok("a line with no code misses",
       !cocosrcinfo_addr_for_line(t, fid, 999, &addr));

    /* --- frame layout: main's locals, all at negative U displacements ----- */
    f = cocosrcinfo_func_at(t, 0x2E83);
    vars = f ? cocosrcinfo_func_vars(t, f, &nvars) : NULL;
    ok("main has exactly 3 locals (x, y, z)", f && nvars == 3);
    if (vars) {
        v = find_var(vars, nvars, "x");
        ok("x is at -6,U, 2 bytes, type int, not a parameter",
           v && v->frame_off == -6 && v->size == 2 &&
           strcmp(v->type, "int") == 0 && !v->is_param);
        v = find_var(vars, nvars, "y");
        ok("y is at -4,U", v && v->frame_off == -4);
        v = find_var(vars, nvars, "z");
        ok("z is at -2,U, declared on line 14", v && v->frame_off == -2 &&
                                                v->decl_line == 14);
    }

    /* --- frame layout: add's parameters, at positive U displacements,
     * correctly distinguished from its one local -------------------------- */
    f = cocosrcinfo_func_at(t, 0x2E6D);
    vars = f ? cocosrcinfo_func_vars(t, f, &nvars) : NULL;
    ok("add has exactly 3 variables (a, b, c)", f && nvars == 3);
    if (vars) {
        v = find_var(vars, nvars, "a");
        ok("a is a parameter at 4,U",
           v && v->frame_off == 4 && v->is_param);
        v = find_var(vars, nvars, "b");
        ok("b is a parameter at 6,U", v && v->frame_off == 6 && v->is_param);
        v = find_var(vars, nvars, "c");
        ok("c is a local (not a parameter) at -2,U",
           v && v->frame_off == -2 && !v->is_param);
    }

    /* --- files -------------------------------------------------------------- */
    ok("exactly one source file was seen", cocosrcinfo_file_count(t) == 1);
    {
        const char *p = cocosrcinfo_file_path(t, 0);
        ok("its path ends in hello.c",
           p && strlen(p) >= 7 && strcmp(p + strlen(p) - 7, "hello.c") == 0);
        ok("file_id resolves the same path back",
           p && cocosrcinfo_file_id(t, p) == 0);
    }
    ok("an out-of-range file_id returns NULL",
       cocosrcinfo_file_path(t, 99) == NULL);

    /* --- a missing listing or map is reported, not silently empty --------- */
    ok("a nonexistent .lst returns -1",
       cocosrcinfo_load(t, "does-not-exist.lst", NULL) == -1);

    /* --- clear drops everything --------------------------------------------- */
    cocosrcinfo_clear(t);
    ok("the table is empty after clear", cocosrcinfo_empty(t));
    ok("a lookup after clear misses", !cocosrcinfo_is_stmt(t, 0x2E83));

    cocosrcinfo_destroy(t);
    cocosymtab_destroy(sym);

    /* --- multiple modules: loading accumulates rather than replaces -------
     * core/tests/data/multi/{main,module}.lst are a real `cmoc -i --decb
     * main.c module.c -o game.bin` build: two listings, but only ONE
     * game.map (cmoc names it after the linked executable, not either
     * source file), which is exactly the case cocosrcinfo_load's
     * directory-fallback exists for -- neither listing's own basename
     * guess ("main.map"/"module.map") exists next to it. -------------------- */
    {
        cocosrcinfo *mt = cocosrcinfo_create();
        cocosymtab *msym = cocosymtab_create();
        char main_lst[512], module_lst[512];
        int n1, n2;
        uint16_t maddr;
        const cocosrc_func *fm, *fs;

        snprintf(main_lst, sizeof(main_lst), "%s/multi/main.lst",
                CSRCINFO_TEST_DATA_DIR);
        snprintf(module_lst, sizeof(module_lst), "%s/multi/module.lst",
                CSRCINFO_TEST_DATA_DIR);

        n1 = cocosrcinfo_load(mt, main_lst, msym);
        ok("main.lst loads even though main.map doesn't exist (falls back "
           "to the directory's one game.map)", n1 > 0);

        n2 = cocosrcinfo_load(mt, module_lst, msym);
        ok("module.lst loading ADDS to main.lst's data rather than "
           "replacing it", n2 > n1);

        ok("_main resolves via the symbol table",
           cocosymtab_lookup_name(msym, "_main", &maddr) && maddr == 0x2E85);
        ok("_square resolves too (pushed in by the second, additive load)",
           cocosymtab_lookup_name(msym, "_square", &maddr) &&
           maddr == 0x2EAC);

        fm = cocosrcinfo_func_at(mt, 0x2E85);
        ok("main() is still found after module.lst was also loaded",
           fm && strcmp(fm->name, "main") == 0);

        fs = cocosrcinfo_func_at(mt, 0x2EAC);
        ok("square() is found, from the second load", fs != NULL);
        if (fs) {
            int nv;
            const cocosrc_var *vars, *v;

            ok("square's name is right", strcmp(fs->name, "square") == 0);
            ok("square's lo is its entry", fs->lo == 0x2EAC);
            ok("square's hi is one past its last instruction",
               fs->hi == 0x2EBF);

            vars = cocosrcinfo_func_vars(mt, fs, &nv);
            ok("square has 2 variables (n, r)", nv == 2);
            v = find_var(vars, nv, "n");
            ok("n is a parameter at 4,U", v && v->frame_off == 4 &&
                                          v->is_param);
            v = find_var(vars, nv, "r");
            ok("r is a local at -2,U", v && v->frame_off == -2 &&
                                       !v->is_param);
        }

        ok("module.c:6 (a line entry from the second load) resolves",
           cocosrcinfo_line_at(mt, 0x2EB2, NULL, NULL));
        ok("main.c:9 (a line entry from the first load) still resolves",
           cocosrcinfo_line_at(mt, 0x2E90, NULL, NULL));
        ok("two distinct source files are known", cocosrcinfo_file_count(mt) == 2);

        cocosrcinfo_destroy(mt);
        cocosymtab_destroy(msym);
    }

    if (failures) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    printf("\nall csrcinfo tests passed\n");
    return 0;
}
