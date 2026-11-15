/*
 * memtools_test -- search parsing, the watch list, range parsing, trace
 * filters and breakpoint text. No machine, so no ROMs are needed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <string.h>

#include "cocomemtools.h"
#include "m6809dasm.h"

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

static cocodebug_trace_entry make_entry(uint16_t pc, int is6309, const uint8_t *bytes, int n)
{
    cocodebug_trace_entry ent;
    memset(&ent, 0, sizeof ent);
    ent.pc = pc;
    ent.is6309 = (uint8_t)is6309;
    memcpy(ent.bytes, bytes, (size_t)n);
    return ent;
}

int main(void)
{
    uint8_t pat[COCOSEARCH_MAX], mask;
    int count, flags;
    uint32_t lo, hi;
    char buf[160];

    /* ---- search parsing ---- */
    CHECK(cocosearch_parse(COCOSEARCH_HEX, "5A 3c,$FF 0x10", pat, &count, &mask, &flags), "hex parses");
    CHECK(count == 4 && pat[0] == 0x5A && pat[1] == 0x3C && pat[2] == 0xFF && pat[3] == 0x10, "hex bytes");
    CHECK(mask == 0xFF && flags == 0, "hex mask and flags");
    CHECK(cocosearch_parse(COCOSEARCH_HEX, "5", pat, &count, &mask, &flags) && count == 1 && pat[0] == 5,
          "a lone digit is a byte");
    CHECK(!cocosearch_parse(COCOSEARCH_HEX, "zz", pat, &count, &mask, &flags), "bad hex");
    CHECK(!cocosearch_parse(COCOSEARCH_HEX, "  ", pat, &count, &mask, &flags), "empty hex");
    CHECK(cocosearch_parse(COCOSEARCH_TEXT, "Ab", pat, &count, &mask, &flags) && count == 2 &&
          pat[0] == 'A' && (flags & COCODEBUG_SEARCH_NOCASE), "text is case-insensitive");

    /* ---- range parsing, numbers only (no debugger) ---- */
    CHECK(cocosym_parse_range(NULL, COCODEBUG_SPACE_CPU, "0400", &lo, &hi) && lo == 0x400 && hi == 0x400,
          "a bare hex token is hex");
    CHECK(cocosym_parse_range(NULL, COCODEBUG_SPACE_CPU, "$0400-$05FF", &lo, &hi) && lo == 0x400 && hi == 0x5FF,
          "lo-hi");
    CHECK(cocosym_parse_range(NULL, COCODEBUG_SPACE_CPU, "$05FF..$0400", &lo, &hi) && lo == 0x400 && hi == 0x5FF,
          "lo..hi, swapped");
    CHECK(cocosym_parse_range(NULL, COCODEBUG_SPACE_CPU, "$1FFFF", &lo, &hi) && lo == 0xFFFF,
          "CPU addresses are 16 bits");
    CHECK(cocosym_parse_range(NULL, COCODEBUG_SPACE_PHYS, "1F400", &lo, &hi) && lo == 0x1F400,
          "PHYS takes five digits as hex");
    CHECK(cocosym_parse_range(NULL, COCODEBUG_SPACE_PHYS, "$7FFFF-$80000", &lo, &hi) && lo == 0x7FFFF &&
          hi == 0x80000, "PHYS ranges cross 512K");
    CHECK(!cocosym_parse_range(NULL, COCODEBUG_SPACE_CPU, "nosuchname", &lo, &hi), "an unknown name");
    CHECK(!cocosym_parse_range(NULL, COCODEBUG_SPACE_CPU, "  ", &lo, &hi), "empty");

    /* ---- watches ---- */
    cocowatch_clear();
    CHECK(cocowatch_add("  $0400 ", COCODEBUG_SPACE_CPU) == 0 && cocowatch_add("1F400", COCODEBUG_SPACE_PHYS) == 1,
          "add");
    CHECK(cocowatch_add("   ", COCODEBUG_SPACE_CPU) == -1, "an empty add is refused");
    CHECK(cocowatch_count() == 2 && !strcmp(cocowatch_text(0), "$0400"), "text is trimmed");
    CHECK(cocowatch_space(0) == COCODEBUG_SPACE_CPU && cocowatch_space(1) == COCODEBUG_SPACE_PHYS,
          "each watch keeps its space");
    CHECK(cocowatch_range(NULL, 1, &lo, &hi) && lo == 0x1F400, "a PHYS watch resolves in PHYS");
    cocowatch_format(NULL, 0, buf, sizeof buf);
    CHECK(!strncmp(buf, "$0400", 5) && strstr(buf, "$0400  -"), "a running machine shows a dash");
    {
        unsigned gen = cocowatch_generation();
        cocowatch_remove(0);
        CHECK(cocowatch_count() == 1 && !strcmp(cocowatch_text(0), "1F400"), "remove");
        CHECK(cocowatch_space(0) == COCODEBUG_SPACE_PHYS, "the space moves with its watch");
        CHECK(cocowatch_generation() != gen, "the generation bumps");
    }
    cocowatch_clear();
    CHECK(cocowatch_count() == 0, "clear");

    /* ---- trace filters ---- */
    {
        static const struct { uint8_t bytes[6]; int cpu6309; int branch; const char *what; } cases[] = {
            { { 0x20, 0x10 }, 0, 1, "BRA" },
            { { 0x26, 0x10 }, 0, 1, "BNE" },
            { { 0x16, 0x04, 0x00 }, 0, 1, "LBRA" },
            { { 0x10, 0x27, 0x04, 0x00 }, 0, 1, "LBEQ" },
            { { 0x7E, 0x04, 0x00 }, 0, 1, "JMP extended" },
            { { 0x6E, 0x84 }, 0, 1, "JMP indexed" },
            { { 0x8D, 0x10 }, 0, 1, "BSR" },
            { { 0x17, 0x04, 0x00 }, 0, 1, "LBSR" },
            { { 0xBD, 0x04, 0x00 }, 0, 1, "JSR" },
            { { 0x39 }, 0, 1, "RTS" },
            { { 0x3B }, 0, 1, "RTI" },
            { { 0x35, 0x80 }, 0, 1, "PULS PC" },
            { { 0x35, 0x96 }, 0, 1, "PULS A,B,X,PC" },
            { { 0x37, 0xFF }, 0, 1, "PULU everything" },
            { { 0x1F, 0x50 }, 0, 1, "TFR D,PC" },
            { { 0x1E, 0x15 }, 0, 1, "EXG X,PC" },
            { { 0x3F }, 0, 1, "SWI" },
            { { 0x10, 0x3F }, 0, 1, "SWI2" },
            { { 0x11, 0x3F }, 0, 1, "SWI3" },
            { { 0x12 }, 0, 0, "NOP" },
            { { 0x35, 0x7F }, 0, 0, "PULS without PC" },
            { { 0x34, 0xFF }, 0, 0, "PSHS everything" },
            { { 0x1F, 0x89 }, 0, 0, "TFR A,B" },
            { { 0x1F, 0x01 }, 0, 0, "TFR D,X" },
            { { 0x86, 0x41 }, 0, 0, "LDA #" },
            { { 0x85, 0x01 }, 0, 0, "BITA is not a branch" },
            { { 0xC5, 0x01 }, 0, 0, "BITB is not a branch" },
            { { 0x10, 0x86, 0x00, 0x41 }, 1, 0, "6309 LDW #" },
            { { 0x1F, 0x67 }, 1, 0, "6309 TFR W,V is not PC" },
            { { 0x1F, 0x05 }, 1, 1, "6309 TFR D,PC" },
        };
        unsigned pos;
        for (pos = 0; pos < sizeof cases / sizeof cases[0]; pos++) {
            cocodebug_trace_entry ent = make_entry(0x0600, cases[pos].cpu6309, cases[pos].bytes, 6);
            CHECK(!!cocodebug_trace_match(&ent, COCODEBUG_TRACE_BRANCHES, 0, 0) == cases[pos].branch,
                  cases[pos].what);
        }
    }
    {
        static const uint8_t nop[] = { 0x12 };
        cocodebug_trace_entry ent = make_entry(0x0433, 0, nop, 1);
        CHECK(cocodebug_trace_match(&ent, COCODEBUG_TRACE_ALL, 0, 0), "all");
        CHECK(cocodebug_trace_match(&ent, COCODEBUG_TRACE_RANGE, 0x0400, 0x04FF), "in range");
        CHECK(!cocodebug_trace_match(&ent, COCODEBUG_TRACE_RANGE, 0x0500, 0x05FF), "out of range");
        CHECK(cocodebug_trace_match(&ent, COCODEBUG_TRACE_RANGE, 0x04FF, 0x0400), "the range swaps");
        ent.a = 0x41; ent.cc = 0x50; ent.s = 0x7F00;
        cocodebug_trace_format(&ent, buf, sizeof buf);
        CHECK(!strncmp(buf, "0433  NOP", 9) && strstr(buf, "A=41") && strstr(buf, "S=7F00") &&
              strstr(buf, "CC=50 .F.I...."), "trace format");
    }

    /* ---- breakpoint text ---- */
    {
        cocodebug_breakpoint bp;
        memset(&bp, 0, sizeof bp);
        bp.type = COCODEBUG_BP_WRITE; bp.start = 0x0400; bp.end = 0x05FF;
        snprintf(bp.condition, sizeof bp.condition, "value != old");
        cocobp_describe(NULL, &bp, buf, sizeof buf);
        CHECK(!strcmp(buf, "Write  $0400-$05FF  if value != old"), "write bp text");
        bp.type = COCODEBUG_BP_ACCESS; bp.start = bp.end = 0x0400; bp.condition[0] = 0;
        cocobp_describe(NULL, &bp, buf, sizeof buf);
        CHECK(!strcmp(buf, "Access  $0400"), "access bp text");
        bp.type = COCODEBUG_BP_EXEC | COCODEBUG_BP_WRITE; bp.start = bp.end = 0x0810;
        cocobp_describe(NULL, &bp, buf, sizeof buf);
        CHECK(!strcmp(buf, "Exec/Write  $0810"), "mixed bp text");
        bp.type = COCODEBUG_BP_WRITE | COCODEBUG_BP_PHYS; bp.start = bp.end = 0x1F400;
        cocobp_describe(NULL, &bp, buf, sizeof buf);
        CHECK(!strcmp(buf, "Write  phys $01F400"), "physical bp text");
    }

    /* ---- region names ---- */
    {
        static const cocodebug_region map[] = {
            { 0x0000, 0x7FFF, COCODEBUG_REGION_RAM, 1, -1, "RAM" },
            { 0x8000, 0xFEFF, COCODEBUG_REGION_ROM, 0, -1, "BASIC ROM" },
        };
        CHECK(cocomem_region_name(map, 2, 0x9000, buf, sizeof buf) > 0 && !strcmp(buf, "BASIC ROM"),
              "region name");
        CHECK(cocomem_region_name(map, 2, 0xFF00, buf, sizeof buf) == 0 && !buf[0], "no region");
        CHECK(!strcmp(cocomem_space_name(COCODEBUG_SPACE_PHYS), "PHYS"), "space names");
    }

    printf(fails ? "memtools_test: %d failed\n" : "memtools_test: ok\n", fails);
    return fails != 0;
}
