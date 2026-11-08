/*
 * cocohelp -- the app's Help: data/help/help.md, built in by
 * tools/help/make-help.py as topics of blocks of styled spans, which every
 * frontend lays out with its own toolkit.
 *
 * A frontend shows the blocks whose platforms include its own bit. Ids are
 * GitHub's heading slugs ("controllers-and-keys"); a Help button names a
 * context instead ("prefs/Machine", "debugger/cpu"), which stays put when a
 * heading is renamed. Everything here is constant data and reentrant.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef COCOHELP_H
#define COCOHELP_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    COCOHELP_GNOME = 1, COCOHELP_KDE = 2, COCOHELP_WINDOWS = 4, COCOHELP_MACOS = 8
};

typedef enum {
    COCOHELP_H2 = 0, COCOHELP_H3,
    COCOHELP_PARA,
    COCOHELP_BULLET, COCOHELP_BULLET2,    /* "- " and "  - " */
    COCOHELP_NUMBERED,                     /* a step; number is its own */
    COCOHELP_NOTE,
    COCOHELP_PRE,                          /* one TEXT span, lines as they are */
    COCOHELP_TABLE_HEAD,                   /* starts a table; its rows follow */
    COCOHELP_TABLE_ROW
} cocohelp_block_kind;

typedef enum {
    COCOHELP_TEXT = 0, COCOHELP_BOLD, COCOHELP_ITALIC, COCOHELP_CODE,
    COCOHELP_KBD,                          /* keys; mac_text is the Mac's */
    COCOHELP_LINK,                         /* target "#slug" or a URL */
    COCOHELP_CELL                          /* tables: the next column starts */
} cocohelp_span_kind;

typedef struct {
    int kind;
    const char *text;                       /* UTF-8 */
    const char *mac_text;                   /* KBD on macOS, or NULL: text */
    const char *target;
} cocohelp_span;

typedef struct {
    int kind;
    unsigned platforms;                     /* COCOHELP_* bits */
    int number;
    const char *id;                         /* headings: the slug */
    const char *context;                    /* headings: or NULL */
    const cocohelp_span *spans;            /* headings: one TEXT span */
    int nspans;
    /* Tables: the width of each column but the last, in ems, for frontends
     * that set tab stops (the last column wraps under itself). */
    const unsigned char *col_em;
    int ncol_em;
} cocohelp_block;

typedef struct {
    const char *id;
    const char *title;
    const char *context;                    /* or NULL */
    const cocohelp_block *blocks;
    int nblocks;
} cocohelp_topic;

int cocohelp_topic_count(void);
const cocohelp_topic *cocohelp_topic_at(int t);

/* "slug", "#slug" or a context: its topic, and the heading's block (-1 for
 * the topic itself). 0, or -1 if there is none. */
int cocohelp_find(const char *id, int *topic, int *block);

/* The span's text as platform shows it. */
const char *cocohelp_span_text(const cocohelp_span *sp, unsigned platform);

/* A heading block's text. */
const char *cocohelp_heading(const cocohelp_block *b);

/* The block after the section headed by block (the next heading at its
 * level or above, or the topic's end); -1: the whole topic. */
int cocohelp_section_end(int topic, int block);

/* The section headed by block (-1: from the topic's title) as text on
 * platform: a line per block, cells separated by " | ". Returns its length
 * (truncated to dstsz - 1). */
int cocohelp_plain_text(unsigned platform, int topic, int block, char *dst, int dstsz);

/* Sections that have every word of query (case-insensitive), best first:
 * where it is in a title, then how often. block -1 is a topic's start, up
 * to its first heading. Returns how many were written. */
typedef struct {
    int topic, block, score;
} cocohelp_hit;
int cocohelp_search(unsigned platform, const char *query, cocohelp_hit *out, int max);

/* The first match of a word of query in text at byte from or after: where
 * it starts and its length, for highlighting. 1, or 0 if there is none. */
int cocohelp_match(const char *text, const char *query, int from, int *start, int *len);

#ifdef __cplusplus
}
#endif

#endif /* COCOHELP_H */
