/*
 * cocosrcinfo -- C source line/variable info parsed from a CMOC build.
 *
 * `cmoc -i` (--intermediate) keeps two files that together describe a
 * compiled program completely: the assembler listing (lwasm --list, CMOC's
 * .lst) carries "* Line file.c:N:" comments before the code for each C
 * statement, "* FUNCTION f(): defined at file.c:N" / "* END FUNCTION"
 * brackets around each function, and "* <disp>,U: <n> bytes: <name>: <type>:
 * line <n>" comments listing every parameter and local's stack frame slot.
 * The listing's addresses are section-relative (each SECTION restarts at
 * 0000); the sibling .map (lwlink --map, also only kept by -i) gives the
 * link-time base of each section, so addr = section_base + listing_offset.
 * Verified against a real build: _main resolved this way landed on exactly
 * the address the map's own "Symbol: _main ... = " line gives.
 *
 * Not installed; private to the debugger, like symtab.h. The public surface
 * a frontend sees is the cocodebug_source_* wrappers in cocodebug.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef COCO_CSRCINFO_H
#define COCO_CSRCINFO_H

#include <stdint.h>

#include "symtab.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cocosrcinfo cocosrcinfo;

/* One parameter or local variable, from a "* <disp>,U: ..." comment. */
typedef struct {
    char    name[32];
    char    type[32];   /* CMOC's own TypeDesc::toString(), verbatim */
    int16_t frame_off;  /* signed displacement from U; >=0 is a parameter */
    uint8_t size;
    int     decl_line;
    int     is_param;
} cocosrc_var;

/* One function, from a "* FUNCTION f(): defined at file:N" /
 * "* END FUNCTION" bracket pair. */
typedef struct {
    char     name[48];
    uint16_t lo, hi;       /* [lo, hi): lo is the entry point (prologue
                             * included), from "_name EQU *"; hi is one past
                             * the last instruction, from "funcend_name EQU
                             * *". */
    uint16_t body_start;   /* Address of the first C-statement boundary --
                             * the first address at which U is the callee's
                             * own frame rather than the caller's. Locals are
                             * only meaningful at or after this address. */
    int      file_id;
    int      decl_line;

    /* Internal bookkeeping for cocosrcinfo_func_vars(): an index range into
     * the table's flat variable array. Not meant to be read directly. */
    int      var_start, var_count;
} cocosrc_func;

cocosrcinfo *cocosrcinfo_create(void);
void cocosrcinfo_destroy(cocosrcinfo *t);

/* Parses a CMOC listing at lst_path and its sibling .map (same basename,
 * directory, extension ".map"). Function and data symbols found in the map
 * for this listing's own object file are also pushed into symtab via
 * cocosymtab_add(), if symtab is non-NULL, so the disassembly view picks up
 * C-level names for free. Replaces anything already loaded (clears first).
 * Returns the number of C-statement boundaries found, or -1 if the listing
 * or its .map could not be read. */
int cocosrcinfo_load(cocosrcinfo *t, const char *lst_path, cocosymtab *symtab);
void cocosrcinfo_clear(cocosrcinfo *t);
int cocosrcinfo_empty(const cocosrcinfo *t);

/* Exact-address lookup into the statement-boundary table. Returns 1 and
 * fills *file_id_out and *line_out on a hit. */
int cocosrcinfo_line_at(const cocosrcinfo *t, uint16_t addr, int *file_id_out,
                        int *line_out);
/* First statement-boundary address for file_id:line, if that line produced
 * any code (blank lines, declarations with no initializer, etc. may not
 * have one). Returns 1 and fills *addr_out on a hit. */
int cocosrcinfo_addr_for_line(const cocosrcinfo *t, int file_id, int line,
                              uint16_t *addr_out);
/* 1 when addr is exactly a statement boundary. What the stepping engine
 * calls once per instruction while a C-line step is in flight. */
int cocosrcinfo_is_stmt(const cocosrcinfo *t, uint16_t addr);

/* The function whose [lo, hi) contains addr, or NULL. */
const cocosrc_func *cocosrcinfo_func_at(const cocosrcinfo *t, uint16_t addr);
/* f's parameters and locals (declaration order). *n_out is set to how many;
 * the return value is NULL, and *n_out 0, when f has none. f must be a
 * pointer this table returned. */
const cocosrc_var *cocosrcinfo_func_vars(const cocosrcinfo *t,
                                         const cocosrc_func *f, int *n_out);

int cocosrcinfo_file_count(const cocosrcinfo *t);
/* Full path (resolved against the listing's own directory at load time). */
const char *cocosrcinfo_file_path(const cocosrcinfo *t, int file_id);
/* Reverse of the above, exact string match. Returns the file_id, or -1. */
int cocosrcinfo_file_id(const cocosrcinfo *t, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* COCO_CSRCINFO_H */
