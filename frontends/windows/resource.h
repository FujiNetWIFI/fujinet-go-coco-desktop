/* Menu/command identifiers for the Windows frontend.
 *
 * The four *_BASE ranges are radio groups built at runtime by walking a
 * cocosession_*_name(idx) table until NULL (see build_menu() in main.c) --
 * there is no fixed menu resource for them, so the ids just need enough
 * headroom for the table to grow (each currently has 2-5 entries).
 */
#ifndef COCO_WIN_RESOURCE_H
#define COCO_WIN_RESOURCE_H

#define IDI_APPICON        101

#define IDM_RESET          201
#define IDM_IMPORT_MEDIA   202
#define IDM_IMPORT_ROMS    203
#define IDM_FUJINET_CONFIG 204
#define IDM_FUJINET_LOG    205
#define IDM_FULLSCREEN     206
#define IDM_DEBUGGER       207
#define IDM_ABOUT          208
#define IDM_EXIT           209

#define IDM_MACHINE_BASE   300
#define IDM_TVINPUT_BASE   320
#define IDM_CCR_BASE       340
#define IDM_CPU_BASE       360
#define IDM_RADIO_SPAN     20  /* headroom per group */

#endif
