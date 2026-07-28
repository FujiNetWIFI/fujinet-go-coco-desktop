/*
 * A line-oriented 6809 disassembler.
 *
 * The opcode tables below are derived from XRoar's src/mc6809/mc6809_trace.c
 * -- extracted mechanically rather than retyped, so they cannot drift from
 * upstream's by a typo. Both are GPL-3.0-or-later. The decoding and
 * formatting are written for this API: the tracer emits a fixed line with CPU
 * state appended, which is a different job. See COMPLIANCE.md.
 *
 * XRoar marks undocumented opcodes with a trailing '*' in the mnemonic; that
 * convention is kept, and such lines are flagged COCO_DASM_ILLEGAL so nothing
 * offers to follow them.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "m6809dasm.h"

/* Addressing modes, matching upstream's names so the tables transplant. */
enum {
    ILLEGAL = 0, INHERENT, WORD_IMMEDIATE, IMMEDIATE, EXTENDED, DIRECT,
    INDEXED, RELATIVE, LONG_RELATIVE, STACKS, STACKU, REGISTER,
};

static const struct {
    const char *mnemonic;
    int type;
} instructions[3][256] = {
    { /* PAGE 0 (no prefix) */
        /* 00 */ { "NEG", DIRECT }, { "NEG*", DIRECT }, { "NGC*", DIRECT }, { "COM", DIRECT },
        /* 04 */ { "LSR", DIRECT }, { "LSR*", DIRECT }, { "ROR", DIRECT }, { "ASR", DIRECT },
        /* 08 */ { "LSL", DIRECT }, { "ROL", DIRECT }, { "DEC", DIRECT }, { "DEC*", DIRECT },
        /* 0c */ { "INC", DIRECT }, { "TST", DIRECT }, { "JMP", DIRECT }, { "CLR", DIRECT },
        /* 10 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "NOP", INHERENT }, { "SYNC", INHERENT },
        /* 14 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "LBRA", LONG_RELATIVE }, { "LBSR", LONG_RELATIVE },
        /* 18 */ { "LACC*", ILLEGAL }, { "DAA", INHERENT }, { "ORCC", IMMEDIATE }, { NULL, ILLEGAL },
        /* 1c */ { "ANDCC", IMMEDIATE }, { "SEX", INHERENT }, { "EXG", REGISTER }, { "TFR", REGISTER },
        /* 20 */ { "BRA", RELATIVE }, { "BRN", RELATIVE }, { "BHI", RELATIVE }, { "BLS", RELATIVE },
        /* 24 */ { "BCC", RELATIVE }, { "BCS", RELATIVE }, { "BNE", RELATIVE }, { "BEQ", RELATIVE },
        /* 28 */ { "BVC", RELATIVE }, { "BVS", RELATIVE }, { "BPL", RELATIVE }, { "BMI", RELATIVE },
        /* 2c */ { "BGE", RELATIVE }, { "BLT", RELATIVE }, { "BGT", RELATIVE }, { "BLE", RELATIVE },
        /* 30 */ { "LEAX", INDEXED }, { "LEAY", INDEXED }, { "LEAS", INDEXED }, { "LEAU", INDEXED },
        /* 34 */ { "PSHS", STACKS }, { "PULS", STACKS }, { "PSHU", STACKU }, { "PULU", STACKU },
        /* 38 */ { NULL, ILLEGAL }, { "RTS", INHERENT }, { "ABX", INHERENT }, { "RTI", INHERENT },
        /* 3c */ { "CWAI", IMMEDIATE }, { "MUL", INHERENT }, { NULL, ILLEGAL }, { "SWI", INHERENT },
        /* 40 */ { "NEGA", INHERENT }, { "NEGA*", INHERENT }, { "NGCA*", INHERENT }, { "COMA", INHERENT },
        /* 44 */ { "LSRA", INHERENT }, { "LSRA*", INHERENT }, { "RORA", INHERENT }, { "ASRA", INHERENT },
        /* 48 */ { "LSLA", INHERENT }, { "ROLA", INHERENT }, { "DECA", INHERENT }, { "DECA*", INHERENT },
        /* 4c */ { "INCA", INHERENT }, { "TSTA", INHERENT }, { "CLA2*", INHERENT }, { "CLRA", INHERENT },
        /* 50 */ { "NEGB", INHERENT }, { "NEGB*", INHERENT }, { "NGCB*", INHERENT }, { "COMB", INHERENT },
        /* 54 */ { "LSRB", INHERENT }, { "LSRB*", INHERENT }, { "RORB", INHERENT }, { "ASRB", INHERENT },
        /* 58 */ { "LSLB", INHERENT }, { "ROLB", INHERENT }, { "DECB", INHERENT }, { "DECB*", INHERENT },
        /* 5c */ { "INCB", INHERENT }, { "TSTB", INHERENT }, { "CLB2*", INHERENT }, { "CLRB", INHERENT },
        /* 60 */ { "NEG", INDEXED }, { "NEG*", INDEXED }, { "NGC*", INDEXED }, { "COM", INDEXED },
        /* 64 */ { "LSR", INDEXED }, { "LSR*", INDEXED }, { "ROR", INDEXED }, { "ASR", INDEXED },
        /* 68 */ { "LSL", INDEXED }, { "ROL", INDEXED }, { "DEC", INDEXED }, { "DEC*", INDEXED },
        /* 6c */ { "INC", INDEXED }, { "TST", INDEXED }, { "JMP", INDEXED }, { "CLR", INDEXED },
        /* 70 */ { "NEG", EXTENDED }, { "NEG*", EXTENDED }, { "NGC*", EXTENDED }, { "COM", EXTENDED },
        /* 74 */ { "LSR", EXTENDED }, { "LSR*", EXTENDED }, { "ROR", EXTENDED }, { "ASR", EXTENDED },
        /* 78 */ { "LSL", EXTENDED }, { "ROL", EXTENDED }, { "DEC", EXTENDED }, { "DEC*", EXTENDED },
        /* 7c */ { "INC", EXTENDED }, { "TST", EXTENDED }, { "JMP", EXTENDED }, { "CLR", EXTENDED },
        /* 80 */ { "SUBA", IMMEDIATE }, { "CMPA", IMMEDIATE }, { "SBCA", IMMEDIATE }, { "SUBD", WORD_IMMEDIATE },
        /* 84 */ { "ANDA", IMMEDIATE }, { "BITA", IMMEDIATE }, { "LDA", IMMEDIATE }, { "DSCA*", IMMEDIATE },
        /* 88 */ { "EORA", IMMEDIATE }, { "ADCA", IMMEDIATE }, { "ORA", IMMEDIATE }, { "ADDA", IMMEDIATE },
        /* 8c */ { "CMPX", WORD_IMMEDIATE }, { "BSR", RELATIVE }, { "LDX", WORD_IMMEDIATE }, { "STXI*", IMMEDIATE },
        /* 90 */ { "SUBA", DIRECT }, { "CMPA", DIRECT }, { "SBCA", DIRECT }, { "SUBD", DIRECT },
        /* 94 */ { "ANDA", DIRECT }, { "BITA", DIRECT }, { "LDA", DIRECT }, { "STA", DIRECT },
        /* 98 */ { "EORA", DIRECT }, { "ADCA", DIRECT }, { "ORA", DIRECT }, { "ADDA", DIRECT },
        /* 9c */ { "CMPX", DIRECT }, { "JSR", DIRECT }, { "LDX", DIRECT }, { "STX", DIRECT },
        /* a0 */ { "SUBA", INDEXED }, { "CMPA", INDEXED }, { "SBCA", INDEXED }, { "SUBD", INDEXED },
        /* a4 */ { "ANDA", INDEXED }, { "BITA", INDEXED }, { "LDA", INDEXED }, { "STA", INDEXED },
        /* a8 */ { "EORA", INDEXED }, { "ADCA", INDEXED }, { "ORA", INDEXED }, { "ADDA", INDEXED },
        /* ac */ { "CMPX", INDEXED }, { "JSR", INDEXED }, { "LDX", INDEXED }, { "STX", INDEXED },
        /* b0 */ { "SUBA", EXTENDED }, { "CMPA", EXTENDED }, { "SBCA", EXTENDED }, { "SUBD", EXTENDED },
        /* b4 */ { "ANDA", EXTENDED }, { "BITA", EXTENDED }, { "LDA", EXTENDED }, { "STA", EXTENDED },
        /* b8 */ { "EORA", EXTENDED }, { "ADCA", EXTENDED }, { "ORA", EXTENDED }, { "ADDA", EXTENDED },
        /* bc */ { "CMPX", EXTENDED }, { "JSR", EXTENDED }, { "LDX", EXTENDED }, { "STX", EXTENDED },
        /* c0 */ { "SUBB", IMMEDIATE }, { "CMPB", IMMEDIATE }, { "SBCB", IMMEDIATE }, { "ADDD", WORD_IMMEDIATE },
        /* c4 */ { "ANDB", IMMEDIATE }, { "BITB", IMMEDIATE }, { "LDB", IMMEDIATE }, { "DSCB*", IMMEDIATE },
        /* c8 */ { "EORB", IMMEDIATE }, { "ADCB", IMMEDIATE }, { "ORB", IMMEDIATE }, { "ADDB", IMMEDIATE },
        /* cc */ { "LDD", WORD_IMMEDIATE }, { "HCF*", INHERENT }, { "LDU", WORD_IMMEDIATE }, { "STUI*", IMMEDIATE },
        /* d0 */ { "SUBB", DIRECT }, { "CMPB", DIRECT }, { "SBCB", DIRECT }, { "ADDD", DIRECT },
        /* d4 */ { "ANDB", DIRECT }, { "BITB", DIRECT }, { "LDB", DIRECT }, { "STB", DIRECT },
        /* d8 */ { "EORB", DIRECT }, { "ADCB", DIRECT }, { "ORB", DIRECT }, { "ADDB", DIRECT },
        /* dc */ { "LDD", DIRECT }, { "STD", DIRECT }, { "LDU", DIRECT }, { "STU", DIRECT },
        /* e0 */ { "SUBB", INDEXED }, { "CMPB", INDEXED }, { "SBCB", INDEXED }, { "ADDD", INDEXED },
        /* e4 */ { "ANDB", INDEXED }, { "BITB", INDEXED }, { "LDB", INDEXED }, { "STB", INDEXED },
        /* e8 */ { "EORB", INDEXED }, { "ADCB", INDEXED }, { "ORB", INDEXED }, { "ADDB", INDEXED },
        /* ec */ { "LDD", INDEXED }, { "STD", INDEXED }, { "LDU", INDEXED }, { "STU", INDEXED },
        /* f0 */ { "SUBB", EXTENDED }, { "CMPB", EXTENDED }, { "SBCB", EXTENDED }, { "ADDD", EXTENDED },
        /* f4 */ { "ANDB", EXTENDED }, { "BITB", EXTENDED }, { "LDB", EXTENDED }, { "STB", EXTENDED },
        /* f8 */ { "EORB", EXTENDED }, { "ADCB", EXTENDED }, { "ORB", EXTENDED }, { "ADDB", EXTENDED },
        /* fc */ { "LDD", EXTENDED }, { "STD", EXTENDED }, { "LDU", EXTENDED }, { "STU", EXTENDED },
    },
    { /* PAGE 1 (prefix $10) */
        /* 00 */ { "NEG*", DIRECT }, { "NEG*", DIRECT }, { "NGC*", DIRECT }, { "COM*", DIRECT },
        /* 04 */ { "LSR*", DIRECT }, { "LSR*", DIRECT }, { "ROR*", DIRECT }, { "ASR*", DIRECT },
        /* 08 */ { "LSL*", DIRECT }, { "ROL*", DIRECT }, { "DEC*", DIRECT }, { "DEC*", DIRECT },
        /* 0c */ { "INC*", DIRECT }, { "TST*", DIRECT }, { NULL, ILLEGAL }, { "CLR*", DIRECT },
        /* 10 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 14 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 18 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 1c */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 20 */ { "LBRA*", LONG_RELATIVE }, { "LBRN", LONG_RELATIVE }, { "LBHI", LONG_RELATIVE }, { "LBLS", LONG_RELATIVE },
        /* 24 */ { "LBCC", LONG_RELATIVE }, { "LBCS", LONG_RELATIVE }, { "LBNE", LONG_RELATIVE }, { "LBEQ", LONG_RELATIVE },
        /* 28 */ { "LBVC", LONG_RELATIVE }, { "LBVS", LONG_RELATIVE }, { "LBPL", LONG_RELATIVE }, { "LBMI", LONG_RELATIVE },
        /* 2c */ { "LBGE", LONG_RELATIVE }, { "LBLT", LONG_RELATIVE }, { "LBGT", LONG_RELATIVE }, { "LBLE", LONG_RELATIVE },
        /* 30 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 34 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 38 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 3c */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "SWI2", INHERENT },
        /* 40 */ { "NEGA*", INHERENT }, { "NEGA*", INHERENT }, { "NGCA*", INHERENT }, { "COMA*", INHERENT },
        /* 44 */ { "LSRA*", INHERENT }, { "LSRA*", INHERENT }, { "RORA*", INHERENT }, { "ASRA*", INHERENT },
        /* 48 */ { "LSLA*", INHERENT }, { "ROLA*", INHERENT }, { "DECA*", INHERENT }, { "DECA*", INHERENT },
        /* 4c */ { "INCA*", INHERENT }, { "TSTA*", INHERENT }, { "CLA2*", INHERENT }, { "CLRA*", INHERENT },
        /* 50 */ { "NEGB*", INHERENT }, { "NEGB*", INHERENT }, { "NGCB*", INHERENT }, { "COMB*", INHERENT },
        /* 54 */ { "LSRB*", INHERENT }, { "LSRB*", INHERENT }, { "RORB*", INHERENT }, { "ASRB*", INHERENT },
        /* 58 */ { "LSLB*", INHERENT }, { "ROLB*", INHERENT }, { "DECB*", INHERENT }, { "DECB*", INHERENT },
        /* 5c */ { "INCB*", INHERENT }, { "TSTB*", INHERENT }, { "CLB2*", INHERENT }, { "CLRB*", INHERENT },
        /* 60 */ { "NEG*", INDEXED }, { "NEG*", INDEXED }, { "NGC*", INDEXED }, { "COM*", INDEXED },
        /* 64 */ { "LSR*", INDEXED }, { "LSR*", INDEXED }, { "ROR*", INDEXED }, { "ASR*", INDEXED },
        /* 68 */ { "LSL*", INDEXED }, { "ROL*", INDEXED }, { "DEC*", INDEXED }, { "DEC*", INDEXED },
        /* 6c */ { "INC*", INDEXED }, { "TST*", INDEXED }, { NULL, ILLEGAL }, { "CLR*", INDEXED },
        /* 70 */ { "NEG*", EXTENDED }, { "NEG*", EXTENDED }, { "NGC*", EXTENDED }, { "COM*", EXTENDED },
        /* 74 */ { "LSR*", EXTENDED }, { "LSR*", EXTENDED }, { "ROR*", EXTENDED }, { "ASR*", EXTENDED },
        /* 78 */ { "LSL*", EXTENDED }, { "ROL*", EXTENDED }, { "DEC*", EXTENDED }, { "DEC*", EXTENDED },
        /* 7c */ { "INC*", EXTENDED }, { "TST*", EXTENDED }, { NULL, ILLEGAL }, { "CLR*", EXTENDED },
        /* 80 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "CMPD", WORD_IMMEDIATE },
        /* 84 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 88 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 8c */ { "CMPY", WORD_IMMEDIATE }, { NULL, ILLEGAL }, { "LDY", WORD_IMMEDIATE }, { NULL, ILLEGAL },
        /* 90 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "CMPD", DIRECT },
        /* 94 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 98 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 9c */ { "CMPY", DIRECT }, { NULL, ILLEGAL }, { "LDY", DIRECT }, { "STY", DIRECT },
        /* a0 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "CMPD", INDEXED },
        /* a4 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* a8 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* ac */ { "CMPY", INDEXED }, { NULL, ILLEGAL }, { "LDY", INDEXED }, { "STY", INDEXED },
        /* b0 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "CMPD", EXTENDED },
        /* b4 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* b8 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* bc */ { "CMPY", EXTENDED }, { NULL, ILLEGAL }, { "LDY", EXTENDED }, { "STY", EXTENDED },
        /* c0 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* c4 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* c8 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* cc */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "LDS", WORD_IMMEDIATE }, { NULL, ILLEGAL },
        /* d0 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* d4 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* d8 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* dc */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "LDS", DIRECT }, { "STS", DIRECT },
        /* e0 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* e4 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* e8 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* ec */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "LDS", INDEXED }, { "STS", INDEXED },
        /* f0 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* f4 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* f8 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* fc */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "LDS", EXTENDED }, { "STS", EXTENDED },
    },
    { /* PAGE 2 (prefix $11) */
        /* 00 */ { "NEG*", DIRECT }, { "NEG*", DIRECT }, { "NGC*", DIRECT }, { "COM*", DIRECT },
        /* 04 */ { "LSR*", DIRECT }, { "LSR*", DIRECT }, { "ROR*", DIRECT }, { "ASR*", DIRECT },
        /* 08 */ { "LSL*", DIRECT }, { "ROL*", DIRECT }, { "DEC*", DIRECT }, { "DEC*", DIRECT },
        /* 0c */ { "INC*", DIRECT }, { "TST*", DIRECT }, { NULL, ILLEGAL }, { "CLR*", DIRECT },
        /* 10 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 14 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 18 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 1c */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 20 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 24 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 28 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 2c */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 30 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 34 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 38 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 3c */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "SWI3", INHERENT },
        /* 40 */ { "NEGA*", INHERENT }, { "NEGA*", INHERENT }, { "NGCA*", INHERENT }, { "COMA*", INHERENT },
        /* 44 */ { "LSRA*", INHERENT }, { "LSRA*", INHERENT }, { "RORA*", INHERENT }, { "ASRA*", INHERENT },
        /* 48 */ { "LSLA*", INHERENT }, { "ROLA*", INHERENT }, { "DECA*", INHERENT }, { "DECA*", INHERENT },
        /* 4c */ { "INCA*", INHERENT }, { "TSTA*", INHERENT }, { "CLA2*", INHERENT }, { "CLRA*", INHERENT },
        /* 50 */ { "NEGB*", INHERENT }, { "NEGB*", INHERENT }, { "NGCB*", INHERENT }, { "COMB*", INHERENT },
        /* 54 */ { "LSRB*", INHERENT }, { "LSRB*", INHERENT }, { "RORB*", INHERENT }, { "ASRB*", INHERENT },
        /* 58 */ { "LSLB*", INHERENT }, { "ROLB*", INHERENT }, { "DECB*", INHERENT }, { "DECB*", INHERENT },
        /* 5c */ { "INCB*", INHERENT }, { "TSTB*", INHERENT }, { "CLB2*", INHERENT }, { "CLRB*", INHERENT },
        /* 60 */ { "NEG*", INDEXED }, { "NEG*", INDEXED }, { "NGC*", INDEXED }, { "COM*", INDEXED },
        /* 64 */ { "LSR*", INDEXED }, { "LSR*", INDEXED }, { "ROR*", INDEXED }, { "ASR*", INDEXED },
        /* 68 */ { "LSL*", INDEXED }, { "ROL*", INDEXED }, { "DEC*", INDEXED }, { "DEC*", INDEXED },
        /* 6c */ { "INC*", INDEXED }, { "TST*", INDEXED }, { NULL, ILLEGAL }, { "CLR*", INDEXED },
        /* 70 */ { "NEG*", EXTENDED }, { "NEG*", EXTENDED }, { "NGC*", EXTENDED }, { "COM*", EXTENDED },
        /* 74 */ { "LSR*", EXTENDED }, { "LSR*", EXTENDED }, { "ROR*", EXTENDED }, { "ASR*", EXTENDED },
        /* 78 */ { "LSL*", EXTENDED }, { "ROL*", EXTENDED }, { "DEC*", EXTENDED }, { "DEC*", EXTENDED },
        /* 7c */ { "INC*", EXTENDED }, { "TST*", EXTENDED }, { NULL, ILLEGAL }, { "CLR*", EXTENDED },
        /* 80 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "CMPU", WORD_IMMEDIATE },
        /* 84 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 88 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 8c */ { "CMPS", WORD_IMMEDIATE }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 90 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "CMPU", DIRECT },
        /* 94 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 98 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 9c */ { "CMPS", DIRECT }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* a0 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "CMPU", INDEXED },
        /* a4 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* a8 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* ac */ { "CMPS", INDEXED }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* b0 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "CMPU", EXTENDED },
        /* b4 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* b8 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* bc */ { "CMPS", EXTENDED }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* c0 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* c4 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* c8 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* cc */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* d0 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* d4 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* d8 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* dc */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* e0 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* e4 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* e8 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* ec */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* f0 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* f4 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* f8 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* fc */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
    },
};

/* Inter-register postbyte (TFR/EXG). */
static const char *const tfr_regs[16] = {
    "D", "X", "Y", "U", "S", "PC", "*", "*",
    "A", "B", "CC", "DP", "*", "*", "*", "*"
};

/* Indexed addressing postbyte base register. */
static const char *const idx_regs[4] = { "X", "Y", "U", "S" };

/* Reading state for one instruction: everything the decoder consumes goes
 * through here, so `len` is always exactly what was read and a caller
 * walking a range can never lose sync. */
typedef struct {
    uint16_t pc;            /* address of the opcode */
    unsigned n;             /* bytes consumed so far */
    coco_dasm_read read;
    void *user;
    coco_dasm_line *out;
} iter;

static uint8_t next_byte(iter *it)
{
    uint8_t b = it->read((uint16_t)(it->pc + it->n), it->user);
    if (it->n < COCO_DASM_MAX_BYTES)
        it->out->bytes[it->n] = b;
    it->n++;
    return b;
}

static unsigned next_word(iter *it)
{
    unsigned hi = next_byte(it);
    return (hi << 8) | next_byte(it);
}

static int sex5(unsigned v) { return (int)((v & 0x0f) - (v & 0x10)); }
static int sex8(unsigned v) { return (int)(int8_t)(uint8_t)v; }

/* PSHS/PULS name the U register; PSHU/PULU name S. Everything else about the
 * two postbytes is identical. */
static void stack_operand(char *dst, size_t dstsz, unsigned postbyte,
                         const char *reg6)
{
    static const char *const names[8] = {
        "CC", "A", "B", "DP", "X", "Y", NULL /* reg6 */, "PC"
    };
    size_t used = 0;
    int i;

    dst[0] = '\0';
    for (i = 0; i < 8; i++) {
        const char *name = (i == 6) ? reg6 : names[i];
        if (!(postbyte & (1u << i)))
            continue;
        used += (size_t)snprintf(dst + used, dstsz - used, "%s%s",
                                 used ? "," : "", name);
        if (used >= dstsz)
            return;
    }
}

/* An indexed postbyte, into `dst`. Consumes any offset bytes it needs. */
static void indexed_operand(iter *it, char *dst, size_t dstsz)
{
    unsigned postbyte = next_byte(it);
    const char *reg = idx_regs[(postbyte >> 5) & 3];
    const char *pre, *post;
    unsigned mode;

    if ((postbyte & 0x80) == 0) {
        /* 5-bit signed offset, packed into the postbyte itself. */
        snprintf(dst, dstsz, "%d,%s", sex5(postbyte), reg);
        return;
    }

    pre = (postbyte & 0x10) ? "[" : "";
    post = (postbyte & 0x10) ? "]" : "";
    mode = postbyte & 0x0f;

    switch (mode) {
    case 0x0: snprintf(dst, dstsz, "%s,%s+%s", pre, reg, post); break;
    case 0x1: snprintf(dst, dstsz, "%s,%s++%s", pre, reg, post); break;
    case 0x2: snprintf(dst, dstsz, "%s,-%s%s", pre, reg, post); break;
    case 0x3: snprintf(dst, dstsz, "%s,--%s%s", pre, reg, post); break;
    case 0x4: snprintf(dst, dstsz, "%s,%s%s", pre, reg, post); break;
    case 0x5: snprintf(dst, dstsz, "%sB,%s%s", pre, reg, post); break;
    case 0x6: snprintf(dst, dstsz, "%sA,%s%s", pre, reg, post); break;
    case 0x8: {
        int v = sex8(next_byte(it));
        snprintf(dst, dstsz, "%s%s$%02x,%s%s", pre, v < 0 ? "-" : "",
                 v < 0 ? -v : v, reg, post);
    } break;
    case 0x9: snprintf(dst, dstsz, "%s$%04x,%s%s", pre, next_word(it), reg,
                       post); break;
    case 0xb: snprintf(dst, dstsz, "%sD,%s%s", pre, reg, post); break;
    case 0xc: {
        /* PC-relative offsets resolve against the address AFTER the whole
         * instruction, which is why this is computed once the operand bytes
         * have been consumed. */
        int v = sex8(next_byte(it));
        snprintf(dst, dstsz, "%s$%04x,PCR%s", pre,
                 (uint16_t)(it->pc + it->n + v), post);
    } break;
    case 0xd: {
        unsigned v = next_word(it);
        snprintf(dst, dstsz, "%s$%04x,PCR%s", pre,
                 (uint16_t)(it->pc + it->n + v), post);
    } break;
    case 0xf: snprintf(dst, dstsz, "%s$%04x%s", pre, next_word(it), post);
        break;
    /* 0x7, 0xa and 0xe are not valid postbytes on a 6809. */
    default: snprintf(dst, dstsz, "%s??%s", pre, post); break;
    }
}

/* Which mnemonics change the flow, and how. Kept as a lookup on the text
 * rather than a fourth column in the tables, so the tables stay a verbatim
 * transplant of upstream's and can be re-extracted without merging by hand. */
static unsigned flow_flags(const char *mnemonic)
{
    static const char *const calls[] = { "BSR", "LBSR", "JSR", NULL };
    static const char *const noflow[] = { "RTS", "RTI", "JMP", "BRA", "LBRA",
                                          "SWI", "SWI2", "SWI3", NULL };
    int i;

    for (i = 0; calls[i]; i++)
        if (!strcmp(mnemonic, calls[i]))
            return COCO_DASM_BRANCH | COCO_DASM_CALL;
    for (i = 0; noflow[i]; i++)
        if (!strcmp(mnemonic, noflow[i]))
            return COCO_DASM_NOFLOW;
    /* Everything else beginning with B or LB is a conditional branch, which
     * both continues and has a target. BRN is a branch that never taken --
     * still has one. */
    if (mnemonic[0] == 'B' || (mnemonic[0] == 'L' && mnemonic[1] == 'B'))
        return COCO_DASM_BRANCH;
    return 0;
}

int coco_dasm_one(uint16_t addr, coco_dasm_read read, void *user,
                  coco_dasm_line *out)
{
    iter it;
    unsigned op, page = 0;
    const char *mnemonic;
    int type;

    memset(out, 0, sizeof(*out));
    out->addr = addr;

    it.pc = addr;
    it.n = 0;
    it.read = read;
    it.user = user;
    it.out = out;

    op = next_byte(&it);
    /* $10 and $11 are page prefixes, not instructions. A run of them is not
     * legal, but decoding the last one is more useful than giving up. */
    if (op == 0x10 || op == 0x11) {
        page = op - 0x0f;
        op = next_byte(&it);
    }

    mnemonic = instructions[page][op].mnemonic;
    type = instructions[page][op].type;
    if (!mnemonic) {
        mnemonic = "*";
        type = ILLEGAL;
    }
    snprintf(out->mnemonic, sizeof(out->mnemonic), "%s", mnemonic);

    switch (type) {
    case INHERENT:
    case ILLEGAL:
        break;
    case IMMEDIATE:
        snprintf(out->operand, sizeof(out->operand), "#$%02x", next_byte(&it));
        break;
    case WORD_IMMEDIATE:
        snprintf(out->operand, sizeof(out->operand), "#$%04x", next_word(&it));
        break;
    case DIRECT:
        snprintf(out->operand, sizeof(out->operand), "<$%02x", next_byte(&it));
        break;
    case EXTENDED: {
        unsigned v = next_word(&it);
        snprintf(out->operand, sizeof(out->operand), "$%04x", v);
        out->target = (uint16_t)v;
    } break;
    case INDEXED:
        indexed_operand(&it, out->operand, sizeof(out->operand));
        break;
    case RELATIVE: {
        int v = sex8(next_byte(&it));
        out->target = (uint16_t)(addr + it.n + v);
        snprintf(out->operand, sizeof(out->operand), "$%04x", out->target);
    } break;
    case LONG_RELATIVE: {
        unsigned v = next_word(&it);
        out->target = (uint16_t)(addr + it.n + v);
        snprintf(out->operand, sizeof(out->operand), "$%04x", out->target);
    } break;
    case STACKS:
        stack_operand(out->operand, sizeof(out->operand), next_byte(&it), "U");
        break;
    case STACKU:
        stack_operand(out->operand, sizeof(out->operand), next_byte(&it), "S");
        break;
    case REGISTER: {
        unsigned postbyte = next_byte(&it);
        snprintf(out->operand, sizeof(out->operand), "%s,%s",
                 tfr_regs[(postbyte >> 4) & 15], tfr_regs[postbyte & 15]);
    } break;
    default:
        break;
    }

    out->flags = flow_flags(out->mnemonic);
    /* An undocumented opcode is shown but never followed: upstream marks
     * those with a trailing '*'. */
    if (type == ILLEGAL || strchr(out->mnemonic, '*')) {
        out->flags = COCO_DASM_ILLEGAL;
        out->target = 0;
    }
    /* A JMP/JSR through an indexed postbyte has no target this line can name
     * without knowing the registers, so do not claim one. */
    if ((out->flags & COCO_DASM_BRANCH) && type == INDEXED)
        out->flags &= ~COCO_DASM_BRANCH;

    out->len = (uint8_t)(it.n > COCO_DASM_MAX_BYTES ? COCO_DASM_MAX_BYTES
                                                    : it.n);
    if (out->operand[0])
        snprintf(out->text, sizeof(out->text), "%-6s%s", out->mnemonic,
                 out->operand);
    else
        snprintf(out->text, sizeof(out->text), "%s", out->mnemonic);
    return out->len;
}

static uint8_t read_flat(uint16_t addr, void *user)
{
    return ((const uint8_t *)user)[addr];
}

int coco_dasm_one_mem(uint16_t addr, const uint8_t *mem, coco_dasm_line *out)
{
    return coco_dasm_one(addr, read_flat, (void *)(uintptr_t)mem, out);
}
