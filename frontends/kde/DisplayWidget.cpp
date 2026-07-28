/*
 * DisplayWidget: paints the latest emulator frame with QPainter over a
 * QOpenGLWidget, letterboxed to the chosen aspect. The frameSwapped signal
 * (vsync-aligned with the default swap interval of 1) both feeds the
 * session's vsync phase-lock and schedules the next repaint, forming a
 * self-sustaining vsync-paced present loop.
 *
 * Key events are translated to the X11 keysyms coco_key_from_event expects,
 * so this frontend and the GNOME one share one mapping table. Both press and
 * release are forwarded: XRoar's hkbd layer tracks held keys, so a missed
 * release leaves that key down inside the machine.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DisplayWidget.h"

#include <QKeyEvent>
#include <QPainter>

#include <ctime>

namespace {

qint64 monotonic_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (qint64)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

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

DisplayWidget::DisplayWidget(cocosession *session, QWidget *parent)
    : QOpenGLWidget(parent),
      m_session(session),
      m_frame((size_t)COCOSESSION_FB_MAX_WIDTH *
              COCOSESSION_FB_MAX_HEIGHT, 0)
{
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(COCOSESSION_FB_MAX_WIDTH / 2,
                   COCOSESSION_FB_MAX_HEIGHT / 2);
    connect(this, &QOpenGLWidget::frameSwapped, this,
            &DisplayWidget::onFrameSwapped, Qt::QueuedConnection);
}

void DisplayWidget::setAspectMode(int mode)
{
    m_aspectMode = mode;
    update();
}

void DisplayWidget::setSmooth(bool smooth)
{
    m_smooth = smooth;
    update();
}

void DisplayWidget::onFrameSwapped()
{
    cocosession_notify_vsync(m_session, monotonic_ns());
    update();
}

void DisplayWidget::showEvent(QShowEvent *event)
{
    QOpenGLWidget::showEvent(event);
    update(); /* kick the frameSwapped/update loop */
}

QRectF DisplayWidget::destRect() const
{
    const qreal w = width(), h = height();
    const int fbW = m_fbW > 0 ? m_fbW : COCOSESSION_FB_MAX_WIDTH;
    const int fbH = m_fbH > 0 ? m_fbH : COCOSESSION_FB_MAX_HEIGHT;
    qreal dw, dh;

    if (m_aspectMode == AspectInteger) {
        int scale = (int)qMin(w / fbW, h / fbH);
        if (scale < 1) scale = 1;
        dw = scale * fbW;
        dh = scale * fbH;
    } else {
        const qreal aspect = m_aspectMode == AspectTv43
                                 ? 4.0 / 3.0
                                 : (qreal)fbW / fbH;
        if (w / h > aspect) {
            dh = h;
            dw = h * aspect;
        } else {
            dw = w;
            dh = w / aspect;
        }
    }
    return QRectF((w - dw) / 2.0, (h - dh) / 2.0, dw, dh);
}

void DisplayWidget::paintGL()
{
    uint64_t serial = m_serial;
    int w = 0, h = 0;

    if (cocosession_copy_frame(m_session, m_frame.data(), &serial, &w, &h)) {
        m_serial = serial;
        m_fbW = w;
        m_fbH = h;
    }

    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (m_fbW > 0 && m_fbH > 0) {
        p.setRenderHint(QPainter::SmoothPixmapTransform, m_smooth);
        /* A view over the packed buffer: rows are m_fbW pixels apart. */
        const QImage frame((const uchar *)m_frame.data(), m_fbW, m_fbH,
                           m_fbW * 4, QImage::Format_RGB32);
        p.drawImage(destRect(), frame);
    }
}

bool DisplayWidget::event(QEvent *event)
{
    /* Claim every key except the reserved chords (F10 menu, F11 fullscreen,
     * F12 debugger) while focused, so menu accelerators cannot shadow what
     * the CoCo should receive. Alt in particular must NOT reach the menu bar:
     * it is a real key on a CoCo 3 keyboard. */
    if (event->type() == QEvent::ShortcutOverride) {
        QKeyEvent *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() != Qt::Key_F10 && ke->key() != Qt::Key_F11 &&
            ke->key() != Qt::Key_F12) {
            event->accept();
            return true;
        }
    }
    return QOpenGLWidget::event(event);
}

void DisplayWidget::forwardKey(const QKeyEvent *event, int down)
{
    const bool ctrl = event->modifiers().testFlag(Qt::ControlModifier);
    const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
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
    if (keysym == 0)
        return;

    cocosession_key(m_session, down, keysym, unicode, ctrl ? 1 : 0,
                      shift ? 1 : 0);
}

void DisplayWidget::keyPressEvent(QKeyEvent *event)
{
    if (event->isAutoRepeat()) {
        /* XRoar's hkbd holds a key down until it is released, and the CoCo's
         * own BASIC does the repeating. A host repeat storm would just be
         * redundant press events on an already-held key. */
        event->accept();
        return;
    }
    forwardKey(event, 1);
    event->accept();
}

void DisplayWidget::keyReleaseEvent(QKeyEvent *event)
{
    if (event->isAutoRepeat()) {
        event->accept();
        return;
    }
    forwardKey(event, 0);
    event->accept();
}
