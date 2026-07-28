/*
 * A line-oriented 6809 disassembler.
 *
 * The debugger engine and all four frontends consume one shape --
 * coco_dasm_line -- so the disassembly list, the "step over" target and the
 * follow-the-branch affordance all read the same decode rather than each
 * parsing text.
 *
 * The opcode tables are derived from XRoar's src/mc6809/mc6809_trace.c
 * (GPL-3.0-or-later, same licence as this project). The formatting and this
 * API are not: the tracer prints a fixed line with CPU state appended, which
 * is a different job. See COMPLIANCE.md.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef COCO_M6809DASM_H
#define COCO_M6809DASM_H

#include <stdint.h>

/* Longest real encoding is 5 bytes (prefix + opcode + indexed postbyte +
 * 16-bit offset); one spare so a truncated read still has somewhere to go. */
#define COCO_DASM_MAX_BYTES 6

enum {
    /* The instruction transfers control somewhere this line can name, so a
     * frontend can offer to follow it and the engine can set a temporary
     * breakpoint there for "step over". */
    COCO_DASM_BRANCH = 1 << 0,
    /* ...and it is a subroutine call (BSR/LBSR/JSR), which is the case
     * "step over" actually cares about. */
    COCO_DASM_CALL = 1 << 1,
    /* Control does not continue to the next instruction (RTS/RTI/JMP/BRA). */
    COCO_DASM_NOFLOW = 1 << 2,
    /* Not a legal 6809 opcode, or one of the undocumented ones XRoar marks
     * with a trailing '*'. Shown, but never followed. */
    COCO_DASM_ILLEGAL = 1 << 3,
};

typedef struct {
    uint16_t addr;                     /* where the instruction starts */
    uint8_t len;                       /* its length in bytes, >= 1 */
    uint8_t bytes[COCO_DASM_MAX_BYTES];
    char text[40];                     /* "LDA   $1f,X" */
    char mnemonic[8];
    char operand[32];
    uint16_t target;                   /* valid when COCO_DASM_BRANCH */
    unsigned flags;
} coco_dasm_line;

/* Reader for the bytes to decode. Returning a byte for any address is fine;
 * the disassembler never reads past the instruction it is decoding. */
typedef uint8_t (*coco_dasm_read)(uint16_t addr, void *user);

typedef enum {
    COCO_DASM_CPU_MC6809 = 0,
    COCO_DASM_CPU_HD6309 = 1,
} coco_dasm_cpu;

/* Decode one instruction at addr for the given CPU. The HD6309 is a superset
 * of the 6809 -- same three opcode pages (no prefix, $10, $11), but several
 * previously-illegal slots and postbyte values become real instructions, so
 * this genuinely needs to know which chip it is decoding for rather than
 * just decoding "the union of both": a byte pattern a real 6809 would show
 * as illegal must still show as illegal when that is the machine running.
 * Always fills out and always reports a length of at least 1, so a caller
 * walking a range cannot get stuck on an illegal opcode. Returns the
 * length. */
int coco_dasm_one_cpu(uint16_t addr, coco_dasm_read read, void *user,
                      coco_dasm_cpu cpu, coco_dasm_line *out);

/* Convenience for the common case: decode for a plain 6809. */
int coco_dasm_one(uint16_t addr, coco_dasm_read read, void *user,
                  coco_dasm_line *out);

/* Convenience for a flat buffer: `mem` is 64K of address space. */
int coco_dasm_one_mem(uint16_t addr, const uint8_t *mem, coco_dasm_line *out);
int coco_dasm_one_mem_cpu(uint16_t addr, const uint8_t *mem, coco_dasm_cpu cpu,
                          coco_dasm_line *out);

#endif /* COCO_M6809DASM_H */
