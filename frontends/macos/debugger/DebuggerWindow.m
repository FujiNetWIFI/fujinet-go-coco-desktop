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
 *     ADAM has no frontend affordance for this, its tables autoload). With
 *     VDP and Trace gone, CPU+Video is one flattened page rather than
 *     ADAM's three tabs; the NSTabView it sits in is there for the FujiNet
 *     tab alone (the Becker port's DriveWire traffic, refreshed twice a
 *     second while it shows).
 *   * coco_dasm_line carries no embedded symbol pointer; a match is a
 *     separate cocodebug_symbol_at() call per line, appended as a
 *     "   ; NAME" suffix (matching GNOME/Windows) rather than a label
 *     line of its own.
 *
 * C source-level stepping (added after the port above, for CMOC-built
 * programs): a source pane over cocodebug_source_*, a Locals pane over
 * cocodebug_locals_get, and Step Line/Step Over Line -- the same additions
 * GNOME/KDE/Windows all got. See core/debugger/csrcinfo.h for what CMOC
 * emits and how it maps back to source lines.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DebuggerWindow.h"

#include <ctype.h>
#include <stdlib.h>

#include "cocodebug.h"
#include "cocovideo.h"

#define DISASM_LINES 40
#define MEM_ROWS 16
#define LOCALS_MAX 32

/* The tabs, in order: the names COCO_DEBUGGER_TAB takes. */
enum { TAB_CPU, TAB_FUJINET, TAB_COUNT };
static const char *const kTabNames[TAB_COUNT] = { "cpu", "fujinet" };
static NSString *const kTabLabels[TAB_COUNT] = { @"CPU", @"FujiNet" };

/* Entries the FujiNet tab asks for: all the engine keeps, plus the one in
 * progress. */
#define FUJI_TRACE_MAX 513

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

/* Source text view: same click-to-toggle idea as DasmTextView, but simpler
 * -- there is no address to parse out of the row, because every row is
 * exactly one line of the source file (populateSrcList: prints every line,
 * filtering none out), so the 0-based line count of newlines before the
 * click is the line number directly. */
@interface SrcTextView : NSTextView
@property(nonatomic, copy) void (^onToggleLine)(int lineno);
@end

@implementation SrcTextView
- (void)mouseDown:(NSEvent *)event
{
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSUInteger idx = [self characterIndexForInsertionAtPoint:p];
    NSString *text = self.string;
    if (idx > text.length)
        idx = text.length;
    NSUInteger start = [text lineRangeForRange:NSMakeRange(idx, 0)].location;
    NSString *prefix = [text substringToIndex:start];
    NSUInteger lineIndex = [[prefix componentsSeparatedByString:@"\n"] count] - 1;
    if (self.onToggleLine)
        self.onToggleLine((int)lineIndex + 1);
}
@end

@interface DebuggerWindow () <NSWindowDelegate, NSTabViewDelegate>
- (instancetype)initWithSession:(cocosession *)session;
- (void)refreshDisasm;
- (void)populateSrcList:(int)fileId;
- (void)refreshFujinet:(BOOL)force;
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

    NSButton *_stepLineBtn;
    NSButton *_stepOverLineBtn;
    NSPopUpButton *_srcFileCombo;
    SrcTextView *_srcView;
    NSTextView *_locals;
    /* The source pane's own cache: which file's text is currently held in
     * _srcCachedLines, so a stop that doesn't change the displayed file
     * doesn't re-read it from disk. */
    NSString *_srcCachedPath;
    NSArray<NSString *> *_srcCachedLines;
    int _srcDisplayedFileId;
    /* Where PC actually is, independent of which file the combo is showing
     * (a user may be browsing a different file to set a breakpoint ahead of
     * time): the '>' marker only appears when these match the displayed
     * file. */
    int _pcFileId, _pcLine;

    NSTabView *_tabs;
    NSTextField *_fujiNote;
    NSButton *_fujiHideReady;
    NSTextView *_fujiText;
    NSTextView *_fujiState;
    int _fujiCount;
    uint64_t _fujiLast;
    int _fujiTicks;

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
    _pcFileId = -1;
    _srcDisplayedFileId = -1;
    _fujiCount = -1;
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
    } else if ([self currentTab] == TAB_FUJINET && ++_fujiTicks >= 5) {
        /* the traffic twice a second, whether or not the machine stops */
        [self refreshFujinet:NO];
    }
}

- (int)currentTab
{
    return (int)[_tabs indexOfTabViewItem:_tabs.selectedTabViewItem];
}

- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    (void)tabView;
    (void)item;
    if ([self currentTab] == TAB_FUJINET)
        [self refreshFujinet:NO];
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

- (NSTextField *)heading:(NSString *)text
{
    NSTextField *l = [NSTextField labelWithString:text];
    l.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize]];
    return l;
}

/* A column whose scroll view takes the height left. */
static NSStackView *column(NSArray<NSView *> *views, NSScrollView *scroll)
{
    NSStackView *s = [NSStackView stackViewWithViews:views];
    s.orientation = NSUserInterfaceLayoutOrientationVertical;
    s.alignment = NSLayoutAttributeLeading;
    s.distribution = NSStackViewDistributionFill;
    s.spacing = 6;
    [scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow - 1
                       forOrientation:NSLayoutConstraintOrientationVertical];
    for (NSView *v in views)
        [v.widthAnchor constraintEqualToAnchor:s.widthAnchor].active = YES;
    return s;
}

/* COCO_DEBUGGER_TAB: a tab's number or name ("cpu", "fujinet"), or the
 * start of one, any case. */
static int tabFromEnv(void)
{
    const char *tab = getenv("COCO_DEBUGGER_TAB");
    char *end;
    if (!tab || !*tab)
        return -1;
    long n = strtol(tab, &end, 10);
    if (end != tab && *end == '\0')
        return (n >= 0 && n < TAB_COUNT) ? (int)n : -1;
    for (int i = 0; i < TAB_COUNT; i++) {
        size_t k = 0;
        while (tab[k] && tolower((unsigned char)tab[k]) == kTabNames[i][k])
            k++;
        if (!tab[k])
            return i;
    }
    return -1;
}

/* Engine text as an NSString; never nil, so it can always be appended. */
static NSString *textFrom(const char *s)
{
    NSString *t = [NSString stringWithUTF8String:s];
    return t ? t : [NSString stringWithCString:s
                                      encoding:NSISOLatin1StringEncoding];
}

/* Whether a view's reader is at its end (a log then follows new lines). */
static BOOL atEnd(NSTextView *view)
{
    NSScrollView *scroll = view.enclosingScrollView;
    if (!scroll)
        return YES;
    return NSMaxY(scroll.contentView.documentVisibleRect) >=
           NSHeight(view.frame) - 4;
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
static void setText(NSTextView *view, NSString *text, BOOL follow)
{
    if (![view.string isEqualToString:text]) {
        NSScrollView *scroll = view.enclosingScrollView;
        const NSPoint at =
            scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
        view.string = text;
        if (scroll && !follow) {
            [scroll.contentView scrollToPoint:at];
            [scroll reflectScrolledClipView:scroll.contentView];
        }
    }
    if (follow)
        [view scrollRangeToVisible:NSMakeRange(view.string.length, 0)];
}

- (NSView *)buildFujinet
{
    _fujiNote = [self label:@""];
    _fujiNote.textColor = NSColor.secondaryLabelColor;
    [_fujiNote setContentHuggingPriority:1
                          forOrientation:NSLayoutConstraintOrientationHorizontal];
    _fujiHideReady = [NSButton checkboxWithTitle:@"Hide READY probes"
                                          target:self
                                          action:@selector(fujiHideReady:)];
    _fujiHideReady.state = NSControlStateValueOn;
    _fujiHideReady.toolTip = @"fujinet-lib asks E2 00 before every call";
    NSStackView *row = [NSStackView stackViewWithViews:@[
        _fujiNote, _fujiHideReady,
        [self button:@"Clear" action:@selector(fujiClear:)]
    ]];
    row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    row.distribution = NSStackViewDistributionFill;
    row.spacing = 8;

    NSScrollView *fujiScroll, *stateScroll;
    _fujiText = monoView(&fujiScroll);
    _fujiState = monoView(&stateScroll);
    NSStackView *left = column(
        @[ [self heading:@"DriveWire packets"], row, fujiScroll ], fujiScroll);
    NSStackView *right =
        column(@[ [self heading:@"Becker port"], stateScroll ], stateScroll);
    [left.widthAnchor constraintGreaterThanOrEqualToConstant:520].active = YES;
    [right.widthAnchor constraintEqualToConstant:340].active = YES;

    NSStackView *split = [NSStackView stackViewWithViews:@[ left, right ]];
    split.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    split.distribution = NSStackViewDistributionFill;
    split.alignment = NSLayoutAttributeTop;
    split.spacing = 12;
    [left setContentHuggingPriority:NSLayoutPriorityDefaultLow - 1
                     forOrientation:NSLayoutConstraintOrientationHorizontal];
    [left.heightAnchor constraintEqualToAnchor:split.heightAnchor].active = YES;
    [right.heightAnchor constraintEqualToAnchor:split.heightAnchor].active = YES;
    return split;
}

/* A tab's content, inset from the tab view's edges. */
static NSView *tabPage(NSView *content)
{
    NSView *holder = [[NSView alloc] init];
    [holder addSubview:content];
    content.translatesAutoresizingMaskIntoConstraints = NO;
    [content.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor
                                          constant:6].active = YES;
    [content.trailingAnchor constraintEqualToAnchor:holder.trailingAnchor
                                           constant:-6].active = YES;
    [content.topAnchor constraintEqualToAnchor:holder.topAnchor
                                      constant:6].active = YES;
    [content.bottomAnchor constraintEqualToAnchor:holder.bottomAnchor
                                         constant:-6].active = YES;
    return holder;
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
    _stepLineBtn = [self button:@"Step Line (F6)" action:@selector(stepLine:)];
    _stepOverLineBtn = [self button:@"Step Over Line (⇧F6)"
                             action:@selector(stepOverLine:)];
    NSButton *clearBpBtn = [self button:@"Clear Breakpoints"
                                 action:@selector(clearBreakpoints:)];
    NSButton *loadSymBtn = [self button:@"Load Symbols…"
                                 action:@selector(loadSymbols:)];
    NSButton *clearSymBtn = [self button:@"Clear Symbols"
                                  action:@selector(clearSymbols:)];
    NSButton *loadSrcBtn = [self button:@"Load C Source…"
                                 action:@selector(loadSource:)];
    NSButton *clearSrcBtn = [self button:@"Clear C Source"
                                  action:@selector(clearSource:)];
    _runBtn.keyEquivalent =
        [NSString stringWithFormat:@"%C", (unichar)NSF5FunctionKey];
    stepBtn.keyEquivalent =
        [NSString stringWithFormat:@"%C", (unichar)NSF7FunctionKey];
    stepOverBtn.keyEquivalent =
        [NSString stringWithFormat:@"%C", (unichar)NSF8FunctionKey];
    /* Plain F6, the same way stepBtn/stepOverBtn wire an unshifted F-key.
     * Shift+F6 has no equivalent here for the same reason Shift+F8 above
     * (stepOutBtn) does not either: AppKit's keyEquivalent alone does not
     * distinguish the shifted case, and this file has never wired
     * keyEquivalentModifierMask to do it -- the title is a hint, the click
     * is what actually works, same as Step Out today. */
    _stepLineBtn.keyEquivalent =
        [NSString stringWithFormat:@"%C", (unichar)NSF6FunctionKey];

    _status = [self label:@"Running"];
    _status.alignment = NSTextAlignmentRight;

    NSStackView *toolbar = [NSStackView stackViewWithViews:@[
        _runBtn, stepBtn, stepOverBtn, stepOutBtn, _stepLineBtn,
        _stepOverLineBtn, clearBpBtn, loadSymBtn, clearSymBtn, loadSrcBtn,
        clearSrcBtn, _status
    ]];
    toolbar.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    toolbar.spacing = 8;

    /* Disassembly, C source, and registers+locals+video+memory, in one
     * split view: the CPU tab. */
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

    /* C source column: a file picker over cocodebug_source_file_path()'s
     * table (selected index and file_id are the same number: rebuildSrcCombo
     * appends items in exactly the id order the engine hands them out) above
     * the listing itself. */
    _srcFileCombo = [[NSPopUpButton alloc] initWithFrame:NSZeroRect
                                                pullsDown:NO];
    _srcFileCombo.target = self;
    _srcFileCombo.action = @selector(srcFileChanged:);
    _srcFileCombo.enabled = NO;

    NSScrollView *srcScroll = [[NSScrollView alloc] init];
    srcScroll.hasVerticalScroller = YES;
    _srcView = [[SrcTextView alloc] initWithFrame:NSMakeRect(0, 0, 400, 560)];
    _srcView.editable = NO;
    _srcView.richText = NO;
    _srcView.font = [NSFont monospacedSystemFontOfSize:11
                                                weight:NSFontWeightRegular];
    _srcView.autoresizingMask = NSViewWidthSizable;
    _srcView.onToggleLine = ^(int lineno) {
        DebuggerWindow *s = weakSelf;
        if (!s)
            return;
        cocodebug_source_breakpoint_toggle(s->_dbg, s->_srcDisplayedFileId,
                                           lineno);
        [s populateSrcList:s->_srcDisplayedFileId];
    };
    srcScroll.documentView = _srcView;

    NSStackView *srcCol = [NSStackView stackViewWithViews:@[
        _srcFileCombo, srcScroll
    ]];
    srcCol.orientation = NSUserInterfaceLayoutOrientationVertical;
    srcCol.alignment = NSLayoutAttributeLeading;
    srcCol.spacing = 4;

    NSScrollView *regsScroll;
    _regs = monoView(&regsScroll);
    [regsScroll.heightAnchor constraintEqualToConstant:130].active = YES;

    NSScrollView *localsScroll;
    _locals = monoView(&localsScroll);
    [localsScroll.heightAnchor constraintEqualToConstant:110].active = YES;

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
        [self label:@"Registers"], regsScroll, [self label:@"Locals"],
        localsScroll, [self label:@"Video"], _video, [self label:@"Memory"],
        memRow, memScroll
    ]];
    side.orientation = NSUserInterfaceLayoutOrientationVertical;
    side.alignment = NSLayoutAttributeLeading;
    side.spacing = 6;

    NSSplitView *split = [[NSSplitView alloc] init];
    split.vertical = YES;
    [split addArrangedSubview:dasmScroll];
    [split addArrangedSubview:srcCol];
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

    NSView *pages[TAB_COUNT];
    pages[TAB_CPU] = root;
    pages[TAB_FUJINET] = [self buildFujinet];
    _tabs = [[NSTabView alloc] init];
    for (int i = 0; i < TAB_COUNT; i++) {
        NSTabViewItem *item =
            [[NSTabViewItem alloc] initWithIdentifier:@(kTabNames[i])];
        item.label = kTabLabels[i];
        item.view = tabPage(pages[i]);
        [_tabs addTabViewItem:item];
    }
    const int tab = tabFromEnv();
    if (tab >= 0)
        [_tabs selectTabViewItemAtIndex:tab];
    _tabs.delegate = self;

    _window.contentView = _tabs;
    [_window setContentSize:NSMakeSize(1500, 860)];
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

- (void)stepLine:(id)sender
{
    (void)sender;
    cocodebug_step_line(_dbg);
}

- (void)stepOverLine:(id)sender
{
    (void)sender;
    cocodebug_step_over_line(_dbg);
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

/* One or more .lst files, one per module of a multi-file CMOC program --
 * cocodebug_source_load() adds to what is already loaded rather than
 * replacing it (see its own doc comment for why: cmoc names the .map after
 * the linked executable, not any one .lst, so every module's listing wants
 * the very .map its neighbours do), so loading them all here in one panel
 * is exactly the multi-file case working as intended. */
- (void)loadSource:(id)sender
{
    (void)sender;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.title = @"Load C Source";
    panel.allowsMultipleSelection = YES;
    /* No allowedContentTypes: ".lst" is not a registered UTType either, and
     * leaving this unset is what shows every file, same reasoning as
     * loadSymbols: above. */
    if ([panel runModal] != NSModalResponseOK || panel.URLs.count == 0)
        return;

    int loaded = 0, failed = 0;
    for (NSURL *url in panel.URLs) {
        if (cocodebug_source_load(_dbg, url.path.UTF8String) < 0)
            failed++;
        else
            loaded++;
    }
    if (failed && !loaded) {
        _status.stringValue =
            @"Could not open listing or its .map (both need `cmoc -i`)";
        return;
    }
    if (failed)
        _status.stringValue = @"Some listings could not be opened";
    _srcCachedPath = nil; /* the file list may have changed */
    [self rebuildSrcCombo];
}

- (void)clearSource:(id)sender
{
    (void)sender;
    cocodebug_source_clear(_dbg);
    _srcCachedPath = nil;
    _srcCachedLines = nil;
    _pcFileId = -1;
    _srcDisplayedFileId = -1;
    [_srcFileCombo removeAllItems];
    _srcFileCombo.enabled = NO;
    _srcView.string = @"";
}

- (void)fujiClear:(id)sender
{
    (void)sender;
    cocodebug_fujinet_trace_clear(_dbg);
    [self refreshFujinet:YES];
}

- (void)fujiHideReady:(id)sender
{
    (void)sender;
    [self refreshFujinet:YES];
}

- (void)srcFileChanged:(id)sender
{
    (void)sender;
    NSInteger idx = _srcFileCombo.indexOfSelectedItem;
    if (idx >= 0)
        [self populateSrcList:(int)idx];
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
    BOOL srcAvail = cocodebug_source_available(_dbg);
    _stepLineBtn.enabled = srcAvail;
    _stepOverLineBtn.enabled = srcAvail;
    if (paused) {
        [self refreshDisasm];
        [self refreshRegs];
        [self refreshSource];
        [self refreshLocals];
        [self refreshMem];
        [self refreshVideo];
    } else {
        /* Everything on show is only valid while the machine is parked, so
         * clear it rather than leave a stale snapshot that looks live. */
        _disasm.string = @"";
        _regs.string = @"";
        _srcView.string = @"";
        _locals.string = @"";
        _memView.string = @"";
        _video.image = nil;
    }
    if ([self currentTab] == TAB_FUJINET)
        [self refreshFujinet:NO];
}

- (void)refreshFujinet:(BOOL)force
{
    static coco_fuji_packet p[FUJI_TRACE_MAX];
    char state[1024];
    const int n = cocodebug_fujinet_trace(_dbg, p, FUJI_TRACE_MAX);
    const BOOL hide = _fujiHideReady.state == NSControlStateValueOn;

    if (!cocosession_fujinet_running(_session))
        _fujiNote.stringValue = @"FujiNet is off (Settings ▸ FujiNet).";
    else
        _fujiNote.stringValue =
            cocosession_fujinet_link(_session)
                ? @"FujiNet is on the Becker port, connected."
                : @"FujiNet is on the Becker port, not connected.";
    cocodebug_fujinet_state(_dbg, state, (int)sizeof state);
    setText(_fujiState, textFrom(state), NO);
    _fujiTicks = 0;

    if (!force && n == _fujiCount && (n == 0 || p[n - 1].serial == _fujiLast))
        return;
    _fujiCount = n;
    _fujiLast = n > 0 ? p[n - 1].serial : 0;
    const BOOL follow = force || atEnd(_fujiText);
    NSMutableString *text = [NSMutableString string];
    int shown = 0;
    for (int i = 0; i < n; i++) {
        if (hide && cocodebug_fujinet_is_ready(&p[i]))
            continue;
        if (shown++)
            [text appendString:@"\n"];
        [text appendString:textFrom(p[i].text)];
        [text appendString:@"\n     "];
        for (int k = 0; k < p[i].nbytes && k < (int)sizeof p[i].data; k++)
            [text appendFormat:@"%02X ", p[i].data[k]];
        if (p[i].nbytes > (int)sizeof p[i].data)
            [text appendString:@"…"];
    }
    if (shown == 0)
        [text setString:n ? @"(only READY probes since the trace was cleared)"
                          : @"(no DriveWire traffic since the trace was "
                            @"cleared)"];
    setText(_fujiText, text, follow);
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

- (void)refreshLocals
{
    if (!cocodebug_source_available(_dbg)) {
        _locals.string = @"";
        return;
    }
    cocodebug_local locals[LOCALS_MAX];
    int n = cocodebug_locals_get(_dbg, locals, LOCALS_MAX);
    if (n == 0) {
        /* Not necessarily an error: PC may just be before the function's
         * first statement (mid-prologue), where there is nothing correct to
         * show yet. */
        _locals.string = @"(no locals in scope)";
        return;
    }
    NSMutableString *text = [NSMutableString string];
    for (int i = 0; i < n; i++) {
        const cocodebug_local *v = &locals[i];
        if (v->has_value)
            [text appendFormat:@"%-12s %-10s %s= %u ($%X)\n", v->name,
                               v->type, v->is_param ? "(param) " : "",
                               v->value, v->value];
        else
            [text appendFormat:@"%-12s %-10s %s= ?\n", v->name, v->type,
                               v->is_param ? "(param) " : ""];
    }
    _locals.string = text;
}

/* Loads (and caches) fileId's text, then redraws the list with '>' on the
 * line PC is at (if PC's file is the one showing) and '*' on any line
 * carrying a breakpoint. Re-reading the file is cheap enough here since this
 * only runs on an actual stop or a manual file switch, never on every
 * 0.1s tick -- see onTick's early-out. */
- (void)populateSrcList:(int)fileId
{
    const char *path = cocodebug_source_file_path(_dbg, fileId);
    NSString *nsPath = path ? [NSString stringWithUTF8String:path] : nil;
    _srcDisplayedFileId = fileId;

    if (![nsPath isEqualToString:_srcCachedPath]) {
        _srcCachedPath = nsPath;
        NSString *contents =
            nsPath ? [NSString stringWithContentsOfFile:nsPath
                                                encoding:NSUTF8StringEncoding
                                                   error:nil]
                   : nil;
        _srcCachedLines = contents ? [contents componentsSeparatedByString:@"\n"]
                                   : @[];
    }

    NSMutableString *text = [NSMutableString string];
    for (NSUInteger i = 0; i < _srcCachedLines.count; i++) {
        int lineno = (int)i + 1;
        BOOL isPc = (fileId == _pcFileId && lineno == _pcLine);
        BOOL hasBp = cocodebug_source_breakpoint_is_set(_dbg, fileId, lineno);
        [text appendFormat:@"%c%c %5d  %@\n", isPc ? '>' : ' ',
                           hasBp ? '*' : ' ', lineno, _srcCachedLines[i]];
    }
    _srcView.string = text;
}

/* Rebuilds the file picker from cocodebug_source_file_count()/_file_path().
 * Selected index and file_id are the same number: items are appended here in
 * exactly the id order the engine hands them out (0..count-1). */
- (void)rebuildSrcCombo
{
    [_srcFileCombo removeAllItems];
    int n = cocodebug_source_file_count(_dbg);
    for (int i = 0; i < n; i++) {
        const char *path = cocodebug_source_file_path(_dbg, i);
        NSString *full = path ? [NSString stringWithUTF8String:path] : @"";
        NSString *base = full.lastPathComponent;
        [_srcFileCombo addItemWithTitle:base.length ? base : full];
    }
    _srcFileCombo.enabled = (n > 0);
    if (n > 0) {
        [_srcFileCombo selectItemAtIndex:0];
        [self populateSrcList:0];
    } else {
        _srcView.string = @"";
    }
}

/* Follows PC's file automatically. If PC is not itself a mapped statement
 * (mid-prologue, or code with no line info), keeps showing whatever file
 * was last selected and just drops the '>' marker rather than guess. */
- (void)refreshSource
{
    if (!cocodebug_source_available(_dbg)) {
        _srcView.string = @"";
        return;
    }
    cocodebug_regs r;
    if (!cocodebug_regs_get(_dbg, &r))
        return;
    int fid, line;
    if (cocodebug_source_line_at(_dbg, r.pc, &fid, &line)) {
        _pcFileId = fid;
        _pcLine = line;
        if (_srcFileCombo.indexOfSelectedItem != fid) {
            [_srcFileCombo selectItemAtIndex:fid];
            [self populateSrcList:fid];
        } else {
            [self populateSrcList:_srcDisplayedFileId];
        }
    } else {
        _pcFileId = -1;
        _pcLine = -1;
        if (_srcFileCombo.numberOfItems > 0)
            [self populateSrcList:_srcDisplayedFileId];
    }
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
