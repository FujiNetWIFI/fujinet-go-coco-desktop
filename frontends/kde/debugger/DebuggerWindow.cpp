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
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "debugger/DebuggerWindow.h"

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QShortcut>
#include <QTimer>
#include <QVBoxLayout>

#include <utility>

#include "cocodebug.h"

namespace {

/* How much disassembly to show. Rebuilt on every stop, so this is a
 * screenful and a bit rather than a scrollback. */
constexpr int kDisasmLines = 24;

/* The memory pane: a fixed-size hex dump rebuilt from m_memViewAddr on every
 * stop, the same way the disassembly list is. */
constexpr int kMemBytesPerRow = 16;
constexpr int kMemRows = 16;

class DebuggerDialog : public QWidget {
public:
    DebuggerDialog(QWidget *parent, cocodebug *dbg)
        : QWidget(parent, Qt::Window), m_dbg(dbg)
    {
        setWindowTitle(QStringLiteral("Debugger"));
        resize(680, 760);

        auto *outer = new QVBoxLayout(this);

        m_status = new QLabel(QStringLiteral("Running"), this);
        outer->addWidget(m_status);

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
        addButton(bar, QStringLiteral("Clear Breakpoints"),
                  QStringLiteral("Remove every breakpoint"), [this] {
                      cocodebug_breakpoint_clear_all(m_dbg);
                      m_seenSerial = 0; /* redraw the markers */
                  });
        bar->addStretch();
        outer->addLayout(bar);

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

        m_regs = new QLabel(this);
        m_regs->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        m_regs->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        m_regs->setMinimumWidth(200);
        panes->addWidget(m_regs);
        outer->addLayout(panes, 1);

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
        outer->addLayout(memBar);

        m_mem = new QLabel(this);
        m_mem->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        m_mem->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        outer->addWidget(m_mem);

        new QShortcut(QKeySequence(Qt::Key_F5), this, [this] { toggleRun(); });
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

    void poll()
    {
        const uint64_t serial = cocodebug_stop_serial(m_dbg);
        const bool paused = cocodebug_is_paused(m_dbg) != 0;
        /* Rebuild when the engine has stopped somewhere new, or when it has
         * just started running again and the panes need clearing. */
        if (serial == m_seenSerial && paused == m_wasPaused)
            return;
        m_seenSerial = serial;
        m_wasPaused = paused;
        refresh(paused);
    }

    void refresh(bool paused)
    {
        m_runPause->setText(paused ? QStringLiteral("Run")
                                   : QStringLiteral("Pause"));
        m_status->setText(paused ? QStringLiteral("Paused")
                                 : QStringLiteral("Running"));

        m_disasm->clear();
        if (!paused) {
            /* Everything on show is only valid while the machine is parked,
             * so clear it rather than leave a stale snapshot looking live. */
            m_regs->clear();
            m_mem->clear();
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
            /* ">" is where the CPU is, "*" a breakpoint, both in the same
             * column so the addresses stay aligned. */
            const QString text =
                QStringLiteral("%1%2 %3  %4 %5")
                    .arg(l.addr == r.pc ? QLatin1Char('>') : QLatin1Char(' '))
                    .arg(cocodebug_breakpoint_is_set(m_dbg, l.addr)
                             ? QLatin1Char('*') : QLatin1Char(' '))
                    .arg(hex16(l.addr), bytes.leftJustified(10),
                         QString::fromUtf8(l.text));
            auto *item = new QListWidgetItem(text, m_disasm);
            item->setData(Qt::UserRole, (uint)l.addr);
        }

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

    cocodebug *m_dbg;
    QLabel *m_status = nullptr;
    QLabel *m_regs = nullptr;
    QLabel *m_mem = nullptr;
    QLineEdit *m_memAddr = nullptr;
    QListWidget *m_disasm = nullptr;
    QPushButton *m_runPause = nullptr;
    uint64_t m_seenSerial = 0;
    bool m_wasPaused = false;
    uint16_t m_memViewAddr = 0;
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

    auto *w = new DebuggerDialog(parent, dbg);
    w->setAttribute(Qt::WA_DeleteOnClose);
    g_window = w;
    w->show();
}

} // namespace DebuggerWindow
