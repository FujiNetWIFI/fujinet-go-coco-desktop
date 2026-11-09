/*
 * ControllersWindow -- see ControllersWindow.h.
 *
 * The CoCo's two joysticks as a player sees them: the LEFT one on the left
 * and the RIGHT one (port 0, the one nearly every game reads) on the right,
 * each a pale base plate with a round gate, a black stick and red fire buttons
 * (a CoCo 3 has two), RESET centred above them, and, chosen by the Keyboard
 * checkbox, the machine's own keyboard below. Everything is painted from the
 * session's layouts (cocosession_controls_layout and _keyboard_layout), so
 * every frontend shows the same parts in the same places.
 *
 * The joysticks are analog. A drag on the gate moves the stick to the pointer
 * (cocosession_stick_from_vector -> cocosession_pointer_set), a corner is a
 * full diagonal, and the strongest source wins in the core, so the knob you
 * see is the merged position of keys, pads and the mouse. The original Tandy
 * sticks did not centre: "Hold stick" keeps the position when the drag ends.
 * Left-press a fire button to hold it; right-press anywhere on a joystick
 * holds button 1 and middle-press button 2. RESET and Stop in Debugger act on
 * release over them.
 *
 * Keys typed here go to the CoCo exactly as in the main window (every key is
 * the machine's), except in Map mode. The keyboard section injects the same
 * machine keys a host key would; SHIFT, CTRL and ALT stick until the next
 * ordinary key.
 *
 * FIXED SIZE: the drawing is a fixed number of pixels per layout unit, Map
 * legends are fitted into their parts, and gamepad names are elided, so
 * neither Map mode nor a long name resizes the window; only the Keyboard
 * checkbox and a machine change do. A singleton, hidden rather than
 * destroyed, so a half-done remap survives closing it. Timers run only while
 * it is visible.
 *
 * MAP MODE: Map arms a two-step rebind -- click a stick direction, a fire
 * button, RESET or Stop in Debugger, then press a host key or a gamepad
 * button. A key that is bound stops going to the CoCo. Rebinding steals the
 * key from whatever held it (named in the hint). Escape backs out; while Map
 * is armed nothing is typed into the CoCo. The keyboard is dimmed and inert.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ControllersWindow.h"

#include <cmath>

#include <QCheckBox>
#include <QFontDatabase>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPolygonF>
#include <QPushButton>
#include <QRadialGradient>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>
#include <QtMath>

#include <functional>
#include <utility>
#include <vector>

#include "../KeyForward.h"
#include "../debugger/DebuggerWindow.h"
#include "../help/HelpWindow.h"
#include "PadRows.h"

namespace {

constexpr qreal kUnit = 36.0;                 /* pixels per joystick layout unit (a fire button) */
constexpr qreal kTravel = 1.25 * kUnit;       /* the knob's travel from the gate's centre */
constexpr qreal kKnobRadius = 0.55 * kUnit;
constexpr qreal kKeyUnit = 28.0;              /* pixels per keyboard layout unit (a key) */
constexpr int kPadX = 18;                     /* the case's margin round the joysticks */
constexpr int kStrip = 22;                    /* the header strip along the top */
constexpr int kTop = kStrip + 14;             /* where the joystick layout's origin sits */
constexpr int kBottom = 44;                   /* room for the Stop in Debugger plate */
constexpr int kKeyMargin = 14;
constexpr int kMaxPadRows = 3;
constexpr int kLitMilli = 250;                /* a direction is lit past this deflection */
constexpr int kRingMilli = 150;               /* ... and the knob rings past the dead zone */

const QColor kAccent = QColor::fromRgb(QRgb(COCOSESSION_ACCENT_RGB));
const QColor kAccentInk(0x14, 0x21, 0x0A);
const QColor kAccentEdge(0xFF, 0xFF, 0xFF);

/* The case, the strip and the keycaps differ per machine: a CoCo 3 is silver
 * grey with dark keys, a CoCo 2 cream. */
struct Palette {
    QColor caseTop, caseBottom, caseLight, caseEdge;
    QColor stripFill, stripInk;
    QColor capFill, capSide, capInk, capShifted, capSpecial, capFunction;
    QString title;
};

const Palette &paletteFor(int machine)
{
    static const Palette coco3 = {
        QColor(0xB4, 0xB5, 0xB8), QColor(0x8E, 0x8F, 0x93), QColor(0xD9, 0xDA, 0xDC), QColor(0x5E, 0x5F, 0x63),
        QColor(0x26, 0x27, 0x2A), QColor(0xC9, 0xCA, 0xCC),
        QColor(0x2E, 0x2F, 0x33), QColor(0x1F, 0x20, 0x23), QColor(0xEC, 0xEC, 0xEC), QColor(0x9F, 0xA0, 0xA5),
        QColor(0x3E, 0x3F, 0x45), QColor(0x55, 0x56, 0x5C), QStringLiteral("COLOR COMPUTER 3")
    };
    static const Palette coco2 = {
        QColor(0xE6, 0xE1, 0xD3), QColor(0xCF, 0xC9, 0xB8), QColor(0xF4, 0xF1, 0xE8), QColor(0x9B, 0x94, 0x84),
        QColor(0x3A, 0x3A, 0x3C), QColor(0xE6, 0xE1, 0xD3),
        QColor(0x3B, 0x3A, 0x38), QColor(0x26, 0x25, 0x23), QColor(0xF2, 0xEF, 0xE6), QColor(0xA8, 0xA5, 0x9B),
        QColor(0x4A, 0x48, 0x44), QColor(0x5A, 0x58, 0x54), QStringLiteral("COLOR COMPUTER 2")
    };
    return machine == COCOSESSION_MACHINE_COCO2 ? coco2 : coco3;
}

const QColor kInk(0x2A, 0x2B, 0x2E);          /* dark text on the case */

QString local(const char *text) { return QString::fromUtf8(text ? text : ""); }

QFont scaledFont(QFont font, qreal scale, bool bold = false)
{
    font.setPointSizeF((font.pointSizeF() > 0 ? font.pointSizeF() : 10) * scale);
    font.setBold(bold);
    return font;
}

/* Lines centred in box, the font shrunk until they fit. */
void drawFitted(QPainter &paint, const QRectF &box, const QString &text, QFont font,
                Qt::Alignment align = Qt::AlignCenter)
{
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (;;) {
        const QFontMetricsF metrics(font);
        qreal wide = 0;
        for (const QString &line : lines)
            wide = qMax(wide, metrics.horizontalAdvance(line));
        if ((wide <= box.width() && metrics.lineSpacing() * lines.size() <= box.height() + 2) ||
            font.pointSizeF() <= 5.0)
            break;
        font.setPointSizeF(font.pointSizeF() - 0.5);
    }
    paint.setFont(font);
    paint.drawText(box, int(align), text);
}

QString keyName(cocosession *session, int target)
{
    const coco_binding bind = cocosession_binding_get(session, target);
    char key[48] = "";
    if (bind.keysym)
        cocosession_keysym_name(bind.keysym, key, sizeof key);
    return local(key);
}

/* A part's binding as Map mode shows it: the key over the gamepad button. */
QString bindingText(cocosession *session, int target)
{
    const coco_binding bind = cocosession_binding_get(session, target);
    const QString key = keyName(session, target);
    QString text = key.isEmpty() ? QStringLiteral("—") : key;
    if (bind.button != COCO_PAD_BTN_NONE)   /* "D-pad Right" is too long for a sector */
        text += QLatin1Char('\n') + local(coco_pad_button_name(bind.button)).replace(QStringLiteral("D-pad "), QStringLiteral("D-"));
    return text;
}

QString bindingTip(cocosession *session, int target)
{
    const coco_binding bind = cocosession_binding_get(session, target);
    const QString key = keyName(session, target);
    QString tip = QStringLiteral("%1: %2").arg(local(coco_target_name(target)),
                                               key.isEmpty() ? QStringLiteral("no key") : key);
    if (bind.button != COCO_PAD_BTN_NONE)
        tip += QStringLiteral(", ") + local(coco_pad_button_name(bind.button));
    return tip;
}

/* A direction, degrees counter-clockwise from 3 o'clock, as drawn. */
QPointF unitVector(qreal deg)
{
    return QPointF(std::cos(qDegreesToRadians(deg)), -std::sin(qDegreesToRadians(deg)));
}

/* Where each action's sector of the gate sits (the layout's: RIGHT 0, UP 90, LEFT 180, DOWN 270). */
qreal sectorAngle(int act)
{
    switch (act) {
    case COCO_ACT_RIGHT: return 0;
    case COCO_ACT_UP: return 90;
    case COCO_ACT_LEFT: return 180;
    default: return 270;
    }
}

int actOf(const coco_control &ctl) { return ctl.target - COCO_TARGET_PORT(ctl.port, 0); }

QPainterPath sectorPath(const QPointF &centre, qreal inner, qreal outer, int act)
{
    const qreal start = sectorAngle(act) - 45.0 + 1.0, sweep = 88.0;
    const QRectF outerBox(centre.x() - outer, centre.y() - outer, 2 * outer, 2 * outer);
    const QRectF innerBox(centre.x() - inner, centre.y() - inner, 2 * inner, 2 * inner);
    QPainterPath path;
    path.arcMoveTo(outerBox, start);
    path.arcTo(outerBox, start, sweep);
    path.arcTo(innerBox, start + sweep, -sweep);
    path.closeSubpath();
    return path;
}

/* ---- JoystickWidget: the two joysticks and RESET ------------------------------------------ */

class JoystickWidget : public QWidget {
public:
    /* onPick: a part was chosen in Map mode (its target); onHint: a message
     * for the hint line; onStop: the Stop in Debugger plate was pressed */
    JoystickWidget(cocosession *session, std::function<void(int)> onPick,
                   std::function<void(const QString &)> onHint, std::function<void()> onStop)
        : m_session(session), m_onPick(std::move(onPick)), m_onHint(std::move(onHint)),
          m_onStop(std::move(onStop))
    {
        setFocusPolicy(Qt::NoFocus);
        load();
    }

    int machine() const { return m_machine; }

    /* Fetches the layout of the machine now selected (also the first time). */
    void load()
    {
        releaseAll();
        m_machine = cocosession_machine(m_session);
        float wide = 0, high = 0;
        m_layout = cocosession_controls_layout(m_machine, &m_count, &wide, &high);
        m_reset = -1;
        for (Pad &pad : m_pad)
            pad = Pad();
        for (int idx = 0; idx < m_count; ++idx) {
            const coco_control &ctl = m_layout[idx];
            if (ctl.kind == COCO_CTL_RESET)
                m_reset = idx;
            if (ctl.port < 0 || ctl.port >= COCOSESSION_JOYSTICK_PORTS)
                continue;
            Pad &pad = m_pad[ctl.port];
            switch (ctl.kind) {
            case COCO_CTL_CAPTION: pad.caption = idx; break;
            case COCO_CTL_BASE: pad.base = idx; break;
            case COCO_CTL_STICK: pad.stick = idx; break;
            case COCO_CTL_READOUT: pad.readout = idx; break;
            case COCO_CTL_BUTTON: {
                const int act = actOf(ctl) - COCO_ACT_BUTTON1;
                if (act >= 0 && act < 2)
                    pad.fire[act] = idx;
                break;
            }
            case COCO_CTL_DIRECTION: {
                const int act = actOf(ctl);
                if (act >= 0 && act < 4)
                    pad.dir[act] = idx;
                break;
            }
            default: break;
            }
        }
        for (Pad &pad : m_pad) {
            if (pad.stick < 0)
                continue;
            const QRectF gate = rectOf(pad.stick);
            pad.centre = gate.center();
            pad.radius = gate.width() / 2;
        }
        setFixedSize(qCeil(wide * kUnit) + 2 * kPadX, kTop + qCeil(high * kUnit) + kBottom);
        update();
    }

    /* Lets go of what the mouse holds right now. A stick drag ends as the
     * release would (so "Hold stick" keeps the position). */
    void releaseGrab()
    {
        if (m_grab == Grab::None)
            return;
        endGrab(false);
    }

    /* Lets go of everything the mouse holds, the held stick included (hide,
     * machine change, Defaults, a restarted session). */
    void releaseAll()
    {
        for (int port = 0; port < COCOSESSION_JOYSTICK_PORTS; ++port) {
            m_ptrBtn[port] = 0;
            setPointer(port, 0, 0, 0);
        }
        m_grab = Grab::None;
        m_grabInside = false;
        update();
    }

    /* -2 idle, -1 armed, >= 0 waiting for a key or pad button for that target */
    void setMap(int state)
    {
        m_map = state;
        update();
    }
    void setShowBindings(bool active)
    {
        m_showBindings = active;
        update();
    }
    void setHold(bool active)
    {
        m_hold = active;
        if (!active && m_grab == Grab::None) {
            for (int port = 0; port < COCOSESSION_JOYSTICK_PORTS; ++port)
                setPointer(port, 0, 0, m_ptrBtn[port]);
        }
    }

    /* What the machine sees held (keyboard, gamepads, the mouse); eases the
     * knob and repaints only on a change. */
    void pollHeld()
    {
        bool changed = false;
        for (int port = 0; port < COCOSESSION_JOYSTICK_PORTS; ++port) {
            int xaxis = 0, yaxis = 0;
            cocosession_joystick_held(m_session, port, &xaxis, &yaxis);
            if (xaxis != m_axisX[port] || yaxis != m_axisY[port]) {
                m_axisX[port] = xaxis;
                m_axisY[port] = yaxis;
                changed = true;
            }
            const unsigned fire = cocosession_buttons_held(m_session, port);
            if (fire != m_fire[port]) {
                m_fire[port] = fire;
                changed = true;
            }
            changed |= ease(m_knobX[port], m_axisX[port] / 1000.0);
            changed |= ease(m_knobY[port], m_axisY[port] / 1000.0);
        }
        const char reset = cocosession_switch_held(m_session, COCO_SW_RESET) != 0;
        if (reset != m_resetHeld) {
            m_resetHeld = reset;
            changed = true;
        }
        if (changed)
            update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter paint(this);
        paint.setRenderHint(QPainter::Antialiasing);
        paintCase(paint);
        paintReset(paint);
        for (int port = COCOSESSION_JOYSTICK_PORTS - 1; port >= 0; --port)
            paintJoystick(paint, port);
        paintStop(paint);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        const QPointF pos = event->position();
        if (mapping()) {
            if (event->button() == Qt::LeftButton)
                pick(pos);
            return;
        }
        if (m_grab != Grab::None)
            return;                              /* one thing at a time */
        if (event->button() == Qt::RightButton || event->button() == Qt::MiddleButton) {
            const int port = blockAt(pos);
            if (port < 0)
                return;
            beginFire(port, event->button() == Qt::RightButton ? 0 : 1, event->button());
            return;
        }
        if (event->button() != Qt::LeftButton)
            return;
        if (stopRect().contains(pos)) {
            m_grab = Grab::Stop;
            m_grabButton = Qt::LeftButton;
            m_grabInside = true;
            update();
            return;
        }
        const int idx = hitAt(pos);
        if (idx < 0)
            return;
        const coco_control &ctl = m_layout[idx];
        switch (ctl.kind) {
        case COCO_CTL_BUTTON:
            beginFire(ctl.port, actOf(ctl) - COCO_ACT_BUTTON1, Qt::LeftButton);
            m_grabIdx = idx;
            return;
        case COCO_CTL_RESET:
            m_grab = Grab::Reset;
            m_grabButton = Qt::LeftButton;
            m_grabIdx = idx;
            m_grabInside = true;
            break;
        case COCO_CTL_STICK:
        case COCO_CTL_DIRECTION: {
            m_grab = Grab::Stick;
            m_grabButton = Qt::LeftButton;
            m_grabPort = ctl.port;
            m_moved = false;
            const QPointF del = pos - m_pad[ctl.port].centre;
            m_startCentred = std::hypot(del.x(), del.y()) < 0.15 * kTravel;
            follow(pos);
            break;
        }
        default: return;
        }
        update();
    }

    /* Dragging off a button lets it go (and back on, holds it again); on the
     * stick it follows the pointer. */
    void mouseMoveEvent(QMouseEvent *event) override
    {
        const QPointF pos = event->position();
        switch (m_grab) {
        case Grab::Stick:
            m_moved = true;
            follow(pos);
            break;
        case Grab::Fire: {
            const bool inside = hitAt(pos) == m_grabIdx || m_grabIdx < 0;
            if (inside != m_grabInside) {
                m_grabInside = inside;
                const unsigned bit = 1u << m_grabBit;
                setPointer(m_grabPort, m_ptrX[m_grabPort], m_ptrY[m_grabPort],
                           inside ? (m_ptrBtn[m_grabPort] | bit) : (m_ptrBtn[m_grabPort] & ~bit));
            }
            break;
        }
        case Grab::Reset:
            m_grabInside = hitAt(pos) == m_grabIdx;
            update();
            break;
        case Grab::Stop:
            m_grabInside = stopRect().contains(pos);
            update();
            break;
        default: break;
        }
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (m_grab == Grab::None || event->button() != m_grabButton)
            return;
        if (m_grab == Grab::Reset || m_grab == Grab::Stop) {
            const QPointF pos = event->position();
            m_grabInside = m_grab == Grab::Reset ? hitAt(pos) == m_grabIdx : stopRect().contains(pos);
        }
        endGrab(true);
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::ToolTip) {
            auto *help = static_cast<QHelpEvent *>(event);
            const QString tip = tipAt(help->pos());
            if (tip.isEmpty())
                QToolTip::hideText();
            else
                QToolTip::showText(help->globalPos(), tip, this);
            return true;
        }
        return QWidget::event(event);
    }

private:
    enum class Grab { None, Stick, Fire, Reset, Stop };

    /* one joystick's parts, by index into the layout */
    struct Pad {
        int caption = -1, base = -1, stick = -1, readout = -1;
        int fire[2] = { -1, -1 };
        int dir[4] = { -1, -1, -1, -1 };      /* by COCO_ACT_RIGHT, LEFT, UP, DOWN */
        QPointF centre;
        qreal radius = 0;
    };

    QRectF rectOf(int idx) const
    {
        const coco_control &ctl = m_layout[idx];
        return QRectF(kPadX + ctl.left * kUnit, kTop + ctl.top * kUnit, ctl.wide * kUnit, ctl.high * kUnit);
    }

    int hitAt(const QPointF &pos) const
    {
        return cocosession_controls_hit(m_machine, float((pos.x() - kPadX) / kUnit),
                                        float((pos.y() - kTop) / kUnit));
    }

    /* the joystick whose plate is under pos, or -1 */
    int blockAt(const QPointF &pos) const
    {
        for (int port = 0; port < COCOSESSION_JOYSTICK_PORTS; ++port)
            if (m_pad[port].base >= 0 && rectOf(m_pad[port].base).contains(pos))
                return port;
        return -1;
    }

    bool mapping() const { return m_map != -2; }
    bool showBind() const { return mapping() || m_showBindings; }

    /* The knob eases toward its goal, 0.4 of the way each poll. */
    static bool ease(qreal &knob, qreal goal)
    {
        const qreal diff = goal - knob;
        if (diff == 0)
            return false;
        knob = std::fabs(diff) < 0.004 ? goal : knob + diff * 0.4;
        return true;
    }

    void setPointer(int port, int xaxis, int yaxis, unsigned buttons)
    {
        if (port < 0 || port >= COCOSESSION_JOYSTICK_PORTS)
            return;
        if (m_ptrX[port] == xaxis && m_ptrY[port] == yaxis && m_ptrBtn[port] == buttons)
            return;
        m_ptrX[port] = xaxis;
        m_ptrY[port] = yaxis;
        m_ptrBtn[port] = buttons;
        cocosession_pointer_set(m_session, port, xaxis, yaxis, buttons);
    }

    void follow(const QPointF &pos)
    {
        const QPointF del = pos - m_pad[m_grabPort].centre;
        int xaxis = 0, yaxis = 0;
        cocosession_stick_from_vector(float(del.x()), float(del.y()), float(kTravel), &xaxis, &yaxis);
        setPointer(m_grabPort, xaxis, yaxis, m_ptrBtn[m_grabPort]);
    }

    void beginFire(int port, int bit, Qt::MouseButton button)
    {
        m_grab = Grab::Fire;
        m_grabButton = button;
        m_grabPort = port;
        m_grabBit = bit;
        m_grabIdx = -1;
        m_grabInside = true;
        setPointer(port, m_ptrX[port], m_ptrY[port], m_ptrBtn[port] | (1u << bit));
    }

    /* The grab ends: `released` is a real mouse release (RESET and Stop fire
     * only then), else the window lost the pointer. */
    void endGrab(bool released)
    {
        const Grab grab = m_grab;
        m_grab = Grab::None;
        switch (grab) {
        case Grab::Stick:
            if (!m_hold || (m_startCentred && !m_moved))
                setPointer(m_grabPort, 0, 0, m_ptrBtn[m_grabPort]);
            break;
        case Grab::Fire:
            setPointer(m_grabPort, m_ptrX[m_grabPort], m_ptrY[m_grabPort],
                       m_ptrBtn[m_grabPort] & ~(1u << m_grabBit));
            break;
        case Grab::Reset:
            if (released && m_grabInside) {
                cocosession_press(m_session, COCO_TARGET_SWITCH(COCO_SW_RESET), 1);
                cocosession_press(m_session, COCO_TARGET_SWITCH(COCO_SW_RESET), 0);
            }
            break;
        case Grab::Stop:
            m_grabInside = released && m_grabInside;
            if (m_grabInside)
                m_onStop();
            break;
        default: break;
        }
        m_grabInside = false;
        update();
    }

    /* Map mode: a click on a part picks its target. */
    void pick(const QPointF &pos)
    {
        if (stopRect().contains(pos)) {
            m_onPick(COCO_TARGET_SYSACT(COCO_SYSACT_DEBUG_STOP));
            return;
        }
        const int idx = hitAt(pos);
        if (idx < 0)
            return;
        const coco_control &ctl = m_layout[idx];
        if (ctl.target >= 0)
            m_onPick(ctl.target);
        else if (ctl.kind == COCO_CTL_STICK)
            m_onHint(QStringLiteral("Pick one of the stick's four directions, a fire button or RESET."));
    }

    QString tipAt(const QPoint &pos) const
    {
        if (stopRect().contains(pos))
            return bindingTip(m_session, COCO_TARGET_SYSACT(COCO_SYSACT_DEBUG_STOP));
        const int idx = hitAt(pos);
        if (idx < 0)
            return QString();
        const coco_control &ctl = m_layout[idx];
        if (ctl.target >= 0)
            return bindingTip(m_session, ctl.target);
        if (ctl.kind == COCO_CTL_STICK)
            return QStringLiteral("Drag to move the stick; a corner is a full diagonal");
        return QString();
    }

    /* the Stop in Debugger plate, along the bottom */
    QRectF stopRect() const
    {
        const qreal wide = 190, high = 28;
        return QRectF((width() - wide) / 2, height() - kBottom + 8, wide, high);
    }

    bool lit(const coco_control &ctl) const
    {
        if (mapping())
            return ctl.target >= 0 && ctl.target == m_map;
        switch (ctl.kind) {
        case COCO_CTL_BUTTON: return (m_fire[ctl.port] >> (actOf(ctl) - COCO_ACT_BUTTON1)) & 1u;
        case COCO_CTL_RESET: return m_resetHeld != 0 || (m_grab == Grab::Reset && m_grabInside);
        case COCO_CTL_DIRECTION:
            switch (actOf(ctl)) {
            case COCO_ACT_RIGHT: return m_axisX[ctl.port] >= kLitMilli;
            case COCO_ACT_LEFT: return m_axisX[ctl.port] <= -kLitMilli;
            case COCO_ACT_DOWN: return m_axisY[ctl.port] >= kLitMilli;
            default: return m_axisY[ctl.port] <= -kLitMilli;
            }
        default: return false;
        }
    }

    /* Waiting for a key: every pickable part but the picked one is dimmed. */
    bool dimmed(const coco_control &ctl) const { return m_map >= 0 && ctl.target != m_map; }

    /* ---- painting ---- */

    void paintCase(QPainter &paint)
    {
        const Palette &pal = paletteFor(m_machine);
        const QRectF body(0.5, 0.5, width() - 1.0, height() - 1.0);
        QLinearGradient shell(0, 0, 0, height());
        shell.setColorAt(0, pal.caseTop);
        shell.setColorAt(1, pal.caseBottom);
        paint.setPen(QPen(pal.caseEdge, 1));
        paint.setBrush(shell);
        paint.drawRoundedRect(body, 16, 16);

        QPainterPath clip;
        clip.addRoundedRect(body, 16, 16);
        paint.save();
        paint.setClipPath(clip);
        paint.setPen(QPen(pal.caseLight, 1.5));
        paint.drawLine(QPointF(8, kStrip + 3), QPointF(width() - 8, kStrip + 3));
        paint.setPen(Qt::NoPen);
        paint.setBrush(pal.stripFill);
        paint.drawRect(QRectF(0, 0, width(), kStrip));
        paint.setBrush(kAccent);
        paint.drawRect(QRectF(0, kStrip - 2, width(), 2));
        paint.restore();
        paint.setPen(pal.stripInk);
        paint.setFont(scaledFont(font(), 0.85, true));
        paint.drawText(QRectF(0, 0, width(), kStrip - 2), Qt::AlignCenter, pal.title);
    }

    void paintReset(QPainter &paint)
    {
        if (m_reset < 0)
            return;
        const coco_control &ctl = m_layout[m_reset];
        const bool active = lit(ctl);
        paint.setOpacity(dimmed(ctl) ? 0.4 : 1.0);
        const QRectF box = rectOf(m_reset).adjusted(1, 1, -1, -3);
        paint.setPen(Qt::NoPen);
        paint.setBrush(active ? kAccent.darker(160) : QColor(0x1F, 0x1F, 0x21));
        paint.drawRoundedRect(box.translated(0, 3), 8, 8);
        paint.setPen(active ? QPen(kAccentEdge, 2) : QPen(QColor(0x14, 0x14, 0x15), 1));
        paint.setBrush(active ? kAccent : QColor(0x3A, 0x3A, 0x3D));
        paint.drawRoundedRect(box, 8, 8);
        paint.setPen(active ? kAccentInk : QColor(0xE0, 0xE0, 0xE0));
        paint.setFont(scaledFont(font(), 0.9, true));
        paint.drawText(box.adjusted(0, -3, 0, -3), Qt::AlignCenter, QStringLiteral("RESET"));
        paint.setPen(QPen(active ? kAccentInk : QColor(0xD3, 0x3A, 0x2F), 2));
        paint.drawLine(QPointF(box.center().x() - box.width() * 0.28, box.bottom() - 7),
                       QPointF(box.center().x() + box.width() * 0.28, box.bottom() - 7));
        paint.setOpacity(1.0);
        if (showBind()) {
            paint.setPen(kInk);
            drawFitted(paint, QRectF(box.left() - 10, box.bottom() + 5, box.width() + 20, kUnit * 0.7),
                       bindingText(m_session, ctl.target), scaledFont(font(), 0.72, true));
        }
    }

    void paintStop(QPainter &paint)
    {
        const int target = COCO_TARGET_SYSACT(COCO_SYSACT_DEBUG_STOP);
        const Palette &pal = paletteFor(m_machine);
        const bool picked = mapping() && m_map == target;
        const bool pressed = m_grab == Grab::Stop && m_grabInside;
        const bool active = picked || pressed;
        const QRectF box = stopRect();
        paint.setOpacity(m_map >= 0 && !picked ? 0.4 : 1.0);
        paint.setPen(QPen(active ? kAccentEdge : pal.caseEdge, active ? 1.5 : 1));
        paint.setBrush(active ? kAccent : pal.stripFill);
        paint.drawRoundedRect(box, 8, 8);
        paint.setPen(active ? kAccentInk : pal.stripInk);
        QString text = QStringLiteral("Stop in Debugger");
        if (showBind())
            text += QStringLiteral("  ") + bindingText(m_session, target).replace(QLatin1Char('\n'), QStringLiteral(" · "));
        drawFitted(paint, box.adjusted(6, 0, -6, 0), text, scaledFont(font(), 0.78, true));
        paint.setOpacity(1.0);
    }

    void paintJoystick(QPainter &paint, int port)
    {
        const Pad &pad = m_pad[port];
        if (pad.base < 0 || pad.stick < 0)
            return;

        /* caption and base plate */
        paint.setPen(kInk);
        paint.setFont(scaledFont(font(), 0.9, true));
        paint.drawText(rectOf(pad.caption), Qt::AlignCenter, local(m_layout[pad.caption].label));
        const QRectF base = rectOf(pad.base);
        paint.setPen(Qt::NoPen);
        paint.setBrush(QColor(0, 0, 0, 64));
        paint.drawRoundedRect(base.translated(2, 4), 0.5 * kUnit, 0.5 * kUnit);
        QLinearGradient plate(base.topLeft(), base.bottomLeft());
        plate.setColorAt(0, QColor(0xDD, 0xD9, 0xCF));
        plate.setColorAt(1, QColor(0xC2, 0xBD, 0xB1));
        paint.setPen(QPen(QColor(0x8D, 0x88, 0x7C), 1));
        paint.setBrush(plate);
        paint.drawRoundedRect(base, 0.5 * kUnit, 0.5 * kUnit);

        paintGate(paint, port);
        for (int act = 0; act < 2; ++act)
            if (pad.fire[act] >= 0)
                paintFire(paint, pad.fire[act]);
        paintReadout(paint, port);
    }

    void paintFire(QPainter &paint, int idx)
    {
        const coco_control &ctl = m_layout[idx];
        const bool active = lit(ctl);
        const QRectF box = rectOf(idx).adjusted(1, 1, -1, -4);
        paint.setOpacity(dimmed(ctl) ? 0.4 : 1.0);
        paint.setPen(Qt::NoPen);
        paint.setBrush(active ? kAccent.darker(170) : QColor(0x7D, 0x14, 0x0E));
        paint.drawEllipse(box.translated(0, 3));
        QRadialGradient face(box.center() + QPointF(-box.width() * 0.2, -box.height() * 0.25), box.width() * 0.8);
        face.setColorAt(0, active ? kAccent.lighter(130) : QColor(0xE5, 0x59, 0x4C));
        face.setColorAt(1, active ? kAccent : QColor(0xC8, 0x28, 0x1E));
        paint.setPen(active ? QPen(kAccentEdge, 2) : QPen(QColor(0x7D, 0x14, 0x0E), 1));
        paint.setBrush(face);
        paint.drawEllipse(box);
        /* the button's number, under the cap (the cap itself carries no letter) */
        paint.setPen(QColor(0x3A, 0x3A, 0x3C));
        paint.setFont(scaledFont(font(), 1.0, true));
        paint.drawText(QRectF(box.center().x() - 0.5 * kUnit, box.bottom() + 4, kUnit, 0.6 * kUnit), Qt::AlignCenter,
                       local(ctl.label));
        paint.setOpacity(1.0);
        if (showBind()) {
            /* beside the button, toward the middle of the plate */
            const bool leftSide = actOf(ctl) == COCO_ACT_BUTTON1;
            const QRectF note(leftSide ? box.right() + 5 : box.left() - 5 - 1.3 * kUnit, box.top() - 2, 1.3 * kUnit,
                              box.height() + 4);
            paint.setPen(kInk);
            drawFitted(paint, note, bindingText(m_session, ctl.target), scaledFont(font(), 0.7, true),
                       (leftSide ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter);
        }
    }

    /* the gate: a recess with the four direction sectors, the stick's shaft and cap */
    void paintGate(QPainter &paint, int port)
    {
        const Pad &pad = m_pad[port];
        const qreal rad = pad.radius;
        const QPointF mid = pad.centre;
        paint.setPen(Qt::NoPen);
        paint.setBrush(QColor(0x8F, 0x8B, 0x80));
        paint.drawEllipse(mid, rad, rad);
        paint.setBrush(QColor(0xA9, 0xA5, 0x98));
        paint.drawEllipse(mid, rad * 0.93, rad * 0.93);
        /* cross-hairs and the four diagonal ticks */
        paint.setPen(QPen(QColor(0x7A, 0x76, 0x6B), 1));
        paint.drawLine(mid + QPointF(-rad * 0.93, 0), mid + QPointF(rad * 0.93, 0));
        paint.drawLine(mid + QPointF(0, -rad * 0.93), mid + QPointF(0, rad * 0.93));
        for (int tick = 0; tick < 4; ++tick) {
            const QPointF dir = unitVector(45.0 + 90.0 * tick);
            paint.drawLine(mid + dir * (rad * 0.80), mid + dir * (rad * 0.93));
        }

        /* the four sectors: lit while held, a faint chevron otherwise, the
         * bound key under Map and Show bindings */
        const qreal inner = rad * 0.5, outer = rad * 0.93;
        for (int act = 0; act < 4; ++act) {
            if (pad.dir[act] < 0)
                continue;
            const coco_control &ctl = m_layout[pad.dir[act]];
            const bool active = lit(ctl);
            paint.setOpacity(dimmed(ctl) ? 0.4 : 1.0);
            if (active || showBind()) {
                paint.setPen(QPen(active ? kAccentEdge : QColor(0x7A, 0x76, 0x6B), active ? 1.5 : 1));
                paint.setBrush(active ? kAccent : QColor(0xB9, 0xB5, 0xA8));
                paint.drawPath(sectorPath(mid, inner, outer, act));
            }
            const qreal angle = sectorAngle(act);
            const bool sideways = std::abs(unitVector(angle).x()) > 0.5;
            const QPointF centreOfSector = mid + unitVector(angle) * (sideways ? rad * 0.62 : (inner + outer) / 2);
            if (showBind()) {
                // left and right: the legend is held inside the rim
                const qreal half = sideways ? qMin(24.0, outer - rad * 0.62 - 1) : 24.0;
                paint.setPen(active ? kAccentInk : kInk);
                drawFitted(paint, QRectF(centreOfSector.x() - half, centreOfSector.y() - 13, half * 2, 26),
                           bindingText(m_session, ctl.target), scaledFont(font(), 0.7, true));
            } else {
                const QPointF dir = unitVector(angle), perp(-dir.y(), dir.x());
                const qreal size = 4.5;
                QPolygonF chevron;
                chevron << centreOfSector + dir * size << centreOfSector - dir * size + perp * (size * 1.3)
                        << centreOfSector - dir * size - perp * (size * 1.3);
                paint.setPen(Qt::NoPen);
                paint.setBrush(active ? kAccentInk : QColor(0x5D, 0x5A, 0x52));
                paint.drawPolygon(chevron);
            }
            paint.setOpacity(1.0);
        }

        /* the stick: its shaft's shadow on the floor, then the cap at the merged position */
        qreal knobX = m_knobX[port], knobY = m_knobY[port];
        const qreal length = std::hypot(knobX, knobY);
        if (length > 1) {
            knobX /= length;
            knobY /= length;
        }
        const QPointF cap = mid + QPointF(knobX, knobY) * kTravel;
        paint.setPen(QPen(QColor(0, 0, 0, 70), 7, Qt::SolidLine, Qt::RoundCap));
        paint.drawLine(mid + QPointF(2, 3), cap + QPointF(3, 4));
        paint.setPen(Qt::NoPen);
        paint.setBrush(QColor(0x1A, 0x1A, 0x1C));
        paint.drawEllipse(mid, 0.3 * kUnit, 0.3 * kUnit);
        QRadialGradient dome(cap + QPointF(-kKnobRadius * 0.35, -kKnobRadius * 0.4), kKnobRadius * 1.6);
        dome.setColorAt(0, QColor(0x5A, 0x5A, 0x60));
        dome.setColorAt(0.6, QColor(0x1A, 0x1A, 0x1C));
        dome.setColorAt(1, QColor(0x0A, 0x0A, 0x0B));
        const bool deflected = std::abs(m_axisX[port]) >= kRingMilli || std::abs(m_axisY[port]) >= kRingMilli;
        paint.setPen(deflected && !mapping() ? QPen(kAccent, 2.5) : QPen(QColor(0x0A, 0x0A, 0x0B), 1.2));
        paint.setBrush(dome);
        paint.drawEllipse(cap, kKnobRadius, kKnobRadius);
    }

    void paintReadout(QPainter &paint, int port)
    {
        const Pad &pad = m_pad[port];
        if (pad.readout < 0)
            return;
        const QRectF box = rectOf(pad.readout);
        paint.setPen(QPen(QColor(0x9A, 0x96, 0x8A), 1));
        paint.setBrush(QColor(0xB9, 0xB5, 0xA8));
        paint.drawRoundedRect(box, 5, 5);
        paint.setPen(QColor(0x3B, 0x3A, 0x36));
        paint.setFont(scaledFont(QFontDatabase::systemFont(QFontDatabase::FixedFont), 0.95, true));
        paint.drawText(box, Qt::AlignCenter,
                       QStringLiteral("X %1  Y %2")
                           .arg(coco_axis_6bit(m_axisX[port]), 2)
                           .arg(coco_axis_6bit(m_axisY[port]), 2));
    }

    cocosession *m_session;
    std::function<void(int)> m_onPick;
    std::function<void(const QString &)> m_onHint;
    std::function<void()> m_onStop;
    int m_machine = -1;
    const coco_control *m_layout = nullptr;
    int m_count = 0;
    int m_reset = -1;
    Pad m_pad[COCOSESSION_JOYSTICK_PORTS];

    /* what the machine sees, polled */
    int m_axisX[COCOSESSION_JOYSTICK_PORTS] = {};
    int m_axisY[COCOSESSION_JOYSTICK_PORTS] = {};
    unsigned m_fire[COCOSESSION_JOYSTICK_PORTS] = {};
    qreal m_knobX[COCOSESSION_JOYSTICK_PORTS] = {};
    qreal m_knobY[COCOSESSION_JOYSTICK_PORTS] = {};
    char m_resetHeld = 0;

    /* what this window holds as the pointer source */
    int m_ptrX[COCOSESSION_JOYSTICK_PORTS] = {};
    int m_ptrY[COCOSESSION_JOYSTICK_PORTS] = {};
    unsigned m_ptrBtn[COCOSESSION_JOYSTICK_PORTS] = {};

    bool m_showBindings = false;
    bool m_hold = false;
    int m_map = -2;
    Grab m_grab = Grab::None;
    Qt::MouseButton m_grabButton = Qt::NoButton;
    int m_grabIdx = -1;           /* the part the mouse holds (RESET, a fire button) */
    int m_grabPort = 0;
    int m_grabBit = 0;            /* a fire button's bit in the pointer's buttons */
    bool m_grabInside = false;    /* still over the part it began on */
    bool m_moved = false;         /* a stick drag moved after the press */
    bool m_startCentred = false;  /* ... and began on the knob's resting place */
};

/* ---- KeyboardWidget: the machine's keys ----------------------------------------------------- */

class KeyboardWidget : public QWidget {
public:
    explicit KeyboardWidget(cocosession *session) : m_session(session)
    {
        setFocusPolicy(Qt::NoFocus);
        load();
    }

    int machine() const { return m_machine; }

    void load()
    {
        releaseGrab();
        m_machine = cocosession_machine(m_session);
        float wide = 0, high = 0;
        m_caps = cocosession_keyboard_layout(m_machine, &m_count, &wide, &high);
        m_bits.assign(size_t(m_count), 0);
        setFixedSize(qCeil(wide * kKeyUnit) + 2 * kKeyMargin, qCeil(high * kKeyUnit) + 2 * kKeyMargin);
        update();
    }

    /* Lets go of the key the mouse holds (latched modifiers stay: the core owns them). */
    void releaseGrab()
    {
        if (m_grab >= 0 && m_grab < m_count)
            cocosession_keyboard_press(m_session, m_grab, 0);
        m_grab = -1;
    }

    /* Map mode: dimmed, and presses do nothing. */
    void setInert(bool inert)
    {
        if (inert)
            releaseGrab();
        m_inert = inert;
        update();
    }

    void pollHeld()
    {
        bool changed = false;
        for (int idx = 0; idx < m_count; ++idx) {
            const int bits = cocosession_keyboard_held(m_session, idx);
            if (bits != m_bits[size_t(idx)]) {
                m_bits[size_t(idx)] = bits;
                changed = true;
            }
        }
        if (changed)
            update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter paint(this);
        paint.setRenderHint(QPainter::Antialiasing);
        const Palette &pal = paletteFor(m_machine);
        const QRectF body(0.5, 0.5, width() - 1.0, height() - 1.0);
        QLinearGradient shell(0, 0, 0, height());
        shell.setColorAt(0, pal.caseTop);
        shell.setColorAt(1, pal.caseBottom);
        paint.setPen(QPen(pal.caseEdge, 1));
        paint.setBrush(shell);
        paint.drawRoundedRect(body, 14, 14);
        paint.setPen(QPen(pal.caseLight, 1.5));
        paint.drawLine(QPointF(10, 2), QPointF(width() - 10, 2));
        paint.setOpacity(m_inert ? 0.4 : 1.0);
        for (int idx = 0; idx < m_count; ++idx)
            paintCap(paint, idx);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || m_inert || m_grab >= 0)
            return;
        const int idx = capAt(event->position());
        if (idx < 0)
            return;
        m_grab = idx;
        cocosession_keyboard_press(m_session, idx, 1);
        pollHeld();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || m_grab < 0)
            return;
        releaseGrab();
        pollHeld();
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::ToolTip) {
            auto *help = static_cast<QHelpEvent *>(event);
            const int idx = capAt(help->pos());
            if (idx < 0 || m_caps[idx].kind != COCO_KEYCAP_MODIFIER) {
                QToolTip::hideText();
            } else {
                QToolTip::showText(help->globalPos(),
                                   QStringLiteral("Sticks until the next key"), this);
            }
            return true;
        }
        return QWidget::event(event);
    }

private:
    QRectF rectOf(const coco_keycap &cap) const
    {
        return QRectF(kKeyMargin + cap.left * kKeyUnit, kKeyMargin + cap.top * kKeyUnit, cap.wide * kKeyUnit,
                      cap.high * kKeyUnit);
    }

    int capAt(const QPointF &pos) const
    {
        for (int idx = m_count - 1; idx >= 0; --idx)
            if (rectOf(m_caps[idx]).contains(pos))
                return idx;
        return -1;
    }

    void paintCap(QPainter &paint, int idx)
    {
        const coco_keycap &cap = m_caps[idx];
        const Palette &pal = paletteFor(m_machine);
        const int bits = m_bits[size_t(idx)];
        const bool active = (bits & 1) != 0;
        const bool latched = (bits & 2) != 0;
        const QRectF face = rectOf(cap).adjusted(0, 0, 0, -2);
        QColor fill = pal.capFill;
        if (cap.kind == COCO_KEYCAP_SPECIAL || cap.kind == COCO_KEYCAP_MODIFIER)
            fill = pal.capSpecial;
        else if (cap.kind == COCO_KEYCAP_FUNCTION)
            fill = pal.capFunction;

        paint.setPen(Qt::NoPen);
        paint.setBrush(pal.capSide);
        paint.drawRoundedRect(face.translated(0, 2), 4, 4);
        paint.setPen(active || latched ? QPen(kAccentEdge, 1.5) : QPen(pal.capSide, 1));
        paint.setBrush(active || latched ? kAccent : fill);
        paint.drawRoundedRect(face, 4, 4);

        const QColor ink = active || latched ? kAccentInk : pal.capInk;
        paint.setPen(ink);
        if (cap.kind == COCO_KEYCAP_ARROW) {
            paintArrow(paint, face, QString::fromUtf8(cap.label), ink);
            return;
        }
        const QString label = local(cap.label);
        const bool word = label.size() > 1;
        if (cap.shifted) {
            paint.setPen(active || latched ? kAccentInk : pal.capShifted);
            drawFitted(paint, QRectF(face.left() + 3, face.top() + 1, face.width() - 6, face.height() * 0.4),
                       local(cap.shifted), scaledFont(font(), 0.62, true), Qt::AlignLeft | Qt::AlignVCenter);
            paint.setPen(ink);
            drawFitted(paint, QRectF(face.left(), face.top() + face.height() * 0.32, face.width(), face.height() * 0.68),
                       label, scaledFont(font(), 0.95, true));
        } else {
            drawFitted(paint, face.adjusted(2, 0, -2, 0), label, scaledFont(font(), word ? 0.72 : 1.0, true));
        }
        if (latched) {
            paint.setPen(Qt::NoPen);
            paint.setBrush(kAccentInk);
            paint.drawEllipse(QPointF(face.left() + 6, face.top() + 6), 2.5, 2.5);
        }
    }

    /* an arrow key: a drawn triangle, so no font has to carry the glyph */
    static void paintArrow(QPainter &paint, const QRectF &face, const QString &glyph, const QColor &ink)
    {
        const QPointF mid = face.center();
        const qreal size = face.height() * 0.28;
        QPolygonF head;
        const ushort code = glyph.isEmpty() ? 0 : glyph.at(0).unicode();
        if (code == 0x2191)
            head << mid + QPointF(0, -size) << mid + QPointF(-size, size * 0.8) << mid + QPointF(size, size * 0.8);
        else if (code == 0x2193)
            head << mid + QPointF(0, size) << mid + QPointF(-size, -size * 0.8) << mid + QPointF(size, -size * 0.8);
        else if (code == 0x2190)
            head << mid + QPointF(-size, 0) << mid + QPointF(size * 0.8, -size) << mid + QPointF(size * 0.8, size);
        else
            head << mid + QPointF(size, 0) << mid + QPointF(-size * 0.8, -size) << mid + QPointF(-size * 0.8, size);
        paint.setPen(Qt::NoPen);
        paint.setBrush(ink);
        paint.drawPolygon(head);
    }

    cocosession *m_session;
    int m_machine = -1;
    const coco_keycap *m_caps = nullptr;
    int m_count = 0;
    std::vector<int> m_bits;
    int m_grab = -1;              /* the cap the mouse holds */
    bool m_inert = false;
};

/* ---- ControllersWindow --------------------------------------------------------------------- */

class ControllersWindow : public QWidget {
public:
    ControllersWindow(cocosession *session, QWidget *parent);
    void present();
    void refresh();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void changeEvent(QEvent *event) override;
    bool event(QEvent *event) override;
    /* Tab is a key of the CoCo's, not focus traversal. */
    bool focusNextPrevChild(bool) override { return false; }

private:
    void setMapState(int state);
    void bound(int target, const QString &what, const char *stolen);
    void pollCapture();
    void pollHeld();
    void pollSlow();
    void stopInDebugger();
    void machineChanged();

    cocosession *m_session;
    JoystickWidget *m_joy = nullptr;
    KeyboardWidget *m_keys = nullptr;
    PadRows *m_pads = nullptr;
    QPushButton *m_mapButton = nullptr;
    QPushButton *m_clearButton = nullptr;
    ElidedLabel *m_hint = nullptr;
    QTimer m_captureTimer;
    QTimer m_heldTimer;
    QTimer m_slowTimer;
    uint32_t m_swallow = 0;               /* the key Map took: its repeats and release too */
    /* -2 idle, -1 armed and waiting for a target, >= 0 waiting for a key or
     * gamepad button for that target. */
    int m_mapState = -2;
};

ControllersWindow::ControllersWindow(cocosession *session, QWidget *parent)
    /* A dialog-type window, so a tiling compositor floats it: it is a panel
     * of fixed-size parts and nothing here wants more room. */
    : QWidget(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::CustomizeWindowHint),
      m_session(session)
{
    setObjectName(QStringLiteral("controllers"));
    setWindowTitle(QStringLiteral("Controllers"));
    setProperty("cocoHelpContext", QStringLiteral("controllers"));
    setFocusPolicy(Qt::StrongFocus);

    auto *root = new QVBoxLayout(this);
    /* Sized to its parts and kept there: a resizable panel gets tiled and its
     * parts stretched. */
    root->setSizeConstraint(QLayout::SetFixedSize);

    m_joy = new JoystickWidget(
        session,
        [this](int target) { setMapState(target); },
        [this](const QString &text) { m_hint->setText(text); },
        [this] { stopInDebugger(); });
    root->addWidget(m_joy, 0, Qt::AlignHCenter);
    m_keys = new KeyboardWidget(session);
    root->addWidget(m_keys, 0, Qt::AlignHCenter);

    /* Gamepads: one row per pad, in room for a fixed number so the window
     * does not change size when one is plugged in */
    auto *group = new QGroupBox(QStringLiteral("Gamepads"));
    auto *groupBox = new QVBoxLayout(group);
    m_pads = new PadRows(session, kMaxPadRows);
    groupBox->addWidget(m_pads);
    root->addWidget(group);

    auto *maprow = new QHBoxLayout;
    m_mapButton = new QPushButton(QStringLiteral("Map"));
    m_mapButton->setFocusPolicy(Qt::NoFocus);
    m_mapButton->setFixedWidth(qMax(m_mapButton->sizeHint().width(),
                                    m_mapButton->fontMetrics().horizontalAdvance(QStringLiteral("Done")) + 32));
    m_mapButton->setToolTip(QStringLiteral("Pick a part, then press a key or a gamepad button to give it"));
    connect(m_mapButton, &QPushButton::clicked, this, [this] { setMapState(m_mapState == -2 ? -1 : -2); });
    m_clearButton = new QPushButton(QStringLiteral("Clear"));
    m_clearButton->setFocusPolicy(Qt::NoFocus);
    m_clearButton->setToolTip(QStringLiteral("Unbind the picked part's key and gamepad button"));
    connect(m_clearButton, &QPushButton::clicked, this, [this] {
        if (m_mapState < 0)
            return;
        const int target = m_mapState;
        cocosession_binding_set_key(m_session, target, 0, nullptr, 0);
        cocosession_binding_set_button(m_session, target, COCO_PAD_BTN_NONE, nullptr, 0);
        setMapState(-1);
        m_hint->setText(QStringLiteral("%1 is now unbound").arg(local(coco_target_name(target))));
    });
    auto *defaults = new QPushButton(QStringLiteral("Defaults"));
    defaults->setFocusPolicy(Qt::NoFocus);
    defaults->setToolTip(QStringLiteral("Put every key and gamepad button back to its default"));
    connect(defaults, &QPushButton::clicked, this, [this] {
        cocosession_bindings_reset(m_session);
        m_joy->releaseAll();
        cocosession_keyboard_release(m_session);
        setMapState(m_mapState == -2 ? -2 : -1);
        m_hint->setText(QStringLiteral("Every control is back to its default."));
    });
    maprow->addWidget(m_mapButton);
    maprow->addWidget(m_clearButton);
    maprow->addWidget(defaults);
    maprow->addStretch(1);
    maprow->addWidget(cocoHelpButton(QStringLiteral("controllers")));
    root->addLayout(maprow);

    auto *checkrow = new QHBoxLayout;
    auto *bindings = new QCheckBox(QStringLiteral("Show bindings"));
    bindings->setFocusPolicy(Qt::NoFocus);
    bindings->setToolTip(QStringLiteral("Show the key and gamepad button each part has"));
    /* "controllers_show_bindings"; the older spellings are read when it is absent */
    int saved = cocosession_get_int(session, "controllers_show_bindings", -1);
    if (saved < 0)
        saved = cocosession_get_int(session, "controllers_key_names", -1);
    if (saved < 0)
        saved = cocosession_get_int(session, "controllers_bindings", -1);
    if (saved < 0)
        saved = cocosession_get_int(session, "show_bindings", -1);
    bindings->setChecked(saved > 0);
    connect(bindings, &QCheckBox::toggled, this, [this](bool show) {
        cocosession_set_int(m_session, "controllers_show_bindings", show ? 1 : 0);
        m_joy->setShowBindings(show);
    });
    m_joy->setShowBindings(bindings->isChecked());

    auto *keyboard = new QCheckBox(QStringLiteral("Keyboard"));
    keyboard->setFocusPolicy(Qt::NoFocus);
    keyboard->setToolTip(QStringLiteral("Show the CoCo's keyboard to click"));
    keyboard->setChecked(cocosession_get_int(session, "controllers_keyboard", 0) > 0);
    m_keys->setVisible(keyboard->isChecked());
    connect(keyboard, &QCheckBox::toggled, this, [this](bool show) {
        cocosession_set_int(m_session, "controllers_keyboard", show ? 1 : 0);
        if (!show) {
            m_keys->releaseGrab();
            cocosession_keyboard_release(m_session);
        }
        m_keys->setVisible(show);
    });

    auto *hold = new QCheckBox(QStringLiteral("Hold stick"));
    hold->setFocusPolicy(Qt::NoFocus);
    hold->setToolTip(QStringLiteral("Keep the stick where you leave it, as the original Tandy sticks did"));
    hold->setChecked(cocosession_get_int(session, "joystick_hold", 0) > 0);
    m_joy->setHold(hold->isChecked());
    connect(hold, &QCheckBox::toggled, this, [this](bool active) {
        cocosession_set_int(m_session, "joystick_hold", active ? 1 : 0);
        m_joy->setHold(active);
    });
    checkrow->addWidget(bindings);
    checkrow->addWidget(keyboard);
    checkrow->addWidget(hold);
    checkrow->addStretch(1);
    root->addLayout(checkrow);

    m_hint = new ElidedLabel(true, 3);
    root->addWidget(m_hint);

    connect(&m_captureTimer, &QTimer::timeout, this, [this] { pollCapture(); });
    connect(&m_heldTimer, &QTimer::timeout, this, [this] { pollHeld(); });
    connect(&m_slowTimer, &QTimer::timeout, this, [this] { pollSlow(); });
    setMapState(-2);
    pollHeld();
}

/* The Stop in Debugger plate: pause the machine, then show the debugger. */
void ControllersWindow::stopInDebugger()
{
    cocosession_press(m_session, COCO_TARGET_SYSACT(COCO_SYSACT_DEBUG_STOP), 1);
    cocosession_press(m_session, COCO_TARGET_SYSACT(COCO_SYSACT_DEBUG_STOP), 0);
    DebuggerWindow::show(parentWidget(), m_session);
}

/* ---- Map mode -------------------------------------------------------------------------- */

void ControllersWindow::setMapState(int state)
{
    const bool entering = m_mapState == -2 && state != -2;
    m_mapState = state;
    if (entering) {
        /* a key the host holds would never see its release */
        cocosession_release_all(m_session);
        m_swallow = 0;
    }
    if (state == -2) {
        m_captureTimer.stop();
        cocosession_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Map"));
        m_mapButton->setStyleSheet(QString());
        m_hint->setText(QStringLiteral("Drag a stick; click a fire button (right-click holds button 1, "
                                       "middle-click button 2). Keys typed here go to the CoCo."));
    } else if (state == -1) {
        m_captureTimer.stop();
        cocosession_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Done"));
        m_mapButton->setStyleSheet(QStringLiteral("QPushButton { background: #8BC34A; color: #14210A; }"));
        m_hint->setText(QStringLiteral("Click a stick direction, a fire button, RESET or Stop in Debugger "
                                       "to remap it. Escape leaves Map."));
    } else {
        m_joy->releaseGrab();
        m_hint->setText(QStringLiteral("Press a key or a gamepad button for %1 (Escape cancels). "
                                       "A bound key stops going to the CoCo.")
                            .arg(local(coco_target_name(state))));
        cocosession_gamepad_capture_begin(m_session);
        m_captureTimer.start(50);
    }
    m_clearButton->setEnabled(state >= 0);
    m_joy->setMap(m_mapState);
    m_keys->setInert(m_mapState != -2);
    pollHeld();
}

/* A binding was made: stay armed, since remapping several in a row is normal. */
void ControllersWindow::bound(int target, const QString &what, const char *stolen)
{
    setMapState(-1);
    m_hint->setText(stolen[0] ? QStringLiteral("%1 is now %2 (taken from %3)")
                                    .arg(local(coco_target_name(target)), what, local(stolen))
                              : QStringLiteral("%1 is now %2").arg(local(coco_target_name(target)), what));
}

void ControllersWindow::pollCapture()
{
    int button = COCO_PAD_BTN_NONE;
    if (m_mapState < 0) {
        m_captureTimer.stop();
        return;
    }
    if (!cocosession_gamepad_capture_poll(m_session, &button))
        return;
    char stolen[128] = "";
    const int target = m_mapState;
    cocosession_binding_set_button(m_session, target, button, stolen, sizeof stolen);
    bound(target, local(coco_pad_button_name(button)), stolen);
}

/* ---- polling ---------------------------------------------------------------------------- */

/* What the machine sees held, from every source, lit (25 Hz while visible). */
void ControllersWindow::pollHeld()
{
    m_joy->pollHeld();
    m_keys->pollHeld();
}

/* Hot-plugged pads, auto-assignment changes, a machine switch. */
void ControllersWindow::pollSlow()
{
    if (cocosession_machine(m_session) != m_joy->machine())
        machineChanged();
    m_pads->poll();
}

/* The layouts differ per machine (a CoCo 3 has a second fire button, CTRL,
 * ALT, F1 and F2): fetch them again, letting go of what the old one held. */
void ControllersWindow::machineChanged()
{
    cocosession_keyboard_release(m_session);
    m_joy->load();
    m_keys->load();
    pollHeld();
}

/* ---- the window ------------------------------------------------------------------------- */

void ControllersWindow::present()
{
    if (cocosession_machine(m_session) != m_joy->machine())
        machineChanged();
    m_pads->rebuild();
    show();
    raise();
    activateWindow();
}

void ControllersWindow::refresh()
{
    if (cocosession_machine(m_session) != m_joy->machine())
        machineChanged();
    if (isVisible()) {
        m_joy->releaseAll();
        pollHeld();
    }
}

void ControllersWindow::showEvent(QShowEvent *event)
{
    pollHeld();
    m_heldTimer.start(40);
    m_slowTimer.start(250);
    QWidget::showEvent(event);
}

/* Hiding lets go of everything: the mouse's holds, the held stick, the keys
 * clicked on the keyboard and its latched modifiers. */
void ControllersWindow::hideEvent(QHideEvent *event)
{
    m_heldTimer.stop();
    m_slowTimer.stop();
    setMapState(-2);
    m_joy->releaseAll();
    m_keys->releaseGrab();
    cocosession_keyboard_release(m_session);
    QWidget::hideEvent(event);
}

/* Losing the focus lets go of what the mouse holds and the keys typed here;
 * the held stick and the latched modifiers stay. */
void ControllersWindow::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::ActivationChange && !isActiveWindow()) {
        m_joy->releaseGrab();
        m_keys->releaseGrab();
        cocosession_release_all(m_session);
        m_swallow = 0;
    }
    QWidget::changeEvent(event);
}

/* The CoCo owns the keyboard, so every key but the app's own is claimed here
 * too (menu accelerators must not shadow the machine). */
bool ControllersWindow::event(QEvent *event)
{
    if (cocoClaimShortcut(event))
        return true;
    return QWidget::event(event);
}

void ControllersWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_F12) {
        if (!event->isAutoRepeat())
            DebuggerWindow::show(parentWidget(), m_session);
        return;
    }
    if (cocoIsAppKey(event)) {
        QWidget::keyPressEvent(event);
        return;
    }
    const uint32_t keysym = cocoBindableKey(event);
    if (m_swallow && keysym == m_swallow)
        return;

    if (m_mapState != -2) {
        if (event->isAutoRepeat())
            return;
        /* Esc backs out one step: a pending target, then Map itself */
        if (event->key() == Qt::Key_Escape) {
            m_swallow = keysym;
            setMapState(m_mapState >= 0 ? -1 : -2);
            return;
        }
        if (m_mapState < 0 || !keysym)
            return;                     /* armed, or nothing to bind (a modifier): nothing is typed */
        char stolen[128] = "", name[48] = "";
        const int target = m_mapState;
        cocosession_binding_set_key(m_session, target, keysym, stolen, sizeof stolen);
        cocosession_keysym_name(keysym, name, sizeof name);
        m_swallow = keysym;
        bound(target, local(name), stolen);
        return;
    }
    cocoKeyPress(m_session, event);
}

void ControllersWindow::keyReleaseEvent(QKeyEvent *event)
{
    if (event->isAutoRepeat())
        return;
    const uint32_t keysym = cocoBindableKey(event);
    if (m_swallow && keysym == m_swallow) {
        m_swallow = 0;
        return;
    }
    if (m_mapState != -2) {
        event->accept();
        return;
    }
    if (cocoIsAppKey(event)) {
        QWidget::keyReleaseEvent(event);
        return;
    }
    cocoKeyRelease(m_session, event);
}

QPointer<ControllersWindow> g_window;

} // namespace

void cocoControllersToggle(QWidget *parent, cocosession *session)
{
    if (!g_window)
        g_window = new ControllersWindow(session, parent);
    if (g_window->isVisible())
        g_window->hide();
    else
        g_window->present();
}

void cocoControllersShow(QWidget *parent, cocosession *session)
{
    if (!g_window)
        g_window = new ControllersWindow(session, parent);
    g_window->present();
}

bool cocoControllersVisible()
{
    return g_window && g_window->isVisible();
}

void cocoControllersRefresh()
{
    if (g_window)
        g_window->refresh();
}
