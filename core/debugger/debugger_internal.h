/*
 * debugger_internal -- the engine's side shared by debugger.c, prompt.c and
 * memtools.c. Not installed; frontends include cocodebug.h only.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef COCO_DEBUGGER_INTERNAL_H
#define COCO_DEBUGGER_INTERNAL_H

#include "cocodebug.h"
#include "symtab.h"

/* The video text provider the prompt's `video` calls, or NULL. */
cocodebug_video_text_fn cocodebug_video_text_get(void);

/* Register names the evaluator knows, for completion; NULL-terminated. */
extern const char *const cocodebug_reg_names[];

/* Evaluate with `value`, `addr` and `old` unavailable (not in a condition). */
int cocodebug_eval_plain(cocodebug *d, const char *text, long *out, char *err,
                         int errsz);

/* The symbol table, for completion. */
cocosymtab *cocodebug_symtab(cocodebug *d);

/* Whether the CPU is an HD6309 (a stopped machine's answer; cached). */
int cocodebug_cpu_is_6309(cocodebug *d);

/* For tests: whether the machine's CPU has our instruction hook installed,
 * and whether it chains to another (XRoar's own) delegate. */
void cocodebug_hook_state(cocodebug *d, int *ours, int *chained);

#endif /* COCO_DEBUGGER_INTERNAL_H */
