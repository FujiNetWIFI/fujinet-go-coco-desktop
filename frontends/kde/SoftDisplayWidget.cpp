/*
 * SoftDisplayWidget -- see SoftDisplayWidget.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SoftDisplayWidget.h"

#include "DisplayGeometry.h"
#include "KeyForward.h"

#include <QEvent>
#include <QPainter>

SoftDisplayWidget::SoftDisplayWidget(cocosession *session, QWidget *parent)
    : QWidget(parent), m_session(session), m_feed(new FrameFeed(session, this)),
      m_frame((size_t)COCOSESSION_FB_MAX_WIDTH * COCOSESSION_FB_MAX_HEIGHT, 0)
{
    setMinimumSize(COCOSESSION_FB_MAX_WIDTH / 2, COCOSESSION_FB_MAX_HEIGHT / 2);
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(false);
    connect(m_feed, &FrameFeed::frameReady, this, &SoftDisplayWidget::onFrame);
}

SoftDisplayWidget::~SoftDisplayWidget()
{
    m_feed->setActive(false);
}

void SoftDisplayWidget::setAspectMode(int mode) { m_aspectMode = mode; update(); }
void SoftDisplayWidget::setSmooth(bool smooth) { m_smooth = smooth; update(); }

void SoftDisplayWidget::updateActive()
{
    m_feed->setActive(isVisible() && window() && !window()->isMinimized());
}

void SoftDisplayWidget::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    if (window())
        window()->installEventFilter(this);
    updateActive();
}

void SoftDisplayWidget::hideEvent(QHideEvent *e)
{
    QWidget::hideEvent(e);
    if (window())
        window()->removeEventFilter(this);
    updateActive();
}

bool SoftDisplayWidget::eventFilter(QObject *watched, QEvent *e)
{
    if (e->type() == QEvent::WindowStateChange || e->type() == QEvent::Show ||
        e->type() == QEvent::Hide)
        updateActive();
    return QWidget::eventFilter(watched, e);
}

bool SoftDisplayWidget::event(QEvent *e)
{
    if (cocoClaimShortcut(e))
        return true;
    return QWidget::event(e);
}

void SoftDisplayWidget::keyPressEvent(QKeyEvent *event) { cocoKeyPress(m_session, event); }
void SoftDisplayWidget::keyReleaseEvent(QKeyEvent *event) { cocoKeyRelease(m_session, event); }

void SoftDisplayWidget::onFrame()
{
    int w = 0, h = 0;
    if (!cocosession_copy_frame(m_session, m_frame.data(), &m_serial, &w, &h))
        return;
    if (w <= 0 || h <= 0)
        return;
    /* The pixels are 0x00RRGGBB; Format_RGB32 needs 0xFFRRGGBB, or KWin
     * composites the zero alpha as see-through, so force alpha on. */
    m_image = QImage(w, h, QImage::Format_RGB32);
    const size_t n = (size_t)w * h;
    quint32 *dst = reinterpret_cast<quint32 *>(m_image.bits());
    for (size_t i = 0; i < n; i++)
        dst[i] = m_frame[i] | 0xFF000000u;
    update();
}

void SoftDisplayWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (m_image.isNull())
        return;

    p.setRenderHint(QPainter::SmoothPixmapTransform, m_smooth);
    p.drawImage(displayRect(QSizeF(size()), m_image.width(), m_image.height(), m_aspectMode),
                m_image);
}
