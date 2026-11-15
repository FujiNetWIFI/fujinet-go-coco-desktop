/*
 * prompt.c -- the debugger's command line (cocodebug_command) and its Tab
 * completion. Every command is a thin layer over the public API, so a
 * window's buttons and the prompt can never disagree.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cocomemtools.h"
#include "compat.h"
#include "debugger_internal.h"
#include "m6809dasm.h"

typedef struct {
    char *dst;
    int cap, len;
} outbuf;

static void out(outbuf *buf, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (buf->len >= buf->cap - 1)
        return;
    va_start(ap, fmt);
    n = vsnprintf(buf->dst + buf->len, (size_t)(buf->cap - buf->len), fmt, ap);
    va_end(ap);
    if (n > 0)
        buf->len += n < buf->cap - buf->len ? n : buf->cap - buf->len - 1;
}

static const char *const commands[] = {
    "help", "step", "over", "out", "run", "stop", "runto", "line", "overline",
    "break", "bpr", "bpw", "bpa", "delete", "enable", "disable", "clear", "breaks",
    "hits", "print", "regs", "set", "mem", "poke", "disasm", "trace", "watch",
    "unwatch", "find", "symbols", "label", "labels", "source", "locals", "map",
    "io", "cart", "sound", "video", NULL,
};

static const char *help_text =
    "step [n]               run n instructions (F7)\n"
    "over                   step over a call (F8)\n"
    "out                    run to the routine's return (Shift+F8)\n"
    "line | overline        step a C source line / over a call (F6, Shift+F6)\n"
    "run | stop             go / break\n"
    "runto <addr>           run until PC = addr\n"
    "break <addr>[-<end>] [cond]     execute breakpoint\n"
    "bpr | bpw | bpa [phys] <addr>[-<end>] [cond]   read / write / access breakpoint;\n"
    "                       phys = CoCo 3 physical RAM address\n"
    "delete <id> | enable <id> | disable <id> | clear | breaks | hits (clear hit counts)\n"
    "print <expr>           e.g. print x, print [$0400], print {s}, print pc+3\n"
    "regs                   registers and flags (6309: E F W V MD)\n"
    "set <reg> <expr>       e.g. set pc $0402, set a 0, set cc $50, set zf 1\n"
    "mem [phys] <addr> [len]    dump memory (side-effect free)\n"
    "poke [phys] <addr> <v...>  write RAM\n"
    "disasm [addr] [n]\n"
    "trace [on|off|clear|n]  record / show the last n instructions\n"
    "watch [phys] [addr|symbol|lo-hi] | unwatch <n|all>   the watch list\n"
    "find [phys] [hex] <bytes> | find [phys] text <string>   search memory (stopped)\n"
    "symbols [filter] | label <addr> <name> | labels <file>\n"
    "source <file.lst> | locals      C source info from a CMOC -i build; the locals\n"
    "map                    the CPU memory map as mapped now\n"
    "io | sound | cart      PIA0, PIA1, SAM, GIME, joysticks; the audio path; the cartridge\n"
    "video [section]        the Video tab (pixel, registers, colours, lines, timing)\n"
    "Expressions: $0400 0x400 400h 1024 %1010; registers a b d dp x y u s pc cc\n"
    "(6309: e f w v md); flags cf vf zf nf if hf ff ef; [addr] byte, {addr} word;\n"
    "symbols; vpos hpos frame cycles; in a condition, value, old and addr; C operators.\n";

static char *next_tok(char **p)
{
    char *s = *p, *t;
    while (*s && isspace((unsigned char)*s))
        s++;
    if (!*s) {
        *p = s;
        return NULL;
    }
    t = s;
    while (*s && !isspace((unsigned char)*s))
        s++;
    if (*s)
        *s++ = '\0';
    *p = s;
    return t;
}

static char *rest_of(char *p)
{
    while (*p && isspace((unsigned char)*p))
        p++;
    return p;
}

static int eval(cocodebug *d, outbuf *buf, const char *text, long *v)
{
    char err[96];
    if (cocodebug_eval(d, text, v, err, sizeof err) != 0) {
        out(buf, "error: %s\n", err);
        return -1;
    }
    return 0;
}

/* "<addr>" or "<addr>-<end>" */
static int range(cocodebug *d, outbuf *buf, char *tok, long *first, long *last)
{
    char *dash = strchr(tok + 1, '-');
    if (dash) {
        *dash = '\0';
        if (eval(d, buf, tok, first) || eval(d, buf, dash + 1, last))
            return -1;
        return 0;
    }
    if (eval(d, buf, tok, first))
        return -1;
    *last = *first;
    return 0;
}

/* An optional leading "phys" word picks the physical space. */
static int take_space(char **p)
{
    char *scan = rest_of(*p);

    if (!strncasecmp(scan, "phys", 4) && (!scan[4] || isspace((unsigned char)scan[4]))) {
        *p = scan + 4;
        return COCODEBUG_SPACE_PHYS;
    }
    return COCODEBUG_SPACE_CPU;
}

static void show_regs(cocodebug *d, outbuf *buf)
{
    static const char names[] = "EFHINZVC";
    cocodebug_regs r;
    char flags[9];
    int bit;

    if (!cocodebug_regs_get(d, &r)) {
        out(buf, "the machine is running (stop first)\n");
        return;
    }
    for (bit = 0; bit < 8; bit++)
        flags[bit] = (r.cc & (0x80 >> bit)) ? names[bit] : '.';
    flags[8] = '\0';
    out(buf, "PC=%04X  A=%02X B=%02X (D=%04X)  DP=%02X  CC=%02X %s\n", r.pc, r.a, r.b,
        (r.a << 8) | r.b, r.dp, r.cc, flags);
    out(buf, "X=%04X  Y=%04X  U=%04X  S=%04X\n", r.x, r.y, r.u, r.s);
    if (r.is6309)
        out(buf, "E=%02X F=%02X (W=%04X)  V=%04X  MD=%02X\n", r.e, r.f, r.w, r.v, r.md);
}

static void dump(outbuf *buf, int space, uint32_t addr, const uint8_t *bytes, int n)
{
    const int digits = space == COCODEBUG_SPACE_PHYS ? 6 : 4;
    int row, col;

    for (row = 0; row < n; row += 16) {
        out(buf, "%0*X ", digits, addr + (uint32_t)row);
        for (col = 0; col < 16; col++)
            out(buf, col < n - row ? " %02X" : "   ", bytes[row + col]);
        out(buf, "  ");
        for (col = 0; col < 16 && row + col < n; col++)
            out(buf, "%c", (bytes[row + col] >= 0x20 && bytes[row + col] < 0x7f)
                               ? bytes[row + col] : '.');
        out(buf, "\n");
    }
}

static int reg_by_name(const char *name)
{
    static const struct { const char *name; int reg; } map[] = {
        { "pc", COCODEBUG_REG_PC }, { "a", COCODEBUG_REG_A }, { "b", COCODEBUG_REG_B },
        { "d", COCODEBUG_REG_D }, { "dp", COCODEBUG_REG_DP }, { "x", COCODEBUG_REG_X },
        { "y", COCODEBUG_REG_Y }, { "u", COCODEBUG_REG_U }, { "s", COCODEBUG_REG_S },
        { "cc", COCODEBUG_REG_CC }, { "e", COCODEBUG_REG_E }, { "f", COCODEBUG_REG_F },
        { "w", COCODEBUG_REG_W }, { "v", COCODEBUG_REG_V }, { "md", COCODEBUG_REG_MD },
    };
    unsigned pos;
    for (pos = 0; pos < sizeof map / sizeof map[0]; pos++)
        if (!strcasecmp(map[pos].name, name))
            return map[pos].reg;
    return -1;
}

static int flag_by_name(const char *name)
{
    static const struct { const char *name; unsigned mask; } map[] = {
        { "cf", COCODEBUG_CC_C }, { "vf", COCODEBUG_CC_V }, { "zf", COCODEBUG_CC_Z },
        { "nf", COCODEBUG_CC_N }, { "if", COCODEBUG_CC_I }, { "hf", COCODEBUG_CC_H },
        { "ff", COCODEBUG_CC_F }, { "ef", COCODEBUG_CC_E },
    };
    unsigned pos;
    for (pos = 0; pos < sizeof map / sizeof map[0]; pos++)
        if (!strcasecmp(map[pos].name, name))
            return (int)map[pos].mask;
    return 0;
}

static void list_breaks(cocodebug *d, outbuf *buf)
{
    cocodebug_breakpoint bps[COCODEBUG_MAX_BREAKPOINTS];
    int count = cocodebug_breakpoints_list(d, bps, COCODEBUG_MAX_BREAKPOINTS), pos;

    if (!count) {
        out(buf, "no breakpoints\n");
        return;
    }
    for (pos = 0; pos < count; pos++) {
        char text[200];
        cocobp_describe(d, &bps[pos], text, sizeof text);
        out(buf, "#%d %s%s  hits %u\n", bps[pos].id, text,
            bps[pos].enabled ? "" : " (disabled)", bps[pos].hits);
    }
}

static void add_bp(cocodebug *d, outbuf *buf, int type, char *args)
{
    char *tok;
    long first, last;
    int id;

    if (take_space(&args) == COCODEBUG_SPACE_PHYS)
        type |= COCODEBUG_BP_PHYS;
    tok = next_tok(&args);
    if (!tok) {
        out(buf, "usage: [phys] <addr>[-<end>] [condition]\n");
        return;
    }
    if (range(d, buf, tok, &first, &last))
        return;
    id = cocodebug_breakpoint_add(d, type, (uint32_t)first, (uint32_t)last, rest_of(args));
    if (id < 0)
        out(buf, "error: not set (table full, bad range, a CoCo 1/2 has no physical RAM, "
                 "or the condition does not parse)\n");
    else
        out(buf, "breakpoint #%d\n", id);
}

static void wait_stopped(cocodebug *d, uint64_t was)
{
    int tries;
    cocodebug_stop stop;
    for (tries = 0; tries < 2000; tries++) {
        cocodebug_stop_snapshot(d, &stop);
        if (stop.paused && stop.serial != was)
            return;
        coco_sleep_ms(1);
    }
}

static void show_io(cocodebug *d, outbuf *buf)
{
    cocodebug_io io;
    int port;

    if (!cocodebug_io_get(d, &io)) {
        out(buf, "the machine is running (stop first)\n");
        return;
    }
    out(buf, "PIA0  CRA %02X CRB %02X  DDRA %02X DDRB %02X  ORA %02X ORB %02X  pins %02X %02X"
             "  IRQ %d %d\n", io.pia0.cra, io.pia0.crb, io.pia0.ddra, io.pia0.ddrb, io.pia0.ora,
        io.pia0.orb, io.pia0.pina, io.pia0.pinb, io.pia0.irqa, io.pia0.irqb);
    out(buf, "PIA1  CRA %02X CRB %02X  DDRA %02X DDRB %02X  ORA %02X ORB %02X  pins %02X %02X"
             "  IRQ %d %d\n", io.pia1.cra, io.pia1.crb, io.pia1.ddra, io.pia1.ddrb, io.pia1.ora,
        io.pia1.orb, io.pia1.pina, io.pia1.pinb, io.pia1.irqa, io.pia1.irqb);
    if (io.sam_valid)
        out(buf, "SAM   V=%u F=$%04X P=%u R=%u M=%u TY=%u  video address $%04X\n", io.sam_v,
            io.sam_f, io.sam_p, io.sam_r, io.sam_m, io.sam_ty, io.sam_video_address);
    if (io.gime_valid) {
        out(buf, "GIME  $FF90:");
        for (port = 0; port < 16; port++)
            out(buf, " %02X", io.gime_reg[port]);
        out(buf, "\n      MMU $FFA0:");
        for (port = 0; port < 16; port++)
            out(buf, " %02X", io.gime_mmu[port]);
        out(buf, "\n      palette $FFB0:");
        for (port = 0; port < 16; port++)
            out(buf, " %02X", io.gime_palette[port]);
        out(buf, "\n      scanline %u row %u  IRQ %d FIRQ %d\n", io.gime_scanline,
            io.gime_row, io.gime_irq, io.gime_firq);
    }
    for (port = 0; port < 2; port++)
        out(buf, "joystick %s  X %d Y %d  buttons %d %d\n", port ? "left " : "right",
            io.joy_axis[port][0], io.joy_axis[port][1], io.joy_button[port][0],
            io.joy_button[port][1]);
    out(buf, "Becker link %s, %d byte(s) waiting\n", io.becker_link_up ? "up" : "down",
        io.becker_rx_pending);
}

int cocodebug_command(cocodebug *d, const char *command, char *dst, int dstsz)
{
    char line[512];
    char *p = line, *cmd;
    outbuf buf = { dst, dstsz, 0 };
    long v, w;

    if (!d || !dst || dstsz <= 0)
        return 0;
    dst[0] = '\0';
    snprintf(line, sizeof line, "%s", command ? command : "");
    cmd = next_tok(&p);
    if (!cmd)
        return 0;
    for (char *ch = cmd; *ch; ch++)
        *ch = (char)tolower((unsigned char)*ch);

    if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
        out(&buf, "%s", help_text);
    } else if (!strcmp(cmd, "step") || !strcmp(cmd, "s")) {
        char *tok = next_tok(&p);
        v = 1;
        if (tok && eval(d, &buf, tok, &v))
            return buf.len;
        if (!cocodebug_is_paused(d)) {
            out(&buf, "the machine is running (stop first)\n");
        } else {
            long count;
            for (count = 0; count < v; count++) {
                uint64_t was = cocodebug_stop_serial(d);
                cocodebug_step(d);
                wait_stopped(d, was);
            }
        }
    } else if (!strcmp(cmd, "over") || !strcmp(cmd, "o")) {
        cocodebug_step_over(d);
    } else if (!strcmp(cmd, "out")) {
        cocodebug_step_out(d);
    } else if (!strcmp(cmd, "line")) {
        cocodebug_step_line(d);
    } else if (!strcmp(cmd, "overline")) {
        cocodebug_step_over_line(d);
    } else if (!strcmp(cmd, "run") || !strcmp(cmd, "go") || !strcmp(cmd, "g")) {
        cocodebug_resume(d);
    } else if (!strcmp(cmd, "stop")) {
        cocodebug_pause(d);
    } else if (!strcmp(cmd, "runto")) {
        char *tok = next_tok(&p);
        if (!tok || eval(d, &buf, tok, &v))
            return buf.len;
        cocodebug_run_to(d, (uint16_t)v);
    } else if (!strcmp(cmd, "break") || !strcmp(cmd, "bp")) {
        add_bp(d, &buf, COCODEBUG_BP_EXEC, p);
    } else if (!strcmp(cmd, "bpr")) {
        add_bp(d, &buf, COCODEBUG_BP_READ, p);
    } else if (!strcmp(cmd, "bpw")) {
        add_bp(d, &buf, COCODEBUG_BP_WRITE, p);
    } else if (!strcmp(cmd, "bpa")) {
        add_bp(d, &buf, COCODEBUG_BP_ACCESS, p);
    } else if (!strcmp(cmd, "delete") || !strcmp(cmd, "enable") || !strcmp(cmd, "disable")) {
        char *tok = next_tok(&p);
        if (!tok) {
            out(&buf, "usage: %s <id>\n", cmd);
        } else {
            long id = strtol(tok[0] == '#' ? tok + 1 : tok, NULL, 10);
            if (!strcmp(cmd, "delete"))
                cocodebug_breakpoint_remove(d, (int)id);
            else
                cocodebug_breakpoint_enable(d, (int)id, !strcmp(cmd, "enable"));
        }
    } else if (!strcmp(cmd, "clear")) {
        cocodebug_breakpoint_clear_all(d);
    } else if (!strcmp(cmd, "breaks") || !strcmp(cmd, "bl")) {
        list_breaks(d, &buf);
    } else if (!strcmp(cmd, "hits")) {
        cocodebug_breakpoint_clear_hits(d);
    } else if (!strcmp(cmd, "print") || !strcmp(cmd, "p")) {
        if (eval(d, &buf, rest_of(p), &v) == 0) {
            char bin[17];
            int bit;
            for (bit = 0; bit < 16; bit++)
                bin[bit] = (char)('0' + ((v >> (15 - bit)) & 1));
            bin[16] = '\0';
            out(&buf, "$%04lX  %ld  %%%s\n", (unsigned long)v & 0xffff, v, bin);
        }
    } else if (!strcmp(cmd, "regs") || !strcmp(cmd, "r")) {
        show_regs(d, &buf);
    } else if (!strcmp(cmd, "set")) {
        char *name = next_tok(&p);
        int reg = name ? reg_by_name(name) : -1;
        int flag = name ? flag_by_name(name) : 0;
        if (reg < 0 && !flag) {
            out(&buf, "usage: set <reg|flag> <expr>\n");
        } else if (eval(d, &buf, rest_of(p), &v) == 0) {
            if (!cocodebug_is_paused(d))
                out(&buf, "the machine is running (stop first)\n");
            else if (flag ? !cocodebug_flag_set(d, (unsigned)flag, v != 0)
                          : !cocodebug_regs_set(d, reg, (unsigned)v))
                out(&buf, "not available on this CPU\n");
            else
                show_regs(d, &buf);
        }
    } else if (!strcmp(cmd, "mem") || !strcmp(cmd, "m")) {
        int space = take_space(&p);
        char *tok = next_tok(&p), *len = next_tok(&p);
        uint8_t bytes[256];
        int got;
        w = 64;
        if (!tok || eval(d, &buf, tok, &v) || (len && eval(d, &buf, len, &w)))
            return buf.len;
        if (w > 256)
            w = 256;
        got = cocodebug_mem_read(d, space, (uint32_t)v, bytes, (int)w);
        if (got <= 0)
            out(&buf, "the machine is running (stop first), or no such address\n");
        else
            dump(&buf, space, (uint32_t)v, bytes, got);
    } else if (!strcmp(cmd, "poke")) {
        int space = take_space(&p);
        char *tok = next_tok(&p), *item;
        int count = 0;
        if (!tok || eval(d, &buf, tok, &v))
            return buf.len;
        while ((item = next_tok(&p)) != NULL) {
            uint8_t byte;
            if (eval(d, &buf, item, &w))
                return buf.len;
            byte = (uint8_t)w;
            if (cocodebug_mem_write(d, space, (uint32_t)(v + count), &byte, 1) != 1) {
                out(&buf, "stopped at +%d: running, ROM/I-O, or past the space\n", count);
                return buf.len;
            }
            count++;
        }
        out(&buf, "%d byte%s at $%04lX\n", count, count == 1 ? "" : "s", v);
    } else if (!strcmp(cmd, "disasm") || !strcmp(cmd, "d")) {
        char *tok = next_tok(&p), *num = next_tok(&p);
        cocodebug_line lines[64];
        cocodebug_regs r;
        int pcl, count, pos;
        if (!cocodebug_regs_get(d, &r)) {
            out(&buf, "the machine is running (stop first)\n");
            return buf.len;
        }
        v = r.pc;
        w = 16;
        if ((tok && eval(d, &buf, tok, &v)) || (num && eval(d, &buf, num, &w)))
            return buf.len;
        if (w > 64)
            w = 64;
        count = cocodebug_disassemble_ex(d, (uint16_t)v, lines, (int)w, &pcl);
        for (pos = 0; pos < count; pos++) {
            if (lines[pos].label[0])
                out(&buf, "%s:\n", lines[pos].label);
            out(&buf, "%c%c%04X  %-14s %-24s%s%s\n", lines[pos].is_pc ? '>' : ' ',
                lines[pos].has_breakpoint ? '*' : lines[pos].bp_disabled ? 'o' : ' ',
                lines[pos].address, lines[pos].bytes, lines[pos].disasm,
                lines[pos].comment[0] ? "; " : "", lines[pos].comment);
        }
    } else if (!strcmp(cmd, "trace")) {
        char *tok = next_tok(&p);
        if (tok && !strcmp(tok, "on")) {
            cocodebug_trace_enable(d, 1);
        } else if (tok && !strcmp(tok, "off")) {
            cocodebug_trace_enable(d, 0);
        } else if (tok && !strcmp(tok, "clear")) {
            cocodebug_trace_clear(d);
        } else {
            cocodebug_trace_entry ring[64];
            int count, pos;
            w = 16;
            if (tok && eval(d, &buf, tok, &w))
                return buf.len;
            if (w > 64)
                w = 64;
            count = cocodebug_trace_read(d, ring, (int)w);
            if (!count)
                out(&buf, "trace is %s and empty\n", cocodebug_trace_enabled(d) ? "on" : "off");
            for (pos = count - 1; pos >= 0; pos--) {
                char text[256];
                cocodebug_trace_format(&ring[pos], text, sizeof text);
                out(&buf, "%10llu %3d:%3d  %s\n", (unsigned long long)ring[pos].cycles,
                    ring[pos].line, ring[pos].hpos, text);
            }
        }
    } else if (!strcmp(cmd, "watch")) {
        int space = take_space(&p);
        char *text = rest_of(p);
        int pos;
        if (*text && cocowatch_add(text, space) < 0)
            out(&buf, "no room for another watch\n");
        if (!cocowatch_count())
            out(&buf, "no watches (watch [phys] <addr|symbol|lo-hi>)\n");
        else
            for (pos = 0; pos < cocowatch_count(); pos++) {
                char text_line[160];
                cocowatch_format(d, pos, text_line, sizeof text_line);
                out(&buf, "%2d  %s\n", pos, text_line);
            }
    } else if (!strcmp(cmd, "unwatch")) {
        char *tok = next_tok(&p);
        if (tok && !strcmp(tok, "all"))
            cocowatch_clear();
        else if (tok && eval(d, &buf, tok, &v))
            return buf.len;
        else if (tok)
            cocowatch_remove((int)v);
        else
            out(&buf, "unwatch <n|all>\n");
    } else if (!strcmp(cmd, "find")) {
        int space = take_space(&p);
        char *tok = next_tok(&p);
        int kind = COCOSEARCH_HEX, count = 0, flags = 0, found, pos;
        uint8_t pat[COCOSEARCH_MAX], mask;
        uint32_t hits[32];
        char joined[160];
        if (tok && !strcmp(tok, "text")) {
            kind = COCOSEARCH_TEXT;
            tok = rest_of(p);
        } else if (tok && !strcmp(tok, "hex")) {
            tok = rest_of(p);
        } else if (tok) {
            snprintf(joined, sizeof joined, "%s %s", tok, rest_of(p));
            tok = joined;
        }
        if (!tok || !*tok || !cocosearch_parse(kind, tok, pat, &count, &mask, &flags)) {
            out(&buf, "find [phys] [hex] <bytes> | find [phys] text <string>\n");
            return buf.len;
        }
        found = cocodebug_search(d, space, pat, count, mask, flags, 0,
                                 cocodebug_space_size(d, space) - 1, hits, 32);
        if (found < 0)
            out(&buf, "stop the machine first\n");
        else if (!found)
            out(&buf, "not found\n");
        else
            for (pos = 0; pos < found; pos++)
                out(&buf, space == COCODEBUG_SPACE_PHYS ? "$%06X\n" : "$%04X\n", hits[pos]);
    } else if (!strcmp(cmd, "symbols") || !strcmp(cmd, "sym")) {
        char *filter = rest_of(p);
        int picks[400], count, pos;
        count = cocodebug_symbols_filter(d, filter, picks, 400);
        for (pos = 0; pos < count; pos++) {
            uint16_t addr;
            char name[48];
            if (cocodebug_symbols_get(d, picks[pos], &addr, name, sizeof name))
                out(&buf, "%04X  %s\n", addr, name);
        }
        if (!count)
            out(&buf, "no symbols match\n");
    } else if (!strcmp(cmd, "label")) {
        char *tok = next_tok(&p), *name = next_tok(&p);
        if (!tok || !name)
            out(&buf, "usage: label <addr> <name>\n");
        else if (eval(d, &buf, tok, &v) == 0)
            cocodebug_symbol_add(d, (uint16_t)v, name);
    } else if (!strcmp(cmd, "labels")) {
        char *file = rest_of(p);
        int loaded = *file ? cocodebug_symbols_load(d, file) : -1;
        if (loaded < 0)
            out(&buf, "could not read '%s'\n", file);
        else
            out(&buf, "%d symbol(s) loaded\n", loaded);
    } else if (!strcmp(cmd, "source")) {
        char *file = rest_of(p);
        int lines = *file ? cocodebug_source_load(d, file) : -1;
        if (lines < 0)
            out(&buf, "could not read a CMOC listing (and its .map) from '%s'\n", file);
        else
            out(&buf, "%d source line(s) with code\n", lines);
    } else if (!strcmp(cmd, "locals")) {
        cocodebug_local locals[32];
        int count = cocodebug_locals_get(d, locals, 32), pos;
        if (!count)
            out(&buf, "no locals (running, no source info, or before the function body)\n");
        for (pos = 0; pos < count; pos++) {
            out(&buf, "%s %s %s", locals[pos].is_param ? "param" : "local", locals[pos].type,
                locals[pos].name);
            if (locals[pos].has_value)
                out(&buf, " = %u ($%X)", locals[pos].value, locals[pos].value);
            out(&buf, "\n");
        }
    } else if (!strcmp(cmd, "map")) {
        cocodebug_region map[64];
        int count = cocodebug_memory_map(d, map, 64), pos;
        for (pos = 0; pos < count; pos++)
            out(&buf, "$%04X-$%04X  %s%s\n", map[pos].lo, map[pos].hi, map[pos].name,
                map[pos].writable ? "" : " (read only)");
        if (!count)
            out(&buf, "no map yet (stop the machine once)\n");
        else if (cocodebug_space_size(d, COCODEBUG_SPACE_PHYS))
            out(&buf, "physical RAM: %u KB\n", cocodebug_space_size(d, COCODEBUG_SPACE_PHYS) / 1024);
    } else if (!strcmp(cmd, "io")) {
        show_io(d, &buf);
    } else if (!strcmp(cmd, "sound")) {
        cocodebug_sound snd;
        if (!cocodebug_sound_get(d, &snd))
            out(&buf, "the machine is running (stop first)\n");
        else
            out(&buf, "DAC %d  single bit %d  mux source %s%s  motor %s  cassette in %d\n",
                snd.dac, snd.single_bit, snd.mux_name,
                snd.sound_enable ? "" : " (sound off)", snd.motor ? "on" : "off",
                snd.cassette_in);
    } else if (!strcmp(cmd, "cart")) {
        char info[240];
        cocodebug_cart_info(d, info, sizeof info);
        out(&buf, "%s\n", info);
    } else if (!strcmp(cmd, "video")) {
        cocodebug_video_text_fn fn = cocodebug_video_text_get();
        char *section = next_tok(&p);
        static char text[16384];
        int len = fn ? fn(d, section, text, (int)sizeof text) : -1;
        if (!fn)
            out(&buf, "video: not available here\n");
        else if (len <= 0)
            out(&buf, "video: no frame captured yet (stop the machine)\n");
        else
            out(&buf, "%s\n", text);
    } else {
        out(&buf, "unknown command '%s' (try help)\n", cmd);
    }
    return buf.len;
}

int cocodebug_completions(cocodebug *d, const char *prefix, char *dst, int dstsz)
{
    outbuf buf = { dst, dstsz, 0 };
    const char *word;
    size_t len;
    int count = 0, pos;
    const int first_word = !strchr(prefix ? prefix : "", ' ');

    if (!d || !dst || dstsz <= 0)
        return 0;
    dst[0] = '\0';
    if (!prefix)
        prefix = "";
    word = strrchr(prefix, ' ');
    word = word ? word + 1 : prefix;
    len = strlen(word);

    if (first_word) {
        for (pos = 0; commands[pos]; pos++)
            if (!strncmp(commands[pos], word, len)) {
                out(&buf, "%s\n", commands[pos]);
                count++;
            }
        return count;
    }
    for (pos = 0; cocodebug_reg_names[pos]; pos++)
        if (len && !strncmp(cocodebug_reg_names[pos], word, len)) {
            out(&buf, "%s\n", cocodebug_reg_names[pos]);
            count++;
        }
    {
        cocosymtab *table = cocodebug_symtab(d);
        char name[48];
        for (pos = 0; count < 200 && cocosymtab_get(table, pos, NULL, name, sizeof name, NULL);
             pos++)
            if (len && !strncasecmp(name, word, len)) {
                out(&buf, "%s\n", name);
                count++;
            }
    }
    return count;
}
