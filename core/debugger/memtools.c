/*
 * memtools -- see core/include/cocomemtools.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "cocomemtools.h"
#include "debugger_internal.h"
#include "expr.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- range parsing ------------------------------------------------------------ */

static int hexdigit(char ch)
{
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

static long space_mask(int space)
{
    return space == COCODEBUG_SPACE_PHYS ? 0x1FFFFFL : 0xFFFFL;
}

static int none_name(void *user, const char *name, long *out)
{
    (void)user;
    (void)name;
    (void)out;
    return 0;
}

static int eval_addr(cocodebug *d, int space, const char *text, size_t len, long *out)
{
    char buf[128], err[64];
    long value;
    uint16_t found;

    while (len && isspace((unsigned char)text[0])) { text++; len--; }
    while (len && isspace((unsigned char)text[len - 1])) len--;
    if (!len || len >= sizeof buf) return 0;
    memcpy(buf, text, len);
    buf[len] = '\0';
    /* A bare hex token is hex here (memory fields have always taken
     * "C000"), unless it names a symbol. */
    if (len <= (space == COCODEBUG_SPACE_PHYS ? 6u : 4u)) {
        size_t pos;
        for (pos = 0; pos < len && hexdigit(buf[pos]); pos++) ;
        if (pos == len && !(d && cocodebug_symbol_find(d, buf, &found))) {
            *out = strtol(buf, NULL, 16) & space_mask(space);
            return 1;
        }
    }
    if (d) {
        if (cocodebug_eval(d, buf, &value, err, sizeof err) != 0)
            return 0;
    } else {
        expr_ctx none = { none_name, NULL, NULL };
        if (expr_eval(buf, &none, &value, err, sizeof err) != 0)
            return 0;
    }
    *out = value & space_mask(space);
    return 1;
}

/* No operator characters: a number or a symbol. */
static int plain(const char *text, size_t len)
{
    size_t pos;
    for (pos = 0; pos < len; pos++)
        if (strchr("+-*/%&|^<>=!~()[]{}", text[pos])) return 0;
    return 1;
}

int cocosym_parse_range(cocodebug *d, int space, const char *text, uint32_t *lo,
                        uint32_t *hi)
{
    const char *dots, *scan;
    size_t len;
    long first, last;

    if (!text || !lo || !hi) return 0;
    while (isspace((unsigned char)*text)) text++;
    len = strlen(text);
    if (!len) return 0;

    dots = strstr(text, "..");
    if (dots) {
        if (!eval_addr(d, space, text, (size_t)(dots - text), &first) ||
            !eval_addr(d, space, dots + 2, strlen(dots + 2), &last))
            return 0;
    } else {
        int split = 0;
        for (scan = text + 1; *scan; scan++) {
            if (*scan != '-') continue;
            if (plain(text, (size_t)(scan - text)) && plain(scan + 1, strlen(scan + 1)) &&
                eval_addr(d, space, text, (size_t)(scan - text), &first) &&
                eval_addr(d, space, scan + 1, strlen(scan + 1), &last)) {
                split = 1;
                break;
            }
        }
        if (!split) {
            if (!eval_addr(d, space, text, len, &first)) return 0;
            last = first;
        }
    }
    if (first > last) { long swap = first; first = last; last = swap; }
    *lo = (uint32_t)first;
    *hi = (uint32_t)last;
    return 1;
}

/* ---- watches ------------------------------------------------------------------ */

static struct {
    pthread_mutex_t mtx;
    char text[COCOWATCH_MAX][96];
    int space[COCOWATCH_MAX];
    int count;
    unsigned generation;
} g_watch = { .mtx = PTHREAD_MUTEX_INITIALIZER };

int cocowatch_count(void)
{
    int count;
    pthread_mutex_lock(&g_watch.mtx);
    count = g_watch.count;
    pthread_mutex_unlock(&g_watch.mtx);
    return count;
}

int cocowatch_add(const char *text, int space)
{
    int index = -1;

    if (!text) return -1;
    while (isspace((unsigned char)*text)) text++;
    if (!*text) return -1;
    pthread_mutex_lock(&g_watch.mtx);
    if (g_watch.count < COCOWATCH_MAX) {
        size_t len;
        index = g_watch.count++;
        snprintf(g_watch.text[index], sizeof g_watch.text[index], "%s", text);
        g_watch.space[index] = space == COCODEBUG_SPACE_PHYS ? COCODEBUG_SPACE_PHYS
                                                              : COCODEBUG_SPACE_CPU;
        len = strlen(g_watch.text[index]);
        while (len && isspace((unsigned char)g_watch.text[index][len - 1]))
            g_watch.text[index][--len] = '\0';
        g_watch.generation++;
    }
    pthread_mutex_unlock(&g_watch.mtx);
    return index;
}

void cocowatch_remove(int index)
{
    pthread_mutex_lock(&g_watch.mtx);
    if (index >= 0 && index < g_watch.count) {
        int tail = g_watch.count - index - 1;
        memmove(g_watch.text[index], g_watch.text[index + 1],
                (size_t)tail * sizeof g_watch.text[0]);
        memmove(&g_watch.space[index], &g_watch.space[index + 1],
                (size_t)tail * sizeof g_watch.space[0]);
        g_watch.count--;
        g_watch.generation++;
    }
    pthread_mutex_unlock(&g_watch.mtx);
}

void cocowatch_clear(void)
{
    pthread_mutex_lock(&g_watch.mtx);
    g_watch.count = 0;
    g_watch.generation++;
    pthread_mutex_unlock(&g_watch.mtx);
}

/* Valid until the next change; the windows are single-threaded users. */
const char *cocowatch_text(int index)
{
    const char *text = NULL;
    pthread_mutex_lock(&g_watch.mtx);
    if (index >= 0 && index < g_watch.count) text = g_watch.text[index];
    pthread_mutex_unlock(&g_watch.mtx);
    return text;
}

int cocowatch_space(int index)
{
    int space = COCODEBUG_SPACE_CPU;
    pthread_mutex_lock(&g_watch.mtx);
    if (index >= 0 && index < g_watch.count) space = g_watch.space[index];
    pthread_mutex_unlock(&g_watch.mtx);
    return space;
}

unsigned cocowatch_generation(void)
{
    unsigned gen;
    pthread_mutex_lock(&g_watch.mtx);
    gen = g_watch.generation;
    pthread_mutex_unlock(&g_watch.mtx);
    return gen;
}

int cocowatch_range(cocodebug *d, int index, uint32_t *lo, uint32_t *hi)
{
    char text[96];
    const char *source = cocowatch_text(index);

    if (!source) return 0;
    snprintf(text, sizeof text, "%s", source);
    return cocosym_parse_range(d, cocowatch_space(index), text, lo, hi);
}

int cocowatch_format(cocodebug *d, int index, char *dst, int dstsz)
{
    char text[96];
    uint32_t lo, hi, addr;
    const char *source = cocowatch_text(index);
    int space = cocowatch_space(index), written = 0, pos;
    cocodebug_stop stop;
    const char *prefix = space == COCODEBUG_SPACE_PHYS ? "phys " : "";
    const int digits = space == COCODEBUG_SPACE_PHYS ? 6 : 4;

    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!source) return 0;
    snprintf(text, sizeof text, "%s", source);
    if (!cocosym_parse_range(d, space, text, &lo, &hi))
        return snprintf(dst, (size_t)dstsz, "%-14s ?", text);
    written = snprintf(dst, (size_t)dstsz, "%-14s %s$%0*X", text, prefix, digits, lo);
    if (written >= dstsz) return dstsz - 1;
    cocodebug_stop_snapshot(d, &stop);
    if (!stop.paused)
        return written + snprintf(dst + written, (size_t)(dstsz - written), "  -");
    if (lo == hi) {
        uint8_t value = 0;
        cocodebug_mem_read(d, space, lo, &value, 1);
        return written + snprintf(dst + written, (size_t)(dstsz - written),
                                  "  $%02X (%u)", value, value);
    }
    written += snprintf(dst + written, (size_t)(dstsz - written), "-$%0*X ", digits, hi);
    for (pos = 0; pos < 8 && lo + (uint32_t)pos <= hi && written < dstsz - 4; pos++) {
        uint8_t value = 0;
        addr = lo + (uint32_t)pos;
        cocodebug_mem_read(d, space, addr, &value, 1);
        written += snprintf(dst + written, (size_t)(dstsz - written), " %02X", value);
    }
    if (hi - lo >= 8 && written < dstsz - 4)
        written += snprintf(dst + written, (size_t)(dstsz - written), " ...");
    return written < dstsz ? written : dstsz - 1;
}

/* ---- search ------------------------------------------------------------------- */

static int hexval(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

int cocosearch_parse(int kind, const char *text, uint8_t *pat, int *n,
                     uint8_t *mask, int *flags)
{
    int count = 0;

    if (!text || !pat || !n || !mask || !flags) return 0;
    *mask = 0xFF;
    *flags = 0;
    if (kind == COCOSEARCH_TEXT) {
        const char *scan;
        for (scan = text; *scan && count < COCOSEARCH_MAX; scan++)
            pat[count++] = (uint8_t)*scan;
        *flags = COCODEBUG_SEARCH_NOCASE;
    } else {
        const char *scan = text;
        while (*scan) {
            int high, low;
            if (isspace((unsigned char)*scan) || *scan == ',') { scan++; continue; }
            if (*scan == '$') { scan++; continue; }
            if (scan[0] == '0' && (scan[1] == 'x' || scan[1] == 'X')) { scan += 2; continue; }
            high = hexval(*scan);
            if (high < 0) return 0;
            low = hexval(scan[1]);
            if (low < 0) { low = high; high = 0; scan += 1; }   /* a lone digit is a byte */
            else scan += 2;
            if (count >= COCOSEARCH_MAX) break;
            pat[count++] = (uint8_t)(high << 4 | low);
        }
    }
    *n = count;
    return count > 0;
}

/* ---- breakpoint description ----------------------------------------------------- */

int cocobp_describe(cocodebug *d, const cocodebug_breakpoint *bp, char *dst, int dstsz)
{
    static const struct { int bit; const char *name; } types[] = {
        { COCODEBUG_BP_EXEC, "Exec" }, { COCODEBUG_BP_READ, "Read" },
        { COCODEBUG_BP_WRITE, "Write" },
    };
    char kind[48] = "", label[48] = "";
    int written, pos;
    const int phys = (bp && (bp->type & COCODEBUG_BP_PHYS)) != 0;
    const int digits = phys ? 6 : 4;

    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!bp) return 0;
    for (pos = 0; pos < 3; pos++) {
        /* read + write together is one "Access" entry */
        if ((bp->type & COCODEBUG_BP_ACCESS) == COCODEBUG_BP_ACCESS) {
            if (types[pos].bit == COCODEBUG_BP_READ) {
                if (kind[0]) strncat(kind, "/", sizeof kind - strlen(kind) - 1);
                strncat(kind, "Access", sizeof kind - strlen(kind) - 1);
                continue;
            }
            if (types[pos].bit == COCODEBUG_BP_WRITE) continue;
        }
        if (bp->type & types[pos].bit) {
            if (kind[0]) strncat(kind, "/", sizeof kind - strlen(kind) - 1);
            strncat(kind, types[pos].name, sizeof kind - strlen(kind) - 1);
        }
    }
    /* a symbol only names a CPU address */
    if (!phys && d)
        cocodebug_symbol_at(d, (uint16_t)bp->start, label, sizeof label);
    if (bp->start == bp->end)
        written = snprintf(dst, (size_t)dstsz, "%s  %s$%0*X", kind, phys ? "phys " : "",
                           digits, bp->start);
    else
        written = snprintf(dst, (size_t)dstsz, "%s  %s$%0*X-$%0*X", kind, phys ? "phys " : "",
                           digits, bp->start, digits, bp->end);
    if (written < dstsz && label[0])
        written += snprintf(dst + written, (size_t)(dstsz - written), " (%s)", label);
    if (written < dstsz && bp->condition[0])
        written += snprintf(dst + written, (size_t)(dstsz - written), "  if %s", bp->condition);
    return written < dstsz ? written : dstsz - 1;
}

/* ---- memory map labels ---------------------------------------------------------- */

int cocomem_region_name(const cocodebug_region *map, int count, uint16_t addr,
                        char *dst, int dstsz)
{
    int pos;

    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    for (pos = 0; map && pos < count; pos++)
        if (addr >= map[pos].lo && addr <= map[pos].hi)
            return snprintf(dst, (size_t)dstsz, "%s", map[pos].name);
    return 0;
}

const char *cocomem_space_name(int space)
{
    return space == COCODEBUG_SPACE_PHYS ? "PHYS" : "CPU";
}
