/*
 * Debugger window (Qt6 Widgets) over the 6809/6309 debugger engine, via
 * core/include/cocodebug.h. Mirrors the GNOME one tab for tab, and follows the
 * Studio II, Channel F, Astrocade, SMS and Apple II KDE debuggers.
 *
 * Tabs, in this order (COCO_DEBUGGER_TAB picks the one a window opens on, by
 * index or by name -- 0 console, 1 disassembly, 2 csource, 3 memory,
 * 4 breakpoints, 5 watches, 6 cart, 7 trace, 8 history, 9 video, 10 sound,
 * 11 symbols, 12 fujinet -- or by the start of a label; "cpu" is the old name
 * of the Disassembly): the Console (the engine's command line, with a history
 * and Tab completion), the Disassembly (click a line to select it for Run to
 * Cursor; click its left edge, double-click it or press F9 for its
 * breakpoint), C Source (a CMOC listing, one row a line: click a line for its
 * breakpoint, the current function's locals beside it; Step Line F6 and Step
 * Over Line Shift+F6), Memory (the CPU's 64K, or a CoCo 3's physical RAM; Go
 * to a label / address / range, a hex or text search, Poke), Breakpoints
 * (execute, read, write, access, physical on a CoCo 3, with a condition),
 * Watches (a value list, Break on change), Cart & I/O (the cartridge, the
 * memory map, the PIAs, the SAM, the GIME, the joysticks), Trace and History
 * (the engine's instruction ring: live, and read at a stop), Video (the VDG's
 * and the GIME's pictures), Sound (the DAC and the one-bit output, with their
 * recent writes), Symbols (every label, filtered) and FujiNet (the DriveWire
 * packets on the Becker port). The registers sit in a strip under the tabs,
 * on every one of them. Any tab can be popped out into a window of its own
 * (COCO_POPOUT names one at start). The family's keys apply (F5 run/stop, F6
 * step line, F7 step, F8 step over, Shift+F8 step out, F9 breakpoint, F12
 * close).
 *
 * Showing the window engages the engine (which stops the machine, as on every
 * sibling); hiding it releases, which lets the machine run on.
 *
 * A short timer polls the engine's generation and stop serial and refreshes
 * the tab showing when they moved; while the machine runs, the live tabs
 * refresh every 400 ms. Every new stop re-centres every view on the spot.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DebuggerWindow.h"
#include "VideoTab.h"
#include "../help/HelpWindow.h"

extern "C" {
#include "cocomemtools.h"
}

#include <QCloseEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QIcon>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QRegularExpression>
#include <QScrollArea>
#include <QShortcut>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextStream>
#include <QThread>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cstdio>
#include <cstring>
#include <utility>

#define DISASM_WINDOW 64        /* lines fetched; as many show as fit */
#define MEM_ROW_TEXT 160        /* bytes of text a Memory row can take */
#define MEM_ROWS_MAX 256        /* rows of the dump shown at once */
#define BP_MAX 256
#define TRACE_MAX 513
#define HIST_SHOWN 2000         /* trace lines shown */
#define RING_MAX 65537
#define CONSOLE_LINES 5000
#define REGION_MAX 64
#define SOUND_MAX 4096
#define LOCALS_MAX 32

namespace {

enum {
    TAB_CONSOLE, TAB_DISASM, TAB_SOURCE, TAB_MEMORY, TAB_BREAKS, TAB_WATCHES, TAB_CART, TAB_TRACE, TAB_HISTORY,
    TAB_VIDEO, TAB_SOUND, TAB_SYMBOLS, TAB_FUJINET, TAB_COUNT
};

/* The tabs, in order: the names COCO_DEBUGGER_TAB takes, their labels (which
 * it takes too), and their Help contexts (debugger/<help>). */
const struct { const char *name, *label, *help; } kTabs[TAB_COUNT] = {
    { "console", "Console", "console" },           { "disassembly", "Disassembly", "disassembly" },
    { "csource", "C Source", "csource" },          { "memory", "Memory", "memory" },
    { "breakpoints", "Breakpoints", "breakpoints" }, { "watches", "Watches", "watches" },
    { "cart", "Cart & I/O", "cart" },              { "trace", "Trace", "trace" },
    { "history", "History", "history" },           { "video", "Video", "video" },
    { "sound", "Sound", "sound" },                 { "symbols", "Symbols", "symbols" },
    { "fujinet", "FujiNet", "fujinet" },
};

/* The registers the strip edits, in the order shown. */
const struct { const char *name; int id, digits; const char *tip; } kRegs[] = {
    { "PC", COCODEBUG_REG_PC, 4, "The instruction about to run" },
    { "A", COCODEBUG_REG_A, 2, "Accumulator A" },
    { "B", COCODEBUG_REG_B, 2, "Accumulator B" },
    { "D", COCODEBUG_REG_D, 4, "A:B as one 16-bit accumulator" },
    { "DP", COCODEBUG_REG_DP, 2, "The direct page" },
    { "X", COCODEBUG_REG_X, 4, "Index register X" },
    { "Y", COCODEBUG_REG_Y, 4, "Index register Y" },
    { "U", COCODEBUG_REG_U, 4, "User stack pointer" },
    { "S", COCODEBUG_REG_S, 4, "System stack pointer" },
    { "CC", COCODEBUG_REG_CC, 2, "The condition codes, EFHINZVC" },
};

/* The HD6309's extra registers, shown only on a 6309. */
const struct { const char *name; int id, digits; const char *tip; } kRegs6309[] = {
    { "E", COCODEBUG_REG_E, 2, "6309 accumulator E" },
    { "F", COCODEBUG_REG_F, 2, "6309 accumulator F" },
    { "W", COCODEBUG_REG_W, 4, "6309 accumulator W (E:F)" },
    { "V", COCODEBUG_REG_V, 4, "6309 register V" },
    { "MD", COCODEBUG_REG_MD, 2, "6309 mode register" },
};

/* CC bits, bit 7 first. */
const struct { const char *name; unsigned mask; const char *tip; } kCc[8] = {
    { "E", COCODEBUG_CC_E, "Entire: the whole state was stacked" },
    { "F", COCODEBUG_CC_F, "FIRQ mask" },
    { "H", COCODEBUG_CC_H, "Half carry" },
    { "I", COCODEBUG_CC_I, "IRQ mask" },
    { "N", COCODEBUG_CC_N, "Negative" },
    { "Z", COCODEBUG_CC_Z, "Zero" },
    { "V", COCODEBUG_CC_V, "Overflow" },
    { "C", COCODEBUG_CC_C, "Carry" },
};

/* The breakpoint types the Breakpoints tab offers, in the order of its chooser. */
const struct { const char *label; int type; } kBpTypes[4] = {
    { "Execute", COCODEBUG_BP_EXEC }, { "Read", COCODEBUG_BP_READ }, { "Write", COCODEBUG_BP_WRITE },
    { "Access", COCODEBUG_BP_ACCESS },
};

const QString kGray = QStringLiteral("color: gray;");

/* The PC's line and a marked memory row: the CoCo icon's green. It is mid
 * light, so every fill of it carries dark ink. */
QColor rgbOf(unsigned rgb)
{
    return QColor((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

QColor accentColor()
{
    return rgbOf(COCOSESSION_ACCENT_RGB);
}

QColor accentInk()
{
    return rgbOf(COCOSESSION_ACCENT_INK_RGB);
}

/* Accent for lines and text: the dark green on a light theme. */
QColor accentTextColor(const QWidget *w)
{
    if (w && w->palette().color(QPalette::Window).lightness() < 128) return accentColor();
    return rgbOf(COCOSESSION_ACCENT_DARK_RGB);
}

QTextCharFormat accentFormat()
{
    QTextCharFormat f;
    f.setBackground(accentColor());
    f.setForeground(accentInk());
    return f;
}

/* A watched range in the dump: a blue the accent does not hide. */
QTextCharFormat watchFormat()
{
    QTextCharFormat f;
    f.setBackground(QColor(0x1c, 0x71, 0xd8));
    f.setForeground(Qt::white);
    return f;
}

void setMono(QWidget *w)
{
    QFont f = w->font();
    f.setFamily(QStringLiteral("monospace"));
    f.setStyleHint(QFont::TypeWriter);
    w->setFont(f);
}

QPlainTextEdit *monoView()
{
    auto *v = new QPlainTextEdit;
    v->setReadOnly(true);
    setMono(v);
    v->setLineWrapMode(QPlainTextEdit::NoWrap);
    return v;
}

/* Engine text for a view: control bytes dropped. */
QString stripControl(const char *s)
{
    const QString in = QString::fromUtf8(s);
    QString out;
    out.reserve(in.size());
    for (const QChar c : in)
        if (c.unicode() >= 0x20 || c == QLatin1Char('\n') || c == QLatin1Char('\t')) out += c;
    return out;
}

QString hx(unsigned v, int digits)
{
    return QStringLiteral("%1").arg(v, digits, 16, QLatin1Char('0')).toUpper();
}

/* "Stopped" and the engine's reason: "stopped at $0412" reads "Stopped at
 * $0412", anything else "Stopped: <reason>". */
QString stoppedText(const QString &reason)
{
    if (reason.isEmpty()) return QStringLiteral("Stopped");
    if (reason.startsWith(QLatin1String("stopped"))) return QStringLiteral("S") + reason.mid(1);
    return QStringLiteral("Stopped: ") + reason;
}

QLabel *titleLabel(const QString &text)
{
    auto *l = new QLabel(text);
    QFont f = l->font();
    f.setBold(true);
    l->setFont(f);
    return l;
}

QLabel *hintLabel(const QString &text)
{
    auto *l = new QLabel(text);
    l->setStyleSheet(kGray);
    l->setWordWrap(true);
    return l;
}

QLineEdit *smallEntry(const QString &placeholder, int chars)
{
    auto *e = new QLineEdit;
    e->setPlaceholderText(placeholder);
    e->setFixedWidth(e->fontMetrics().averageCharWidth() * chars + 16);
    return e;
}

/* A register entry exactly wide enough for its hex digits. */
void setDigits(QLineEdit *e, int digits)
{
    e->setFixedWidth(e->fontMetrics().horizontalAdvance(QString(digits, QLatin1Char('0'))) + 16);
}

/* A toggle takes the checked style (the accent, a box of its own), which is
 * not the native button's: one size for both states and every text,
 * so it never jumps. */
void fixToggleSize(QPushButton *b, const QStringList &texts, int minHeight)
{
    b->ensurePolished();
    QSize s(0, minHeight);
    for (const bool on : { false, true })
        for (const QString &t : texts) {
            b->setChecked(on);
            b->setText(t);
            s = s.expandedTo(b->sizeHint());
        }
    b->setChecked(false);
    b->setText(texts.first());
    b->setFixedSize(s);
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
        if (follow) v->verticalScrollBar()->setValue(v->verticalScrollBar()->maximum());
        return;
    }
    const int top = v->verticalScrollBar()->value(), left = v->horizontalScrollBar()->value();
    v->setPlainText(text);
    v->verticalScrollBar()->setValue(follow ? v->verticalScrollBar()->maximum() : top);
    v->horizontalScrollBar()->setValue(left);
}

/* A stretch of one line of a view in a format. */
QTextEdit::ExtraSelection spanSelection(QPlainTextEdit *v, int line, int from, int length, const QTextCharFormat &fmt)
{
    QTextEdit::ExtraSelection s;
    const QTextBlock block = v->document()->findBlockByNumber(line);
    s.cursor = QTextCursor(block);
    s.cursor.setPosition(block.position() + from);
    s.cursor.setPosition(block.position() + from + length, QTextCursor::KeepAnchor);
    s.format = fmt;
    return s;
}

/* COCO_DEBUGGER_TAB: a page number, or a tab's name or label (any case), or
 * the start of one, or of a word of a label ("i/o" ...). The name of the tab
 * the older window had still reaches its successor. */
int tabFromEnv(const char *var = "COCO_DEBUGGER_TAB")
{
    const QString tab = qEnvironmentVariable(var).trimmed();
    if (tab.isEmpty()) return -1;
    if (tab.compare(QLatin1String("io"), Qt::CaseInsensitive) == 0) return TAB_CART;
    if (tab.compare(QLatin1String("prompt"), Qt::CaseInsensitive) == 0) return TAB_CONSOLE;
    if (tab.compare(QLatin1String("cpu"), Qt::CaseInsensitive) == 0) return TAB_DISASM;
    if (tab.compare(QLatin1String("source"), Qt::CaseInsensitive) == 0) return TAB_SOURCE;
    bool isNum = false;
    const int n = tab.toInt(&isNum);
    if (isNum) return n >= 0 && n < TAB_COUNT ? n : -1;
    for (int pass = 0; pass < 3; ++pass)
        for (int i = 0; i < TAB_COUNT; ++i) {
            const QString name = QString::fromUtf8(kTabs[i].name), label = QString::fromUtf8(kTabs[i].label);
            if (pass == 0 && name.startsWith(tab, Qt::CaseInsensitive)) return i;
            if (pass == 1 && label.startsWith(tab, Qt::CaseInsensitive)) return i;
            if (pass == 2) {
                const QStringList words = label.split(QLatin1Char(' '));
                for (int w = 1; w < words.size(); ++w)
                    if (words[w].startsWith(tab, Qt::CaseInsensitive)) return i;
            }
        }
    return -1;
}

/* The family's keys: F5 run/stop, F6 step line (Shift: over), F7 step, F8 step
 * over, Shift+F8 step out, F9 breakpoint, F12 close. */
bool isDebuggerKey(const QKeyEvent *e)
{
    const Qt::KeyboardModifiers mods = e->modifiers() & ~Qt::KeypadModifier;
    switch (e->key()) {
    case Qt::Key_F5: case Qt::Key_F7: case Qt::Key_F9: case Qt::Key_F12: return mods == Qt::NoModifier;
    case Qt::Key_F6: case Qt::Key_F8: return mods == Qt::NoModifier || mods == Qt::ShiftModifier;
    default: return false;
    }
}

QPointer<DebuggerWindow> s_win;

/* The breakpoint type of a chooser row. */
int bpTypeAt(int row)
{
    return kBpTypes[qBound(0, row, 3)].type;
}

/* A gutter dot: on, off (disabled) or blank. */
QString dotFor(int state)
{
    return state == 1 ? QStringLiteral("●") : state == 2 ? QStringLiteral("○") : QStringLiteral(" ");
}

/* The current line of a listing: a bar in the accent. The bar is the block's
 * own background (a full-width ExtraSelection would repaint the gutter's dot);
 * the ink is a selection over all but the gutter column. `prev` is the block
 * wearing the bar, so a refresh that kept the text can move it. */
void barOn(QList<QTextEdit::ExtraSelection> &out, QPlainTextEdit *v, int &prev, int block)
{
    QTextDocument *doc = v->document();
    auto setBg = [doc](int n, const QBrush &brush) {
        const QTextBlock b = doc->findBlockByNumber(n);
        if (!b.isValid()) return;
        QTextCursor c(b);
        QTextBlockFormat f = c.blockFormat();
        f.setBackground(brush);
        c.setBlockFormat(f);
    };
    if (prev >= 0 && prev != block) setBg(prev, Qt::NoBrush);
    prev = -1;
    if (block < 0) return;
    setBg(block, accentColor());
    prev = block;
    QTextEdit::ExtraSelection ink;
    const QTextBlock b = doc->findBlockByNumber(block);
    ink.cursor = QTextCursor(b);
    ink.cursor.setPosition(b.position() + 1);
    ink.cursor.setPosition(b.position() + b.length() - 1, QTextCursor::KeepAnchor);
    ink.format.setForeground(accentInk());
    out.append(ink);
}

} // namespace

/* ---- the painted panel -------------------------------------------------------- */

/* The sound writes as a wave: the DAC's level and the one-bit output over the
 * most recent stretch, the newest at the right edge. */
class SoundPlot : public QWidget {
public:
    explicit SoundPlot(QWidget *parent = nullptr) : QWidget(parent)
    {
        setMinimumSize(360, 150);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }
    /* newest first, as cocodebug_sound_history gives them */
    void set(const std::vector<cocodebug_sound_write> &history)
    {
        if (history.size() == m_history.size() &&
            (history.empty() || (history[0].tick == m_history[0].tick && history[0].value == m_history[0].value)))
            return;
        m_history = history;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const QPalette &pal = palette();
        p.fillRect(rect(), pal.color(QPalette::Base));
        const int left = 44, top = 8;
        const QRect plot(left, top, width() - left - 8, height() - top - 22);
        const int dacHeight = plot.height() * 7 / 10, bitTop = plot.top() + dacHeight + 8;
        p.setPen(pal.color(QPalette::Mid));
        p.drawRect(plot);
        p.setPen(pal.color(QPalette::Text));
        p.drawText(QRect(0, plot.top(), left - 4, 14), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("63"));
        p.drawText(QRect(0, plot.top() + dacHeight - 14, left - 4, 14), Qt::AlignRight | Qt::AlignVCenter,
                   QStringLiteral("0"));
        p.drawText(QRect(0, bitTop - 2, left - 4, 14), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("bit"));
        p.drawText(QRect(plot.left(), plot.bottom() + 4, plot.width(), 16), Qt::AlignRight | Qt::AlignVCenter,
                   m_history.empty() ? QString() : QStringLiteral("last %1 ms").arg(spanTicks() * 1000.0 / kTicksPerSecond, 0, 'f', 1));
        if (m_history.empty()) {
            p.drawText(plot, Qt::AlignCenter, QStringLiteral("No writes to the DAC or the one-bit output yet"));
            return;
        }
        const uint32_t newest = m_history[0].tick;
        struct Ev { uint32_t age; int dac, bit; };
        std::vector<Ev> events;
        int bitState = 0;
        /* oldest first, so the levels before the window are known */
        for (int i = (int)m_history.size() - 1; i >= 0; --i) {
            const cocodebug_sound_write &w = m_history[(size_t)i];
            if (w.port == 2) bitState = (w.value >> 1) & 1;
            events.push_back({ newest - w.tick, w.dac, bitState });
        }
        const uint32_t span = spanTicks();
        auto xOf = [&](uint32_t age) {
            return plot.right() - (int)((double)qMin<uint32_t>(age, span) * plot.width() / span);
        };
        auto yDac = [&](int level) { return plot.top() + dacHeight - 1 - level * (dacHeight - 2) / 63; };
        auto yBit = [&](int on) { return on ? bitTop : plot.bottom() - 4; };
        QPainterPath dac, bit;
        dac.moveTo(plot.left(), yDac(events[0].dac));
        bit.moveTo(plot.left(), yBit(events[0].bit));
        int lastDac = events[0].dac, lastBit = events[0].bit;
        for (const Ev &ev : events) {
            const int x = xOf(ev.age);
            dac.lineTo(x, yDac(lastDac));
            dac.lineTo(x, yDac(ev.dac));
            bit.lineTo(x, yBit(lastBit));
            bit.lineTo(x, yBit(ev.bit));
            lastDac = ev.dac;
            lastBit = ev.bit;
        }
        dac.lineTo(plot.right(), yDac(lastDac));
        bit.lineTo(plot.right(), yBit(lastBit));
        p.setClipRect(plot.adjusted(1, 1, -1, -1));
        p.setPen(QPen(accentTextColor(this), 1.5));
        p.drawPath(dac);
        p.setPen(QPen(QColor(0x1c, 0x71, 0xd8), 1.5));
        p.drawPath(bit);
    }

private:
    static constexpr uint32_t kTicksPerSecond = 14318180;
    /* What the plot covers: all the recorded writes, but not less than 2 ms
     * nor more than a fifteenth of a second. */
    uint32_t spanTicks() const
    {
        if (m_history.empty()) return kTicksPerSecond / 30;
        const uint32_t recorded = m_history.front().tick - m_history.back().tick;
        return qBound<uint32_t>(kTicksPerSecond / 500, recorded, kTicksPerSecond / 15);
    }
    std::vector<cocodebug_sound_write> m_history;
};

void DebuggerWindow::show(QWidget *parent, cocosession *session)
{
    if (!s_win) s_win = new DebuggerWindow(session, parent);
    /* showEvent engages, which stops the machine */
    s_win->QMainWindow::show();
    s_win->raise();
    s_win->activateWindow();
}

void DebuggerWindow::toggle(QWidget *parent, cocosession *session)
{
    if (s_win && s_win->isVisible()) s_win->hide();
    else show(parent, session);
}

DebuggerWindow::DebuggerWindow(cocosession *session, QWidget *parent)
    : QMainWindow(parent, Qt::Window), m_session(session), m_dbg(cocosession_debugger(session))
{
    setObjectName(QStringLiteral("debugger"));
    setWindowTitle(QStringLiteral("CoCo Debugger"));
    resize(1400, 820);

    auto *central = new QWidget;
    auto *root = new QVBoxLayout(central);
    root->addWidget(buildToolbar());
    m_tabs = new QTabWidget;
    m_tabs->setUsesScrollButtons(true);
    m_pages[TAB_CONSOLE] = buildConsole();
    m_pages[TAB_DISASM] = buildDisasm();
    m_pages[TAB_SOURCE] = buildSource();
    m_pages[TAB_MEMORY] = buildMemory();
    m_pages[TAB_BREAKS] = buildBreaks();
    m_pages[TAB_WATCHES] = buildWatches();
    m_pages[TAB_CART] = buildCart();
    m_pages[TAB_TRACE] = buildHist(0);
    m_pages[TAB_HISTORY] = buildHist(1);
    m_pages[TAB_VIDEO] = buildVideo();
    m_pages[TAB_SOUND] = buildSound();
    m_pages[TAB_SYMBOLS] = buildSymbols();
    m_pages[TAB_FUJINET] = buildFujinet();
    for (int i = 0; i < TAB_COUNT; ++i) {
        /* a page's place in the tab widget is not its identity once tabs move
         * into windows of their own: each carries its own number */
        m_pages[i]->setProperty("tab", i);
        m_tabs->addTab(m_pages[i], QString::fromUtf8(kTabs[i].label).replace(QLatin1Char('&'), QLatin1String("&&")));
    }
    const int tab = tabFromEnv();
    if (tab >= 0) m_tabs->setCurrentIndex(tab);
    /* Help's contexts, one per tab */
    auto setHelpContext = [this](int page) {
        if (page >= 0 && page < TAB_COUNT)
            setProperty("cocoHelpContext", QStringLiteral("debugger/%1").arg(QLatin1String(kTabs[page].help)));
    };
    setHelpContext(currentTab());
    new QShortcut(QKeySequence(Qt::Key_F1), this, [this] { cocoHelpFor(this); });
    connect(m_tabs, &QTabWidget::currentChanged, this, [this, setHelpContext](int) {
        const int page = currentTab();
        setHelpContext(page);
        if (page == TAB_VIDEO) m_videoForce = true;
        if (page == TAB_HISTORY) m_hSeen[1] = -1;
        if (page == TAB_SOURCE) m_srcSig.clear();
        refreshPage(page, false);
        watchSound();
        if (page == TAB_CONSOLE) m_conIn->setFocus();
    });
    root->addWidget(m_tabs, 1);
    root->addWidget(buildRegs());
    setCentralWidget(central);
    statusBar();

    /* for captures and tests: COCO_WATCH adds watches (a comma list),
     * COCO_WATCH_BREAK a watch with a break-on-change over it, COCO_MEM_GOTO
     * shows a range in Memory, COCO_HISTORY records the trace and COCO_POPOUT
     * peels a tab off */
    {
        const QByteArray mg = qgetenv("COCO_MEM_GOTO");
        const QString wb = qEnvironmentVariable("COCO_WATCH_BREAK").trimmed();
        for (const QString &text : qEnvironmentVariable("COCO_WATCH").split(QLatin1Char(','), Qt::SkipEmptyParts))
            if (!text.trimmed().isEmpty()) cocowatch_add(text.trimmed().toUtf8().constData(), COCODEBUG_SPACE_CPU);
        if (!wb.isEmpty()) {
            const int index = cocowatch_add(wb.toUtf8().constData(), COCODEBUG_SPACE_CPU);
            if (index >= 0) watchBreak(index);
        }
        uint32_t lo, hi;
        if (!mg.isEmpty() && cocosym_parse_range(m_dbg, COCODEBUG_SPACE_CPU, mg.constData(), &lo, &hi))
            showInMemory(COCODEBUG_SPACE_CPU, lo, hi);
        if (tabFromEnv("COCO_POPOUT") >= 0) popOut(tabFromEnv("COCO_POPOUT"));
    }

    /* the timer runs only while the window is shown (showEvent/hideEvent) */
    connect(&m_timer, &QTimer::timeout, this, [this] { tick(); });
}

/* Only at exit, with the main window: the session outlives its windows. */
DebuggerWindow::~DebuggerWindow()
{
    m_closing = true;       /* a peeled-off tab's window goes with the debugger */
    for (QWidget *&win : m_popped)
        if (win) {
            QWidget *w = win;
            win = nullptr;
            w->close();
        }
    if (cocodebug_is_engaged(m_dbg)) {
        cocodebug_sound_watch(m_dbg, 0);
        cocodebug_set_engaged(m_dbg, 0);
    }
}

/* ---- building ------------------------------------------------------------- */

/* Three rows: the stepping controls, Pop Out and Help; then Break at, the
 * symbols and the C source; then the status line, which has the width of the
 * window and is never cut. */
QWidget *DebuggerWindow::buildToolbar()
{
    auto *bar = new QWidget;
    auto *rows = new QVBoxLayout(bar);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(2);
    auto *h = new QHBoxLayout;
    h->setContentsMargins(0, 0, 0, 0);
    rows->addLayout(h);
    auto add = [&](const QString &label, const QString &tip, std::function<void()> fn) {
        auto *b = new QPushButton(label);
        b->setFocusPolicy(Qt::NoFocus);
        b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, fn);
        h->addWidget(b);
        return b;
    };
    /* Run is a checkable button, checked while the machine is stopped: its
     * stylesheet gives that the accent. */
    m_runBtn = add(QStringLiteral("Pause (F5)"), QStringLiteral("Run, or pause before the next instruction"),
                   [this] { toggleRun(); });
    m_runBtn->setCheckable(true);
    m_runBtn->setStyleSheet(QStringLiteral("QPushButton:checked { background:%1; color:%2; }")
                                .arg(accentColor().name(), accentInk().name()));
    auto *step = add(QStringLiteral("Step (F7)"), QStringLiteral("One instruction"),
                     [this] { stepAnd([this] { cocodebug_step(m_dbg); }); });
    add(QStringLiteral("Step Over (F8)"), QStringLiteral("Over a subroutine call (BSR, LBSR, JSR)"),
        [this] { stepAnd([this] { cocodebug_step_over(m_dbg); }); });
    add(QStringLiteral("Step Out (Shift+F8)"), QStringLiteral("On to the routine's return"),
        [this] { stepAnd([this] { cocodebug_step_out(m_dbg); }); });
    m_stepLine = add(QStringLiteral("Step Line (F6)"),
                     QStringLiteral("Run until a different C source line. Needs C source info loaded."),
                     [this] { stepAnd([this] { cocodebug_step_line(m_dbg); }); });
    m_stepOverLine = add(QStringLiteral("Step Over Line (Shift+F6)"),
                         QStringLiteral("Like Step Line, but a call on this line runs to completion rather than "
                                        "being stepped into"),
                         [this] { stepAnd([this] { cocodebug_step_over_line(m_dbg); }); });
    add(QStringLiteral("Run to Cursor"), QStringLiteral("On to the line selected in the Disassembly"),
        [this] { runToCursor(); });
    add(QStringLiteral("Clear Breakpoints"), QStringLiteral("Remove every breakpoint"), [this] {
        clearBreakpoints();
        resync();
    });
    fixToggleSize(m_runBtn, { QStringLiteral("Pause (F5)"), QStringLiteral("Run (F5)") }, step->sizeHint().height());
    h->addStretch(1);
    auto *pop = new QPushButton(QStringLiteral("Pop Out"));
    pop->setFocusPolicy(Qt::NoFocus);
    pop->setToolTip(QStringLiteral("Move the tab that shows into a window of its own; Dock puts it back"));
    connect(pop, &QPushButton::clicked, this, [this] { popOut(currentTab()); });
    h->addWidget(pop);
    auto *help = qobject_cast<QToolButton *>(cocoHelpButton(QString()));
    if (help) {
        help->setAutoRaise(false);
        help->setText(QStringLiteral("Help"));
        help->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    }
    h->addWidget(help);

    auto *second = new QHBoxLayout;
    second->setContentsMargins(0, 0, 0, 0);
    rows->addLayout(second);
    second->addWidget(new QLabel(QStringLiteral("Break at:")));
    m_breakAt = smallEntry(QStringLiteral("symbol or $addr"), 18);
    m_breakAt->setToolTip(QStringLiteral("A symbol or a hex address, then Enter: an execute breakpoint there"));
    connect(m_breakAt, &QLineEdit::returnPressed, this, [this] { breakAt(); });
    second->addWidget(m_breakAt);

    auto addSecond = [&](const QString &label, const QString &tip, std::function<void()> fn) {
        auto *b = new QPushButton(label);
        b->setFocusPolicy(Qt::NoFocus);
        b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, fn);
        second->addWidget(b);
        return b;
    };
    addSecond(QStringLiteral("Load Symbols…"),
              QStringLiteral("Load a symbol file (LWASM --symbol-dump, or plain \"NAME ADDR\" lines) to label the "
                             "disassembly"),
              [this] { loadSymbols(); });
    addSecond(QStringLiteral("Clear Symbols"),
              QStringLiteral("Drop loaded symbols, keeping the built-in Color BASIC entry points"),
              [this] { clearSymbols(); });
    addSecond(QStringLiteral("Load C Source…"),
              QStringLiteral("Load one or more CMOC listings (.lst, from `cmoc -i`) for source-level stepping and "
                             "locals -- select every module of a multi-file program at once. The shared .map (also "
                             "only kept by -i) is found automatically."),
              [this] { loadSource(); });
    addSecond(QStringLiteral("Clear C Source"), QStringLiteral("Drop loaded C source info"), [this] { clearSource(); });

    second->addStretch(1);

    /* the status line has a row of its own: its text is never cut or wrapped */
    m_status = new QLabel;
    m_status->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_status->setStyleSheet(kGray);
    m_status->setContentsMargins(2, 0, 2, 0);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_status->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    rows->addWidget(m_status);
    return bar;
}

QWidget *DebuggerWindow::buildConsole()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    m_conOut = monoView();
    m_conOut->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_conOut->setMaximumBlockCount(CONSOLE_LINES);
    m_conOut->setPlainText(QStringLiteral("Debugger console. Type 'help' for every command.\n"));
    m_conIn = new QLineEdit;
    setMono(m_conIn);
    m_conIn->setPlaceholderText(QStringLiteral("command (help, step, break, bpr, bpw, bpa, print, mem, poke, "
                                               "runto, disasm, line, video, cart ...) — Tab completes, Up/Down recall"));
    m_conIn->installEventFilter(this);
    connect(m_conIn, &QLineEdit::returnPressed, this, [this] { runConsole(); });
    v->addWidget(m_conOut, 1);
    v->addWidget(m_conIn);
    return w;
}

/* The registers, under the tabs and so on every one of them: PC A B D DP X Y
 * U S CC on the first row (Enter sets one), then CC's eight flags, and on a
 * 6309 its extra registers. */
QWidget *DebuggerWindow::buildRegs()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);

    auto *regs = new QHBoxLayout;
    regs->setSpacing(6);
    auto addReg = [&](QHBoxLayout *row, int reg, const QString &name, int digits, const QString &tip) {
        QLabel *label = m_regLabel[reg] = new QLabel(name);
        label->setStyleSheet(kGray);
        row->addWidget(label);
        QLineEdit *e = m_reg[reg] = new QLineEdit;
        setMono(e);
        e->setMaxLength(12);
        setDigits(e, digits);
        e->setToolTip(tip + QStringLiteral(" (Enter sets it, hex or an expression; only while stopped)"));
        connect(e, &QLineEdit::returnPressed, this, [this, reg] { setReg(reg); });
        row->addWidget(e);
    };
    for (const auto &r : kRegs) addReg(regs, r.id, QString::fromUtf8(r.name), r.digits, QString::fromUtf8(r.tip));
    regs->addStretch(1);
    m_machine = new QLabel;
    m_machine->setStyleSheet(kGray);
    regs->addWidget(m_machine);
    v->addLayout(regs);

    auto *flags = new QHBoxLayout;
    flags->setSpacing(6);
    auto *ccLabel = new QLabel(QStringLiteral("CC"));
    ccLabel->setStyleSheet(kGray);
    flags->addWidget(ccLabel);
    for (int i = 0; i < 8; ++i) {
        m_cc[i] = new QCheckBox(QString::fromUtf8(kCc[i].name));
        m_cc[i]->setFocusPolicy(Qt::NoFocus);
        m_cc[i]->setToolTip(QString::fromUtf8(kCc[i].tip) + QStringLiteral(" (click to flip it; only while stopped)"));
        const unsigned mask = kCc[i].mask;
        connect(m_cc[i], &QCheckBox::toggled, this, [this, mask](bool on) { setFlag(mask, on); });
        flags->addWidget(m_cc[i]);
    }
    flags->addSpacing(16);
    m_regs6309 = new QWidget;
    auto *extra = new QHBoxLayout(m_regs6309);
    extra->setContentsMargins(0, 0, 0, 0);
    extra->setSpacing(6);
    auto *tag = new QLabel(QStringLiteral("6309"));
    tag->setStyleSheet(kGray);
    extra->addWidget(tag);
    for (const auto &r : kRegs6309) addReg(extra, r.id, QString::fromUtf8(r.name), r.digits, QString::fromUtf8(r.tip));
    flags->addWidget(m_regs6309);
    m_regs6309->hide();
    flags->addStretch(1);
    v->addLayout(flags);
    return w;
}

/* The dump, the space chooser, Go to (a label, address or range, which it
 * marks), the write control, and the search bar. */
QWidget *DebuggerWindow::buildMemory()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);

    auto *head = new QHBoxLayout;
    head->addWidget(titleLabel(QStringLiteral("Memory")));
    m_memSpace = new QComboBox;
    m_memSpace->addItem(QStringLiteral("CPU (the 64K the CPU sees)"), COCODEBUG_SPACE_CPU);
    m_memSpace->setToolTip(QStringLiteral("CPU: the 64K as the SAM or the GIME's MMU map it now. PHYS (CoCo 3): the "
                                          "physical RAM, whichever way the MMU maps it"));
    connect(m_memSpace, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) {
        if (m_memFilling || i < 0) return;
        memSpaceChanged(m_memSpace->itemData(i).toInt());
    });
    head->addWidget(m_memSpace);
    head->addStretch(1);
    v->addLayout(head);

    auto *dump = new QHBoxLayout;
    dump->setSpacing(0);
    m_mem = monoView();
    /* no scroll bar of its own: the dump is a window of rows over the space */
    m_mem->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_mem->viewport()->installEventFilter(this);
    m_memBar = new QScrollBar(Qt::Vertical);
    connect(m_memBar, &QScrollBar::valueChanged, this, [this](int row) {
        if (m_memFilling) return;
        m_memTop = (uint32_t)row;
        refreshMem();
    });
    dump->addWidget(m_mem, 1);
    dump->addWidget(m_memBar);
    v->addLayout(dump, 1);

    auto *nav = new QHBoxLayout;
    nav->addWidget(new QLabel(QStringLiteral("Go to")));
    m_memGoto = smallEntry(QStringLiteral("$0400, label or lo-hi"), 20);
    connect(m_memGoto, &QLineEdit::returnPressed, this, [this] { memGoto(); });
    nav->addWidget(m_memGoto);
    nav->addSpacing(12);
    nav->addWidget(new QLabel(QStringLiteral("Poke")));
    m_memAddr = smallEntry(QStringLiteral("$0400"), 10);
    m_memVal = smallEntry(QStringLiteral("$00"), 5);
    connect(m_memAddr, &QLineEdit::returnPressed, this, [this] { memWrite(); });
    connect(m_memVal, &QLineEdit::returnPressed, this, [this] { memWrite(); });
    nav->addWidget(m_memAddr);
    nav->addWidget(new QLabel(QStringLiteral("value")));
    nav->addWidget(m_memVal);
    nav->addWidget(hintLabel(QStringLiteral("RAM takes it; ROM and I/O do not")), 1);
    v->addLayout(nav);
    v->addWidget(buildSearchBar());
    return w;
}

QWidget *DebuggerWindow::buildSearchBar()
{
    auto *bar = new QWidget;
    auto *row = new QHBoxLayout(bar);
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(new QLabel(QStringLiteral("Search")));
    m_srchKind = new QComboBox;
    m_srchKind->addItems({ QStringLiteral("Hex"), QStringLiteral("Text") });
    m_srchKind->setToolTip(QStringLiteral("Hex bytes (3E 5A) or text, one byte per character"));
    row->addWidget(m_srchKind);
    m_srchEntry = new QLineEdit;
    setMono(m_srchEntry);
    m_srchEntry->setPlaceholderText(QStringLiteral("bytes or text to find"));
    connect(m_srchEntry, &QLineEdit::returnPressed, this, [this] { memSearch(false); });
    row->addWidget(m_srchEntry, 1);
    auto *next = new QPushButton(QStringLiteral("Find Next"));
    auto *prev = new QPushButton(QStringLiteral("Find Previous"));
    connect(next, &QPushButton::clicked, this, [this] { memSearch(false); });
    connect(prev, &QPushButton::clicked, this, [this] { memSearch(true); });
    row->addWidget(next);
    row->addWidget(prev);
    m_srchNote = new QLabel;
    m_srchNote->setStyleSheet(kGray);
    row->addWidget(m_srchNote);
    return bar;
}

QWidget *DebuggerWindow::buildDisasm()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *row = new QHBoxLayout;
    m_followPc = new QCheckBox(QStringLiteral("Follow PC"));
    m_followPc->setChecked(true);
    connect(m_followPc, &QCheckBox::clicked, this, [this] { refreshDisasm(); });
    m_jump = smallEntry(QStringLiteral("$address or label"), 20);
    connect(m_jump, &QLineEdit::returnPressed, this, [this] { jumpTo(); });
    row->addWidget(m_followPc);
    row->addSpacing(8);
    row->addWidget(new QLabel(QStringLiteral("Jump to")));
    row->addWidget(m_jump);
    row->addSpacing(8);
    row->addWidget(hintLabel(QStringLiteral("Click a line to select it (Run to Cursor); click its left edge, "
                                            "double-click it or press F9 for a breakpoint; scroll to browse")), 1);
    v->addLayout(row);
    m_disasm = monoView();
    /* no scrollbar: the wheel scrolls by instructions, as many lines as fit */
    m_disasm->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_disasm->setFocusPolicy(Qt::NoFocus);
    m_disasm->viewport()->setCursor(Qt::ArrowCursor);
    m_disasm->viewport()->installEventFilter(this);
    v->addWidget(m_disasm, 1);
    return w;
}

/* A CMOC listing with a gutter dot and a line number for every line, the
 * current function's locals beside it. */
QWidget *DebuggerWindow::buildSource()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *bar = new QHBoxLayout;
    auto *load = new QPushButton(QStringLiteral("Load C Source…"));
    load->setToolTip(QStringLiteral("Load one or more CMOC listings (.lst, from `cmoc -i`)"));
    connect(load, &QPushButton::clicked, this, [this] { loadSource(); });
    bar->addWidget(load);
    auto *clear = new QPushButton(QStringLiteral("Clear C Source"));
    connect(clear, &QPushButton::clicked, this, [this] { clearSource(); });
    bar->addWidget(clear);
    m_srcFiles = new QComboBox;
    m_srcFiles->setToolTip(QStringLiteral("The source file shown"));
    m_srcFiles->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_srcFiles->setMinimumContentsLength(24);
    connect(m_srcFiles, QOverload<int>::of(&QComboBox::activated), this, [this](int i) { sourceFileChosen(i); });
    bar->addWidget(m_srcFiles);
    bar->addSpacing(8);
    bar->addWidget(hintLabel(QStringLiteral("Click a line for its breakpoint. Step Line (F6) and Step Over Line "
                                            "(Shift+F6) go by C line.")), 1);
    layout->addLayout(bar);
    m_srcNote = new QLabel;
    m_srcNote->setStyleSheet(kGray);
    m_srcNote->setWordWrap(true);
    layout->addWidget(m_srcNote);
    auto *split = new QSplitter(Qt::Horizontal);
    m_srcView = monoView();
    m_srcView->setObjectName(QStringLiteral("sourceView"));
    m_srcView->setFocusPolicy(Qt::NoFocus);
    m_srcView->viewport()->setCursor(Qt::ArrowCursor);
    m_srcView->viewport()->installEventFilter(this);
    split->addWidget(m_srcView);
    auto *right = new QWidget;
    auto *rv = new QVBoxLayout(right);
    rv->setContentsMargins(0, 0, 0, 0);
    rv->addWidget(titleLabel(QStringLiteral("Parameters and locals")));
    m_srcLocals = monoView();
    m_srcLocals->setObjectName(QStringLiteral("sourceLocals"));
    rv->addWidget(m_srcLocals, 1);
    right->setMinimumWidth(380);
    split->addWidget(right);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
    split->setSizes({ 900, 420 });
    split->setChildrenCollapsible(false);
    layout->addWidget(split, 1);
    return page;
}

QWidget *DebuggerWindow::buildVideo()
{
    m_video = new VideoTab(m_dbg);
    return m_video;
}

QWidget *DebuggerWindow::buildBreaks()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);

    m_bpStop = new QLabel;
    m_bpStop->setStyleSheet(QStringLiteral("font-weight: bold;"));
    m_bpStop->setWordWrap(true);
    v->addWidget(m_bpStop);

    v->addWidget(hintLabel(QStringLiteral("Execute stops before the instruction runs; the others after the "
                                          "access. A condition may use the registers, old and value (a write's "
                                          "byte and what it replaced). On a CoCo 3, Physical makes the range "
                                          "physical RAM addresses, whichever way the MMU maps them. Breakpoints "
                                          "are kept while the debugger is closed, but only act while it is open.")));

    m_bpTable = new QTableWidget(0, 4);
    m_bpTable->setHorizontalHeaderLabels({ QStringLiteral("On"), QStringLiteral("Breakpoint"), QStringLiteral("Hits"),
                                           QString() });
    m_bpTable->verticalHeader()->setVisible(false);
    m_bpTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_bpTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_bpTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_bpTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_bpTable->setFocusPolicy(Qt::NoFocus);
    connect(m_bpTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (m_bpFilling || item->column() != 0) return;
        cocodebug_breakpoint_enable(m_dbg, item->data(Qt::UserRole).toInt(), item->checkState() == Qt::Checked);
        syncLater();
    });
    v->addWidget(m_bpTable, 1);
    m_bpEmpty = hintLabel(QStringLiteral("No breakpoints. Click the left edge of a disassembly line, or add one below."));
    v->addWidget(m_bpEmpty);

    auto *row = new QHBoxLayout;
    m_bpType = new QComboBox;
    for (const auto &entry : kBpTypes) m_bpType->addItem(QString::fromUtf8(entry.label));
    m_bpPhys = new QCheckBox(QStringLiteral("Physical"));
    m_bpPhys->setToolTip(QStringLiteral("CoCo 3: the range is physical RAM addresses (Read, Write and Access only)"));
    m_bpPhys->setEnabled(false);
    m_bpStart = smallEntry(QStringLiteral("address, range or symbol"), 24);
    m_bpEnd = smallEntry(QStringLiteral("end (optional)"), 16);
    m_bpCond = new QLineEdit;
    m_bpCond->setPlaceholderText(QStringLiteral("condition (optional, e.g. a == 5 && value != old)"));
    auto *add = new QPushButton(QStringLiteral("Add"));
    auto *clear = new QPushButton(QStringLiteral("Clear Breakpoints"));
    auto *clearHits = new QPushButton(QStringLiteral("Clear Hits"));
    connect(clearHits, &QPushButton::clicked, this, [this] {
        cocodebug_breakpoint_clear_hits(m_dbg);
        resync();
    });
    connect(add, &QPushButton::clicked, this, [this] { addBreakpoint(); });
    connect(clear, &QPushButton::clicked, this, [this] {
        clearBreakpoints();
        resync();
    });
    connect(m_bpType, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { syncSpaces(); });
    for (QLineEdit *e : { m_bpStart, m_bpEnd, m_bpCond })
        connect(e, &QLineEdit::returnPressed, this, [this] { addBreakpoint(); });
    row->addWidget(m_bpType);
    row->addWidget(m_bpPhys);
    row->addWidget(m_bpStart);
    row->addWidget(m_bpEnd);
    row->addWidget(m_bpCond, 1);
    row->addWidget(add);
    row->addWidget(clearHits);
    row->addWidget(clear);
    v->addLayout(row);

    m_bpMsg = new QLabel;
    m_bpMsg->setWordWrap(true);
    m_bpMsg->setStyleSheet(QStringLiteral("color: #e01b24;"));
    v->addWidget(m_bpMsg);
    return w;
}

QWidget *DebuggerWindow::buildWatches()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(QStringLiteral("Watch")));
    m_wtEntry = new QLineEdit;
    setMono(m_wtEntry);
    m_wtEntry->setPlaceholderText(QStringLiteral("address, label, expression or lo-hi"));
    connect(m_wtEntry, &QLineEdit::returnPressed, this, [this] { addWatch(m_wtEntry->text()); });
    m_wtSpace = new QComboBox;
    m_wtSpace->addItem(QStringLiteral("CPU"), COCODEBUG_SPACE_CPU);
    m_wtSpace->setToolTip(QStringLiteral("The space the watch reads: CPU, or (CoCo 3) physical RAM"));
    auto *add = new QPushButton(QStringLiteral("Add"));
    connect(add, &QPushButton::clicked, this, [this] { addWatch(m_wtEntry->text()); });
    row->addWidget(m_wtEntry, 1);
    row->addWidget(m_wtSpace);
    row->addWidget(add);
    v->addLayout(row);

    m_wtTable = new QTableWidget(0, 2);
    m_wtTable->setHorizontalHeaderLabels({ QStringLiteral("Watch"), QString() });
    m_wtTable->verticalHeader()->setVisible(false);
    m_wtTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_wtTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_wtTable->setColumnWidth(1, 350);
    m_wtTable->verticalHeader()->setDefaultSectionSize(34);
    m_wtTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_wtTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_wtTable->setFocusPolicy(Qt::NoFocus);
    v->addWidget(m_wtTable, 1);
    v->addWidget(hintLabel(QStringLiteral("The values are read at a stop and show - while the machine runs. A watch "
                                          "is kept as text, so a label that loads later starts resolving. "
                                          "Break on change stops after a write that alters the value.")));
    return w;
}

/* The cartridge and the memory map on the left; the chips on the right. */
QWidget *DebuggerWindow::buildCart()
{
    auto *split = new QSplitter(Qt::Horizontal);
    auto *left = new QWidget;
    auto *lv = new QVBoxLayout(left);
    lv->setContentsMargins(0, 0, 0, 0);
    lv->addWidget(titleLabel(QStringLiteral("Cartridge and memory map")));
    m_cartText = monoView();
    m_cartText->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    lv->addWidget(m_cartText, 1);
    auto *right = new QWidget;
    auto *rv = new QVBoxLayout(right);
    rv->setContentsMargins(0, 0, 0, 0);
    rv->addWidget(titleLabel(QStringLiteral("PIAs, SAM, GIME, joysticks and the Becker port")));
    m_io = monoView();
    rv->addWidget(m_io, 1);
    split->addWidget(left);
    split->addWidget(right);
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 3);
    split->setChildrenCollapsible(false);
    auto *page = new QWidget;
    auto *pv = new QVBoxLayout(page);
    pv->addWidget(split);
    return page;
}

/* Trace (idx 0) reads the instruction ring live; History (idx 1) reads it at
 * a stop. They share the one ring and recorder, so the two Record buttons
 * follow each other. */
QWidget *DebuggerWindow::buildHist(int idx)
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *bar = new QHBoxLayout;

    m_hRec[idx] = new QPushButton(QStringLiteral("Record"));
    m_hRec[idx]->setCheckable(true);
    m_hRec[idx]->setFocusPolicy(Qt::NoFocus);
    m_hRec[idx]->setToolTip(QStringLiteral("Record every instruction the CPU runs (the machine runs a little slower)"));
    m_hRec[idx]->setStyleSheet(QStringLiteral("QPushButton:checked { background:%1; color:%2; }")
                                   .arg(accentColor().name(), accentInk().name()));
    fixToggleSize(m_hRec[idx], { QStringLiteral("Record") }, m_hRec[idx]->sizeHint().height());
    connect(m_hRec[idx], &QPushButton::toggled, this, [this](bool on) {
        if (m_hSync) return;
        cocodebug_trace_enable(m_dbg, on);
        for (int i = 0; i < 2; ++i) refreshHist(i, true);
    });
    m_hFilter[idx] = new QComboBox;
    m_hFilter[idx]->addItems({ QStringLiteral("All instructions"), QStringLiteral("Branches only"),
                               QStringLiteral("In range") });
    m_hRange[idx] = new QLineEdit;
    setMono(m_hRange[idx]);
    m_hRange[idx]->setPlaceholderText(QStringLiteral("range: lo-hi or a label"));
    connect(m_hFilter[idx], QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, idx](int) { refreshHist(idx, true); });
    connect(m_hRange[idx], &QLineEdit::returnPressed, this, [this, idx] { refreshHist(idx, true); });
    auto *clear = new QPushButton(QStringLiteral("Clear"));
    connect(clear, &QPushButton::clicked, this, [this] {
        cocodebug_trace_clear(m_dbg);
        for (int i = 0; i < 2; ++i) refreshHist(i, true);
    });
    m_hNote[idx] = new QLabel;
    m_hNote[idx]->setStyleSheet(kGray);
    m_hNote[idx]->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    bar->addWidget(m_hRec[idx]);
    bar->addWidget(m_hFilter[idx]);
    bar->addWidget(m_hRange[idx], 1);
    bar->addWidget(clear);
    bar->addWidget(m_hNote[idx], 1);
    v->addLayout(bar);
    m_hText[idx] = monoView();
    v->addWidget(m_hText[idx], 1);
    return w;
}

QWidget *DebuggerWindow::buildSound()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    m_sound = monoView();
    m_sound->setMaximumHeight(230);
    v->addWidget(m_sound);
    auto *row = new QHBoxLayout;
    row->addWidget(titleLabel(QStringLiteral("Recent writes to the DAC and the one-bit output")));
    row->addStretch(1);
    auto *clear = new QPushButton(QStringLiteral("Clear"));
    connect(clear, &QPushButton::clicked, this, [this] {
        cocodebug_sound_history_clear(m_dbg);
        refreshSound();
    });
    row->addWidget(clear);
    v->addLayout(row);
    m_soundPlot = new SoundPlot;
    v->addWidget(m_soundPlot, 1);
    return w;
}

/* A filter over a table of address and name; activating a row opens that
 * address in the Disassembly. */
QWidget *DebuggerWindow::buildSymbols()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(QStringLiteral("Filter")));
    m_symFilter = new QLineEdit;
    m_symFilter->setPlaceholderText(QStringLiteral("part of a name"));
    m_symFilter->setClearButtonEnabled(true);
    connect(m_symFilter, &QLineEdit::textChanged, this, [this] { refreshSymbols(true); });
    row->addWidget(m_symFilter, 1);
    m_symCount = new QLabel;
    m_symCount->setStyleSheet(kGray);
    row->addWidget(m_symCount);
    auto *load = new QPushButton(QStringLiteral("Load…"));
    load->setToolTip(QStringLiteral("Load a symbol file on top of the symbols already known"));
    connect(load, &QPushButton::clicked, this, [this] { loadSymbols(); });
    auto *clear = new QPushButton(QStringLiteral("Clear"));
    clear->setToolTip(QStringLiteral("Drop every loaded symbol; the built-in ones stay"));
    connect(clear, &QPushButton::clicked, this, [this] { clearSymbols(); });
    auto *brk = new QPushButton(QStringLiteral("Break at"));
    brk->setToolTip(QStringLiteral("An execute breakpoint at the selected symbol"));
    connect(brk, &QPushButton::clicked, this, [this] {
        const int r = m_symTable->currentRow();
        QTableWidgetItem *item = r >= 0 ? m_symTable->item(r, 0) : nullptr;
        if (!item) {
            message(QStringLiteral("Select a symbol first"));
            return;
        }
        toggleBreakpoint(item->data(Qt::UserRole).toInt());
    });
    row->addWidget(load);
    row->addWidget(clear);
    row->addWidget(brk);
    v->addLayout(row);

    m_symTable = new QTableWidget(0, 2);
    m_symTable->setHorizontalHeaderLabels({ QStringLiteral("Address"), QStringLiteral("Name") });
    m_symTable->verticalHeader()->setVisible(false);
    m_symTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_symTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_symTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_symTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_symTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    setMono(m_symTable);
    auto activate = [this](int r) {
        QTableWidgetItem *item = r >= 0 ? m_symTable->item(r, 0) : nullptr;
        if (item) showInDisassembly((uint16_t)item->data(Qt::UserRole).toUInt());
    };
    connect(m_symTable, &QTableWidget::cellDoubleClicked, this, [activate](int r, int) { activate(r); });
    connect(m_symTable, &QTableWidget::itemActivated, this, [activate](QTableWidgetItem *item) { activate(item->row()); });
    v->addWidget(m_symTable, 1);

    auto *addRow = new QHBoxLayout;
    addRow->addWidget(new QLabel(QStringLiteral("Add a label")));
    m_symAddr = smallEntry(QStringLiteral("$address"), 10);
    m_symName = smallEntry(QStringLiteral("name"), 20);
    auto *add = new QPushButton(QStringLiteral("Add"));
    auto doAdd = [this] {
        long a;
        const QString name = m_symName->text().trimmed();
        if (name.isEmpty()) {
            message(QStringLiteral("Give the label a name"));
            return;
        }
        if (!addressOf(m_symAddr->text(), COCODEBUG_SPACE_CPU, &a)) return;
        cocodebug_symbol_add(m_dbg, (uint16_t)a, name.toUtf8().constData());
        m_symName->clear();
        m_symAddr->clear();
        refreshSymbols(true);
        resync();
    };
    connect(add, &QPushButton::clicked, this, doAdd);
    connect(m_symName, &QLineEdit::returnPressed, this, doAdd);
    connect(m_symAddr, &QLineEdit::returnPressed, this, doAdd);
    addRow->addWidget(m_symAddr);
    addRow->addWidget(m_symName);
    addRow->addWidget(add);
    addRow->addStretch(1);
    v->addLayout(addRow);
    v->addWidget(hintLabel(QStringLiteral("Double-click a row (or press Enter on it) to open its address in the "
                                          "Disassembly.")));
    return w;
}

/* The packets left (a note, Hide READY probes and Clear over them), the
 * Becker port's state right. */
QWidget *DebuggerWindow::buildFujinet()
{
    auto *split = new QSplitter(Qt::Horizontal);
    auto *left = new QWidget;
    auto *lv = new QVBoxLayout(left);
    lv->addWidget(titleLabel(QStringLiteral("DriveWire packets")));
    auto *row = new QHBoxLayout;
    m_fujiNote = new QLabel;
    m_fujiNote->setStyleSheet(kGray);
    m_fujiNote->setWordWrap(true);
    m_fujiHideReady = new QCheckBox(QStringLiteral("Hide READY probes"));
    m_fujiHideReady->setChecked(true);
    m_fujiHideReady->setToolTip(QStringLiteral("fujinet-lib asks E2 00 before every call"));
    connect(m_fujiHideReady, &QCheckBox::toggled, this, [this] { refreshFujinet(true); });
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

/* ---- refresh -------------------------------------------------------------- */

/* The one question every refresh asks, answered from the engine's locked
 * snapshot. */
bool DebuggerWindow::stopped()
{
    cocodebug_stop snap;
    cocodebug_stop_snapshot(m_dbg, &snap);
    return snap.paused != 0;
}

void DebuggerWindow::refreshStatus()
{
    cocodebug_stop snap;
    cocodebug_stop_snapshot(m_dbg, &snap);
    const bool stop = snap.paused != 0;
    QString text = QStringLiteral("Running");
    if (stop) {
        text = stoppedText(stripControl(snap.reason));
        /* where, by name when it has one */
        char name[64];
        if (snap.reason_addr >= 0 && cocodebug_symbol_at(m_dbg, (uint16_t)snap.reason_addr, name, sizeof name) && name[0])
            text += QStringLiteral("  (%1)").arg(QString::fromUtf8(name));
    }
    if (m_status->text() != text) {
        m_status->setText(text);
        m_status->setToolTip(text);
    }
    const QString run = stop ? QStringLiteral("Run (F5)") : QStringLiteral("Pause (F5)");
    if (m_runBtn->text() != run) m_runBtn->setText(run);
    if (m_runBtn->isChecked() != stop) m_runBtn->setChecked(stop);
    const bool source = cocodebug_source_available(m_dbg) != 0;
    m_stepLine->setEnabled(source);
    m_stepOverLine->setEnabled(source);
}

/* The refresh never overwrites a value being typed. */
void DebuggerWindow::setEntry(QLineEdit *e, const QString &text)
{
    if (e->hasFocus()) return;
    if (e->text() != text) e->setText(text);
}

/* Which spaces the machine has: PHYS only on a CoCo 3. The choosers, the
 * Physical box and the Memory tab follow. */
void DebuggerWindow::syncSpaces()
{
    const bool phys = cocodebug_space_size(m_dbg, COCODEBUG_SPACE_PHYS) > 0;
    for (QComboBox *combo : { m_memSpace, m_wtSpace }) {
        if ((combo->count() == 2) == phys) continue;
        const QSignalBlocker block(combo);
        const bool memory = combo == m_memSpace;
        m_memFilling = true;
        if (phys) {
            combo->addItem(memory ? QStringLiteral("PHYS (physical RAM)") : QStringLiteral("PHYS"),
                           COCODEBUG_SPACE_PHYS);
        } else {
            combo->removeItem(1);
            combo->setCurrentIndex(0);
            if (memory) m_space = COCODEBUG_SPACE_CPU;
        }
        m_memFilling = false;
    }
    if (!phys && m_space != COCODEBUG_SPACE_CPU) m_space = COCODEBUG_SPACE_CPU;
    m_bpPhys->setEnabled(phys && bpTypeAt(m_bpType->currentIndex()) != COCODEBUG_BP_EXEC);
    if (!phys) m_bpPhys->setChecked(false);
}

void DebuggerWindow::refreshCpu()
{
    syncSpaces();
    cocodebug_regs r;
    const bool ok = cocodebug_regs_get(m_dbg, &r) != 0;
    m_regFilling = true;
    for (QLineEdit *e : m_reg)
        if (e) e->setEnabled(ok);
    for (QCheckBox *c : m_cc) c->setEnabled(ok);
    if (!ok) {
        m_regFilling = false;
        return;
    }
    m_is6309 = r.is6309 != 0;
    m_regs6309->setVisible(m_is6309);
    setEntry(m_reg[COCODEBUG_REG_PC], hx(r.pc, 4));
    setEntry(m_reg[COCODEBUG_REG_A], hx(r.a, 2));
    setEntry(m_reg[COCODEBUG_REG_B], hx(r.b, 2));
    setEntry(m_reg[COCODEBUG_REG_D], hx((unsigned)(r.a << 8 | r.b), 4));
    setEntry(m_reg[COCODEBUG_REG_DP], hx(r.dp, 2));
    setEntry(m_reg[COCODEBUG_REG_X], hx(r.x, 4));
    setEntry(m_reg[COCODEBUG_REG_Y], hx(r.y, 4));
    setEntry(m_reg[COCODEBUG_REG_U], hx(r.u, 4));
    setEntry(m_reg[COCODEBUG_REG_S], hx(r.s, 4));
    setEntry(m_reg[COCODEBUG_REG_CC], hx(r.cc, 2));
    for (int i = 0; i < 8; ++i) m_cc[i]->setChecked((r.cc & kCc[i].mask) != 0);
    if (m_is6309) {
        setEntry(m_reg[COCODEBUG_REG_E], hx(r.e, 2));
        setEntry(m_reg[COCODEBUG_REG_F], hx(r.f, 2));
        setEntry(m_reg[COCODEBUG_REG_W], hx(r.w, 4));
        setEntry(m_reg[COCODEBUG_REG_V], hx(r.v, 4));
        setEntry(m_reg[COCODEBUG_REG_MD], hx(r.md, 2));
    }
    cocodebug_io io;
    if (cocodebug_io_get(m_dbg, &io)) {
        m_coco3 = io.is_coco3 != 0;
        m_machine->setText(QStringLiteral("%1, %2").arg(m_coco3 ? QStringLiteral("CoCo 3") : QStringLiteral("CoCo 1/2"),
                                                       m_is6309 ? QStringLiteral("HD6309") : QStringLiteral("MC6809")));
    }
    m_regFilling = false;
}

uint32_t DebuggerWindow::memRows() const
{
    uint32_t size = cocodebug_space_size(m_dbg, m_space);
    if (size == 0) size = cocodebug_space_size(m_dbg, COCODEBUG_SPACE_CPU);
    return size / 16;
}

/* The dump lines that fit the view (the header takes one). */
int DebuggerWindow::memVisibleRows() const
{
    const int line = m_mem->fontMetrics().lineSpacing();
    const int height = m_mem->viewport()->height() - 2 * (int)m_mem->document()->documentMargin();
    if (line <= 0 || height <= 0) return 24;
    return qBound(4, height / line - 1, MEM_ROWS_MAX);
}

/* The window of rows the dump shows: each row named by the memory-map region
 * it starts in (CPU space; one fetch of the map for the refresh). The Go to /
 * search range stays marked, and so does every watch of the space. */
void DebuggerWindow::refreshMem()
{
    if (!stopped()) {
        if (m_mem->toPlainText().isEmpty())
            m_mem->setPlainText(QStringLiteral("(memory is read at a stop: pause the machine)"));
        return;
    }
    static uint8_t mem[MEM_ROWS_MAX * 16];
    static char text[MEM_ROWS_MAX * MEM_ROW_TEXT + 256];
    static cocodebug_region map[REGION_MAX];
    const int space = cocodebug_space_size(m_dbg, m_space) > 0 ? m_space : COCODEBUG_SPACE_CPU;
    const uint32_t total = memRows();
    const int vis = memVisibleRows();
    const uint32_t maxTop = total > (uint32_t)vis ? total - (uint32_t)vis : 0;
    if (m_memTop > maxTop) m_memTop = maxTop;
    memset(mem, 0, sizeof mem);
    const int got = cocodebug_mem_read(m_dbg, space, m_memTop * 16, mem, vis * 16);
    Q_UNUSED(got);
    const int regions = space == COCODEBUG_SPACE_CPU ? cocodebug_memory_map(m_dbg, map, REGION_MAX) : 0;
    const int digits = space == COCODEBUG_SPACE_PHYS ? 6 : 4;
    const int prefix = digits + 3;

    int len = snprintf(text, sizeof text, "%*s", prefix, "");
    for (int col = 0; col < 16; ++col) len += snprintf(text + len, sizeof text - (size_t)len, "%X  ", col);
    for (int row = 0; row < vis; ++row) {
        const uint8_t *bytes = mem + row * 16;
        const uint32_t addr = (m_memTop + (uint32_t)row) * 16;
        len += snprintf(text + len, sizeof text - (size_t)len, "\n$%0*X: ", digits, addr);
        for (int col = 0; col < 16; ++col) len += snprintf(text + len, sizeof text - (size_t)len, "%02X ", bytes[col]);
        text[len++] = ' ';
        for (int col = 0; col < 16; ++col) text[len++] = bytes[col] >= 0x20 && bytes[col] < 0x7F ? (char)bytes[col] : '.';
        if (regions > 0) {
            char name[64];
            cocomem_region_name(map, regions, (uint16_t)addr, name, sizeof name);
            len += snprintf(text + len, sizeof text - (size_t)len, "  %s", name);
        }
    }
    setText(m_mem, QString::fromLatin1(text, len));
    m_mem->verticalScrollBar()->setValue(0);

    m_memFilling = true;
    m_memBar->setRange(0, (int)maxTop);
    m_memBar->setPageStep(vis);
    m_memBar->setSingleStep(1);
    m_memBar->setValue((int)m_memTop);
    m_memFilling = false;

    /* the ranges marked: each row's bytes in the hex and in the text column */
    QList<QTextEdit::ExtraSelection> sel;
    auto mark = [&](uint32_t from, uint32_t to, const QTextCharFormat &fmt) {
        const uint32_t firstRow = qMax(from / 16, m_memTop), lastRow = qMin(to / 16, m_memTop + (uint32_t)vis - 1);
        for (uint32_t row = firstRow; row <= lastRow && row >= firstRow; ++row) {
            const int first = qMax((int)from - (int)(row * 16), 0), last = qMin((int)to - (int)(row * 16), 15);
            const int block = 1 + (int)(row - m_memTop);
            sel << spanSelection(m_mem, block, prefix + first * 3, (last - first) * 3 + 2, fmt);
            sel << spanSelection(m_mem, block, prefix + 48 + 1 + first, last - first + 1, fmt);
        }
    };
    for (int i = 0; i < cocowatch_count(); ++i) {
        uint32_t lo, hi;
        if (cocowatch_space(i) == space && cocowatch_range(m_dbg, i, &lo, &hi)) mark(lo, hi, watchFormat());
    }
    if (m_hl && m_hlSpace == space) mark(m_hlLo, m_hlHi, accentFormat());
    m_mem->setExtraSelections(sel);
}

/* Memory scrolled so the row holding addr sits a third of the way down. */
void DebuggerWindow::scrollMemory(uint32_t addr)
{
    const int vis = memVisibleRows();
    const uint32_t row = addr / 16, back = (uint32_t)(vis * 3 / 10);
    m_memTop = row > back ? row - back : 0;
    refreshMem();
}

/* The disassembly lines that fit the view. */
int DebuggerWindow::visibleRows() const
{
    const int line = m_disasm->fontMetrics().lineSpacing();
    const int height = m_disasm->viewport()->height() - 2 * (int)m_disasm->document()->documentMargin();
    if (line <= 0 || height <= 0) return 32;
    return qBound(4, height / line, DISASM_WINDOW);
}

void DebuggerWindow::refreshDisasm()
{
    static cocodebug_line lines[DISASM_WINDOW];
    if (!stopped()) {
        m_lineAddr.clear();
        if (m_disasm->toPlainText().isEmpty()) m_disasm->setPlainText(QStringLiteral("(running: pause the machine to read the code)"));
        return;
    }
    const int rows = visibleRows();
    if (m_followPc->isChecked()) {
        cocodebug_regs r;
        if (cocodebug_regs_get(m_dbg, &r))
            /* the PC a third of the way down */
            m_disasmTop = (uint16_t)cocodebug_row_address(m_dbg, r.pc, -(rows / 3));
    }
    int pcLine = -1;
    const int n = cocodebug_disassemble_ex(m_dbg, m_disasmTop, lines, rows, &pcLine);
    /* the label column is as wide as the longest label shown, within reason */
    int labelWidth = 0;
    for (int i = 0; i < n; ++i) labelWidth = qMax(labelWidth, (int)strlen(lines[i].label));
    labelWidth = qMin(labelWidth, 24);

    m_lineAddr.clear();
    QString text;
    char buf[160];
    struct Span { int line, from, length; };
    std::vector<Span> grays;
    for (int i = 0; i < n; ++i) {
        m_lineAddr.push_back(lines[i].address);
        /* the gutter: a breakpoint dot (hollow when disabled), then the
         * PC's arrow */
        QString ln = lines[i].has_breakpoint ? QStringLiteral("●") : lines[i].bp_disabled ? QStringLiteral("○")
                                                                                          : QStringLiteral(" ");
        snprintf(buf, sizeof buf, "%c%04X  %-11.11s ", lines[i].is_pc ? '>' : ' ', lines[i].address, lines[i].bytes);
        ln += QString::fromUtf8(buf);
        const int labelFrom = (int)ln.size();
        char label[64];
        snprintf(label, sizeof label, "%-*.*s ", labelWidth, labelWidth, lines[i].label);
        ln += QString::fromUtf8(label);
        if (lines[i].label[0] && i != pcLine)
            grays.push_back({ i, labelFrom, (int)strlen(lines[i].label) < labelWidth ? (int)strlen(lines[i].label)
                                                                                      : labelWidth });
        snprintf(buf, sizeof buf, "%-24s", lines[i].disasm);
        ln += QString::fromUtf8(buf);
        if (lines[i].comment[0]) {
            const QString comment = QStringLiteral("; ") + QString::fromUtf8(lines[i].comment);
            if (i != pcLine) grays.push_back({ i, (int)ln.size(), (int)comment.size() });
            ln += comment;
        }
        if (i) text += QLatin1Char('\n');
        text += ln;
    }
    if (n == 0) text = QStringLiteral("(no disassembly)");
    m_disasm->setPlainText(stripControl(text.toUtf8().constData()));
    m_disasm->verticalScrollBar()->setValue(0);

    /* The PC's line is the accent bar: the block's own background, not an
     * extra selection -- a full-width selection paints over the gutter's
     * red dot, a block background lies under it. */
    if (pcLine >= 0 && pcLine < n) {
        QTextCursor cur(m_disasm->document()->findBlockByNumber(pcLine));
        QTextBlockFormat bf;
        bf.setBackground(accentColor());
        cur.setBlockFormat(bf);
        cur.select(QTextCursor::LineUnderCursor);
        QTextCharFormat cf;
        cf.setForeground(accentInk());
        if (lines[pcLine].address == m_selected) {
            cf.setFontUnderline(true);
            cf.setFontWeight(QFont::Bold);
        }
        cur.mergeCharFormat(cf);
    }

    QList<QTextEdit::ExtraSelection> sel;
    /* labels and comments in the system's grey */
    QTextCharFormat grey;
    grey.setForeground(m_disasm->palette().color(QPalette::Disabled, QPalette::Text));
    for (const Span &g : grays) sel << spanSelection(m_disasm, g.line, g.from, g.length, grey);
    for (int i = 0; i < n; ++i) {
        /* the gutter: a breakpoint red (dark on the PC's bar), a disabled one grey */
        if (lines[i].has_breakpoint || lines[i].bp_disabled) {
            QTextCharFormat f;
            if (lines[i].has_breakpoint) {
                f.setForeground(i == pcLine ? QColor(0xa0, 0x10, 0x18) : QColor(0xe0, 0x1b, 0x24));
                f.setFontWeight(QFont::Bold);
            } else {
                f.setForeground(i == pcLine ? QColor(0x55, 0x55, 0x55) : QColor(0x9a, 0x99, 0x96));
            }
            sel << spanSelection(m_disasm, i, 0, 1, f);
        }
        /* the selected line underlined (the PC's own is done above) */
        if (lines[i].address == m_selected && i != pcLine) {
            QTextCharFormat f;
            f.setFontUnderline(true);
            f.setFontWeight(QFont::Bold);
            QTextEdit::ExtraSelection s;
            s.cursor = QTextCursor(m_disasm->document()->findBlockByNumber(i));
            s.cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            s.format = f;
            sel << s;
        }
    }
    m_disasm->setExtraSelections(sel);
}

/* ---- C source ---------------------------------------------------------------- */

void DebuggerWindow::rebuildSourceFiles()
{
    const int n = cocodebug_source_file_count(m_dbg);
    const QSignalBlocker block(m_srcFiles);
    m_srcFiles->clear();
    for (int i = 0; i < n; ++i) {
        const char *path = cocodebug_source_file_path(m_dbg, i);
        m_srcFiles->addItem(QFileInfo(QString::fromUtf8(path ? path : "")).fileName(), i);
        m_srcFiles->setItemData(m_srcFiles->count() - 1, QString::fromUtf8(path ? path : ""), Qt::ToolTipRole);
    }
    m_srcCount = n;
    if (m_srcFile >= n) m_srcFile = -1;
    if (m_srcFile < 0 && n > 0) m_srcFile = 0;
    m_srcFiles->setCurrentIndex(m_srcFile);
    m_srcSig.clear();
}

void DebuggerWindow::sourceFileChosen(int index)
{
    if (index < 0) return;
    m_srcFile = m_srcFiles->itemData(index).toInt();
    m_srcSig.clear();
    refreshSource(true);
}

/* Rebuilt when the listing, a breakpoint or the PC changed; every new stop
 * brings the machine's line into view, even when the reader has scrolled
 * away. The locals come from the stopped machine. */
void DebuggerWindow::refreshSource(bool force)
{
    const int files = cocodebug_source_file_count(m_dbg);
    if (files != m_srcCount) rebuildSourceFiles();
    cocodebug_stop snap;
    cocodebug_stop_snapshot(m_dbg, &snap);
    const bool stop = snap.paused != 0;
    const bool available = cocodebug_source_available(m_dbg) != 0;

    int pf = -1, pl = 0;
    if (stop && available) {
        cocodebug_regs r;
        if (!cocodebug_regs_get(m_dbg, &r) || !cocodebug_source_line_at(m_dbg, r.pc, &pf, &pl)) pf = -1, pl = 0;
    }
    const bool newStop = stop && snap.serial != m_srcSerial;
    if (stop) m_srcSerial = snap.serial;
    if (pf >= 0 && pf != m_srcFile && newStop) {       /* followed into another file */
        m_srcFile = pf;
        const QSignalBlocker block(m_srcFiles);
        for (int i = 0; i < m_srcFiles->count(); ++i)
            if (m_srcFiles->itemData(i).toInt() == pf) m_srcFiles->setCurrentIndex(i);
        m_srcSig.clear();
    }

    if (files == 0) {
        m_srcNote->setText(QStringLiteral("Load a CMOC listing (.lst from `cmoc -i`): it puts each C line at its "
                                          "address."));
    } else {
        const char *path = m_srcFile >= 0 ? cocodebug_source_file_path(m_dbg, m_srcFile) : nullptr;
        m_srcNote->setText(QStringLiteral("%1 source file%2%3").arg(files).arg(files == 1 ? QString() : QStringLiteral("s"))
                               .arg(path ? QStringLiteral(": ") + QString::fromUtf8(path) : QString()));
    }

    /* the file's text, read again only when the path changes */
    const char *cpath = m_srcFile >= 0 ? cocodebug_source_file_path(m_dbg, m_srcFile) : nullptr;
    const QString path = QString::fromUtf8(cpath ? cpath : "");
    if (path != m_srcPath) {
        m_srcLines.clear();
        QFile f(path);
        if (!path.isEmpty() && f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream ts(&f);
            while (!ts.atEnd()) m_srcLines << ts.readLine();
        }
        m_srcPath = path;
        m_srcSig.clear();
    }
    const int n = (int)m_srcLines.size();

    QString text;
    QList<int> on;
    text.reserve(n * 48);
    QString sig = path + QLatin1Char('|');
    for (int i = 1; i <= n; ++i) {
        const int state = cocodebug_source_breakpoint_is_set(m_dbg, m_srcFile, i) ? 1 : 0;
        if (state) on << i - 1;
        text += QStringLiteral("%1 %2  %3\n").arg(dotFor(state)).arg(i, 5).arg(m_srcLines[i - 1]);
    }
    for (int b : on) sig += QString::number(b) + QLatin1Char(',');
    const bool onPc = pf == m_srcFile && pl >= 1 && pl <= n;
    sig += QStringLiteral("|%1").arg(onPc ? pl : 0);
    if (n == 0) text = files ? QStringLiteral("(the source file could not be read)") : QString();

    const bool changed = force || sig != m_srcSig || m_srcView->toPlainText() != text;
    if (changed) {
        m_srcSig = sig;
        const int top = m_srcView->verticalScrollBar()->value();
        if (m_srcView->toPlainText() != text) {
            m_srcBar = -1;                  /* the document is new: no block wears the bar */
            m_srcView->setPlainText(text);
            m_srcView->verticalScrollBar()->setValue(top);
        }
        QList<QTextEdit::ExtraSelection> sel;
        barOn(sel, m_srcView, m_srcBar, onPc ? pl - 1 : -1);
        for (int b : on) {
            QTextCharFormat f;
            f.setForeground(b == pl - 1 && onPc ? QColor(0xa0, 0x10, 0x18) : QColor(0xe0, 0x1b, 0x24));
            f.setFontWeight(QFont::Bold);
            sel << spanSelection(m_srcView, b, 0, 1, f);
        }
        m_srcView->setExtraSelections(sel);
    }
    if (pl != m_srcPcLine || pf != m_srcPcFile || (newStop && onPc)) {
        m_srcPcLine = pl;
        m_srcPcFile = pf;
        if (onPc) {
            const QTextBlock block = m_srcView->document()->findBlockByNumber(pl - 1);
            if (block.isValid()) {
                m_srcView->setTextCursor(QTextCursor(block));
                m_srcView->centerCursor();
            }
        }
    }

    /* the locals of the current function */
    QString locals;
    if (!stop) {
        locals = QStringLiteral("(locals are read at a stop)");
    } else if (!available) {
        locals = QStringLiteral("(no C source loaded)");
    } else {
        cocodebug_local list[LOCALS_MAX];
        const int count = cocodebug_locals_get(m_dbg, list, LOCALS_MAX);
        if (count == 0) {
            locals = QStringLiteral("(no locals in scope)");
        } else {
            for (int i = 0; i < count; ++i) {
                const cocodebug_local &v = list[i];
                const QString value = v.has_value ? QStringLiteral("%1 ($%2)").arg(v.value).arg(v.value, 0, 16) : QStringLiteral("?");
                locals += QStringLiteral("%1 %2 %3= %4\n").arg(QString::fromUtf8(v.name).leftJustified(12),
                                                               QString::fromUtf8(v.type).leftJustified(10),
                                                               v.is_param ? QStringLiteral("(param) ") : QString(), value);
            }
            locals = locals.trimmed();
        }
    }
    setText(m_srcLocals, locals);
}

void DebuggerWindow::sourceToggleAt(int line)
{
    if (m_srcFile < 0 || line < 1) return;
    const int r = cocodebug_source_breakpoint_toggle(m_dbg, m_srcFile, line);
    if (r == -2) message(QStringLiteral("No code on that line"));
    else if (r < 0) message(QStringLiteral("The breakpoint table is full"));
    m_srcSig.clear();
    resync();
}

/* One or more .lst files, one per module of a multi-file CMOC program --
 * cocodebug_source_load() adds to what is already loaded rather than
 * replacing it, so loading them all in one dialog is the multi-file case. */
void DebuggerWindow::loadSource()
{
    const QStringList paths = QFileDialog::getOpenFileNames(this, QStringLiteral("Load C Source"), QString(),
                                                            QStringLiteral("CMOC listings (*.lst);;All files (*)"));
    if (paths.isEmpty()) return;
    int loaded = 0, failed = 0;
    for (const QString &path : paths) {
        if (cocodebug_source_load(m_dbg, path.toUtf8().constData()) < 0) failed++;
        else loaded++;
    }
    if (failed && !loaded) say(QStringLiteral("Could not open the listing or its .map (both need `cmoc -i`)"));
    else if (failed) say(QStringLiteral("Some listings could not be opened"));
    else say(QStringLiteral("Loaded %1 listing%2").arg(loaded).arg(loaded == 1 ? QString() : QStringLiteral("s")));
    m_srcPath.clear();
    m_srcFile = -1;
    rebuildSourceFiles();
    refreshSymbols(true);
    resync();
}

void DebuggerWindow::clearSource()
{
    cocodebug_source_clear(m_dbg);
    m_srcFile = -1;
    m_srcPath.clear();
    m_srcLines.clear();
    m_srcPcFile = -1;
    m_srcPcLine = 0;
    rebuildSourceFiles();
    say(QStringLiteral("C source cleared"));
    refreshSymbols(true);
    resync();
}

/* The Video tab takes its own snapshot; a stopped machine redraws only when
 * the core says something changed. */
void DebuggerWindow::refreshVideo(bool force)
{
    m_video->refresh(force);
}

void DebuggerWindow::refreshBps()
{
    static cocodebug_breakpoint bps[BP_MAX];
    const int n = cocodebug_breakpoints_list(m_dbg, bps, BP_MAX);

    cocodebug_stop snap;
    cocodebug_stop_snapshot(m_dbg, &snap);
    if (snap.paused) m_bpStop->setText(stoppedText(stripControl(snap.reason)) + QLatin1Char('.'));
    else m_bpStop->setText(QStringLiteral("Running."));

    /* rebuilt only when it changed (each step refreshes), and never while a
     * row's own button is being handled */
    QString sig;
    for (int i = 0; i < n; ++i)
        sig += QStringLiteral("%1 %2 %3 %4 %5 %6 %7\n").arg(bps[i].id).arg(bps[i].enabled).arg(bps[i].hits)
                   .arg(bps[i].type).arg(bps[i].start).arg(bps[i].end).arg(QString::fromUtf8(bps[i].condition));
    if (sig == m_bpSig && m_bpTable->rowCount() == n) return;
    m_bpSig = sig;

    QFont mono = m_bpTable->font();
    mono.setFamily(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::TypeWriter);
    m_bpFilling = true;
    m_bpTable->setRowCount(n);
    for (int i = 0; i < n; ++i) {
        auto *on = new QTableWidgetItem;
        on->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        on->setCheckState(bps[i].enabled ? Qt::Checked : Qt::Unchecked);
        on->setData(Qt::UserRole, bps[i].id);
        on->setToolTip(QStringLiteral("Enabled"));
        m_bpTable->setItem(i, 0, on);
        char desc[256];
        cocobp_describe(m_dbg, &bps[i], desc, sizeof desc);
        m_bpTable->setItem(i, 1, new QTableWidgetItem(stripControl(desc)));
        m_bpTable->setItem(i, 2, new QTableWidgetItem(QString::number(bps[i].hits)));
        for (int col = 1; col < 3; ++col) m_bpTable->item(i, col)->setFont(mono);
        auto *rm = new QToolButton;
        rm->setIcon(QIcon::fromTheme(QStringLiteral("edit-delete"), QIcon::fromTheme(QStringLiteral("user-trash"))));
        if (rm->icon().isNull()) rm->setText(QStringLiteral("Remove"));
        rm->setAutoRaise(true);
        rm->setToolTip(QStringLiteral("Remove"));
        const int id = bps[i].id;
        connect(rm, &QToolButton::clicked, this, [this, id] {
            cocodebug_breakpoint_remove(m_dbg, id);
            syncLater();
        });
        m_bpTable->setCellWidget(i, 3, rm);
    }
    m_bpFilling = false;
    m_bpEmpty->setVisible(n == 0);
}

/* PIA registers as two lines. */
static QString piaText(const char *name, const char *range, const char *note, const cocodebug_pia &p)
{
    QString s = QStringLiteral("%1 (%2)  %3\n").arg(QString::fromUtf8(name), QString::fromUtf8(range), QString::fromUtf8(note));
    s += QStringLiteral("  A: CR $%1  DDR $%2  OR $%3  pins $%4   CA1 %5  CA2 %6  IRQ %7\n")
             .arg(hx(p.cra, 2), hx(p.ddra, 2), hx(p.ora, 2), hx(p.pina, 2)).arg(p.ca1).arg(p.ca2).arg(p.irqa);
    s += QStringLiteral("  B: CR $%1  DDR $%2  OR $%3  pins $%4   CB1 %5  CB2 %6  IRQ %7\n")
             .arg(hx(p.crb, 2), hx(p.ddrb, 2), hx(p.orb, 2), hx(p.pinb, 2)).arg(p.cb1).arg(p.cb2).arg(p.irqb);
    return s;
}

/* The cartridge and the memory map on the left; the PIAs, the SAM, the GIME,
 * the joysticks and the Becker port on the right. */
void DebuggerWindow::refreshCart()
{
    static cocodebug_region reg[REGION_MAX];
    char info[512];
    cocodebug_cart_info(m_dbg, info, sizeof info);

    QString left = stripControl(info) + QStringLiteral("\n\nMemory map\n");
    const int n = cocodebug_memory_map(m_dbg, reg, REGION_MAX);
    if (n == 0) left += QStringLiteral("  (read at a stop)\n");
    for (int i = 0; i < n; ++i) {
        left += QStringLiteral("  $%1-$%2  %3  %4").arg(hx(reg[i].lo, 4), hx(reg[i].hi, 4),
                                                       reg[i].kind == COCODEBUG_REGION_UNMAPPED ? QStringLiteral("--")
                                                       : reg[i].writable                        ? QStringLiteral("rw")
                                                                                                : QStringLiteral("r-"),
                                                       QString::fromUtf8(reg[i].name));
        if (reg[i].physical_base >= 0) left += QStringLiteral("  (phys $%1)").arg(hx((unsigned)reg[i].physical_base, 6));
        left += QLatin1Char('\n');
    }
    setText(m_cartText, left);

    cocodebug_io io;
    if (!cocodebug_io_get(m_dbg, &io)) {
        if (m_io->toPlainText().isEmpty()) m_io->setPlainText(QStringLiteral("(the chips are read at a stop: pause the machine)"));
        return;
    }
    QString s;
    s += piaText("PIA0", "$FF00-$FF03", "keyboard, joysticks, the VDG's sync", io.pia0);
    s += QLatin1Char('\n');
    s += piaText("PIA1", "$FF20-$FF23", "DAC, cassette, VDG mode, one-bit sound", io.pia1);
    const unsigned vdg = io.pia1.orb;
    s += QStringLiteral("  VDG pins: A/G %1  GM2 %2  GM1 %3  GM0 %4  CSS %5\n")
             .arg((vdg >> 7) & 1).arg((vdg >> 6) & 1).arg((vdg >> 5) & 1).arg((vdg >> 4) & 1).arg((vdg >> 3) & 1);
    if (io.sam_valid)
        s += QStringLiteral("\nSAM  V %1  F $%2  P %3  R %4  M %5  TY %6  video address $%7\n")
                 .arg(io.sam_v).arg(hx(io.sam_f, 2)).arg(io.sam_p).arg(io.sam_r).arg(io.sam_m).arg(io.sam_ty)
                 .arg(hx(io.sam_video_address, 4));
    if (io.is_coco3 && io.gime_valid) {
        static const char *const names[16] = { "INIT0", "INIT1", "IRQENR", "FIRQENR", "TIMER hi", "TIMER lo", "-", "-",
                                               "VMODE", "VRES", "BORDER", "-", "V.SCROLL", "V.OFFSET hi",
                                               "V.OFFSET lo", "H.OFFSET" };
        s += QStringLiteral("\nGIME  scanline %1  row %2  IRQ %3  FIRQ %4\n").arg(io.gime_scanline).arg(io.gime_row)
                 .arg(io.gime_irq).arg(io.gime_firq);
        for (int i = 0; i < 16; ++i)
            s += QStringLiteral("  $FF%1  %2  $%3\n").arg(hx(0x90 + (unsigned)i, 2), QString::fromUtf8(names[i]).leftJustified(11), hx(io.gime_reg[i], 2));
        s += QStringLiteral("  MMU task 0 ($FFA0): ");
        for (int i = 0; i < 8; ++i) s += hx(io.gime_mmu[i], 2) + QLatin1Char(' ');
        s += QStringLiteral("\n  MMU task 1 ($FFA8): ");
        for (int i = 8; i < 16; ++i) s += hx(io.gime_mmu[i], 2) + QLatin1Char(' ');
        s += QStringLiteral("\n  Palette ($FFB0): ");
        for (int i = 0; i < 16; ++i) s += hx(io.gime_palette[i], 2) + QLatin1Char(' ');
        s += QLatin1Char('\n');
    }
    s += QStringLiteral("\nJoysticks\n");
    for (int j = 0; j < 2; ++j)
        s += QStringLiteral("  %1: x %2  y %3  buttons %4 %5\n").arg(j ? QStringLiteral("Left ") : QStringLiteral("Right"))
                 .arg(io.joy_axis[j][0], 5).arg(io.joy_axis[j][1], 5).arg(io.joy_button[j][0]).arg(io.joy_button[j][1]);
    s += QStringLiteral("\nBecker port (FujiNet)  link %1  receive bytes pending %2\n")
             .arg(io.becker_link_up ? QStringLiteral("up") : QStringLiteral("down")).arg(io.becker_rx_pending);
    setText(m_io, s);
}

/* The DAC and the one-bit output now, and the waveform of their recent
 * writes. */
void DebuggerWindow::refreshSound()
{
    static cocodebug_sound_write hist[SOUND_MAX];
    cocodebug_sound a;
    QString s;
    if (cocodebug_sound_get(m_dbg, &a)) {
        s += QStringLiteral("DAC (PIA1 A, $FF20)   %1 of 63\n").arg(a.dac);
        s += QStringLiteral("One-bit (PIA1 B, $FF22 bit 1)   %1\n").arg(a.single_bit);
        s += QStringLiteral("Sound enable (PIA1 CB2)   %1\n").arg(a.sound_enable ? QStringLiteral("on") : QStringLiteral("off"));
        s += QStringLiteral("Mux source (PIA0 CB2:CA2)   %1\n").arg(QString::fromUtf8(a.mux_name ? a.mux_name : "?"));
        s += QStringLiteral("Cassette motor (PIA1 CA2)   %1\n").arg(a.motor ? QStringLiteral("on") : QStringLiteral("off"));
        s += QStringLiteral("Cassette input (PIA1 A bit 0)   %1\n").arg(a.cassette_in);
    } else {
        s += QStringLiteral("(the registers are read at a stop; the waveform below is live)\n");
    }
    const int n = cocodebug_sound_history(m_dbg, hist, SOUND_MAX);
    s += QStringLiteral("\n%1 write%2 recorded").arg(n).arg(n == 1 ? QString() : QStringLiteral("s"));
    if (n == 0 && !m_soundWatching) s += QStringLiteral(" (recording starts when this tab shows)");
    s += QLatin1Char('\n');
    setText(m_sound, s);
    m_soundPlot->set(std::vector<cocodebug_sound_write>(hist, hist + n));
}

/* The labels the engine holds, filtered, in the order the filter gives
 * (address, then name). The table is rebuilt only when what it would show
 * changed: the engine's own index order is not stable, so the entries are
 * fetched anew each time and compared by content, and each row keeps its own
 * address for activation. */
void DebuggerWindow::refreshSymbols(bool force)
{
    const int total = cocodebug_symbols_count(m_dbg);
    const QByteArray filter = m_symFilter->text().trimmed().toUtf8();
    std::vector<int> indices((size_t)qMax(total, 0));
    const int shown = total > 0 ? cocodebug_symbols_filter(m_dbg, filter.constData(), indices.data(), total) : 0;

    struct Row { uint16_t addr; char name[96]; };
    std::vector<Row> rows;
    rows.reserve((size_t)shown);
    QString sig = QStringLiteral("%1/%2/%3:").arg(total).arg(shown).arg(QString::fromUtf8(filter));
    for (int i = 0; i < shown; ++i) {
        Row row = {};
        if (!cocodebug_symbols_get(m_dbg, indices[(size_t)i], &row.addr, row.name, (int)sizeof row.name)) continue;
        rows.push_back(row);
        sig += QStringLiteral("%1 %2;").arg(row.addr).arg(QString::fromUtf8(row.name));
    }
    if (!force && sig == m_symSig) return;
    m_symSig = sig;

    m_symCount->setText(QStringLiteral("%1 of %2 symbols").arg(rows.size()).arg(total));
    const int top = m_symTable->verticalScrollBar()->value();
    m_symTable->setUpdatesEnabled(false);
    m_symTable->setRowCount((int)rows.size());
    for (int i = 0; i < (int)rows.size(); ++i) {
        auto *addr = new QTableWidgetItem(QStringLiteral("$") + hx(rows[(size_t)i].addr, 4));
        addr->setData(Qt::UserRole, rows[(size_t)i].addr);
        m_symTable->setItem(i, 0, addr);
        m_symTable->setItem(i, 1, new QTableWidgetItem(QString::fromUtf8(rows[(size_t)i].name)));
    }
    m_symTable->setUpdatesEnabled(true);
    m_symTable->verticalScrollBar()->setValue(top);
}

void DebuggerWindow::addWatch(const QString &entry)
{
    const QByteArray text = entry.trimmed().toUtf8();
    if (text.isEmpty()) return;
    const int space = m_wtSpace->currentData().toInt();
    if (cocowatch_add(text.constData(), space) < 0) {
        message(QStringLiteral("The watch list is full"));
        return;
    }
    m_wtEntry->clear();
    refreshWatches(true);
}

/* Break on change: stop after a write over the watch that alters a byte. */
void DebuggerWindow::watchBreak(int index)
{
    uint32_t lo, hi;
    if (!cocowatch_range(m_dbg, index, &lo, &hi)) {
        message(QStringLiteral("That watch does not resolve yet"));
        return;
    }
    const int type = COCODEBUG_BP_WRITE | (cocowatch_space(index) == COCODEBUG_SPACE_PHYS ? COCODEBUG_BP_PHYS : 0);
    if (cocodebug_breakpoint_add(m_dbg, type, lo, hi, "value != old") < 0)
        message(QStringLiteral("The breakpoint could not be added"));
    else
        message(QStringLiteral("Breaking when it changes"));
    resync();
}

/* One row a watch, its value live (a dash while the machine runs). The rows
 * are rebuilt when the list changed; otherwise only their text moves. */
void DebuggerWindow::refreshWatches(bool force)
{
    const int n = cocowatch_count();
    const unsigned gen = cocowatch_generation();
    QFont mono = m_wtTable->font();
    mono.setFamily(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::TypeWriter);
    if (force || gen != m_wtGen || m_wtTable->rowCount() != n) {
        m_wtGen = gen;
        m_wtTable->setRowCount(n);
        for (int i = 0; i < n; ++i) {
            auto *item = new QTableWidgetItem;
            item->setFont(mono);
            m_wtTable->setItem(i, 0, item);
            auto *box = new QWidget;
            auto *h = new QHBoxLayout(box);
            h->setContentsMargins(2, 0, 2, 0);
            auto *mem = new QPushButton(QStringLiteral("Memory"));
            mem->setToolTip(QStringLiteral("Show it in the Memory tab"));
            connect(mem, &QPushButton::clicked, this, [this, i] {
                uint32_t lo, hi;
                if (!cocowatch_range(m_dbg, i, &lo, &hi)) {
                    message(QStringLiteral("That watch does not resolve yet"));
                    return;
                }
                if (m_popped[TAB_MEMORY]) m_popped[TAB_MEMORY]->activateWindow();
                else m_tabs->setCurrentWidget(m_pages[TAB_MEMORY]);
                showInMemory(cocowatch_space(i), lo, hi);
            });
            auto *brk = new QPushButton(QStringLiteral("Break on change"));
            brk->setToolTip(QStringLiteral("Stop after a write that alters it"));
            connect(brk, &QPushButton::clicked, this, [this, i] { watchBreak(i); });
            auto *rm = new QPushButton(QStringLiteral("Remove"));
            connect(rm, &QPushButton::clicked, this, [this, i] {
                cocowatch_remove(i);
                QTimer::singleShot(0, this, [this] { refreshWatches(true); });
            });
            for (QPushButton *b : { mem, brk, rm }) {
                b->setFocusPolicy(Qt::NoFocus);
                h->addWidget(b);
            }
            m_wtTable->setCellWidget(i, 1, box);
        }
    }
    for (int i = 0; i < n; ++i) {
        char line[256];
        cocowatch_format(m_dbg, i, line, sizeof line);
        QString text = stripControl(line);
        if (cocowatch_space(i) == COCODEBUG_SPACE_PHYS) text = QStringLiteral("phys  ") + text;
        if (m_wtTable->item(i, 0)->text() != text) m_wtTable->item(i, 0)->setText(text);
    }
}

/* Trace (live) and History (at a stop): the ring newest first, shown
 * oldest first, the newest HIST_SHOWN lines that pass the filter. */
void DebuggerWindow::refreshHist(int idx, bool force)
{
    static cocodebug_trace_entry buf[RING_MAX];
    const bool stop = stopped();
    const bool on = cocodebug_trace_enabled(m_dbg) != 0;
    const int count = cocodebug_trace_count(m_dbg);

    m_hSync = true;                     /* both Record buttons follow the engine */
    for (int i = 0; i < 2; ++i)
        if (m_hRec[i]->isChecked() != on) m_hRec[i]->setChecked(on);
    m_hSync = false;

    if (force) m_hSeen[idx] = -1;
    /* History reads at a stop (it keeps what it showed while the machine
     * runs); at a stop neither view rebuilds for the same count */
    if (idx == 1 && !stop && m_hSeen[1] >= 0) return;
    if (stop && count == m_hSeen[idx]) return;
    m_hSeen[idx] = count;

    uint32_t lo = 0, hi = 0xFFFF;
    int filter = COCODEBUG_TRACE_ALL;
    switch (m_hFilter[idx]->currentIndex()) {
    case 1: filter = COCODEBUG_TRACE_BRANCHES; break;
    case 2:
        if (cocosym_parse_range(m_dbg, COCODEBUG_SPACE_CPU, m_hRange[idx]->text().toUtf8().constData(), &lo, &hi))
            filter = COCODEBUG_TRACE_RANGE;
        break;
    default: break;
    }
    const int n = cocodebug_trace_read(m_dbg, buf, RING_MAX);
    QList<int> hit;
    for (int i = 0; i < n && hit.size() < HIST_SHOWN; ++i)
        if (cocodebug_trace_match(&buf[i], filter, (uint16_t)lo, (uint16_t)hi)) hit.append(i);
    QString t;
    if (!on && n == 0) t = QStringLiteral("Recording is off. Press Record, then let the machine run.");
    char line[256];
    for (int k = (int)hit.size() - 1; k >= 0; --k) {
        cocodebug_trace_format(&buf[hit[k]], line, sizeof line);
        t += QString::fromUtf8(line);
        if (k) t += QLatin1Char('\n');
    }
    setText(m_hText[idx], t, true);
    m_hNote[idx]->setText(on ? QStringLiteral("recording, %1 instructions").arg(n)
                             : n ? QStringLiteral("recording off, %1 instructions").arg(n)
                                 : QStringLiteral("recording off"));
}

/* The packet log is rebuilt only when it changed: a new packet, or a
 * repeat (which moves the newest serial on). */
void DebuggerWindow::refreshFujinet(bool force)
{
    static coco_fuji_packet p[TRACE_MAX];
    char state[1024];
    const int n = cocodebug_fujinet_trace(m_dbg, p, TRACE_MAX);
    const bool hide = m_fujiHideReady->isChecked();

    if (!cocosession_fujinet_running(m_session) && !cocosession_fujinet_external(m_session))
        m_fujiNote->setText(QStringLiteral("FujiNet is off (Settings ▸ FujiNet)."));
    else
        m_fujiNote->setText(QStringLiteral("%1 FujiNet on the Becker port %2, %3.")
                                .arg(cocosession_fujinet_external(m_session) ? QStringLiteral("External")
                                                                              : QStringLiteral("Built-in"),
                                     QString::fromUtf8(cocosession_becker_endpoint(m_session)),
                                     cocosession_fujinet_link(m_session) ? QStringLiteral("connected")
                                                                         : QStringLiteral("not connected")));
    state[0] = '\0';
    cocodebug_fujinet_state(m_dbg, state, sizeof state);
    setText(m_fujiState, stripControl(state));

    if (!force && n == m_fujiCount && (n == 0 || p[n - 1].serial == m_fujiLast)) return;
    const bool follow = force || m_fujiCount < 0 || atEnd(m_fujiText);
    m_fujiCount = n;
    m_fujiLast = n > 0 ? p[n - 1].serial : 0;
    QString t;
    int shown = 0;
    for (int i = 0; i < n; ++i) {
        if (hide && cocodebug_fujinet_is_ready(&p[i])) continue;
        if (shown++) t += QLatin1Char('\n');
        t += QString::fromUtf8(p[i].text);
        if (p[i].repeat > 1) t += QStringLiteral(" ×%1").arg(p[i].repeat);
        t += QStringLiteral("\n     ");
        for (int k = 0; k < p[i].nbytes && k < int(sizeof p[i].data); k++) t += hx(p[i].data[k], 2) + QLatin1Char(' ');
        if (p[i].nbytes > int(sizeof p[i].data)) t += QStringLiteral("…");
    }
    if (shown == 0)
        t = n ? QStringLiteral("(only READY probes since the trace was cleared)")
              : QStringLiteral("(no DriveWire traffic since the trace was cleared)");
    setText(m_fujiText, t, follow);
}

/* One tab; live is the twice-a-second refresh while the machine runs, which
 * leaves out what only changes at a stop. */
void DebuggerWindow::refreshPage(int page, bool live)
{
    switch (page) {
    case TAB_DISASM: if (!live) refreshDisasm(); break;
    case TAB_SOURCE: if (!live) refreshSource(false); break;
    case TAB_MEMORY: if (!live) refreshMem(); break;
    case TAB_VIDEO: refreshVideo(std::exchange(m_videoForce, false)); break;
    case TAB_BREAKS: refreshBps(); break;
    case TAB_WATCHES: refreshWatches(false); break;
    case TAB_CART: if (!live) refreshCart(); break;
    case TAB_TRACE: refreshHist(0, false); break;
    case TAB_HISTORY: if (!live) refreshHist(1, false); break;
    case TAB_SOUND: refreshSound(); break;
    case TAB_SYMBOLS: refreshSymbols(false); break;
    case TAB_FUJINET: refreshFujinet(false); break;
    default: break;
    }
}

/* The registers show everywhere; then the tab showing and any in a window of
 * its own. */
void DebuggerWindow::refreshAll()
{
    refreshStatus();
    refreshCpu();
    refreshPage(currentTab(), false);
    for (int t = 0; t < TAB_COUNT; ++t)
        if (m_popped[t]) refreshPage(t, false);
}

/* A page's number, whichever place it holds in the tab widget now. */
int DebuggerWindow::currentTab() const
{
    QWidget *w = m_tabs->currentWidget();
    return w ? w->property("tab").toInt() : -1;
}

/* On screen: the tab widget's page, or a window of its own. */
bool DebuggerWindow::tabVisible(int tab) const
{
    return currentTab() == tab || m_popped[tab] != nullptr;
}

/* The sound writes are recorded only while the Sound tab shows: it costs the
 * machine's bus tap. */
void DebuggerWindow::watchSound()
{
    const bool want = isVisible() && tabVisible(TAB_SOUND);
    if (want == m_soundWatching) return;
    m_soundWatching = want;
    cocodebug_sound_watch(m_dbg, want ? 1 : 0);
}

/* A new stop brings every view to the spot, whatever the reader had
 * scrolled to: the Disassembly to the PC, Memory to the range it marks (or
 * the PC), the C source to its line, the Trace and History to their newest
 * line, the Video tab to a redraw. */
void DebuggerWindow::recentreOnStop()
{
    m_followPc->setChecked(true);
    m_disasm->verticalScrollBar()->setValue(0);
    m_hSeen[0] = m_hSeen[1] = -1;
    m_videoForce = true;
    m_srcSerial = 0;
    m_srcPcLine = -1;
    refreshAll();
    cocodebug_regs r;
    if (m_hl && m_hlSpace == m_space) scrollMemory(m_hlLo);
    else if (m_space == COCODEBUG_SPACE_CPU && cocodebug_regs_get(m_dbg, &r)) scrollMemory(r.pc);
    for (QPlainTextEdit *view : m_hText)
        view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum());
}

/* After anything that may have moved the engine: the console notes a new
 * stop, then every view follows it. */
void DebuggerWindow::resync()
{
    cocodebug_stop snap;
    cocodebug_stop_snapshot(m_dbg, &snap);
    const bool stop = snap.paused != 0;
    const bool newStop = stop && snap.serial != m_seenSerial;
    if (newStop && snap.reason[0] && strncmp(snap.reason, "step", 4) != 0)
        /* a step already says where it is */
        consoleAppend(QStringLiteral("— %1\n").arg(stripControl(snap.reason)));
    m_seenSerial = snap.serial;
    m_wasStopped = stop;
    m_seenGen = cocodebug_generation(m_dbg);
    m_runningTicks = 0;
    if (newStop) recentreOnStop();
    else refreshAll();
}

/* From inside a breakpoint row's own signal: the table rebuilds after. */
void DebuggerWindow::syncLater()
{
    QTimer::singleShot(0, this, [this] { resync(); });
}

/* A step takes microseconds: a moment's wait for the stop keeps the window
 * from flashing "Running" in between. */
void DebuggerWindow::settleAndSync()
{
    for (int i = 0; i < 40 && !stopped(); ++i) QThread::msleep(1);
    resync();
}

/* The engine reports a stop through the generation and the stop serial, so
 * this polls them: a refresh when they moved, and the live values twice a
 * second while the machine runs. */
void DebuggerWindow::tick()
{
    if (!isVisible()) return;
    watchSound();
    cocodebug_stop snap;
    cocodebug_stop_snapshot(m_dbg, &snap);
    const unsigned gen = cocodebug_generation(m_dbg);
    const bool stop = snap.paused != 0;
    if (gen != m_seenGen || stop != m_wasStopped || snap.serial != m_seenSerial) {
        resync();
    } else if (tabVisible(TAB_WATCHES) && cocowatch_generation() != m_wtGen) {
        refreshWatches(false);          /* the list changed elsewhere (the console) */
    } else if (tabVisible(TAB_SOURCE) && cocodebug_source_file_count(m_dbg) != m_srcCount) {
        refreshSource(false);           /* a listing loaded by the session */
    } else if (!stop && ++m_runningTicks >= 4) {
        m_runningTicks = 0;
        refreshStatus();
        refreshCpu();
        refreshPage(currentTab(), true);
        for (int t = 0; t < TAB_COUNT; ++t)
            if (m_popped[t]) refreshPage(t, true);
    }
}

/* ---- the console ---------------------------------------------------------- */

/* Onto the transcript, which follows its end while the reader is there. */
void DebuggerWindow::consoleAppend(const QString &text)
{
    const bool follow = atEnd(m_conOut);
    QTextCursor cur(m_conOut->document());
    cur.movePosition(QTextCursor::End);
    cur.insertText(text);
    if (follow) m_conOut->verticalScrollBar()->setValue(m_conOut->verticalScrollBar()->maximum());
}

void DebuggerWindow::runConsole()
{
    static char out[65536];
    const QString cmd = m_conIn->text().trimmed();
    if (cmd.isEmpty()) return;
    if (m_history.isEmpty() || m_history.last() != cmd) m_history << cmd;
    m_historyPos = (int)m_history.size();
    /* a command brings the reader back down */
    m_conOut->verticalScrollBar()->setValue(m_conOut->verticalScrollBar()->maximum());
    consoleAppend(QStringLiteral("> %1\n").arg(cmd));
    m_conIn->clear();

    out[0] = '\0';
    cocodebug_command(m_dbg, cmd.toUtf8().constData(), out, sizeof out);
    QString text = stripControl(out);
    if (!text.isEmpty()) consoleAppend(text.endsWith(QLatin1Char('\n')) ? text : text + QLatin1Char('\n'));
    settleAndSync();
}

/* Tab: the engine completes the word at the end of the line. One match
 * replaces it; several are listed and the line extends to what they share. */
void DebuggerWindow::completeConsole()
{
    char comps[4096];
    const QString text = m_conIn->text();
    comps[0] = '\0';
    const int n = cocodebug_completions(m_dbg, text.toUtf8().constData(), comps, sizeof comps);
    if (n <= 0) return;
    const QStringList words = stripControl(comps).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (words.isEmpty()) return;
    const int sp = text.lastIndexOf(QLatin1Char(' '));
    const QString head = text.left(sp + 1);
    if (words.size() == 1) {
        m_conIn->setText(head + words[0] + QLatin1Char(' '));
        return;
    }
    QString common = words[0];
    for (const QString &w : words)
        while (!w.startsWith(common)) common.chop(1);
    consoleAppend(words.join(QStringLiteral("  ")) + QLatin1Char('\n'));
    if (common.size() > text.size() - sp - 1) m_conIn->setText(head + common);
}

/* Messages land in the console and, since it may not be the tab showing,
 * the status bar. */
void DebuggerWindow::say(const QString &msg)
{
    consoleAppend(msg + QLatin1Char('\n'));
    message(msg);
}

void DebuggerWindow::message(const QString &msg)
{
    statusBar()->showMessage(msg, 6000);
}

/* ---- the rest of the handlers ----------------------------------------------- */

void DebuggerWindow::toggleRun()
{
    if (stopped()) {
        cocodebug_resume(m_dbg);
        resync();
    } else {
        cocodebug_pause(m_dbg);
        settleAndSync();
    }
}

void DebuggerWindow::stepAnd(const std::function<void()> &fn)
{
    if (!stopped()) {
        message(QStringLiteral("Pause the machine first"));
        return;
    }
    fn();
    settleAndSync();
}

void DebuggerWindow::runToCursor()
{
    if (m_selected < 0) {
        message(QStringLiteral("Select a line in the Disassembly to run to"));
        return;
    }
    stepAnd([this] { cocodebug_run_to(m_dbg, (uint16_t)m_selected); });
}

/* Every breakpoint of every kind (the older clear_all takes only the enabled
 * execute ones). */
void DebuggerWindow::clearBreakpoints()
{
    static cocodebug_breakpoint list[BP_MAX];
    cocodebug_breakpoint_clear_all(m_dbg);
    const int n = cocodebug_breakpoints_list(m_dbg, list, BP_MAX);
    for (int i = 0; i < n; ++i) cocodebug_breakpoint_remove(m_dbg, list[i].id);
}

void DebuggerWindow::toggleBreakpoint(int addr)
{
    cocodebug_breakpoint_toggle(m_dbg, (uint16_t)addr);
    resync();
}

void DebuggerWindow::toggleSelected()
{
    if (m_selected < 0) {
        message(QStringLiteral("Select a line in the Disassembly first"));
        return;
    }
    toggleBreakpoint(m_selected);
}

/* The toolbar's Break at: a symbol or a hex address, an execute breakpoint
 * there (one already set stays). */
void DebuggerWindow::breakAt()
{
    const QString text = m_breakAt->text().trimmed();
    if (text.isEmpty()) return;
    long addr;
    QString why;
    if (!addressOf(text, COCODEBUG_SPACE_CPU, &addr, &why)) {
        message(why);
        return;
    }
    if (cocodebug_breakpoint_is_set(m_dbg, (uint16_t)addr)) {
        message(QStringLiteral("A breakpoint is already set at $%1").arg(hx((unsigned)addr, 4)));
    } else {
        cocodebug_breakpoint_toggle(m_dbg, (uint16_t)addr);
        message(QStringLiteral("Breakpoint at $%1").arg(hx((unsigned)addr, 4)));
    }
    m_breakAt->clear();
    resync();
}

/* A number typed into an entry: a symbol first, then bare digits as hex (as
 * in the sibling debuggers), then whatever the engine evaluates ($0400,
 * label+2, registers, [ptr]; the console itself reads bare digits as decimal).
 * When it is none of those, the reason goes to *why, or to the status bar
 * without. */
bool DebuggerWindow::numberOf(const QString &text, long *out, QString *why)
{
    static const QRegularExpression hexOnly(QStringLiteral("^[0-9A-Fa-f]+$"));
    const QString t = text.trimmed();
    const QByteArray utf8 = t.toUtf8();
    long v = 0;
    QString reason;
    uint16_t sym;
    uint32_t lo, hi;
    char err[160] = "";
    if (t.isEmpty()) {
        reason = QStringLiteral("Not a number");
    } else if (cocodebug_symbol_find(m_dbg, utf8.constData(), &sym)) {
        v = sym;
    } else if (hexOnly.match(t).hasMatch()) {
        bool ok;
        v = t.toLong(&ok, 16);
        if (!ok) reason = QStringLiteral("Not a number");
    } else if (cocosym_parse_range(m_dbg, cocodebug_space_size(m_dbg, COCODEBUG_SPACE_PHYS) ? COCODEBUG_SPACE_PHYS
                                                                                            : COCODEBUG_SPACE_CPU,
                                   utf8.constData(), &lo, &hi) && lo == hi) {
        v = (long)lo;
    } else if (cocodebug_eval(m_dbg, utf8.constData(), &v, err, sizeof err) < 0) {
        reason = QStringLiteral("Not a number ($0400, a symbol or an expression): %1").arg(t);
    }
    if (reason.isEmpty()) {
        *out = v;
        return true;
    }
    if (why) *why = reason;
    else message(reason);
    return false;
}

/* The same, as an address of a space: from 0 to its last. */
bool DebuggerWindow::addressOf(const QString &text, int space, long *out, QString *why)
{
    long v;
    if (!numberOf(text, &v, why)) return false;
    uint32_t size = cocodebug_space_size(m_dbg, space);
    if (size == 0) size = 0x10000;
    if (v < 0 || (uint32_t)v >= size) {
        const QString reason = space == COCODEBUG_SPACE_PHYS
                                   ? QStringLiteral("Out of range (physical RAM ends at $%1)").arg(hx(size - 1, 6))
                                   : QStringLiteral("Out of range ($0000-$FFFF)");
        if (why) *why = reason;
        else message(reason);
        return false;
    }
    *out = v;
    return true;
}

/* A breakpoint's range from its two entries. The start may itself be a
 * range (lo-hi); the end is optional. */
bool DebuggerWindow::rangeOf(const QString &startText, const QString &endText, int space, uint32_t *lo, uint32_t *hi,
                             QString *why)
{
    long first = 0, last = 0;
    const QString start = startText.trimmed(), end = endText.trimmed();
    if (end.isEmpty()) {
        uint32_t from, to;
        if (!cocosym_parse_range(m_dbg, space, start.toUtf8().constData(), &from, &to)) {
            *why = QStringLiteral("Not an address, label or range: %1").arg(start);
            return false;
        }
        first = (long)from;
        last = (long)to;
    } else {
        if (!addressOf(start, space, &first, why)) return false;
        last = first;
        if (!addressOf(end, space, &last, why)) return false;
    }
    if (last < first) {
        *why = QStringLiteral("The end is before the start");
        return false;
    }
    *lo = (uint32_t)first;
    *hi = (uint32_t)last;
    return true;
}

/* Edits go in only while the machine is stopped. A register entry shows hex
 * digits, so bare digits there are hex; anything else is an expression. */
void DebuggerWindow::setReg(int reg)
{
    QLineEdit *e = m_reg[reg];
    long v = 0;
    QString why;
    const bool ok = numberOf(e->text(), &v, &why);
    if (!stopped()) message(QStringLiteral("Pause the machine first"));
    else if (!ok) message(why);
    else if (!cocodebug_regs_set(m_dbg, reg, (unsigned)v)) message(QStringLiteral("That register does not exist on this CPU"));
    e->clearFocus();
    resync();
}

void DebuggerWindow::setFlag(unsigned mask, bool on)
{
    if (m_regFilling) return;
    if (!stopped()) {
        message(QStringLiteral("Pause the machine first"));
    } else {
        cocodebug_flag_set(m_dbg, mask, on ? 1 : 0);
    }
    resync();
}

/* Shows a range in the dump, from just above it, marked and scrolled to. */
void DebuggerWindow::showInMemory(int space, uint32_t lo, uint32_t hi)
{
    if (space != m_space) {
        m_space = space;
        m_memFilling = true;
        for (int i = 0; i < m_memSpace->count(); ++i)
            if (m_memSpace->itemData(i).toInt() == space) m_memSpace->setCurrentIndex(i);
        m_memFilling = false;
    }
    m_hl = true;
    m_hlSpace = space;
    m_hlLo = lo;
    m_hlHi = hi;
    scrollMemory(lo);
}

void DebuggerWindow::memSpaceChanged(int space)
{
    m_space = space;
    m_hl = false;
    m_memTop = 0;
    cocodebug_regs r;
    if (space == COCODEBUG_SPACE_CPU && stopped() && cocodebug_regs_get(m_dbg, &r)) scrollMemory(r.pc);
    else refreshMem();
}

/* Opens an address in the Disassembly (the tab, or its window), the PC no
 * longer followed, the line selected. */
void DebuggerWindow::showInDisassembly(uint16_t addr)
{
    if (m_popped[TAB_DISASM]) m_popped[TAB_DISASM]->activateWindow();
    else m_tabs->setCurrentWidget(m_pages[TAB_DISASM]);
    m_followPc->setChecked(false);
    m_selected = addr;
    m_disasmTop = (uint16_t)cocodebug_row_address(m_dbg, addr, -(visibleRows() / 3));
    refreshDisasm();
}

/* Go to: a label, an address, an expression or a range (lo-hi), which is
 * marked. Bare digits are hex, as everywhere in the entries. */
void DebuggerWindow::memGoto()
{
    const QString text = m_memGoto->text().trimmed();
    if (text.isEmpty()) return;
    uint32_t lo, hi;
    if (!cocosym_parse_range(m_dbg, m_space, text.toUtf8().constData(), &lo, &hi)) {
        m_hl = false;
        refreshMem();
        message(QStringLiteral("Not an address, label or range: %1").arg(text));
        return;
    }
    showInMemory(m_space, lo, hi);
}

/* Find Next / Previous from the marked range (or the top of the view); the
 * hit is marked. */
void DebuggerWindow::memSearch(bool backward)
{
    uint8_t pat[COCOSEARCH_MAX], mask = 0xFF;
    int n = 0, flags = 0;
    if (!cocosearch_parse(m_srchKind->currentIndex(), m_srchEntry->text().toUtf8().constData(), pat, &n, &mask,
                          &flags) || n <= 0) {
        m_srchNote->setText(QStringLiteral("Type hex bytes (3E 5A) or some text."));
        return;
    }
    uint32_t from = 0, found = 0;
    if (m_hl && m_hlSpace == m_space) from = backward ? m_hlLo : m_hlLo + 1;
    const int r = cocodebug_search_next(m_dbg, m_space, pat, n, mask, flags, from, backward ? 1 : 0, &found);
    if (r < 0) {
        m_srchNote->setText(QStringLiteral("Pause the machine first"));
        message(QStringLiteral("Pause the machine first"));
    } else if (r == 0) {
        m_srchNote->setText(QStringLiteral("Not found"));
        message(QStringLiteral("Not found"));
    } else {
        m_srchNote->setText(QStringLiteral("Found at $%1").arg(hx(found, m_space == COCODEBUG_SPACE_PHYS ? 6 : 4)));
        showInMemory(m_space, found, found + (uint32_t)n - 1);
    }
}

void DebuggerWindow::memWrite()
{
    long a, v;
    if (!addressOf(m_memAddr->text(), m_space, &a)) return;
    if (!numberOf(m_memVal->text(), &v)) return;
    if (v < 0 || v > 0xFF) {
        message(QStringLiteral("The value is a byte ($00-$FF)"));
        return;
    }
    const uint8_t byte = (uint8_t)v;
    if (!stopped()) {
        message(QStringLiteral("Pause the machine first"));
        return;
    }
    if (cocodebug_mem_write(m_dbg, m_space, (uint32_t)a, &byte, 1) < 1)
        message(QStringLiteral("$%1 is not RAM: nothing was stored").arg(hx((unsigned)a, 4)));
    resync();
}

void DebuggerWindow::jumpTo()
{
    long a;
    if (!addressOf(m_jump->text(), COCODEBUG_SPACE_CPU, &a)) return;
    m_followPc->setChecked(false);
    m_selected = (int)a;
    m_disasmTop = (uint16_t)cocodebug_row_address(m_dbg, (uint16_t)a, -(visibleRows() / 3));
    refreshDisasm();
}

/* The type takes an address, range or label; the Physical box (CoCo 3) makes
 * it physical RAM. The end is optional; the condition is an expression the
 * engine checks. */
void DebuggerWindow::addBreakpoint()
{
    int type = bpTypeAt(m_bpType->currentIndex());
    const QString start = m_bpStart->text().trimmed(), end = m_bpEnd->text().trimmed();
    if (start.isEmpty()) return;
    const bool phys = m_bpPhys->isChecked() && m_bpPhys->isEnabled();
    uint32_t lo = 0, hi = 0;
    QString why;
    if (!rangeOf(start, end, phys ? COCODEBUG_SPACE_PHYS : COCODEBUG_SPACE_CPU, &lo, &hi, &why)) {
        m_bpMsg->setText(why);
        return;
    }
    if (phys) type |= COCODEBUG_BP_PHYS;
    const QByteArray cond = m_bpCond->text().trimmed().toUtf8();
    if (cocodebug_breakpoint_add(m_dbg, type, lo, hi, cond.constData()) < 0) {
        m_bpMsg->setText(QStringLiteral("The breakpoint could not be added (the condition does not parse, or the "
                                        "table is full)"));
        return;
    }
    m_bpMsg->clear();
    m_bpStart->clear();
    m_bpEnd->clear();
    m_bpCond->clear();
    resync();
}

/* A symbol file the user picks. */
void DebuggerWindow::loadSymbols()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Load Symbols"), QString(),
                                                      QStringLiteral("Symbol files (*.sym *.map *.txt);;All files (*)"));
    if (path.isEmpty()) return;
    const int n = cocodebug_symbols_load(m_dbg, path.toUtf8().constData());
    say(n < 0 ? QStringLiteral("Could not open the symbol file") : QStringLiteral("Loaded %1 symbols").arg(n));
    refreshSymbols(true);
    resync();
}

void DebuggerWindow::clearSymbols()
{
    cocodebug_symbols_clear(m_dbg);
    say(QStringLiteral("Symbols cleared (the built-in ones stay)"));
    refreshSymbols(true);
    resync();
}

/* ---- tabs in windows of their own ----------------------------------------- */

namespace {
/* A tab's window: closing it docks the tab again. */
class PopWindow : public QWidget {
public:
    std::function<void()> onClose;
protected:
    void closeEvent(QCloseEvent *e) override
    {
        if (onClose) onClose();
        QWidget::closeEvent(e);
    }
};
} // namespace

void DebuggerWindow::popOut(int tab)
{
    if (tab < 0 || tab >= TAB_COUNT || m_popped[tab] || m_tabs->count() <= 1) return;     /* one tab always stays */
    QWidget *page = m_pages[tab];
    m_tabs->removeTab(m_tabs->indexOf(page));

    auto *win = new PopWindow;
    win->setAttribute(Qt::WA_DeleteOnClose);
    win->setObjectName(QStringLiteral("debugger-pop-%1").arg(QLatin1String(kTabs[tab].name)));
    win->setWindowTitle(QStringLiteral("CoCo Debugger - %1").arg(QLatin1String(kTabs[tab].label)));
    win->setProperty("cocoHelpContext", QStringLiteral("debugger/%1").arg(QLatin1String(kTabs[tab].help)));
    auto *layout = new QVBoxLayout(win);
    auto *bar = new QHBoxLayout;
    auto *dockBtn = new QPushButton(QStringLiteral("Dock"));
    dockBtn->setToolTip(QStringLiteral("Put this tab back in the debugger"));
    bar->addWidget(dockBtn);
    bar->addStretch(1);
    bar->addWidget(cocoHelpButton(QString()));
    layout->addLayout(bar);
    layout->addWidget(page, 1);
    page->show();
    win->resize(760, 520);
    m_popped[tab] = win;
    static_cast<PopWindow *>(win)->onClose = [this, tab] {
        if (!m_closing) dock(tab);
    };
    connect(dockBtn, &QPushButton::clicked, win, &QWidget::close);
    /* the stepping keys work in the window too */
    new QShortcut(QKeySequence(Qt::Key_F1), win, [win] { cocoHelpFor(win); });
    new QShortcut(QKeySequence(Qt::Key_F5), win, [this] { toggleRun(); });
    new QShortcut(QKeySequence(Qt::Key_F6), win, [this] { stepAnd([this] { cocodebug_step_line(m_dbg); }); });
    new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F6), win, [this] { stepAnd([this] { cocodebug_step_over_line(m_dbg); }); });
    new QShortcut(QKeySequence(Qt::Key_F7), win, [this] { stepAnd([this] { cocodebug_step(m_dbg); }); });
    new QShortcut(QKeySequence(Qt::Key_F8), win, [this] { stepAnd([this] { cocodebug_step_over(m_dbg); }); });
    new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F8), win, [this] { stepAnd([this] { cocodebug_step_out(m_dbg); }); });
    if (isVisible()) win->show();
    if (tab == TAB_VIDEO) m_videoForce = true;
    if (tab == TAB_HISTORY) m_hSeen[1] = -1;
    if (tab == TAB_SOURCE) m_srcSig.clear();
    refreshPage(tab, false);
    watchSound();
}

void DebuggerWindow::dock(int tab)
{
    QWidget *win = m_popped[tab];
    if (!win) return;
    QWidget *page = m_pages[tab];
    m_popped[tab] = nullptr;
    int pos = 0;
    for (int t = 0; t < tab; ++t)       /* back where it was in the order */
        if (!m_popped[t]) ++pos;
    page->setParent(nullptr);
    m_tabs->insertTab(pos, page, QString::fromUtf8(kTabs[tab].label).replace(QLatin1Char('&'), QLatin1String("&&")));
    m_tabs->setCurrentIndex(pos);
    if (tab == TAB_VIDEO) m_videoForce = true;
    if (tab == TAB_HISTORY) m_hSeen[1] = -1;
    if (tab == TAB_SOURCE) m_srcSig.clear();
    refreshPage(tab, false);
    watchSound();
}

/* ---- events --------------------------------------------------------------- */

bool DebuggerWindow::eventFilter(QObject *obj, QEvent *e)
{
    /* Tab completes; Up and Down walk the history */
    if (obj == m_conIn && e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if (ke->key() == Qt::Key_Tab) {
            completeConsole();
            return true;
        }
        if (ke->key() == Qt::Key_Up || ke->key() == Qt::Key_Down) {
            if (ke->key() == Qt::Key_Up && m_historyPos > 0) --m_historyPos;
            else if (ke->key() == Qt::Key_Down && m_historyPos < m_history.size()) ++m_historyPos;
            m_conIn->setText(m_historyPos < m_history.size() ? m_history.at(m_historyPos) : QString());
            return true;
        }
    }
    if (m_mem && obj == m_mem->viewport()) {
        /* the wheel moves the window of rows, three to a notch */
        if (e->type() == QEvent::Wheel) {
            m_wheelMem += static_cast<QWheelEvent *>(e)->angleDelta().y();
            const int rows = -m_wheelMem / 40;
            if (rows != 0) {
                m_wheelMem += rows * 40;
                m_memBar->setValue(m_memBar->value() + rows);
            }
            return true;
        }
        if (e->type() == QEvent::Resize && isVisible()) QTimer::singleShot(0, this, [this] { refreshMem(); });
        return QMainWindow::eventFilter(obj, e);
    }
    if (m_srcView && obj == m_srcView->viewport()) {
        /* A click on a line toggles its breakpoint; nothing else moves the cursor */
        if (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseButtonDblClick) {
            auto *me = static_cast<QMouseEvent *>(e);
            if (me->button() == Qt::LeftButton && e->type() == QEvent::MouseButtonPress)
                sourceToggleAt(m_srcView->cursorForPosition(me->position().toPoint()).blockNumber() + 1);
            return true;
        }
        if (e->type() == QEvent::MouseButtonRelease || e->type() == QEvent::MouseMove) return true;
        return QMainWindow::eventFilter(obj, e);
    }
    if (m_disasm && obj == m_disasm->viewport()) {
        /* A click selects a line (Run to Cursor); a click on its left edge,
         * or a double-click, toggles its breakpoint. A label has no line of
         * its own, so there is nothing else to hit. */
        if (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseButtonDblClick) {
            auto *me = static_cast<QMouseEvent *>(e);
            if (me->button() != Qt::LeftButton) return true;
            const QTextCursor cur = m_disasm->cursorForPosition(me->position().toPoint());
            const int line = cur.blockNumber();
            if (line < 0 || line >= (int)m_lineAddr.size()) return true;
            const bool gutter = cur.positionInBlock() <= 2;
            const bool dbl = e->type() == QEvent::MouseButtonDblClick;
            m_selected = m_lineAddr[(size_t)line];
            if (gutter ? !dbl : dbl) toggleBreakpoint(m_lineAddr[(size_t)line]);
            else refreshDisasm();
            return true;
        }
        if (e->type() == QEvent::MouseButtonRelease || e->type() == QEvent::MouseMove) return true;
        if (e->type() == QEvent::Wheel) {
            m_wheel += static_cast<QWheelEvent *>(e)->angleDelta().y();
            const int rows = -m_wheel / 40;     /* a notch is three rows */
            if (rows != 0) {
                m_wheel += rows * 40;
                m_followPc->setChecked(false);
                m_disasmTop = (uint16_t)cocodebug_row_address(m_dbg, m_disasmTop, rows);
                refreshDisasm();
            }
            return true;
        }
        if (e->type() == QEvent::Resize && isVisible()) refreshDisasm();
    }
    return QMainWindow::eventFilter(obj, e);
}

/* These keys are the debugger's here, taken as plain key presses ahead of
 * any application-wide shortcut. */
bool DebuggerWindow::event(QEvent *e)
{
    if (e->type() == QEvent::ShortcutOverride && isDebuggerKey(static_cast<QKeyEvent *>(e))) {
        e->accept();
        return true;
    }
    return QMainWindow::event(e);
}

void DebuggerWindow::keyPressEvent(QKeyEvent *e)
{
    if (!isDebuggerKey(e)) {
        QMainWindow::keyPressEvent(e);
        return;
    }
    const bool shift = e->modifiers() & Qt::ShiftModifier;
    /* held, the steps repeat; the others do not */
    if (e->isAutoRepeat() && (e->key() == Qt::Key_F5 || e->key() == Qt::Key_F9 || e->key() == Qt::Key_F12)) return;
    switch (e->key()) {
    case Qt::Key_F5: toggleRun(); break;
    case Qt::Key_F6:
        if (!cocodebug_source_available(m_dbg)) message(QStringLiteral("Load C source first (Load C Source…)"));
        else stepAnd(shift ? std::function<void()>([this] { cocodebug_step_over_line(m_dbg); })
                           : std::function<void()>([this] { cocodebug_step_line(m_dbg); }));
        break;
    case Qt::Key_F7: stepAnd([this] { cocodebug_step(m_dbg); }); break;
    case Qt::Key_F8: stepAnd(shift ? std::function<void()>([this] { cocodebug_step_out(m_dbg); })
                                   : std::function<void()>([this] { cocodebug_step_over(m_dbg); })); break;
    case Qt::Key_F9: toggleSelected(); break;
    default: hide(); break;     /* F12 */
    }
}

/* Shown: the engine engages and pauses, which stops the machine, as on every
 * sibling. The tabs have no size until the window is up, so the first refresh
 * waits for the event loop. A restore from the taskbar is not a show. */
void DebuggerWindow::showEvent(QShowEvent *e)
{
    QMainWindow::showEvent(e);
    m_timer.start(100);
    if (e->spontaneous()) return;
    for (QWidget *win : m_popped)
        if (win) win->show();
    cocodebug_set_engaged(m_dbg, 1);
    cocodebug_pause(m_dbg);
    static bool historyOnce = true;     /* COCO_HISTORY: record from the first show */
    if (historyOnce && qEnvironmentVariableIsSet("COCO_HISTORY")) cocodebug_trace_enable(m_dbg, 1);
    historyOnce = false;
    m_wasStopped = false;
    QTimer::singleShot(0, this, [this] {
        if (!isVisible()) return;
        if (m_tabs->currentIndex() == TAB_CONSOLE) m_conIn->setFocus();
        watchSound();
        settleAndSync();
    });
}

/* Hidden by F12, the close button or the main window: the engine is released
 * and the machine runs on. The close button is a plain close, which only
 * hides the window (it is not deleted, so it comes back as it was);
 * ignoring the close instead would make QApplication's quit, which closes
 * every window first, give up whenever it reached this one. A minimised
 * window's spontaneous hide is not a release. */
void DebuggerWindow::hideEvent(QHideEvent *e)
{
    m_timer.stop();
    if (!e->spontaneous()) {
        for (QWidget *win : m_popped)
            if (win) win->hide();
        if (m_soundWatching) {
            m_soundWatching = false;
            cocodebug_sound_watch(m_dbg, 0);
        }
        cocodebug_set_engaged(m_dbg, 0);
    }
    QMainWindow::hideEvent(e);
}
