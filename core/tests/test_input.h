/*
 * test_input.h -- what the input tests share: a pass/fail line, a nap, and a
 * session on a fresh temporary directory (no ROMs, no FujiNet, no machine).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef COCO_TEST_INPUT_H
#define COCO_TEST_INPUT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cocosession.h"
#include "test_tmpdir.h"

static int failures;

static void check(int pass, const char *what)
{
    printf("%s: %s\n", pass ? "ok" : "FAIL", what);
    if (!pass) failures++;
}

static inline void nap_ms(int msec)
{
    struct timespec span;

    span.tv_sec = msec / 1000;
    span.tv_nsec = (long)(msec % 1000) * 1000000L;
    nanosleep(&span, NULL);
}

/* A new directory for config and data (the caller's buffers hold the paths,
 * which a later session over the same settings reuses). */
static int test_dirs(const char *tag, char *config, size_t configsz, char *data, size_t datasz)
{
    char tmpl[512];
    char *base;

    test_tmpl(tmpl, sizeof tmpl, tag);
    base = mkdtemp(tmpl);
    if (!base) {
        perror("mkdtemp");
        return -1;
    }
    snprintf(config, configsz, "%s/config", base);
    snprintf(data, datasz, "%s/data", base);
    return 0;
}

static cocosession *test_session(const char *config, const char *data)
{
    cocosession_paths paths;

    memset(&paths, 0, sizeof paths);
    paths.config_dir = config;
    paths.data_dir = data;
    paths.fujinet_lib = "";      /* no runtime: these tests must not bind a port */
    return cocosession_new(&paths);
}

#endif
