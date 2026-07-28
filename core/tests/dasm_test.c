/*
 * dasm_test -- the 6809/6309 disassembler.
 *
 * Two kinds of check. The named cases below pin down one encoding each,
 * chosen for the things that are easy to get wrong: page prefixes, the
 * indexed postbyte's many shapes, PC-relative offsets resolving against the
 * END of the instruction, the push/pull register lists, and -- for the
 * HD6309 -- every new addressing mode (INMEM_*, MEMBIT, QUAD_IMMEDIATE, the
 * four TFM variants) plus the fact that a byte pattern only valid on the
 * 6309 must still decode as illegal when asked for as a plain 6809: this is
 * two decoders sharing one dispatcher, not "the union of both chips".
 *
 * Then a sweep over all 65536 two-byte openings, for BOTH chips, which
 * asserts only the invariant that matters to a caller walking a range: every
 * opcode decodes to a length of at least 1, so a disassembly list can never
 * get stuck.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "m6809dasm.h"

static int failures;
static uint8_t mem[0x10000];

/* Decode `bytes` placed at `addr` and compare the rendered line. */
static void expect(const char *what, uint16_t addr, const char *bytes,
                   int nbytes, const char *want_text, int want_len)
{
    coco_dasm_line line;
    int i;

    memset(mem, 0, sizeof(mem));
    for (i = 0; i < nbytes; i++)
        mem[(uint16_t)(addr + i)] = (uint8_t)bytes[i];

    coco_dasm_one_mem(addr, mem, &line);
    if (strcmp(line.text, want_text) != 0) {
        printf("FAIL %s: \"%s\", expected \"%s\"\n", what, line.text,
               want_text);
        failures++;
        return;
    }
    if (line.len != want_len) {
        printf("FAIL %s: length %d, expected %d\n", what, line.len, want_len);
        failures++;
        return;
    }
    printf("ok   %s -> %s\n", what, line.text);
}

/* Same as expect(), but for a specific CPU -- used for the HD6309 cases and
 * for confirming a 6309-only encoding is NOT decoded as one on a plain
 * 6809. */
static void expect_cpu(const char *what, coco_dasm_cpu cpu, uint16_t addr,
                       const char *bytes, int nbytes, const char *want_text,
                       int want_len)
{
    coco_dasm_line line;
    int i;

    memset(mem, 0, sizeof(mem));
    for (i = 0; i < nbytes; i++)
        mem[(uint16_t)(addr + i)] = (uint8_t)bytes[i];

    coco_dasm_one_mem_cpu(addr, mem, cpu, &line);
    if (strcmp(line.text, want_text) != 0) {
        printf("FAIL %s: \"%s\", expected \"%s\"\n", what, line.text,
               want_text);
        failures++;
        return;
    }
    if (line.len != want_len) {
        printf("FAIL %s: length %d, expected %d\n", what, line.len, want_len);
        failures++;
        return;
    }
    printf("ok   %s -> %s\n", what, line.text);
}

static void expect_flags(const char *what, const char *bytes, int nbytes,
                         unsigned want_flags, uint16_t want_target)
{
    coco_dasm_line line;
    int i;

    memset(mem, 0, sizeof(mem));
    for (i = 0; i < nbytes; i++)
        mem[0x1000 + i] = (uint8_t)bytes[i];
    coco_dasm_one_mem(0x1000, mem, &line);

    if (line.flags != want_flags) {
        printf("FAIL %s: flags %u, expected %u\n", what, line.flags,
               want_flags);
        failures++;
        return;
    }
    if (want_target && line.target != want_target) {
        printf("FAIL %s: target $%04x, expected $%04x\n", what, line.target,
               want_target);
        failures++;
        return;
    }
    printf("ok   %s\n", what);
}

int main(void)
{
    coco_dasm_line line;
    unsigned op, post;
    int stuck = 0;

    /* --- the plain addressing modes ------------------------------------- */
    expect("inherent", 0x1000, "\x39", 1, "RTS", 1);
    expect("immediate", 0x1000, "\x86\x42", 2, "LDA   #$42", 2);
    expect("16-bit immediate", 0x1000, "\x8e\x12\x34", 3, "LDX   #$1234", 3);
    expect("direct", 0x1000, "\x96\x20", 2, "LDA   <$20", 2);
    expect("extended", 0x1000, "\xb6\xff\x00", 3, "LDA   $ff00", 3);

    /* --- page prefixes --------------------------------------------------
     * $10 and $11 are not instructions; the byte after selects the page. */
    expect("page 1 ($10)", 0x1000, "\x10\x8e\x40\x00", 4, "LDY   #$4000", 4);
    expect("page 2 ($11)", 0x1000, "\x11\x83\x12\x34", 4, "CMPU  #$1234", 4);

    /* --- relative branches resolve against the END of the instruction --- */
    expect("short branch forwards", 0x1000, "\x20\x10", 2, "BRA   $1012", 2);
    expect("short branch backwards", 0x1000, "\x20\xfe", 2, "BRA   $1000", 2);
    expect("long branch", 0x1000, "\x16\x01\x00", 3, "LBRA  $1103", 3);
    /* Wrapping the 16-bit address space is legal and must not be clamped. */
    expect("a branch wraps at $ffff", 0xfffe, "\x20\x10", 2, "BRA   $0010", 2);

    /* --- indexed postbytes ---------------------------------------------- */
    expect("5-bit offset", 0x1000, "\xa6\x01", 2, "LDA   1,X", 2);
    expect("5-bit negative offset", 0x1000, "\xa6\x1f", 2, "LDA   -1,X", 2);
    expect("no offset", 0x1000, "\xa6\x84", 2, "LDA   ,X", 2);
    expect("post-increment by 1", 0x1000, "\xa6\x80", 2, "LDA   ,X+", 2);
    expect("post-increment by 2", 0x1000, "\xa6\x81", 2, "LDA   ,X++", 2);
    expect("pre-decrement by 1", 0x1000, "\xa6\x82", 2, "LDA   ,-X", 2);
    expect("pre-decrement by 2", 0x1000, "\xa6\x83", 2, "LDA   ,--X", 2);
    expect("accumulator offset", 0x1000, "\xa6\x86", 2, "LDA   A,X", 2);
    expect("D offset", 0x1000, "\xa6\x8b", 2, "LDA   D,X", 2);
    expect("8-bit offset", 0x1000, "\xa6\x88\x10", 3, "LDA   $10,X", 3);
    expect("8-bit negative offset", 0x1000, "\xa6\x88\xf0", 3,
           "LDA   -$10,X", 3);
    expect("16-bit offset", 0x1000, "\xa6\x89\x12\x34", 4, "LDA   $1234,X", 4);
    /* The base register comes from bits 5-6. */
    expect("indexed off Y", 0x1000, "\xa6\xa4", 2, "LDA   ,Y", 2);
    expect("indexed off U", 0x1000, "\xa6\xc4", 2, "LDA   ,U", 2);
    expect("indexed off S", 0x1000, "\xa6\xe4", 2, "LDA   ,S", 2);
    /* Bit 4 makes it indirect. */
    expect("indirect", 0x1000, "\xa6\x94", 2, "LDA   [,X]", 2);
    expect("indirect extended", 0x1000, "\xa6\x9f\x20\x00", 4,
           "LDA   [$2000]", 4);
    /* PCR is relative to the address AFTER the whole instruction -- the
     * classic off-by-the-operand-length bug. */
    expect("8-bit PCR", 0x1000, "\xa6\x8c\x10", 3, "LDA   $1013,PCR", 3);
    expect("16-bit PCR", 0x1000, "\xa6\x8d\x01\x00", 4, "LDA   $1104,PCR", 4);

    /* --- register lists -------------------------------------------------- */
    expect("push one register", 0x1000, "\x34\x02", 2, "PSHS  A", 2);
    /* Bit order is CC,A,B,DP,X,Y,U/S,PC -- not the bit numbering. */
    expect("push several", 0x1000, "\x34\x06", 2, "PSHS  A,B", 2);
    expect("push everything", 0x1000, "\x34\xff", 2,
           "PSHS  CC,A,B,DP,X,Y,U,PC", 2);
    /* PSHS names U in that slot; PSHU names S. */
    expect("PSHU names S, not U", 0x1000, "\x36\x40", 2, "PSHU  S", 2);
    expect("PSHS names U", 0x1000, "\x34\x40", 2, "PSHS  U", 2);
    expect("an empty register list", 0x1000, "\x34\x00", 2, "PSHS", 2);

    /* --- inter-register -------------------------------------------------- */
    expect("TFR", 0x1000, "\x1f\x89", 2, "TFR   A,B", 2);
    expect("EXG", 0x1000, "\x1e\x01", 2, "EXG   D,X", 2);

    /* --- flow flags ------------------------------------------------------ */
    /* BRA always goes; it does not continue to the next instruction. */
    expect_flags("BRA does not continue", "\x20\x10", 2, COCO_DASM_NOFLOW, 0);
    expect_flags("BSR is a call", "\x8d\x10", 2,
                 COCO_DASM_BRANCH | COCO_DASM_CALL, 0x1012);
    expect_flags("JSR extended is a call", "\xbd\x20\x00", 3,
                 COCO_DASM_BRANCH | COCO_DASM_CALL, 0x2000);
    expect_flags("BEQ is a conditional branch", "\x27\x04", 2,
                 COCO_DASM_BRANCH, 0x1006);
    expect_flags("RTS does not continue", "\x39", 1, COCO_DASM_NOFLOW, 0);
    /* An indexed JSR's target depends on the registers, so the line must not
     * claim one a frontend would then offer to jump to. */
    expect_flags("indexed JSR claims no target", "\xad\x84", 2,
                 COCO_DASM_CALL, 0);
    /* Undocumented opcodes are shown but never followed. */
    expect_flags("an undocumented opcode is flagged", "\x01\x20", 2,
                 COCO_DASM_ILLEGAL, 0);
    expect_flags("an illegal opcode is flagged", "\x41", 1,
                 COCO_DASM_ILLEGAL, 0);

    /* --- HD6309: new addressing modes ------------------------------------
     * The HD6309 shares the 6809's three-page layout, so page prefixes and
     * ordinary addressing still work unchanged (proven by the tests above,
     * which do not need repeating per-chip); what needs its own coverage is
     * every addressing mode the 6809 does not have at all. */
    expect_cpu("OIM (INMEM_DIRECT)", COCO_DASM_CPU_HD6309, 0x1000,
              "\x01\x0f\x20", 3, "OIM   #$0f,<$20", 3);
    expect_cpu("OIM (INMEM_INDEXED)", COCO_DASM_CPU_HD6309, 0x1000,
              "\x61\x0f\x84", 3, "OIM   #$0f,,X", 3);
    expect_cpu("OIM (INMEM_EXTENDED)", COCO_DASM_CPU_HD6309, 0x1000,
              "\x71\x0f\x20\x00", 4, "OIM   #$0f,$2000", 4);
    expect_cpu("LDQ (QUAD_IMMEDIATE)", COCO_DASM_CPU_HD6309, 0x1000,
              "\xcd\x12\x34\x56\x78", 5, "LDQ   #$12345678", 5);
    /* postbyte $4a: reg=(bits7-6)=01->A, sbit=(bits5-3)=001=1, dbit=1-0=010=2 */
    expect_cpu("BAND (MEMBIT)", COCO_DASM_CPU_HD6309, 0x1000,
              "\x11\x30\x4a\x20", 4, "BAND  A,1,2,<$20", 4);
    /* The four TFM variants are picked by opcode ($38-$3b on page 2), not by
     * postbyte -- easy to get backwards. */
    expect_cpu("TFM R+,R+", COCO_DASM_CPU_HD6309, 0x1000, "\x11\x38\x12", 3,
              "TFM   X+,Y+", 3);
    expect_cpu("TFM R-,R-", COCO_DASM_CPU_HD6309, 0x1000, "\x11\x39\x12", 3,
              "TFM   X-,Y-", 3);
    expect_cpu("TFM R+,R", COCO_DASM_CPU_HD6309, 0x1000, "\x11\x3a\x12", 3,
              "TFM   X+,Y", 3);
    expect_cpu("TFM R,R+", COCO_DASM_CPU_HD6309, 0x1000, "\x11\x3b\x12", 3,
              "TFM   X,Y+", 3);
    expect_cpu("SEXW", COCO_DASM_CPU_HD6309, 0x1000, "\x14", 1, "SEXW", 1);
    /* New indexed accumulator offsets, filling nibbles the 6809 leaves
     * illegal (postbyte low nibble 7/a/e). */
    expect_cpu("E accumulator offset", COCO_DASM_CPU_HD6309, 0x1000,
              "\xa6\x87", 2, "LDA   E,X", 2);
    expect_cpu("F accumulator offset", COCO_DASM_CPU_HD6309, 0x1000,
              "\xa6\x8a", 2, "LDA   F,X", 2);
    expect_cpu("W accumulator offset", COCO_DASM_CPU_HD6309, 0x1000,
              "\xa6\x8e", 2, "LDA   W,X", 2);
    /* W as the BASE register is not bits 5-6 selecting it like X/Y/U/S --
     * it is specific full postbyte values hijacking otherwise-valid 6809
     * encodings, which is the one place this needed its own override
     * rather than falling out of the existing nibble switch. */
    expect_cpu(",W", COCO_DASM_CPU_HD6309, 0x1000, "\xa6\x8f", 2, "LDA   ,W",
              2);
    expect_cpu("16-bit offset,W", COCO_DASM_CPU_HD6309, 0x1000,
              "\xa6\xaf\x12\x34", 4, "LDA   $1234,W", 4);
    expect_cpu(",W++", COCO_DASM_CPU_HD6309, 0x1000, "\xa6\xcf", 2,
              "LDA   ,W++", 2);
    expect_cpu(",--W", COCO_DASM_CPU_HD6309, 0x1000, "\xa6\xef", 2,
              "LDA   ,--W", 2);
    /* TFR/EXG's register field is a full 16 entries on the 6309 (adds W, V,
     * and a literal zero register at what the 6809 leaves undefined). */
    expect_cpu("TFR W,V", COCO_DASM_CPU_HD6309, 0x1000, "\x1f\x67", 2,
              "TFR   W,V", 2);
    expect_cpu("TFR to the zero register", COCO_DASM_CPU_HD6309, 0x1000,
              "\x1f\xc8", 2, "TFR   0,A", 2);
    /* Page 1 register-to-register ops are entirely new on the 6309. */
    expect_cpu("ADDR", COCO_DASM_CPU_HD6309, 0x1000, "\x10\x30\x89", 3,
              "ADDR  A,B", 3);

    /* --- fidelity: a byte pattern only valid on one chip must not decode
     * as though it were the other ------------------------------------------
     * This is the property the whole two-table design exists for: it would
     * be easy (and wrong) to just decode "whatever either chip supports". */
    expect_cpu("6309's OIM is a real 6809 opcode too, just a different one",
              COCO_DASM_CPU_MC6809, 0x1000, "\x01\x20", 2, "NEG*  <$20", 2);
    expect_cpu("a 6309-only indexed nibble is illegal on a real 6809",
              COCO_DASM_CPU_MC6809, 0x1000, "\xa6\x87", 2, "LDA   ??", 2);

    /* --- nothing can wedge a caller walking a range, on either chip ------ */
    {
        static const coco_dasm_cpu cpus[2] = { COCO_DASM_CPU_MC6809,
                                               COCO_DASM_CPU_HD6309 };
        int c;

        for (c = 0; c < 2 && !stuck; c++) {
            for (op = 0; op < 256 && !stuck; op++) {
                for (post = 0; post < 256; post++) {
                    memset(mem, 0, sizeof(mem));
                    mem[0x1000] = (uint8_t)op;
                    mem[0x1001] = (uint8_t)post;
                    coco_dasm_one_mem_cpu(0x1000, mem, cpus[c], &line);
                    if (line.len < 1 || line.len > COCO_DASM_MAX_BYTES) {
                        printf("FAIL sweep (cpu %d): $%02x $%02x gave "
                               "length %d\n", c, op, post, line.len);
                        failures++;
                        stuck = 1;
                        break;
                    }
                    if (line.text[0] == '\0') {
                        printf("FAIL sweep (cpu %d): $%02x $%02x rendered "
                               "nothing\n", c, op, post);
                        failures++;
                        stuck = 1;
                        break;
                    }
                }
            }
        }
    }
    if (!stuck)
        printf("ok   all 65536 two-byte openings decode to a usable length, "
               "on both chips\n");

    if (failures) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    printf("\nall disassembler tests passed\n");
    return 0;
}
