/*
 * The Win32 ROMs window: a note on where the BIOS comes from and where it is
 * kept, Import... and Import a Folder..., and a list of each ROM item,
 * imported or not (the other frontends' Preferences ROMs page). Also home of
 * the BIOS import itself, which the menu and the No-BIOS page share.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "prefs.h"

#include <commctrl.h>
#include <commdlg.h>
#include <objbase.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

#include "resource.h"

#define ROMS_CLASS "CocoRomsWindow"

/* ---- import ---------------------------------------------------------------- */

static const char k_filter[] =
    "BIOS images and ROM sets\0*.zip;*.rom;*.bin\0All files\0*.*\0\0";

typedef struct {
    cocosession *session;
    char log[4096];
    int imported;
} import_log;

static void import_one(import_log *l, const char *path)
{
    char msg[1024];
    size_t len = strlen(l->log);
    msg[0] = '\0';
    if (cocosession_rom_import(l->session, path, msg, sizeof(msg)) == 0)
        l->imported++;
    snprintf(l->log + len, sizeof(l->log) - len, "%s%s", len ? "\n" : "", msg);
}

/* OFN_ALLOWMULTISELECT packs several picks as "dir\0file1\0file2\0\0" and a
 * single one as the whole path. */
static int pick_files(HWND owner, import_log *l)
{
    static char buf[64 * 1024];
    OPENFILENAMEA ofn;
    const char *p;

    memset(&ofn, 0, sizeof(ofn));
    buf[0] = buf[1] = '\0';
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = k_filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = sizeof(buf);
    ofn.lpstrTitle = "Import BIOS";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                OFN_EXPLORER | OFN_ALLOWMULTISELECT;
    if (!GetOpenFileNameA(&ofn))
        return 0;
    p = buf + strlen(buf) + 1;
    if (!*p) {
        import_one(l, buf);
        return 1;
    }
    for (; *p; p += strlen(p) + 1) {
        char path[MAX_PATH * 2];
        snprintf(path, sizeof(path), "%s\\%s", buf, p);
        import_one(l, path);
    }
    return 1;
}

static int pick_folder(HWND owner, char *path, DWORD pathsz)
{
    BROWSEINFOA bi;
    PIDLIST_ABSOLUTE pidl;
    char dir[MAX_PATH] = "";

    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = owner;
    bi.lpszTitle = "Import BIOS from a Folder (MAME's coco and coco3 sets work)";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    pidl = SHBrowseForFolderA(&bi);
    if (!pidl)
        return 0;
    if (!SHGetPathFromIDListA(pidl, dir))
        dir[0] = '\0';
    CoTaskMemFree(pidl);
    snprintf(path, pathsz, "%s", dir);
    return path[0] != '\0';
}

int coco_import_bios(HWND owner, cocosession *session, int folder)
{
    static import_log l;

    memset(&l, 0, sizeof(l));
    l.session = session;
    if (folder) {
        char dir[MAX_PATH];
        if (!pick_folder(owner, dir, sizeof(dir)))
            return -1;
        import_one(&l, dir);
    } else if (!pick_files(owner, &l)) {
        return -1;
    }
    if (l.log[0])
        MessageBoxA(owner, l.log,
                    l.imported ? "BIOS Imported" : "Nothing Imported",
                    (l.imported ? MB_ICONINFORMATION : MB_ICONWARNING) | MB_OK);
    return l.imported;
}

/* ---- ROMs window ------------------------------------------------------------- */

enum { ID_FILES = 100, ID_FOLDER, ID_LIST, ID_CLOSE };

static HWND g_win, g_note, g_path, g_files, g_folder, g_list, g_close;
static cocosession *g_session;
static coco_prefs_imported_fn g_imported;
static int g_dpi = 96;

static int S(int v) { return MulDiv(v, g_dpi, 96); }

static void refresh_roms(void)
{
    int i;
    SendMessageA(g_list, LVM_DELETEALLITEMS, 0, 0);
    for (i = 0; i < cocosession_rom_item_count(); i++) {
        const coco_rom_item *item = cocosession_rom_item(i);
        LVITEMA it;
        memset(&it, 0, sizeof(it));
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = (char *)item->name;
        SendMessageA(g_list, LVM_INSERTITEMA, 0, (LPARAM)&it);
        it.iSubItem = 1;
        it.pszText = (char *)item->group;
        SendMessageA(g_list, LVM_SETITEMTEXTA, (WPARAM)i, (LPARAM)&it);
        it.iSubItem = 2;
        it.pszText = cocosession_rom_item_available(g_session, i)
                         ? "Imported" : "Not imported";
        SendMessageA(g_list, LVM_SETITEMTEXTA, (WPARAM)i, (LPARAM)&it);
    }
}

static void layout(const RECT *c)
{
    const int m = S(12), w = c->right - 2 * m;
    LVCOLUMNA col;

    MoveWindow(g_note, m, m, w, S(64), TRUE);
    MoveWindow(g_path, m, m + S(66), w, S(20), TRUE);
    MoveWindow(g_files, m, m + S(94), S(140), S(28), TRUE);
    MoveWindow(g_folder, m + S(148), m + S(94), S(160), S(28), TRUE);
    MoveWindow(g_list, m, m + S(132), w, c->bottom - m - S(132) - S(44), TRUE);
    MoveWindow(g_close, c->right - m - S(96), c->bottom - m - S(28), S(96),
               S(28), TRUE);
    memset(&col, 0, sizeof(col));
    col.mask = LVCF_WIDTH;
    col.cx = w - S(90) - S(110) - GetSystemMetrics(SM_CXVSCROLL) - S(4);
    SendMessageA(g_list, LVM_SETCOLUMNA, 0, (LPARAM)&col);
    col.cx = S(90);
    SendMessageA(g_list, LVM_SETCOLUMNA, 1, (LPARAM)&col);
    col.cx = S(110);
    SendMessageA(g_list, LVM_SETCOLUMNA, 2, (LPARAM)&col);
}

static void import_roms(int folder)
{
    if (coco_import_bios(g_win, g_session, folder) < 0)
        return;
    refresh_roms();
    if (g_imported)
        g_imported();
}

static LRESULT CALLBACK roms_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_FILES:  import_roms(0); return 0;
        case ID_FOLDER: import_roms(1); return 0;
        case ID_CLOSE:
        case IDCANCEL:
            DestroyWindow(hwnd);
            return 0;
        default:
            break;
        }
        break;
    case WM_SIZE:
        if (g_list) {
            RECT c;
            GetClientRect(hwnd, &c);
            layout(&c);
        }
        return 0;
    case WM_DESTROY:
        g_win = NULL;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static HWND child(const char *cls, const char *text, DWORD style, int id,
                  HINSTANCE inst)
{
    HWND h = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0,
                             1, 1, g_win, (HMENU)(INT_PTR)id, inst, NULL);
    SendMessageA(h, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return h;
}

void coco_roms_show(HWND parent, cocosession *session, coco_prefs_imported_fn imported)
{
    static int registered;
    HINSTANCE inst = GetModuleHandleA(NULL);
    char path[1024], *c;
    RECT r = {0, 0, 0, 0}, cr;
    HDC dc;
    static const char *const cols[] = {"ROM", "Group", "State"};
    int i;

    if (g_win) {
        SetForegroundWindow(g_win);
        return;
    }
    g_session = session;
    g_imported = imported;
    if (!registered) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = roms_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = ROMS_CLASS;
        wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(IDI_APPICON));
        RegisterClassA(&wc);
        registered = 1;
    }
    dc = GetDC(NULL);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(NULL, dc);

    /* fixed size, sized from the client at this DPI */
    r.right = S(560);
    r.bottom = S(360);
    AdjustWindowRect(&r, WS_CAPTION | WS_SYSMENU, FALSE);
    g_win = CreateWindowExA(WS_EX_DLGMODALFRAME, ROMS_CLASS, "ROMs",
                            WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                            r.bottom - r.top, parent, NULL, inst, NULL);
    if (!g_win)
        return;

    g_note = child("STATIC",
        "The CoCo's system ROMs (BASIC) are not part of the app. Import them from "
        "a file, a zip or a folder (MAME's coco and coco3 sets work); they are "
        "recognised by size and checksum, whatever they are named. Kept in:",
        SS_LEFT | SS_NOPREFIX, 0, inst);
    snprintf(path, sizeof(path), "%s", cocosession_roms_path(session));
    for (c = path; *c; c++)
        if (*c == '/') *c = '\\';
    g_path = child("STATIC", path, SS_LEFT | SS_NOPREFIX | SS_PATHELLIPSIS, 0, inst);
    g_files = child("BUTTON", "Import...", WS_TABSTOP, ID_FILES, inst);
    g_folder = child("BUTTON", "Import a Folder...", WS_TABSTOP, ID_FOLDER, inst);
    g_list = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
                             WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL |
                                 LVS_NOSORTHEADER,
                             0, 0, 1, 1, g_win, (HMENU)(INT_PTR)ID_LIST, inst,
                             NULL);
    SendMessageA(g_list, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    SendMessageA(g_list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT);
    for (i = 0; i < 3; i++) {
        LVCOLUMNA col;
        memset(&col, 0, sizeof(col));
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = 100;
        col.pszText = (char *)cols[i];
        SendMessageA(g_list, LVM_INSERTCOLUMNA, (WPARAM)i, (LPARAM)&col);
    }
    g_close = child("BUTTON", "Close", WS_TABSTOP | BS_DEFPUSHBUTTON, ID_CLOSE, inst);

    GetClientRect(g_win, &cr);
    layout(&cr);
    refresh_roms();
    ShowWindow(g_win, SW_SHOW);
}
