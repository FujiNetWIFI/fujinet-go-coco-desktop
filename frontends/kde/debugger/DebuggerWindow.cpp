/*
 * The 6809 debugger window for the KDE/Qt frontend.
 *
 * Same contract and same shape as the GNOME one
 * (frontends/gnome/debugger/dbg_window.c): it drives core/include/cocodebug.h
 * and nothing else, and it POLLS cocodebug_stop_serial() rather than being
 * called back. The engine stops the machine on the emulator thread deep
 * inside XRoar, and touching widgets from there would be a threading bug; a
 * timer on the UI thread cannot be.
 *
 * Two tabs, like GNOME's: the 6809 view, and FujiNet's DriveWire traffic on
 * the Becker port, which refreshes twice a second while it shows.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "debugger/DebuggerWindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabWidget>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <QVector>

#include <utility>

#include "cocodebug.h"
#include "cocovideo.h"

namespace {

/* How much disassembly to show. Rebuilt on every stop, so this is a
 * screenful and a bit rather than a scrollback. */
constexpr int kDisasmLines = 24;

/* The memory pane: a fixed-size hex dump rebuilt from m_memViewAddr on every
 * stop, the same way the disassembly list is. */
constexpr int kMemBytesPerRow = 16;
constexpr int kMemRows = 16;

/* The tabs, in order: the names COCO_DEBUGGER_TAB takes. */
enum { kTabCpu, kTabFujinet, kTabCount };
const char *const kTabNames[kTabCount] = { "cpu", "fujinet" };

/* Entries the FujiNet tab asks for: all the engine keeps, plus the one in
 * progress. */
constexpr int kFujiTraceMax = 513;

/* COCO_DEBUGGER_TAB: a tab's number or name ("cpu", "fujinet"), or the
 * start of one, any case. */
int tabFromEnv()
{
    const QString tab = qEnvironmentVariable("COCO_DEBUGGER_TAB");
    if (tab.isEmpty())
        return -1;
    bool ok = false;
    const int n = tab.toInt(&ok);
    if (ok)
        return n >= 0 && n < kTabCount ? n : -1;
    for (int i = 0; i < kTabCount; i++)
        if (QString::fromLatin1(kTabNames[i]).startsWith(tab, Qt::CaseInsensitive))
            return i;
    return -1;
}

QLabel *titleLabel(const QString &text)
{
    auto *l = new QLabel(text);
    QFont f = l->font();
    f.setBold(true);
    l->setFont(f);
    return l;
}

/* By family and hint, as msdos does: the platform's FixedFont can come back
 * proportional outside Plasma, and these columns must line up. */
QPlainTextEdit *monoView()
{
    auto *v = new QPlainTextEdit;
    QFont f = v->font();
    f.setFamily(QStringLiteral("monospace"));
    f.setStyleHint(QFont::TypeWriter);
    v->setFont(f);
    v->setReadOnly(true);
    v->setLineWrapMode(QPlainTextEdit::NoWrap);
    return v;
}

/* Whether a view's reader is at its end (a log then follows new lines). */
bool atEnd(QPlainTextEdit *v)
{
    return v->verticalScrollBar()->value() >= v->verticalScrollBar()->maximum() - 1;
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
void setText(QPlainTextEdit *v, const QString &text, bool follow = false)
{
    if (v->toPlainText() == text) {
        if (follow)
            v->verticalScrollBar()->setValue(v->verticalScrollBar()->maximum());
        return;
    }
    const int top = v->verticalScrollBar()->value();
    const int left = v->horizontalScrollBar()->value();
    v->setPlainText(text);
    v->verticalScrollBar()->setValue(follow ? v->verticalScrollBar()->maximum() : top);
    v->horizontalScrollBar()->setValue(left);
}

class DebuggerDialog : public QWidget {
public:
    DebuggerDialog(QWidget *parent, cocosession *session, cocodebug *dbg)
        : QWidget(parent, Qt::Window), m_session(session), m_dbg(dbg),
          m_videoBuf((size_t)COCOVIDEO_MAX_WIDTH * COCOVIDEO_MAX_HEIGHT, 0)
    {
        setWindowTitle(QStringLiteral("Debugger"));
        resize(980, 760);

        auto *outer = new QVBoxLayout(this);

        m_status = new QLabel(QStringLiteral("Running"), this);
        outer->addWidget(m_status);

        auto *cpuPage = new QWidget;
        auto *cpu = new QVBoxLayout(cpuPage);
        cpu->setContentsMargins(0, 0, 0, 0);

        auto *bar = new QHBoxLayout;
        m_runPause = addButton(bar, QStringLiteral("Pause"),
                               QStringLiteral("Run or pause (F5)"),
                               [this] { toggleRun(); });
        addButton(bar, QStringLiteral("Step"),
                  QStringLiteral("One instruction (F7)"),
                  [this] { cocodebug_step(m_dbg); });
        addButton(bar, QStringLiteral("Step Over"),
                  QStringLiteral("Over a subroutine call (F8)"),
                  [this] { cocodebug_step_over(m_dbg); });
        addButton(bar, QStringLiteral("Step Out"),
                  QStringLiteral("Until this routine returns (Shift+F8)"),
                  [this] { cocodebug_step_out(m_dbg); });
        m_stepLine = addButton(
            bar, QStringLiteral("Step Line"),
            QStringLiteral("Run until a different C source line (F6). Needs "
                           "C source info loaded."),
            [this] { cocodebug_step_line(m_dbg); });
        m_stepOverLine = addButton(
            bar, QStringLiteral("Step Over Line"),
            QStringLiteral("Like Step Line, but a call on this line runs to "
                           "completion rather than being stepped into "
                           "(Shift+F6)."),
            [this] { cocodebug_step_over_line(m_dbg); });
        addButton(bar, QStringLiteral("Clear Breakpoints"),
                  QStringLiteral("Remove every breakpoint"), [this] {
                      cocodebug_breakpoint_clear_all(m_dbg);
                      m_seenSerial = 0; /* redraw the markers */
                  });
        addButton(bar, QStringLiteral("Load Symbols…"),
                  QStringLiteral("Load a symbol file (LWASM --symbol-dump, "
                                 "or plain \"NAME ADDR\" lines) to label the "
                                 "disassembly"),
                  [this] { loadSymbols(); });
        addButton(bar, QStringLiteral("Clear Symbols"),
                  QStringLiteral("Drop loaded symbols, keeping the built-in "
                                 "Color BASIC entry points"),
                  [this] {
                      cocodebug_symbols_clear(m_dbg);
                      m_seenSerial = 0;
                  });
        addButton(bar, QStringLiteral("Load C Source…"),
                  QStringLiteral("Load one or more CMOC listings (.lst, "
                                 "from `cmoc -i`) for source-level stepping "
                                 "and locals -- select every module of a "
                                 "multi-file program at once. The shared "
                                 ".map (also only kept by -i) is found "
                                 "automatically."),
                  [this] { loadSource(); });
        addButton(bar, QStringLiteral("Clear C Source"),
                  QStringLiteral("Drop loaded C source info"), [this] {
                      cocodebug_source_clear(m_dbg);
                      m_srcFileCombo->clear();
                      m_srcCachedPath.clear();
                      m_pcFileId = -1;
                      m_seenSerial = 0;
                  });
        bar->addStretch();
        cpu->addLayout(bar);

        auto *panes = new QHBoxLayout;
        m_disasm = new QListWidget(this);
        m_disasm->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        /* Clicking a line toggles a breakpoint on it -- the one interaction
         * the disassembly list has. */
        connect(m_disasm, &QListWidget::itemClicked, this,
                [this](QListWidgetItem *item) {
                    cocodebug_breakpoint_toggle(
                        m_dbg, (uint16_t)item->data(Qt::UserRole).toUInt());
                    m_seenSerial = 0;
                });
        panes->addWidget(m_disasm, 1);

        auto *srcCol = new QVBoxLayout;
        m_srcFileCombo = new QComboBox(this);
        m_srcFileCombo->setEnabled(false);
        connect(m_srcFileCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int idx) {
                    if (idx < 0 || !m_dbg)
                        return;
                    populateSourceList(m_srcFileCombo->itemData(idx).toInt());
                });
        srcCol->addWidget(m_srcFileCombo);
        m_srcList = new QListWidget(this);
        m_srcList->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        connect(m_srcList, &QListWidget::itemClicked, this,
                [this](QListWidgetItem *item) {
                    cocodebug_source_breakpoint_toggle(
                        m_dbg, m_srcDisplayedFileId,
                        item->data(Qt::UserRole).toInt());
                    m_seenSerial = 0;
                });
        srcCol->addWidget(m_srcList, 1);
        panes->addLayout(srcCol, 1);

        auto *sidebar = new QVBoxLayout;
        m_regs = new QLabel(this);
        m_regs->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        m_regs->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        m_regs->setMinimumWidth(200);
        sidebar->addWidget(m_regs);

        m_locals = new QLabel(this);
        m_locals->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        m_locals->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        m_locals->setWordWrap(false);
        sidebar->addWidget(m_locals);

        m_video = new QLabel(this);
        m_video->setAlignment(Qt::AlignCenter);
        m_video->setMinimumSize(256, 192);
        /* Distinct from m_regs's dark theme -- video content is meant to be
         * seen against black the way the machine's own screen is, not
         * whatever the app's palette happens to be. */
        m_video->setStyleSheet(QStringLiteral("background-color: black;"));
        sidebar->addWidget(m_video, 1);

        panes->addLayout(sidebar);
        cpu->addLayout(panes, 1);

        auto *memBar = new QHBoxLayout;
        memBar->addWidget(new QLabel(QStringLiteral("Address:"), this));
        m_memAddr = new QLineEdit(QStringLiteral("0000"), this);
        m_memAddr->setMaximumWidth(80);
        connect(m_memAddr, &QLineEdit::returnPressed, this,
                [this] { goToMemAddr(); });
        memBar->addWidget(m_memAddr);
        auto *memGo = new QPushButton(QStringLiteral("Go"), this);
        memGo->setToolTip(QStringLiteral("Jump to this address"));
        connect(memGo, &QPushButton::clicked, this,
                [this] { goToMemAddr(); });
        memBar->addWidget(memGo);
        memBar->addStretch();
        cpu->addLayout(memBar);

        m_mem = new QLabel(this);
        m_mem->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        m_mem->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        cpu->addWidget(m_mem);

        m_tabs = new QTabWidget(this);
        m_tabs->addTab(cpuPage, QStringLiteral("CPU"));
        m_tabs->addTab(buildFujinet(), QStringLiteral("FujiNet"));
        const int tab = tabFromEnv();
        if (tab >= 0)
            m_tabs->setCurrentIndex(tab);
        connect(m_tabs, &QTabWidget::currentChanged, this, [this](int i) {
            if (i == kTabFujinet)
                refreshFujinet(false);
        });
        outer->addWidget(m_tabs, 1);

        new QShortcut(QKeySequence(Qt::Key_F5), this, [this] { toggleRun(); });
        new QShortcut(QKeySequence(Qt::Key_F6), this,
                      [this] { cocodebug_step_line(m_dbg); });
        new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F6), this,
                      [this] { cocodebug_step_over_line(m_dbg); });
        new QShortcut(QKeySequence(Qt::Key_F7), this,
                      [this] { cocodebug_step(m_dbg); });
        new QShortcut(QKeySequence(Qt::Key_F8), this,
                      [this] { cocodebug_step_over(m_dbg); });
        new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F8), this,
                      [this] { cocodebug_step_out(m_dbg); });

        /* 60ms is well under a human's notice, and cheaper than the frame
         * clock the GNOME window happens to have available. */
        auto *timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this] { poll(); });
        timer->start(60);

        /* Engaging switches the emulator loop to the path that honours
         * breakpoints; pausing immediately is what a user opening a debugger
         * expects, since otherwise the window shows nothing at all. */
        cocodebug_set_engaged(m_dbg, 1);
        cocodebug_pause(m_dbg);
    }

    ~DebuggerDialog() override
    {
        /* Closing releases the debugger: leaving it engaged would cost a hook
         * call per instruction forever, and leaving it PAUSED would look
         * exactly like the emulator having hung. */
        cocodebug_set_engaged(m_dbg, 0);
    }

private:
    QWidget *buildFujinet()
    {
        auto *split = new QSplitter(Qt::Horizontal);
        auto *left = new QWidget;
        auto *lv = new QVBoxLayout(left);
        lv->addWidget(titleLabel(QStringLiteral("DriveWire packets")));
        auto *row = new QHBoxLayout;
        m_fujiNote = new QLabel;
        m_fujiNote->setStyleSheet(QStringLiteral("color: gray;"));
        m_fujiNote->setWordWrap(true);
        m_fujiHideReady = new QCheckBox(QStringLiteral("Hide READY probes"));
        m_fujiHideReady->setChecked(true);
        m_fujiHideReady->setToolTip(
            QStringLiteral("fujinet-lib asks E2 00 before every call"));
        connect(m_fujiHideReady, &QCheckBox::toggled, this,
                [this] { refreshFujinet(true); });
        auto *clear = new QPushButton(QStringLiteral("Clear"));
        connect(clear, &QPushButton::clicked, this, [this] {
            cocodebug_fujinet_trace_clear(m_dbg);
            refreshFujinet(true);
        });
        row->addWidget(m_fujiNote, 1);
        row->addWidget(m_fujiHideReady);
        row->addWidget(clear);
        lv->addLayout(row);
        m_fujiText = monoView();
        lv->addWidget(m_fujiText, 1);
        left->setMinimumWidth(560);

        auto *right = new QWidget;
        auto *rv = new QVBoxLayout(right);
        rv->addWidget(titleLabel(QStringLiteral("Becker port")));
        m_fujiState = monoView();
        rv->addWidget(m_fujiState, 1);
        right->setMinimumWidth(340);
        split->addWidget(left);
        split->addWidget(right);
        split->setStretchFactor(0, 3);
        split->setStretchFactor(1, 2);
        split->setChildrenCollapsible(false);
        return split;
    }

    void refreshFujinet(bool force)
    {
        static coco_fuji_packet p[kFujiTraceMax];
        char state[1024];
        const int n = cocodebug_fujinet_trace(m_dbg, p, kFujiTraceMax);
        const bool hide = m_fujiHideReady->isChecked();

        if (!cocosession_fujinet_running(m_session) &&
            !cocosession_fujinet_external(m_session))
            m_fujiNote->setText(
                QStringLiteral("FujiNet is off (Settings ▸ FujiNet)."));
        else
            m_fujiNote->setText(
                QStringLiteral("%1 FujiNet on the Becker port %2, %3.")
                    .arg(cocosession_fujinet_external(m_session)
                             ? QStringLiteral("External")
                             : QStringLiteral("Built-in"),
                         QString::fromUtf8(cocosession_becker_endpoint(m_session)),
                         cocosession_fujinet_link(m_session)
                             ? QStringLiteral("connected")
                             : QStringLiteral("not connected")));
        cocodebug_fujinet_state(m_dbg, state, sizeof(state));
        setText(m_fujiState, QString::fromUtf8(state));
        m_fujiClock.restart();

        if (!force && n == m_fujiCount &&
            (n == 0 || p[n - 1].serial == m_fujiLast))
            return;
        m_fujiCount = n;
        m_fujiLast = n > 0 ? p[n - 1].serial : 0;
        QString t;
        int shown = 0;
        for (int i = 0; i < n; i++) {
            if (hide && cocodebug_fujinet_is_ready(&p[i]))
                continue;
            if (shown++)
                t += QLatin1Char('\n');
            t += QString::fromUtf8(p[i].text) + QStringLiteral("\n     ");
            for (int k = 0; k < p[i].nbytes && k < (int)sizeof p[i].data; k++)
                t += hex8(p[i].data[k]) + QLatin1Char(' ');
            if (p[i].nbytes > (int)sizeof p[i].data)
                t += QStringLiteral("…");
        }
        if (shown == 0)
            t = n ? QStringLiteral("(only READY probes since the trace was "
                                   "cleared)")
                  : QStringLiteral("(no DriveWire traffic since the trace was "
                                   "cleared)");
        setText(m_fujiText, t, force || atEnd(m_fujiText));
    }

    template <typename F>
    QPushButton *addButton(QHBoxLayout *bar, const QString &label,
                           const QString &tip, F &&fn)
    {
        auto *b = new QPushButton(label, this);
        b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, std::forward<F>(fn));
        bar->addWidget(b);
        return b;
    }

    void toggleRun()
    {
        if (cocodebug_is_paused(m_dbg))
            cocodebug_resume(m_dbg);
        else
            cocodebug_pause(m_dbg);
    }

    void goToMemAddr()
    {
        bool ok = false;
        const uint v = m_memAddr->text().toUInt(&ok, 16);
        if (ok)
            m_memViewAddr = (uint16_t)v;
        m_seenSerial = 0; /* force a redraw at the new address */
    }

    void loadSymbols()
    {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Load Symbols"), QString(),
            QStringLiteral("Symbol files (*.sym);;All files (*)"));
        if (path.isEmpty())
            return;
        if (cocodebug_symbols_load(m_dbg, path.toUtf8().constData()) < 0) {
            m_status->setText(QStringLiteral("Could not open symbol file"));
            return;
        }
        m_seenSerial = 0; /* redraw the disassembly with the new labels */
    }

    /* One or more .lst files, one per module of a multi-file CMOC program --
     * cocodebug_source_load() adds to what is already loaded rather than
     * replacing it (see its own doc comment for why: cmoc names the .map
     * after the linked executable, not any one .lst, so every module's
     * listing wants the very .map its neighbours do), so loading them all
     * here in one dialog is exactly the multi-file case working as
     * intended. */
    void loadSource()
    {
        const QStringList paths = QFileDialog::getOpenFileNames(
            this, QStringLiteral("Load C Source"), QString(),
            QStringLiteral("CMOC listings (*.lst);;All files (*)"));
        if (paths.isEmpty())
            return;
        int loaded = 0, failed = 0;
        for (const QString &path : paths) {
            if (cocodebug_source_load(m_dbg, path.toUtf8().constData()) < 0)
                failed++;
            else
                loaded++;
        }
        if (failed && !loaded) {
            m_status->setText(
                QStringLiteral("Could not open listing or its .map (both "
                               "need `cmoc -i`)"));
            return;
        }
        if (failed)
            m_status->setText(
                QStringLiteral("Some listings could not be opened"));
        m_srcCachedPath.clear(); /* the file list may have changed */
        rebuildSourceCombo();
        m_seenSerial = 0;
    }

    void rebuildSourceCombo()
    {
        const QSignalBlocker blocker(m_srcFileCombo);
        m_srcFileCombo->clear();
        const int n = cocodebug_source_file_count(m_dbg);
        for (int i = 0; i < n; i++) {
            const char *path = cocodebug_source_file_path(m_dbg, i);
            const QString qpath = QString::fromUtf8(path ? path : "");
            const int slash = qpath.lastIndexOf(QLatin1Char('/'));
            m_srcFileCombo->addItem(
                slash >= 0 ? qpath.mid(slash + 1) : qpath, i);
        }
        m_srcFileCombo->setEnabled(n > 0);
        if (n > 0)
            populateSourceList(m_srcFileCombo->itemData(0).toInt());
        else
            m_srcList->clear();
    }

    /* Loads (and caches) fileId's text, then redraws the list with '>' on
     * the line PC is at (if PC's file is the one showing) and '*' on any
     * line carrying a breakpoint. Re-reading the file is cheap enough here
     * since this only runs on an actual stop or a manual file switch, never
     * on every poll tick -- see poll()'s early-out. */
    void populateSourceList(int fileId)
    {
        const char *path = cocodebug_source_file_path(m_dbg, fileId);
        const QString qpath = QString::fromUtf8(path ? path : "");
        m_srcDisplayedFileId = fileId;

        if (qpath != m_srcCachedPath) {
            m_srcCachedLines.clear();
            QFile f(qpath);
            if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QTextStream ts(&f);
                while (!ts.atEnd())
                    m_srcCachedLines << ts.readLine();
            }
            m_srcCachedPath = qpath;
        }

        m_srcList->clear();
        int scrollTo = -1;
        for (int i = 0; i < m_srcCachedLines.size(); i++) {
            const int lineNo = i + 1;
            const bool isPc = fileId == m_pcFileId && lineNo == m_pcLine;
            const bool hasBp =
                cocodebug_source_breakpoint_is_set(m_dbg, fileId, lineNo);
            const QString text =
                QStringLiteral("%1%2 %3  %4")
                    .arg(isPc ? QLatin1Char('>') : QLatin1Char(' '))
                    .arg(hasBp ? QLatin1Char('*') : QLatin1Char(' '))
                    .arg(lineNo, 5)
                    .arg(m_srcCachedLines[i]);
            auto *item = new QListWidgetItem(text, m_srcList);
            item->setData(Qt::UserRole, lineNo);
            if (isPc)
                scrollTo = m_srcList->count() - 1;
        }
        if (scrollTo >= 0)
            m_srcList->setCurrentRow(scrollTo);
    }

    void updateLocalsPane(bool paused)
    {
        if (!paused || !cocodebug_source_available(m_dbg)) {
            m_locals->clear();
            return;
        }
        cocodebug_local locals[32];
        const int n = cocodebug_locals_get(m_dbg, locals, 32);
        if (n == 0) {
            /* Not necessarily an error: PC may just be before the function's
             * first statement (mid-prologue), where there is nothing correct
             * to show yet. */
            m_locals->setText(QStringLiteral("(no locals in scope)"));
            return;
        }
        QString text;
        for (int i = 0; i < n; i++) {
            const cocodebug_local &v = locals[i];
            const QString valueStr =
                v.has_value
                    ? QStringLiteral("%1 ($%2)")
                          .arg(v.value)
                          .arg(v.value, 0, 16)
                    : QStringLiteral("?");
            text += QStringLiteral("%1 %2 %3= %4\n")
                        .arg(QString::fromUtf8(v.name).leftJustified(12))
                        .arg(QString::fromUtf8(v.type).leftJustified(10))
                        .arg(v.is_param ? QStringLiteral("(param) ")
                                        : QString())
                        .arg(valueStr);
        }
        m_locals->setText(text.trimmed());
    }

    void poll()
    {
        const uint64_t serial = cocodebug_stop_serial(m_dbg);
        const bool paused = cocodebug_is_paused(m_dbg) != 0;
        /* Rebuild when the engine has stopped somewhere new, or when it has
         * just started running again and the panes need clearing. */
        if (serial == m_seenSerial && paused == m_wasPaused) {
            /* the traffic twice a second, whether or not the machine stops */
            if (m_tabs->currentIndex() == kTabFujinet &&
                (!m_fujiClock.isValid() || m_fujiClock.elapsed() >= 500))
                refreshFujinet(false);
            return;
        }
        m_seenSerial = serial;
        m_wasPaused = paused;
        refresh(paused);
        if (m_tabs->currentIndex() == kTabFujinet)
            refreshFujinet(false);
    }

    void refresh(bool paused)
    {
        m_runPause->setText(paused ? QStringLiteral("Run")
                                   : QStringLiteral("Pause"));
        m_status->setText(paused ? QStringLiteral("Paused")
                                 : QStringLiteral("Running"));
        const bool srcAvail = cocodebug_source_available(m_dbg) != 0;
        m_stepLine->setEnabled(srcAvail);
        m_stepOverLine->setEnabled(srcAvail);

        m_disasm->clear();
        if (!paused) {
            /* Everything on show is only valid while the machine is parked,
             * so clear it rather than leave a stale snapshot looking live. */
            m_regs->clear();
            m_mem->clear();
            m_video->clear();
            m_locals->clear();
            return;
        }

        cocodebug_regs r;
        if (!cocodebug_regs_get(m_dbg, &r)) {
            m_regs->clear();
            return;
        }

        /* CC is the one register whose bits matter more than its value:
         * Entire, FIRQ mask, Half carry, IRQ mask, Negative, Zero, oVerflow,
         * Carry. */
        m_regs->setText(
            QStringLiteral("PC  %1      A %2  B %3\n"
                           "X   %4      DP %5\n"
                           "Y   %6\nU   %7\nS   %8\n\n"
                           "CC  %9  %10")
                .arg(hex16(r.pc), hex8(r.a), hex8(r.b), hex16(r.x), hex8(r.dp),
                     hex16(r.y), hex16(r.u), hex16(r.s))
                .arg(hex8(r.cc), ccBits(r.cc)));

        coco_dasm_line lines[kDisasmLines];
        const int n = cocodebug_disassemble(m_dbg, r.pc, lines, kDisasmLines);
        for (int i = 0; i < n; i++) {
            const coco_dasm_line &l = lines[i];
            QString bytes;
            for (int b = 0; b < l.len; b++)
                bytes += hex8(l.bytes[b]);
            char symname[32];
            const QString symSuffix =
                cocodebug_symbol_at(m_dbg, l.addr, symname, sizeof(symname))
                    ? QStringLiteral("   ; %1").arg(QString::fromUtf8(symname))
                    : QString();
            /* ">" is where the CPU is, "*" a breakpoint, both in the same
             * column so the addresses stay aligned. */
            const QString text =
                QStringLiteral("%1%2 %3  %4 %5%6")
                    .arg(l.addr == r.pc ? QLatin1Char('>') : QLatin1Char(' '))
                    .arg(cocodebug_breakpoint_is_set(m_dbg, l.addr)
                             ? QLatin1Char('*') : QLatin1Char(' '))
                    .arg(hex16(l.addr), bytes.leftJustified(10),
                         QString::fromUtf8(l.text), symSuffix);
            auto *item = new QListWidgetItem(text, m_disasm);
            item->setData(Qt::UserRole, (uint)l.addr);
        }

        /* Source pane: follow PC's file automatically. If PC is not itself
         * a mapped statement (mid-prologue, or code with no line info), keep
         * showing whatever file was last selected and just drop the '>'
         * marker rather than guess. */
        if (srcAvail) {
            int fid, line;
            if (cocodebug_source_line_at(m_dbg, r.pc, &fid, &line)) {
                m_pcFileId = fid;
                m_pcLine = line;
                int idx = -1;
                for (int i = 0; i < m_srcFileCombo->count(); i++)
                    if (m_srcFileCombo->itemData(i).toInt() == fid) {
                        idx = i;
                        break;
                    }
                if (idx >= 0 && idx != m_srcFileCombo->currentIndex())
                    m_srcFileCombo->setCurrentIndex(idx); /* repopulates */
                else
                    populateSourceList(m_srcDisplayedFileId);
            } else {
                m_pcFileId = -1;
                m_pcLine = -1;
                if (m_srcFileCombo->count() > 0)
                    populateSourceList(m_srcDisplayedFileId);
            }
        }
        updateLocalsPane(paused);

        uint8_t mem[kMemBytesPerRow * kMemRows];
        const int memN = cocodebug_read(m_dbg, m_memViewAddr, mem, sizeof(mem));
        QString dump;
        for (int row = 0; row * kMemBytesPerRow < memN; row++) {
            const uint16_t addr =
                (uint16_t)(m_memViewAddr + row * kMemBytesPerRow);
            QString ascii;

            dump += hex16(addr) + QLatin1Char(' ');
            for (int col = 0; col < kMemBytesPerRow; col++) {
                const int idx = row * kMemBytesPerRow + col;
                if (idx < memN) {
                    const uint8_t b = mem[idx];
                    dump += QLatin1Char(' ') + hex8(b);
                    ascii += (b >= 0x20 && b < 0x7f) ? QLatin1Char((char)b)
                                                      : QLatin1Char('.');
                } else {
                    dump += QStringLiteral("   ");
                    ascii += QLatin1Char(' ');
                }
            }
            dump += QStringLiteral("  ") + ascii;
            if ((row + 1) * kMemBytesPerRow < memN)
                dump += QLatin1Char('\n');
        }
        m_mem->setText(dump);

        int vw = 0, vh = 0;
        if (cocodebug_video_decode(m_dbg, m_videoBuf.data(), &vw, &vh) &&
            vw > 0 && vh > 0) {
            /* A view over the packed buffer: rows are vw pixels apart, same
             * XRGB8888 -> Format_RGB32 convention as DisplayWidget's own
             * paintGL(). Scaled to fit rather than shown 1:1 -- native
             * resolution varies a lot between modes (a CoCo 1/2 canvas is a
             * fixed 256x192, a CoCo 3 GIME mode can be much wider). */
            const QImage frame((const uchar *)m_videoBuf.data(), vw, vh,
                               vw * 4, QImage::Format_RGB32);
            m_video->setPixmap(QPixmap::fromImage(frame).scaled(
                m_video->size(), Qt::KeepAspectRatio,
                Qt::SmoothTransformation));
        } else {
            m_video->clear();
        }
    }

    static QString hex8(uint8_t v)
    {
        return QStringLiteral("%1").arg(v, 2, 16, QLatin1Char('0')).toUpper();
    }
    static QString hex16(uint16_t v)
    {
        return QStringLiteral("%1").arg(v, 4, 16, QLatin1Char('0')).toUpper();
    }
    static QString ccBits(uint8_t cc)
    {
        static const char names[] = "EFHINZVC";
        QString s;
        for (int i = 0; i < 8; i++)
            s += (cc & (0x80 >> i)) ? QLatin1Char(names[i]) : QLatin1Char('-');
        return s;
    }

    cocosession *m_session;
    cocodebug *m_dbg;
    QTabWidget *m_tabs = nullptr;
    QLabel *m_fujiNote = nullptr;
    QCheckBox *m_fujiHideReady = nullptr;
    QPlainTextEdit *m_fujiText = nullptr;
    QPlainTextEdit *m_fujiState = nullptr;
    int m_fujiCount = -1;
    uint64_t m_fujiLast = 0;
    QElapsedTimer m_fujiClock;
    QLabel *m_status = nullptr;
    QLabel *m_regs = nullptr;
    QLabel *m_locals = nullptr;
    QLabel *m_video = nullptr;
    QLabel *m_mem = nullptr;
    QLineEdit *m_memAddr = nullptr;
    QListWidget *m_disasm = nullptr;
    QComboBox *m_srcFileCombo = nullptr;
    QListWidget *m_srcList = nullptr;
    QPushButton *m_runPause = nullptr;
    QPushButton *m_stepLine = nullptr;
    QPushButton *m_stepOverLine = nullptr;
    QVector<uint32_t> m_videoBuf;
    uint64_t m_seenSerial = 0;
    bool m_wasPaused = false;
    uint16_t m_memViewAddr = 0;

    /* The source pane's own cache: which file's text is currently held in
     * m_srcCachedLines, so a stop that doesn't change the displayed file
     * doesn't re-read it from disk. */
    QString m_srcCachedPath;
    QStringList m_srcCachedLines;
    int m_srcDisplayedFileId = -1;
    /* Where PC actually is, independent of which file the combo is showing
     * (a user may be browsing a different file to set a breakpoint ahead of
     * time): the '>' marker only ever appears when these match the
     * displayed file. */
    int m_pcFileId = -1;
    int m_pcLine = -1;
};

/* One live window at a time: a second would fight the first over the same
 * engine, and there is nothing useful to see twice. */
QPointer<DebuggerDialog> g_window;

} // namespace

namespace DebuggerWindow {

void show(QWidget *parent, cocosession *session)
{
    if (g_window) {
        g_window->raise();
        g_window->activateWindow();
        return;
    }

    cocodebug *dbg = cocosession_debugger(session);
    if (!dbg) {
        QMessageBox::information(
            parent, QStringLiteral("Debugger Unavailable"),
            QStringLiteral("The debugger could not be created for this "
                           "session."));
        return;
    }

    auto *w = new DebuggerDialog(parent, session, dbg);
    w->setAttribute(Qt::WA_DeleteOnClose);
    g_window = w;
    w->show();
}

} // namespace DebuggerWindow
