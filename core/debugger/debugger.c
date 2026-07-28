/*
 * cocodebug -- the 6809/6309 debugger engine.
 *
 * PLACEHOLDER. The session already carries the seam the engine plugs into --
 * s->dbg_engaged switches the emulator loop to the stepped path, and stop()
 * unparks a paused debugger so the thread can exit -- so this file exists to
 * satisfy that seam while the engine itself is written (milestone M6).
 *
 * Until then cocodebug_create() returns NULL, so cocosession_debugger() hands
 * frontends NULL and they leave the debugger action insensitive. Nothing else
 * changes: dbg_engaged is never set, and the loop runs the plain path.
 *
 * The real engine will build on XRoar's own hooks rather than a stepped core
 * path of its own: struct debug_cpu (xroar src/debug_cpu.h) provides
 * instruction_hook / instruction_posthook / get_pc / set_pc delegates, and
 * src/breakpoint.c a breakpoint list.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "coco_host.h"
#include "session_internal.h"

cocodebug *cocodebug_create(cocosession *s)
{
    (void)s;
    return NULL;
}

void cocodebug_destroy(cocodebug *d)
{
    (void)d;
}

int cocodebug_session_frame(cocosession *s)
{
    /* Unreachable while create() returns NULL (nothing can set dbg_engaged),
     * but correct if it is ever called: run the frame the loop would have. */
    (void)s;
    cocohost_core_run_frame();
    return 1;
}

void cocodebug_resume_for_stop(cocodebug *d)
{
    (void)d;
}
