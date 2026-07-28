/*
 * cocosession path layout: XDG config/data directories, ROM materialisation
 * and resolution, and first-run provisioning of the FujiNet runtime tree
 * (fnconfig.ini, data/, SD/) from the installed share directory or the
 * development build output.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "roms_embedded.h"
#include "session_internal.h"

#if defined(_WIN32)
#include <direct.h> /* _getcwd */
#include <windows.h>
#define getcwd _getcwd
#else
#include <unistd.h>
#endif

/* Compiled-in development fallbacks (set by CMake to paths in the source /
 * build trees) so a git checkout runs without an install step. */
#ifndef COCO_DEV_FUJINET_OUT
#define COCO_DEV_FUJINET_OUT ""
#endif
#ifndef COCO_INSTALL_DATADIR
#define COCO_INSTALL_DATADIR ""
#endif
#ifndef COCO_INSTALL_LIBDIR
#define COCO_INSTALL_LIBDIR ""
#endif

/* Directory holding the running executable, or "" when it cannot be
 * determined. A Windows install is a folder you copy around -- the exe and
 * fujinet.dll side by side -- so that folder is the first place to look for
 * the runtime. Elsewhere the install/dev directories baked in at configure
 * time answer the question. */
static const char *exe_dir(void)
{
    static char dir[COCO_PATH_MAX];
#if defined(_WIN32)
    static int resolved;
    char *p;

    if (!resolved) {
        resolved = 1;
        if (GetModuleFileNameA(NULL, dir, (DWORD)sizeof(dir)) == 0) {
            dir[0] = '\0';
        } else {
            dir[sizeof(dir) - 1] = '\0';
            p = strrchr(dir, '\\');
            if (!p)
                p = strrchr(dir, '/');
            if (p)
                *p = '\0';
            else
                dir[0] = '\0';
        }
    }
#endif
    return dir;
}

/* mkdir differs: POSIX takes a mode, the Windows CRT does not. */
static int make_dir(const char *path)
{
#if defined(_WIN32)
    return mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

static int is_sep(char c)
{
#if defined(_WIN32)
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

int mkdir_p(const char *path)
{
    char buf[COCO_PATH_MAX];
    char *p;
    if (!path || !*path) return -1;
    snprintf(buf, sizeof(buf), "%s", path);
    for (p = buf + 1; *p; p++) {
        if (is_sep(*p)) {
            char save = *p;
            *p = '\0';
            if (make_dir(buf) != 0 && errno != EEXIST) return -1;
            *p = save;
        }
    }
    if (make_dir(buf) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int is_dir(const char *path)
{
    struct stat st;
    return path && *path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int is_file(const char *path)
{
    struct stat st;
    return path && *path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int copy_file(const char *src, const char *dst)
{
    FILE *in, *out;
    char buf[65536];
    size_t n;
    int rc = 0;

    in = fopen(src, "rb");
    if (!in) return -1;
    out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            rc = -1;
            break;
        }
    }
    if (ferror(in)) rc = -1;
    fclose(in);
    if (fclose(out) != 0) rc = -1;
    return rc;
}

static int copy_tree(const char *src, const char *dst)
{
    DIR *d;
    struct dirent *e;
    int rc = 0;

    if (mkdir_p(dst) != 0) return -1;
    d = opendir(src);
    if (!d) return -1;
    while ((e = readdir(d)) != NULL) {
        char from[COCO_PATH_MAX], to[COCO_PATH_MAX];
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(from, sizeof(from), "%s/%s", src, e->d_name);
        snprintf(to, sizeof(to), "%s/%s", dst, e->d_name);
        if (stat(from, &st) != 0) continue;
        if (S_ISDIR(st.st_mode))
            rc |= copy_tree(from, to);
        else if (S_ISREG(st.st_mode))
            rc |= copy_file(from, to);
    }
    closedir(d);
    return rc;
}

/* Write the compiled-in ROM images into the ROM directory, skipping any name
 * that is already there. That skip is the whole policy: a build with the Tandy
 * system ROMs compiled in lays them down once, a build without them lays down
 * only the freely redistributable HDB-DOS pair, and in either case a ROM the
 * user imported (or hand-copied) is never overwritten.
 *
 * Failures are silent by design -- a read-only or full data directory should
 * degrade to "no ROMs found, please import some", which the frontends already
 * handle, not to a session that refuses to be created. */
void paths_materialise_roms(cocosession *s)
{
    char dst[COCO_PATH_MAX];
    int i;

    for (i = 0; i < coco_embedded_rom_count; i++) {
        const coco_embedded_rom *r = &coco_embedded_roms[i];
        FILE *f;
        snprintf(dst, sizeof(dst), "%s/%s", s->roms_dir, r->name);
        if (is_file(dst))
            continue;
        f = fopen(dst, "wb");
        if (!f)
            continue;
        if (fwrite(r->data, 1, r->size, f) != r->size) {
            fclose(f);
            remove(dst); /* a half-written ROM is worse than none at all */
            continue;
        }
        fclose(f);
    }
}

/* The ROM basenames each machine cannot boot without. XRoar will start without
 * them, but the machine comes up to a blank screen, so the frontends ask
 * first. Cartridge ROMs are deliberately not listed: HDB-DOS is always
 * present, and a missing one costs the FujiNet link, not the boot. */
int roms_present_for_machine(const char *dir, int machine)
{
    static const char *const coco2[] = { "bas13.rom", "extbas11.rom", NULL };
    static const char *const coco3[] = { "coco3.rom", NULL };
    const char *const *need =
        (machine == COCOSESSION_MACHINE_COCO2) ? coco2 : coco3;
    char probe[COCO_PATH_MAX];
    int i;

    for (i = 0; need[i]; i++) {
        snprintf(probe, sizeof(probe), "%s/%s", dir, need[i]);
        if (!is_file(probe))
            return 0;
    }
    return 1;
}

/* Per-user config/data directory. On Windows this is %APPDATA% (config)
 * or %LOCALAPPDATA% (data); elsewhere the XDG variable, then $HOME/suffix. */
static void default_dir(char *dst, size_t dstsz, const char *xdg_env,
                        const char *win_env, const char *home_suffix)
{
#if defined(_WIN32)
    const char *v = getenv(win_env);
    (void)xdg_env;
    (void)home_suffix;
    snprintf(dst, dstsz, "%s\\fujinet-go-coco", (v && *v) ? v : ".");
#else
    const char *v = getenv(xdg_env);
    (void)win_env;
    if (v && *v) {
        snprintf(dst, dstsz, "%s/fujinet-go-coco", v);
    } else {
        const char *home = getenv("HOME");
        snprintf(dst, dstsz, "%s/%s/fujinet-go-coco", home ? home : ".",
                 home_suffix);
    }
#endif
}

/* Is this path already rooted? On Windows a leading separator is not enough:
 * "\foo" is relative to the current drive. */
static int is_absolute(const char *p)
{
    if (!p || !*p) return 0;
#if defined(_WIN32)
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) &&
        p[1] == ':' && is_sep(p[2]))
        return 1;
    return is_sep(p[0]) && is_sep(p[1]); /* \\server\share */
#else
    return p[0] == '/';
#endif
}

/* Rewrite a relative directory in place against the current working
 * directory.
 *
 * This is not cosmetic, and it is more important here than on the other
 * targets. The FujiNet entry wrapper chdir()s into its runtime root before
 * calling main_setup, and on THIS target FujiNet starts before the emulator
 * -- so by the time XRoar resolves -rompath, the process CWD is somewhere
 * else entirely and a relative path silently finds nothing. The machine then
 * comes up with "(unpopulated)" ROM slots and a blank screen, with the files
 * sitting right there on disk.
 *
 * Resolving once, at startup, means nothing downstream has to care who
 * chdir'd. Left alone if it cannot be resolved: a relative path is still
 * better than an empty one. */
static void make_absolute(char *path, size_t size)
{
    char cwd[COCO_PATH_MAX];
    char joined[COCO_PATH_MAX];

    if (is_absolute(path))
        return;
    if (!getcwd(cwd, sizeof(cwd)))
        return;
    snprintf(joined, sizeof(joined), "%s/%s", cwd, path);
    snprintf(path, size, "%s", joined);
}

int paths_init(cocosession *s, const cocosession_paths *p)
{
    if (p && p->config_dir && *p->config_dir)
        snprintf(s->config_dir, sizeof(s->config_dir), "%s", p->config_dir);
    else
        default_dir(s->config_dir, sizeof(s->config_dir), "XDG_CONFIG_HOME",
                    "APPDATA", ".config");

    if (p && p->data_dir && *p->data_dir)
        snprintf(s->data_dir, sizeof(s->data_dir), "%s", p->data_dir);
    else
        default_dir(s->data_dir, sizeof(s->data_dir), "XDG_DATA_HOME",
                    "LOCALAPPDATA", ".local/share");

    make_absolute(s->config_dir, sizeof(s->config_dir));
    make_absolute(s->data_dir, sizeof(s->data_dir));

    if (mkdir_p(s->config_dir) != 0 || mkdir_p(s->data_dir) != 0)
        return -1;

    snprintf(s->settings_file, sizeof(s->settings_file), "%s/settings.ini",
             s->config_dir);

    /* XRoar's -rompath. Everything the machine needs is resolved from here by
     * exact basename, so it holds both the compiled-in ROMs this build
     * materialises and whatever the user imports. COCO_ROM_DIR overrides it
     * outright, which is how the test suite points at a fixture directory. */
    {
        const char *env = getenv("COCO_ROM_DIR");
        if (env && *env)
            snprintf(s->roms_dir, sizeof(s->roms_dir), "%s", env);
        else
            snprintf(s->roms_dir, sizeof(s->roms_dir), "%s/roms", s->data_dir);
    }
    /* COCO_ROM_DIR may well be relative -- see make_absolute. */
    make_absolute(s->roms_dir, sizeof(s->roms_dir));
    mkdir_p(s->roms_dir);
    paths_materialise_roms(s);

    /* The FujiNet runtime tree. These are pure derivations of data_dir, so
     * they are settled here rather than in paths_provision_fujinet -- that
     * runs only when the runtime is actually started, and a session with
     * FujiNet disabled would otherwise have an empty SD path, which makes
     * "Import Disk Image..." fail with "No FujiNet SD directory" and
     * cocosession_sd_path() return "". Knowing WHERE the tree goes does not
     * depend on the library being found; provisioning it does. */
    snprintf(s->fujinet_root, sizeof(s->fujinet_root), "%s/fujinet",
             s->data_dir);
    snprintf(s->fujinet_config, sizeof(s->fujinet_config), "%s/fnconfig.ini",
             s->fujinet_root);
    snprintf(s->fujinet_sd, sizeof(s->fujinet_sd), "%s/SD", s->fujinet_root);
    snprintf(s->fujinet_data, sizeof(s->fujinet_data), "%s/data",
             s->fujinet_root);

    /* Cartridge images (.rom/.ccc) the user imports. Separate from roms_dir so
     * a cartridge cannot shadow a system ROM by sharing its basename. */
    snprintf(s->carts_dir, sizeof(s->carts_dir), "%s/carts", s->data_dir);
    mkdir_p(s->carts_dir);

    if (p && p->fujinet_lib) /* may be "" = explicitly disabled */
        snprintf(s->fujinet_lib, sizeof(s->fujinet_lib), "%s", p->fujinet_lib);
    else
        s->fujinet_lib[0] = '\0'; /* resolved in paths_provision_fujinet */

    if (p && p->fujinet_runtime_src && *p->fujinet_runtime_src)
        snprintf(s->fujinet_src, sizeof(s->fujinet_src), "%s",
                 p->fujinet_runtime_src);
    else
        s->fujinet_src[0] = '\0';

    snprintf(s->webui_url, sizeof(s->webui_url), "http://127.0.0.1:%d/",
             COCOSESSION_WEBUI_PORT);
    return 0;
}

/* Locate libfujinet.so and provision the runtime tree on first run. The
 * runtime tree (fnconfig.ini + data/ + SD/) is copied from the newest
 * available source: the installed share dir or the dev build output. */
int paths_provision_fujinet(cocosession *s)
{
    const char *env;
    char src_root[COCO_PATH_MAX];
    char probe[COCO_PATH_MAX];

    /* Resolve the shared library unless the caller pinned/disabled it.
     * Every platform's name is probed so this path layer is shared
     * unchanged (.so Linux, .dylib macOS, .dll Windows). */
    if (!s->fujinet_lib[0]) {
        static const char *const names[] = {
            "libfujinet.so", "libfujinet.dylib", "fujinet.dll"};
        const char *const dirs[] = {exe_dir(), COCO_INSTALL_LIBDIR,
                                    COCO_DEV_FUJINET_OUT};
        const size_t nnames = sizeof(names) / sizeof(names[0]);
        const size_t ndirs = sizeof(dirs) / sizeof(dirs[0]);
        size_t di, ni;
        env = getenv("FUJINET_LIB");
        if (env && is_file(env)) {
            snprintf(s->fujinet_lib, sizeof(s->fujinet_lib), "%s", env);
        } else {
            for (di = 0; di < ndirs && !s->fujinet_lib[0]; di++)
                for (ni = 0; ni < nnames && !s->fujinet_lib[0]; ni++) {
                    if (!dirs[di][0])
                        continue;
                    snprintf(probe, sizeof(probe), "%s/%s", dirs[di],
                             names[ni]);
                    if (is_file(probe))
                        snprintf(s->fujinet_lib, sizeof(s->fujinet_lib),
                                 "%s", probe);
                }
        }
    }
    if (!s->fujinet_lib[0])
        return -1; /* no runtime available; session runs without FujiNet */

    /* Provision the runtime tree once (keep user data on later runs). */
    if (!is_file(s->fujinet_config)) {
        src_root[0] = '\0';
        if (s->fujinet_src[0]) {
            snprintf(probe, sizeof(probe), "%s/fnconfig.ini",
                     s->fujinet_src);
            if (is_file(probe))
                snprintf(src_root, sizeof(src_root), "%s", s->fujinet_src);
        }
        /* Beside the executable, the layout a Windows install ships. */
        if (!src_root[0] && exe_dir()[0]) {
            snprintf(probe, sizeof(probe), "%s/fujinet/fnconfig.ini",
                     exe_dir());
            if (is_file(probe))
                snprintf(src_root, sizeof(src_root), "%s/fujinet", exe_dir());
        }
        if (!src_root[0]) {
            snprintf(probe, sizeof(probe), "%s/fujinet/fnconfig.ini",
                     COCO_INSTALL_DATADIR);
            if (is_file(probe))
                snprintf(src_root, sizeof(src_root), "%s/fujinet",
                         COCO_INSTALL_DATADIR);
        }
        if (!src_root[0]) {
            snprintf(probe, sizeof(probe), "%s/fnconfig.ini",
                     COCO_DEV_FUJINET_OUT);
            if (is_file(probe))
                snprintf(src_root, sizeof(src_root), "%s",
                         COCO_DEV_FUJINET_OUT);
        }
        if (!src_root[0]) {
            session_set_error(s, "FujiNet runtime data not found (looked in "
                              "%s and %s)", COCO_INSTALL_DATADIR,
                              COCO_DEV_FUJINET_OUT);
            s->fujinet_lib[0] = '\0';
            return -1;
        }
        mkdir_p(s->fujinet_root);
        snprintf(probe, sizeof(probe), "%s/fnconfig.ini", src_root);
        copy_file(probe, s->fujinet_config);
        snprintf(probe, sizeof(probe), "%s/data", src_root);
        if (is_dir(probe)) copy_tree(probe, s->fujinet_data);
        snprintf(probe, sizeof(probe), "%s/SD", src_root);
        if (is_dir(probe)) copy_tree(probe, s->fujinet_sd);
    }
    mkdir_p(s->fujinet_sd);
    mkdir_p(s->fujinet_data);
    return 0;
}
