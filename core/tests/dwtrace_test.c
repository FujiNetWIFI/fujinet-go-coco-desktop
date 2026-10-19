/*
 * dwtrace_test -- the FujiNet tab's DriveWire decoder, on made-up bytes.
 *
 * No XRoar, no session, no ROMs, no network: the decoder is fed the bytes
 * the Becker port would carry, both ways, and its entries are checked.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "dwtrace.h"

#define TO_FN 1   /* CoCo -> FujiNet */
#define TO_COCO 0 /* FujiNet -> CoCo */

static int failures;
static coco_fuji_packet pk[DWTRACE_MAX + 8];
static int got; /* what the last get() returned */

static void ok(const char *what, int cond)
{
    int i;
    if (cond) {
        printf("ok   %s\n", what);
    } else {
        printf("FAIL %s\n", what);
        for (i = 0; i < got && i < 8; i++)
            printf("     [%d] %s\n", i, pk[i].text);
        failures++;
    }
}

static void feed(dwtrace *t, int dir, const uint8_t *b, size_t n)
{
    dwtrace_feed(t, dir, b, n);
}

/* One byte at a time, the way becker.c hands over what the CoCo reads. */
static void feed_slowly(dwtrace *t, int dir, const uint8_t *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        dwtrace_feed(t, dir, b + i, 1);
}

static int get(dwtrace *t)
{
    return got = dwtrace_get(t, pk, DWTRACE_MAX + 8);
}

static int has(const coco_fuji_packet *p, const char *s)
{
    return strstr(p->text, s) != NULL;
}

static void readex(dwtrace *t, uint8_t fill, int slowly)
{
    static const uint8_t req[] = { 0xD2, 0x00, 0x00, 0x01, 0x23 };
    static const uint8_t ck[] = { 0x12, 0x34 }, status[] = { 0x00 };
    uint8_t sector[256];
    void (*f)(dwtrace *, int, const uint8_t *, size_t) =
        slowly ? feed_slowly : feed;
    memset(sector, fill, sizeof(sector));
    f(t, TO_FN, req, sizeof(req));
    f(t, TO_COCO, sector, sizeof(sector));
    f(t, TO_FN, ck, sizeof(ck));
    f(t, TO_COCO, status, sizeof(status));
}

static void ready(dwtrace *t)
{
    static const uint8_t req[] = { 0xE2, 0x00 }, rep[] = { 0x01 };
    feed(t, TO_FN, req, 2);
    feed(t, TO_COCO, rep, 1);
}

int main(void)
{
    dwtrace *t = dwtrace_create();
    char pending[96];
    int n, i;
    uint64_t serial;

    ok("a trace can be created", t != NULL);
    if (!t)
        return 1;

    /* --- empty ---------------------------------------------------------- */
    ok("an empty trace has no entries", get(t) == 0);
    dwtrace_pending(t, pending, sizeof(pending));
    ok("an empty trace is idle", strcmp(pending, "idle") == 0);
    ok("an empty trace has counted no bytes",
       dwtrace_bytes(t, TO_FN) == 0 && dwtrace_bytes(t, TO_COCO) == 0);

    /* --- READEX: four segments, one entry -------------------------------- */
    readex(t, 0xAA, 1);
    n = get(t);
    ok("READEX is one entry", n == 1);
    ok("READEX's opcode is the device", pk[0].device == 0xD2);
    ok("READEX names drive and LSN", has(&pk[0], "READEX drive 0 LSN 000123"));
    ok("READEX shows the sector and the status",
       has(&pk[0], "256 B, status 00"));
    ok("READEX counts both ways", pk[0].nbytes == 5 + 256 + 2 + 1 &&
                                  pk[0].length == 257);
    ok("READEX's hex starts with the request, then the sector",
       pk[0].data[0] == 0xD2 && pk[0].data[4] == 0x23 &&
       pk[0].data[5] == 0xAA && pk[0].data[63] == 0xAA);
    ok("bytes are counted each way", dwtrace_bytes(t, TO_FN) == 7 &&
                                     dwtrace_bytes(t, TO_COCO) == 257);

    /* --- split input: a reply arriving in pieces is in progress ---------- */
    {
        static const uint8_t req[] = { 0xD2, 0x00, 0x00, 0x00, 0x07 };
        uint8_t part[100];
        memset(part, 0x55, sizeof(part));
        feed(t, TO_FN, req, 3);
        feed(t, TO_FN, req + 3, 2);
        feed_slowly(t, TO_COCO, part, sizeof(part));
        n = get(t);
        ok("a half-read sector shows as the last entry", n == 2 &&
           has(&pk[1], "LSN 000007") && has(&pk[1], "100 B"));
        dwtrace_pending(t, pending, sizeof(pending));
        ok("pending says how far the sector has got",
           strcmp(pending, "READEX, 100 of 256 B in") == 0);
        serial = pk[1].serial;
        feed(t, TO_COCO, part, 1);
        get(t);
        ok("the entry in progress changes serial as bytes arrive",
           pk[1].serial != serial);
        /* The CoCo gives up and asks again: the old one is cut short. */
        readex(t, 0x00, 0);
        n = get(t);
        ok("a request mid-reply cuts the old exchange short", n == 3 &&
           has(&pk[1], "(cut short)") && has(&pk[1], "101 B"));
    }

    /* --- fujinet-lib's GET ADAPTERCONFIG -------------------------------- */
    dwtrace_clear(t);
    ok("clear empties the list", get(t) == 0);
    ok("clear keeps the counters", dwtrace_exchanges(t) == 3);
    {
        static const uint8_t cmd[] = { 0xE2, 0xE8, 0xE2, 0x00 };
        static const uint8_t err_req[] = { 0xE2, 0x02 }, err_rep[] = { 0x00 };
        static const uint8_t resp_req[] = { 0xE2, 0x01 };
        uint8_t cfg[140];
        for (i = 0; i < 140; i++)
            cfg[i] = (uint8_t)i;

        ready(t);
        feed_slowly(t, TO_FN, cmd, sizeof(cmd)); /* command, then bus_ready */
        feed(t, TO_COCO, (const uint8_t *)"\x01", 1);
        n = get(t);
        ok("a trailing E2 00 splits off as READY", n == 3 &&
           pk[1].device == 0xE2 && pk[1].command == 0xE8 &&
           pk[1].length == -1 && pk[1].nbytes == 2 &&
           cocodebug_fujinet_is_ready(&pk[2]) && pk[2].length == 1);
        ok("the command is named",
           has(&pk[1], "FUJI E8 GET ADAPTERCONFIG"));
        ok("READY shows its reply byte", has(&pk[2], "DEVICE READY") &&
           has(&pk[2], "\xe2\x86\x90 01"));
        ok("only READY probes are READY",
           cocodebug_fujinet_is_ready(&pk[0]) &&
           !cocodebug_fujinet_is_ready(&pk[1]));

        feed(t, TO_FN, err_req, 2);
        feed(t, TO_COCO, err_rep, 1);
        ready(t);
        feed(t, TO_FN, resp_req, 2);
        feed_slowly(t, TO_COCO, cfg, sizeof(cfg));
        n = get(t);
        ok("SEND ERROR is its own entry", n == 6 &&
           has(&pk[3], "FUJI 02 SEND ERROR") && has(&pk[3], "\xe2\x86\x90 00"));
        ok("a SEND RESPONSE still arriving shows on its own",
           has(&pk[5], "SEND RESPONSE") && pk[5].length == 140);
        dwtrace_pending(t, pending, sizeof(pending));
        ok("pending names the FUJI command",
           strcmp(pending, "FUJI 01, 140 B in") == 0);
        serial = pk[4].serial;

        ready(t); /* the next call's bus_ready ends the response */
        n = get(t);
        ok("SEND RESPONSE folds into the command it answers", n == 5 &&
           pk[1].command == 0xE8 && pk[1].length == 140 &&
           pk[1].nbytes == 142 && has(&pk[1], "\xe2\x86\x90 140 B"));
        ok("the folded reply follows the request in the hex",
           pk[1].data[0] == 0xE2 && pk[1].data[1] == 0xE8 &&
           pk[1].data[2] == 0 && pk[1].data[3] == 1 && pk[1].data[63] == 61);
        ok("entries after the folded one get new serials",
           pk[3].serial > pk[1].serial && pk[4].serial > serial);
    }

    /* --- coalescing ------------------------------------------------------ */
    dwtrace_clear(t);
    ready(t);
    get(t);
    serial = pk[0].serial;
    ready(t);
    ready(t);
    n = get(t);
    ok("identical exchanges in a row fold into one", n == 1 &&
       pk[0].repeat == 3 && has(&pk[0], "\xc3\x97" "3"));
    ok("a fold changes the serial", pk[0].serial != serial);
    readex(t, 0x11, 0);
    readex(t, 0x11, 0);
    readex(t, 0x22, 0);
    n = get(t);
    ok("only byte-identical exchanges fold", n == 3 &&
       pk[1].repeat == 2 && pk[2].repeat == 1);

    /* --- NET: the command comes after the unit --------------------------- */
    dwtrace_clear(t);
    {
        static const uint8_t rd[] = { 0xE3, 0x01, 0x52, 0x00, 0x04,
                                      0xE2, 0x00 };
        static const uint8_t err[] = { 0xE3, 0x01, 0x02, 0x00, 0x00 };
        static const uint8_t resp[] = { 0xE3, 0x01, 0x01, 0x00, 0x04 };
        static const uint8_t data[] = { 'D', 'A', 'T', 'A' };
        ready(t);
        feed(t, TO_FN, rd, sizeof(rd));
        feed(t, TO_COCO, (const uint8_t *)"\x01", 1);
        feed(t, TO_FN, err, sizeof(err));
        feed(t, TO_COCO, (const uint8_t *)"\x00", 1);
        ready(t);
        feed(t, TO_FN, resp, sizeof(resp));
        feed(t, TO_COCO, data, sizeof(data));
        n = get(t);
        ok("NET's SEND RESPONSE ends at the length it asked for", n == 5);
        ok("NET names unit and command", pk[1].device == 0xE3 &&
           pk[1].command == 0x52 && has(&pk[1], "NET 1 52 READ"));
        ok("NET's reply folds into its READ", pk[1].length == 4 &&
           has(&pk[1], "\xe2\x86\x90 4 B") && pk[1].data[5] == 'D');
        ok("NET's SEND ERROR stays", has(&pk[3], "NET 1 02 SEND ERROR"));
    }

    /* --- the odd cases ----------------------------------------------------- */
    dwtrace_clear(t);
    {
        static const uint8_t junk[] = { 0x01, 0x02, 0x03 };
        static const uint8_t time_req[] = { 0x23 };
        static const uint8_t time_rep[] = { 126, 10, 19, 12, 0, 0 };
        static const uint8_t dwinit[] = { 0x5A, 0x01 };
        feed(t, TO_COCO, junk, sizeof(junk));
        feed(t, TO_FN, time_req, 1);
        n = get(t);
        ok("bytes from FujiNet with no request are their own entry",
           n == 2 && pk[0].to_host == 1 && pk[0].device == -1 &&
           has(&pk[0], "3 B with no request"));
        feed(t, TO_COCO, time_rep, sizeof(time_rep));
        feed(t, TO_FN, dwinit, sizeof(dwinit));
        feed(t, TO_COCO, (const uint8_t *)"\x04", 1);
        n = get(t);
        ok("TIME ends after its six bytes", n == 3 &&
           has(&pk[1], "TIME \xe2\x86\x90 6 B"));
        ok("DWINIT ends after its one", has(&pk[2], "DWINIT \xe2\x86\x90 04"));

        /* A reconnect closes whatever was half-way through. */
        feed(t, TO_FN, (const uint8_t *)"\xD2\x00", 2);
        dwtrace_feed(t, TO_COCO, NULL, 0);
        n = get(t);
        ok("a reconnect cuts the exchange in progress short",
           n == 4 && has(&pk[3], "(cut short)"));
        dwtrace_pending(t, pending, sizeof(pending));
        ok("and leaves nothing pending", strcmp(pending, "idle") == 0);
    }

    /* --- the ring keeps the newest DWTRACE_MAX --------------------------- */
    dwtrace_clear(t);
    for (i = 0; i < DWTRACE_MAX + 88; i++) {
        uint8_t getstat[] = { 0x47, 0x00, (uint8_t)i };
        getstat[1] = (uint8_t)(i >> 8);
        feed(t, TO_FN, getstat, sizeof(getstat));
    }
    n = get(t);
    ok("the ring holds DWTRACE_MAX", n == DWTRACE_MAX);
    ok("the oldest fall off the front",
       pk[0].data[1] == 0 && pk[0].data[2] == 88 &&
       pk[n - 1].data[2] == (uint8_t)(DWTRACE_MAX + 87));
    for (i = 1; i < n; i++)
        if (pk[i].serial <= pk[i - 1].serial)
            break;
    ok("serials rise oldest to newest", i == n);
    n = dwtrace_get(t, pk, 10);
    ok("a short read takes the newest", n == 10 &&
       pk[9].data[2] == (uint8_t)(DWTRACE_MAX + 87));

    dwtrace_destroy(t);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
