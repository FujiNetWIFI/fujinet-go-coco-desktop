/*
 * cocosrcinfo -- parses a CMOC listing (.lst) and its sibling linker map
 * (.map) into a line table, a function table with frame layouts, and the
 * global/function symbols the map itself resolved.
 *
 * The listing's line format was verified against a real `cmoc -i --decb`
 * build (cmoc 0.1.86/0.1.99, lwasm from lwtools 4.24) rather than assumed;
 * see the header for the shape being parsed. Two things make address
 * columns unambiguous without needing to track column position by hand:
 *
 *   - A line that actually emitted code/data has its 4-digit hex address
 *     at column 0 (e.g. "0006 EC44 ..."). A line that only defines a
 *     constant via EQU has its value at column 5 instead (e.g.
 *     "     0010 ... funcend_add EQU *"), five spaces in. No other kind of
 *     line has a hex run at either position, so testing both columns
 *     separates "real PC" from "compile-time constant" without needing to
 *     understand the mnemonic.
 *   - The source text (whether a "* ..." comment or an assembler
 *     statement) always follows the first "):" + digits + whitespace in
 *     the line -- that marker is lwasm's own "(sourcefile):linenumber"
 *     listing annotation, which the rest of this parser never has to
 *     reproduce, only skip past.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "csrcinfo.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint16_t addr;
    int      file_id;
    int      line;
} src_line_entry;

struct cocosrcinfo {
    char **files;
    int    nfiles, filecap;

    cocosrc_func *funcs;
    int           nfuncs, funccap;

    cocosrc_var *vars;
    int          nvars, varcap;

    src_line_entry *lines;
    int             nlines, linecap;
};

/* ---- tiny growable-array helper, shared by the four tables above -------- */

static void *grow(void *arr, int *cap, int need, size_t elemsz)
{
    int newcap;
    void *na;

    if (need <= *cap)
        return arr;
    newcap = *cap ? *cap * 2 : 16;
    while (newcap < need)
        newcap *= 2;
    na = realloc(arr, (size_t)newcap * elemsz);
    if (!na)
        return NULL;
    *cap = newcap;
    return na;
}

/* ---- lifecycle ------------------------------------------------------------ */

cocosrcinfo *cocosrcinfo_create(void)
{
    return (cocosrcinfo *)calloc(1, sizeof(cocosrcinfo));
}

void cocosrcinfo_clear(cocosrcinfo *t)
{
    int i;
    if (!t)
        return;
    for (i = 0; i < t->nfiles; i++)
        free(t->files[i]);
    free(t->files);
    free(t->funcs);
    free(t->vars);
    free(t->lines);
    t->files = NULL; t->nfiles = 0; t->filecap = 0;
    t->funcs = NULL; t->nfuncs = 0; t->funccap = 0;
    t->vars = NULL; t->nvars = 0; t->varcap = 0;
    t->lines = NULL; t->nlines = 0; t->linecap = 0;
}

void cocosrcinfo_destroy(cocosrcinfo *t)
{
    if (!t)
        return;
    cocosrcinfo_clear(t);
    free(t);
}

int cocosrcinfo_empty(const cocosrcinfo *t)
{
    return !t || t->nlines == 0;
}

/* ---- small string helpers -------------------------------------------------- */

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

static int hex4(const char *p)
{
    char buf[5];
    memcpy(buf, p, 4);
    buf[4] = '\0';
    return (int)strtoul(buf, NULL, 16);
}

static void split_path(const char *path, char *dir, size_t dirsz, char *base,
                       size_t basesz)
{
    const char *slash = strrchr(path, '/');
    if (slash) {
        size_t n = (size_t)(slash - path);
        if (n >= dirsz) n = dirsz - 1;
        memcpy(dir, path, n);
        dir[n] = '\0';
        snprintf(base, basesz, "%s", slash + 1);
    } else {
        snprintf(dir, dirsz, ".");
        snprintf(base, basesz, "%s", path);
    }
}

static void replace_ext(const char *base, const char *newext, char *out,
                        size_t outsz)
{
    const char *dot = strrchr(base, '.');
    size_t n = dot ? (size_t)(dot - base) : strlen(base);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, base, n);
    snprintf(out + n, outsz - n, "%s", newext);
}

static int looks_like_identifier(const char *s)
{
    if (!*s || !(isalpha((unsigned char)*s) || *s == '_'))
        return 0;
    for (s++; *s; s++)
        if (!(isalnum((unsigned char)*s) || *s == '_'))
            return 0;
    return 1;
}

/* ---- one physical listing line -------------------------------------------- */

/* kind: 0 no address on this line, 1 a real PC (code/data emitted),
 * 2 a compile-time constant from an EQU. *text_out points into `line`,
 * at the source text following lwasm's "(file):NNNNN" marker. */
static int parse_row(const char *line, size_t linelen, int *kind,
                     uint16_t *val, const char **text_out)
{
    const char *marker, *p;

    *kind = 0;
    *val = 0;
    if (linelen >= 5 &&
        isxdigit((unsigned char)line[0]) && isxdigit((unsigned char)line[1]) &&
        isxdigit((unsigned char)line[2]) && isxdigit((unsigned char)line[3]) &&
        line[4] == ' ') {
        *kind = 1;
        *val = (uint16_t)hex4(line);
    } else if (linelen >= 9 &&
              line[0] == ' ' && line[1] == ' ' && line[2] == ' ' &&
              line[3] == ' ' && line[4] == ' ' &&
              isxdigit((unsigned char)line[5]) && isxdigit((unsigned char)line[6]) &&
              isxdigit((unsigned char)line[7]) && isxdigit((unsigned char)line[8])) {
        *kind = 2;
        *val = (uint16_t)hex4(line + 5);
    }

    marker = strstr(line, "):");
    if (!marker) {
        *text_out = NULL;
        return 0;
    }
    p = marker + 2;
    while (isdigit((unsigned char)*p))
        p++;
    while (*p == ' ' || *p == '\t')
        p++;
    *text_out = p;
    return 1;
}

/* "SECTION <name>" */
static int try_parse_section(const char *text, char *name, size_t namesz)
{
    const char *p, *start;
    size_t n;

    if (strncmp(text, "SECTION", 7) != 0 || (text[7] != ' ' && text[7] != '\t'))
        return 0;
    p = skip_ws(text + 7);
    start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
        p++;
    n = (size_t)(p - start);
    if (n == 0 || n >= namesz)
        return 0;
    memcpy(name, start, n);
    name[n] = '\0';
    return 1;
}

/* "* Line <file>:<n>: ..." */
static int try_parse_line_comment(const char *text, char *file, size_t filesz,
                                  int *lineno)
{
    const char *p, *colon;
    char *endp;
    long ln;
    size_t n;

    if (strncmp(text, "* Line ", 7) != 0)
        return 0;
    p = text + 7;
    colon = strchr(p, ':');
    if (!colon)
        return 0;
    n = (size_t)(colon - p);
    if (n == 0 || n >= filesz)
        return 0;
    memcpy(file, p, n);
    file[n] = '\0';
    ln = strtol(colon + 1, &endp, 10);
    if (endp == colon + 1)
        return 0;
    *lineno = (int)ln;
    return 1;
}

/* "* FUNCTION <name>(): defined at <file>:<n>" */
static int try_parse_function_open(const char *text, char *name, size_t namesz,
                                   char *file, size_t filesz, int *lineno)
{
    const char *p, *paren, *marker, *colon;
    char *endp;
    long ln;
    size_t n;

    if (strncmp(text, "* FUNCTION ", 11) != 0)
        return 0;
    p = text + 11;
    paren = strchr(p, '(');
    if (!paren)
        return 0;
    n = (size_t)(paren - p);
    if (n == 0 || n >= namesz)
        return 0;
    memcpy(name, p, n);
    name[n] = '\0';

    marker = strstr(paren, "defined at ");
    if (!marker)
        return 0;
    p = marker + 11;
    colon = strchr(p, ':');
    if (!colon)
        return 0;
    n = (size_t)(colon - p);
    if (n == 0 || n >= filesz)
        return 0;
    memcpy(file, p, n);
    file[n] = '\0';
    ln = strtol(colon + 1, &endp, 10);
    if (endp == colon + 1)
        return 0;
    *lineno = (int)ln;
    return 1;
}

/* "<label> EQU *" -- a location equate, i.e. a symbol bound to a PC rather
 * than an arbitrary constant. Used for a function's entry (_name), its end
 * (funcend_name), and a handful of things this parser has no use for
 * (program_start, string_literals_start, ...); the caller only acts on it
 * when the label matches a function it already knows about. */
static int try_parse_equ_star(const char *text, char *label, size_t labelsz)
{
    const char *p, *start;
    size_t n;

    p = skip_ws(text);
    start = p;
    while (*p && *p != ' ' && *p != '\t')
        p++;
    n = (size_t)(p - start);
    if (n == 0 || n >= labelsz)
        return 0;
    memcpy(label, start, n);
    label[n] = '\0';

    p = skip_ws(p);
    if (strncmp(p, "EQU", 3) != 0)
        return 0;
    p = skip_ws(p + 3);
    return *p == '*';
}

/* "* <disp>,U: <n> byte(s): <name>: <type>: line <n>" */
static int try_parse_var(const char *text, int16_t *disp, uint8_t *size,
                         char *name, size_t namesz, char *type, size_t typesz,
                         int *lineno)
{
    const char *p, *colon;
    char *endp;
    long d, sz, ln;
    size_t n;

    if (text[0] != '*')
        return 0;
    p = skip_ws(text + 1);
    if (!(*p == '-' || isdigit((unsigned char)*p)))
        return 0;
    d = strtol(p, &endp, 10);
    if (endp == p)
        return 0;
    p = endp;
    if (strncmp(p, ",U:", 3) != 0)
        return 0;
    p = skip_ws(p + 3);
    sz = strtol(p, &endp, 10);
    if (endp == p)
        return 0;
    p = endp;

    /* Skip " byte" / " bytes" up to the colon that follows it. */
    colon = strchr(p, ':');
    if (!colon)
        return 0;
    p = colon + 1;

    colon = strchr(p, ':');
    if (!colon)
        return 0;
    n = (size_t)(colon - p);
    { /* trim */
        const char *s = skip_ws(p);
        const char *e = p + n;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
            e--;
        n = (size_t)(e - s);
        if (n >= namesz) n = namesz - 1;
        memcpy(name, s, n);
        name[n] = '\0';
    }
    p = colon + 1;

    colon = strchr(p, ':');
    if (!colon)
        return 0;
    n = (size_t)(colon - p);
    {
        const char *s = skip_ws(p);
        const char *e = p + n;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
            e--;
        n = (size_t)(e - s);
        if (n >= typesz) n = typesz - 1;
        memcpy(type, s, n);
        type[n] = '\0';
    }
    p = colon + 1;

    p = skip_ws(p);
    if (strncmp(p, "line", 4) != 0)
        return 0;
    p = skip_ws(p + 4);
    ln = strtol(p, &endp, 10);
    if (endp == p)
        return 0;

    *disp = (int16_t)d;
    *size = (uint8_t)sz;
    *lineno = (int)ln;
    return 1;
}

/* ---- the .map: section load bases and the object's own symbols ----------- */

#define MAX_MAP_SECTIONS 64

typedef struct {
    char     name[32];
    uint16_t base;
} section_base_entry;

static int match_section_line(const char *line, char *name, size_t namesz,
                              char *obj, size_t objsz, uint16_t *base)
{
    const char *p, *paren, *close, *loadat;
    char *endp;
    unsigned long v;
    size_t n;

    if (strncmp(line, "Section: ", 9) != 0)
        return 0;
    p = line + 9;
    paren = strchr(p, '(');
    if (!paren)
        return 0;
    n = (size_t)(paren - p);
    while (n > 0 && p[n - 1] == ' ')
        n--;
    if (n == 0 || n >= namesz)
        return 0;
    memcpy(name, p, n);
    name[n] = '\0';

    p = paren + 1;
    close = strchr(p, ')');
    if (!close)
        return 0;
    n = (size_t)(close - p);
    if (n >= objsz) n = objsz - 1;
    memcpy(obj, p, n);
    obj[n] = '\0';

    loadat = strstr(close, "load at ");
    if (!loadat)
        return 0;
    v = strtoul(loadat + 8, &endp, 16);
    if (endp == loadat + 8)
        return 0;
    *base = (uint16_t)v;
    return 1;
}

static int match_symbol_line(const char *line, char *name, size_t namesz,
                             char *obj, size_t objsz, uint16_t *val)
{
    const char *p, *paren, *close, *eq;
    char *endp;
    unsigned long v;
    size_t n;

    if (strncmp(line, "Symbol: ", 8) != 0)
        return 0;
    p = line + 8;
    paren = strchr(p, '(');
    if (!paren)
        return 0;
    n = (size_t)(paren - p);
    while (n > 0 && p[n - 1] == ' ')
        n--;
    if (n == 0 || n >= namesz)
        return 0;
    memcpy(name, p, n);
    name[n] = '\0';

    p = paren + 1;
    close = strchr(p, ')');
    if (!close)
        return 0;
    n = (size_t)(close - p);
    if (n >= objsz) n = objsz - 1;
    memcpy(obj, p, n);
    obj[n] = '\0';

    eq = strchr(close, '=');
    if (!eq)
        return 0;
    v = strtoul(eq + 1, &endp, 16);
    if (endp == eq + 1)
        return 0;
    *val = (uint16_t)v;
    return 1;
}

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

/* Falls back to this when the name derived from the .lst's own basename
 * doesn't exist. That guess only ever matches for a single-file build
 * (cmoc names the .map after the linked *executable*, not any one source
 * file -- a multi-module program's main.lst and module.lst both want the
 * one game.map, not a nonexistent main.map/module.map of their own). All
 * of a build's .lst files share a directory with that one .map, so if
 * exactly one *.map is sitting there, it has to be it. Returns 1 and fills
 * out on an unambiguous match, 0 if none or more than one was found (in
 * which case the caller's own basename guess, which will fail to open, is
 * left standing -- an ambiguous directory is a caller problem, not
 * something to guess through). */
static int find_map_fallback(const char *dir, char *out, size_t outsz)
{
    DIR *d = opendir(dir);
    struct dirent *ent;
    int nfound = 0;

    if (!d)
        return 0;
    while ((ent = readdir(d)) != NULL) {
        size_t n = strlen(ent->d_name);
        if (n > 4 && strcmp(ent->d_name + n - 4, ".map") == 0) {
            nfound++;
            if (nfound == 1)
                snprintf(out, outsz, "%s/%s", dir, ent->d_name);
        }
    }
    closedir(d);
    return nfound == 1;
}

static int load_map(const char *map_path, const char *target_obj,
                    section_base_entry *bases, int *nbases,
                    cocosymtab *symtab)
{
    FILE *f = fopen(map_path, "r");
    char line[512];

    if (!f)
        return -1;

    while (fgets(line, sizeof(line), f)) {
        char name[32], obj[64];
        uint16_t val;

        if (match_section_line(line, name, sizeof(name), obj, sizeof(obj),
                               &val)) {
            if (strcmp(obj, target_obj) == 0 && *nbases < MAX_MAP_SECTIONS) {
                snprintf(bases[*nbases].name, sizeof(bases[*nbases].name),
                        "%s", name);
                bases[*nbases].base = val;
                (*nbases)++;
            }
        } else if (match_symbol_line(line, name, sizeof(name), obj,
                                     sizeof(obj), &val)) {
            if (symtab && strcmp(obj, target_obj) == 0 &&
                looks_like_identifier(name))
                cocosymtab_add(symtab, val, name);
        }
    }
    fclose(f);
    return 0;
}

static int section_base_lookup(const section_base_entry *bases, int nbases,
                               const char *name, uint16_t *base_out)
{
    int i;
    for (i = 0; i < nbases; i++) {
        if (strcmp(bases[i].name, name) == 0) {
            *base_out = bases[i].base;
            return 1;
        }
    }
    return 0;
}

/* ---- table builders -------------------------------------------------------- */

static int intern_file(cocosrcinfo *t, const char *lst_dir, const char *name)
{
    char full[512];
    int i;

    if (name[0] == '/')
        snprintf(full, sizeof(full), "%s", name);
    else
        snprintf(full, sizeof(full), "%s/%s", lst_dir, name);

    for (i = 0; i < t->nfiles; i++)
        if (strcmp(t->files[i], full) == 0)
            return i;

    {
        void *na = grow(t->files, &t->filecap, t->nfiles + 1, sizeof(char *));
        char *copy;
        if (!na)
            return -1;
        t->files = (char **)na;
        copy = strdup(full);
        if (!copy)
            return -1;
        t->files[t->nfiles] = copy;
        return t->nfiles++;
    }
}

static cocosrc_func *func_open(cocosrcinfo *t, const char *name, int file_id,
                               int decl_line)
{
    void *na = grow(t->funcs, &t->funccap, t->nfuncs + 1, sizeof(cocosrc_func));
    cocosrc_func *f;
    if (!na)
        return NULL;
    t->funcs = (cocosrc_func *)na;
    f = &t->funcs[t->nfuncs++];
    memset(f, 0, sizeof(*f));
    snprintf(f->name, sizeof(f->name), "%s", name);
    f->file_id = file_id;
    f->decl_line = decl_line;
    f->var_start = t->nvars;
    f->var_count = 0;
    return f;
}

static cocosrc_func *func_find_open(cocosrcinfo *t, const char *label,
                                    const char *prefix)
{
    size_t plen = strlen(prefix);
    int i;

    if (strncmp(label, prefix, plen) != 0)
        return NULL;
    /* Search back-to-front: the function this label belongs to is always
     * the most recently opened one still missing that piece. */
    for (i = t->nfuncs - 1; i >= 0; i--)
        if (strcmp(t->funcs[i].name, label + plen) == 0)
            return &t->funcs[i];
    return NULL;
}

static int line_append(cocosrcinfo *t, uint16_t addr, int file_id, int line)
{
    void *na = grow(t->lines, &t->linecap, t->nlines + 1,
                    sizeof(src_line_entry));
    if (!na)
        return -1;
    t->lines = (src_line_entry *)na;
    t->lines[t->nlines].addr = addr;
    t->lines[t->nlines].file_id = file_id;
    t->lines[t->nlines].line = line;
    t->nlines++;
    return 0;
}

static int var_append(cocosrcinfo *t, cocosrc_func *f, int16_t disp,
                      uint8_t size, const char *name, const char *type,
                      int decl_line)
{
    void *na = grow(t->vars, &t->varcap, t->nvars + 1, sizeof(cocosrc_var));
    cocosrc_var *v;
    if (!na)
        return -1;
    t->vars = (cocosrc_var *)na;
    v = &t->vars[t->nvars++];
    snprintf(v->name, sizeof(v->name), "%s", name);
    snprintf(v->type, sizeof(v->type), "%s", type);
    v->frame_off = disp;
    v->size = size;
    v->decl_line = decl_line;
    v->is_param = disp >= 0;
    f->var_count++;
    return 0;
}

static int cmp_line_entry(const void *a, const void *b)
{
    uint16_t x = ((const src_line_entry *)a)->addr;
    uint16_t y = ((const src_line_entry *)b)->addr;
    return (x > y) - (x < y);
}

/* ---- the listing itself ---------------------------------------------------- */

int cocosrcinfo_load(cocosrcinfo *t, const char *lst_path, cocosymtab *symtab)
{
    char lst_dir[400], lst_base[128], obj_name[136], map_path[600];
    section_base_entry bases[MAX_MAP_SECTIONS];
    int nbases = 0;
    FILE *f;
    char line[1024];
    char current_section[32] = "";
    cocosrc_func *open_func = NULL;
    int have_pending = 0, pend_file = 0, pend_line = 0;

    if (!t || !lst_path)
        return -1;

    split_path(lst_path, lst_dir, sizeof(lst_dir), lst_base, sizeof(lst_base));
    replace_ext(lst_base, ".o", obj_name, sizeof(obj_name));
    {
        char map_base[136];
        replace_ext(lst_base, ".map", map_base, sizeof(map_base));
        snprintf(map_path, sizeof(map_path), "%s/%s", lst_dir, map_base);
    }
    if (!file_exists(map_path)) {
        /* Not a single-file build: fall back to the directory's one .map,
         * shared by every module linked into the same program. */
        char fallback[600];
        if (find_map_fallback(lst_dir, fallback, sizeof(fallback)))
            snprintf(map_path, sizeof(map_path), "%s", fallback);
    }

    if (load_map(map_path, obj_name, bases, &nbases, symtab) < 0)
        return -1;

    f = fopen(lst_path, "r");
    if (!f)
        return -1;

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        int kind;
        uint16_t val;
        const char *text;

        if (!parse_row(line, len, &kind, &val, &text))
            continue;

        if (text[0] == '*') {
            char file[128], name[48];
            int lineno;
            int16_t disp;
            uint8_t size;
            char vname[32], vtype[32];

            if (try_parse_line_comment(text, file, sizeof(file), &lineno)) {
                pend_file = intern_file(t, lst_dir, file);
                pend_line = lineno;
                have_pending = (pend_file >= 0);
            } else if (try_parse_function_open(text, name, sizeof(name), file,
                                               sizeof(file), &lineno)) {
                int fid = intern_file(t, lst_dir, file);
                if (fid >= 0)
                    open_func = func_open(t, name, fid, lineno);
            } else if (strncmp(text, "* END FUNCTION ", 15) == 0) {
                open_func = NULL;
            } else if (open_func &&
                      try_parse_var(text, &disp, &size, vname, sizeof(vname),
                                    vtype, sizeof(vtype), &lineno)) {
                var_append(t, open_func, disp, size, vname, vtype, lineno);
            }
            continue;
        }

        if (kind == 0) {
            char name[32];
            if (try_parse_section(text, name, sizeof(name)))
                snprintf(current_section, sizeof(current_section), "%s", name);
            continue;
        }

        if (kind == 2) {
            char label[48];
            cocosrc_func *f;
            uint16_t base;

            if (!try_parse_equ_star(text, label, sizeof(label)))
                continue;
            if (!section_base_lookup(bases, nbases, current_section, &base))
                continue;
            if ((f = func_find_open(t, label, "funcend_")) != NULL)
                f->hi = (uint16_t)(base + val);
            else if ((f = func_find_open(t, label, "_")) != NULL && f->lo == 0)
                f->lo = (uint16_t)(base + val);
            continue;
        }

        /* kind == 1: a real PC. */
        {
            uint16_t base, addr;
            if (!section_base_lookup(bases, nbases, current_section, &base))
                continue;
            addr = (uint16_t)(base + val);
            if (have_pending) {
                line_append(t, addr, pend_file, pend_line);
                have_pending = 0;
                if (open_func && open_func->body_start == 0)
                    open_func->body_start = addr;
            }
        }
    }
    fclose(f);

    qsort(t->lines, (size_t)t->nlines, sizeof(src_line_entry), cmp_line_entry);
    return t->nlines;
}

/* ---- queries ---------------------------------------------------------------- */

static int find_line_idx(const cocosrcinfo *t, uint16_t addr)
{
    int lo = 0, hi = t->nlines - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (t->lines[mid].addr == addr)
            return mid;
        if (t->lines[mid].addr < addr)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

int cocosrcinfo_line_at(const cocosrcinfo *t, uint16_t addr, int *file_id_out,
                        int *line_out)
{
    int idx;
    if (!t)
        return 0;
    idx = find_line_idx(t, addr);
    if (idx < 0)
        return 0;
    if (file_id_out) *file_id_out = t->lines[idx].file_id;
    if (line_out) *line_out = t->lines[idx].line;
    return 1;
}

int cocosrcinfo_is_stmt(const cocosrcinfo *t, uint16_t addr)
{
    return t ? find_line_idx(t, addr) >= 0 : 0;
}

int cocosrcinfo_addr_for_line(const cocosrcinfo *t, int file_id, int line,
                              uint16_t *addr_out)
{
    int i;
    if (!t)
        return 0;
    /* Table is small enough (one line entry per C statement in the whole
     * program) that a linear scan here is fine; only interactive callers
     * (setting a breakpoint by clicking a source line) reach this. */
    for (i = 0; i < t->nlines; i++) {
        if (t->lines[i].file_id == file_id && t->lines[i].line == line) {
            if (addr_out) *addr_out = t->lines[i].addr;
            return 1;
        }
    }
    return 0;
}

const cocosrc_func *cocosrcinfo_func_at(const cocosrcinfo *t, uint16_t addr)
{
    int i;
    if (!t)
        return NULL;
    for (i = 0; i < t->nfuncs; i++)
        if (addr >= t->funcs[i].lo && addr < t->funcs[i].hi)
            return &t->funcs[i];
    return NULL;
}

const cocosrc_var *cocosrcinfo_func_vars(const cocosrcinfo *t,
                                         const cocosrc_func *f, int *n_out)
{
    if (!t || !f) {
        if (n_out) *n_out = 0;
        return NULL;
    }
    if (n_out) *n_out = f->var_count;
    return f->var_count ? &t->vars[f->var_start] : NULL;
}

int cocosrcinfo_file_count(const cocosrcinfo *t)
{
    return t ? t->nfiles : 0;
}

const char *cocosrcinfo_file_path(const cocosrcinfo *t, int file_id)
{
    if (!t || file_id < 0 || file_id >= t->nfiles)
        return NULL;
    return t->files[file_id];
}

int cocosrcinfo_file_id(const cocosrcinfo *t, const char *path)
{
    int i;
    if (!t || !path)
        return -1;
    for (i = 0; i < t->nfiles; i++)
        if (strcmp(t->files[i], path) == 0)
            return i;
    return -1;
}
