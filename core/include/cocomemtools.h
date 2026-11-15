/*
 * Watches, range parsing, memory-search queries and breakpoint descriptions,
 * kept in the core so the debugger windows only lay them out. Every call
 * that takes an address knows the two spaces of cocodebug.h: the CPU's 64K
 * and (CoCo 3) physical RAM.
 *
 * A watch is text the user typed (an address, a symbol, an expression, or a
 * range lo-hi / lo..hi) shown with its live value. It is stored as text and
 * resolved every time it is shown, so loading symbols later makes a symbol
 * start resolving.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef COCOMEMTOOLS_H
#define COCOMEMTOOLS_H

#include <stdint.h>

#include "cocodebug.h"

#ifdef __cplusplus
extern "C" {
#endif

/* "lo..hi", "lo-hi" (plain numbers or symbols on both sides), or one
 * expression (an address, symbol, symbol+3, [ptr]). A bare token of hex
 * digits ("0400"; up to 4 in the CPU space, 6 in PHYS) is hex unless it
 * names a symbol; "$0400" and expressions work as in the console. With d
 * NULL only numbers parse. Returns 1 and fills lo and hi (inclusive,
 * lo <= hi, clamped to the space), 0 if the text means nothing. */
int cocosym_parse_range(cocodebug *d, int space, const char *text, uint32_t *lo,
                        uint32_t *hi);

/* ---- watches ---- */
#define COCOWATCH_MAX 64
/* The process-wide list, like the symbol tables: it outlives the window. */
int  cocowatch_count(void);
/* Returns the new index, or -1 if empty / full. A watch that does not resolve
 * yet is kept (it shows "?"). `space` is COCODEBUG_SPACE_CPU or _PHYS. */
int  cocowatch_add(const char *text, int space);
void cocowatch_remove(int index);
void cocowatch_clear(void);
const char *cocowatch_text(int index);
int  cocowatch_space(int index);
unsigned cocowatch_generation(void);
/* "score  $0400  $42 (66)". While the machine runs the value is "-"; a range
 * shows its first 8 bytes. Returns the length. */
int  cocowatch_format(cocodebug *d, int index, char *dst, int dstsz);
/* The range a watch covers (for "Show in Memory"). 1 if it resolves. */
int  cocowatch_range(cocodebug *d, int index, uint32_t *lo, uint32_t *hi);

/* ---- memory search ---- */
enum { COCOSEARCH_HEX = 0, COCOSEARCH_TEXT };
#define COCOSEARCH_MAX 32
/* Hex: byte pairs, optional "$" / "0x" / spaces. Text: one byte per
 * character, case-insensitive. Fills pat, n, mask (0xFF) and flags for
 * cocodebug_search. Returns 1, or 0 for empty / malformed text. */
int cocosearch_parse(int kind, const char *text, uint8_t *pat, int *n,
                     uint8_t *mask, int *flags);

/* ---- breakpoints ---- */
/* "Write  $0400 (SCREEN)  if value != old"; a PHYS breakpoint reads
 * "Write  phys $01F400". */
int cocobp_describe(cocodebug *d, const cocodebug_breakpoint *bp, char *dst,
                    int dstsz);

/* ---- memory map ---- */
/* The name of the region of `map` (from cocodebug_memory_map) holding addr
 * ("RAM", "BASIC ROM", "PIA0"), for labelling Memory rows. Fetch the map once
 * per refresh and look rows up in it. Returns the length, 0 and "" when no
 * region covers addr. */
int cocomem_region_name(const cocodebug_region *map, int count, uint16_t addr,
                        char *dst, int dstsz);
/* "CPU" / "PHYS": the Memory tab's space selector labels. */
const char *cocomem_space_name(int space);

#ifdef __cplusplus
}
#endif

#endif /* COCOMEMTOOLS_H */
