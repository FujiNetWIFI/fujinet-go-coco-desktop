/*
 * MainWindow: menu bar over the emulator display for the KDE/Qt frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QActionGroup>
#include <QMainWindow>

#include "cocosession.h"

class DisplayWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(cocosession *session, QWidget *parent = nullptr);

private:
    void buildMenus();
    void rebuildTvInputMenu();
    void applyDisplaySettings();
    void restartSession();
    void importDiskImage();
    void importSystemRoms();
    void status(const QString &message);

    cocosession *m_session;
    DisplayWidget *m_display;
    /* Rebuilt when the machine changes: a CoCo 2 has no RGB input, so that
     * entry has to disappear rather than sit there rendering a blank screen. */
    QMenu *m_tvInputMenu = nullptr;
    QActionGroup *m_tvInputGroup = nullptr;
    QActionGroup *m_machineGroup = nullptr;
    QActionGroup *m_cpuGroup = nullptr;
};
