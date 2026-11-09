/*
 * PadRows: one row per connected gamepad -- its name, the joystick it drives
 * and the one it is set to (Automatic / Right / Left) -- rebuilt as pads come
 * and go. Shared by the Controllers window and the Input preferences.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QPainter>
#include <QString>
#include <QWidget>

#include <vector>

#include "cocosession.h"

class QLabel;

/* A label that never widens its window: one line elides, more wrap into a
 * fixed height. */
class ElidedLabel : public QWidget {
public:
    explicit ElidedLabel(bool dim, int lines = 1) : m_dim(dim), m_lines(lines)
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }
    void setText(const QString &text)
    {
        if (text == m_text)
            return;
        m_text = text;
        update();
    }
    QSize sizeHint() const override { return QSize(0, fontMetrics().lineSpacing() * m_lines); }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter paint(this);
        paint.setPen(m_dim ? QColor(Qt::gray) : palette().color(QPalette::WindowText));
        if (m_lines > 1)
            paint.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, m_text);
        else
            paint.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter,
                           fontMetrics().elidedText(m_text, Qt::ElideRight, width()));
    }

private:
    QString m_text;
    bool m_dim;
    int m_lines;
};

class PadRows : public QWidget {
public:
    /* maxRows > 0 reserves room for that many rows (the window must not change
     * size when a pad is plugged in); 0 shows every pad and grows. */
    PadRows(cocosession *session, int maxRows, QWidget *parent = nullptr);

    /* Call at 4 Hz: rebuilds on a generation change, else refreshes the
     * "drives ..." text. */
    void poll();
    void rebuild();

private:
    void refreshPorts();

    cocosession *m_session;
    int m_maxRows;
    class QVBoxLayout *m_box = nullptr;
    std::vector<QWidget *> m_rows;
    std::vector<QLabel *> m_drives;     /* each pad's "drives the right joystick" */
    unsigned m_generation = ~0u;
};
