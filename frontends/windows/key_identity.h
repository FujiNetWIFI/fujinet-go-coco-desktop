/*
 * Win32 key messages to the (keysym, unicode) pair cocosession_key takes, and
 * from it the key identity the bindings use. The main window and the
 * Controllers window share it, so a key captured in Map mode is exactly the
 * key cocosession_key would see.
 *
 * Win32's VK_* codes carry no shift state of their own (VK_A is the same code
 * whether or not shift is held), so rather than reconstruct a shifted symbol
 * with ToUnicode (and have to keep key-down and key-up consistent despite the
 * transient modifier state), non-special keys are resolved straight to their
 * base US-layout character -- 'a', '1', '-', ... -- which is exactly the
 * "unshifted symbol on the same physical key" coco_key_from_event's own
 * printable_scancode()/unshift_us() want, and is stable across down/up by
 * construction.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef COCO_WIN_KEY_IDENTITY_H
#define COCO_WIN_KEY_IDENTITY_H

#include <stdint.h>
#include <windows.h>

static inline unsigned coco_special_keysym(WPARAM vkey)
{
    switch (vkey) {
    case VK_RETURN:   return 0xFF0D;
    case VK_ESCAPE:   return 0xFF1B;
    case VK_TAB:      return 0xFF09;
    case VK_BACK:     return 0xFF08;
    case VK_DELETE:   return 0xFFFF;
    case VK_INSERT:   return 0xFF63;
    case VK_HOME:     return 0xFF50;
    case VK_END:      return 0xFF57;
    case VK_PRIOR:    return 0xFF55; /* Page Up */
    case VK_NEXT:     return 0xFF56; /* Page Down */
    case VK_UP:       return 0xFF52;
    case VK_DOWN:     return 0xFF54;
    case VK_LEFT:     return 0xFF51;
    case VK_RIGHT:    return 0xFF53;
    case VK_CAPITAL:  return 0xFFE5;
    case VK_LSHIFT:   return 0xFFE1;
    case VK_RSHIFT:   return 0xFFE2;
    case VK_LCONTROL: return 0xFFE3;
    case VK_RCONTROL: return 0xFFE4;
    case VK_LMENU:    return 0xFFE9;
    case VK_RMENU:    return 0xFFEA;
    case VK_LWIN:     return 0xFFEB;
    case VK_RWIN:     return 0xFFEC;
    case VK_NUMPAD0:  return 0xFFB0;
    case VK_DECIMAL:  return 0xFFAE;
    case VK_DIVIDE:   return 0xFFAF;
    case VK_MULTIPLY: return 0xFFAA;
    case VK_SUBTRACT: return 0xFFAD;
    case VK_ADD:      return 0xFFAB;
    default:
        if (vkey >= VK_F1 && vkey <= VK_F12)
            return 0xFFBE + (unsigned)(vkey - VK_F1);
        if (vkey >= VK_NUMPAD1 && vkey <= VK_NUMPAD9)
            return 0xFFB1 + (unsigned)(vkey - VK_NUMPAD1);
        return 0;
    }
}

/* The base (unshifted) US-layout character for a printable key. Matches
 * core/src/input_map.c's printable_scancode() switch one for one. */
static inline uint32_t coco_base_char(WPARAM vkey)
{
    if (vkey >= 'A' && vkey <= 'Z') return (uint32_t)(vkey - 'A' + 'a');
    if (vkey >= '0' && vkey <= '9') return (uint32_t)vkey;
    switch (vkey) {
    case VK_SPACE:      return ' ';
    case VK_OEM_MINUS:  return '-';
    case VK_OEM_PLUS:   return '=';
    case VK_OEM_4:      return '[';
    case VK_OEM_6:      return ']';
    case VK_OEM_5:      return '\\';
    case VK_OEM_1:      return ';';
    case VK_OEM_7:      return '\'';
    case VK_OEM_3:      return '`';
    case VK_OEM_COMMA:  return ',';
    case VK_OEM_PERIOD: return '.';
    case VK_OEM_2:      return '/';
    default:            return 0;
    }
}

/* WM_(SYS)KEYDOWN/UP only report the generic VK_SHIFT/CONTROL/MENU; the
 * left/right pair is recovered from the scancode (Shift) or the extended-key
 * bit (Control/Alt), the standard Win32 idiom for this. */
static inline WPARAM coco_resolve_side(WPARAM vkey, LPARAM lpar)
{
    switch (vkey) {
    case VK_SHIFT:
        return MapVirtualKeyA((UINT)((lpar >> 16) & 0xFF), MAPVK_VSC_TO_VK_EX);
    case VK_CONTROL:
        return (lpar & 0x01000000) ? VK_RCONTROL : VK_LCONTROL;
    case VK_MENU:
        return (lpar & 0x01000000) ? VK_RMENU : VK_LMENU;
    default:
        return vkey;
    }
}

/* The pair to give cocosession_key (and cocosession_bindable_key) for a key
 * message; 0 when it is not a key the emulated keyboard has. */
static inline int coco_key_identity(WPARAM vkey, LPARAM lpar, unsigned *keysym, uint32_t *unicode)
{
    const WPARAM resolved = coco_resolve_side(vkey, lpar);

    *unicode = 0;
    *keysym = coco_special_keysym(resolved);
    if (*keysym)
        return 1;
    *unicode = coco_base_char(resolved);
    return *unicode != 0;
}

#endif /* COCO_WIN_KEY_IDENTITY_H */
