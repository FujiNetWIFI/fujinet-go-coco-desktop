/*
 * CocoControllersWindow -- the AppKit Controllers panel: the CoCo's two
 * joysticks and RESET drawn from the session's layout
 * (cocosession_controls_layout) with NSBezierPath, so every frontend shows
 * the same console; optionally the CoCo's keyboard from its keycap table
 * (cocosession_keyboard_layout); the gamepads and the joystick each drives;
 * and Map mode.
 *
 * Each joystick is the Tandy pattern: a cream base with a round gate, a black
 * stick and red fire buttons (the CoCo 3's with a second). The stick is
 * analog, so the knob is drawn where the machine reads it (the merge of the
 * keyboard, the gamepads and the pointer: cocosession_joystick_held) and
 * a "X 31  Y 31" readout shows the 6-bit values a program gets. One
 * custom-drawn view (CocoConsoleView) paints the joysticks and takes the
 * mouse; another (CocoKeyboardView) paints the keys. The layout, the exact hit
 * shapes and the stick maths are the core's.
 *
 * Gestures: drag on the gate = the analog position (a corner is a full
 * diagonal); a click and hold on a red button holds it, a right-press anywhere
 * on a joystick holds its button 1 and a middle press its button 2; RESET fires
 * when the click ends over it. "Hold stick" keeps the stick where it was left,
 * like the original, non-centring sticks. The pointer is its own input source
 * (cocosession_pointer_set), so the panel lets go of what the mouse holds
 * when it is hidden, rebuilt or loses key status. A key cap acts on the press
 * and the release; SHIFT, CTRL and ALT latch until the next key.
 *
 * What the machine sees held -- host keyboard, gamepads and the mouse -- is
 * painted in the accent, so the panel doubles as an input tester for a new
 * gamepad. Keys typed with the panel key go to the CoCo as in the main window
 * (every key is the CoCo's); no view here takes the keyboard, so the panel
 * itself is the first responder.
 *
 * FIXED SIZE: an NSPanel without a resize mask, sized from its contents. Only
 * the Keyboard checkbox and a machine change resize it; the top edge stays
 * put. Hidden at start, ordered out rather than closed, so its position
 * survives. The 30 Hz poll runs only while the panel can be seen.
 *
 * MAP MODE: Map arms a two-step rebind -- click a control on the picture or
 * choose one from the list (all COCO_TARGET_COUNT targets, the debugger stop
 * among them), then press a key or a gamepad button; the next one is bound
 * and whatever held it is told so in the status line. Esc cancels the pick.
 * Keys are swallowed only while Map is armed or waiting: otherwise they are the
 * CoCo's. Rebinding steals the key from its old target.
 *
 * Written to the family's AppKit pattern; built and verified on CI only (no
 * Apple toolchain here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "ControllersWindow.h"

#import "../KeyForward.h"
#import "../help/HelpWindow.h"

#include <math.h>
#include <string.h>

#define UNIT_PT      34.0   /* points per joystick layout unit (a fire button's width) */
#define KEY_PT       26.0   /* points per keyboard key unit */
#define MARGIN       16.0
#define KBD_MARGIN   14.0
#define HEADER_H     24.0   /* the strip (22) and the accent line under it (2) */
#define MAX_CTL      32     /* room for the core's 21 controls */
#define MAX_CAPS     64     /* room for the core's 57 key caps */
#define TITLE_H      18.0
#define ROW_H        26.0
#define EFF_W        96.0
#define POPUP_W     130.0
#define BTN_H        28.0
#define MAX_PAD_ROWS  3
#define SLOW_TICKS    8     /* the 30 Hz tick's 250 ms part */
#define TRAVEL_UNITS  1.25  /* the knob's travel: the gate's radius less the knob's */
#define KNOB_UNITS    0.55  /* the knob's radius */
#define LIT_MILLI     250   /* a direction sector lights past this deflection */
#define CENTRE_FRAC   0.3   /* a Hold-stick press this near the middle (of the travel) centres */
#define DIM_ALPHA     0.4   /* the controls Map is not asking about */
#define INK_ON_ACCENT 0x14210A
#define LABEL_INK     0x2A2A2D

/* The colours of one machine's case, header and key caps: a CoCo 3 is the
 * silver-grey one with dark grey keys, a CoCo 2 the cream one. */
typedef struct {
    uint32_t caseTop, caseBottom, caseHigh, caseEdge;
    uint32_t strip, stripInk;
    uint32_t keyCap, keySide, keyInk, keyShift, keySpecial, keyFunction;
} CocoPalette;

static const CocoPalette kCoco3Palette = {
    0xB4B5B8, 0x8E8F93, 0xD9DADC, 0x5E5F63, 0x26272A, 0xC9CACC,
    0x2E2F33, 0x1F2023, 0xECECEC, 0x9FA0A5, 0x3E3F45, 0x55565C,
};

static const CocoPalette kCoco2Palette = {
    0xE6E1D3, 0xCFC9B8, 0xF4F1E8, 0x9B9484, 0x3A3A3C, 0xE6E1D3,
    0x3B3A38, 0x262523, 0xF2EFE6, 0xA8A59B, 0x4A4844, 0x4A4844,
};

static const CocoPalette *paletteFor(int machine)
{
    return machine == COCOSESSION_MACHINE_COCO2 ? &kCoco2Palette : &kCoco3Palette;
}

static NSString *nsstr(const char *text)
{
    NSString *res = text ? [NSString stringWithUTF8String:text] : nil;
    return res ? res : @"";
}

static NSString *keyName(uint32_t keysym)
{
    char name[48];
    name[0] = '\0';
    if (keysym) cocosession_keysym_name(keysym, name, (int)sizeof name);
    return nsstr(name);
}

static NSColor *rgb(uint32_t value)
{
    return [NSColor colorWithSRGBRed:((value >> 16) & 0xff) / 255.0
                               green:((value >> 8) & 0xff) / 255.0
                                blue:(value & 0xff) / 255.0
                               alpha:1.0];
}

/* Lines of text centred in rct, the font shrunk (to 6 pt) until they fit. */
static void drawFitted(NSString *text, NSRect rct, CGFloat size, BOOL bold, NSColor *ink)
{
    NSArray<NSString *> *lines = [text componentsSeparatedByString:@"\n"];
    const CGFloat count = (CGFloat)[lines count];
    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.alignment = NSTextAlignmentCenter;
    para.lineBreakMode = NSLineBreakByClipping;
    NSDictionary *attrs = nil;
    CGFloat lineH = 0;
    for (;;) {
        NSFont *font = bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size];
        attrs = @{ NSFontAttributeName: font,
                   NSParagraphStyleAttributeName: para,
                   NSForegroundColorAttributeName: ink };
        CGFloat wide = 0;
        lineH = 0;
        for (NSString *line in lines) {
            const NSSize extent = [line sizeWithAttributes:attrs];
            if (extent.width > wide) wide = extent.width;
            if (extent.height > lineH) lineH = extent.height;
        }
        if ((wide <= rct.size.width && lineH * count <= rct.size.height + 2) || size <= 6) break;
        size -= 0.5;
    }
    CGFloat posy = rct.origin.y + floor((rct.size.height - lineH * count) / 2);
    for (NSString *line in lines) {
        [line drawInRect:NSMakeRect(rct.origin.x, posy, rct.size.width, lineH) withAttributes:attrs];
        posy += lineH;
    }
}

/* A linear gradient from the top of the path's box to its bottom, clipped to
 * the path. The views are flipped, so the top is the smaller y. */
static void fillGradient(NSBezierPath *path, uint32_t topRgb, uint32_t bottomRgb)
{
    const NSRect box = [path bounds];
    NSGradient *grad = [[NSGradient alloc] initWithStartingColor:rgb(topRgb) endingColor:rgb(bottomRgb)];
    [NSGraphicsContext saveGraphicsState];
    [path addClip];
    [grad drawFromPoint:NSMakePoint(NSMidX(box), NSMinY(box))
                toPoint:NSMakePoint(NSMidX(box), NSMaxY(box))
                options:0];
    [NSGraphicsContext restoreGraphicsState];
}

/* The sector of a ring between radii rad0 and rad1, centred on centreDeg and
 * halfDeg either side of it. Angles are mathematical (counter-clockwise as
 * seen, 0 = right); the view is flipped, so y is subtracted. Straight
 * segments, so nothing depends on how AppKit turns arcs in a flipped view. */
static NSBezierPath *ringSector(NSPoint ctr, CGFloat rad0, CGFloat rad1, double centreDeg, double halfDeg)
{
    NSBezierPath *path = [NSBezierPath bezierPath];
    const int steps = 8;
    for (int step = 0; step <= steps; step++) {
        const double ang = (centreDeg - halfDeg + 2.0 * halfDeg * step / steps) * M_PI / 180.0;
        const NSPoint pnt = NSMakePoint(ctr.x + rad1 * cos(ang), ctr.y - rad1 * sin(ang));
        if (step == 0) [path moveToPoint:pnt]; else [path lineToPoint:pnt];
    }
    for (int step = steps; step >= 0; step--) {
        const double ang = (centreDeg - halfDeg + 2.0 * halfDeg * step / steps) * M_PI / 180.0;
        [path lineToPoint:NSMakePoint(ctr.x + rad0 * cos(ang), ctr.y - rad0 * sin(ang))];
    }
    [path closePath];
    return path;
}

/* The direction a sector's target points, in degrees. */
static double sectorAngle(int target, int port)
{
    switch (target - COCO_TARGET_PORT(port, 0)) {
    case COCO_ACT_RIGHT: return 0;
    case COCO_ACT_UP:    return 90;
    case COCO_ACT_LEFT:  return 180;
    default:             return 270;
    }
}

/* ---- views --------------------------------------------------------------------- */

/* Laid out from the top, like the other frontends' panels. */
@interface CocoFlippedView : NSView
@end

@implementation CocoFlippedView
- (BOOL)isFlipped { return YES; }
@end

/* Keys typed with the panel key go to its owner. */
@class CocoControllersWindow;
@interface CocoControllersPanel : NSPanel
@property (nonatomic, weak) CocoControllersWindow *owner;
@end

@interface CocoControllersWindow () <NSWindowDelegate>
- (void)mapPick:(int)target;
- (NSString *)legendForTarget:(int)target;
- (NSString *)bindingText:(int)target;
- (void)panelKeyDown:(NSEvent *)event;
- (void)panelKeyUp:(NSEvent *)event;
@end

/* The joysticks: the case with its header strip, the two Tandy sticks and
 * RESET. Everything on it is drawn here and hit-tested here (by the core). */
@interface CocoConsoleView : CocoFlippedView
@property (nonatomic) cocosession *session;
@property (nonatomic, weak) CocoControllersWindow *owner;
@property (nonatomic, copy) NSString *title;
@property (nonatomic) BOOL mapping;          /* Map armed: bindings shown, nothing lit */
@property (nonatomic) int mapTarget;         /* the control waiting for a key, or -1 */
@property (nonatomic) BOOL showKeys;         /* the bound host key as a small legend */
@property (nonatomic) BOOL holdStick;        /* the stick stays where it is left */
@property (nonatomic, readonly) NSSize contentSize;
- (void)configureForMachine:(int)machine;
/* Reads what the machine sees held and moves the knobs toward it; redraws
 * when anything changed. */
- (void)poll;
/* Lets go of what the pointer holds in a press (a held stick stays). */
- (void)releaseMouse;
/* Lets go of everything the pointer holds, a held stick too. */
- (void)releaseAll;
/* Hold stick was turned off: centre the sticks nobody is dragging. */
- (void)recentreHeld;
@end

@implementation CocoConsoleView {
    int _machine;
    const coco_control *_ctl;
    int _count;
    float _lwide, _lhigh;
    NSSize _size;
    BOOL _lit[MAX_CTL];
    int _defx[COCOSESSION_JOYSTICK_PORTS], _defy[COCOSESSION_JOYSTICK_PORTS];   /* milli-units, merged */
    double _knobx[COCOSESSION_JOYSTICK_PORTS], _knoby[COCOSESSION_JOYSTICK_PORTS];  /* eased, -1..1 */

    /* the mouse */
    int _downIdx;                /* the control pressed, or -1 */
    int _downWhich;              /* 0 left, 1 right, 2 middle */
    int _downPort;               /* the joystick whose stick is dragged, or -1 */
    int _downTarget;             /* the fire button held, or -1 */
    BOOL _stickDrag, _moved, _centrePress, _resetInside;
    NSPoint _downPnt;
    int _ptrx[COCOSESSION_JOYSTICK_PORTS], _ptry[COCOSESSION_JOYSTICK_PORTS];   /* what pointer_set holds */
    NSToolTipTag _tipTag;
}

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (!self) return nil;
    _downIdx = -1;
    _downPort = -1;
    _downTarget = -1;
    _mapTarget = -1;
    _machine = COCOSESSION_MACHINE_COCO3;
    return self;
}

- (BOOL)isOpaque { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { (void)event; return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }
/* a Control-click is the right button, not a context menu */
- (NSMenu *)menuForEvent:(NSEvent *)event { (void)event; return nil; }

- (NSSize)contentSize { return _size; }

- (void)configureForMachine:(int)machine
{
    _machine = machine;
    _ctl = cocosession_controls_layout(machine, &_count, &_lwide, &_lhigh);
    if (!_ctl) _count = 0;
    if (_count > MAX_CTL) _count = MAX_CTL;
    _size = NSMakeSize(ceil(_lwide * UNIT_PT) + 2 * MARGIN, HEADER_H + MARGIN + ceil(_lhigh * UNIT_PT) + MARGIN);
    memset(_lit, 0, sizeof _lit);
    [self setNeedsDisplay:YES];
}

/* The tooltip covers the whole view; the text is chosen by the control under
 * the pointer. */
- (void)resetToolTip
{
    if (_tipTag) [self removeToolTip:_tipTag];
    _tipTag = [self addToolTipRect:[self bounds] owner:self userData:NULL];
}

- (NSString *)view:(NSView *)view stringForToolTip:(NSToolTipTag)tag point:(NSPoint)point userData:(void *)data
{
    (void)view; (void)tag; (void)data;
    const int idx = [self hitIndex:point];
    if (idx < 0) return nil;
    const coco_control *ctl = &_ctl[idx];
    if (ctl->target < 0) return nil;
    return [NSString stringWithFormat:@"%@\n%@", nsstr(coco_target_name(ctl->target)),
            [self.owner bindingText:ctl->target]];
}

/* ---- geometry ------------------------------------------------------------------ */

/* The joysticks are centred in the view, which is wider when the keyboard is. */
- (CGFloat)originX
{
    return floor(([self bounds].size.width - _lwide * UNIT_PT) / 2);
}

- (NSRect)rectOf:(const coco_control *)ctl
{
    return NSMakeRect([self originX] + ctl->left * UNIT_PT, HEADER_H + MARGIN + ctl->top * UNIT_PT,
                      ctl->wide * UNIT_PT, ctl->high * UNIT_PT);
}

- (int)hitIndex:(NSPoint)pnt
{
    const int idx = cocosession_controls_hit(_machine, (float)((pnt.x - [self originX]) / UNIT_PT),
                                             (float)((pnt.y - HEADER_H - MARGIN) / UNIT_PT));
    return idx >= 0 && idx < _count ? idx : -1;
}

/* The gate of a joystick: its centre and its radius, in view points. */
- (BOOL)gateOfPort:(int)port centre:(NSPoint *)centre radius:(CGFloat *)radius
{
    for (int idx = 0; idx < _count; idx++) {
        const coco_control *ctl = &_ctl[idx];
        if (ctl->kind != COCO_CTL_STICK || ctl->port != port) continue;
        const NSRect rct = [self rectOf:ctl];
        *centre = NSMakePoint(NSMidX(rct), NSMidY(rct));
        *radius = rct.size.width / 2;
        return YES;
    }
    return NO;
}

/* ---- the held state --------------------------------------------------------------- */

- (void)poll
{
    BOOL dirty = NO;
    unsigned btns[COCOSESSION_JOYSTICK_PORTS];
    for (int port = 0; port < COCOSESSION_JOYSTICK_PORTS; port++) {
        int posx = 0, posy = 0;
        btns[port] = 0;
        if (!self.mapping) {
            cocosession_joystick_held(_session, port, &posx, &posy);
            btns[port] = cocosession_buttons_held(_session, port);
        }
        if (posx != _defx[port] || posy != _defy[port]) {
            _defx[port] = posx;
            _defy[port] = posy;
            dirty = YES;
        }
        /* the knob eases toward it, snapping when close */
        const double goalx = posx / 1000.0, goaly = posy / 1000.0;
        double nextx = _knobx[port] + (goalx - _knobx[port]) * 0.4;
        double nexty = _knoby[port] + (goaly - _knoby[port]) * 0.4;
        if (fabs(goalx - nextx) < 0.004) nextx = goalx;
        if (fabs(goaly - nexty) < 0.004) nexty = goaly;
        if (nextx != _knobx[port] || nexty != _knoby[port]) {
            _knobx[port] = nextx;
            _knoby[port] = nexty;
            dirty = YES;
        }
    }

    for (int idx = 0; idx < _count; idx++) {
        const coco_control *ctl = &_ctl[idx];
        BOOL lit = NO;
        const int port = ctl->port;
        switch (ctl->kind) {
        case COCO_CTL_BUTTON:
            lit = port >= 0 && port < COCOSESSION_JOYSTICK_PORTS &&
                  (btns[port] & (1u << (ctl->target - COCO_TARGET_PORT(port, COCO_ACT_BUTTON1)))) != 0;
            break;
        case COCO_CTL_RESET:
            lit = !self.mapping && (cocosession_switch_held(_session, COCO_SW_RESET) != 0 ||
                                    (_downIdx == idx && _resetInside));
            break;
        case COCO_CTL_DIRECTION:
            if (port >= 0 && port < COCOSESSION_JOYSTICK_PORTS) {
                switch (ctl->target - COCO_TARGET_PORT(port, 0)) {
                case COCO_ACT_RIGHT: lit = _defx[port] > LIT_MILLI; break;
                case COCO_ACT_LEFT:  lit = _defx[port] < -LIT_MILLI; break;
                case COCO_ACT_UP:    lit = _defy[port] < -LIT_MILLI; break;
                default:             lit = _defy[port] > LIT_MILLI; break;
                }
            }
            break;
        default:
            break;
        }
        if (lit != _lit[idx]) {
            _lit[idx] = lit;
            dirty = YES;
        }
    }
    if (dirty) [self setNeedsDisplay:YES];
}

/* ---- drawing -------------------------------------------------------------------------- */

- (NSString *)legendFor:(const coco_control *)ctl
{
    if (!(self.mapping || self.showKeys) || ctl->target < 0) return nil;
    return [self.owner legendForTarget:ctl->target];
}

- (BOOL)selected:(const coco_control *)ctl
{
    return self.mapping && ctl->target >= 0 && ctl->target == self.mapTarget;
}

/* Map asked about one control: the others are drawn faint. */
- (BOOL)dimmed:(const coco_control *)ctl
{
    return self.mapping && self.mapTarget >= 0 && ctl->target >= 0 && ctl->target != self.mapTarget;
}

- (void)beginDim:(BOOL)dim
{
    if (!dim) return;
    CGContextRef gctx = [[NSGraphicsContext currentContext] CGContext];
    CGContextSaveGState(gctx);
    CGContextSetAlpha(gctx, DIM_ALPHA);
    CGContextBeginTransparencyLayer(gctx, NULL);
}

- (void)endDim:(BOOL)dim
{
    if (!dim) return;
    CGContextRef gctx = [[NSGraphicsContext currentContext] CGContext];
    CGContextEndTransparencyLayer(gctx);
    CGContextRestoreGState(gctx);
}

/* A joystick's base plate: cream, with its shadow. */
- (void)drawBase:(const coco_control *)ctl
{
    const NSRect rct = [self rectOf:ctl];
    const CGFloat rad = 0.5 * UNIT_PT;
    NSBezierPath *shadow = [NSBezierPath bezierPathWithRoundedRect:NSOffsetRect(rct, 0, 3) xRadius:rad yRadius:rad];
    [[NSColor colorWithWhite:0.0 alpha:0.25] setFill];
    [shadow fill];
    NSBezierPath *plate = [NSBezierPath bezierPathWithRoundedRect:rct xRadius:rad yRadius:rad];
    fillGradient(plate, 0xDDD9CF, 0xC2BDB1);
    [rgb(0x8D887C) setStroke];
    plate.lineWidth = 1;
    [plate stroke];
}

/* The gate: a recessed disc with cross-hairs and the four diagonal ticks. */
- (void)drawGate:(const coco_control *)ctl
{
    const NSRect rct = [self rectOf:ctl];
    const NSPoint ctr = NSMakePoint(NSMidX(rct), NSMidY(rct));
    const CGFloat rad = rct.size.width / 2;
    NSBezierPath *ring = [NSBezierPath bezierPathWithOvalInRect:rct];
    [rgb(0x8F8B80) setFill];
    [ring fill];
    NSBezierPath *floorDisc = [NSBezierPath bezierPathWithOvalInRect:NSInsetRect(rct, 4, 4)];
    [rgb(0xA9A598) setFill];
    [floorDisc fill];
    [rgb(0x6C685E) setStroke];
    floorDisc.lineWidth = 1;
    [floorDisc stroke];

    NSBezierPath *marks = [NSBezierPath bezierPath];
    [marks moveToPoint:NSMakePoint(ctr.x - rad + 6, ctr.y)];
    [marks lineToPoint:NSMakePoint(ctr.x + rad - 6, ctr.y)];
    [marks moveToPoint:NSMakePoint(ctr.x, ctr.y - rad + 6)];
    [marks lineToPoint:NSMakePoint(ctr.x, ctr.y + rad - 6)];
    for (int diag = 0; diag < 4; diag++) {
        const double ang = (45.0 + 90.0 * diag) * M_PI / 180.0;
        [marks moveToPoint:NSMakePoint(ctr.x + rad * 0.72 * cos(ang), ctr.y - rad * 0.72 * sin(ang))];
        [marks lineToPoint:NSMakePoint(ctr.x + rad * 0.86 * cos(ang), ctr.y - rad * 0.86 * sin(ang))];
    }
    marks.lineWidth = 1;
    [rgb(0x7A766B) setStroke];
    [marks stroke];
}

/* One of the gate's four sectors: lit while the stick is that way, outlined and
 * carrying its binding in Map mode and with Show bindings. */
- (void)drawSector:(const coco_control *)ctl lit:(BOOL)lit centre:(NSPoint)ctr radius:(CGFloat)rad
{
    NSColor *accent = rgb(COCOSESSION_ACCENT_RGB);
    const BOOL sel = [self selected:ctl];
    const BOOL hot = lit || sel;
    const BOOL hints = self.mapping || self.showKeys;
    const double ang = sectorAngle(ctl->target, ctl->port);
    NSBezierPath *sector = ringSector(ctr, rad * 0.5 + 1.5, rad - 1.5, ang, 43.0);
    if (hot) {
        [accent setFill];
        [sector fill];
        [[NSColor whiteColor] setStroke];
        sector.lineWidth = 2;
        [sector stroke];
    } else if (hints) {
        [rgb(0x7A766B) setStroke];
        sector.lineWidth = 1;
        [sector stroke];
    }
    if (!hints) return;

    NSColor *ink = hot ? rgb(INK_ON_ACCENT) : rgb(0x5D5A52);
    const double rad0 = ang * M_PI / 180.0;
    const NSPoint dir = NSMakePoint(cos(rad0), -sin(rad0));
    const NSPoint side = NSMakePoint(-dir.y, dir.x);
    /* the direction arrow on the inner half, the key on the outer */
    const NSPoint tip = NSMakePoint(ctr.x + dir.x * rad * 0.66, ctr.y + dir.y * rad * 0.66);
    NSBezierPath *arrow = [NSBezierPath bezierPath];
    [arrow moveToPoint:tip];
    [arrow lineToPoint:NSMakePoint(tip.x - dir.x * 7 + side.x * 4.5, tip.y - dir.y * 7 + side.y * 4.5)];
    [arrow lineToPoint:NSMakePoint(tip.x - dir.x * 7 - side.x * 4.5, tip.y - dir.y * 7 - side.y * 4.5)];
    [arrow closePath];
    [ink setFill];
    [arrow fill];
    NSString *key = [self legendFor:ctl];
    if (key.length) {
        /* left and right: pulled in off the rim, a little wider */
        const BOOL sideways = fabs(dir.x) > 0.5;
        const CGFloat mid = rad * (sideways ? 0.66 : 0.84), half = sideways ? 17 : 14;
        drawFitted(key, NSMakeRect(ctr.x + dir.x * mid - half, ctr.y + dir.y * mid - 6, half * 2, 12), 8, NO,
                   hot ? rgb(INK_ON_ACCENT) : rgb(LABEL_INK));
    }
}

/* The stick from above: a shaft to the knob (a shadow, a black cap lit from
 * the upper left), and an accent ring while it is off centre. */
- (void)drawKnobOfPort:(int)port
{
    NSPoint ctr;
    CGFloat rad;
    if (![self gateOfPort:port centre:&ctr radius:&rad]) return;
    const CGFloat travel = TRAVEL_UNITS * UNIT_PT, knobR = KNOB_UNITS * UNIT_PT;
    double offx = _knobx[port] * travel, offy = _knoby[port] * travel;
    /* the range is a square, the gate a circle: a corner is drawn on the rim */
    const double len = hypot(offx, offy);
    if (len > travel) {
        offx *= travel / len;
        offy *= travel / len;
    }
    const NSPoint cap = NSMakePoint(ctr.x + offx, ctr.y + offy);

    NSBezierPath *shaft = [NSBezierPath bezierPath];
    [shaft moveToPoint:ctr];
    [shaft lineToPoint:cap];
    shaft.lineWidth = 6;
    shaft.lineCapStyle = NSLineCapStyleRound;
    [[NSColor colorWithWhite:0.0 alpha:0.35] setStroke];
    [shaft stroke];

    NSBezierPath *shade = [NSBezierPath bezierPathWithOvalInRect:
        NSMakeRect(cap.x - knobR + 2, cap.y - knobR + 4, 2 * knobR, 2 * knobR)];
    [[NSColor colorWithWhite:0.0 alpha:0.28] setFill];
    [shade fill];

    NSBezierPath *body = [NSBezierPath bezierPathWithOvalInRect:
        NSMakeRect(cap.x - knobR, cap.y - knobR, 2 * knobR, 2 * knobR)];
    NSGradient *dome = [[NSGradient alloc] initWithStartingColor:rgb(0x5A5A60) endingColor:rgb(0x1A1A1C)];
    [NSGraphicsContext saveGraphicsState];
    [body addClip];
    [dome drawFromCenter:NSMakePoint(cap.x - knobR * 0.35, cap.y - knobR * 0.35) radius:0
                toCenter:cap radius:knobR * 1.2 options:NSGradientDrawsAfterEndingLocation];
    [NSGraphicsContext restoreGraphicsState];
    [rgb(0x0A0A0B) setStroke];
    body.lineWidth = 1;
    [body stroke];

    if (_defx[port] != 0 || _defy[port] != 0) {
        NSBezierPath *halo = [NSBezierPath bezierPathWithOvalInRect:
            NSMakeRect(cap.x - knobR - 3, cap.y - knobR - 3, 2 * knobR + 6, 2 * knobR + 6)];
        halo.lineWidth = 3;
        [rgb(COCOSESSION_ACCENT_RGB) setStroke];
        [halo stroke];
    }
}

/* A red fire button; its number under it on the plate. */
- (void)drawFire:(const coco_control *)ctl lit:(BOOL)lit
{
    NSColor *accent = rgb(COCOSESSION_ACCENT_RGB);
    const NSRect rct = NSInsetRect([self rectOf:ctl], 1, 1);
    const BOOL active = lit || [self selected:ctl];
    NSBezierPath *lower = [NSBezierPath bezierPathWithOvalInRect:NSOffsetRect(rct, 0, 3)];
    [rgb(0x7D140E) setFill];
    [lower fill];
    NSBezierPath *disc = [NSBezierPath bezierPathWithOvalInRect:rct];
    [(active ? accent : rgb(0xC8281E)) setFill];
    [disc fill];
    if (!active) {
        NSBezierPath *shine = [NSBezierPath bezierPathWithOvalInRect:
            NSMakeRect(rct.origin.x + rct.size.width * 0.2, rct.origin.y + rct.size.height * 0.14,
                       rct.size.width * 0.38, rct.size.height * 0.26)];
        [[rgb(0xE5594C) colorWithAlphaComponent:0.7] setFill];
        [shine fill];
    }
    [(active ? [NSColor whiteColor] : rgb(0x7D140E)) setStroke];
    disc.lineWidth = active ? 2 : 1.5;
    [disc stroke];

    /* the binding sits beside the cap, toward the middle of the plate; never on it */
    NSString *key = [self legendFor:ctl];
    if (key.length) {
        const CGFloat noteW = 1.3 * UNIT_PT;
        const BOOL first = ctl->target == COCO_TARGET_PORT(ctl->port, COCO_ACT_BUTTON1);
        const NSRect note = NSMakeRect(first ? NSMaxX(rct) + 4 : NSMinX(rct) - 4 - noteW, NSMinY(rct), noteW,
                                       rct.size.height);
        drawFitted(key, note, 8, YES, rgb(0x3A3A3C));
    }
    const NSRect below = NSMakeRect(NSMidX(rct) - 10, NSMaxY(rct) + 4, 20, 12);
    drawFitted(nsstr(ctl->label), below, 10, YES, rgb(0x3A3A3C));
}

/* The RESET cap, its name and a red rule under it. */
- (void)drawReset:(const coco_control *)ctl lit:(BOOL)lit
{
    NSColor *accent = rgb(COCOSESSION_ACCENT_RGB);
    const NSRect cap = [self rectOf:ctl];
    const NSRect rct = NSInsetRect(cap, 0.5, 0.5);
    const BOOL active = lit || [self selected:ctl];
    NSBezierPath *side = [NSBezierPath bezierPathWithRoundedRect:NSOffsetRect(rct, 0, 3) xRadius:7 yRadius:7];
    [rgb(0x1F1F21) setFill];
    [side fill];
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:rct xRadius:7 yRadius:7];
    [(active ? accent : rgb(0x3A3A3D)) setFill];
    [path fill];
    [(active ? [NSColor whiteColor] : rgb(0x1F1F21)) setStroke];
    path.lineWidth = active ? 2 : 1;
    [path stroke];

    NSString *key = [self legendFor:ctl];
    if (key.length)
        drawFitted(key, NSInsetRect(cap, 4, 6), 9, NO, active ? rgb(INK_ON_ACCENT) : rgb(0xE0E0E0));
    const NSRect below = NSMakeRect(cap.origin.x - 10, NSMaxY(cap) + 6, cap.size.width + 20, 14);
    drawFitted(nsstr(ctl->label), below, 10, YES, rgb(LABEL_INK));
    const CGFloat rule = 36;
    [rgb(0xD33A2F) setFill];
    NSRectFill(NSMakeRect(NSMidX(cap) - rule / 2, NSMaxY(below) + 1, rule, 2));
}

/* "X 31  Y 31": the 6-bit values a program reads, in a recessed box. */
- (void)drawReadout:(const coco_control *)ctl
{
    const int port = ctl->port;
    if (port < 0 || port >= COCOSESSION_JOYSTICK_PORTS) return;
    const NSRect rct = [self rectOf:ctl];
    NSBezierPath *box = [NSBezierPath bezierPathWithRoundedRect:rct xRadius:4 yRadius:4];
    [rgb(0xB9B5A8) setFill];
    [box fill];
    [rgb(0x8D887C) setStroke];
    box.lineWidth = 1;
    [box stroke];
    NSString *text = [NSString stringWithFormat:@"X %2d  Y %2d", coco_axis_6bit(_defx[port]),
                      coco_axis_6bit(_defy[port])];
    NSDictionary *attrs = @{ NSFontAttributeName: [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightMedium],
                             NSForegroundColorAttributeName: rgb(0x3B3A36) };
    const NSSize extent = [text sizeWithAttributes:attrs];
    [text drawAtPoint:NSMakePoint(NSMidX(rct) - extent.width / 2, NSMidY(rct) - extent.height / 2)
       withAttributes:attrs];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    const NSRect bnd = [self bounds];
    const CocoPalette *pal = paletteFor(_machine);
    NSColor *accent = rgb(COCOSESSION_ACCENT_RGB);

    /* the case, with a highlight under the strip and a dark edge below */
    fillGradient([NSBezierPath bezierPathWithRect:bnd], pal->caseTop, pal->caseBottom);
    [rgb(pal->caseHigh) setFill];
    NSRectFill(NSMakeRect(0, HEADER_H, bnd.size.width, 1.5));
    [rgb(pal->caseEdge) setFill];
    NSRectFill(NSMakeRect(0, bnd.size.height - 1, bnd.size.width, 1));

    /* the strip with the machine's name, and the accent line */
    [rgb(pal->strip) setFill];
    NSRectFill(NSMakeRect(0, 0, bnd.size.width, HEADER_H - 2));
    [accent setFill];
    NSRectFill(NSMakeRect(0, HEADER_H - 2, bnd.size.width, 2));
    NSDictionary *nameplate = @{ NSFontAttributeName: [NSFont boldSystemFontOfSize:11],
                                 NSForegroundColorAttributeName: rgb(pal->stripInk) };
    [self.title drawAtPoint:NSMakePoint(MARGIN, 4) withAttributes:nameplate];

    /* each joystick's name and plate first, so the parts ride over them */
    for (int idx = 0; idx < _count; idx++) {
        const coco_control *ctl = &_ctl[idx];
        if (ctl->kind == COCO_CTL_BASE) [self drawBase:ctl];
        if (ctl->kind == COCO_CTL_CAPTION)
            drawFitted(nsstr(ctl->label).uppercaseString, [self rectOf:ctl], 11, YES, rgb(LABEL_INK));
    }
    for (int idx = 0; idx < _count; idx++) {
        const coco_control *ctl = &_ctl[idx];
        const BOOL dim = [self dimmed:ctl];
        NSPoint ctr;
        CGFloat rad;
        switch (ctl->kind) {
        case COCO_CTL_BUTTON:
            [self beginDim:dim];
            [self drawFire:ctl lit:_lit[idx]];
            [self endDim:dim];
            break;
        case COCO_CTL_RESET:
            [self beginDim:dim];
            [self drawReset:ctl lit:_lit[idx]];
            [self endDim:dim];
            break;
        case COCO_CTL_STICK:
            [self drawGate:ctl];
            break;
        case COCO_CTL_DIRECTION:
            if ([self gateOfPort:ctl->port centre:&ctr radius:&rad]) {
                [self beginDim:dim];
                [self drawSector:ctl lit:_lit[idx] centre:ctr radius:rad];
                [self endDim:dim];
            }
            break;
        case COCO_CTL_READOUT:
            [self drawReadout:ctl];
            break;
        default:
            break;
        }
    }
    for (int port = 0; port < COCOSESSION_JOYSTICK_PORTS; port++) [self drawKnobOfPort:port];

    /* Map mode: the picked control is called out */
    if (self.mapping && self.mapTarget >= 0) {
        for (int idx = 0; idx < _count; idx++) {
            const coco_control *ctl = &_ctl[idx];
            if (ctl->target != self.mapTarget || ctl->kind == COCO_CTL_DIRECTION) continue;
            NSBezierPath *ring = [NSBezierPath bezierPathWithRect:NSInsetRect([self rectOf:ctl], -2, -2)];
            ring.lineWidth = 2;
            [accent setStroke];
            [ring stroke];
        }
    }
}

/* ---- the mouse ---------------------------------------------------------------------- */

- (void)applyPointerOfPort:(int)port
{
    cocosession_pointer_set(_session, port, _ptrx[port], _ptry[port], 0);
}

- (void)setPointerOfPort:(int)port atX:(int)posx atY:(int)posy
{
    if (_ptrx[port] == posx && _ptry[port] == posy) return;
    _ptrx[port] = posx;
    _ptry[port] = posy;
    [self applyPointerOfPort:port];
}

- (void)endHolds
{
    if (_downTarget >= 0) cocosession_press(_session, _downTarget, 0);
    _downIdx = -1;
    _downPort = -1;
    _downTarget = -1;
    _stickDrag = NO;
    _moved = NO;
    _centrePress = NO;
    _resetInside = NO;
}

- (void)releaseMouse
{
    const int port = _downPort;
    const BOOL dragged = _stickDrag;
    [self endHolds];
    if (dragged && port >= 0 && !self.holdStick) [self setPointerOfPort:port atX:0 atY:0];
    [self poll];
}

- (void)releaseAll
{
    [self endHolds];
    for (int port = 0; port < COCOSESSION_JOYSTICK_PORTS; port++) {
        _ptrx[port] = 0;
        _ptry[port] = 0;
        [self applyPointerOfPort:port];
    }
    [self poll];
}

- (void)recentreHeld
{
    for (int port = 0; port < COCOSESSION_JOYSTICK_PORTS; port++)
        if (!(_stickDrag && _downPort == port)) [self setPointerOfPort:port atX:0 atY:0];
    [self poll];
}

/* The stick dragged to a point: the core turns the vector into the analog
 * position (each axis clamped, a corner a full diagonal, a little snapped to
 * the middle). */
- (void)steerTo:(NSPoint)pnt
{
    NSPoint ctr;
    CGFloat rad;
    int posx = 0, posy = 0;
    if (_downPort < 0 || ![self gateOfPort:_downPort centre:&ctr radius:&rad]) return;
    cocosession_stick_from_vector((float)(pnt.x - ctr.x), (float)(pnt.y - ctr.y),
                                  (float)(TRAVEL_UNITS * UNIT_PT), &posx, &posy);
    [self setPointerOfPort:_downPort atX:posx atY:posy];
}

- (void)pressAt:(NSPoint)pnt which:(int)which
{
    if (_downIdx >= 0) return;
    const int idx = [self hitIndex:pnt];
    if (idx < 0) return;
    const coco_control *ctl = &_ctl[idx];

    if (self.mapping) {
        /* Map: a part with a target of its own is picked */
        if (which == 0 && ctl->target >= 0 &&
            (ctl->kind == COCO_CTL_BUTTON || ctl->kind == COCO_CTL_RESET || ctl->kind == COCO_CTL_DIRECTION))
            [self.owner mapPick:ctl->target];
        return;
    }

    if (which != 0) {
        /* a right press anywhere on a joystick holds its button 1, a middle press button 2 */
        if (ctl->port < 0 || ctl->port >= COCOSESSION_JOYSTICK_PORTS) return;
        _downIdx = idx;
        _downWhich = which;
        _downTarget = COCO_TARGET_PORT(ctl->port, which == 1 ? COCO_ACT_BUTTON1 : COCO_ACT_BUTTON2);
        cocosession_press(_session, _downTarget, 1);
        [self poll];
        return;
    }

    _downWhich = 0;
    switch (ctl->kind) {
    case COCO_CTL_BUTTON:
        _downIdx = idx;
        _downTarget = ctl->target;
        cocosession_press(_session, _downTarget, 1);
        break;
    case COCO_CTL_RESET:
        /* fires when the click ends over it */
        _downIdx = idx;
        _resetInside = YES;
        break;
    case COCO_CTL_STICK:
    case COCO_CTL_DIRECTION: {
        NSPoint ctr;
        CGFloat rad;
        if (![self gateOfPort:ctl->port centre:&ctr radius:&rad]) return;
        _downIdx = idx;
        _downPort = ctl->port;
        _stickDrag = YES;
        _moved = NO;
        _downPnt = pnt;
        _centrePress = hypot(pnt.x - ctr.x, pnt.y - ctr.y) < CENTRE_FRAC * TRAVEL_UNITS * UNIT_PT;
        [self steerTo:pnt];
        break;
    }
    default:
        break;
    }
    [self poll];
}

- (void)mouseDown:(NSEvent *)event
{
    const NSPoint pnt = [self convertPoint:[event locationInWindow] fromView:nil];
    [self pressAt:pnt which:([event modifierFlags] & NSEventModifierFlagControl) ? 1 : 0];
}

- (void)rightMouseDown:(NSEvent *)event
{
    [self pressAt:[self convertPoint:[event locationInWindow] fromView:nil] which:1];
}

- (void)otherMouseDown:(NSEvent *)event
{
    [self pressAt:[self convertPoint:[event locationInWindow] fromView:nil] which:2];
}

- (void)dragTo:(NSPoint)pnt
{
    if (_downIdx < 0 || _downWhich != 0) return;
    if (_stickDrag) {
        if (hypot(pnt.x - _downPnt.x, pnt.y - _downPnt.y) > 3.0) _moved = YES;
        [self steerTo:pnt];
        [self poll];
        return;
    }
    /* a fire button or RESET: dragging off lets go */
    const BOOL inside = NSPointInRect(pnt, [self rectOf:&_ctl[_downIdx]]);
    if (_ctl[_downIdx].kind == COCO_CTL_RESET) {
        _resetInside = inside;
        [self poll];
    } else if (!inside) {
        [self releaseMouse];
    }
}

- (void)mouseDragged:(NSEvent *)event
{
    [self dragTo:[self convertPoint:[event locationInWindow] fromView:nil]];
}

- (void)rightMouseDragged:(NSEvent *)event
{
    [self dragTo:[self convertPoint:[event locationInWindow] fromView:nil]];
}

- (void)otherMouseDragged:(NSEvent *)event
{
    [self dragTo:[self convertPoint:[event locationInWindow] fromView:nil]];
}

- (void)endPressWhich:(int)which
{
    if (_downIdx < 0 || _downWhich != which) return;
    if (_stickDrag) {
        /* Hold stick: a press in the middle that never moved centres the stick; any other
         * release leaves it where it is */
        if (self.holdStick && _centrePress && !_moved) [self setPointerOfPort:_downPort atX:0 atY:0];
        [self releaseMouse];
        return;
    }
    if (_ctl[_downIdx].kind == COCO_CTL_RESET) {
        const int target = _ctl[_downIdx].target;
        const BOOL fire = _resetInside;
        [self endHolds];
        if (fire) {
            cocosession_press(_session, target, 1);
            cocosession_press(_session, target, 0);
        }
        [self poll];
        return;
    }
    [self releaseMouse];
}

- (void)mouseUp:(NSEvent *)event
{
    (void)event;
    /* the Control-click that began as a right press ends here */
    if (_downWhich == 1) [self endPressWhich:1];
    else [self endPressWhich:0];
}

- (void)rightMouseUp:(NSEvent *)event { (void)event; [self endPressWhich:1]; }
- (void)otherMouseUp:(NSEvent *)event { (void)event; [self endPressWhich:2]; }
@end

/* The keyboard: the case continued, and the caps of the machine's layout. */
@interface CocoKeyboardView : CocoFlippedView
@property (nonatomic) cocosession *session;
@property (nonatomic, readonly) NSSize contentSize;
- (void)configureForMachine:(int)machine;
- (void)poll;
/* Lets go of what the pointer holds (a latched modifier stays). */
- (void)releaseMouse;
/* Lets go of every cap and latch. */
- (void)releaseAll;
@end

@implementation CocoKeyboardView {
    int _machine;
    const coco_keycap *_caps;
    int _count;
    float _kwide, _khigh;
    NSSize _size;
    int _state[MAX_CAPS];          /* cocosession_keyboard_held: bit 0 held, bit 1 latched */
    int _downCap;
}

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (!self) return nil;
    _downCap = -1;
    _machine = COCOSESSION_MACHINE_COCO3;
    return self;
}

- (BOOL)isOpaque { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { (void)event; return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }
- (NSSize)contentSize { return _size; }

- (void)configureForMachine:(int)machine
{
    _machine = machine;
    _downCap = -1;
    _caps = cocosession_keyboard_layout(machine, &_count, &_kwide, &_khigh);
    if (!_caps) _count = 0;
    if (_count > MAX_CAPS) _count = MAX_CAPS;
    _size = NSMakeSize(ceil(_kwide * KEY_PT) + 2 * KBD_MARGIN, ceil(_khigh * KEY_PT) + 2 * KBD_MARGIN);
    memset(_state, 0, sizeof _state);
    [self setNeedsDisplay:YES];
}

- (CGFloat)originX
{
    return floor(([self bounds].size.width - _kwide * KEY_PT) / 2);
}

- (NSRect)rectOf:(const coco_keycap *)cap
{
    return NSMakeRect([self originX] + cap->left * KEY_PT, KBD_MARGIN + cap->top * KEY_PT,
                      cap->wide * KEY_PT, cap->high * KEY_PT);
}

- (void)poll
{
    BOOL dirty = NO;
    for (int idx = 0; idx < _count; idx++) {
        const int now = cocosession_keyboard_held(_session, idx);
        if (now != _state[idx]) {
            _state[idx] = now;
            dirty = YES;
        }
    }
    if (dirty) [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    const NSRect bnd = [self bounds];
    const CocoPalette *pal = paletteFor(_machine);
    NSColor *accent = rgb(COCOSESSION_ACCENT_RGB);

    fillGradient([NSBezierPath bezierPathWithRect:bnd], pal->caseTop, pal->caseBottom);
    [rgb(pal->caseEdge) setFill];
    NSRectFill(NSMakeRect(0, bnd.size.height - 1, bnd.size.width, 1));

    for (int idx = 0; idx < _count; idx++) {
        const coco_keycap *cap = &_caps[idx];
        const NSRect rct = [self rectOf:cap];
        const BOOL active = _state[idx] != 0;
        uint32_t face = pal->keyCap;
        if (cap->kind == COCO_KEYCAP_SPECIAL || cap->kind == COCO_KEYCAP_MODIFIER) face = pal->keySpecial;
        else if (cap->kind == COCO_KEYCAP_FUNCTION) face = pal->keyFunction;

        NSBezierPath *side = [NSBezierPath bezierPathWithRoundedRect:NSOffsetRect(rct, 0, 2) xRadius:4 yRadius:4];
        [rgb(pal->keySide) setFill];
        [side fill];
        NSBezierPath *top = [NSBezierPath bezierPathWithRoundedRect:rct xRadius:4 yRadius:4];
        [(active ? accent : rgb(face)) setFill];
        [top fill];
        if (active) {
            [[NSColor whiteColor] setStroke];
            top.lineWidth = 2;
            [top stroke];
        }
        NSColor *ink = active ? rgb(INK_ON_ACCENT) : rgb(pal->keyInk);

        if (cap->kind == COCO_KEYCAP_ARROW) {
            drawFitted(nsstr(cap->label), rct, 15, YES, ink);
        } else if (cap->kind == COCO_KEYCAP_SPACE) {
            drawFitted(nsstr(cap->label), rct, 9, NO, ink);
        } else if (cap->shifted) {
            /* the shifted symbol small above, the main legend below it */
            drawFitted(nsstr(cap->shifted), NSMakeRect(rct.origin.x, rct.origin.y + 2, rct.size.width, 10), 8, NO,
                       active ? ink : rgb(pal->keyShift));
            drawFitted(nsstr(cap->label), NSMakeRect(rct.origin.x, rct.origin.y + 11, rct.size.width, rct.size.height - 12),
                       12, YES, ink);
        } else {
            const CGFloat size = strlen(cap->label) > 2 ? 9 : 12;
            drawFitted(nsstr(cap->label), NSInsetRect(rct, 1, 0), size, YES, ink);
        }

        /* a latched modifier keeps its light and shows a dot */
        if (_state[idx] & 2) {
            NSBezierPath *dot = [NSBezierPath bezierPathWithOvalInRect:
                NSMakeRect(rct.origin.x + 4, rct.origin.y + 4, 6, 6)];
            [(active ? rgb(INK_ON_ACCENT) : accent) setFill];
            [dot fill];
        }
    }
}

- (int)capAt:(NSPoint)pnt
{
    for (int idx = _count - 1; idx >= 0; idx--)
        if (NSPointInRect(pnt, [self rectOf:&_caps[idx]])) return idx;
    return -1;
}

- (void)mouseDown:(NSEvent *)event
{
    if (_downCap >= 0) return;
    const int idx = [self capAt:[self convertPoint:[event locationInWindow] fromView:nil]];
    if (idx < 0) return;
    /* the layout is the machine's now: a stale one is rebuilt by the owner */
    if (cocosession_machine(_session) != _machine) return;
    _downCap = idx;
    cocosession_keyboard_press(_session, idx, 1);
    [self poll];
}

- (void)mouseDragged:(NSEvent *)event
{
    if (_downCap < 0) return;
    const NSPoint pnt = [self convertPoint:[event locationInWindow] fromView:nil];
    if (NSPointInRect(pnt, [self rectOf:&_caps[_downCap]])) return;
    /* dragging off lets go (a modifier ignores the release: it latches) */
    cocosession_keyboard_press(_session, _downCap, 0);
    _downCap = -1;
    [self poll];
}

- (void)mouseUp:(NSEvent *)event
{
    (void)event;
    if (_downCap < 0) return;
    cocosession_keyboard_press(_session, _downCap, 0);
    _downCap = -1;
    [self poll];
}

- (void)releaseMouse
{
    if (_downCap >= 0) cocosession_keyboard_press(_session, _downCap, 0);
    _downCap = -1;
    [self poll];
}

- (void)releaseAll
{
    _downCap = -1;
    cocosession_keyboard_release(_session);
    [self poll];
}
@end

@implementation CocoControllersPanel
- (void)keyDown:(NSEvent *)event { [self.owner panelKeyDown:event]; }
- (void)keyUp:(NSEvent *)event { [self.owner panelKeyUp:event]; }
/* Escape cancels a Map pick, or is the CoCo's: not "close the panel". */
- (void)cancelOperation:(id)sender { (void)sender; }
@end

/* ---- the panel ------------------------------------------------------------------- */

@implementation CocoControllersWindow {
    cocosession *_session;
    CocoControllersPanel *_panel;
    CocoFlippedView *_content;
    CocoConsoleView *_console;
    CocoKeyboardView *_keyboard;
    NSTextField *_padsTitle;
    NSTextField *_noPads;
    NSTextField *_padName[MAX_PAD_ROWS];
    NSTextField *_padEff[MAX_PAD_ROWS];     /* each pad's "right joystick" */
    NSPopUpButton *_padAssign[MAX_PAD_ROWS];
    int _padRows;
    unsigned _padGeneration;
    NSButton *_mapButton;
    NSButton *_clearButton;
    NSButton *_defaultsButton;
    NSButton *_showKeysBox;
    NSButton *_keyboardBox;
    NSButton *_holdBox;
    NSTextField *_pickLabel;
    NSPopUpButton *_pickPopup;              /* every target, for Map */
    NSButton *_helpButton;
    NSTextField *_hint;
    NSTimer *_timer;
    int _slow;
    int _builtMachine;
    NSMutableSet<NSNumber *> *_swallowed;   /* key codes Map took: their repeats and release too */
    /* -2 idle, -1 armed and waiting for a control, >= 0 waiting for a key or
     * gamepad button to bind to that target */
    int _mapState;
    BOOL _placed;
}

- (instancetype)initWithSession:(cocosession *)session
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _mapState = -2;
    _builtMachine = -1;
    _padGeneration = (unsigned)-1;
    _swallowed = [NSMutableSet set];

    _panel = [[CocoControllersPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, 440, 640)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskUtilityWindow)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [_panel setTitle:@"Controllers"];
    /* Floats above the machine's window, a full-screen one too. */
    [_panel setLevel:NSFloatingWindowLevel];
    [_panel setCollectionBehavior:NSWindowCollectionBehaviorFullScreenAuxiliary];
    [_panel setReleasedWhenClosed:NO];
    [_panel setBecomesKeyOnlyIfNeeded:NO];
    _panel.owner = self;
    [_panel setDelegate:self];
    CocoSetHelpContext(_panel, @"controllers");

    _content = [[CocoFlippedView alloc] initWithFrame:NSMakeRect(0, 0, 440, 640)];
    [_content setAutoresizesSubviews:NO];
    [_panel setContentView:_content];

    _console = [[CocoConsoleView alloc] initWithFrame:NSZeroRect];
    _console.owner = self;
    _console.session = _session;
    _console.mapTarget = -1;
    {
        /* the older spellings are read when the key is absent */
        int saved = cocosession_get_int(_session, "controllers_show_bindings", -1);
        if (saved < 0) saved = cocosession_get_int(_session, "controllers_key_names", -1);
        if (saved < 0) saved = cocosession_get_int(_session, "controllers_bindings", -1);
        if (saved < 0) saved = cocosession_get_int(_session, "show_bindings", -1);
        _console.showKeys = saved > 0;
    }
    _console.holdStick = cocosession_get_int(_session, "joystick_hold", 0) != 0;
    [_content addSubview:_console];
    _keyboard = [[CocoKeyboardView alloc] initWithFrame:NSZeroRect];
    _keyboard.session = _session;
    [_content addSubview:_keyboard];

    /* Gamepads: one row per pad, its name, the joystick it drives and the one
     * it is set to; rows reserved, so pads coming and going keep the size. */
    _padsTitle = [NSTextField labelWithString:@"Gamepads"];
    [_padsTitle setFont:[NSFont boldSystemFontOfSize:12]];
    [_content addSubview:_padsTitle];
    _noPads = [NSTextField labelWithString:@"No gamepads connected"];
    [_noPads setTextColor:[NSColor secondaryLabelColor]];
    [_content addSubview:_noPads];
    for (int row = 0; row < MAX_PAD_ROWS; row++) {
        _padName[row] = [NSTextField labelWithString:@""];
        [_padName[row] setLineBreakMode:NSLineBreakByTruncatingTail];
        [_content addSubview:_padName[row]];
        _padEff[row] = [NSTextField labelWithString:@""];
        [_padEff[row] setTextColor:[NSColor secondaryLabelColor]];
        [_padEff[row] setAlignment:NSTextAlignmentRight];
        [_content addSubview:_padEff[row]];
        /* -1 = automatic (connection order), 0 = the right joystick, 1 = the left */
        _padAssign[row] = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        [_padAssign[row] addItemsWithTitles:@[@"Automatic", @"Right joystick", @"Left joystick"]];
        _padAssign[row].tag = row;
        _padAssign[row].target = self;
        _padAssign[row].action = @selector(padAssignChanged:);
        [_padAssign[row] setRefusesFirstResponder:YES];
        [_content addSubview:_padAssign[row]];
    }

    _mapButton = [NSButton buttonWithTitle:@"Map" target:self action:@selector(toggleMap:)];
    [_mapButton setRefusesFirstResponder:YES];
    [_content addSubview:_mapButton];
    _clearButton = [NSButton buttonWithTitle:@"Clear" target:self action:@selector(clearBinding:)];
    [_clearButton setRefusesFirstResponder:YES];
    [_content addSubview:_clearButton];
    _defaultsButton = [NSButton buttonWithTitle:@"Defaults" target:self action:@selector(restoreDefaults:)];
    [_defaultsButton setRefusesFirstResponder:YES];
    [_content addSubview:_defaultsButton];
    _pickLabel = [NSTextField labelWithString:@"Control"];
    [_content addSubview:_pickLabel];
    _pickPopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (int target = 0; target < COCO_TARGET_COUNT; target++)
        [[_pickPopup menu] addItemWithTitle:nsstr(coco_target_name(target)) action:nil keyEquivalent:@""];
    _pickPopup.target = self;
    _pickPopup.action = @selector(pickChanged:);
    [_pickPopup setRefusesFirstResponder:YES];
    [_content addSubview:_pickPopup];
    _helpButton = CocoHelpButton(@"controllers");
    [_helpButton setRefusesFirstResponder:YES];
    [_content addSubview:_helpButton];

    _showKeysBox = [NSButton checkboxWithTitle:@"Show bindings" target:self action:@selector(toggleShowKeys:)];
    [_showKeysBox setState:_console.showKeys ? NSControlStateValueOn : NSControlStateValueOff];
    [_showKeysBox setRefusesFirstResponder:YES];
    [_content addSubview:_showKeysBox];
    _keyboardBox = [NSButton checkboxWithTitle:@"Keyboard" target:self action:@selector(toggleKeyboard:)];
    [_keyboardBox setState:cocosession_get_int(_session, "controllers_keyboard", 0) ? NSControlStateValueOn
                                                                                    : NSControlStateValueOff];
    [_keyboardBox setRefusesFirstResponder:YES];
    [_content addSubview:_keyboardBox];
    _holdBox = [NSButton checkboxWithTitle:@"Hold stick" target:self action:@selector(toggleHold:)];
    [_holdBox setState:_console.holdStick ? NSControlStateValueOn : NSControlStateValueOff];
    [_holdBox setRefusesFirstResponder:YES];
    [_content addSubview:_holdBox];

    _hint = [NSTextField wrappingLabelWithString:@""];
    [_hint setFont:[NSFont systemFontOfSize:[NSFont smallSystemFontSize]]];
    [_hint setTextColor:[NSColor secondaryLabelColor]];
    [_hint setMaximumNumberOfLines:2];
    [[_hint cell] setTruncatesLastVisibleLine:YES];
    [_content addSubview:_hint];

    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(occlusionChanged:)
                                                 name:NSWindowDidChangeOcclusionStateNotification
                                               object:_panel];

    [self setMapState:-2];
    [self rebuild];
    return self;
}

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [_timer invalidate];
    [_panel setDelegate:nil];
}

/* ---- the console ------------------------------------------------------------------- */

- (BOOL)keyboardShown
{
    return [_keyboardBox state] == NSControlStateValueOn;
}

- (int)machineNow
{
    const int machine = cocosession_machine(_session);
    return machine == COCOSESSION_MACHINE_COCO2 ? COCOSESSION_MACHINE_COCO2 : COCOSESSION_MACHINE_COCO3;
}

/* A new content size with the top-left corner where it was. */
- (void)fitPanel:(NSSize)size
{
    const NSRect frame = [_panel frame];
    NSRect content = [_panel contentRectForFrameRect:frame];
    if (NSEqualSizes(content.size, size)) return;
    content.size = size;
    NSRect next = [_panel frameRectForContentRect:content];
    next.origin.x = frame.origin.x;
    next.origin.y = NSMaxY(frame) - next.size.height;
    [_panel setFrame:next display:YES];
}

- (void)relayout
{
    const BOOL kbd = [self keyboardShown];
    const NSSize console = _console.contentSize;
    const NSSize keys = _keyboard.contentSize;
    const CGFloat wide = kbd ? MAX(console.width, keys.width) : console.width;
    const CGFloat inner = wide - 2 * MARGIN;
    const CGFloat keysY = console.height;
    const CGFloat padsY = keysY + (kbd ? keys.height : 0) + 10;
    const CGFloat rowsY = padsY + TITLE_H;
    const CGFloat mapY = rowsY + MAX_PAD_ROWS * ROW_H + 8;
    const CGFloat pickY = mapY + BTN_H + 6;
    const CGFloat boxY = pickY + 25 + 6;
    const CGFloat hintY = boxY + 22 + 6;
    const CGFloat total = hintY + 32 + MARGIN;

    [self fitPanel:NSMakeSize(wide, total)];
    [_content setFrame:NSMakeRect(0, 0, wide, total)];
    [_console setFrame:NSMakeRect(0, 0, wide, console.height)];
    [_console resetToolTip];
    [_keyboard setFrame:NSMakeRect(0, keysY, wide, keys.height)];
    [_keyboard setHidden:!kbd];
    [_padsTitle setFrame:NSMakeRect(MARGIN, padsY, 200, 16)];
    [_noPads setFrame:NSMakeRect(MARGIN, rowsY + 4, inner, 18)];
    for (int row = 0; row < MAX_PAD_ROWS; row++) {
        const CGFloat posy = rowsY + row * ROW_H;
        [_padName[row] setFrame:NSMakeRect(MARGIN, posy + 4, inner - POPUP_W - EFF_W - 16, 18)];
        [_padEff[row] setFrame:NSMakeRect(MARGIN + inner - POPUP_W - EFF_W - 8, posy + 4, EFF_W, 18)];
        [_padAssign[row] setFrame:NSMakeRect(MARGIN + inner - POPUP_W, posy, POPUP_W, 25)];
    }
    [_mapButton setFrame:NSMakeRect(MARGIN, mapY, 56, BTN_H)];
    [_clearButton setFrame:NSMakeRect(MARGIN + 62, mapY, 60, BTN_H)];
    [_defaultsButton setFrame:NSMakeRect(MARGIN + 128, mapY, 80, BTN_H)];
    [_pickLabel setFrame:NSMakeRect(MARGIN, pickY + 4, 50, 18)];
    [_pickPopup setFrame:NSMakeRect(MARGIN + 52, pickY, inner - 52, 25)];
    [_helpButton setFrameOrigin:NSMakePoint(wide - MARGIN - _helpButton.frame.size.width, mapY + 2)];
    [_showKeysBox setFrame:NSMakeRect(MARGIN, boxY, 130, 22)];
    [_keyboardBox setFrame:NSMakeRect(MARGIN + 140, boxY, 100, 22)];
    [_holdBox setFrame:NSMakeRect(MARGIN + 250, boxY, 110, 22)];
    [_hint setFrame:NSMakeRect(MARGIN, hintY, inner, 32)];
    [_console setNeedsDisplay:YES];
    [_keyboard setNeedsDisplay:YES];
}

- (void)rebuild
{
    [self releaseEverything];
    _builtMachine = [self machineNow];
    [_console configureForMachine:_builtMachine];
    [_keyboard configureForMachine:_builtMachine];
    _console.title = _builtMachine == COCOSESSION_MACHINE_COCO2 ? @"COLOR COMPUTER 2" : @"COLOR COMPUTER 3";
    [self relayout];
    [self refreshLabels];
}

/* Lets go of everything this panel holds: the mouse's holds, a held stick, the
 * caps and the latches. */
- (void)releaseEverything
{
    [_console releaseAll];
    [_keyboard releaseAll];
}

/* ---- Map mode --------------------------------------------------------------------- */

- (NSString *)legendForTarget:(int)target
{
    return keyName(cocosession_binding_get(_session, target).keysym);
}

/* What is bound to a target, for the status line and the tooltip. */
- (NSString *)bindingText:(int)target
{
    const coco_binding bnd = cocosession_binding_get(_session, target);
    NSString *key = keyName(bnd.keysym);
    NSString *pad = bnd.button != COCO_PAD_BTN_NONE ? nsstr(coco_pad_button_name(bnd.button)) : @"none";
    return [NSString stringWithFormat:@"Key: %@  Gamepad: %@", key.length ? key : @"none", pad];
}

/* In Map mode nothing is lit and every control shows what is bound to it. */
- (void)refreshLabels
{
    _console.mapping = _mapState != -2;
    _console.mapTarget = _mapState >= 0 ? _mapState : -1;
    [_console setNeedsDisplay:YES];
    [_console poll];
}

- (void)setMapState:(int)state
{
    _mapState = state;
    if (state >= 0) {
        [self releaseEverything];
        cocosession_gamepad_capture_begin(_session);
        _hint.stringValue = [NSString stringWithFormat:@"Press a key or a gamepad button for %@ (now: %@). Esc cancels.",
                             nsstr(coco_target_name(state)), [self bindingText:state]];
    } else {
        cocosession_gamepad_capture_cancel(_session);
        _hint.stringValue = state == -1
            ? @"Click a control, or choose one from the list, to remap it."
            : @"Drag a stick, click and hold the red buttons (right-click is button 1, middle-click button 2). "
              @"Keys typed here go to the CoCo.";
    }
    [_mapButton setTitle:(state == -2 ? @"Map" : @"Done")];
    _mapButton.bezelColor = state == -2 ? nil : rgb(COCOSESSION_ACCENT_RGB);
    _pickPopup.enabled = state != -2;
    _clearButton.enabled = state >= 0;
    if (state >= 0 && state < [_pickPopup numberOfItems]) [_pickPopup selectItemAtIndex:state];
    if (state == -2) [_swallowed removeAllObjects];
    [self refreshLabels];
}

/* A control clicked on the picture in Map mode. */
- (void)mapPick:(int)target
{
    if (_mapState != -2) [self setMapState:target];
}

- (void)pickChanged:(NSPopUpButton *)sender
{
    if (_mapState == -2) return;
    const NSInteger sel = sender.indexOfSelectedItem;
    if (sel >= 0 && sel < COCO_TARGET_COUNT) [self setMapState:(int)sel];
}

- (void)showBound:(NSString *)name target:(int)target stolen:(const char *)stolen
{
    NSString *what = nsstr(coco_target_name(target));
    _hint.stringValue = stolen[0]
        ? [NSString stringWithFormat:@"%@ is now %@ (it was %@)", what, name, nsstr(stolen)]
        : [NSString stringWithFormat:@"%@ is now %@", what, name];
}

- (void)bindKey:(uint32_t)keysym
{
    char stolen[128];
    stolen[0] = '\0';
    const int target = _mapState;
    cocosession_binding_set_key(_session, target, keysym, stolen, (int)sizeof stolen);
    [self setMapState:-1];   /* stay armed: remapping several in a row is normal */
    [self showBound:keyName(keysym) target:target stolen:stolen];
}

- (void)pollCapture
{
    int button = COCO_PAD_BTN_NONE;
    char stolen[128];
    if (_mapState < 0 || !cocosession_gamepad_capture_poll(_session, &button)) return;
    stolen[0] = '\0';
    const int target = _mapState;
    cocosession_binding_set_button(_session, target, button, stolen, (int)sizeof stolen);
    [self setMapState:-1];
    [self showBound:nsstr(coco_pad_button_name(button)) target:target stolen:stolen];
}

- (void)toggleMap:(id)sender
{
    (void)sender;
    [self setMapState:(_mapState == -2 ? -1 : -2)];
}

/* Unbinds the picked target's key and its pad button. */
- (void)clearBinding:(id)sender
{
    (void)sender;
    if (_mapState < 0) return;
    const int target = _mapState;
    cocosession_binding_set_key(_session, target, 0, NULL, 0);
    cocosession_binding_set_button(_session, target, COCO_PAD_BTN_NONE, NULL, 0);
    [self setMapState:-1];
    _hint.stringValue = [NSString stringWithFormat:@"%@ is now unbound", nsstr(coco_target_name(target))];
}

- (void)restoreDefaults:(id)sender
{
    (void)sender;
    cocosession_bindings_reset(_session);
    [self releaseEverything];
    [self refreshLabels];
    _hint.stringValue = @"Default bindings restored";
}

- (void)toggleShowKeys:(id)sender
{
    (void)sender;
    const BOOL active = [_showKeysBox state] == NSControlStateValueOn;
    cocosession_set_int(_session, "controllers_show_bindings", active ? 1 : 0);
    _console.showKeys = active;
    [_console setNeedsDisplay:YES];
}

- (void)toggleKeyboard:(id)sender
{
    (void)sender;
    cocosession_set_int(_session, "controllers_keyboard", [self keyboardShown] ? 1 : 0);
    if (![self keyboardShown]) [_keyboard releaseAll];
    [self relayout];
}

- (void)toggleHold:(id)sender
{
    (void)sender;
    const BOOL active = [_holdBox state] == NSControlStateValueOn;
    cocosession_set_int(_session, "joystick_hold", active ? 1 : 0);
    _console.holdStick = active;
    if (!active) [_console recentreHeld];
}

/* ---- gamepads ----------------------------------------------------------------------- */

- (void)refreshPadPorts
{
    for (int row = 0; row < _padRows; row++) {
        const int port = cocosession_gamepad_effective_port(_session, row);
        NSString *text = port == 0 ? @"right joystick" : (port == 1 ? @"left joystick" : @"nothing");
        if (![_padEff[row].stringValue isEqualToString:text]) _padEff[row].stringValue = text;
    }
}

- (void)refreshPads
{
    const int count = cocosession_gamepad_count(_session);
    _padGeneration = cocosession_gamepad_generation(_session);
    _padRows = count < MAX_PAD_ROWS ? (count > 0 ? count : 0) : MAX_PAD_ROWS;
    _noPads.hidden = count > 0;
    for (int row = 0; row < MAX_PAD_ROWS; row++) {
        const BOOL shown = row < _padRows;
        _padName[row].hidden = !shown;
        _padEff[row].hidden = !shown;
        _padAssign[row].hidden = !shown;
        if (!shown) continue;
        char name[128];
        name[0] = '\0';
        cocosession_gamepad_name(_session, row, name, (int)sizeof name);
        _padName[row].stringValue = nsstr(name);
        _padName[row].toolTip = nsstr(name);
        const int assign = cocosession_gamepad_assignment(_session, row);
        [_padAssign[row] selectItemAtIndex:(assign < 0 || assign > 1 ? 0 : assign + 1)];
    }
    [self refreshPadPorts];
}

- (void)padAssignChanged:(NSPopUpButton *)sender
{
    const int sel = (int)sender.indexOfSelectedItem;
    cocosession_gamepad_assign(_session, (int)sender.tag, sel <= 0 ? -1 : sel - 1);
    [self refreshPadPorts];
}

/* ---- the poll ----------------------------------------------------------------------- */

/* 30 Hz while the panel can be seen: what is held and the gamepad capture;
 * every 250 ms the hot-plugged pads, auto-assignment and a machine change in
 * the Machine menu. */
- (void)tick
{
    if (_mapState >= 0) [self pollCapture];
    if (++_slow >= SLOW_TICKS) {
        _slow = 0;
        if (cocosession_gamepad_generation(_session) != _padGeneration) [self refreshPads];
        else [self refreshPadPorts];
        if ([self machineNow] != _builtMachine) [self rebuild];
    }
    [_console poll];
    if ([self keyboardShown]) [_keyboard poll];
}

/* The timer runs only while the panel is on screen and not occluded or
 * hidden with its app. */
- (void)updateTimer
{
    const BOOL want = [_panel isVisible] && ([_panel occlusionState] & NSWindowOcclusionStateVisible);
    if (want && !_timer) {
        __weak CocoControllersWindow *weakSelf = self;
        _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30 repeats:YES block:^(NSTimer *timer) {
            (void)timer;
            [weakSelf tick];
        }];
        _timer.tolerance = 0.005;
    } else if (!want && _timer) {
        [_timer invalidate];
        _timer = nil;
    }
}

- (void)occlusionChanged:(NSNotification *)note
{
    (void)note;
    [self updateTimer];
}

/* ---- the keyboard ------------------------------------------------------------------- */

/* Keys typed with the panel key. Outside Map they are the CoCo's, exactly
 * as in the main window; while Map is armed or waiting they are taken (and
 * not typed). */
- (void)panelKeyDown:(NSEvent *)event
{
    if ([event modifierFlags] & NSEventModifierFlagCommand) return;
    if (_mapState == -2) {
        CocoForwardKey(_session, event, 1);
        return;
    }
    [_swallowed addObject:@([event keyCode])];
    if ([event isARepeat] || _mapState < 0) return;     /* armed: typing does nothing */
    uint32_t keysym = 0, unicode = 0;
    if (!CocoKeyFromEvent(event, &keysym, &unicode)) return;
    if (keysym == COCO_KEYSYM_ESCAPE) {
        [self setMapState:-1];
        return;
    }
    const uint32_t key = cocosession_bindable_key(keysym, unicode);
    if (key) [self bindKey:key];
    else _hint.stringValue = @"That key cannot be bound. Press another, or Esc to cancel.";
}

- (void)panelKeyUp:(NSEvent *)event
{
    NSNumber *code = @([event keyCode]);
    if ([_swallowed containsObject:code]) {
        [_swallowed removeObject:code];
        return;
    }
    CocoForwardKey(_session, event, 0);
}

/* ---- the window --------------------------------------------------------------------- */

/* Hidden: Map disarmed, nothing held (a latch and a held stick too), no polling. */
- (void)quiesce
{
    [self setMapState:-2];
    [self releaseEverything];
    [_timer invalidate];
    _timer = nil;
}

- (void)show
{
    if ([self machineNow] != _builtMachine) [self rebuild];
    [self refreshPads];
    [self setMapState:-2];
    if (!_placed) {
        [_panel center];
        _placed = YES;
    }
    [_panel makeFirstResponder:nil];    /* the panel itself takes the keys */
    [_panel makeKeyAndOrderFront:nil];
    [self updateTimer];
}

- (void)hide
{
    [_panel orderOut:nil];
    [self quiesce];
}

- (void)toggle
{
    if ([self isVisible]) [self hide];
    else [self show];
}

- (BOOL)isVisible
{
    return [_panel isVisible];
}

- (void)refresh
{
    if ([self isVisible]) {
        [self rebuild];
        [self refreshPads];
    } else {
        [self releaseEverything];
        _builtMachine = -1;     /* rebuilt when next shown */
    }
}

/* Losing key status: only what this panel's mouse holds (a held stick and a
 * latched modifier stay); the main window releases its own keys. */
- (void)windowDidResignKey:(NSNotification *)note
{
    (void)note;
    [_console releaseMouse];
    [_keyboard releaseMouse];
    [_swallowed removeAllObjects];    /* their releases go elsewhere */
    /* keys typed here are held host keys; the key-up goes to another app.
     * Latches and the stick stay as the user left them. */
    cocosession_release_all(_session);
}

- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
    [self quiesce];
}
@end
