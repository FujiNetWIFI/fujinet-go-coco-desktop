/*
 * MainWindow: menu bar over the emulator display. Plain Qt6 Widgets, no KDE
 * Frameworks -- Breeze arrives through the platform theme, and staying
 * framework-free keeps this frontend reusable elsewhere.
 *
 * Machine, Display and Artifact Colour are checkable radio groups rather than
 * settings-dialog rows, because on this target they all switch the running
 * machine in place: the menu is the readout of what is selected right now.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QStatusBar>
#include <QVBoxLayout>

#include "DisplayWidget.h"
#include "debugger/DebuggerWindow.h"
#include "FujiNetWindows.h"
#include "SettingsDialog.h"

#ifndef COCO_VERSION_STRING
#define COCO_VERSION_STRING "0.0.0"
#endif

MainWindow::MainWindow(cocosession *session, QWidget *parent)
    : QMainWindow(parent), m_session(session)
{
    setWindowTitle(QStringLiteral("FujiNet Go CoCo"));
    resize(1120, 840);

    m_display = new DisplayWidget(session, this);
    m_noBios = buildNoBiosPage();
    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_display);
    m_stack->addWidget(m_noBios);
    setCentralWidget(m_stack);
    statusBar();

    buildMenus();
    applyDisplaySettings();
    showPage();

    /* Developer affordance: open the debugger alongside the main window,
     * which is what you want when the app dies before you reach a menu. */
    if (qEnvironmentVariableIsSet("COCO_OPEN_DEBUGGER"))
        DebuggerWindow::show(this, m_session);
}

void MainWindow::status(const QString &message)
{
    statusBar()->showMessage(message, 6000);
}

void MainWindow::applyDisplaySettings()
{
    m_display->setAspectMode(
        cocosession_get_int(m_session, "aspect_mode",
                            DisplayWidget::AspectTv43));
    m_display->setSmooth(cocosession_get_int(m_session, "smooth_scaling", 0));
}

void MainWindow::restartSession()
{
    cocosession_start_opts opts;
    cocosession_stop(m_session);
    cocosession_default_opts(m_session, &opts);
    if (cocosession_start(m_session, &opts) != 0)
        status(QString::fromUtf8(cocosession_last_error(m_session)));
    showPage();
}

void MainWindow::importDiskImage()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import Disk or Tape Image"), QString(),
        QStringLiteral("CoCo disk and tape images (*.dsk *.os9 *.vdk *.cas "
                       "*.wav);;Cartridges (*.rom *.ccc);;All files (*)"));
    if (path.isEmpty())
        return;

    char dest[1024];
    if (cocosession_import_media(m_session, path.toUtf8().constData(), dest,
                                 sizeof(dest)) != 0) {
        status(QString::fromUtf8(cocosession_last_error(m_session)));
        return;
    }
    status(QStringLiteral("Copied to the FujiNet SD folder. Mount it from "
                          "FujiNet Configuration."));
}

/* "BIOS images and ROM sets (*.bin *.BIN ...);;All files (*)": either case. */
static QString romFilter()
{
    QStringList pats;
    for (const char *e : { "zip", "rom", "bin" })
        pats << QStringLiteral("*.%1").arg(QLatin1String(e))
             << QStringLiteral("*.%1").arg(QString::fromLatin1(e).toUpper());
    return QStringLiteral("BIOS images and ROM sets (%1);;All files (*)").arg(pats.join(QLatin1Char(' ')));
}

/* Each chosen file (or the folder) through the import, then one message
 * saying what came of it all, and the machine starts if it now can. The
 * core rebuilds a running machine itself. */
void MainWindow::importBios(bool folder)
{
    QStringList paths;
    if (folder) {
        const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Import BIOS from a Folder"));
        if (!dir.isEmpty())
            paths << dir;
    } else {
        paths = QFileDialog::getOpenFileNames(this, QStringLiteral("Import BIOS"), QString(), romFilter());
    }
    QStringList log;
    int imported = 0;
    for (const QString &p : paths) {
        char msg[1024];
        if (cocosession_rom_import(m_session, p.toUtf8().constData(), msg, sizeof msg) == 0)
            imported++;
        log << QString::fromUtf8(msg);
    }
    if (!log.isEmpty())
        QMessageBox::information(this, imported ? QStringLiteral("BIOS Imported") : QStringLiteral("Nothing Imported"),
                                 log.join(QLatin1Char('\n')));
    start();
}

/* In place of the picture until the selected machine has its ROMs. */
QWidget *MainWindow::buildNoBiosPage()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    v->addStretch(1);
    auto *title = new QLabel(QStringLiteral("No CoCo BIOS"));
    QFont tf = title->font();
    tf.setPointSizeF((tf.pointSizeF() > 0 ? tf.pointSizeF() : 10) * 1.8);
    tf.setBold(true);
    title->setFont(tf);
    title->setAlignment(Qt::AlignCenter);
    auto *body = new QLabel(QStringLiteral(
        "The CoCo needs its system ROMs (Color BASIC, Extended Color BASIC, and Super Extended "
        "Color BASIC for the CoCo 3), which are not part of the app. Import the ones you have "
        "\u2014 files, a zip such as MAME's coco or coco3 set, or a folder \u2014 and the CoCo starts."));
    body->setWordWrap(true);
    body->setAlignment(Qt::AlignCenter);
    body->setFixedWidth(520);
    body->setMinimumHeight(body->heightForWidth(520));
    auto *row = new QHBoxLayout;
    auto *files = new QPushButton(QStringLiteral("Import BIOS\u2026"));
    auto *folder = new QPushButton(QStringLiteral("Import a Folder\u2026"));
    files->setDefault(true);
    connect(files, &QPushButton::clicked, this, [this] { importBios(false); });
    connect(folder, &QPushButton::clicked, this, [this] { importBios(true); });
    row->addStretch(1);
    row->addWidget(files);
    row->addWidget(folder);
    row->addStretch(1);
    v->addWidget(title);
    v->addSpacing(8);
    v->addWidget(body, 0, Qt::AlignHCenter);
    v->addSpacing(16);
    v->addLayout(row);
    v->addStretch(1);
    return page;
}

/* The picture while the machine runs or can boot, else the No-BIOS page. */
void MainWindow::showPage()
{
    const bool bios = cocosession_is_running(m_session) ||
                      cocosession_has_system_roms(m_session, cocosession_machine(m_session));
    m_stack->setCurrentWidget(bios ? static_cast<QWidget *>(m_display) : m_noBios);
    if (bios)
        m_display->setFocus();
}

/* Starts the machine once its ROMs are there (after an import). */
void MainWindow::start()
{
    if (!cocosession_is_running(m_session) &&
        cocosession_has_system_roms(m_session, cocosession_machine(m_session)))
        restartSession();
    showPage();
}

/* The Display submenu is the one that depends on which machine is running, so
 * it is rebuilt rather than merely re-checked: a CoCo 2 has no RGB input. */
void MainWindow::rebuildTvInputMenu()
{
    m_tvInputMenu->clear();
    delete m_tvInputGroup;
    m_tvInputGroup = new QActionGroup(this);
    m_tvInputGroup->setExclusive(true);

    const int machine = cocosession_machine(m_session);
    const int current = cocosession_tv_input(m_session);
    for (int i = 0; const char *name = cocosession_tv_input_name(i); i++) {
        if (!cocosession_tv_input_supported(machine, i))
            continue;
        QAction *act = m_tvInputMenu->addAction(QString::fromUtf8(name));
        act->setCheckable(true);
        act->setChecked(i == current);
        m_tvInputGroup->addAction(act);
        connect(act, &QAction::triggered, this, [this, i] {
            cocosession_set_tv_input(m_session, i);
            m_display->setFocus();
        });
    }
}

void MainWindow::buildMenus()
{
    QMenu *machineMenu = menuBar()->addMenu(QStringLiteral("&Machine"));

    /* Switching the machine hard-resets it with the matching HDB-DOS Becker
     * ROM, in place -- FujiNet stays up and becker reconnects. */
    QMenu *modelMenu = machineMenu->addMenu(QStringLiteral("&Model"));
    m_machineGroup = new QActionGroup(this);
    m_machineGroup->setExclusive(true);
    for (int i = 0; const char *name = cocosession_machine_name(i); i++) {
        QAction *act = modelMenu->addAction(QString::fromUtf8(name));
        act->setCheckable(true);
        act->setChecked(i == cocosession_machine(m_session));
        m_machineGroup->addAction(act);
        connect(act, &QAction::triggered, this, [this, i] {
            cocosession_set_machine(m_session, i);
            /* The TV input may have been carried to something the new machine
             * can produce, so rebuild rather than assume. */
            rebuildTvInputMenu();
            status(QStringLiteral("%1 (reset)")
                       .arg(QString::fromUtf8(cocosession_machine_name(
                           cocosession_machine(m_session)))));
            start();
        });
    }

    machineMenu->addAction(QStringLiteral("&Reset"), this, [this] {
        cocosession_reset(m_session);
        status(QStringLiteral("Reset"));
    });
    machineMenu->addSeparator();
    machineMenu->addAction(QStringLiteral("Import &BIOS…"), this,
                           [this] { importBios(false); });
    machineMenu->addAction(QStringLiteral("Import BIOS from a &Folder…"), this,
                           [this] { importBios(true); });
    machineMenu->addSeparator();
    machineMenu->addAction(QStringLiteral("&Quit"), QKeySequence::Quit, qApp,
                           &QApplication::quit);

    QMenu *media = menuBar()->addMenu(QStringLiteral("Me&dia"));
    media->addAction(QStringLiteral("Import &Disk or Tape Image…"), this,
                     &MainWindow::importDiskImage);

    QMenu *fujinet = menuBar()->addMenu(QStringLiteral("&FujiNet"));
    fujinet->addAction(QStringLiteral("&Configuration…"), this,
                       [this] { fujinet_config_show(this, m_session); });
    fujinet->addAction(QStringLiteral("Console &Log…"), this,
                       [this] { fujinet_log_show(this, m_session); });

    QMenu *view = menuBar()->addMenu(QStringLiteral("&View"));
    m_tvInputMenu = view->addMenu(QStringLiteral("&Display"));
    rebuildTvInputMenu();

    /* Artifact colour: how faithfully the cross-colour in the composite TV
     * inputs is simulated. Every machine has all of them. */
    QMenu *ccrMenu = view->addMenu(QStringLiteral("&Artifact Colour"));
    QActionGroup *ccrGroup = new QActionGroup(this);
    ccrGroup->setExclusive(true);
    for (int i = 0; const char *name = cocosession_ccr_name(i); i++) {
        QAction *act = ccrMenu->addAction(QString::fromUtf8(name));
        act->setCheckable(true);
        act->setChecked(i == cocosession_ccr(m_session));
        ccrGroup->addAction(act);
        connect(act, &QAction::triggered, this, [this, i] {
            cocosession_set_ccr(m_session, i);
            m_display->setFocus();
        });
    }

    /* CPU: both machine types support either core (a real CoCo's 6809 was
     * socketed), so this switches in place without touching the machine, TV
     * input or cartridge ROM. */
    QMenu *cpuMenu = view->addMenu(QStringLiteral("CP&U"));
    m_cpuGroup = new QActionGroup(this);
    m_cpuGroup->setExclusive(true);
    for (int i = 0; const char *name = cocosession_cpu_name(i); i++) {
        QAction *act = cpuMenu->addAction(QString::fromUtf8(name));
        act->setCheckable(true);
        act->setChecked(i == cocosession_cpu(m_session));
        m_cpuGroup->addAction(act);
        connect(act, &QAction::triggered, this, [this, i] {
            cocosession_set_cpu(m_session, i);
            status(QStringLiteral("%1 (reset)")
                       .arg(QString::fromUtf8(cocosession_cpu_name(
                           cocosession_cpu(m_session)))));
            m_display->setFocus();
        });
    }

    view->addSeparator();
    view->addAction(QStringLiteral("&Debugger"), QKeySequence(Qt::Key_F12),
                    this, [this] { DebuggerWindow::show(this, m_session); });

    QMenu *settings = menuBar()->addMenu(QStringLiteral("&Settings"));
    settings->addAction(QStringLiteral("&Settings…"), this, [this] {
        SettingsDialog dlg(m_session, this);
        connect(&dlg, &SettingsDialog::romsImported, this, &MainWindow::start);
        connect(&dlg, &SettingsDialog::displayChanged, this,
                &MainWindow::applyDisplaySettings);
        dlg.exec();
        if (dlg.sessionDirty()) {
            restartSession();
            status(QStringLiteral("FujiNet options applied "
                                  "(session restarted)"));
        }
        showPage();
    });

    QMenu *help = menuBar()->addMenu(QStringLiteral("&Help"));
    help->addAction(QStringLiteral("&About"), this, [this] {
        QMessageBox::about(
            this, QStringLiteral("FujiNet Go CoCo"),
            QStringLiteral(
                "<b>FujiNet Go CoCo</b> %1<br><br>"
                "Self-contained Tandy Color Computer with built-in "
                "FujiNet.<br>"
                "Emulation by XRoar; FujiNet runs in-process over the "
                "Becker port.<br><br>"
                "Copyright © 2026 Thomas Cherryhomes<br>"
                "GPL-3.0-or-later &middot; "
                "<a href=\"https://fujinet.online/\">fujinet.online</a>")
                .arg(QStringLiteral(COCO_VERSION_STRING)));
    });
}
