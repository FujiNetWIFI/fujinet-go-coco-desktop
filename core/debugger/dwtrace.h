/*
 * dwtrace -- the Becker port's DriveWire bytes, as the exchanges the
 * debugger's FujiNet tab lists.
 *
 * DriveWire has no framing. A request is an opcode and whatever that opcode
 * reads; the reply is whatever FujiNet writes back. The fixed-length opcodes
 * (READEX, WRITE, TIME, DWINIT, ...) are cut by length, one entry per
 * request and its reply. FujiNet's own FUJI (E2) and NET (E3) carry no
 * length on the wire, so they end where the direction changes; a trailing
 * E2 00 (bus_ready) answered by one byte is split off as its own READY entry,
 * and a SEND RESPONSE is folded into the command it answers. Consecutive
 * identical exchanges collapse into one with a repeat count.
 *
 * Pure: no emulator, no locking. debugger.c feeds it under its trace mutex,
 * and dwtrace_test drives it with made-up bytes.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DWTRACE_H
#define DWTRACE_H

#include <stddef.h>
#include <stdint.h>

#include "cocodebug.h"

/* Exchanges kept; older ones fall off the front. */
#define DWTRACE_MAX 512

typedef struct dwtrace dwtrace;

dwtrace *dwtrace_create(void);
void dwtrace_destroy(dwtrace *t);

/* dir 1: CoCo -> FujiNet; dir 0: FujiNet -> CoCo. len 0 says the link
 * (re)connected: whatever was half-way through is closed as cut short. */
void dwtrace_feed(dwtrace *t, int dir, const uint8_t *buf, size_t len);

/* Empties the list. The counters and the exchange in progress stay. */
void dwtrace_clear(dwtrace *t);

/* The newest max entries, oldest first; the exchange still in progress, if
 * any, is the last. Returns how many. */
int dwtrace_get(const dwtrace *t, coco_fuji_packet *out, int max);

/* Bytes seen in direction dir, and exchanges finished (repeats counted),
 * since creation. */
uint64_t dwtrace_bytes(const dwtrace *t, int dir);
uint64_t dwtrace_exchanges(const dwtrace *t);

/* How far the exchange in progress has got, as one short line ("READEX,
 * 100 of 256 B in"); "idle" when there is none. */
void dwtrace_pending(const dwtrace *t, char *dst, int dstsz);

#endif /* DWTRACE_H */
