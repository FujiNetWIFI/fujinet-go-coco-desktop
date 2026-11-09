/*
 * MainWindow: menu bar over the emulator display for the KDE/Qt frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QActionGroup>
#include <QMainWindow>
#include <QTimer>

#include "cocosession.h"

class DisplayView;
class QStackedWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(cocosession *session, QWidget *parent = nullptr);

protected:
    bool event(QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    void tick();
    void runSysaction(int sysact);
    void toggleControllers();
    void buildMenus();
    void rebuildTvInputMenu();
    void applyDisplaySettings();
    void useSoftwareDisplay();
    void restartSession();
    void importDiskImage();
    void importBios(bool folder);
    QWidget *buildNoBiosPage();
    void showPage();
    void start();
    void status(const QString &message);

    cocosession *m_session;
    /* The pad thread cannot call into Qt: it posts system actions and gamepad
     * events, and this timer takes them. */
    QTimer m_tick;
    unsigned m_padGen = 0;
    DisplayView *m_display;
    QStackedWidget *m_stack;
    QWidget *m_noBios;
    /* Rebuilt when the machine changes: a CoCo 2 has no RGB input, so that
     * entry has to disappear rather than sit there rendering a blank screen. */
    QMenu *m_tvInputMenu = nullptr;
    QActionGroup *m_tvInputGroup = nullptr;
    QActionGroup *m_machineGroup = nullptr;
    QActionGroup *m_cpuGroup = nullptr;
};
