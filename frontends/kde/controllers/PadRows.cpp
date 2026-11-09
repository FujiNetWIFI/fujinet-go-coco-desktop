/*
 * PadRows -- see PadRows.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "PadRows.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace {

QString driveText(int port)
{
    if (port == 0)
        return QStringLiteral("drives the right joystick");
    if (port == 1)
        return QStringLiteral("drives the left joystick");
    return QStringLiteral("drives nothing");
}

} // namespace

PadRows::PadRows(cocosession *session, int maxRows, QWidget *parent)
    : QWidget(parent), m_session(session), m_maxRows(maxRows)
{
    m_box = new QVBoxLayout(this);
    m_box->setContentsMargins(0, 0, 0, 0);
    m_box->setSpacing(2);
    m_box->setAlignment(Qt::AlignTop);
    if (maxRows > 0) {
        const int rowHeight = QComboBox().sizeHint().height() + 2;
        setFixedHeight(rowHeight * maxRows + m_box->spacing() * (maxRows - 1));
    }
    m_generation = cocosession_gamepad_generation(session);
    rebuild();
}

void PadRows::poll()
{
    const unsigned gen = cocosession_gamepad_generation(m_session);
    if (gen != m_generation) {
        m_generation = gen;
        rebuild();
    } else {
        refreshPorts();
    }
}

void PadRows::refreshPorts()
{
    for (size_t idx = 0; idx < m_drives.size(); ++idx) {
        const QString text = driveText(cocosession_gamepad_effective_port(m_session, int(idx)));
        if (m_drives[idx]->text() != text)
            m_drives[idx]->setText(text);
    }
}

void PadRows::rebuild()
{
    for (QWidget *row : m_rows)
        delete row;
    m_rows.clear();
    m_drives.clear();
    const int count = cocosession_gamepad_count(m_session);
    if (count == 0) {
        auto *none = new QLabel(QStringLiteral("No gamepads connected"));
        none->setStyleSheet(QStringLiteral("color: gray;"));
        m_box->addWidget(none);
        m_rows.push_back(none);
        return;
    }
    for (int idx = 0; idx < count && (m_maxRows <= 0 || idx < m_maxRows); ++idx) {
        char name[128] = "";
        cocosession_gamepad_name(m_session, idx, name, sizeof name);
        auto *row = new QWidget;
        auto *line = new QHBoxLayout(row);
        line->setContentsMargins(0, 0, 0, 0);
        auto *label = new ElidedLabel(false);
        label->setText(QString::fromUtf8(name));
        label->setToolTip(QString::fromUtf8(name));
        auto *drives = new QLabel;
        drives->setStyleSheet(QStringLiteral("color: gray;"));
        /* -1 = automatic (connection order), 0 = right joystick, 1 = left */
        auto *box = new QComboBox;
        box->setFocusPolicy(Qt::NoFocus);
        box->addItems({ QStringLiteral("Automatic"), QStringLiteral("Right joystick"),
                        QStringLiteral("Left joystick") });
        const int assign = cocosession_gamepad_assignment(m_session, idx);
        box->setCurrentIndex(assign < 0 || assign > 1 ? 0 : assign + 1);
        connect(box, &QComboBox::currentIndexChanged, this, [this, idx](int sel) {
            cocosession_gamepad_assign(m_session, idx, sel <= 0 ? -1 : sel - 1);
            refreshPorts();
        });
        line->addWidget(label, 1);
        line->addWidget(drives);
        line->addWidget(box);
        m_box->addWidget(row);
        m_rows.push_back(row);
        m_drives.push_back(drives);
    }
    if (m_maxRows > 0 && count > m_maxRows)
        m_drives.back()->setToolTip(
            QStringLiteral("%1 more gamepads are listed in Settings, Input").arg(count - m_maxRows));
    refreshPorts();
}
