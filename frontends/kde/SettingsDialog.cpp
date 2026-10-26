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
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
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

void SettingsDialog::refreshRoms()
{
    for (size_t r = 0; r < m_romRows.size(); r++)
        m_romRows[r]->setText(cocosession_rom_item_available(m_session, (int)r)
                                  ? QStringLiteral("Imported") : QStringLiteral("Not imported"));
}

void SettingsDialog::importRoms(bool folder)
{
    const QString path = folder
        ? QFileDialog::getExistingDirectory(this, QStringLiteral("Import BIOS from a Folder"))
        : QFileDialog::getOpenFileName(this, QStringLiteral("Import BIOS"), QString(),
                                       QStringLiteral("BIOS images and ROM sets (*.zip *.ZIP *.rom *.ROM "
                                                      "*.bin *.BIN);;All files (*)"));
    if (path.isEmpty())
        return;
    char msg[1024];
    const int rc = cocosession_rom_import(m_session, path.toUtf8().constData(), msg, sizeof msg);
    QMessageBox::information(this, rc == 0 ? QStringLiteral("BIOS Imported") : QStringLiteral("Nothing Imported"),
                             QString::fromUtf8(msg));
    emit romsImported();
    refreshRoms();
}

SettingsDialog::SettingsDialog(cocosession *session, QWidget *parent)
    : QDialog(parent), m_session(session)
{
    setWindowTitle(QStringLiteral("Settings"));

    auto *outer = new QVBoxLayout(this);
    auto *tabs = new QTabWidget(this);
    auto *general = new QWidget;
    auto *gl = new QVBoxLayout(general);

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
    gl->addWidget(displayBox);

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
    gl->addWidget(runtimeBox);
    gl->addStretch(1);
    tabs->addTab(general, QStringLiteral("General"));

    /* ROMs: the three system ROMs, grouped, each imported or not. */
    auto *roms = new QWidget;
    auto *rv = new QVBoxLayout(roms);
    auto *rnote = new QLabel(
        QStringLiteral("The CoCo's system ROMs are not part of the app. Import them from files, a zip or a "
                       "folder (MAME's coco and coco3 sets work); they are recognised by their checksum. "
                       "Kept in %1.").arg(QString::fromUtf8(cocosession_roms_path(m_session))),
        roms);
    rnote->setWordWrap(true);
    rnote->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
    auto *rbuttons = new QHBoxLayout;
    auto *files = new QPushButton(QStringLiteral("Import…"), roms);
    auto *dir = new QPushButton(QStringLiteral("Import a Folder…"), roms);
    connect(files, &QPushButton::clicked, this, [this] { importRoms(false); });
    connect(dir, &QPushButton::clicked, this, [this] { importRoms(true); });
    rbuttons->addWidget(files);
    rbuttons->addWidget(dir);
    rbuttons->addStretch(1);
    rv->addWidget(rnote);
    rv->addLayout(rbuttons);
    QGroupBox *group = nullptr;
    QFormLayout *rform = nullptr;
    QString last;
    for (int r = 0; r < cocosession_rom_item_count(); r++) {
        const coco_rom_item *item = cocosession_rom_item(r);
        const QString g = QString::fromUtf8(item->group);
        if (!group || g != last) {
            group = new QGroupBox(g, roms);
            rform = new QFormLayout(group);
            rv->addWidget(group);
            last = g;
        }
        auto *state = new QLabel(roms);
        state->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
        rform->addRow(new QLabel(QString::fromUtf8(item->name)), state);
        m_romRows.push_back(state);
    }
    rv->addStretch(1);
    refreshRoms();
    tabs->addTab(roms, QStringLiteral("ROMs"));
    if (qgetenv("COCO_OPEN_PREFS") == "ROMs")
        tabs->setCurrentIndex(1);
    outer->addWidget(tabs);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    outer->addWidget(buttons);
}
