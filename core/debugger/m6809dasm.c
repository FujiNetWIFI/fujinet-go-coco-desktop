/*
 * A line-oriented 6809/6309 disassembler.
 *
 * The opcode tables below are derived from XRoar's src/mc6809/mc6809_trace.c
 * and src/mc6809/hd6309_trace.c -- extracted mechanically rather than
 * retyped, so they cannot drift from upstream's by a typo. Both are
 * GPL-3.0-or-later. The decoding and formatting are written for this API:
 * the tracers emit a fixed line with CPU state appended, which is a
 * different job. See COMPLIANCE.md.
 *
 * XRoar marks undocumented opcodes with a trailing '*' in the mnemonic; that
 * convention is kept, and such lines are flagged COCO_DASM_ILLEGAL so nothing
 * offers to follow them. Some genuine HD6309 opcodes (eg COMW/LSRW/RORW/
 * ROLW/DECW/INCW/TSTW/CLRW) are tagged ILLEGAL in upstream's own table too --
 * carried over verbatim rather than second-guessed, since that is upstream's
 * call to make, not this transcription's.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "m6809dasm.h"

/* Addressing modes, matching upstream's names so the tables transplant. The
 * first 12 are shared with the 6809; everything from QUAD_IMMEDIATE on is
 * HD6309-only and only ever appears in hd6309_instructions[]. */
enum {
    ILLEGAL = 0, INHERENT, WORD_IMMEDIATE, IMMEDIATE, EXTENDED, DIRECT,
    INDEXED, RELATIVE, LONG_RELATIVE, STACKS, STACKU, REGISTER,
    QUAD_IMMEDIATE, MEMBIT, INMEM_DIRECT, INMEM_INDEXED, INMEM_EXTENDED,
    TFMPP, TFMMM, TFMP0, TFM0P,
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

/* HD6309 opcode table, mechanically extracted from XRoar's
 * src/mc6809/hd6309_trace.c the same way instructions[] above was, from
 * src/mc6809/mc6809_trace.c. A separate, complete table rather than a diff
 * against instructions[] above: about 40% of the 768 slots differ (new
 * opcodes filling previously-illegal gaps, redefined opcodes, some outright
 * removed), and a verbatim transplant is what stays re-extractable without
 * hand-merging. */
static const struct {
    const char *mnemonic;
    int type;
} hd6309_instructions[3][256] = {
    { /* PAGE 0 (no prefix) */
        /* 00 */ { "NEG", DIRECT }, { "OIM", INMEM_DIRECT }, { "AIM", INMEM_DIRECT }, { "COM", DIRECT },
        /* 04 */ { "LSR", DIRECT }, { "EIM", INMEM_DIRECT }, { "ROR", DIRECT }, { "ASR", DIRECT },
        /* 08 */ { "LSL", DIRECT }, { "ROL", DIRECT }, { "DEC", DIRECT }, { "TIM", INMEM_DIRECT },
        /* 0c */ { "INC", DIRECT }, { "TST", DIRECT }, { "JMP", DIRECT }, { "CLR", DIRECT },
        /* 10 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "NOP", INHERENT }, { "SYNC", INHERENT },
        /* 14 */ { "SEXW", INHERENT }, { NULL, ILLEGAL }, { "LBRA", LONG_RELATIVE }, { "LBSR", LONG_RELATIVE },
        /* 18 */ { NULL, ILLEGAL }, { "DAA", INHERENT }, { "ORCC", IMMEDIATE }, { NULL, ILLEGAL },
        /* 1c */ { "ANDCC", IMMEDIATE }, { "SEX", INHERENT }, { "EXG", REGISTER }, { "TFR", REGISTER },
        /* 20 */ { "BRA", RELATIVE }, { "BRN", RELATIVE }, { "BHI", RELATIVE }, { "BLS", RELATIVE },
        /* 24 */ { "BCC", RELATIVE }, { "BCS", RELATIVE }, { "BNE", RELATIVE }, { "BEQ", RELATIVE },
        /* 28 */ { "BVC", RELATIVE }, { "BVS", RELATIVE }, { "BPL", RELATIVE }, { "BMI", RELATIVE },
        /* 2c */ { "BGE", RELATIVE }, { "BLT", RELATIVE }, { "BGT", RELATIVE }, { "BLE", RELATIVE },
        /* 30 */ { "LEAX", INDEXED }, { "LEAY", INDEXED }, { "LEAS", INDEXED }, { "LEAU", INDEXED },
        /* 34 */ { "PSHS", STACKS }, { "PULS", STACKS }, { "PSHU", STACKU }, { "PULU", STACKU },
        /* 38 */ { NULL, ILLEGAL }, { "RTS", INHERENT }, { "ABX", INHERENT }, { "RTI", INHERENT },
        /* 3c */ { "CWAI", IMMEDIATE }, { "MUL", INHERENT }, { NULL, ILLEGAL }, { "SWI", INHERENT },
        /* 40 */ { "NEGA", INHERENT }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "COMA", INHERENT },
        /* 44 */ { "LSRA", INHERENT }, { NULL, ILLEGAL }, { "RORA", INHERENT }, { "ASRA", INHERENT },
        /* 48 */ { "LSLA", INHERENT }, { "ROLA", INHERENT }, { "DECA", INHERENT }, { NULL, ILLEGAL },
        /* 4c */ { "INCA", INHERENT }, { "TSTA", INHERENT }, { NULL, ILLEGAL }, { "CLRA", INHERENT },
        /* 50 */ { "NEGB", INHERENT }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "COMB", INHERENT },
        /* 54 */ { "LSRB", INHERENT }, { NULL, ILLEGAL }, { "RORB", INHERENT }, { "ASRB", INHERENT },
        /* 58 */ { "LSLB", INHERENT }, { "ROLB", INHERENT }, { "DECB", INHERENT }, { NULL, ILLEGAL },
        /* 5c */ { "INCB", INHERENT }, { "TSTB", INHERENT }, { NULL, ILLEGAL }, { "CLRB", INHERENT },
        /* 60 */ { "NEG", INDEXED }, { "OIM", INMEM_INDEXED }, { "AIM", INMEM_INDEXED }, { "COM", INDEXED },
        /* 64 */ { "LSR", INDEXED }, { "EIM", INMEM_INDEXED }, { "ROR", INDEXED }, { "ASR", INDEXED },
        /* 68 */ { "LSL", INDEXED }, { "ROL", INDEXED }, { "DEC", INDEXED }, { "TIM", INMEM_INDEXED },
        /* 6c */ { "INC", INDEXED }, { "TST", INDEXED }, { "JMP", INDEXED }, { "CLR", INDEXED },
        /* 70 */ { "NEG", EXTENDED }, { "OIM", INMEM_EXTENDED }, { "AIM", INMEM_EXTENDED }, { "COM", EXTENDED },
        /* 74 */ { "LSR", EXTENDED }, { "EIM", INMEM_EXTENDED }, { "ROR", EXTENDED }, { "ASR", EXTENDED },
        /* 78 */ { "LSL", EXTENDED }, { "ROL", EXTENDED }, { "DEC", EXTENDED }, { "TIM", INMEM_EXTENDED },
        /* 7c */ { "INC", EXTENDED }, { "TST", EXTENDED }, { "JMP", EXTENDED }, { "CLR", EXTENDED },
        /* 80 */ { "SUBA", IMMEDIATE }, { "CMPA", IMMEDIATE }, { "SBCA", IMMEDIATE }, { "SUBD", WORD_IMMEDIATE },
        /* 84 */ { "ANDA", IMMEDIATE }, { "BITA", IMMEDIATE }, { "LDA", IMMEDIATE }, { NULL, ILLEGAL },
        /* 88 */ { "EORA", IMMEDIATE }, { "ADCA", IMMEDIATE }, { "ORA", IMMEDIATE }, { "ADDA", IMMEDIATE },
        /* 8c */ { "CMPX", WORD_IMMEDIATE }, { "BSR", RELATIVE }, { "LDX", WORD_IMMEDIATE }, { NULL, ILLEGAL },
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
        /* c4 */ { "ANDB", IMMEDIATE }, { "BITB", IMMEDIATE }, { "LDB", IMMEDIATE }, { NULL, ILLEGAL },
        /* c8 */ { "EORB", IMMEDIATE }, { "ADCB", IMMEDIATE }, { "ORB", IMMEDIATE }, { "ADDB", IMMEDIATE },
        /* cc */ { "LDD", WORD_IMMEDIATE }, { "LDQ", QUAD_IMMEDIATE }, { "LDU", WORD_IMMEDIATE }, { NULL, ILLEGAL },
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
        /* 00 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 04 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 08 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 0c */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 10 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 14 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 18 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 1c */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 20 */ { NULL, ILLEGAL }, { "LBRN", LONG_RELATIVE }, { "LBHI", LONG_RELATIVE }, { "LBLS", LONG_RELATIVE },
        /* 24 */ { "LBCC", LONG_RELATIVE }, { "LBCS", LONG_RELATIVE }, { "LBNE", LONG_RELATIVE }, { "LBEQ", LONG_RELATIVE },
        /* 28 */ { "LBVC", LONG_RELATIVE }, { "LBVS", LONG_RELATIVE }, { "LBPL", LONG_RELATIVE }, { "LBMI", LONG_RELATIVE },
        /* 2c */ { "LBGE", LONG_RELATIVE }, { "LBLT", LONG_RELATIVE }, { "LBGT", LONG_RELATIVE }, { "LBLE", LONG_RELATIVE },
        /* 30 */ { "ADDR", REGISTER }, { "ADCR", REGISTER }, { "SUBR", REGISTER }, { "SBCR", REGISTER },
        /* 34 */ { "ANDR", REGISTER }, { "ORR", REGISTER }, { "EORR", REGISTER }, { "CMPR", REGISTER },
        /* 38 */ { "PSHSW", INHERENT }, { "PULSW", INHERENT }, { "PSHUW", INHERENT }, { "PULUW", INHERENT },
        /* 3c */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "SWI2", INHERENT },
        /* 40 */ { "NEGD", INHERENT }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "COMD", INHERENT },
        /* 44 */ { "LSRD", INHERENT }, { NULL, ILLEGAL }, { "RORD", INHERENT }, { "ASRD", INHERENT },
        /* 48 */ { "LSLD", INHERENT }, { "ROLD", INHERENT }, { "DECD", INHERENT }, { NULL, ILLEGAL },
        /* 4c */ { "INCD", INHERENT }, { "TSTD", INHERENT }, { NULL, ILLEGAL }, { "CLRD", INHERENT },
        /* 50 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "COMW", ILLEGAL },
        /* 54 */ { "LSRW", ILLEGAL }, { NULL, ILLEGAL }, { "RORW", ILLEGAL }, { NULL, ILLEGAL },
        /* 58 */ { NULL, ILLEGAL }, { "ROLW", ILLEGAL }, { "DECW", ILLEGAL }, { NULL, ILLEGAL },
        /* 5c */ { "INCW", ILLEGAL }, { "TSTW", ILLEGAL }, { NULL, ILLEGAL }, { "CLRW", ILLEGAL },
        /* 60 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 64 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 68 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 6c */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 70 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 74 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 78 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 7c */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 80 */ { "SUBW", WORD_IMMEDIATE }, { "CMPW", WORD_IMMEDIATE }, { "SBCD", WORD_IMMEDIATE }, { "CMPD", WORD_IMMEDIATE },
        /* 84 */ { "ANDD", WORD_IMMEDIATE }, { "BITD", WORD_IMMEDIATE }, { "LDW", WORD_IMMEDIATE }, { NULL, ILLEGAL },
        /* 88 */ { "EORD", WORD_IMMEDIATE }, { "ADCD", WORD_IMMEDIATE }, { "ORD", WORD_IMMEDIATE }, { "ADDW", WORD_IMMEDIATE },
        /* 8c */ { "CMPY", WORD_IMMEDIATE }, { NULL, ILLEGAL }, { "LDY", WORD_IMMEDIATE }, { NULL, ILLEGAL },
        /* 90 */ { "SUBW", DIRECT }, { "CMPW", DIRECT }, { "SBCD", DIRECT }, { "CMPD", DIRECT },
        /* 94 */ { "ANDD", DIRECT }, { "BITD", DIRECT }, { "LDW", DIRECT }, { "STW", DIRECT },
        /* 98 */ { "EORD", DIRECT }, { "ADCD", DIRECT }, { "ORD", DIRECT }, { "ADDW", DIRECT },
        /* 9c */ { "CMPY", DIRECT }, { NULL, ILLEGAL }, { "LDY", DIRECT }, { "STY", DIRECT },
        /* a0 */ { "SUBW", INDEXED }, { "CMPW", INDEXED }, { "SBCD", INDEXED }, { "CMPD", INDEXED },
        /* a4 */ { "ANDD", INDEXED }, { "BITD", INDEXED }, { "LDW", INDEXED }, { "STW", INDEXED },
        /* a8 */ { "EORD", INDEXED }, { "ADCD", INDEXED }, { "ORD", INDEXED }, { "ADDW", INDEXED },
        /* ac */ { "CMPY", INDEXED }, { NULL, ILLEGAL }, { "LDY", INDEXED }, { "STY", INDEXED },
        /* b0 */ { "SUBW", EXTENDED }, { "CMPW", EXTENDED }, { "SBCD", EXTENDED }, { "CMPD", EXTENDED },
        /* b4 */ { "ANDD", EXTENDED }, { "BITD", EXTENDED }, { "LDW", EXTENDED }, { "STW", EXTENDED },
        /* b8 */ { "EORD", EXTENDED }, { "ADCD", EXTENDED }, { "ORD", EXTENDED }, { "ADDW", EXTENDED },
        /* bc */ { "CMPY", EXTENDED }, { NULL, ILLEGAL }, { "LDY", EXTENDED }, { "STY", EXTENDED },
        /* c0 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* c4 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* c8 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* cc */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "LDS", WORD_IMMEDIATE }, { NULL, ILLEGAL },
        /* d0 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* d4 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* d8 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* dc */ { "LDQ", DIRECT }, { "STQ", DIRECT }, { "LDS", DIRECT }, { "STS", DIRECT },
        /* e0 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* e4 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* e8 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* ec */ { "LDQ", INDEXED }, { "STQ", INDEXED }, { "LDS", INDEXED }, { "STS", INDEXED },
        /* f0 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* f4 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* f8 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* fc */ { "LDQ", EXTENDED }, { "STQ", EXTENDED }, { "LDS", EXTENDED }, { "STS", EXTENDED },
    },
    { /* PAGE 2 (prefix $11) */
        /* 00 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 04 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 08 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 0c */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 10 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 14 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 18 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 1c */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 20 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 24 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 28 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 2c */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 30 */ { "BAND", MEMBIT }, { "BIAND", MEMBIT }, { "BOR", MEMBIT }, { "BIOR", MEMBIT },
        /* 34 */ { "BEOR", MEMBIT }, { "BIEOR", MEMBIT }, { "LDBT", MEMBIT }, { "STBT", MEMBIT },
        /* 38 */ { "TFM", TFMPP }, { "TFM", TFMMM }, { "TFM", TFMP0 }, { "TFM", TFM0P },
        /* 3c */ { "BITMD", IMMEDIATE }, { "LDMD", IMMEDIATE }, { NULL, ILLEGAL }, { "SWI3", INHERENT },
        /* 40 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "COME", INHERENT },
        /* 44 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 48 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "DECE", INHERENT }, { NULL, ILLEGAL },
        /* 4c */ { "INCE", INHERENT }, { "TSTE", INHERENT }, { NULL, ILLEGAL }, { "CLRE", INHERENT },
        /* 50 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "COMF", INHERENT },
        /* 54 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 58 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "DECF", INHERENT }, { NULL, ILLEGAL },
        /* 5c */ { "INCF", INHERENT }, { "TSTF", INHERENT }, { NULL, ILLEGAL }, { "CLRF", INHERENT },
        /* 60 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 64 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 68 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 6c */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 70 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 74 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 78 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 7c */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* 80 */ { "SUBE", IMMEDIATE }, { "CMPE", IMMEDIATE }, { NULL, ILLEGAL }, { "CMPU", WORD_IMMEDIATE },
        /* 84 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "LDE", IMMEDIATE }, { NULL, ILLEGAL },
        /* 88 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "ADDE", IMMEDIATE },
        /* 8c */ { "CMPS", WORD_IMMEDIATE }, { "DIVD", IMMEDIATE }, { "DIVQ", WORD_IMMEDIATE }, { "MULD", WORD_IMMEDIATE },
        /* 90 */ { "SUBE", DIRECT }, { "CMPE", DIRECT }, { NULL, ILLEGAL }, { "CMPU", DIRECT },
        /* 94 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "LDE", DIRECT }, { "STE", DIRECT },
        /* 98 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "ADDE", DIRECT },
        /* 9c */ { "CMPS", DIRECT }, { "DIVD", DIRECT }, { "DIVQ", DIRECT }, { "MULD", DIRECT },
        /* a0 */ { "SUBE", INDEXED }, { "CMPE", INDEXED }, { NULL, ILLEGAL }, { "CMPU", INDEXED },
        /* a4 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "LDE", INDEXED }, { "STE", INDEXED },
        /* a8 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "ADDE", INDEXED },
        /* ac */ { "CMPS", INDEXED }, { "DIVD", INDEXED }, { "DIVQ", INDEXED }, { "MULD", INDEXED },
        /* b0 */ { "SUBE", EXTENDED }, { "CMPE", EXTENDED }, { NULL, ILLEGAL }, { "CMPU", EXTENDED },
        /* b4 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "LDE", EXTENDED }, { "STE", EXTENDED },
        /* b8 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "ADDE", EXTENDED },
        /* bc */ { "CMPS", EXTENDED }, { "DIVD", EXTENDED }, { "DIVQ", EXTENDED }, { "MULD", EXTENDED },
        /* c0 */ { "SUBF", IMMEDIATE }, { "CMPF", IMMEDIATE }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* c4 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "LDF", IMMEDIATE }, { NULL, ILLEGAL },
        /* c8 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "ADDF", IMMEDIATE },
        /* cc */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* d0 */ { "SUBF", DIRECT }, { "CMPF", DIRECT }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* d4 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "LDF", DIRECT }, { "STF", DIRECT },
        /* d8 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "ADDF", DIRECT },
        /* dc */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* e0 */ { "SUBF", INDEXED }, { "CMPF", INDEXED }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* e4 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "LDF", INDEXED }, { "STF", INDEXED },
        /* e8 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "ADDF", INDEXED },
        /* ec */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* f0 */ { "SUBF", EXTENDED }, { "CMPF", EXTENDED }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
        /* f4 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "LDF", EXTENDED }, { "STF", EXTENDED },
        /* f8 */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { "ADDF", EXTENDED },
        /* fc */ { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL }, { NULL, ILLEGAL },
    },
};

/* Inter-register postbyte (TFR/EXG). The 6309 defines all 16 slots (adding
 * W, V, and redefining 12/13 as a literal zero register); the 6809 leaves
 * six of them undefined. */
static const char *const tfr_regs_6809[16] = {
    "D", "X", "Y", "U", "S", "PC", "*", "*",
    "A", "B", "CC", "DP", "*", "*", "*", "*"
};
static const char *const tfr_regs_6309[16] = {
    "D", "X", "Y", "U", "S", "PC", "W", "V",
    "A", "B", "CC", "DP", "0", "0", "E", "F"
};

/* Indexed addressing postbyte base register. */
static const char *const idx_regs[4] = { "X", "Y", "U", "S" };

/* HD6309-only: accumulator-offset indexed modes filling nibbles the 6809
 * leaves illegal (postbyte & 0x0f == 0x7/0xa/0xe), and the TFM register-pair
 * postbyte format strings (R+,R+ / R-,R- / R+,R / R,R+, one per TFM
 * mnemonic variant since XRoar picks the format by opcode, not postbyte). */
static const char *const membit_regs[4] = { "CC", "A", "B", "*" };
static const char *const tfm_fmts[4] = {
    "%s+,%s+", "%s-,%s-", "%s+,%s", "%s,%s+"
};

/* Reading state for one instruction: everything the decoder consumes goes
 * through here, so `len` is always exactly what was read and a caller
 * walking a range can never lose sync. */
typedef struct {
    uint16_t pc;            /* address of the opcode */
    unsigned n;             /* bytes consumed so far */
    coco_dasm_read read;
    void *user;
    coco_dasm_line *out;
    coco_dasm_cpu cpu;
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

/* An indexed postbyte, into `dst`. Consumes any offset bytes it needs.
 *
 * The HD6309 reuses this postbyte format but fills in three nibbles the
 * 6809 leaves illegal (0x7/0xa/0xe -> E,R / F,R / W,R accumulator offset,
 * no extra bytes) and hijacks two specific FULL postbyte values per W-mode
 * to mean ",W" addressing regardless of what the base-register bits would
 * otherwise say (0x8f and 0x90 both mean ,W; 0xaf/0xb0 mean n16,W; 0xcf/0xd0
 * mean ,W++; 0xef/0xf0 mean ,--W) -- transcribed from hd6309_trace.c's own
 * override table, not derived from general 6309 documentation, since this
 * is exactly the kind of detail easy to get subtly wrong from memory. */
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

    if (it->cpu == COCO_DASM_CPU_HD6309) {
        if (postbyte == 0x8f || postbyte == 0x90) {
            snprintf(dst, dstsz, "%s,W%s", pre, post);
            return;
        }
        if (postbyte == 0xaf || postbyte == 0xb0) {
            snprintf(dst, dstsz, "%s$%04x,W%s", pre, next_word(it), post);
            return;
        }
        if (postbyte == 0xcf || postbyte == 0xd0) {
            snprintf(dst, dstsz, "%s,W++%s", pre, post);
            return;
        }
        if (postbyte == 0xef || postbyte == 0xf0) {
            snprintf(dst, dstsz, "%s,--W%s", pre, post);
            return;
        }
    }

    switch (mode) {
    case 0x0: snprintf(dst, dstsz, "%s,%s+%s", pre, reg, post); break;
    case 0x1: snprintf(dst, dstsz, "%s,%s++%s", pre, reg, post); break;
    case 0x2: snprintf(dst, dstsz, "%s,-%s%s", pre, reg, post); break;
    case 0x3: snprintf(dst, dstsz, "%s,--%s%s", pre, reg, post); break;
    case 0x4: snprintf(dst, dstsz, "%s,%s%s", pre, reg, post); break;
    case 0x5: snprintf(dst, dstsz, "%sB,%s%s", pre, reg, post); break;
    case 0x6: snprintf(dst, dstsz, "%sA,%s%s", pre, reg, post); break;
    case 0x7:
        if (it->cpu == COCO_DASM_CPU_HD6309) {
            snprintf(dst, dstsz, "%sE,%s%s", pre, reg, post);
            break;
        }
        snprintf(dst, dstsz, "%s??%s", pre, post);
        break;
    case 0x8: {
        int v = sex8(next_byte(it));
        snprintf(dst, dstsz, "%s%s$%02x,%s%s", pre, v < 0 ? "-" : "",
                 v < 0 ? -v : v, reg, post);
    } break;
    case 0x9: snprintf(dst, dstsz, "%s$%04x,%s%s", pre, next_word(it), reg,
                       post); break;
    case 0xa:
        if (it->cpu == COCO_DASM_CPU_HD6309) {
            snprintf(dst, dstsz, "%sF,%s%s", pre, reg, post);
            break;
        }
        snprintf(dst, dstsz, "%s??%s", pre, post);
        break;
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
    case 0xe:
        if (it->cpu == COCO_DASM_CPU_HD6309) {
            snprintf(dst, dstsz, "%sW,%s%s", pre, reg, post);
            break;
        }
        snprintf(dst, dstsz, "%s??%s", pre, post);
        break;
    case 0xf: snprintf(dst, dstsz, "%s$%04x%s", pre, next_word(it), post);
        break;
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

int coco_dasm_one_cpu(uint16_t addr, coco_dasm_read read, void *user,
                      coco_dasm_cpu cpu, coco_dasm_line *out)
{
    iter it;
    unsigned op, page = 0, im_value = 0;
    const char *mnemonic;
    const char *const *tfr_regs;
    int type;

    memset(out, 0, sizeof(*out));
    out->addr = addr;

    it.pc = addr;
    it.n = 0;
    it.read = read;
    it.user = user;
    it.out = out;
    it.cpu = cpu;

    op = next_byte(&it);
    /* $10 and $11 are page prefixes, not instructions. A run of them is not
     * legal, but decoding the last one is more useful than giving up. */
    if (op == 0x10 || op == 0x11) {
        page = op - 0x0f;
        op = next_byte(&it);
    }

    if (cpu == COCO_DASM_CPU_HD6309) {
        mnemonic = hd6309_instructions[page][op].mnemonic;
        type = hd6309_instructions[page][op].type;
        tfr_regs = tfr_regs_6309;
    } else {
        mnemonic = instructions[page][op].mnemonic;
        type = instructions[page][op].type;
        tfr_regs = tfr_regs_6809;
    }
    if (!mnemonic) {
        mnemonic = "*";
        type = ILLEGAL;
    }
    snprintf(out->mnemonic, sizeof(out->mnemonic), "%s", mnemonic);

    /* INMEM_* (HD6309 OIM/AIM/EIM/TIM: an immediate operand applied to a
     * memory location addressed by DIRECT/INDEXED/EXTENDED) reads that
     * immediate byte before the address, matching hd6309_trace.c's own
     * ordering -- the immediate comes first in the encoding either way. */
    if (type == INMEM_DIRECT || type == INMEM_INDEXED ||
        type == INMEM_EXTENDED)
        im_value = next_byte(&it);

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
    case INMEM_DIRECT:
        snprintf(out->operand, sizeof(out->operand), "#$%02x,<$%02x",
                im_value, next_byte(&it));
        break;
    case EXTENDED: {
        unsigned v = next_word(&it);
        snprintf(out->operand, sizeof(out->operand), "$%04x", v);
        out->target = (uint16_t)v;
    } break;
    case INMEM_EXTENDED:
        snprintf(out->operand, sizeof(out->operand), "#$%02x,$%04x",
                im_value, next_word(&it));
        break;
    case INDEXED:
        indexed_operand(&it, out->operand, sizeof(out->operand));
        break;
    case INMEM_INDEXED: {
        char idx[24];
        indexed_operand(&it, idx, sizeof(idx));
        snprintf(out->operand, sizeof(out->operand), "#$%02x,%s", im_value,
                idx);
    } break;
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
    case QUAD_IMMEDIATE: {
        unsigned hi = next_word(&it);
        unsigned lo = next_word(&it);
        snprintf(out->operand, sizeof(out->operand), "#$%04x%04x", hi, lo);
    } break;
    case TFMPP: case TFMMM: case TFMP0: case TFM0P: {
        /* Picking the format by opcode value (type - TFMPP), not by
         * postbyte, matches hd6309_trace.c's `tfm_fmts[ins & 3]` -- the
         * four TFM opcodes are consecutive ($38-$3b on page 2), so the low
         * 2 bits of the opcode already say which variant this is. */
        const char *fmt = tfm_fmts[type - TFMPP];
        unsigned postbyte = next_byte(&it);
        if ((postbyte >> 4) > 4 || (postbyte & 15) > 4)
            snprintf(out->mnemonic, sizeof(out->mnemonic), "TFM*");
        snprintf(out->operand, sizeof(out->operand), fmt,
                 tfr_regs[(postbyte >> 4) & 15], tfr_regs[postbyte & 15]);
    } break;
    case MEMBIT: {
        unsigned postbyte = next_byte(&it);
        unsigned direct = next_byte(&it);
        snprintf(out->operand, sizeof(out->operand), "%s,%u,%u,<$%02x",
                 membit_regs[(postbyte >> 6) & 3], (postbyte >> 3) & 7,
                 postbyte & 7, direct);
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
    if ((out->flags & COCO_DASM_BRANCH) &&
        (type == INDEXED || type == INMEM_INDEXED))
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

int coco_dasm_one(uint16_t addr, coco_dasm_read read, void *user,
                  coco_dasm_line *out)
{
    return coco_dasm_one_cpu(addr, read, user, COCO_DASM_CPU_MC6809, out);
}

static uint8_t read_flat(uint16_t addr, void *user)
{
    return ((const uint8_t *)user)[addr];
}

int coco_dasm_one_mem(uint16_t addr, const uint8_t *mem, coco_dasm_line *out)
{
    return coco_dasm_one(addr, read_flat, (void *)(uintptr_t)mem, out);
}

int coco_dasm_one_mem_cpu(uint16_t addr, const uint8_t *mem, coco_dasm_cpu cpu,
                          coco_dasm_line *out)
{
    return coco_dasm_one_cpu(addr, read_flat, (void *)(uintptr_t)mem, cpu,
                             out);
}
