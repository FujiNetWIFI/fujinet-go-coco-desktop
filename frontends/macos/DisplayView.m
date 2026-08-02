/*
 * DisplayView: paints the emulator's latest frame with CoreGraphics,
 * letterboxed to the chosen aspect. A CVDisplayLink provides the vsync
 * ticks that feed the session's phase-lock (the macOS analog of the GTK
 * frame clock / Qt frameSwapped loop) and schedules main-thread repaints.
 *
 * Ported from fujinet-go-adam-desktop's DisplayView.m, not copied
 * line-for-line:
 *   * cocosession_copy_frame reports a DYNAMIC frame size (up to
 *     COCOSESSION_FB_MAX_WIDTH x MAX_HEIGHT, the video mode can change at
 *     runtime), not ADAM's compile-time-constant 256x212 -- the scratch
 *     buffer is sized at the maximum once and _fbW/_fbH track the
 *     *current* frame, read fresh in destRect/drawRect rather than cached.
 *   * The core's XRGB8888 is already 0x00RRGGBB = bytes B,G,R,X on a
 *     little-endian host (same conclusion GNOME's own code comment draws
 *     for its GDK format), so unlike ADAM's RGB565 source there is no
 *     manual pixel-conversion loop here -- CGImageCreate points straight
 *     at the session's own buffer, with kCGImageAlphaNoneSkipFirst |
 *     kCGBitmapByteOrder32Little (BGRX-in-memory) instead of ADAM's
 *     alpha-last RGBA flags.
 *   * Keyboard forwards raw keysym/unicode/down per keystroke via
 *     cocosession_key -- the session does the CoCo-key translation, unlike
 *     adamsession_key's single pre-translated byte -- so keyUp: forwards a
 *     real release instead of only clearing ADAM's Ctrl+digit keypad
 *     state, which has no CoCo equivalent and is dropped entirely.
 *     NSEvent.charactersIgnoringModifiers already gives a layout-aware
 *     unshifted character, so the function-key-to-X11-keysym table below
 *     is otherwise unchanged from ADAM's.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DisplayView.h"

#import <CoreVideo/CoreVideo.h>

#include <time.h>

@implementation DisplayView {
    cocosession *_session;
    CVDisplayLinkRef _link;
    uint32_t *_buf; /* COCOSESSION_FB_MAX_WIDTH * MAX_HEIGHT, XRGB8888 */
    int _fbW, _fbH; /* the CURRENT frame's size; changes at runtime */
    uint64_t _serial;
    CGImageRef _image;
    int _aspectMode;
    BOOL _smooth;
}

static int64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static CVReturn link_cb(CVDisplayLinkRef link, const CVTimeStamp *now,
                        const CVTimeStamp *output, CVOptionFlags flagsIn,
                        CVOptionFlags *flagsOut, void *ctx)
{
    DisplayView *view = (__bridge DisplayView *)ctx;
    (void)link; (void)now; (void)output; (void)flagsIn; (void)flagsOut;
    [view onVsync];
    return kCVReturnSuccess;
}

- (instancetype)initWithSession:(cocosession *)session
{
    self = [super initWithFrame:NSMakeRect(0, 0, 800, 600)];
    if (!self)
        return nil;
    _session = session;
    _buf = calloc((size_t)COCOSESSION_FB_MAX_WIDTH * COCOSESSION_FB_MAX_HEIGHT,
                 sizeof(uint32_t));
    return self;
}

- (void)dealloc
{
    [self stop];
    if (_image)
        CGImageRelease(_image);
    free(_buf);
}

/* CVDisplayLink is deprecated in macOS 15 in favor of NSView.displayLink
 * (macOS 14+), but the retro community runs plenty of older Intel Macs
 * that never see macOS 14 -- so the older API stays, deliberately. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
- (void)start
{
    if (_link)
        return;
    CVDisplayLinkCreateWithActiveCGDisplays(&_link);
    CVDisplayLinkSetOutputCallback(_link, link_cb, (__bridge void *)self);
    CVDisplayLinkStart(_link);
}

- (void)stop
{
    if (_link) {
        CVDisplayLinkStop(_link);
        CVDisplayLinkRelease(_link);
        _link = NULL;
    }
}
#pragma clang diagnostic pop

- (void)setAspectMode:(int)mode
{
    _aspectMode = mode;
    [self setNeedsDisplay:YES];
}

- (void)setSmooth:(BOOL)smooth
{
    _smooth = smooth;
    [self setNeedsDisplay:YES];
}

/* Display-link thread: feed the phase-lock, then hop to the main thread to
 * pull and paint whatever frame is latest. */
- (void)onVsync
{
    cocosession_notify_vsync(_session, monotonic_ns());
    dispatch_async(dispatch_get_main_queue(), ^{
        [self pullFrame];
    });
}

- (void)pullFrame
{
    int w = 0, h = 0;
    if (!cocosession_copy_frame(_session, _buf, &_serial, &w, &h) ||
        w <= 0 || h <= 0)
        return;
    _fbW = w;
    _fbH = h;

    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider = CGDataProviderCreateWithData(
        NULL, _buf, (size_t)w * (size_t)h * 4, NULL);
    CGImageRef image = CGImageCreate(
        (size_t)w, (size_t)h, 8, 32, (size_t)w * 4, space,
        kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, provider,
        NULL, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(space);

    if (_image)
        CGImageRelease(_image);
    _image = image;
    [self setNeedsDisplay:YES];
}

- (NSRect)destRect
{
    const CGFloat w = self.bounds.size.width;
    const CGFloat h = self.bounds.size.height;
    CGFloat dw, dh;

    if (_fbW <= 0 || _fbH <= 0)
        return NSZeroRect;

    if (_aspectMode == 2) { /* integer scale */
        int scale = (int)MIN(w / _fbW, h / _fbH);
        if (scale < 1)
            scale = 1;
        dw = scale * _fbW;
        dh = scale * _fbH;
    } else {
        const CGFloat aspect =
            _aspectMode == 1 ? 4.0 / 3.0 : (CGFloat)_fbW / _fbH;
        if (w / h > aspect) {
            dh = h;
            dw = h * aspect;
        } else {
            dw = w;
            dh = w / aspect;
        }
    }
    return NSMakeRect((w - dw) / 2.0, (h - dh) / 2.0, dw, dh);
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    [[NSColor blackColor] setFill];
    NSRectFill(self.bounds);
    if (!_image)
        return;

    CGContextRef ctx = NSGraphicsContext.currentContext.CGContext;
    CGContextSetInterpolationQuality(
        ctx, _smooth ? kCGInterpolationLow : kCGInterpolationNone);
    CGContextDrawImage(ctx, NSRectToCGRect([self destRect]), _image);
}

/* ---- input -------------------------------------------------------------------
 * cocosession_key wants an X11/xkb keysym (or 0, meaning "look at unicode
 * instead" -- see core/src/input_map.c's coco_key_from_event, which is
 * what every frontend's translation ultimately feeds) plus the raw
 * unicode character, down/up state and modifier flags -- the session does
 * the CoCo-key translation itself, unlike ADAM's frontend-side
 * adam_key_from_event. NSEvent.charactersIgnoringModifiers already gives a
 * layout-aware *unshifted* character, so this table (itself already X11
 * keysym values, e.g. 0xFF52 = XK_Up) needs no change from ADAM's. */

- (BOOL)acceptsFirstResponder
{
    return YES;
}

static uint32_t keysym_for_char(unichar c)
{
    switch (c) {
    case NSUpArrowFunctionKey:    return 0xFF52;
    case NSDownArrowFunctionKey:  return 0xFF54;
    case NSLeftArrowFunctionKey:  return 0xFF51;
    case NSRightArrowFunctionKey: return 0xFF53;
    case NSHomeFunctionKey:       return 0xFF50;
    case NSEndFunctionKey:        return 0xFF57;
    case NSPageUpFunctionKey:     return 0xFF55;
    case NSPageDownFunctionKey:   return 0xFF56;
    case NSInsertFunctionKey:     return 0xFF63;
    case NSDeleteFunctionKey:     return 0xFFFF; /* forward delete */
    case 0x7F:                    return 0xFF08; /* backspace */
    case 0x0D: case 0x03:         return 0xFF0D; /* return / enter */
    case 0x09:                    return 0xFF09; /* tab */
    case 0x1B:                    return 0xFF1B; /* escape */
    default:
        if (c >= NSF1FunctionKey && c <= NSF1FunctionKey + 5)
            return 0xFFBE + (uint32_t)(c - NSF1FunctionKey);
        return 0;
    }
}

- (void)forwardKeyEvent:(NSEvent *)event down:(int)down
{
    NSString *chars = event.charactersIgnoringModifiers;
    unichar c = chars.length ? [chars characterAtIndex:0] : 0;
    const int ctrl = (event.modifierFlags & NSEventModifierFlagControl) != 0;
    const int shift = (event.modifierFlags & NSEventModifierFlagShift) != 0;
    uint32_t keysym = keysym_for_char(c);
    uint32_t unicode = (c >= 0x20 && c < 0xF700) ? c : 0;

    if (!keysym && !unicode)
        return; /* not a key the emulated keyboard has */
    cocosession_key(_session, down, keysym, unicode, ctrl, shift);
}

- (void)keyDown:(NSEvent *)event
{
    [self forwardKeyEvent:event down:1];
}

- (void)keyUp:(NSEvent *)event
{
    [self forwardKeyEvent:event down:0];
}

@end
