/*
 * Key and joystick mapping: X11/xkb keysym -> XRoar hkbd scancode, and gamepad
 * stick -> CoCo analog joystick DAC value.
 *
 * Every frontend translates its toolkit's key events to X11/xkb keysyms (GDK
 * keyvals already are; Qt, AppKit and Win32 map through a small table) and then
 * calls cocosession_key, so this one table serves all four.
 *
 * XRoar's host keyboard layer (src/hkbd.h) takes USB HID usage codes and is
 * *positional*: hk_scan_2 is "the key two along the top row", and whether that
 * types " or @ on the emulated machine is the CoCo's business, not ours. So the
 * job here is to recover which physical key the event came from.
 *
 * A keysym is a symbol, not a position, so the recovery is: take the symbol,
 * map it back to the unshifted symbol on the same physical key of a US/ANSI
 * keyboard, and look that up. That is exact for US layouts and close enough
 * elsewhere -- a UK user pressing shift-2 sends " and lands on hk_scan_2, which
 * is the key they pressed and, as it happens, the key that types " on a CoCo
 * too. Layouts that put a symbol on a genuinely different key (AZERTY's digit
 * row, say) type by symbol rather than by position; XRoar's own untranslated
 * mode has the same limitation for the same reason.
 *
 * Ported from the Android app's input/Coco.kt, widened from the on-screen
 * keyboard's subset to a full desktop keyboard.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>

#include "cocosession.h"

/* --- XRoar hkbd scancodes (USB HID usage codes; xroar src/hkbd.h) --------- */
#define HK_a            0x04
#define HK_1            0x1e
#define HK_0            0x27
#define HK_RETURN       0x28
#define HK_ESCAPE       0x29   /* BREAK */
#define HK_BACKSPACE    0x2a
#define HK_TAB          0x2b
#define HK_SPACE        0x2c
#define HK_MINUS        0x2d
#define HK_EQUAL        0x2e
#define HK_BRACKETLEFT  0x2f
#define HK_BRACKETRIGHT 0x30
#define HK_BACKSLASH    0x31
#define HK_SEMICOLON    0x33
#define HK_APOSTROPHE   0x34
#define HK_GRAVE        0x35
#define HK_COMMA        0x36
#define HK_PERIOD       0x37
#define HK_SLASH        0x38
#define HK_CAPS_LOCK    0x39
#define HK_F1           0x3a
#define HK_F9           0x42
#define HK_INSERT       0x49
#define HK_HOME         0x4a   /* CLEAR */
#define HK_PAGE_UP      0x4b
#define HK_DELETE       0x4c
#define HK_END          0x4d
#define HK_PAGE_DOWN    0x4e
#define HK_RIGHT        0x4f
#define HK_LEFT         0x50
#define HK_DOWN         0x51
#define HK_UP           0x52
#define HK_KP_DIVIDE    0x54
#define HK_KP_MULTIPLY  0x55
#define HK_KP_SUBTRACT  0x56
#define HK_KP_ADD       0x57
#define HK_KP_ENTER     0x58
#define HK_KP_1         0x59
#define HK_KP_0         0x62
#define HK_KP_DECIMAL   0x63
#define HK_CONTROL_L    0xe0
#define HK_SHIFT_L      0xe1
#define HK_ALT_L        0xe2
#define HK_SUPER_L      0xe3
#define HK_CONTROL_R    0xe4
#define HK_SHIFT_R      0xe5
#define HK_ALT_R        0xe6
#define HK_SUPER_R      0xe7

/* --- X11/xkb keysyms ----------------------------------------------------- */
#define XKS_BACKSPACE 0xFF08
#define XKS_TAB       0xFF09
#define XKS_RETURN    0xFF0D
#define XKS_ESCAPE    0xFF1B
#define XKS_DELETE    0xFFFF
#define XKS_HOME      0xFF50
#define XKS_LEFT      0xFF51
#define XKS_UP        0xFF52
#define XKS_RIGHT     0xFF53
#define XKS_DOWN      0xFF54
#define XKS_PAGE_UP   0xFF55
#define XKS_PAGE_DOWN 0xFF56
#define XKS_END       0xFF57
#define XKS_INSERT    0xFF63
#define XKS_KP_ENTER  0xFF8D
#define XKS_KP_MULT   0xFFAA
#define XKS_KP_ADD    0xFFAB
#define XKS_KP_SUB    0xFFAD
#define XKS_KP_DEC    0xFFAE
#define XKS_KP_DIV    0xFFAF
#define XKS_KP_0      0xFFB0
#define XKS_KP_9      0xFFB9
#define XKS_F1        0xFFBE
#define XKS_F9        0xFFC6
#define XKS_F10       0xFFC7
#define XKS_F11       0xFFC8
#define XKS_F12       0xFFC9
#define XKS_SHIFT_L   0xFFE1
#define XKS_SHIFT_R   0xFFE2
#define XKS_CONTROL_L 0xFFE3
#define XKS_CONTROL_R 0xFFE4
#define XKS_CAPS_LOCK 0xFFE5
#define XKS_META_L    0xFFE7
#define XKS_META_R    0xFFE8
#define XKS_ALT_L     0xFFE9
#define XKS_ALT_R     0xFFEA
#define XKS_SUPER_L   0xFFEB
#define XKS_SUPER_R   0xFFEC

/* Non-printing keys, resolved by keysym alone. Returns 0 for anything not in
 * the table. */
static unsigned special_scancode(uint32_t keysym)
{
    /* Function keys. F1 and F2 are real keys on a CoCo 3; the rest exist so a
     * cartridge or an OS-9 program that watches for them can see them. F10,
     * F11 and F12 are handled by the caller and never reach here. */
    if (keysym >= XKS_F1 && keysym <= XKS_F9)
        return HK_F1 + (keysym - XKS_F1);

    /* Numeric keypad digits. HID puts KP_1..KP_9 before KP_0. */
    if (keysym > XKS_KP_0 && keysym <= XKS_KP_9)
        return HK_KP_1 + (keysym - XKS_KP_0 - 1);
    if (keysym == XKS_KP_0)
        return HK_KP_0;

    switch (keysym) {
    case XKS_RETURN:     return HK_RETURN;
    case XKS_KP_ENTER:   return HK_KP_ENTER;
    /* ESCAPE is the CoCo's BREAK key: XRoar's untranslated keymap puts BREAK
     * there, and it is what interrupts a running BASIC program. */
    case XKS_ESCAPE:     return HK_ESCAPE;
    case XKS_TAB:        return HK_TAB;
    case XKS_BACKSPACE:  return HK_BACKSPACE;
    case XKS_DELETE:     return HK_DELETE;
    case XKS_INSERT:     return HK_INSERT;
    /* HOME is the CoCo's CLEAR key. */
    case XKS_HOME:       return HK_HOME;
    case XKS_END:        return HK_END;
    case XKS_PAGE_UP:    return HK_PAGE_UP;
    case XKS_PAGE_DOWN:  return HK_PAGE_DOWN;
    case XKS_UP:         return HK_UP;
    case XKS_DOWN:       return HK_DOWN;
    case XKS_LEFT:       return HK_LEFT;
    case XKS_RIGHT:      return HK_RIGHT;
    case XKS_KP_DIV:     return HK_KP_DIVIDE;
    case XKS_KP_MULT:    return HK_KP_MULTIPLY;
    case XKS_KP_SUB:     return HK_KP_SUBTRACT;
    case XKS_KP_ADD:     return HK_KP_ADD;
    case XKS_KP_DEC:     return HK_KP_DECIMAL;
    case XKS_SHIFT_L:    return HK_SHIFT_L;
    case XKS_SHIFT_R:    return HK_SHIFT_R;
    /* CTRL and ALT are real keys on a CoCo 3 keyboard. */
    case XKS_CONTROL_L:  return HK_CONTROL_L;
    case XKS_CONTROL_R:  return HK_CONTROL_R;
    case XKS_ALT_L:      return HK_ALT_L;
    case XKS_ALT_R:      return HK_ALT_R;
    case XKS_CAPS_LOCK:  return HK_CAPS_LOCK;
    /* Meta/Super: a Mac keyboard reports its Command keys here. XRoar tracks
     * Super as a modifier, so pass it through rather than swallowing it. */
    case XKS_META_L:
    case XKS_SUPER_L:    return HK_SUPER_L;
    case XKS_META_R:
    case XKS_SUPER_R:    return HK_SUPER_R;
    default:             return 0;
    }
}

/* The unshifted symbol on the same physical key of a US/ANSI keyboard. See the
 * file comment for why this, and not the typed character, is what we want. */
static uint32_t unshift_us(uint32_t ch)
{
    switch (ch) {
    case '!': return '1';
    case '@': return '2';
    case '#': return '3';
    case '$': return '4';
    case '%': return '5';
    case '^': return '6';
    case '&': return '7';
    case '*': return '8';
    case '(': return '9';
    case ')': return '0';
    case '_': return '-';
    case '+': return '=';
    case '{': return '[';
    case '}': return ']';
    case '|': return '\\';
    case ':': return ';';
    case '"': return '\'';
    case '<': return ',';
    case '>': return '.';
    case '?': return '/';
    case '~': return '`';
    default:
        if (ch >= 'A' && ch <= 'Z') return ch + 32;
        return ch;
    }
}

/* Unshifted US-layout printable -> scancode. */
static unsigned printable_scancode(uint32_t ch)
{
    if (ch >= 'a' && ch <= 'z') return HK_a + (ch - 'a');
    /* HID puts 1..9 before 0. */
    if (ch >= '1' && ch <= '9') return HK_1 + (ch - '1');
    switch (ch) {
    case '0':  return HK_0;
    case ' ':  return HK_SPACE;
    case '-':  return HK_MINUS;
    case '=':  return HK_EQUAL;
    case '[':  return HK_BRACKETLEFT;
    case ']':  return HK_BRACKETRIGHT;
    case '\\': return HK_BACKSLASH;
    case ';':  return HK_SEMICOLON;
    case '\'': return HK_APOSTROPHE;
    case '`':  return HK_GRAVE;
    case ',':  return HK_COMMA;
    case '.':  return HK_PERIOD;
    case '/':  return HK_SLASH;
    default:   return 0;
    }
}

int coco_key_from_event(uint32_t keysym, uint32_t unicode, int ctrl_down,
                        int shift_down, unsigned *scancode_out)
{
    unsigned scancode;
    uint32_t ch;

    /* The modifier state is not needed to identify the key: a modifier arrives
     * as its own press/release event and XRoar tracks it from those, and a
     * shifted symbol already says which physical key was struck. Both are
     * accepted so every frontend can call this with the same arguments it
     * passes cocosession_key. */
    (void)ctrl_down;
    (void)shift_down;

    /* Reserved by the frontends: menu, fullscreen, debugger. Never forwarded,
     * so a machine cannot swallow the only way out of fullscreen. */
    if (keysym == XKS_F10 || keysym == XKS_F11 || keysym == XKS_F12)
        return 0;

    scancode = special_scancode(keysym);
    if (!scancode) {
        /* Printable path. Prefer the keysym, which for ASCII printables IS the
         * symbol; unicode is the fallback for toolkits that report a keysym we
         * do not recognise. Deliberately not the other way round: with Ctrl
         * held, some toolkits report the control character in unicode. */
        ch = (keysym >= 0x20 && keysym <= 0x7E) ? keysym : unicode;
        if (ch < 0x20 || ch > 0x7E) return 0;
        scancode = printable_scancode(unshift_us(ch));
        if (!scancode) return 0;
    }

    if (scancode_out) *scancode_out = scancode;
    return 1;
}

int coco_axis_from_stick(float v, float deadzone)
{
    float mag, scaled;
    int value;

    if (deadzone < 0.0f) deadzone = 0.0f;
    if (deadzone > 0.95f) deadzone = 0.95f;

    if (v < -1.0f) v = -1.0f;
    if (v > 1.0f) v = 1.0f;

    mag = v < 0.0f ? -v : v;
    if (mag <= deadzone) {
        scaled = 0.0f;
    } else {
        /* Rescale the live part of the travel back to a full 0..1 so the
         * joystick still reaches its stops despite the deadzone. */
        scaled = (mag - deadzone) / (1.0f - deadzone);
        if (scaled > 1.0f) scaled = 1.0f;
        if (v < 0.0f) scaled = -scaled;
    }

    /* The CoCo joystick is two 6-bit DACs, presented to XRoar as 0..65535 with
     * 32767 at centre. Truncation is what puts a centred stick on 32767 rather
     * than 32768, matching the Android app's Coco.axisValue(). */
    value = (int)((scaled + 1.0f) / 2.0f * 65535.0f);
    if (value < 0) value = 0;
    if (value > 65535) value = 65535;
    return value;
}

int coco_pad_for_port(const int *bindings, int npads, int port)
{
    int i, seen = 0;

    /* A machine with two ports has no third one to assign a pad to, however
     * many are plugged in. */
    if (!bindings || port < 0 || port >= COCOSESSION_JOYSTICK_PORTS)
        return -1;

    /* An explicit binding wins outright. */
    for (i = 0; i < npads; i++)
        if (bindings[i] == port)
            return i;

    /* Otherwise the unbound pads fill the remaining ports in order. Pads
     * bound to *another* port are skipped rather than counted, so binding
     * pad 0 to the left port hands the right port to pad 1 rather than
     * leaving it dead. */
    for (i = 0; i < npads; i++) {
        if (bindings[i] >= 0)
            continue;
        if (seen == port)
            return i;
        seen++;
    }
    return -1;
}
