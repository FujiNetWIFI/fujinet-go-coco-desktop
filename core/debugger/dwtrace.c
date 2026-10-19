/*
 * dwtrace -- DriveWire bytes into the FujiNet tab's entries. See dwtrace.h.
 *
 * Every exchange is a run of segments that alternate direction, starting
 * with the CoCo: READEX is 5 bytes out, 256 back, 2 out (the checksum), 1
 * back (the status). segment() knows the lengths per opcode; FUJI, NET and
 * anything unknown are one variable segment each way.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dwtrace.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fuji_names.h"

/* DriveWire opcodes, from fujinet-firmware lib/bus/drivewire/opcode.h. */
enum {
    OP_NOP = 0x00, OP_NAMEOBJ_MNT = 0x01, OP_TIME = 0x23, OP_SERREAD = 0x43,
    OP_SERGETSTAT = 0x44, OP_SERINIT = 0x45, OP_PRINTFLUSH = 0x46,
    OP_GETSTAT = 0x47, OP_INIT = 0x49, OP_PRINT = 0x50, OP_READ = 0x52,
    OP_SETSTAT = 0x53, OP_TERM = 0x54, OP_WRITE = 0x57, OP_DWINIT = 0x5A,
    OP_SERREADM = 0x63, OP_SERWRITEM = 0x64, OP_REREAD = 0x72,
    OP_REWRITE = 0x77, OP_JEFF = 0xA5, OP_SERWRITE = 0xC3,
    OP_SERSETSTAT = 0xC4, OP_SERTERM = 0xC5, OP_READEX = 0xD2,
    OP_FUJI = 0xE2, OP_NET = 0xE3, OP_CPM = 0xE4, OP_CLOCK = 0xE5,
    OP_REREADEX = 0xF2, OP_RESET3 = 0xF8, OP_RESET2 = 0xFE, OP_RESET1 = 0xFF,
};

/* The FUJI/NET commands the decoder acts on itself. */
enum { CMD_READY = 0x00, CMD_SEND_RESPONSE = 0x01, CMD_SEND_ERROR = 0x02 };

#define VAR (-1)        /* a segment that runs until the direction changes */
#define XBUF 1024       /* bytes kept each way of the exchange in progress */
#define JOIN_LOOKBACK 8 /* entries a SEND RESPONSE looks back over */

typedef struct {
    coco_fuji_packet p;
    uint64_t hash; /* every byte of the exchange: what "identical" compares */
} slot;

struct dwtrace {
    slot ring[DWTRACE_MAX];
    int head, count; /* oldest at head */
    uint64_t serial;

    /* The exchange in progress. */
    int active;
    int orphan;        /* began with FujiNet talking: no request */
    int seg, seg_have; /* which segment, and its bytes so far */
    int cut;           /* a fixed-length segment ended early */
    uint8_t tx[XBUF], rx[XBUF];
    int ntx, nrx;      /* every byte, even past XBUF */
    uint8_t tx_last[2];
    uint8_t wire[64];  /* the first bytes, in the order they crossed */
    int nwire;
    uint64_t cur_serial;

    uint64_t bytes[2], exchanges;
};

static const char *op_name(int op)
{
    switch (op) {
    case OP_NOP:        return "NOP";
    case OP_NAMEOBJ_MNT: return "NAMEOBJ MOUNT";
    case OP_TIME:       return "TIME";
    case OP_SERREAD:    return "SERREAD";
    case OP_SERGETSTAT: return "SERGETSTAT";
    case OP_SERINIT:    return "SERINIT";
    case OP_PRINTFLUSH: return "PRINTFLUSH";
    case OP_GETSTAT:    return "GETSTAT";
    case OP_INIT:       return "INIT";
    case OP_PRINT:      return "PRINT";
    case OP_READ:       return "READ";
    case OP_SETSTAT:    return "SETSTAT";
    case OP_TERM:       return "TERM";
    case OP_WRITE:      return "WRITE";
    case OP_DWINIT:     return "DWINIT";
    case OP_SERREADM:   return "SERREADM";
    case OP_SERWRITEM:  return "SERWRITEM";
    case OP_REREAD:     return "REREAD";
    case OP_REWRITE:    return "REWRITE";
    case OP_JEFF:       return "JEFF";
    case OP_SERWRITE:   return "SERWRITE";
    case OP_SERSETSTAT: return "SERSETSTAT";
    case OP_SERTERM:    return "SERTERM";
    case OP_READEX:     return "READEX";
    case OP_FUJI:       return "FUJI";
    case OP_NET:        return "NET";
    case OP_CPM:        return "CPM";
    case OP_CLOCK:      return "CLOCK";
    case OP_REREADEX:   return "REREADEX";
    case OP_RESET1: case OP_RESET2: case OP_RESET3: return "RESET";
    default:            return NULL;
    }
}

static const char *fuji_command_name(int id)
{
    size_t i;
    for (i = 0; i < sizeof fuji_command_names / sizeof fuji_command_names[0];
         i++)
        if (fuji_command_names[i].id == id)
            return fuji_command_names[i].name;
    return "";
}

/* Segment seg of the exchange in progress: sets *dir (1 to FujiNet, 0 back)
 * and returns its length, VAR, or 0 once there are no more. */
static int segment(const dwtrace *t, int seg, int *dir)
{
    static const int readex[] = { 5, 256, 2, 1 };

    *dir = (seg & 1) ? 0 : 1;
    if (t->orphan) {
        *dir = 0;
        return seg == 0 ? VAR : 0;
    }
    switch (t->tx[0]) {
    case OP_READEX:
    case OP_REREADEX:
        return seg < 4 ? readex[seg] : 0;
    case OP_WRITE:
    case OP_REWRITE:
        return seg == 0 ? 263 : seg == 1 ? 1 : 0;
    case OP_TIME:
        return seg == 0 ? 1 : seg == 1 ? 6 : 0;
    case OP_DWINIT:
        return seg == 0 ? 2 : seg == 1 ? 1 : 0;
    case OP_SERREAD:
        return seg == 0 ? 1 : seg == 1 ? 2 : 0;
    case OP_JEFF:
        return seg == 0 ? 1 : seg == 1 ? 7 : 0;
    case OP_GETSTAT:
    case OP_SETSTAT:
    case OP_SERGETSTAT:
    case OP_SERWRITE:
        return seg == 0 ? 3 : 0;
    case OP_SERINIT:
    case OP_SERTERM:
    case OP_PRINT:
        return seg == 0 ? 2 : 0;
    case OP_NOP:
    case OP_INIT:
    case OP_TERM:
    case OP_PRINTFLUSH:
    case OP_RESET1:
    case OP_RESET2:
    case OP_RESET3:
        return seg == 0 ? 1 : 0;
    case OP_FUJI:
        if (t->ntx < 2)
            break;
        if (t->tx[1] == CMD_READY || t->tx[1] == CMD_SEND_ERROR)
            return seg == 0 ? 2 : seg == 1 ? 1 : 0;
        if (t->tx[1] == CMD_SEND_RESPONSE)
            return seg == 0 ? 2 : seg == 1 ? VAR : 0;
        break;
    case OP_NET:
        /* E3 unit command aux1 aux2; SEND RESPONSE's aux is the length the
         * CoCo asked for, and FujiNet pads its reply to it. */
        if (t->ntx < 3)
            break;
        if (t->tx[2] == CMD_SEND_ERROR)
            return seg == 0 ? 5 : seg == 1 ? 1 : 0;
        if (t->tx[2] == CMD_SEND_RESPONSE)
            return seg == 0 ? 5 : seg == 1 ? (t->tx[3] << 8 | t->tx[4]) : 0;
        break;
    default:
        break;
    }
    return seg < 2 ? VAR : 0;
}

static slot *at(dwtrace *t, int k)
{
    return &t->ring[(t->head + k) % DWTRACE_MAX];
}

static const slot *at_c(const dwtrace *t, int k)
{
    return &t->ring[(t->head + k) % DWTRACE_MAX];
}

/* snprintf onto the end of dst, clamped. */
static void add(char *dst, size_t sz, const char *fmt, ...)
{
    size_t n = strlen(dst);
    va_list ap;
    if (n + 1 >= sz)
        return;
    va_start(ap, fmt);
    vsnprintf(dst + n, sz - n, fmt, ap);
    va_end(ap);
}

static void add_reply(char *dst, size_t sz, int n, uint8_t first)
{
    if (n == 1)
        add(dst, sz, " \xe2\x86\x90 %02X", first);
    else if (n > 1)
        add(dst, sz, " \xe2\x86\x90 %d B", n);
}

static void render(const dwtrace *t, char *dst, size_t sz)
{
    const int op = t->tx[0];
    const char *name = op_name(op);

    dst[0] = '\0';
    if (t->orphan) {
        add(dst, sz, "\xe2\x86\x90 %d B with no request", t->nrx);
        return;
    }
    if (name)
        add(dst, sz, "\xe2\x86\x92 %s", name);
    else
        add(dst, sz, "\xe2\x86\x92 %02X", op);

    switch (op) {
    case OP_READEX:
    case OP_REREADEX:
    case OP_WRITE:
    case OP_REWRITE:
        if (t->ntx >= 5)
            add(dst, sz, " drive %u LSN %06X", t->tx[1],
                (unsigned)(t->tx[2] << 16 | t->tx[3] << 8 | t->tx[4]));
        break;
    case OP_GETSTAT:
    case OP_SETSTAT:
        if (t->ntx >= 3)
            add(dst, sz, " drive %u code %02X", t->tx[1], t->tx[2]);
        break;
    case OP_FUJI:
    case OP_CPM:
    case OP_CLOCK:
        if (t->ntx >= 2) {
            add(dst, sz, " %02X", t->tx[1]);
            if (op == OP_FUJI && fuji_command_name(t->tx[1])[0])
                add(dst, sz, " %s", fuji_command_name(t->tx[1]));
        }
        if (t->ntx > 2)
            add(dst, sz, " (%d B)", t->ntx);
        break;
    case OP_NET:
        if (t->ntx >= 2)
            add(dst, sz, " %u", t->tx[1]);
        if (t->ntx >= 3) {
            add(dst, sz, " %02X", t->tx[2]);
            if (fuji_command_name(t->tx[2])[0])
                add(dst, sz, " %s", fuji_command_name(t->tx[2]));
        }
        if (t->ntx > 5)
            add(dst, sz, " (%d B)", t->ntx);
        break;
    default:
        /* Only the variable-length ones are worth a size. */
        if (t->ntx > 1 && (!name || op == OP_READ || op == OP_REREAD ||
                           op == OP_NAMEOBJ_MNT || op == OP_SERREADM ||
                           op == OP_SERWRITEM || op == OP_SERSETSTAT))
            add(dst, sz, " (%d B)", t->ntx);
        break;
    }

    if ((op == OP_READEX || op == OP_REREADEX) && t->nrx > 256)
        add(dst, sz, " \xe2\x86\x90 256 B, status %02X", t->rx[256]);
    else
        add_reply(dst, sz, t->nrx, t->rx[0]);
    if (t->cut)
        add(dst, sz, " (cut short)");
}

/* The exchange in progress, as an entry. */
static void describe(const dwtrace *t, coco_fuji_packet *p)
{
    const int op = t->tx[0];

    memset(p, 0, sizeof *p);
    p->serial = t->cur_serial;
    p->to_host = t->orphan;
    p->device = t->orphan ? -1 : op;
    p->command = -1;
    if (!t->orphan && (op == OP_FUJI || op == OP_CPM || op == OP_CLOCK) &&
        t->ntx >= 2)
        p->command = t->tx[1];
    if (!t->orphan && op == OP_NET && t->ntx >= 3)
        p->command = t->tx[2];
    p->length = t->nrx > 0 ? t->nrx : -1;
    p->nbytes = t->ntx + t->nrx;
    memcpy(p->data, t->wire, (size_t)t->nwire);
    p->repeat = 1;
    render(t, p->text, sizeof p->text);
}

static uint64_t fnv(uint64_t h, const uint8_t *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        h ^= b[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static uint64_t hash_of(const dwtrace *t)
{
    uint8_t tail[4] = { (uint8_t)t->orphan, (uint8_t)t->cut,
                        (uint8_t)(t->ntx & 0xFF), (uint8_t)(t->nrx & 0xFF) };
    uint64_t h = 1469598103934665603ULL;
    h = fnv(h, t->tx, t->ntx < XBUF ? t->ntx : XBUF);
    h = fnv(h, t->rx, t->nrx < XBUF ? t->nrx : XBUF);
    return fnv(h, tail, 4);
}

static int same(const slot *a, const slot *b)
{
    const int n = a->p.nbytes < 64 ? a->p.nbytes : 64;
    return a->hash == b->hash && a->p.nbytes == b->p.nbytes &&
           a->p.device == b->p.device && a->p.to_host == b->p.to_host &&
           a->p.length == b->p.length &&
           memcmp(a->p.data, b->p.data, (size_t)n) == 0 &&
           strcmp(a->p.text, b->p.text) == 0;
}

static void push(dwtrace *t, const slot *s)
{
    if (t->count == DWTRACE_MAX) {
        t->head = (t->head + 1) % DWTRACE_MAX;
        t->count--;
    }
    *at(t, t->count) = *s;
    at(t, t->count)->p.serial = ++t->serial;
    t->count++;
}

/* A SEND RESPONSE carries the reply to the FUJI or NET command before it:
 * fold it into that command's entry, looking back over the READY probes and
 * SEND ERRORs fujinet-lib puts in between. */
static int join(dwtrace *t, const slot *resp)
{
    const coco_fuji_packet *r = &resp->p;
    const int off = r->nbytes - r->length; /* where its reply starts */
    int k, look, i;

    if (r->length <= 0)
        return 0;
    for (k = t->count - 1, look = 0; k >= 0 && look < JOIN_LOOKBACK;
         k--, look++) {
        slot *s = at(t, k);
        coco_fuji_packet *c = &s->p;

        if (c->to_host)
            return 0;
        if (c->device == OP_FUJI && c->command == CMD_READY)
            continue;
        if (c->device != r->device ||
            (r->device == OP_NET && c->data[1] != r->data[1]))
            return 0;
        if (c->command == CMD_SEND_ERROR)
            continue;
        if (c->command < 0 || c->command == CMD_READY ||
            c->command == CMD_SEND_RESPONSE || c->length >= 0 ||
            c->repeat > 1)
            return 0;

        for (i = 0; i < r->length && c->nbytes + i < 64 && off + i < 64; i++)
            c->data[c->nbytes + i] = r->data[off + i];
        c->nbytes += r->length;
        c->length = r->length;
        add_reply(c->text, sizeof c->text, r->length,
                  off < 64 ? r->data[off] : 0);
        s->hash = (s->hash ^ resp->hash) * 1099511628211ULL;
        /* New serials from here on, so a UI watching the last one redraws. */
        for (i = k; i < t->count; i++)
            at(t, i)->p.serial = ++t->serial;
        return 1;
    }
    return 0;
}

static void finish(dwtrace *t)
{
    slot s;
    const coco_fuji_packet *p = &s.p;

    describe(t, &s.p);
    s.hash = hash_of(t);
    t->active = 0;
    t->exchanges++;

    if (!t->cut && p->command == CMD_SEND_RESPONSE &&
        (p->device == OP_FUJI || p->device == OP_NET) && join(t, &s))
        return;
    if (t->count > 0 && same(at(t, t->count - 1), &s)) {
        slot *last = at(t, t->count - 1);
        last->p.repeat++;
        last->p.serial = ++t->serial;
        return;
    }
    push(t, &s);
}

static void begin(dwtrace *t, int dir)
{
    t->active = 1;
    t->orphan = dir == 0;
    t->seg = t->seg_have = 0;
    t->cut = 0;
    t->ntx = t->nrx = 0;
    t->nwire = 0;
    t->tx_last[0] = t->tx_last[1] = 0;
}

static void append(dwtrace *t, int dir, uint8_t b)
{
    if (dir) {
        if (t->ntx < XBUF)
            t->tx[t->ntx] = b;
        t->ntx++;
        t->tx_last[0] = t->tx_last[1];
        t->tx_last[1] = b;
    } else {
        if (t->nrx < XBUF)
            t->rx[t->nrx] = b;
        t->nrx++;
    }
    if (t->nwire < (int)sizeof t->wire)
        t->wire[t->nwire++] = b;
    t->seg_have++;
    t->cur_serial = ++t->serial;
}

/* A command with no reply, then bus_ready's E2 00 in the same breath: the
 * command becomes an entry of its own, and E2 00 a READY awaiting its byte. */
static int split_ready(dwtrace *t)
{
    if (t->orphan || t->ntx <= 2 || t->tx_last[0] != OP_FUJI ||
        t->tx_last[1] != CMD_READY)
        return 0;
    t->ntx -= 2;
    if (t->nwire > t->ntx)
        t->nwire = t->ntx;
    finish(t);
    begin(t, 1);
    append(t, 1, OP_FUJI);
    append(t, 1, CMD_READY);
    t->seg = 1;
    t->seg_have = 0;
    return 1;
}

/* Whether a byte in direction dir belongs to the exchange in progress. A
 * variable segment ends where the direction changes; a fixed one that does
 * leaves the exchange cut short. */
static int accepts(dwtrace *t, int dir)
{
    int sdir, len = segment(t, t->seg, &sdir);

    if (len == 0)
        return 0;
    if (sdir == dir)
        return 1;
    if (len != VAR) {
        t->cut = 1;
        return 0;
    }
    if (dir == 0 && t->seg == 0 && split_ready(t))
        return 1;
    t->seg++;
    t->seg_have = 0;
    len = segment(t, t->seg, &sdir);
    return len != 0 && sdir == dir;
}

static void feed_byte(dwtrace *t, int dir, uint8_t b)
{
    int sdir, len;

    t->bytes[dir]++;
    if (t->active && !accepts(t, dir))
        finish(t);
    if (!t->active)
        begin(t, dir);
    append(t, dir, b);
    for (;;) {
        len = segment(t, t->seg, &sdir);
        if (len == 0) {
            finish(t);
            return;
        }
        if (len == VAR || t->seg_have < len)
            return;
        t->seg++;
        t->seg_have = 0;
    }
}

dwtrace *dwtrace_create(void)
{
    return calloc(1, sizeof(dwtrace));
}

void dwtrace_destroy(dwtrace *t)
{
    free(t);
}

void dwtrace_feed(dwtrace *t, int dir, const uint8_t *buf, size_t len)
{
    size_t i;

    if (!t)
        return;
    dir = dir ? 1 : 0;
    if (len == 0) {
        /* A new connection: nothing half-way through will be finished. */
        if (t->active) {
            int sdir;
            int seglen = segment(t, t->seg, &sdir);
            if (seglen != VAR)
                t->cut = 1;
            finish(t);
        }
        return;
    }
    for (i = 0; i < len; i++)
        feed_byte(t, dir, buf[i]);
}

void dwtrace_clear(dwtrace *t)
{
    if (!t)
        return;
    t->head = t->count = 0;
}

int dwtrace_get(const dwtrace *t, coco_fuji_packet *out, int max)
{
    int total, k, n = 0;

    if (!t || !out || max <= 0)
        return 0;
    total = t->count + (t->active ? 1 : 0);
    for (k = total > max ? total - max : 0; k < total; k++) {
        coco_fuji_packet *p = &out[n++];
        if (k < t->count)
            *p = at_c(t, k)->p;
        else
            describe(t, p);
        if (p->repeat > 1)
            add(p->text, sizeof p->text, " \xc3\x97%u", (unsigned)p->repeat);
    }
    return n;
}

uint64_t dwtrace_bytes(const dwtrace *t, int dir)
{
    return t ? t->bytes[dir ? 1 : 0] : 0;
}

uint64_t dwtrace_exchanges(const dwtrace *t)
{
    return t ? t->exchanges : 0;
}

void dwtrace_pending(const dwtrace *t, char *dst, int dstsz)
{
    const char *name, *way;
    int dir, len;

    if (!dst || dstsz <= 0)
        return;
    if (!t || !t->active) {
        snprintf(dst, (size_t)dstsz, "idle");
        return;
    }
    len = segment(t, t->seg, &dir);
    way = dir ? "out" : "in";
    name = t->orphan ? "unasked" : op_name(t->tx[0]);
    dst[0] = '\0';
    if (name)
        add(dst, (size_t)dstsz, "%s", name);
    else
        add(dst, (size_t)dstsz, "%02X", t->tx[0]);
    if (!t->orphan && t->tx[0] == OP_NET && t->ntx >= 3)
        add(dst, (size_t)dstsz, " %u %02X", t->tx[1], t->tx[2]);
    else if (!t->orphan && name && t->tx[0] >= OP_FUJI &&
             t->tx[0] <= OP_CLOCK && t->ntx >= 2)
        add(dst, (size_t)dstsz, " %02X", t->tx[1]);
    if (len == VAR)
        add(dst, (size_t)dstsz, ", %d B %s", t->seg_have, way);
    else
        add(dst, (size_t)dstsz, ", %d of %d B %s", t->seg_have, len, way);
}
