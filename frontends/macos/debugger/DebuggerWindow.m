/*
 * Debugger window (AppKit): disassembly with click-to-toggle breakpoints,
 * registers, memory view and the decoded video pane, over the shared
 * cocodebug engine -- the same content as the GNOME/KDE/Windows debuggers,
 * built from plain AppKit views.
 *
 * Ported from fujinet-go-adam-desktop's DebuggerWindow.m, not copied
 * line-for-line -- cocodebug.h's shape differs from adamdebug.h in ways
 * that matter here (see frontends/windows/debugger/dbg_window.c's file
 * comment for the full account, which applies here unchanged):
 *
 *   * No stop callback. adamdebug fires one on the emulator thread,
 *     marshaled to the main queue with dispatch_async. cocodebug is
 *     poll-only (cocodebug_stop_serial()), because XRoar's instruction-
 *     hook context makes a cross-thread callback into a toolkit unsafe --
 *     so this polls on the existing 0.1s NSTimer instead, checking both
 *     the stop serial and the paused state each tick.
 *   * No register or memory *editing*: cocodebug_regs_get has no _set,
 *     cocodebug_read has no _write. Registers and memory are display-only
 *     here too.
 *   * No VDP, no instruction trace -- neither exists for a CoCo. In their
 *     place: one decoded screen image (cocodebug_video_decode) and Load
 *     Symbols…/Clear Symbols buttons (cocodebug_symbols_load/_clear --
 *     ADAM has no frontend affordance for this, its tables autoload). No
 *     NSTabView at all: with VDP and Trace gone there is nothing left to
 *     justify one, so this is a single flattened CPU+Video layout instead
 *     of ADAM's three-tab one.
 *   * coco_dasm_line carries no embedded symbol pointer; a match is a
 *     separate cocodebug_symbol_at() call per line, appended as a
 *     "   ; NAME" suffix (matching GNOME/Windows) rather than a label
 *     line of its own.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DebuggerWindow.h"

#include "cocodebug.h"
#include "cocovideo.h"

#define DISASM_LINES 40
#define MEM_ROWS 16

static DebuggerWindow *g_debugger;

/* Disassembly text view: a click toggles the breakpoint on the clicked
 * line. The address sits at columns 3-6 of each row (refreshDisasm's
 * "%c%c %04X  ..." format -- two flag columns, a space, then the hex
 * address), matching GNOME's and the Windows frontend's rendering. */
@interface DasmTextView : NSTextView
@property(nonatomic, copy) void (^onToggleAddr)(uint16_t addr);
@end

@implementation DasmTextView
- (void)mouseDown:(NSEvent *)event
{
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSUInteger idx = [self characterIndexForInsertionAtPoint:p];
    NSString *text = self.string;
    if (idx > text.length)
        idx = text.length;
    NSUInteger start = [text lineRangeForRange:NSMakeRange(idx, 0)].location;
    NSString *line = [text substringFromIndex:start];
    if (line.length >= 7) {
        unsigned addr = 0;
        NSScanner *scan =
            [NSScanner scannerWithString:[line substringWithRange:
                                                   NSMakeRange(3, 4)]];
        if ([scan scanHexInt:&addr] && addr <= 0xFFFF && self.onToggleAddr)
            self.onToggleAddr((uint16_t)addr);
    }
}
@end

@interface DebuggerWindow () <NSWindowDelegate>
- (instancetype)initWithSession:(cocosession *)session;
- (void)refreshDisasm;
@end

@implementation DebuggerWindow {
    cocosession *_session;
    cocodebug *_dbg;
    NSWindow *_window;

    NSButton *_runBtn;
    NSTextField *_status;
    DasmTextView *_disasm;

    NSTextView *_regs;
    NSImageView *_video;
    int _videoW, _videoH;
    uint32_t *_videoBuf; /* COCOVIDEO_MAX_WIDTH * MAX_HEIGHT, XRGB8888 */

    NSTextField *_memAddr;
    NSTextView *_memView;
    uint16_t _memBase;

    NSTimer *_tick;
    uint64_t _seenSerial;
    /* Plain int, not BOOL: seeded to -1 so the first tick always refreshes
     * regardless of whether the machine happens to have already reached
     * "paused" by then -- a BOOL here would collapse -1 to a truthy value
     * that could coincide with a real paused==1 reading and skip it. */
    int _wasPaused;
}

static NSImage *imageFromXRGB(const uint32_t *px, int w, int h)
{
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider = CGDataProviderCreateWithData(
        NULL, px, (size_t)w * (size_t)h * 4, NULL);
    CGImageRef cg = CGImageCreate(
        (size_t)w, (size_t)h, 8, 32, (size_t)w * 4, space,
        kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, provider,
        NULL, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(space);
    NSImage *img = [[NSImage alloc] initWithCGImage:cg
                                               size:NSMakeSize(w, h)];
    CGImageRelease(cg);
    return img;
}

+ (void)showForSession:(cocosession *)session
{
    if (!g_debugger) {
        g_debugger = [[DebuggerWindow alloc] initWithSession:session];
    } else {
        /* Reengage: closing hides rather than destroys (see
         * windowShouldClose:), and disengages on the way out. */
        cocosession_debugger(session);
        cocodebug_set_engaged(g_debugger->_dbg, 1);
        cocodebug_pause(g_debugger->_dbg);
    }
    [g_debugger->_window makeKeyAndOrderFront:nil];
}

- (instancetype)initWithSession:(cocosession *)session
{
    self = [super init];
    if (!self)
        return nil;
    _session = session;
    _dbg = cocosession_debugger(session);
    _memBase = 0;
    _videoBuf = (uint32_t *)calloc((size_t)COCOVIDEO_MAX_WIDTH *
                                   COCOVIDEO_MAX_HEIGHT, sizeof(uint32_t));
    [self buildWindow];

    /* Engaging switches the emulator loop to the path that honours
     * breakpoints. Pausing immediately is what a user opening a debugger
     * expects -- otherwise the window opens showing nothing at all. */
    cocodebug_set_engaged(_dbg, 1);
    cocodebug_pause(_dbg);

    __weak DebuggerWindow *weakSelf = self;
    _tick = [NSTimer scheduledTimerWithTimeInterval:0.1
                                            repeats:YES
                                              block:^(NSTimer *t) {
                                                (void)t;
                                                [weakSelf onTick];
                                              }];
    _wasPaused = -1; /* force the first tick to refresh */
    return self;
}

- (void)onTick
{
    uint64_t serial = cocodebug_stop_serial(_dbg);
    BOOL paused = cocodebug_is_paused(_dbg);
    if (serial != _seenSerial || paused != _wasPaused) {
        _seenSerial = serial;
        _wasPaused = paused;
        [self refreshAll:paused];
    }
}

/* ---- construction helpers ---------------------------------------------------- */

static NSTextView *monoView(NSScrollView **scrollOut)
{
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    NSTextView *view =
        [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
    view.editable = NO;
    view.richText = NO;
    view.font = [NSFont monospacedSystemFontOfSize:11
                                            weight:NSFontWeightRegular];
    view.autoresizingMask = NSViewWidthSizable;
    scroll.documentView = view;
    *scrollOut = scroll;
    return view;
}

- (NSButton *)button:(NSString *)title action:(SEL)sel
{
    return [NSButton buttonWithTitle:title target:self action:sel];
}

- (NSTextField *)label:(NSString *)text
{
    return [NSTextField labelWithString:text];
}

- (void)buildWindow
{
    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 1180, 780)
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable |
                            NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"Debugger";
    _window.releasedWhenClosed = NO;
    _window.delegate = self;

    /* Toolbar row */
    _runBtn = [self button:@"Pause (F5)" action:@selector(togglePause:)];
    NSButton *stepBtn = [self button:@"Step (F7)" action:@selector(step:)];
    NSButton *stepOverBtn = [self button:@"Step Over (F8)"
                                  action:@selector(stepOver:)];
    NSButton *stepOutBtn = [self button:@"Step Out (⇧F8)"
                                 action:@selector(stepOut:)];
    NSButton *clearBpBtn = [self button:@"Clear Breakpoints"
                                 action:@selector(clearBreakpoints:)];
    NSButton *loadSymBtn = [self button:@"Load Symbols…"
                                 action:@selector(loadSymbols:)];
    NSButton *clearSymBtn = [self button:@"Clear Symbols"
                                  action:@selector(clearSymbols:)];
    _runBtn.keyEquivalent =
        [NSString stringWithFormat:@"%C", (unichar)NSF5FunctionKey];
    stepBtn.keyEquivalent =
        [NSString stringWithFormat:@"%C", (unichar)NSF7FunctionKey];
    stepOverBtn.keyEquivalent =
        [NSString stringWithFormat:@"%C", (unichar)NSF8FunctionKey];

    _status = [self label:@"Running"];
    _status.alignment = NSTextAlignmentRight;

    NSStackView *toolbar = [NSStackView stackViewWithViews:@[
        _runBtn, stepBtn, stepOverBtn, stepOutBtn, clearBpBtn, loadSymBtn,
        clearSymBtn, _status
    ]];
    toolbar.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    toolbar.spacing = 8;

    /* Disassembly on the left, registers + video + memory stacked on the
     * right, in one split view -- no tabs. */
    NSScrollView *dasmScroll = [[NSScrollView alloc] init];
    dasmScroll.hasVerticalScroller = YES;
    _disasm = [[DasmTextView alloc] initWithFrame:NSMakeRect(0, 0, 560, 600)];
    _disasm.editable = NO;
    _disasm.richText = NO;
    _disasm.font = [NSFont monospacedSystemFontOfSize:11
                                               weight:NSFontWeightRegular];
    _disasm.autoresizingMask = NSViewWidthSizable;
    __weak DebuggerWindow *weakSelf = self;
    _disasm.onToggleAddr = ^(uint16_t addr) {
        DebuggerWindow *s = weakSelf;
        if (!s)
            return;
        cocodebug_breakpoint_toggle(s->_dbg, addr);
        [s refreshDisasm];
    };
    dasmScroll.documentView = _disasm;

    NSScrollView *regsScroll;
    _regs = monoView(&regsScroll);
    [regsScroll.heightAnchor constraintEqualToConstant:130].active = YES;

    _video = [[NSImageView alloc] init];
    _video.imageScaling = NSImageScaleProportionallyUpOrDown;
    _video.wantsLayer = YES;
    _video.layer.magnificationFilter = kCAFilterNearest;
    [_video.heightAnchor constraintEqualToConstant:240].active = YES;

    _memAddr = [[NSTextField alloc] init];
    _memAddr.placeholderString = @"Memory addr / symbol";
    _memAddr.target = self;
    _memAddr.action = @selector(gotoMem:);
    NSButton *memGo = [self button:@"Go" action:@selector(gotoMem:)];
    NSStackView *memRow = [NSStackView stackViewWithViews:@[
        _memAddr, memGo
    ]];
    memRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    NSScrollView *memScroll;
    _memView = monoView(&memScroll);

    NSStackView *side = [NSStackView stackViewWithViews:@[
        [self label:@"Registers"], regsScroll, [self label:@"Video"], _video,
        [self label:@"Memory"], memRow, memScroll
    ]];
    side.orientation = NSUserInterfaceLayoutOrientationVertical;
    side.alignment = NSLayoutAttributeLeading;
    side.spacing = 6;

    NSSplitView *split = [[NSSplitView alloc] init];
    split.vertical = YES;
    [split addArrangedSubview:dasmScroll];
    [split addArrangedSubview:side];

    NSStackView *root = [NSStackView stackViewWithViews:@[ toolbar, split ]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [toolbar.widthAnchor constraintEqualToAnchor:root.widthAnchor
                                        constant:-16].active = YES;
    [split.widthAnchor constraintEqualToAnchor:root.widthAnchor
                                      constant:-16].active = YES;
    [split.heightAnchor constraintGreaterThanOrEqualToConstant:600].active =
        YES;

    _window.contentView = root;
    [_window center];
}

/* ---- helpers ------------------------------------------------------------------ */

- (BOOL)parseAddr:(NSString *)text into:(uint16_t *)out
{
    NSString *t = [text stringByTrimmingCharactersInSet:
                            [NSCharacterSet whitespaceCharacterSet]];
    if ([t hasPrefix:@"$"])
        t = [t substringFromIndex:1];
    unsigned v = 0;
    NSScanner *scan = [NSScanner scannerWithString:t];
    if ([scan scanHexInt:&v] && scan.atEnd && v <= 0xFFFF) {
        *out = (uint16_t)v;
        return YES;
    }
    uint16_t addr;
    if (cocodebug_symbol_find(_dbg, text.UTF8String, &addr)) {
        *out = addr;
        return YES;
    }
    return NO;
}

/* ---- actions -------------------------------------------------------------------- */

- (void)togglePause:(id)sender
{
    (void)sender;
    if (cocodebug_is_paused(_dbg))
        cocodebug_resume(_dbg);
    else
        cocodebug_pause(_dbg);
}

- (void)step:(id)sender
{
    (void)sender;
    cocodebug_step(_dbg);
}

- (void)stepOver:(id)sender
{
    (void)sender;
    cocodebug_step_over(_dbg);
}

- (void)stepOut:(id)sender
{
    (void)sender;
    cocodebug_step_out(_dbg);
}

- (void)clearBreakpoints:(id)sender
{
    (void)sender;
    cocodebug_breakpoint_clear_all(_dbg);
    if (cocodebug_is_paused(_dbg))
        [self refreshDisasm];
}

- (void)loadSymbols:(id)sender
{
    (void)sender;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.title = @"Load Symbols";
    /* No allowedContentTypes: LWASM's --symbol-dump has no fixed
     * convention, and leaving the property unset (rather than an empty
     * array, which would allow nothing) is what shows every file. */
    if ([panel runModal] != NSModalResponseOK || !panel.URL)
        return;
    if (cocodebug_symbols_load(_dbg, panel.URL.path.UTF8String) < 0)
        _status.stringValue = @"Could not open symbol file";
    if (cocodebug_is_paused(_dbg))
        [self refreshDisasm];
}

- (void)clearSymbols:(id)sender
{
    (void)sender;
    cocodebug_symbols_clear(_dbg);
    if (cocodebug_is_paused(_dbg))
        [self refreshDisasm];
}

- (void)gotoMem:(id)sender
{
    (void)sender;
    uint16_t addr;
    if ([self parseAddr:_memAddr.stringValue into:&addr]) {
        _memBase = addr;
        if (cocodebug_is_paused(_dbg))
            [self refreshMem];
    }
}

/* windowShouldClose: fires whenever the window's close control (or Window >
 * Close) is used; keep the window alive but hidden, like GNOME/Windows do,
 * so F12 reopens it instantly with its state intact -- but disengage RIGHT
 * HERE rather than relying on -dealloc/deinit-style cleanup, which for this
 * hide-not-destroy shape would only ever run at process exit. Leaving the
 * debugger engaged after every close would otherwise run the emulator on
 * the slow instruction-hooked path forever with the window merely hidden. */
- (BOOL)windowShouldClose:(NSWindow *)sender
{
    (void)sender;
    cocodebug_set_engaged(_dbg, 0);
    [_window orderOut:nil];
    return NO;
}

/* ---- refreshers ------------------------------------------------------------------ */

- (void)refreshAll:(BOOL)paused
{
    _runBtn.title = paused ? @"Run (F5)" : @"Pause (F5)";
    _status.stringValue = paused ? @"Paused" : @"Running";
    if (paused) {
        [self refreshDisasm];
        [self refreshRegs];
        [self refreshMem];
        [self refreshVideo];
    } else {
        /* Everything on show is only valid while the machine is parked, so
         * clear it rather than leave a stale snapshot that looks live. */
        _disasm.string = @"";
        _regs.string = @"";
        _memView.string = @"";
        _video.image = nil;
    }
}

- (void)refreshDisasm
{
    coco_dasm_line lines[DISASM_LINES];
    cocodebug_regs r;
    if (!cocodebug_regs_get(_dbg, &r))
        return;

    int n = cocodebug_disassemble(_dbg, r.pc, lines, DISASM_LINES);
    NSMutableString *text = [NSMutableString string];
    for (int i = 0; i < n; i++) {
        char bytes[16] = "";
        char symname[32];
        for (int b = 0; b < lines[i].len; b++)
            snprintf(bytes + b * 2, 3, "%02X", lines[i].bytes[b]);
        [text appendFormat:@"%c%c %04X  %-10s %s",
                           cocodebug_breakpoint_is_set(_dbg, lines[i].addr)
                               ? '*' : ' ',
                           lines[i].addr == r.pc ? '>' : ' ',
                           lines[i].addr, bytes, lines[i].text];
        if (cocodebug_symbol_at(_dbg, lines[i].addr, symname, sizeof(symname)))
            [text appendFormat:@"   ; %s", symname];
        [text appendString:@"\n"];
    }
    _disasm.string = text;
}

- (void)refreshRegs
{
    cocodebug_regs r;
    if (!cocodebug_regs_get(_dbg, &r))
        return;
    static const char names[] = "EFHINZVC";
    char bits[9];
    for (int i = 0; i < 8; i++)
        bits[i] = (r.cc & (0x80 >> i)) ? names[i] : '-';
    bits[8] = '\0';
    _regs.string = [NSString
        stringWithFormat:@"PC  %04X      A %02X  B %02X\n"
                         @"X   %04X      DP %02X\n"
                         @"Y   %04X\n"
                         @"U   %04X\n"
                         @"S   %04X\n"
                         @"\n"
                         @"CC  %02X  %s",
                         r.pc, r.a, r.b, r.x, r.dp, r.y, r.u, r.s, r.cc,
                         bits];
}

- (void)refreshMem
{
    uint8_t data[MEM_ROWS * 16];
    int n = cocodebug_read(_dbg, _memBase, data, sizeof(data));
    if (n <= 0) {
        _memView.string = @"";
        return;
    }
    NSMutableString *text = [NSMutableString string];
    for (int row = 0; row * 16 < n; row++) {
        [text appendFormat:@"%04X ", (unsigned)(_memBase + row * 16)];
        for (int col = 0; col < 16; col++) {
            int idx = row * 16 + col;
            if (idx < n)
                [text appendFormat:@" %02X", data[idx]];
            else
                [text appendString:@"   "];
        }
        [text appendString:@"  "];
        for (int col = 0; col < 16; col++) {
            int idx = row * 16 + col;
            uint8_t c = (idx < n) ? data[idx] : (uint8_t)' ';
            [text appendFormat:@"%c",
                               (c >= 0x20 && c < 0x7F) ? (char)c : '.'];
        }
        [text appendString:@"\n"];
    }
    _memView.string = text;
}

- (void)refreshVideo
{
    int w = 0, h = 0;
    if (cocodebug_video_decode(_dbg, _videoBuf, &w, &h) && w > 0 && h > 0) {
        _videoW = w;
        _videoH = h;
        _video.image = imageFromXRGB(_videoBuf, w, h);
    } else {
        _video.image = nil;
    }
}

@end
