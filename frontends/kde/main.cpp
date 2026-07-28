/*
 * FujiNet Go CoCo -- KDE/Qt frontend entry point.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QApplication>
#include <QIcon>
#include <QSurfaceFormat>

#include <cstdio>

#include "MainWindow.h"
#include "cocosession.h"

int main(int argc, char *argv[])
{
    /* Vsync'd swaps for the display widget -- frameSwapped is what feeds the
     * session's phase-lock. Shared contexts for WebEngine. */
    QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
    fmt.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(fmt);
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("FujiNet Go CoCo"));
    app.setDesktopFileName(QStringLiteral("online.fujinet.go.coco.kde"));
#ifdef COCO_ICON_FILE
    /* Dev-tree fallback; installed runs resolve the themed icon. */
    app.setWindowIcon(QIcon::fromTheme(
        QStringLiteral("online.fujinet.go.coco.kde"),
        QIcon(QStringLiteral(COCO_ICON_FILE))));
#endif

    cocosession *session = cocosession_new(nullptr);
    if (!session) {
        fprintf(stderr, "fatal: could not create the session\n");
        return 1;
    }
    cocosession_start_opts opts;
    cocosession_default_opts(session, &opts);
    if (cocosession_start(session, &opts) != 0)
        fprintf(stderr, "session start: %s\n",
                cocosession_last_error(session));

    MainWindow win(session);
    win.show();
    const int rc = app.exec();

    cocosession_free(session);
    return rc;
}
