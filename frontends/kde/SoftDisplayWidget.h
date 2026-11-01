/*
 * The software display: the fallback for a machine with no usable GPU
 * device. A QWidget that scales with QPainter, fed by FrameFeed like the GPU
 * one.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QImage>
#include <QWidget>
#include <cstdint>
#include <vector>

#include "DisplayView.h"
#include "FrameFeed.h"
#include "cocosession.h"

class SoftDisplayWidget : public QWidget, public DisplayView {
    Q_OBJECT
public:
    explicit SoftDisplayWidget(cocosession *session, QWidget *parent = nullptr);
    ~SoftDisplayWidget() override;

    QWidget *widget() override { return this; }
    void setAspectMode(int mode) override;
    void setSmooth(bool smooth) override;

protected:
    void paintEvent(QPaintEvent *) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    bool event(QEvent *e) override;
    bool eventFilter(QObject *watched, QEvent *e) override;
    bool focusNextPrevChild(bool) override { return false; }

private:
    void onFrame();
    void updateActive();

    cocosession *m_session;
    FrameFeed *m_feed;
    std::vector<uint32_t> m_frame;   /* as copy_frame wrote it */
    QImage m_image;                  /* Format_RGB32, alpha forced on */
    uint64_t m_serial = 0;
    int m_aspectMode = CocoAspectTv43;
    bool m_smooth = false;
};
