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
#include "controllers/PadRows.h"
#include "help/HelpWindow.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QShortcut>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>
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

void SettingsDialog::done(int result)
{
    const QByteArray addr = m_externalAddr->text().trimmed().toUtf8();
    char host[64];
    int port;

    if (m_external->isChecked() &&
        cocosession_parse_hostport(addr.constData(), host, sizeof host,
                                   &port) != 0) {
        QMessageBox::warning(
            this, QStringLiteral("External FujiNet"),
            QStringLiteral("\"%1\" is not an address:port. Example: "
                           "127.0.0.1:65504")
                .arg(m_externalAddr->text()));
        m_externalAddr->setFocus();
        m_wantControllers = false;
        return;
    }
    if (QString::fromUtf8(cocosession_get_str(m_session,
                                              "fujinet_external_addr", "")) !=
        QString::fromUtf8(addr)) {
        cocosession_set_str(m_session, "fujinet_external_addr",
                            addr.constData());
        m_sessionDirty = true;
    }
    QDialog::done(result);
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
    m_external = addStartOption(runtimeForm,
                                QStringLiteral("Use external FujiNet"),
                                "fujinet_external", 0);
    m_externalAddr = new QLineEdit(
        QString::fromUtf8(cocosession_get_str(m_session,
                                              "fujinet_external_addr", "")),
        runtimeBox);
    m_externalAddr->setPlaceholderText(QStringLiteral("127.0.0.1:65504"));
    m_externalAddr->setEnabled(m_external->isChecked());
    connect(m_external, &QCheckBox::toggled, m_externalAddr,
            &QWidget::setEnabled);
    runtimeForm->addRow(QStringLiteral("Address:port"), m_externalAddr);
    addStartOption(runtimeForm, QStringLiteral("Sound"), "enable_audio", 1);
    auto *note = new QLabel(
        QStringLiteral("FujiNet listens on the Becker port and the emulator "
                       "connects out to it; with an external FujiNet it "
                       "connects to the address above instead. These are "
                       "applied by restarting the session when this dialog "
                       "closes."),
        runtimeBox);
    note->setWordWrap(true);
    runtimeForm->addRow(note);
    gl->addWidget(runtimeBox);
    gl->addStretch(1);
    tabs->addTab(general, QStringLiteral("General"));

    /* Input: the gamepads and which joystick each drives, the controls' reset.
     * Keys and buttons themselves are mapped in the Controllers window. */
    auto *input = new QWidget;
    auto *inputLayout = new QVBoxLayout(input);
    auto *padBox = new QGroupBox(QStringLiteral("Gamepads"), input);
    auto *padForm = new QFormLayout(padBox);
    addStartOption(padForm, QStringLiteral("Gamepads"), "enable_gamepad", 1);
    auto *analog = new QCheckBox(padBox);
    analog->setChecked(cocosession_get_int(m_session, "analog_joystick", 1) != 0);
    analog->setToolTip(QStringLiteral("The pad's stick moves the joystick in proportion; off, it works like a "
                                      "switch with a full push either way"));
    connect(analog, &QCheckBox::toggled, this, [this](bool enable) { cocosession_set_analog(m_session, enable ? 1 : 0); });
    padForm->addRow(QStringLiteral("Analog joystick"), analog);
    auto *padNote = new QLabel(
        QStringLiteral("Turning gamepads on or off is applied by restarting the session when this dialog closes."),
        padBox);
    padNote->setWordWrap(true);
    padNote->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
    padForm->addRow(padNote);
    inputLayout->addWidget(padBox);

    auto *pairBox = new QGroupBox(QStringLiteral("Pads"), input);
    auto *pairLayout = new QVBoxLayout(pairBox);
    auto *padRows = new PadRows(m_session, 0, pairBox);
    pairLayout->addWidget(padRows);
    inputLayout->addWidget(pairBox);
    /* pads come and go while the dialog is open */
    auto *padTimer = new QTimer(this);
    connect(padTimer, &QTimer::timeout, padRows, [padRows] { padRows->poll(); });
    padTimer->start(250);

    auto *controlBox = new QGroupBox(QStringLiteral("Controls"), input);
    auto *controlLayout = new QVBoxLayout(controlBox);
    auto *controlNote = new QLabel(
        QStringLiteral("Keys and buttons are mapped in the Controllers window (Ctrl+Shift+K). Every key "
                       "belongs to the CoCo until you bind it."),
        controlBox);
    controlNote->setWordWrap(true);
    controlNote->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
    auto *controlButtons = new QHBoxLayout;
    auto *resetControls = new QPushButton(QStringLiteral("Reset Controls"), controlBox);
    resetControls->setToolTip(QStringLiteral("Put every key and gamepad button back to its default"));
    auto *openControllers = new QPushButton(QStringLiteral("Open Controllers"), controlBox);
    connect(resetControls, &QPushButton::clicked, this, [this, resetControls] {
        cocosession_bindings_reset(m_session);
        resetControls->setText(QStringLiteral("Controls Reset"));
    });
    connect(openControllers, &QPushButton::clicked, this, [this] {
        m_wantControllers = true;
        accept();
    });
    controlButtons->addWidget(resetControls);
    controlButtons->addWidget(openControllers);
    controlButtons->addStretch(1);
    controlLayout->addWidget(controlNote);
    controlLayout->addLayout(controlButtons);
    inputLayout->addWidget(controlBox);
    inputLayout->addStretch(1);
    tabs->addTab(input, QStringLiteral("Input"));

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
        tabs->setCurrentIndex(2);
    else if (qgetenv("COCO_OPEN_PREFS") == "Input")
        tabs->setCurrentIndex(1);
    outer->addWidget(tabs);

    /* Help, F1 and Ctrl+Shift+H (the app's shortcuts don't reach a modal dialog) */
    auto pageContext = [](int page) {
        return page == 2 ? QStringLiteral("prefs/ROMs")
             : page == 1 ? QStringLiteral("prefs/Input") : QStringLiteral("prefs/Machine");
    };
    setProperty("cocoHelpContext", pageContext(tabs->currentIndex()));
    connect(tabs, &QTabWidget::currentChanged, this,
            [this, pageContext](int page) { setProperty("cocoHelpContext", pageContext(page)); });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close | QDialogButtonBox::Help, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::helpRequested, this, [this] { cocoHelpFor(this); });
    for (const char *key : { "F1", "Ctrl+Shift+H" })
        new QShortcut(QKeySequence(QString::fromLatin1(key)), this, [this] { cocoHelpFor(this); });
    outer->addWidget(buttons);
}
