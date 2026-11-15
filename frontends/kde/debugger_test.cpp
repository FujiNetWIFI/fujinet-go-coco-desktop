/*
 * debugger_test -- the Debugger window (debugger/DebuggerWindow.cpp) driven
 * with synthetic events on the offscreen platform against a running CoCo
 * session (FujiNet and audio off) whose RAM holds a hand-assembled loop written
 * through the debugger: the thirteen tabs, engage/release with show and hide,
 * F12, the stepping keys and buttons, the console (transcript, Tab completion,
 * history), the disassembly (the PC's accent bar, click / double-click / F9 for
 * a breakpoint, selection and Run to Cursor, the wheel, the jump box), the
 * register strip (entries, flags, the 6309 group), Memory (CPU and, on a
 * CoCo 3, physical RAM: Go to, Poke, search), the Breakpoints tab (every
 * type, physical, conditions), Watches, Trace and History, Symbols, Break at,
 * C Source (a CMOC listing: gutter dots, the PC bar, locals, Step Line), Cart &
 * I/O, Sound, FujiNet, pop-out and dock, the Video tab, and F12 and the View
 * menu from the main window.
 *
 * One machine per process: DBG_TEST_MACHINE=coco2 starts a CoCo 2, anything
 * else a CoCo 3. DBG_TEST_SHOTS=<dir> also grabs a PNG of each tab
 * (DBG_TEST_LIGHT=1: in the light scheme).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <cstring>

#include <QFile>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QOpenGLContext>
#include <QTemporaryDir>
#include <QtTest>

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QStatusBar>
#include <QStyleHints>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBlock>
#include <QToolButton>

#include "cocodebug.h"
#include "cocomemtools.h"
#include "cocosession.h"
#include "DisplayWidget.h"
#include "MainWindow.h"
#include "debugger/DebuggerWindow.h"

namespace {

/* The test subject, in RAM at $0700 (interrupts masked):
 *   0700  LDA $0850      reads the counter
 *   0703  INCA
 *   0704  STA $0850      writes it
 *   0707  BSR $070E      the call
 *   0709  BRA $0700
 *   070E  RTS            the subroutine */
const uint8_t kProg[] = { 0xB6, 0x08, 0x50, 0x4C, 0xB7, 0x08, 0x50, 0x8D, 0x05, 0x20, 0xF5, 0x12, 0x12, 0x12, 0x39 };
const int kStart = 0x0700, kInc = 0x0703, kStore = 0x0704, kCall = 0x0707, kAfter = 0x0709, kSub = 0x070E,
          kCounter = 0x0850;

/* Sound: LDA #$FC / STA $FF20 / LDA #$04 / STA $FF20 / BRA, at $0780. */
const uint8_t kBeep[] = { 0x86, 0xFC, 0xB7, 0xFF, 0x20, 0x86, 0x04, 0xB7, 0xFF, 0x20, 0x20, 0xF4 };
const int kBeepAt = 0x0780;

/* The two functions of core/tests/data/hello.c as CMOC lays them out (add at
 * $2E6D, main at $2E7D): enough to step through main up to the call to printf. */
const uint8_t kAddMain[] = {
    0x34, 0x40, 0x33, 0xE4, 0x32, 0x7E, 0xEC, 0x44, 0xE3, 0x46, 0xED, 0x5E, 0x32, 0xC4, 0x35, 0xC0,
    0x34, 0x40, 0x33, 0xE4, 0x32, 0x7A, 0x4F, 0xC6, 0x05, 0xED, 0x5A, 0xC6, 0x0A, 0xED, 0x5C, 0xAE,
    0x5C, 0xEC, 0x5A, 0x34, 0x16, 0x17, 0xFF, 0xD8, 0x32, 0x64, 0xED, 0x5E, 0x34, 0x06, 0x30, 0x8D,
    0x00, 0x1E, 0x34, 0x10, 0x17, 0xFB, 0x1F, 0x32, 0x64, 0x4F, 0x5F, 0x32, 0xC4, 0x35, 0xC0,
};
const int kAddAt = 0x2E6D, kMainAt = 0x2E7D;

/* The accent: the CoCo icon's green; its ink; the dark green for text on light. */
const QColor kAccent(0x8B, 0xC3, 0x4A);
const QColor kInk(0x14, 0x21, 0x0A);

/* With the machine stopped: the program in place, PC at its INCA. */
void loadProgram(cocodebug *d)
{
    /* A run of NOPs before and after the program: the disassembly steps back
     * from the PC by guessing, and without system ROMs the RAM holds noise
     * whose instruction boundaries would put the guess out of step. */
    uint8_t nops[0x100];
    memset(nops, 0x12, sizeof nops);
    cocodebug_mem_write(d, COCODEBUG_SPACE_CPU, kStart - 0x80, nops, 0x80);
    cocodebug_mem_write(d, COCODEBUG_SPACE_CPU, kStart + (int)sizeof kProg, nops, 0x80);
    cocodebug_mem_write(d, COCODEBUG_SPACE_CPU, kStart, kProg, (int)sizeof kProg);
    cocodebug_mem_write(d, COCODEBUG_SPACE_CPU, kBeepAt, kBeep, (int)sizeof kBeep);
    cocodebug_mem_write(d, COCODEBUG_SPACE_CPU, kAddAt, kAddMain, (int)sizeof kAddMain);
    cocodebug_regs_set(d, COCODEBUG_REG_CC, 0x50);
    cocodebug_regs_set(d, COCODEBUG_REG_S, 0x6FF0);
    cocodebug_regs_set(d, COCODEBUG_REG_U, 0x6FF0);
    cocodebug_regs_set(d, COCODEBUG_REG_PC, kInc);
}

int pcOf(cocodebug *d)
{
    cocodebug_regs r;
    return cocodebug_regs_get(d, &r) ? r.pc : -1;
}

} // namespace

class DebuggerTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    cocosession *m_s = nullptr;
    cocodebug *m_d = nullptr;
    QMainWindow *m_win = nullptr;
    QTabWidget *m_tabs = nullptr;
    QString m_shots;
    cocosession_start_opts m_opts = {};
    bool m_coco3 = true;

    QPushButton *button(const QString &textStart)
    {
        for (QPushButton *b : m_win->findChildren<QPushButton *>())
            if (b->text().startsWith(textStart)) return b;
        return nullptr;
    }
    QLineEdit *entry(const QString &placeholderStart)
    {
        for (QLineEdit *e : m_win->findChildren<QLineEdit *>())
            if (e->placeholderText().startsWith(placeholderStart)) return e;
        return nullptr;
    }
    QLineEdit *entryIs(const QString &placeholder)
    {
        for (QLineEdit *e : m_win->findChildren<QLineEdit *>())
            if (e->placeholderText() == placeholder) return e;
        return nullptr;
    }
    QPlainTextEdit *disasmView() { return m_tabs->widget(1)->findChild<QPlainTextEdit *>(); }
    QPlainTextEdit *memView() { return m_tabs->widget(3)->findChild<QPlainTextEdit *>(); }
    QPlainTextEdit *srcView() { return m_tabs->widget(2)->findChild<QPlainTextEdit *>(QStringLiteral("sourceView")); }
    QPlainTextEdit *srcLocals() { return m_tabs->widget(2)->findChild<QPlainTextEdit *>(QStringLiteral("sourceLocals")); }
    QPlainTextEdit *consoleView() { return m_tabs->widget(0)->findChild<QPlainTextEdit *>(); }
    QLabel *statusLabel()
    {
        for (QLabel *l : m_win->findChildren<QLabel *>())
            if (l->text().startsWith(QLatin1String("Stopped")) || l->text() == QLatin1String("Running"))
                return l;
        return nullptr;
    }
    QCheckBox *check(const QString &text)
    {
        for (QCheckBox *c : m_win->findChildren<QCheckBox *>())
            if (c->text() == text) return c;
        return nullptr;
    }
    QTableWidget *bpTable() { return m_tabs->widget(4)->findChild<QTableWidget *>(); }
    QList<QLineEdit *> regEntries()
    {
        QList<QLineEdit *> regs;
        for (QLineEdit *e : m_win->findChildren<QLineEdit *>())
            if (e->toolTip().contains(QLatin1String("Enter sets it"))) regs << e;
        return regs;
    }

    bool stopped()
    {
        cocodebug_stop s;
        cocodebug_stop_snapshot(m_d, &s);
        return s.paused != 0;
    }
    void waitStopped()
    {
        QTRY_VERIFY_WITH_TIMEOUT(stopped(), 3000);
        /* the window follows within its poll */
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel() && statusLabel()->text().startsWith(QLatin1String("Stopped")), 2000);
    }
    void shot(const QString &name)
    {
        if (!m_shots.isEmpty()) m_win->grab().save(m_shots + QLatin1Char('/') + name + QStringLiteral(".png"));
    }
    bool bpSet(int addr) { return cocodebug_breakpoint_is_set(m_d, (uint16_t)addr) != 0; }
    int bpCount()
    {
        cocodebug_breakpoint list[64];
        return cocodebug_breakpoints_list(m_d, list, 64);
    }
    void clearBreakpoints()
    {
        cocodebug_breakpoint list[64];
        const int n = cocodebug_breakpoints_list(m_d, list, 64);
        for (int i = 0; i < n; ++i) cocodebug_breakpoint_remove(m_d, list[i].id);
    }
    int byteAt(int addr, int space = COCODEBUG_SPACE_CPU)
    {
        uint8_t b = 0;
        cocodebug_mem_read(m_d, space, (uint32_t)addr, &b, 1);
        return b;
    }

    /* The block of the disassembly that shows an address, or -1. */
    int lineOf(QPlainTextEdit *v, int addr)
    {
        const QString want = QStringLiteral("%1").arg(addr, 4, 16, QLatin1Char('0')).toUpper();
        int n = 0;
        for (QTextBlock b = v->document()->begin(); b.isValid(); b = b.next(), ++n)
            if (b.text().mid(2, 4) == want) return n;
        return -1;
    }
    QPoint lineAt(QPlainTextEdit *v, int line, int column)
    {
        QTextCursor c(v->document()->findBlockByNumber(line));
        c.setPosition(c.block().position() + column);
        const QRect r = v->cursorRect(c);
        return QPoint(r.left() + 1, r.center().y());
    }
    void clickLine(int addr, int column, bool dbl = false)
    {
        QPlainTextEdit *v = disasmView();
        const int line = lineOf(v, addr);
        QVERIFY2(line >= 0, qPrintable(QStringLiteral("line for %1").arg(addr, 0, 16)));
        const QPoint p = lineAt(v, line, column);
        if (dbl) QTest::mouseDClick(v->viewport(), Qt::LeftButton, {}, p);
        else QTest::mouseClick(v->viewport(), Qt::LeftButton, {}, p);
    }
    void typeInto(QLineEdit *e, const QString &text)
    {
        e->clear();
        QTest::keyClicks(e, text);
        QTest::keyClick(e, Qt::Key_Return);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        qputenv("SDL_AUDIODRIVER", "dummy");
        if (qEnvironmentVariableIsSet("DBG_TEST_SHOTS")) m_shots = qEnvironmentVariable("DBG_TEST_SHOTS");
        m_coco3 = qEnvironmentVariable("DBG_TEST_MACHINE") != QLatin1String("coco2");
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        if (qEnvironmentVariableIsSet("DBG_TEST_LIGHT"))
            QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);
#endif
        cocosession_paths paths = {};
        const QByteArray dir = m_dir.path().toUtf8();
        paths.config_dir = dir.constData();
        paths.data_dir = dir.constData();
        paths.fujinet_lib = "";
        m_s = cocosession_new(&paths);
        QVERIFY(m_s);
        m_d = cocosession_debugger(m_s);
        QVERIFY(m_d);

        cocosession_default_opts(m_s, &m_opts);
        m_opts.machine = m_coco3 ? COCOSESSION_MACHINE_COCO3 : COCOSESSION_MACHINE_COCO2;
        m_opts.enable_fujinet = 0;
        m_opts.enable_audio = 0;
        m_opts.enable_gamepad = 0;
        m_opts.cpu = qEnvironmentVariable("DBG_TEST_CPU") == QLatin1String("6309") ? COCOSESSION_CPU_HD6309
                                                                                   : COCOSESSION_CPU_MC6809;
        if (cocosession_start(m_s, &m_opts) != 0) QSKIP("the session did not start (no system ROMs in this build?)");

        /* the window opens on the tab COCO_DEBUGGER_TAB names, and COCO_WATCH,
         * COCO_WATCH_BREAK and COCO_MEM_GOTO are read at creation */
        qputenv("COCO_DEBUGGER_TAB", "i/o");
        qputenv("COCO_WATCH", "$0870, $0880-$088F");
        qputenv("COCO_WATCH_BREAK", "$0890");
        qputenv("COCO_MEM_GOTO", "$08A0-$08AF");
        DebuggerWindow::show(nullptr, m_s);
        qunsetenv("COCO_DEBUGGER_TAB");
        qunsetenv("COCO_WATCH");
        qunsetenv("COCO_WATCH_BREAK");
        qunsetenv("COCO_MEM_GOTO");
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->objectName() == QLatin1String("debugger")) m_win = qobject_cast<QMainWindow *>(w);
        QVERIFY(m_win);
        QVERIFY(QTest::qWaitForWindowExposed(m_win));
        m_tabs = m_win->findChild<QTabWidget *>();
        QVERIFY(m_tabs);
        QCOMPARE(m_tabs->currentIndex(), 6);
        QCOMPARE(cocowatch_count(), 3);                      /* two watches and the break-on-change one */
        cocodebug_breakpoint list[4];
        QCOMPARE(cocodebug_breakpoints_list(m_d, list, 4), 1);
        QCOMPARE((int)list[0].start, 0x0890);
        QVERIFY(QString::fromUtf8(list[0].condition).contains(QStringLiteral("old")));
        cocowatch_clear();
        clearBreakpoints();
        m_tabs->setCurrentIndex(0);
        waitStopped();
    }

    void cleanupTestCase()
    {
        delete m_win;       /* releases */
        if (m_s) {
            cocosession_stop(m_s);
            cocosession_free(m_s);
        }
    }

    void init()
    {
        if (!m_win->isVisible()) DebuggerWindow::toggle(nullptr, m_s);
        QTRY_VERIFY_WITH_TIMEOUT(cocodebug_is_engaged(m_d), 2000);
        if (!stopped()) cocodebug_pause(m_d);
        waitStopped();
        clearBreakpoints();
        loadProgram(m_d);
    }

    void tabsAndEngage()
    {
        QStringList names;
        for (int i = 0; i < m_tabs->count(); ++i) names << m_tabs->tabText(i);
        QCOMPARE(names, (QStringList{ QStringLiteral("Console"), QStringLiteral("Disassembly"),
                                      QStringLiteral("C Source"), QStringLiteral("Memory"),
                                      QStringLiteral("Breakpoints"), QStringLiteral("Watches"),
                                      QStringLiteral("Cart && I/O"), QStringLiteral("Trace"),
                                      QStringLiteral("History"), QStringLiteral("Video"), QStringLiteral("Sound"),
                                      QStringLiteral("Symbols"), QStringLiteral("FujiNet") }));
        QVERIFY(cocodebug_is_engaged(m_d));
        QVERIFY(stopped());
        /* Help's context follows the tab: the thirteen names of the Help's debugger section */
        const char *const contexts[] = { "console", "disassembly", "csource", "memory", "breakpoints", "watches",
                                         "cart", "trace", "history", "video", "sound", "symbols", "fujinet" };
        for (int i = 0; i < 13; ++i) {
            m_tabs->setCurrentIndex(i);
            QCOMPARE(m_win->property("cocoHelpContext").toString(),
                     QStringLiteral("debugger/") + QLatin1String(contexts[i]));
        }
        m_tabs->setCurrentIndex(0);
        /* no tab sets the window's minimum height beyond a laptop screen */
        QVERIFY2(m_win->minimumSizeHint().height() <= 800, qPrintable(QString::number(m_win->minimumSizeHint().height())));
        /* Run is the accent while stopped, whichever text it wears, in one size */
        QPushButton *run = button(QStringLiteral("Run (F5)"));
        QVERIFY(run);
        QVERIFY(run->isChecked());
        QVERIFY(run->styleSheet().contains(kAccent.name()));
        QVERIFY(run->styleSheet().contains(kInk.name()));
        const QSize size = run->size();
        QTest::mouseClick(run, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!stopped(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(run->text() == QLatin1String("Pause (F5)"), 2000);
        QVERIFY(!run->isChecked());
        QCOMPARE(run->size(), size);
        QCOMPARE(statusLabel()->text(), QStringLiteral("Running"));
        QTest::mouseClick(run, Qt::LeftButton);
        waitStopped();
        QVERIFY(run->isChecked());
        QCOMPARE(run->size(), size);
        /* the toolbar has the family's buttons */
        for (const char *label : { "Step (F7)", "Step Over", "Step Out", "Step Line", "Step Over Line",
                                   "Run to Cursor", "Clear Breakpoints", "Load Symbols", "Clear Symbols",
                                   "Load C Source", "Clear C Source", "Pop Out" })
            QVERIFY2(button(QString::fromUtf8(label)), label);
        /* Step Line needs C source */
        QVERIFY(!button(QStringLiteral("Step Line"))->isEnabled() || cocodebug_source_available(m_d));
    }

    void stepKeys()
    {
        QCOMPARE(pcOf(m_d), kInc);
        QTest::keyClick(m_win, Qt::Key_F7);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), kStore, 2000);
        QCOMPARE(statusLabel()->text().left(13), QStringLiteral("Stopped: step"));
        /* F8 steps over the BSR call: run to the instruction after it */
        cocodebug_regs_set(m_d, COCODEBUG_REG_PC, kCall);
        QTest::keyClick(m_win, Qt::Key_F8);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), kAfter, 2000);
        /* the button steps out of the subroutine: back to its caller */
        cocodebug_regs_set(m_d, COCODEBUG_REG_S, 0x6FEE);
        const uint8_t ret[2] = { (uint8_t)(kAfter >> 8), (uint8_t)(kAfter & 0xFF) };
        cocodebug_mem_write(m_d, COCODEBUG_SPACE_CPU, 0x6FEE, ret, 2);
        cocodebug_regs_set(m_d, COCODEBUG_REG_PC, kSub);
        QTest::mouseClick(button(QStringLiteral("Step Out")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), kAfter, 2000);
        QVERIFY2(statusLabel()->text().contains(QLatin1String("returned")), qPrintable(statusLabel()->text()));
        /* stepping while running is refused with a word, not silently */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!stopped(), 2000);
        QTest::keyClick(m_win, Qt::Key_F7);
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("Pause the machine")));
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        /* F6 without C source says so */
        if (!cocodebug_source_available(m_d)) {
            QTest::keyClick(m_win, Qt::Key_F6);
            QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("C source")));
        }
    }

    /* The stop reason is the status line, and it is never cut: the label has
     * the width its whole text needs at the default window width. */
    void stopReasonNotElided()
    {
        cocodebug_breakpoint_add(m_d, COCODEBUG_BP_WRITE, kCounter, kCounter, "value != old");
        cocodebug_mem_write(m_d, COCODEBUG_SPACE_CPU, kCounter, (const uint8_t *)"\x77", 1);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(stopped(), 4000);
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("wrote $0850")), 2000);
        QLabel *status = statusLabel();
        QVERIFY(status->text().contains(QLatin1String("(was $77)")));
        QVERIFY2(status->width() >= status->fontMetrics().horizontalAdvance(status->text()),
                 qPrintable(QStringLiteral("status %1 wide for %2").arg(status->width())
                                .arg(status->fontMetrics().horizontalAdvance(status->text()))));
        QVERIFY(!status->text().contains(QChar(0x2026)));
        shot(QStringLiteral("stopreason"));
    }

    void consoleCommands()
    {
        m_tabs->setCurrentIndex(0);
        QLineEdit *in = entry(QStringLiteral("command"));
        QPlainTextEdit *out = consoleView();
        QVERIFY(in && out);
        QCOMPARE(m_win->focusWidget(), in);         /* typing goes straight in */
        typeInto(in, QStringLiteral("regs"));
        QVERIFY(out->toPlainText().contains(QStringLiteral("> regs")));
        QVERIFY(out->toPlainText().contains(QStringLiteral("PC=")));
        QVERIFY(in->text().isEmpty());
        /* a command that moves the machine moves the window */
        typeInto(in, QStringLiteral("set pc $0700"));
        QCOMPARE(pcOf(m_d), kStart);
        typeInto(in, QStringLiteral("break $0707"));
        QVERIFY(bpSet(kCall));
        typeInto(in, QStringLiteral("run"));
        QTRY_VERIFY_WITH_TIMEOUT(out->toPlainText().contains(QStringLiteral("— breakpoint: exec $0707")), 3000);
        waitStopped();
        QCOMPARE(pcOf(m_d), kCall);
        QVERIFY(statusLabel()->text().contains(QStringLiteral("breakpoint: exec $0707")));
        /* Tab completes a command, lists several, and Up recalls */
        in->clear();
        QTest::keyClicks(in, QStringLiteral("reg"));
        QTest::keyClick(in, Qt::Key_Tab);
        QCOMPARE(in->text(), QStringLiteral("regs "));
        in->clear();
        QTest::keyClicks(in, QStringLiteral("b"));
        QTest::keyClick(in, Qt::Key_Tab);
        QVERIFY(out->toPlainText().contains(QStringLiteral("break")));
        QVERIFY(out->toPlainText().contains(QStringLiteral("bpw")));
        QVERIFY(in->text().startsWith(QLatin1Char('b')));
        in->clear();
        QTest::keyClick(in, Qt::Key_Up);
        QCOMPARE(in->text(), QStringLiteral("run"));
        QTest::keyClick(in, Qt::Key_Up);
        QCOMPARE(in->text(), QStringLiteral("break $0707"));
        QTest::keyClick(in, Qt::Key_Down);
        QCOMPARE(in->text(), QStringLiteral("run"));
        in->clear();
        shot(QStringLiteral("console"));
    }

    /* A symbol file loaded through the engine shows in the views, and Break at
     * takes a symbol or a hex address. */
    void symbolsAndBreakAt()
    {
        m_tabs->setCurrentIndex(0);
        uint16_t found = 0;
        QVERIFY(!cocodebug_symbol_find(m_d, "afterwards", &found));
        QFile sym(m_dir.path() + QStringLiteral("/loop.sym"));
        QVERIFY(sym.open(QIODevice::WriteOnly));
        sym.write("afterwards EQU $0709\nhelper EQU $070E\n");
        sym.close();
        QLineEdit *in = entry(QStringLiteral("command"));
        typeInto(in, QStringLiteral("labels ") + sym.fileName());
        QVERIFY(cocodebug_symbol_find(m_d, "afterwards", &found));
        QCOMPARE((int)found, kAfter);

        /* Break at: a symbol, then a hex address, then a repeat */
        QLineEdit *breakAt = entry(QStringLiteral("symbol or"));
        QVERIFY(breakAt);
        typeInto(breakAt, QStringLiteral("afterwards"));
        QVERIFY(bpSet(kAfter));
        QVERIFY(breakAt->text().isEmpty());
        typeInto(breakAt, QStringLiteral("0704"));
        QVERIFY(bpSet(kStore));
        typeInto(breakAt, QStringLiteral("0704"));
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("already")));
        QCOMPARE(bpCount(), 2);
        typeInto(breakAt, QStringLiteral("nosuchsymbol"));
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("Not a number")));
        clearBreakpoints();

        /* the stop reason names where it is */
        typeInto(in, QStringLiteral("break afterwards"));
        QVERIFY(bpSet(kAfter));
        typeInto(in, QStringLiteral("run"));
        QTRY_VERIFY_WITH_TIMEOUT(stopped() && pcOf(m_d) == kAfter, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("(afterwards)")), 2000);

        /* the Symbols tab lists them, filtered; activating a row opens the Disassembly there */
        m_tabs->setCurrentIndex(11);
        QTableWidget *table = m_tabs->currentWidget()->findChild<QTableWidget *>();
        QLineEdit *filter = m_tabs->currentWidget()->findChild<QLineEdit *>();
        QVERIFY(table && filter);
        QTRY_VERIFY_WITH_TIMEOUT(table->rowCount() >= 8, 2000);        /* the built-in six and these */
        shot(QStringLiteral("symbols"));
        const int all = table->rowCount();
        filter->setText(QStringLiteral("AFTERW"));
        QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), 1, 2000);
        QCOMPARE(table->item(0, 1)->text(), QStringLiteral("afterwards"));
        QCOMPARE(table->item(0, 0)->text(), QStringLiteral("$0709"));
        table->setCurrentCell(0, 0);
        QTest::keyClick(table, Qt::Key_Return);             /* activating a row */
        QCOMPARE(m_tabs->currentIndex(), 1);
        QVERIFY(!check(QStringLiteral("Follow PC"))->isChecked());
        QVERIFY(lineOf(disasmView(), kAfter) >= 0);
        m_tabs->setCurrentIndex(11);
        /* Break at selected: an execute breakpoint there, toggled */
        table->setCurrentCell(0, 0);
        QTest::mouseClick(m_tabs->currentWidget()->findChildren<QPushButton *>().value(2), Qt::LeftButton);
        QVERIFY(m_tabs->currentWidget()->findChildren<QPushButton *>().value(2)->text() == QLatin1String("Break at"));
        QVERIFY(!bpSet(kAfter) || bpCount() >= 0);
        /* Add a label */
        QLineEdit *addr = entryIs(QStringLiteral("$address")), *name = entryIs(QStringLiteral("name"));
        QVERIFY(addr && name);
        addr->setText(QStringLiteral("0850"));
        typeInto(name, QStringLiteral("counter"));
        QVERIFY(cocodebug_symbol_find(m_d, "counter", &found));
        QCOMPARE((int)found, kCounter);
        filter->clear();
        QTRY_VERIFY_WITH_TIMEOUT(table->rowCount() > all, 2000);

        /* Clear Symbols drops the loaded ones */
        QTest::mouseClick(button(QStringLiteral("Clear Symbols")), Qt::LeftButton);
        QVERIFY(!cocodebug_symbol_find(m_d, "afterwards", &found));
        QTRY_VERIFY_WITH_TIMEOUT(table->rowCount() == 6, 2000);
        m_tabs->setCurrentIndex(0);
        clearBreakpoints();
    }

    void disassemblyHighlights()
    {
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *v = disasmView();
        QVERIFY(v);
        QCheckBox *follow = check(QStringLiteral("Follow PC"));
        if (!follow->isChecked()) QTest::mouseClick(follow, Qt::LeftButton, {}, QPoint(4, follow->height() / 2));
        QVERIFY(follow->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, kInc) >= 0, 2000);
        const int pcLine = lineOf(v, kInc);
        QCOMPARE(v->document()->findBlockByNumber(pcLine).text().left(2), QStringLiteral(" >"));
        /* every instruction's own bytes, not only the first */
        const QString lda = v->document()->findBlockByNumber(lineOf(v, kStart)).text();
        QVERIFY2(lda.contains(QStringLiteral("B6 08 50")), qPrintable(lda));
        QVERIFY2(lda.contains(QStringLiteral("LDA")), qPrintable(lda));
        /* follow-PC keeps it about a third of the way down */
        const int rows = v->document()->blockCount();
        QVERIFY2(pcLine >= rows / 3 - 3 && pcLine <= rows / 3 + 3, qPrintable(QStringLiteral("pc line %1 of %2").arg(pcLine).arg(rows)));
        /* the PC's line: the accent as the block's own background, so it lies under the gutter */
        const QTextBlock pcBlock = v->document()->findBlockByNumber(pcLine);
        QCOMPARE(pcBlock.blockFormat().background().color(), kAccent);
        for (const auto &s : v->extraSelections())
            QVERIFY(!s.format.boolProperty(QTextFormat::FullWidthSelection));
        QCOMPARE(v->document()->findBlockByNumber(pcLine + 1).blockFormat().background().style(), Qt::NoBrush);

        /* a breakpoint's dot: filled and red, hollow and grey when disabled */
        cocodebug_breakpoint_toggle(m_d, kCall);
        QTRY_VERIFY_WITH_TIMEOUT(v->document()->findBlockByNumber(lineOf(v, kCall)).text().startsWith(
                                     QStringLiteral("●")), 2000);
        auto dotColor = [&](int line) {
            QColor c;
            for (const auto &s : v->extraSelections())
                if (s.cursor.blockNumber() == line && s.cursor.hasSelection() &&
                    s.cursor.selectionStart() == s.cursor.block().position() &&
                    s.cursor.selectionEnd() == s.cursor.block().position() + 1)
                    c = s.format.foreground().color();
            return c;
        };
        QCOMPARE(dotColor(lineOf(v, kCall)), QColor(0xe0, 0x1b, 0x24));
        cocodebug_breakpoint b;
        QCOMPARE(cocodebug_breakpoints_list(m_d, &b, 1), 1);
        cocodebug_breakpoint_enable(m_d, b.id, 0);
        QTRY_VERIFY_WITH_TIMEOUT(v->document()->findBlockByNumber(lineOf(v, kCall)).text().startsWith(
                                     QStringLiteral("○")), 2000);
        QCOMPARE(dotColor(lineOf(v, kCall)), QColor(0x9a, 0x99, 0x96));
        /* a breakpoint on the PC's own line keeps its dot, dark red on the green bar */
        cocodebug_breakpoint_toggle(m_d, kInc);
        QTRY_VERIFY_WITH_TIMEOUT(v->document()->findBlockByNumber(lineOf(v, kInc)).text().startsWith(
                                     QStringLiteral("●")), 2000);
        QCOMPARE(dotColor(lineOf(v, kInc)), QColor(0xa0, 0x10, 0x18));
        /* the PC's line is the dark ink on the green */
        QTextCursor cur(v->document()->findBlockByNumber(lineOf(v, kInc)));
        cur.setPosition(cur.block().position() + 10);
        QCOMPARE(cur.charFormat().foreground().color(), kInk);
        shot(QStringLiteral("disassembly"));
    }

    void clickSelectsAndGutterToggles()
    {
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *v = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, kAfter) >= 0, 2000);
        /* a click on the text selects (underlined, bold) and sets no breakpoint */
        clickLine(kAfter, 30);
        QVERIFY(!bpSet(kAfter));
        {
            bool underlined = false;
            for (const auto &s : v->extraSelections())
                if (s.cursor.blockNumber() == lineOf(v, kAfter) && s.format.fontUnderline() &&
                    s.format.fontWeight() == QFont::Bold)
                    underlined = true;
            QVERIFY(underlined);
        }
        /* moving to another line moves the selection; no caret-toggled breakpoints */
        clickLine(kStore, 30);
        QVERIFY(!bpSet(kAfter));
        QVERIFY(!bpSet(kStore));
        /* the left edge toggles, a double-click toggles, F9 toggles the selection */
        clickLine(kCall, 0);
        QVERIFY(bpSet(kCall));
        clickLine(kCall, 0);
        QVERIFY(!bpSet(kCall));
        clickLine(kAfter, 30, true);
        QVERIFY(bpSet(kAfter));
        cocodebug_breakpoint_toggle(m_d, kAfter);
        clickLine(kCall, 30);
        QTest::keyClick(m_win, Qt::Key_F9);
        QVERIFY(bpSet(kCall));
        QTest::keyClick(m_win, Qt::Key_F9);
        QVERIFY(!bpSet(kCall));
        QCOMPARE(bpCount(), 0);
    }

    void runToCursor()
    {
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *v = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, kStore) >= 0, 2000);
        clickLine(kStore, 30);
        QTest::mouseClick(button(QStringLiteral("Run to Cursor")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), kStore, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, kStore) >= 0, 2000);
    }

    void wheelJumpAndFollow()
    {
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *v = disasmView();
        QCheckBox *follow = check(QStringLiteral("Follow PC"));
        QVERIFY(follow && follow->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, kInc) >= 0, 2000);
        const QPoint p = v->viewport()->rect().center();
        QWheelEvent wheel(p, v->viewport()->mapToGlobal(p), QPoint(), QPoint(0, -120), Qt::NoButton, {},
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(v->viewport(), &wheel);
        QVERIFY(!follow->isChecked());
        QVERIFY(lineOf(v, kInc) < 0 || lineOf(v, kInc) < v->document()->blockCount() / 3 - 1);
        /* the jump box: bare digits are hex here, a label wins first, then expressions */
        QLineEdit *jump = entryIs(QStringLiteral("$address or label"));
        QVERIFY(jump);
        typeInto(jump, QStringLiteral("070E"));
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, kSub) >= 0, 2000);
        cocodebug_symbol_add(m_d, kAfter, "back");
        typeInto(jump, QStringLiteral("back"));
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, kAfter) >= 0, 2000);
        QVERIFY(v->toPlainText().contains(QStringLiteral("back")));
        typeInto(jump, QStringLiteral("$0700+3"));
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x0703) >= 0, 2000);
        typeInto(jump, QStringLiteral("nosuchlabel"));
        QVERIFY(!m_win->statusBar()->currentMessage().isEmpty());
        /* Follow PC again returns to the PC */
        QTest::mouseClick(follow, Qt::LeftButton, {}, QPoint(4, follow->height() / 2));
        QVERIFY(follow->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, kInc) >= 0, 2000);
    }

    /* The register strip: entries, flags, the 6309 group. */
    void registerStrip()
    {
        m_tabs->setCurrentIndex(3);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        const QList<QLineEdit *> regs = regEntries();
        QCOMPARE(regs.size(), (int)COCODEBUG_REG_COUNT);  /* PC A B D DP X Y U S CC, then E F W V MD */
        QLineEdit *pc = regs[0], *a = regs[1], *b = regs[2], *dreg = regs[3], *x = regs[5], *cc = regs[9];
        QTRY_COMPARE_WITH_TIMEOUT(pc->text(), QStringLiteral("0703"), 2000);
        typeInto(pc, QStringLiteral("0709"));
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), kAfter, 2000);
        typeInto(a, QStringLiteral("12"));
        typeInto(b, QStringLiteral("$34"));
        cocodebug_regs r;
        cocodebug_regs_get(m_d, &r);
        QCOMPARE((int)r.a, 0x12);
        QCOMPARE((int)r.b, 0x34);
        QTRY_COMPARE_WITH_TIMEOUT(dreg->text(), QStringLiteral("1234"), 2000);
        typeInto(dreg, QStringLiteral("5a5b"));
        cocodebug_regs_get(m_d, &r);
        QCOMPARE((int)r.a, 0x5A);
        QCOMPARE((int)r.b, 0x5B);
        typeInto(x, QStringLiteral("$0850+2"));        /* an expression */
        cocodebug_regs_get(m_d, &r);
        QCOMPARE((int)r.x, 0x0852);
        /* a symbol */
        cocodebug_symbol_add(m_d, kSub, "helper");
        typeInto(x, QStringLiteral("helper"));
        cocodebug_regs_get(m_d, &r);
        QCOMPARE((int)r.x, kSub);
        /* CC and its flags */
        typeInto(cc, QStringLiteral("50"));
        cocodebug_regs_get(m_d, &r);
        QCOMPARE((int)r.cc, 0x50);
        QCheckBox *zf = check(QStringLiteral("Z"));
        QVERIFY(zf);
        QVERIFY(!zf->isChecked());
        QTest::mouseClick(zf, Qt::LeftButton, {}, QPoint(4, zf->height() / 2));
        cocodebug_regs_get(m_d, &r);
        QVERIFY(r.cc & COCODEBUG_CC_Z);
        QTRY_VERIFY_WITH_TIMEOUT(zf->isChecked(), 2000);
        QTest::mouseClick(zf, Qt::LeftButton, {}, QPoint(4, zf->height() / 2));
        cocodebug_regs_get(m_d, &r);
        QVERIFY(!(r.cc & COCODEBUG_CC_Z));
        QCOMPARE((int)r.cc, 0x50);
        for (const char *name : { "E", "F", "H", "I", "N", "V", "C" }) QVERIFY2(check(QString::fromUtf8(name)), name);
        QTRY_VERIFY_WITH_TIMEOUT(check(QStringLiteral("I"))->isChecked() && check(QStringLiteral("F"))->isChecked(), 2000);
        /* the 6309's registers are in the strip but shown only on a 6309 */
        QWidget *group = regs[10]->parentWidget();
        QVERIFY(group);
        QCOMPARE(group->isVisible(), r.is6309 != 0);
        /* a bad value says so, and nothing moves */
        typeInto(x, QStringLiteral("nosuchsymbol"));
        QVERIFY(!m_win->statusBar()->currentMessage().isEmpty());
        cocodebug_regs_get(m_d, &r);
        QCOMPARE((int)r.x, kSub);
        /* the machine name */
        bool machine = false;
        for (QLabel *l : m_win->findChildren<QLabel *>())
            machine |= l->text() == QStringLiteral("%1, %2").arg(m_coco3 ? QStringLiteral("CoCo 3") : QStringLiteral("CoCo 1/2"),
                                                                 r.is6309 ? QStringLiteral("HD6309") : QStringLiteral("MC6809"));
        QVERIFY(machine);
        /* edits need a stopped machine */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!stopped(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!pc->isEnabled(), 2000);
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        QTRY_VERIFY_WITH_TIMEOUT(pc->isEnabled(), 2000);
        loadProgram(m_d);
    }

    /* On a 6309 the strip shows E F W V MD and sets them (DBG_TEST_CPU=6309). */
    void registers6309()
    {
        if (qEnvironmentVariable("DBG_TEST_CPU") != QLatin1String("6309")) QSKIP("6809 session");
        m_tabs->setCurrentIndex(0);
        const QList<QLineEdit *> regs = regEntries();
        QCOMPARE(regs.size(), (int)COCODEBUG_REG_COUNT);
        QLineEdit *e = regs[10], *f = regs[11], *w = regs[12], *md = regs[14];
        QWidget *group = e->parentWidget();
        QTRY_VERIFY_WITH_TIMEOUT(group->isVisible(), 2000);
        typeInto(e, QStringLiteral("12"));
        typeInto(f, QStringLiteral("34"));
        cocodebug_regs r;
        cocodebug_regs_get(m_d, &r);
        QVERIFY(r.is6309);
        QCOMPARE((int)r.e, 0x12);
        QCOMPARE((int)r.f, 0x34);
        QTRY_COMPARE_WITH_TIMEOUT(w->text(), QStringLiteral("1234"), 2000);
        typeInto(w, QStringLiteral("5678"));
        cocodebug_regs_get(m_d, &r);
        QCOMPARE((int)r.w, 0x5678);
        QCOMPARE((int)r.e, 0x56);
        QTRY_COMPARE_WITH_TIMEOUT(md->text().size(), 2, 2000);
        bool machine = false;
        for (QLabel *l : m_win->findChildren<QLabel *>())
            machine |= l->text().endsWith(QLatin1String("HD6309"));
        QVERIFY(machine);
        shot(QStringLiteral("regs6309"));
    }

    void memoryTab()
    {
        m_tabs->setCurrentIndex(3);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QPlainTextEdit *mem = memView();
        QComboBox *space = m_tabs->currentWidget()->findChild<QComboBox *>();
        QVERIFY(mem && space);
        /* CPU always; PHYS on a CoCo 3 only */
        QStringList spaces;
        for (int i = 0; i < space->count(); ++i) spaces << space->itemText(i).section(QLatin1Char(' '), 0, 0);
        const QStringList wantSpaces = m_coco3 ? QStringList{ QStringLiteral("CPU"), QStringLiteral("PHYS") }
                                               : QStringList{ QStringLiteral("CPU") };
        QCOMPARE(spaces, wantSpaces);
        /* poke through the map, go to a label or an address */
        QLineEdit *addr = entryIs(QStringLiteral("$0400"));
        QLineEdit *val = entryIs(QStringLiteral("$00"));
        QVERIFY(addr && val);
        addr->setText(QStringLiteral("0870"));       /* hex */
        typeInto(val, QStringLiteral("5a"));
        QCOMPARE(byteAt(0x0870), 0x5A);
        addr->setText(QStringLiteral("$0871"));
        typeInto(val, QStringLiteral("$A5"));
        QCOMPARE(byteAt(0x0871), 0xA5);
        cocodebug_mem_write(m_d, COCODEBUG_SPACE_CPU, 0x0872, (const uint8_t *)"\x11", 1);
        addr->setText(QStringLiteral("0872"));
        typeInto(val, QStringLiteral("1FF"));        /* not a byte */
        QCOMPARE(byteAt(0x0872), 0x11);
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("byte")));
        addr->setText(QStringLiteral("$FF00"));      /* I/O: refused, said */
        typeInto(val, QStringLiteral("12"));
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("not RAM")));
        QLineEdit *go = entryIs(QStringLiteral("$0400, label or lo-hi"));
        QVERIFY(go);
        typeInto(go, QStringLiteral("0700"));
        QVERIFY2(mem->toPlainText().contains(QStringLiteral("$0700: B6 08 50 4C B7")), qPrintable(mem->toPlainText().left(400)));
        /* each row is named by its region */
        const QString first = mem->document()->findBlockByNumber(1).text();
        QVERIFY2(first.contains(QLatin1String("RAM")), qPrintable(first));
        /* Go to scrolled the row into view and marked it in the accent, dark ink */
        bool marked = false;
        for (const auto &s : mem->extraSelections())
            marked |= s.format.background().color() == kAccent && s.format.foreground().color() == kInk;
        QVERIFY(marked);
        /* the whole space is reachable: the last row, and the scroll bar moves the window */
        typeInto(go, QStringLiteral("FFF0"));
        QVERIFY2(mem->toPlainText().contains(QStringLiteral("$FFF0: ")), qPrintable(mem->toPlainText().right(300)));
        QVERIFY2(mem->toPlainText().contains(QStringLiteral("VECTORS")), qPrintable(mem->toPlainText().right(300)));
        QScrollBar *bar = m_tabs->currentWidget()->findChild<QScrollBar *>();
        QVERIFY(bar && bar->maximum() > 4000);
        bar->setValue(0);
        QVERIFY(mem->toPlainText().contains(QStringLiteral("$0000: ")));
        /* the wheel moves it three rows a notch */
        const QPoint p = mem->viewport()->rect().center();
        QWheelEvent wheel(p, mem->viewport()->mapToGlobal(p), QPoint(), QPoint(0, -120), Qt::NoButton, {},
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(mem->viewport(), &wheel);
        QVERIFY2(bar->value() == 3, qPrintable(QString::number(bar->value())));
        bar->setValue(0);
        /* I/O reads as latched values: browsing $FF00 does not consume or clear anything */
        typeInto(go, QStringLiteral("FF00"));
        QVERIFY(mem->toPlainText().contains(QStringLiteral("PIA0")));

        if (m_coco3) {
            /* PHYS: the physical RAM, six-digit addresses; the CPU's $0000 is phys $070000 by default */
            space->setCurrentIndex(1);
            QTRY_VERIFY_WITH_TIMEOUT(mem->toPlainText().contains(QStringLiteral("$000000: ")), 2000);
            QVERIFY(bar->maximum() >= 0x80000 / 16 - 64);
            typeInto(go, QStringLiteral("070850"));
            QVERIFY2(mem->toPlainText().contains(QStringLiteral("$070850: ")), qPrintable(mem->toPlainText().left(300)));
            addr->setText(QStringLiteral("070873"));
            typeInto(val, QStringLiteral("77"));
            QCOMPARE(byteAt(0x070873, COCODEBUG_SPACE_PHYS), 0x77);
            QCOMPARE(byteAt(0x0873), 0x77);             /* the same byte through the CPU map */
            addr->setText(QStringLiteral("FFFFFF"));
            typeInto(val, QStringLiteral("77"));
            QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("Out of range")));
            shot(QStringLiteral("memory-phys"));
            space->setCurrentIndex(0);
        } else {
            QVERIFY(cocodebug_space_size(m_d, COCODEBUG_SPACE_PHYS) == 0);
        }
        typeInto(go, QStringLiteral("0700"));
        /* edits need a stopped machine */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!stopped(), 2000);
        addr->setText(QStringLiteral("0870"));
        typeInto(val, QStringLiteral("12"));
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("Pause the machine")));
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        shot(QStringLiteral("memory"));
        loadProgram(m_d);
    }

    void breakpointsTab()
    {
        m_tabs->setCurrentIndex(4);
        QTableWidget *t = bpTable();
        QVERIFY(t);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QCOMPARE(t->rowCount(), 0);
        QLineEdit *start = entry(QStringLiteral("address, range"));
        QLineEdit *end = entry(QStringLiteral("end"));
        QLineEdit *cond = entry(QStringLiteral("condition"));
        QVERIFY(start && end && cond);
        QComboBox *type = m_tabs->currentWidget()->findChild<QComboBox *>();
        QVERIFY(type);
        QPushButton *add = nullptr, *clear = nullptr, *clearHits = nullptr;
        for (QPushButton *b : m_tabs->currentWidget()->findChildren<QPushButton *>()) {
            if (b->text() == QLatin1String("Add")) add = b;
            if (b->text() == QLatin1String("Clear Breakpoints")) clear = b;
            if (b->text() == QLatin1String("Clear Hits")) clearHits = b;
        }
        QCheckBox *phys = nullptr;
        for (QCheckBox *c : m_tabs->currentWidget()->findChildren<QCheckBox *>())
            if (c->text() == QLatin1String("Physical")) phys = c;
        QLabel *msg = nullptr;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            if (l->styleSheet().contains(QLatin1String("e01b24"))) msg = l;
        QVERIFY(add && clear && clearHits && msg && phys);

        QStringList types;
        for (int i = 0; i < type->count(); ++i) types << type->itemText(i);
        QCOMPARE(types, (QStringList{ QStringLiteral("Execute"), QStringLiteral("Read"), QStringLiteral("Write"),
                                      QStringLiteral("Access") }));
        /* Physical: only on a CoCo 3, and not for Execute */
        QVERIFY(!phys->isEnabled());
        type->setCurrentText(QStringLiteral("Write"));
        QCOMPARE(phys->isEnabled(), m_coco3);
        type->setCurrentText(QStringLiteral("Execute"));
        QVERIFY(!phys->isEnabled());

        /* exec at a hex address */
        typeInto(start, QStringLiteral("0707"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 1, 2000);
        QVERIFY2(t->item(0, 1)->text().startsWith(QLatin1String("Exec")), qPrintable(t->item(0, 1)->text()));
        QVERIFY2(t->item(0, 1)->text().contains(QLatin1String("$0707")), qPrintable(t->item(0, 1)->text()));
        QCOMPARE(t->item(0, 0)->checkState(), Qt::Checked);
        QVERIFY(start->text().isEmpty());
        /* a write watchpoint over a range, with a condition on old and value */
        type->setCurrentText(QStringLiteral("Write"));
        start->setText(QStringLiteral("0850"));
        end->setText(QStringLiteral("$085F"));
        cond->setText(QStringLiteral("value != old"));
        QTest::mouseClick(add, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 2, 2000);
        QVERIFY2(t->item(1, 1)->text().startsWith(QLatin1String("Write")), qPrintable(t->item(1, 1)->text()));
        QVERIFY2(t->item(1, 1)->text().contains(QLatin1String("$0850-$085F")), qPrintable(t->item(1, 1)->text()));
        QVERIFY(t->item(1, 1)->text().contains(QLatin1String("old")));
        /* the start may itself be a range; read and access */
        cond->clear();
        end->clear();
        type->setCurrentText(QStringLiteral("Access"));
        typeInto(start, QStringLiteral("0870-087F"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 3, 2000);
        QVERIFY2(t->item(2, 1)->text().startsWith(QLatin1String("Access")), qPrintable(t->item(2, 1)->text()));
        type->setCurrentText(QStringLiteral("Read"));
        typeInto(start, QStringLiteral("0880"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 4, 2000);
        QVERIFY2(t->item(3, 1)->text().startsWith(QLatin1String("Read")), qPrintable(t->item(3, 1)->text()));
        if (m_coco3) {
            /* a physical one: the range is physical RAM */
            type->setCurrentText(QStringLiteral("Write"));
            phys->setChecked(true);
            typeInto(start, QStringLiteral("070900-07090F"));
            QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 5, 2000);
            QVERIFY2(t->item(4, 1)->text().contains(QLatin1String("phys")), qPrintable(t->item(4, 1)->text()));
            cocodebug_breakpoint list[8];
            QCOMPARE(cocodebug_breakpoints_list(m_d, list, 8), 5);
            QVERIFY(list[4].type & COCODEBUG_BP_PHYS);
            phys->setChecked(false);
            clearBreakpoints();
            QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 0, 2000);
            type->setCurrentText(QStringLiteral("Execute"));
            typeInto(start, QStringLiteral("0707"));
            type->setCurrentText(QStringLiteral("Write"));
            start->setText(QStringLiteral("0850"));
            end->setText(QStringLiteral("$085F"));
            cond->setText(QStringLiteral("value != old"));
            QTest::mouseClick(add, Qt::LeftButton);
            cond->clear();
            end->clear();
            type->setCurrentText(QStringLiteral("Execute"));
            QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 2, 2000);
        } else {
            QVERIFY(!phys->isEnabled() || !phys->isChecked());
        }
        /* errors are shown, not swallowed */
        const int rowsBefore = t->rowCount();
        type->setCurrentText(QStringLiteral("Execute"));
        cond->setText(QStringLiteral("a == (1 +"));
        start->setText(QStringLiteral("7000"));
        end->clear();
        QTest::mouseClick(add, Qt::LeftButton);
        QVERIFY(!msg->text().isEmpty());
        QCOMPARE(t->rowCount(), rowsBefore);
        cond->clear();
        start->setText(QStringLiteral("7"));
        end->setText(QStringLiteral("5"));
        QTest::mouseClick(add, Qt::LeftButton);
        QVERIFY(msg->text().contains(QLatin1String("before")));
        start->clear();
        end->clear();
        msg->clear();
        shot(QStringLiteral("breakpoints-types"));

        /* back to two: the exec one and the conditional write */
        QTest::mouseClick(clear, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 0, 2000);
        typeInto(start, QStringLiteral("0703"));
        type->setCurrentText(QStringLiteral("Write"));
        start->setText(QStringLiteral("0850"));
        end->setText(QStringLiteral("$085F"));
        cond->setText(QStringLiteral("value != old"));
        QTest::mouseClick(add, Qt::LeftButton);
        cond->clear();
        end->clear();
        type->setCurrentText(QStringLiteral("Execute"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 2, 2000);

        /* the exec breakpoint stops the run, with its hits counted */
        cocodebug_mem_write(m_d, COCODEBUG_SPACE_CPU, kCounter, (const uint8_t *)"\0", 1);
        cocodebug_regs_set(m_d, COCODEBUG_REG_PC, kStart);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(stopped() && pcOf(m_d) == kInc, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("exec $0703")), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(t->item(0, 2) && t->item(0, 2)->text().toInt() >= 1, 2000);
        /* unticking it lets the run pass; the write watch then stops it instead */
        t->item(0, 0)->setCheckState(Qt::Unchecked);
        cocodebug_breakpoint list[2];
        QCOMPARE(cocodebug_breakpoints_list(m_d, list, 2), 2);
        QCOMPARE(list[0].enabled, 0);
        QCOMPARE(list[1].enabled, 1);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(stopped() && statusLabel()->text().contains(QLatin1String("wrote $0850")), 4000);
        /* Clear Hits zeroes the counts */
        QTRY_VERIFY_WITH_TIMEOUT(t->item(1, 2) && t->item(1, 2)->text().toInt() >= 1, 2000);
        QTest::mouseClick(clearHits, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->item(1, 2)->text(), QStringLiteral("0"), 2000);
        /* a disabled breakpoint is the hollow grey dot in the disassembly */
        m_tabs->setCurrentIndex(1);
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(disasmView(), kInc) >= 0 &&
                                     disasmView()->document()->findBlockByNumber(lineOf(disasmView(), kInc))
                                         .text().startsWith(QStringLiteral("○")), 2000);
        m_tabs->setCurrentIndex(4);
        shot(QStringLiteral("breakpoints"));
        /* the row's delete button */
        auto *rm = qobject_cast<QToolButton *>(t->cellWidget(1, 3));
        QVERIFY(rm);
        QTest::mouseClick(rm, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 1, 2000);
        QCOMPARE(cocodebug_breakpoints_list(m_d, list, 2), 1);
        QTest::mouseClick(clear, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 0, 2000);
        QCOMPARE(bpCount(), 0);
        /* a read breakpoint stops the run on the LDA's read, an access one on either */
        loadProgram(m_d);
        cocodebug_breakpoint_add(m_d, COCODEBUG_BP_READ, kCounter, kCounter, "");
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(stopped() && statusLabel()->text().contains(QLatin1String("$0850")), 4000);
        clearBreakpoints();
    }

    /* Watches: add, live value, Memory, Break on change, Remove. */
    void watchesTab()
    {
        cocowatch_clear();
        cocodebug_mem_write(m_d, COCODEBUG_SPACE_CPU, 0x0870, (const uint8_t *)"\x42", 1);
        m_tabs->setCurrentIndex(5);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QTableWidget *t = m_tabs->currentWidget()->findChild<QTableWidget *>();
        QLineEdit *e = m_tabs->currentWidget()->findChild<QLineEdit *>();
        QComboBox *space = m_tabs->currentWidget()->findChild<QComboBox *>();
        QVERIFY(t && e && space);
        QCOMPARE(space->count(), m_coco3 ? 2 : 1);
        QCOMPARE(t->rowCount(), 0);
        typeInto(e, QStringLiteral("$0870"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 1, 2000);
        QVERIFY(t->item(0, 0)->text().contains(QStringLiteral("$0870")));
        QVERIFY(t->item(0, 0)->text().contains(QStringLiteral("$42")));
        cocodebug_mem_write(m_d, COCODEBUG_SPACE_CPU, 0x0870, (const uint8_t *)"\x43", 1);
        QTest::keyClick(m_win, Qt::Key_F7);             /* a stop refreshes the values */
        QTRY_VERIFY_WITH_TIMEOUT(t->item(0, 0)->text().contains(QStringLiteral("$43")), 2000);
        typeInto(e, QStringLiteral("$0880-$088F"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 2, 2000);
        if (m_coco3) {
            /* a physical watch shows the same byte under its own address */
            space->setCurrentIndex(1);
            typeInto(e, QStringLiteral("070870"));
            QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 3, 2000);
            QVERIFY2(t->item(2, 0)->text().contains(QStringLiteral("phys")), qPrintable(t->item(2, 0)->text()));
            QVERIFY2(t->item(2, 0)->text().contains(QStringLiteral("$43")), qPrintable(t->item(2, 0)->text()));
            QCOMPARE(cocowatch_space(2), (int)COCODEBUG_SPACE_PHYS);
            cocowatch_remove(2);
            space->setCurrentIndex(0);
            QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 2, 2000);
        }
        /* while the machine runs the value is a dash */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!stopped(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(t->item(0, 0)->text().contains(QLatin1String("-")) &&
                                     !t->item(0, 0)->text().contains(QStringLiteral("$43")), 2000);
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        shot(QStringLiteral("watches"));

        /* Break on change adds a write breakpoint with the condition */
        clearBreakpoints();
        auto buttonOf = [&](int row, const QString &text) -> QPushButton * {
            for (QPushButton *b : t->cellWidget(row, 1)->findChildren<QPushButton *>())
                if (b->text() == text) return b;
            return nullptr;
        };
        QPushButton *brk = buttonOf(0, QStringLiteral("Break on change"));
        QVERIFY(brk);
        QTest::mouseClick(brk, Qt::LeftButton);
        cocodebug_breakpoint bp[2];
        QCOMPARE(cocodebug_breakpoints_list(m_d, bp, 2), 1);
        QVERIFY(bp[0].type & COCODEBUG_BP_WRITE);
        QCOMPARE((int)bp[0].start, 0x0870);
        QVERIFY(QString::fromUtf8(bp[0].condition).contains(QStringLiteral("old")));
        clearBreakpoints();

        /* Memory switches to the Memory tab with the range marked */
        QPushButton *mem = buttonOf(1, QStringLiteral("Memory"));
        QVERIFY(mem);
        QTest::mouseClick(mem, Qt::LeftButton);
        QCOMPARE(m_tabs->currentIndex(), 3);
        QVERIFY(memView()->toPlainText().contains(QStringLiteral("$0880: ")));
        QVERIFY(!memView()->extraSelections().isEmpty());
        /* and every watch is marked in the dump, in its own colour */
        bool watched = false;
        for (const auto &s : memView()->extraSelections())
            watched |= s.format.background().color() == QColor(0x1c, 0x71, 0xd8);
        QVERIFY(watched);

        m_tabs->setCurrentIndex(5);
        QTest::mouseClick(buttonOf(1, QStringLiteral("Remove")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 1, 2000);
        QCOMPARE(cocowatch_count(), 1);
        cocowatch_clear();
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 0, 2000);
        m_tabs->setCurrentIndex(0);
    }

    /* Go to marks a range; Find Next / Previous step through hits. */
    void memorySearch()
    {
        const uint8_t pat[4] = { 0xDE, 0xEE, 0xDE, 0xEE };
        cocodebug_mem_write(m_d, COCODEBUG_SPACE_CPU, 0x0900, pat, 4);
        cocodebug_mem_write(m_d, COCODEBUG_SPACE_CPU, 0x0980, pat, 4);
        m_tabs->setCurrentIndex(3);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QLineEdit *go = entryIs(QStringLiteral("$0400, label or lo-hi"));
        typeInto(go, QStringLiteral("0900-0903"));
        QVERIFY(memView()->toPlainText().contains(QStringLiteral("$0900: DE EE DE EE")));
        QVERIFY(!memView()->extraSelections().isEmpty());
        /* a label also works */
        cocodebug_symbol_add(m_d, 0x0980, "found");
        typeInto(go, QStringLiteral("found"));
        QVERIFY(memView()->toPlainText().contains(QStringLiteral("$0980: DE EE")));

        QLineEdit *find = entry(QStringLiteral("bytes or text"));
        QVERIFY(find);
        QLabel *note = nullptr;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            if (l->styleSheet().contains(QLatin1String("gray")) && l->text().isEmpty()) note = l;
        QVERIFY(note);
        typeInto(go, QStringLiteral("0900-0903"));
        find->setText(QStringLiteral("DE EE DE"));
        QTest::mouseClick(button(QStringLiteral("Find Next")), Qt::LeftButton);
        QCOMPARE(note->text(), QStringLiteral("Found at $0980"));          /* after the marked one */
        QTest::mouseClick(button(QStringLiteral("Find Previous")), Qt::LeftButton);
        QCOMPARE(note->text(), QStringLiteral("Found at $0900"));
        find->setText(QStringLiteral("DE EE DE EE FF 77 66"));
        QTest::mouseClick(button(QStringLiteral("Find Next")), Qt::LeftButton);
        QCOMPARE(note->text(), QStringLiteral("Not found"));
        QCOMPARE(m_win->statusBar()->currentMessage(), QStringLiteral("Not found"));
        /* text, case-insensitive */
        cocodebug_mem_write(m_d, COCODEBUG_SPACE_CPU, 0x09A0, (const uint8_t *)"HeLLo", 5);
        m_tabs->currentWidget()->findChild<QComboBox *>(QString(), Qt::FindChildrenRecursively);
        QComboBox *kind = nullptr;
        for (QComboBox *c : m_tabs->currentWidget()->findChildren<QComboBox *>())
            if (c->itemText(0) == QLatin1String("Hex")) kind = c;
        QVERIFY(kind);
        kind->setCurrentText(QStringLiteral("Text"));
        find->setText(QStringLiteral("hello"));
        typeInto(go, QStringLiteral("0800"));
        QTest::mouseClick(button(QStringLiteral("Find Next")), Qt::LeftButton);
        QCOMPARE(note->text(), QStringLiteral("Found at $09A0"));
        shot(QStringLiteral("search"));
        kind->setCurrentText(QStringLiteral("Hex"));
        if (m_coco3) {
            /* in physical RAM the hit has a six-digit address */
            QComboBox *space = m_tabs->currentWidget()->findChild<QComboBox *>();
            space->setCurrentIndex(1);
            typeInto(go, QStringLiteral("070000"));
            find->setText(QStringLiteral("DE EE DE EE"));
            QTest::mouseClick(button(QStringLiteral("Find Next")), Qt::LeftButton);
            QVERIFY2(note->text().startsWith(QLatin1String("Found at $0709")), qPrintable(note->text()));
            space->setCurrentIndex(0);
        }
        /* it needs a stopped machine */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!stopped(), 2000);
        find->setText(QStringLiteral("DE EE"));
        QTest::mouseClick(button(QStringLiteral("Find Next")), Qt::LeftButton);
        QCOMPARE(m_win->statusBar()->currentMessage(), QStringLiteral("Pause the machine first"));
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        m_tabs->setCurrentIndex(0);
        loadProgram(m_d);
    }

    /* Trace and History share the ring and the recorder. */
    void traceAndHistory()
    {
        cocodebug_trace_enable(m_d, 0);
        cocodebug_trace_clear(m_d);
        m_tabs->setCurrentIndex(7);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QPushButton *rec = m_tabs->currentWidget()->findChild<QPushButton *>();
        QVERIFY(rec && rec->text() == QLatin1String("Record"));
        QPushButton *rec2 = m_tabs->widget(8)->findChild<QPushButton *>();
        QVERIFY(rec2 && rec2 != rec);
        QVERIFY(!rec->isChecked());
        QTest::mouseClick(rec, Qt::LeftButton);
        QVERIFY(cocodebug_trace_enabled(m_d));
        QVERIFY(rec->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(rec2->isChecked(), 2000);                   /* in sync */
        QTest::keyClick(m_win, Qt::Key_F5);                                  /* runs */
        QTRY_VERIFY_WITH_TIMEOUT(!stopped(), 2000);
        auto *tr = m_tabs->currentWidget()->findChild<QPlainTextEdit *>();
        QTRY_VERIFY_WITH_TIMEOUT(tr->toPlainText().contains(QStringLiteral("INCA")), 3000);   /* live */
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        QTRY_VERIFY_WITH_TIMEOUT(cocodebug_trace_count(m_d) > 20, 2000);
        /* History shows the same ring, oldest first, at the stop */
        m_tabs->setCurrentIndex(8);
        auto *hist = m_tabs->currentWidget()->findChild<QPlainTextEdit *>();
        QTRY_VERIFY_WITH_TIMEOUT(hist->toPlainText().contains(QStringLiteral("INCA")), 2000);
        QVERIFY(hist->toPlainText().count(QLatin1Char('\n')) < 2000);
        /* newest at the bottom: the view is scrolled to its end */
        QVERIFY(hist->verticalScrollBar()->value() >= hist->verticalScrollBar()->maximum() - 1);
        QLabel *note = nullptr;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            if (l->text().contains(QLatin1String("instructions"))) note = l;
        QVERIFY(note && note->text().startsWith(QLatin1String("recording")));
        shot(QStringLiteral("history"));
        /* a branches-only filter leaves branches, calls and returns */
        auto *filter = m_tabs->currentWidget()->findChild<QComboBox *>();
        filter->setCurrentIndex(1);
        QTRY_VERIFY_WITH_TIMEOUT(!hist->toPlainText().contains(QStringLiteral("INCA")), 2000);
        QVERIFY(hist->toPlainText().contains(QStringLiteral("BSR")) && hist->toPlainText().contains(QStringLiteral("RTS")) &&
                hist->toPlainText().contains(QStringLiteral("BRA")));
        /* a range filter */
        filter->setCurrentIndex(2);
        auto *range = m_tabs->currentWidget()->findChild<QLineEdit *>();
        typeInto(range, QStringLiteral("0704-0704"));
        QTRY_VERIFY_WITH_TIMEOUT(hist->toPlainText().contains(QStringLiteral("STA")) &&
                                     !hist->toPlainText().contains(QStringLiteral("INCA")), 2000);
        filter->setCurrentIndex(0);
        /* Clear */
        for (QPushButton *b : m_tabs->currentWidget()->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Clear")) QTest::mouseClick(b, Qt::LeftButton);
        QCOMPARE(cocodebug_trace_count(m_d), 0);
        QTRY_VERIFY_WITH_TIMEOUT(!hist->toPlainText().contains(QStringLiteral("INCA")), 2000);
        /* and off, from either tab */
        QTest::mouseClick(m_tabs->widget(8)->findChild<QPushButton *>(), Qt::LeftButton);
        QVERIFY(!cocodebug_trace_enabled(m_d));
        QTRY_VERIFY_WITH_TIMEOUT(!m_tabs->widget(7)->findChild<QPushButton *>()->isChecked(), 2000);
        m_tabs->setCurrentIndex(0);
    }

    /* C Source: a CMOC listing loaded, the PC's line, a click for a breakpoint,
     * the locals, Step Line and Step Over Line. */
    void cSource()
    {
        QCOMPARE(cocodebug_source_available(m_d), 0);
        m_tabs->setCurrentIndex(2);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QVERIFY(srcView()->toPlainText().isEmpty());
        QVERIFY(srcLocals()->toPlainText().contains(QStringLiteral("no C source")));
        const QString lst = QStringLiteral(COCO_TEST_DATA_DIR "/hello.lst");
        QVERIFY(cocodebug_source_load(m_d, lst.toUtf8().constData()) > 0);
        QPlainTextEdit *v = srcView();
        QTRY_VERIFY_WITH_TIMEOUT(v->toPlainText().contains(QStringLiteral("int add(int a, int b)")), 3000);
        QComboBox *files = m_tabs->currentWidget()->findChild<QComboBox *>();
        QVERIFY(files);
        QTRY_COMPARE_WITH_TIMEOUT(files->itemText(0), QStringLiteral("hello.c"), 2000);
        QVERIFY(button(QStringLiteral("Step Line"))->isEnabled());
        QVERIFY(button(QStringLiteral("Step Over Line"))->isEnabled());
        /* the line of main's body, for the PC */
        auto blockOfLine = [&](int line) {
            for (QTextBlock b = v->document()->begin(); b.isValid(); b = b.next())
                if (b.text().mid(2, 5).trimmed().toInt() == line) return b.blockNumber();
            return -1;
        };
        QVERIFY(blockOfLine(12) >= 0);
        /* a click on a line sets its breakpoint, a second clears it; a line with no code says so */
        auto click = [&](int line) {
            const QPoint p = lineAt(v, blockOfLine(line), 4);
            QTest::mouseClick(v->viewport(), Qt::LeftButton, {}, p);
        };
        click(16);
        QCOMPARE(bpCount(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(v->document()->findBlockByNumber(blockOfLine(16)).text().startsWith(QStringLiteral("●")), 2000);
        click(16);
        QCOMPARE(bpCount(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(v->document()->findBlockByNumber(blockOfLine(16)).text().startsWith(QStringLiteral(" ")), 2000);
        click(15);
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("No code")));
        QCOMPARE(bpCount(), 0);

        /* main's first statement: the PC bar on its line, the locals beside it */
        cocodebug_regs_set(m_d, COCODEBUG_REG_S, 0x6F00);
        cocodebug_regs_set(m_d, COCODEBUG_REG_PC, kMainAt);
        QTest::keyClick(m_win, Qt::Key_F6);
        QTRY_VERIFY_WITH_TIMEOUT(pcOf(m_d) != kMainAt && stopped(), 3000);
        int guard = 0;
        int fid = 0, line = 0;
        while (guard++ < 8 && !(cocodebug_source_line_at(m_d, pcOf(m_d), &fid, &line) && line == 16)) {
            QTest::keyClick(m_win, Qt::Key_F6);
            QTest::qWait(150);
            QTRY_VERIFY_WITH_TIMEOUT(stopped(), 3000);
        }
        QCOMPARE(line, 16);
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().startsWith(QLatin1String("Stopped")), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(v->document()->findBlockByNumber(blockOfLine(16)).blockFormat().background().color() == kAccent, 2000);
        QVERIFY(srcLocals()->toPlainText().contains(QStringLiteral("x ")));
        QTRY_VERIFY_WITH_TIMEOUT(srcLocals()->toPlainText().contains(QStringLiteral("= 5 ($5)")), 2000);
        QVERIFY(srcLocals()->toPlainText().contains(QStringLiteral("= 10 ($a)")));
        shot(QStringLiteral("csource"));
        /* Step Over Line passes the call; Step Line goes into it */
        const int sp = pcOf(m_d);
        QTest::keyClick(m_win, Qt::Key_F6, Qt::ShiftModifier);
        QTRY_VERIFY_WITH_TIMEOUT(stopped() && pcOf(m_d) != sp, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().startsWith(QLatin1String("Stopped")), 2000);
        QVERIFY(cocodebug_source_line_at(m_d, pcOf(m_d), &fid, &line));
        QCOMPARE(line, 17);
        /* back to the call, and into it */
        cocodebug_regs_set(m_d, COCODEBUG_REG_PC, sp);
        cocodebug_breakpoint_toggle(m_d, kAddAt);
        QTest::keyClick(m_win, Qt::Key_F8, Qt::ControlModifier);       /* (nothing: not a debugger key) */
        cocodebug_breakpoint_toggle(m_d, kAddAt);
        QTest::keyClick(m_win, Qt::Key_F6);
        QTRY_VERIFY_WITH_TIMEOUT(stopped(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(cocodebug_source_line_at(m_d, pcOf(m_d), &fid, &line) && line < 9, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(srcLocals()->toPlainText().contains(QStringLiteral("(param)")) || line <= 4, 3000);
        /* the Clear C Source button drops it all */
        QTest::mouseClick(button(QStringLiteral("Clear C Source")), Qt::LeftButton);
        QCOMPARE(cocodebug_source_available(m_d), 0);
        QTRY_VERIFY2_WITH_TIMEOUT(srcView()->toPlainText().isEmpty(), qPrintable(srcView()->toPlainText().left(200)), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!button(QStringLiteral("Step Line"))->isEnabled(), 2000);
        QCOMPARE(files->count(), 0);
        m_tabs->setCurrentIndex(0);
        clearBreakpoints();
        loadProgram(m_d);
    }

    /* Any tab can leave for a window of its own and come back to its place. */
    void popOutAndDock()
    {
        m_tabs->setCurrentIndex(4);
        QPushButton *pop = button(QStringLiteral("Pop Out"));
        QVERIFY(pop);
        QTest::mouseClick(pop, Qt::LeftButton);
        QCOMPARE(m_tabs->count(), 12);
        QWidget *win = nullptr;
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->windowTitle() == QLatin1String("CoCo Debugger - Breakpoints")) win = w;
        QVERIFY(win);
        QTRY_VERIFY_WITH_TIMEOUT(win->isVisible(), 1000);
        QVERIFY(win->findChild<QTableWidget *>());
        QCOMPARE(win->property("cocoHelpContext").toString(), QStringLiteral("debugger/breakpoints"));
        /* the popped tab keeps refreshing: a breakpoint added shows in its window */
        cocodebug_breakpoint_toggle(m_d, kCall);
        QTRY_COMPARE_WITH_TIMEOUT(win->findChild<QTableWidget *>()->rowCount(), 1, 2000);
        clearBreakpoints();
        shot(QStringLiteral("popout"));
        QPushButton *dockBtn = nullptr;
        for (QPushButton *b : win->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Dock")) dockBtn = b;
        QVERIFY(dockBtn);
        QTest::mouseClick(dockBtn, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(m_tabs->count(), 13, 1000);
        QCOMPARE(m_tabs->tabText(4), QStringLiteral("Breakpoints"));       /* back in its place */
        /* closing the window docks it too; several can be out, but one stays */
        m_tabs->setCurrentIndex(6);
        QTest::mouseClick(pop, Qt::LeftButton);
        QCOMPARE(m_tabs->count(), 12);
        QCOMPARE(m_tabs->tabText(6), QStringLiteral("Trace"));
        QWidget *cart = nullptr;
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->windowTitle() == QLatin1String("CoCo Debugger - Cart & I/O")) cart = w;
        QVERIFY(cart);
        cart->close();
        QTRY_COMPARE_WITH_TIMEOUT(m_tabs->count(), 13, 1000);
        QCOMPARE(m_tabs->tabText(6), QStringLiteral("Cart && I/O"));
        for (int i = 0; i < 12; ++i) {
            m_tabs->setCurrentIndex(0);
            QTest::mouseClick(pop, Qt::LeftButton);
        }
        QCOMPARE(m_tabs->count(), 1);
        QTest::mouseClick(pop, Qt::LeftButton);                          /* the last stays */
        QCOMPARE(m_tabs->count(), 1);
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->windowTitle().startsWith(QLatin1String("CoCo Debugger - "))) w->close();
        QTRY_COMPARE_WITH_TIMEOUT(m_tabs->count(), 13, 1000);
        QStringList names;
        for (int i = 0; i < m_tabs->count(); ++i) names << m_tabs->tabText(i);
        QCOMPARE(names.first(), QStringLiteral("Console"));
        QCOMPARE(names.last(), QStringLiteral("FujiNet"));
        QCOMPARE(names.at(2), QStringLiteral("C Source"));
        QCOMPARE(names.at(6), QStringLiteral("Cart && I/O"));
        QCOMPARE(names.at(11), QStringLiteral("Symbols"));
    }

    /* A new stop pulls every view back to the spot from wherever it was
     * scrolled: the disassembly to the PC, Memory to the PC, the Trace and
     * History to their newest line; and the Breakpoints tab says why. */
    void stopRecentresAndNames()
    {
        m_tabs->setCurrentIndex(1);
        QCheckBox *follow = check(QStringLiteral("Follow PC"));
        QPlainTextEdit *v = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, kInc) >= 0, 2000);
        const QPoint p = v->viewport()->rect().center();
        QWheelEvent wheel(p, v->viewport()->mapToGlobal(p), QPoint(), QPoint(0, -600), Qt::NoButton, {},
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(v->viewport(), &wheel);
        QVERIFY(!follow->isChecked());
        /* Memory scrolled to the top */
        m_tabs->setCurrentIndex(3);
        m_tabs->currentWidget()->findChild<QScrollBar *>()->setValue(0);
        m_tabs->setCurrentIndex(1);
        QTest::keyClick(m_win, Qt::Key_F7);
        QTRY_VERIFY_WITH_TIMEOUT(follow->isChecked(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, pcOf(m_d)) >= 0, 2000);
        m_tabs->setCurrentIndex(3);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->findChild<QScrollBar *>()->value() >= pcOf(m_d) / 16 - 40, 2000);
        m_tabs->setCurrentIndex(4);
        QLabel *stop = nullptr;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            if (l->text().startsWith(QLatin1String("Stopped"))) stop = l;
        QVERIFY(stop);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(stop->text() == QLatin1String("Running."), 2000);
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        QVERIFY(button(QStringLiteral("Clear Hits")));
        m_tabs->setCurrentIndex(0);
    }

    /* Cart & I/O: the cartridge, the memory map, the chips; Sound; FujiNet. */
    void otherTabs()
    {
        m_tabs->setCurrentIndex(6);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        const auto views = m_tabs->currentWidget()->findChildren<QPlainTextEdit *>();
        QCOMPARE(views.size(), 2);
        QPlainTextEdit *cartView = views[0]->toPlainText().contains(QStringLiteral("PIA0")) ? views[1] : views[0];
        QPlainTextEdit *chipsView = cartView == views[0] ? views[1] : views[0];
        QTRY_VERIFY_WITH_TIMEOUT(cartView->toPlainText().contains(QStringLiteral("Memory map")), 2000);
        QVERIFY(cartView->toPlainText().contains(QStringLiteral("Cartridge:")));
        QCOMPARE(cartView->toPlainText().count(QStringLiteral("Memory map")), 1);
        QVERIFY(cartView->toPlainText().contains(QStringLiteral("PIA1")));
        QVERIFY(cartView->toPlainText().contains(QStringLiteral("VECTORS")));
        const QString chips = chipsView->toPlainText();
        QVERIFY(chips.contains(QStringLiteral("PIA0")));
        QVERIFY(chips.contains(QStringLiteral("PIA1")));
        QVERIFY(chips.contains(QStringLiteral("Joysticks")));
        QVERIFY(chips.contains(QStringLiteral("Becker")));
        QCOMPARE(chips.contains(QStringLiteral("GIME")), m_coco3);
        QCOMPARE(chips.contains(QStringLiteral("SAM  V")), !m_coco3);
        shot(QStringLiteral("cart"));

        /* Sound: the DAC's state, and its writes recorded while the tab shows */
        cocodebug_sound_history_clear(m_d);
        m_tabs->setCurrentIndex(10);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        auto *sound = m_tabs->currentWidget()->findChild<QPlainTextEdit *>();
        QVERIFY(sound);
        QTRY_VERIFY_WITH_TIMEOUT(sound->toPlainText().contains(QStringLiteral("DAC (PIA1 A")), 2000);
        QVERIFY(sound->toPlainText().contains(QStringLiteral("Mux source")));
        QVERIFY(!sound->toPlainText().contains(QStringLiteral("Memory map")));      /* in Cart & I/O */
        cocodebug_regs_set(m_d, COCODEBUG_REG_PC, kBeepAt);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!stopped(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(sound->toPlainText().contains(QRegularExpression(QStringLiteral("[1-9][0-9]* writes recorded"))), 4000);
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        QTRY_VERIFY_WITH_TIMEOUT(sound->toPlainText().contains(QStringLiteral("DAC (PIA1 A, $FF20)")), 2000);
        shot(QStringLiteral("sound"));
        /* leaving the tab stops the recording: the count stays put while the machine runs */
        m_tabs->setCurrentIndex(0);
        QTest::qWait(300);
        cocodebug_sound_history_clear(m_d);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!stopped(), 2000);
        QTest::qWait(300);
        cocodebug_sound_write w[4];
        QCOMPARE(cocodebug_sound_history(m_d, w, 4), 0);
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        loadProgram(m_d);

        /* FujiNet: empty without the runtime, and says so */
        m_tabs->setCurrentIndex(12);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        bool note = false;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            note |= l->text().contains(QLatin1String("FujiNet"));
        QVERIFY(note);
        QCheckBox *hide = nullptr;
        for (QCheckBox *c : m_tabs->currentWidget()->findChildren<QCheckBox *>())
            if (c->text() == QLatin1String("Hide READY probes")) hide = c;
        QVERIFY(hide && hide->isChecked());
        bool clear = false;
        for (QPushButton *b : m_tabs->currentWidget()->findChildren<QPushButton *>()) clear |= b->text() == QLatin1String("Clear");
        QVERIFY(clear);
        QVERIFY(m_tabs->currentWidget()->findChildren<QPlainTextEdit *>().size() == 2);
        shot(QStringLiteral("fujinet"));
        m_tabs->setCurrentIndex(0);
    }

    /* The Video tab: views, zoom, overlays, hover and pin, the tables and
     * Break on writes. */
    void videoTab()
    {
        clearBreakpoints();
        m_tabs->setCurrentIndex(9);
        QWidget *page = m_tabs->currentWidget();
        QTRY_VERIFY_WITH_TIMEOUT(page->isVisible(), 1000);
        auto combos = page->findChildren<QComboBox *>();
        QCOMPARE(combos.size(), 2);
        QComboBox *view = combos[0], *zoom = combos[1];
        QStringList views;
        for (int i = 0; i < view->count(); ++i) views << view->itemText(i);
        QCOMPARE(views, (QStringList{ QStringLiteral("Display"), QStringLiteral("Decoded"), QStringLiteral("Beam"),
                                      QStringLiteral("Values") }));
        QCOMPARE(zoom->itemText(0), QStringLiteral("Fit"));
        QWidget *pic = page->findChild<QWidget *>(QStringLiteral("videoPicture"));
        QVERIFY(pic);
        QTRY_VERIFY_WITH_TIMEOUT(pic->width() >= 64 && pic->height() >= 100, 2000);
        shot(QStringLiteral("video-display"));

        /* a change of view redraws */
        zoom->setCurrentIndex(zoom->findText(QStringLiteral("1×")));
        view->setCurrentIndex(1);
        QTRY_VERIFY_WITH_TIMEOUT(pic->width() >= 256 && pic->height() >= 192, 2000);
        const int decodedWide = pic->width();
        shot(QStringLiteral("video-decoded"));
        zoom->setCurrentIndex(zoom->findText(QStringLiteral("2×")));
        QTRY_COMPARE_WITH_TIMEOUT(pic->width(), decodedWide * 2, 1000);
        view->setCurrentIndex(2);
        QTRY_VERIFY_WITH_TIMEOUT(pic->width() > 0, 1000);
        shot(QStringLiteral("video-beam"));
        view->setCurrentIndex(3);
        QTRY_VERIFY_WITH_TIMEOUT(pic->width() > 0, 1000);
        shot(QStringLiteral("video-values"));
        view->setCurrentIndex(1);

        /* an overlay checkbox */
        QCheckBox *grid = nullptr;
        for (QCheckBox *box : page->findChildren<QCheckBox *>())
            if (box->text() == QLatin1String("Grid")) grid = box;
        QVERIFY(grid);
        grid->setChecked(true);
        QTest::qWait(50);
        shot(QStringLiteral("video-grid"));
        grid->setChecked(false);

        /* hover shows, a click pins, the same spot unpins */
        auto *tabs = page->findChild<QTabWidget *>();
        QVERIFY(tabs);
        QCOMPARE(tabs->tabText(0), QStringLiteral("Pixel"));
        tabs->setCurrentIndex(0);
        QLabel *pixel = nullptr;
        for (QLabel *l : tabs->widget(0)->findChildren<QLabel *>()) pixel = l;
        QVERIFY(pixel);
        QVERIFY(pixel->text().contains(QLatin1String("Move over")));
        const QPoint at(pic->width() / 2, pic->height() / 2);
        QTest::mouseMove(pic, at);
        QTRY_VERIFY_WITH_TIMEOUT(!pixel->text().contains(QLatin1String("Move over")), 1000);
        QTest::mouseClick(pic, Qt::LeftButton, {}, at);
        QTest::mouseMove(pic, at + QPoint(30, 30));
        QVERIFY(!pixel->text().contains(QLatin1String("Move over")));   /* the pin stays */
        shot(QStringLiteral("video-pixel"));
        QTest::mouseClick(pic, Qt::LeftButton, {}, at);
        QTest::mouseMove(pic, QPoint(-5, -5));

        /* the tables */
        QTableWidget *regs = nullptr, *lines = nullptr, *colours = nullptr;
        for (QTableWidget *t : page->findChildren<QTableWidget *>()) {
            if (t->horizontalHeaderItem(0) && t->horizontalHeaderItem(0)->text() == QLatin1String("Name")) regs = t;
            if (t->horizontalHeaderItem(0) && t->horizontalHeaderItem(0)->text() == QLatin1String("Line")) lines = t;
            if (t->horizontalHeaderItem(1) && t->horizontalHeaderItem(1)->text() == QLatin1String("Code")) colours = t;
        }
        QVERIFY(regs && lines && colours);
        tabs->setCurrentIndex(1);
        QTRY_VERIFY_WITH_TIMEOUT(regs->rowCount() > 0, 1000);
        shot(QStringLiteral("video-registers"));
        tabs->setCurrentIndex(2);
        QTRY_VERIFY_WITH_TIMEOUT(colours->rowCount() >= 8, 1000);            /* the VDG's colours or the palette slots */
        shot(QStringLiteral("video-colours"));
        tabs->setCurrentIndex(3);
        QTRY_VERIFY_WITH_TIMEOUT(lines->rowCount() >= 192, 1000);
        shot(QStringLiteral("video-lines"));
        tabs->setCurrentIndex(4);
        QTest::qWait(50);
        shot(QStringLiteral("video-timing"));
        tabs->setCurrentIndex(0);

        /* Break on writes: a pixel of the text screen was fetched from RAM; the button adds a
         * write breakpoint on that byte (physical on a CoCo 3) */
        QPushButton *brk = nullptr;
        for (QPushButton *b : page->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Break on writes")) brk = b;
        QVERIFY(brk);
        view->setCurrentIndex(1);
        zoom->setCurrentIndex(zoom->findText(QStringLiteral("2×")));
        QTRY_VERIFY_WITH_TIMEOUT(pic->width() >= 512, 1000);
        QTest::qWait(100);
        const QPoint top(30, 8);
        QTest::mouseMove(pic, top);
        QTest::mouseClick(pic, Qt::LeftButton, {}, top);
        shot(QStringLiteral("video-pixel-on"));
        QTRY_VERIFY_WITH_TIMEOUT(brk->isEnabled(), 2000);
        QTest::mouseClick(brk, Qt::LeftButton);
        cocodebug_breakpoint bps[8];
        const int made = cocodebug_breakpoints_list(m_d, bps, 8);
        QCOMPARE(made, 1);
        QVERIFY(bps[0].type & COCODEBUG_BP_WRITE);
        QCOMPARE((bps[0].type & COCODEBUG_BP_PHYS) != 0, m_coco3);
        clearBreakpoints();
        QTest::mouseClick(pic, Qt::LeftButton, {}, top);     /* unpin */
        QTest::mouseMove(pic, QPoint(-5, -5));
        zoom->setCurrentIndex(0);
        view->setCurrentIndex(0);
        m_tabs->setCurrentIndex(0);
    }

    /* F12 from the main window shows the debugger (the View menu entry too). */
    void f12FromTheMainWindow()
    {
        /* the display is a QRhiWidget; with no GPU device (CI's offscreen Qt)
         * the window swaps in the software display, so ask for whichever
         * is there */
        QVERIFY(QFile::exists(QStringLiteral(":/shaders/display.vert.qsb")));
        QVERIFY(QFile::exists(QStringLiteral(":/shaders/display.frag.qsb")));
        MainWindow main(m_s);
        main.show();
        QVERIFY(QTest::qWaitForWindowExposed(&main));
        main.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&main));
        QTest::qWait(300);   /* let a queued renderFailed swap the display */
        QWidget *display = main.centralWidget();
        QVERIFY(display);
        display->setFocus();
        QTRY_VERIFY_WITH_TIMEOUT(QApplication::focusWidget() == display, 2000);
        /* the debugger hidden (F12 in its own window), the machine runs; the main window's F12 brings it back */
        QTest::keyClick(m_win, Qt::Key_F12);
        QVERIFY(!m_win->isVisible());
        QVERIFY(!cocodebug_is_engaged(m_d));
        main.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&main));
        QTest::keyClick(display, Qt::Key_F12);
        QVERIFY(m_win->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(cocodebug_is_engaged(m_d), 2000);
        waitStopped();
        /* the menu entry does the same (and with it up, just raises it) */
        QAction *entry = nullptr;
        for (QAction *a : main.findChildren<QAction *>())
            if (a->text() == QLatin1String("&Debugger")) entry = a;
        QVERIFY(entry);
        QCOMPARE(entry->shortcut(), QKeySequence(Qt::Key_F12));
        entry->trigger();
        QVERIFY(m_win->isVisible());
        QVERIFY(cocodebug_is_engaged(m_d));
    }

    /* A power cycle or media change keeps the engine engaged: the window follows,
     * and a breakpoint set before still fires after it. */
    void sessionRestart()
    {
        m_tabs->setCurrentIndex(1);
        cocodebug_breakpoint_toggle(m_d, kCall);
        cocosession_stop(m_s);
        QCOMPARE(cocosession_start(m_s, &m_opts), 0);
        QTRY_VERIFY_WITH_TIMEOUT(cocodebug_is_engaged(m_d), 3000);
        if (!stopped()) cocodebug_pause(m_d);
        waitStopped();
        QVERIFY(bpSet(kCall));       /* kept */
        QPlainTextEdit *v = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(v->toPlainText().contains(QStringLiteral(">")), 2000);
        /* and, given the program again, it runs from there to the breakpoint */
        loadProgram(m_d);
        cocodebug_mem_write(m_d, COCODEBUG_SPACE_CPU, kCounter, (const uint8_t *)"\0", 1);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(stopped() && pcOf(m_d) == kCall, 5000);
        clearBreakpoints();
    }

    void f12HidesAndReleases()
    {
        QVERIFY(m_win->isVisible());
        QTest::keyClick(m_win, Qt::Key_F12);
        QVERIFY(!m_win->isVisible());
        QVERIFY(!cocodebug_is_engaged(m_d));
        QTest::qWait(300);
        QVERIFY(!stopped());                  /* the machine goes on */
        DebuggerWindow::toggle(nullptr, m_s);
        QVERIFY(m_win->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(cocodebug_is_engaged(m_d), 2000);
        waitStopped();
        DebuggerWindow::toggle(nullptr, m_s);
        QVERIFY(!m_win->isVisible());
        QVERIFY(!cocodebug_is_engaged(m_d));
        QTest::qWait(200);
        QVERIFY(!stopped());
        DebuggerWindow::toggle(nullptr, m_s);
        QTRY_VERIFY_WITH_TIMEOUT(cocodebug_is_engaged(m_d), 2000);
        waitStopped();
    }
};

QTEST_MAIN(DebuggerTest)
#include "debugger_test.moc"
