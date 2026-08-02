/*
 * cocosymtab -- name<->address table for the debugger's disassembly view.
 *
 * Not installed; shared only between debugger.c and symtab.c. The public
 * surface a frontend actually sees is the cocodebug_symbol_* wrappers in
 * cocodebug.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef COCO_SYMTAB_H
#define COCO_SYMTAB_H

#include <stdint.h>

typedef struct cocosymtab cocosymtab;

/* Seeds the built-in table (see symtab.c) and returns an empty-of-user-
 * symbols table ready to use. Never NULL except on out-of-memory. */
cocosymtab *cocosymtab_create(void);
void cocosymtab_destroy(cocosymtab *t);

/* Loads a symbol file on top of whatever is already there (built-in table
 * plus anything already loaded). Accepts LWASM's `--symbol-dump` format
 * ("NAME EQU $ADDR" per line, verified against actual lwasm output) and a
 * plain "NAME ADDR" fallback (hex with an optional $ or 0x prefix, or plain
 * decimal). ';' starts a comment; blank lines are skipped. Returns the
 * number of symbols added, or -1 if the file could not be opened. */
int cocosymtab_load(cocosymtab *t, const char *path);

/* Drops everything cocosymtab_load added; the built-in table is untouched. */
void cocosymtab_clear_user(cocosymtab *t);

/* Exact-address lookup. Returns 1 and copies the name (NUL-terminated,
 * truncated to dstsz) when addr has one, 0 otherwise. Symbols loaded later
 * win ties over earlier ones (a loaded file can shadow a built-in name at
 * the same address), so this scans newest-first. */
int cocosymtab_lookup_addr(const cocosymtab *t, uint16_t addr, char *dst,
                           int dstsz);

/* Name -> address, case-sensitive exact match (the same as lwasm's own
 * symbols). Returns 1 and fills *addr_out on a hit. */
int cocosymtab_lookup_name(const cocosymtab *t, const char *name,
                           uint16_t *addr_out);

#endif /* COCO_SYMTAB_H */
