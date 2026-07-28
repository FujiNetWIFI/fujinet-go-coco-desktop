/*
 * Settings dialog: window options (applied live) and the start-time options
 * that decide what the session runs (applied by restarting it when the dialog
 * closes). Everything persists through the shared settings store, so the
 * GNOME frontend sees the same configuration.
 *
 * Machine, TV input and artifact colour are deliberately NOT here: on this
 * target they switch the running machine in place, so they live in the
 * Machine and View menus where the current selection is visible at a glance.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SettingsDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>

QCheckBox *SettingsDialog::addStartOption(QFormLayout *form,
                                          const QString &label,
                                          const char *key, int def)
{
    auto *box = new QCheckBox(this);
    box->setChecked(cocosession_get_int(m_session, key, def));
    connect(box, &QCheckBox::toggled, this, [this, key](bool on) {
        if (cocosession_get_int(m_session, key, 1) == (on ? 1 : 0))
            return;
        cocosession_set_int(m_session, key, on ? 1 : 0);
        m_sessionDirty = true;
    });
    form->addRow(label, box);
    return box;
}

SettingsDialog::SettingsDialog(cocosession *session, QWidget *parent)
    : QDialog(parent), m_session(session)
{
    setWindowTitle(QStringLiteral("Settings"));

    auto *outer = new QVBoxLayout(this);

    auto *displayBox = new QGroupBox(QStringLiteral("Window"), this);
    auto *displayForm = new QFormLayout(displayBox);

    auto *aspect = new QComboBox(displayBox);
    aspect->addItems({QStringLiteral("TV (4:3)"),
                      QStringLiteral("Square pixels"),
                      QStringLiteral("Integer scale")});
    aspect->setCurrentIndex(cocosession_get_int(m_session, "aspect_mode", 0));
    connect(aspect, &QComboBox::currentIndexChanged, this, [this](int idx) {
        cocosession_set_int(m_session, "aspect_mode", idx);
        emit displayChanged();
    });
    displayForm->addRow(QStringLiteral("Aspect ratio"), aspect);

    auto *smooth = new QCheckBox(displayBox);
    smooth->setChecked(cocosession_get_int(m_session, "smooth_scaling", 0));
    connect(smooth, &QCheckBox::toggled, this, [this](bool on) {
        cocosession_set_int(m_session, "smooth_scaling", on ? 1 : 0);
        emit displayChanged();
    });
    displayForm->addRow(QStringLiteral("Smooth scaling"), smooth);
    outer->addWidget(displayBox);

    auto *runtimeBox = new QGroupBox(QStringLiteral("FujiNet"), this);
    auto *runtimeForm = new QFormLayout(runtimeBox);
    addStartOption(runtimeForm, QStringLiteral("FujiNet"), "enable_fujinet", 1);
    addStartOption(runtimeForm, QStringLiteral("Sound"), "enable_audio", 1);
    addStartOption(runtimeForm, QStringLiteral("Gamepads"), "enable_gamepad", 1);
    auto *note = new QLabel(
        QStringLiteral("FujiNet listens on the Becker port and the emulator "
                       "connects out to it. These are applied by restarting "
                       "the session when this dialog closes."),
        runtimeBox);
    note->setWordWrap(true);
    runtimeForm->addRow(note);
    outer->addWidget(runtimeBox);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    outer->addWidget(buttons);
}
