/*
 * Key handling shared by the GPU and the software display.
 *
 * Key events are translated to the X11 keysyms coco_key_from_event expects,
 * so this frontend and the GNOME one share one mapping table. Both press and
 * release are forwarded: XRoar's hkbd layer tracks held keys, so a missed
 * release leaves that key down inside the machine.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KeyForward.h"

#include <QKeyEvent>

namespace {

/* Qt key code -> X11/xkb keysym, for the keys coco_key_from_event resolves
 * by keysym rather than by character. */
quint32 keysymForKey(const QKeyEvent *event)
{
    /* F1..F9. The CoCo 3 has F1 and F2; F10, F11 and F12 are the frontend's
     * (menu, fullscreen, debugger) and never reach here -- event() lets those
     * three through as shortcuts instead of claiming them. */
    if (event->key() >= Qt::Key_F1 && event->key() <= Qt::Key_F9)
        return 0xFFBE + (quint32)(event->key() - Qt::Key_F1);

    switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:     return 0xFF0D;
    /* ESC is the CoCo's BREAK key. */
    case Qt::Key_Escape:    return 0xFF1B;
    case Qt::Key_Tab:       return 0xFF09;
    case Qt::Key_Backspace: return 0xFF08;
    case Qt::Key_Delete:    return 0xFFFF;
    case Qt::Key_Insert:    return 0xFF63;
    /* HOME is the CoCo's CLEAR key. */
    case Qt::Key_Home:      return 0xFF50;
    case Qt::Key_End:       return 0xFF57;
    case Qt::Key_PageUp:    return 0xFF55;
    case Qt::Key_PageDown:  return 0xFF56;
    case Qt::Key_Left:      return 0xFF51;
    case Qt::Key_Up:        return 0xFF52;
    case Qt::Key_Right:     return 0xFF53;
    case Qt::Key_Down:      return 0xFF54;
    /* Modifiers. SHIFT, CTRL and ALT are all real keys on a CoCo 3 keyboard,
     * so they are forwarded rather than swallowed. Qt reports left and right
     * as the same key code for Shift and Control, so both land on the left
     * one -- XRoar treats them as the same modifier anyway. */
    case Qt::Key_Shift:     return 0xFFE1; /* Shift_L */
    case Qt::Key_Control:   return 0xFFE3; /* Control_L */
    case Qt::Key_Alt:       return 0xFFE9; /* Alt_L */
    case Qt::Key_AltGr:     return 0xFFEA; /* Alt_R */
    case Qt::Key_Meta:      return 0xFFE7; /* Meta_L */
    case Qt::Key_CapsLock:  return 0xFFE5;
    default:                return 0;
    }
}

} // namespace

/* The keysym and unicode cocosession_key wants for an event; false for a key
 * with neither. */
static bool resolveKey(const QKeyEvent *event, quint32 *keysym_out, quint32 *unicode_out)
{
    const bool ctrl = event->modifiers().testFlag(Qt::ControlModifier);
    quint32 unicode = 0;
    if (!event->text().isEmpty())
        unicode = event->text().at(0).unicode();

    quint32 keysym = keysymForKey(event);
    if (keysym == 0) {
        /* Printable: Qt gives no text for a Ctrl combo, so fall back to the
         * unshifted letter, which is what the keysym contract wants. */
        if (ctrl && event->key() >= Qt::Key_A && event->key() <= Qt::Key_Z)
            keysym = 'a' + (quint32)(event->key() - Qt::Key_A);
        else
            keysym = unicode;
    }
    *keysym_out = keysym;
    *unicode_out = unicode;
    return keysym != 0;
}

static void forwardKey(cocosession *session, const QKeyEvent *event, int down)
{
    const bool ctrl = event->modifiers().testFlag(Qt::ControlModifier);
    const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
    quint32 keysym = 0, unicode = 0;
    if (!resolveKey(event, &keysym, &unicode))
        return;

    cocosession_key(session, down, keysym, unicode, ctrl ? 1 : 0,
                      shift ? 1 : 0);
}

bool cocoIsAppKey(const QKeyEvent *event)
{
    const int key = event->key();
    if (key == Qt::Key_F10 || key == Qt::Key_F11 || key == Qt::Key_F12)
        return true;
    /* Ctrl+Shift+letter is the app's (Controllers, Help), never the CoCo's. */
    const Qt::KeyboardModifiers mods = event->modifiers();
    return mods.testFlag(Qt::ControlModifier) && mods.testFlag(Qt::ShiftModifier) &&
           key >= Qt::Key_A && key <= Qt::Key_Z;
}

uint32_t cocoBindableKey(const QKeyEvent *event)
{
    quint32 keysym = 0, unicode = 0;
    if (!resolveKey(event, &keysym, &unicode))
        return 0;
    return cocosession_bindable_key(keysym, unicode);
}

bool cocoClaimShortcut(QEvent *event)
{
    /* Claim every key except the reserved chords (F10 menu, F11 fullscreen,
     * F12 debugger, Ctrl+Shift+letter: Controllers, Help) while focused, so
     * menu accelerators cannot shadow what the CoCo should receive. Alt in
     * particular must NOT reach the menu bar: it is a real key on a CoCo 3
     * keyboard. */
    if (event->type() == QEvent::ShortcutOverride) {
        QKeyEvent *ke = static_cast<QKeyEvent *>(event);
        if (!cocoIsAppKey(ke)) {
            event->accept();
            return true;
        }
    }
    return false;
}

void cocoKeyPress(cocosession *session, QKeyEvent *event)
{
    if (event->isAutoRepeat()) {
        /* XRoar's hkbd holds a key down until it is released, and the CoCo's
         * own BASIC does the repeating. A host repeat storm would just be
         * redundant press events on an already-held key. */
        event->accept();
        return;
    }
    forwardKey(session, event, 1);
    event->accept();
}

void cocoKeyRelease(cocosession *session, QKeyEvent *event)
{
    if (event->isAutoRepeat()) {
        event->accept();
        return;
    }
    forwardKey(session, event, 0);
    event->accept();
}
