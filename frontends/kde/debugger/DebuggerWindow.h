/*
 * Debugger window (Qt6 Widgets) over the 6809/6309 debugger engine, via
 * core/include/cocodebug.h. Mirrors the other family windows tab for tab: a
 * QTabWidget under a toolbar, the registers in a strip under the tabs.
 *
 * The engine is engaged while the window is shown (which stops the machine, as
 * on every sibling) and released when it is hidden, which lets the machine run
 * on. The window is never deleted by its close button: it is hidden, and comes
 * back as it was.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QStringList>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <cstdint>
#include <functional>
#include <vector>

extern "C" {
#include "cocodebug.h"
#include "cocosession.h"
}

class VideoTab;
class SoundPlot;

class DebuggerWindow : public QMainWindow {
    Q_OBJECT
public:
    /* Shows the window (engaging, so the machine stops). */
    static void show(QWidget *parent, cocosession *session);
    /* F12: shows it, or hides it (releasing) when it is already up. */
    static void toggle(QWidget *parent, cocosession *session);

protected:
    bool event(QEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    explicit DebuggerWindow(cocosession *session, QWidget *parent);
    ~DebuggerWindow() override;
    QWidget *buildToolbar();
    QWidget *buildConsole();
    QWidget *buildRegs();
    QWidget *buildDisasm();
    QWidget *buildSource();
    QWidget *buildMemory();
    QWidget *buildSearchBar();
    QWidget *buildVideo();
    QWidget *buildBreaks();
    QWidget *buildWatches();
    QWidget *buildCart();
    QWidget *buildHist(int idx);
    QWidget *buildSound();
    QWidget *buildSymbols();
    QWidget *buildFujinet();

    void refreshStatus();
    void refreshCpu();
    void refreshMem();
    void refreshDisasm();
    void refreshSource(bool force);
    void refreshVideo(bool force);
    void refreshBps();
    void refreshWatches(bool force);
    void refreshCart();
    void refreshHist(int idx, bool force);
    void refreshSound();
    void refreshSymbols(bool force);
    void refreshFujinet(bool force);
    void refreshPage(int page, bool live);
    void refreshAll();
    void syncSpaces();
    void resync();
    void syncLater();
    void settleAndSync();
    void tick();
    void recentreOnStop();
    void watchSound();

    void consoleAppend(const QString &text);
    void runConsole();
    void completeConsole();
    void say(const QString &msg);
    void message(const QString &msg);

    bool stopped();
    void toggleRun();
    void stepAnd(const std::function<void()> &fn);
    void runToCursor();
    void clearBreakpoints();
    void toggleBreakpoint(int addr);
    void toggleSelected();
    void addBreakpoint();
    void breakAt();
    void setReg(int reg);
    void setFlag(unsigned mask, bool on);
    void memGoto();
    void memSearch(bool backward);
    void memSpaceChanged(int space);
    void showInMemory(int space, uint32_t lo, uint32_t hi);
    void showInDisassembly(uint16_t addr);
    void scrollMemory(uint32_t addr);
    int memVisibleRows() const;
    uint32_t memRows() const;
    void popOut(int tab);
    void dock(int tab);
    int currentTab() const;
    bool tabVisible(int tab) const;
    void addWatch(const QString &text);
    void watchBreak(int index);
    void memWrite();
    void jumpTo();
    void loadSymbols();
    void clearSymbols();
    void loadSource();
    void clearSource();
    void sourceToggleAt(int line);
    void sourceFileChosen(int index);
    void rebuildSourceFiles();
    int visibleRows() const;
    bool numberOf(const QString &text, long *out, QString *why = nullptr);
    bool addressOf(const QString &text, int space, long *out, QString *why = nullptr);
    bool rangeOf(const QString &startText, const QString &endText, int space, uint32_t *lo, uint32_t *hi,
                 QString *why);
    void setEntry(QLineEdit *e, const QString &text);

    cocosession *m_session;
    cocodebug *m_dbg;
    unsigned m_seenGen = 0;
    bool m_wasStopped = false;
    bool m_is6309 = false;
    bool m_coco3 = false;
    int m_runningTicks = 0;
    QTimer m_timer;
    QTabWidget *m_tabs = nullptr;

    QLabel *m_status = nullptr;
    QPushButton *m_runBtn = nullptr;
    QPushButton *m_stepLine = nullptr, *m_stepOverLine = nullptr;
    QLineEdit *m_breakAt = nullptr;

    /* console */
    QPlainTextEdit *m_conOut = nullptr;
    QLineEdit *m_conIn = nullptr;
    QStringList m_history;
    int m_historyPos = 0;

    /* registers strip: m_reg is indexed by cocodebug_reg */
    QLineEdit *m_reg[COCODEBUG_REG_COUNT] = {};
    QLabel *m_regLabel[COCODEBUG_REG_COUNT] = {};
    QCheckBox *m_cc[8] = {};            /* E F H I N Z V C, bit 7 first */
    QWidget *m_regs6309 = nullptr;
    QLabel *m_machine = nullptr;
    bool m_regFilling = false;

    /* memory: one space at a time, a window of rows the scroll bar moves over */
    QPlainTextEdit *m_mem = nullptr;
    QScrollBar *m_memBar = nullptr;
    QComboBox *m_memSpace = nullptr;
    QLineEdit *m_memGoto = nullptr, *m_memAddr = nullptr, *m_memVal = nullptr;
    int m_space = COCODEBUG_SPACE_CPU;
    uint32_t m_memTop = 0;              /* the first row shown (16 bytes a row) */
    bool m_hl = false;                  /* a range is highlighted in the dump */
    int m_hlSpace = COCODEBUG_SPACE_CPU;
    uint32_t m_hlLo = 0, m_hlHi = 0;
    int m_wheelMem = 0;
    bool m_memFilling = false;
    QComboBox *m_srchKind = nullptr;
    QLineEdit *m_srchEntry = nullptr;
    QLabel *m_srchNote = nullptr;

    /* disassembly */
    QPlainTextEdit *m_disasm = nullptr;
    QCheckBox *m_followPc = nullptr;
    QLineEdit *m_jump = nullptr;
    uint16_t m_disasmTop = 0;
    uint64_t m_seenSerial = 0;          /* the stop serial last followed */
    int m_selected = -1;                /* the selected line's address, or -1 */
    int m_wheel = 0;                    /* wheel travel not yet a row */
    std::vector<uint16_t> m_lineAddr;

    /* C source */
    QComboBox *m_srcFiles = nullptr;
    QPlainTextEdit *m_srcView = nullptr;
    QPlainTextEdit *m_srcLocals = nullptr;
    QLabel *m_srcNote = nullptr;
    QString m_srcPath;                  /* the file m_srcLines holds */
    QStringList m_srcLines;
    int m_srcFile = -1;                 /* the file shown, -1 none */
    int m_srcBar = -1;                  /* the block wearing the PC bar */
    int m_srcPcFile = -1, m_srcPcLine = 0;
    int m_srcCount = -1;                /* source files known when the combo was filled */
    QString m_srcSig;                   /* what the view shows, to rebuild on change */
    uint64_t m_srcSerial = 0;

    /* video */
    VideoTab *m_video = nullptr;
    bool m_videoForce = true;           /* the tab was just shown: redraw everything */

    /* breakpoints */
    QTableWidget *m_bpTable = nullptr;
    QString m_bpSig;                    /* what the table shows, to rebuild on change */
    bool m_bpFilling = false;
    QLabel *m_bpEmpty = nullptr, *m_bpMsg = nullptr;
    QComboBox *m_bpType = nullptr;
    QCheckBox *m_bpPhys = nullptr;
    QLineEdit *m_bpStart = nullptr, *m_bpEnd = nullptr, *m_bpCond = nullptr;
    QLabel *m_bpStop = nullptr;

    /* watches */
    QLineEdit *m_wtEntry = nullptr;
    QComboBox *m_wtSpace = nullptr;
    QTableWidget *m_wtTable = nullptr;
    unsigned m_wtGen = ~0u;

    /* cart and I/O */
    QPlainTextEdit *m_cartText = nullptr, *m_io = nullptr;

    /* sound */
    QPlainTextEdit *m_sound = nullptr;
    SoundPlot *m_soundPlot = nullptr;
    bool m_soundWatching = false;

    /* symbols */
    QLineEdit *m_symFilter = nullptr;
    QLineEdit *m_symAddr = nullptr, *m_symName = nullptr;
    QTableWidget *m_symTable = nullptr;
    QLabel *m_symCount = nullptr;
    QString m_symSig;

    /* trace (0, live) and history (1, on a stop) share the engine's ring */
    QPushButton *m_hRec[2] = {};
    QComboBox *m_hFilter[2] = {};
    QLineEdit *m_hRange[2] = {};
    QPlainTextEdit *m_hText[2] = {};
    QLabel *m_hNote[2] = {};
    int m_hSeen[2] = { -1, -1 };
    bool m_hSync = false;

    /* tabs peeled off into windows of their own */
    QWidget *m_pages[16] = {};
    QWidget *m_popped[16] = {};
    bool m_closing = false;

    /* fujinet */
    QLabel *m_fujiNote = nullptr;
    QCheckBox *m_fujiHideReady = nullptr;
    QPlainTextEdit *m_fujiText = nullptr, *m_fujiState = nullptr;
    uint64_t m_fujiLast = 0;
    int m_fujiCount = -1;
};
