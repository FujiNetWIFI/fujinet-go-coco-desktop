/*
 * Debugger window (AppKit) over the shared cocodebug engine (cocodebug.h,
 * cocomemtools.h, cocovidtab.h): the family's thirteen-tab window, tab for
 * tab like the GNOME, KDE and Windows ones. Console, Disassembly, C Source,
 * Memory, Breakpoints, Watches, Cart & I/O, Trace, History, Video, Sound,
 * Symbols, FujiNet. The 6809's (6309's) registers sit in an editable strip
 * under the tabs; any tab can be popped out into a window of its own (Pop
 * Out / Dock).
 *
 * Opening the window stops the machine (engage + pause); closing it releases
 * the engine so the machine goes on at full speed.
 *
 * A 100 ms NSTimer polls cocodebug_stop_snapshot() and cocodebug_generation()
 * and refreshes the tabs on show when either moved (a stop, a resume, a step,
 * a console command). A new stop serial puts every view back on the PC. The
 * machine can only be read while it is parked, so a running machine's views
 * say so; the Trace log and the FujiNet packets follow it live.
 *
 * Keys work anywhere in the window (a local key monitor, so the console's
 * field takes them too): F5 pause/run, F6 step line (Shift: over the line),
 * F7 step, F8 step over (Shift: step out).
 *
 * The C Source tab shows the CMOC listings loaded with Load C Source: a click
 * toggles a breakpoint on a line, the line the PC is on is the accent bar, and
 * Locals shows the current function's parameters and locals.
 *
 * COCO_DEBUGGER_TAB picks the tab shown first: an index (0 Console,
 * 1 Disassembly, 2 C Source, 3 Memory, 4 Breakpoints, 5 Watches, 6 Cart & I/O,
 * 7 Trace, 8 History, 9 Video, 10 Sound, 11 Symbols, 12 FujiNet) or the start
 * of a tab's name ("cpu" is Disassembly, "io" Cart & I/O, "source" C Source).
 * For captures: COCO_WATCH (a comma list), COCO_WATCH_BREAK, COCO_MEM_GOTO,
 * COCO_HISTORY=1 and COCO_POPOUT=<tab>; COCO_DEBUGGER_VIDEO_* are the Video
 * tab's (read by cocovid_tab_new).
 *
 * Highlights: the PC's line is the accent green with the dark ink on it, an
 * enabled breakpoint's marker red and a disabled one's hollow grey, labels
 * grey; the Run button is the accent while stopped.
 *
 * NOT COMPILED OR RUN HERE (no Cocoa SDK on the Linux dev box): checked for
 * syntax against a Cocoa stub, built for real by the macOS CI job.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DebuggerWindow.h"

#import "../help/HelpWindow.h"
#import "VideoTab.h"

#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "cocodebug.h"
#include "cocomemtools.h"
#include "cocosession.h"
#include "cocovidtab.h"

#define DISASM_WINDOW 48
#define MAX_BPS 128
#define MEM_ROWS 32             /* the hex dump: 16 bytes a row */
#define MEM_BYTES (MEM_ROWS * 16)
#define MAX_REGIONS 64
#define LOCALS_MAX 32
#define FUJI_PACKETS_MAX 513    /* all the engine keeps, plus the one in progress */
#define NUM_REG_FIELDS 14       /* PC A B DP X Y U S CC, then E F W V MD (6309) */
#define NUM_MAIN_REGS 9         /* PC A B DP X Y U S CC */
#define NUM_FLAGS 8
#define TAB_CONSOLE 0           /* the tabs, in order */
#define TAB_DISASM 1
#define TAB_SOURCE 2
#define TAB_MEMORY 3
#define TAB_BREAKS 4
#define TAB_WATCHES 5
#define TAB_CART 6
#define TAB_TRACE 7
#define TAB_HIST 8
#define TAB_VIDEO 9
#define TAB_SOUND 10
#define TAB_SYMBOLS 11
#define TAB_FUJINET 12
#define NUM_TABS 13
#define HIST_RING 65536         /* the engine's trace ring */
#define HIST_SHOWN 2000         /* the newest matching lines a tab shows */
#define TRACE_TICKS 4           /* 4 x 100 ms: the Trace tab's live refresh */
#define LIVE_TICKS 5            /* 5 x 100 ms: the live refresh while running */
#define SOUND_SHOWN 160         /* DAC/single-bit writes listed on the Sound tab */

static DebuggerWindow *g_debugger;

/* ---- colours ---------------------------------------------------------------- */

static NSColor *rgbColor(unsigned rgb)
{
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8) & 0xFF) / 255.0
                                blue:(rgb & 0xFF) / 255.0
                               alpha:1.0];
}

/* The app's accent green, and the ink that reads on it. */
static NSColor *accentFill(void)
{
    return rgbColor(COCOSESSION_ACCENT_RGB);
}

static NSColor *accentInk(void)
{
    return rgbColor(COCOSESSION_ACCENT_INK_RGB);
}

/* Toggled-on look for a push button: accent bezel and a dark-ink title. Off
 * restores the plain bezel and title. Call again after setting a new title
 * on a button that is on (a plain title drops the attributed one). */
static void setButtonAccent(NSButton *button, BOOL active)
{
    if (!button)
        return;
    NSString *title = [button.title copy];
    if (active) {
        NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
        para.alignment = NSTextAlignmentCenter;
        button.bezelColor = accentFill();
        button.attributedTitle = [[NSAttributedString alloc]
            initWithString:title
                attributes:@{
                    NSFontAttributeName: button.font ?: [NSFont systemFontOfSize:[NSFont systemFontSize]],
                    NSForegroundColorAttributeName: accentInk(),
                    NSParagraphStyleAttributeName: para
                }];
    } else {
        button.bezelColor = nil;
        button.title = title;       /* drops the attributed title */
    }
}

/* An enabled breakpoint's marker: the same red in every toolkit's debugger. */
static NSColor *breakpointRed(void)
{
    return [NSColor colorWithSRGBRed:0xE0 / 255.0 green:0x1B / 255.0 blue:0x24 / 255.0 alpha:1.0];
}

/* ---- text views that report clicks ------------------------------------------ */

/* A monospace text view for rows: a click reports its line, column and click
 * count; the wheel browses by row (when onScroll is set). The disassembly and
 * the memory dump. */
@interface DasmTextView : NSTextView
@property (nonatomic, copy) void (^onClick)(int line, int column, NSInteger clicks);
@property (nonatomic, copy) void (^onScroll)(int rows);
@end

@implementation DasmTextView {
    CGFloat _wheel;     /* the wheel's fraction not yet scrolled */
}

- (void)mouseDown:(NSEvent *)event
{
    NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    NSUInteger index = [self characterIndexForInsertionAtPoint:point];
    NSString *text = self.string;
    if (index > text.length)
        index = text.length;
    int line = 0;
    NSUInteger lineStart = 0;
    for (NSUInteger pos = 0; pos < index; pos++)
        if ([text characterAtIndex:pos] == '\n') {
            line++;
            lineStart = pos + 1;
        }
    if (self.onClick)
        self.onClick(line, (int)(index - lineStart), event.clickCount);
}

- (void)scrollWheel:(NSEvent *)event
{
    if (!self.onScroll) {
        [super scrollWheel:event];
        return;
    }
    /* a notch is a few rows; a trackpad's pixels are many */
    _wheel += event.hasPreciseScrollingDeltas ? -event.scrollingDeltaY / 8.0 : -event.scrollingDeltaY * 3.0;
    const int rows = (int)_wheel;
    if (rows == 0)
        return;
    _wheel -= rows;
    self.onScroll(rows);
}
@end

/* The Symbols list and the C source: a click reports the clicked line (the
 * wheel scrolls them as any text view does). */
@interface LineTextView : NSTextView
@property (nonatomic, copy) void (^onClickLine)(int line, NSInteger clicks);
@end

@implementation LineTextView
- (void)mouseDown:(NSEvent *)event
{
    NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    NSUInteger index = [self characterIndexForInsertionAtPoint:point];
    NSString *text = self.string;
    if (index > text.length)
        index = text.length;
    int line = 0;
    for (NSUInteger pos = 0; pos < index; pos++)
        if ([text characterAtIndex:pos] == '\n')
            line++;
    if (self.onClickLine)
        self.onClickLine(line, event.clickCount);
}
@end

@interface DebuggerWindow () <NSWindowDelegate, NSTabViewDelegate, NSTextFieldDelegate>
- (instancetype)initWithSession:(cocosession *)session;
- (void)show;
- (void)refreshNow;
@end

@implementation DebuggerWindow {
    cocosession *_session;
    cocodebug *_dbg;
    NSWindow *_window;
    NSTimer *_tick;
    id _keyMonitor;
    unsigned _seenGen;
    int _wasPaused;             /* -1 until the first look: forces a refresh */
    int _liveTicks;
    int _traceTicks;
    NSTabView *_tabs;

    NSButton *_runBtn;
    NSButton *_stepLineBtn, *_stepOverLineBtn;
    NSTextField *_status;
    NSTextField *_breakAt;

    NSTextView *_conOut;
    NSTextField *_conIn;
    NSMutableArray<NSString *> *_history;
    NSInteger _historyPos;

    /* the strip under the tabs */
    NSTextField *_reg[NUM_REG_FIELDS];
    NSButton *_flag[NUM_FLAGS];
    NSTextField *_dLabel;
    NSStackView *_extraRegs;     /* the 6309's E F W V MD, shown only on one */

    /* Disassembly */
    NSButton *_followPc;
    NSTextField *_jump;
    DasmTextView *_disasm;
    uint16_t _disasmAddr;        /* the first line shown */
    BOOL _scrolledAway;          /* the wheel moved the disassembly off the PC */
    cocodebug_line _lines[DISASM_WINDOW];
    int _lineCount;

    /* Memory */
    NSPopUpButton *_memSpace;
    DasmTextView *_mem;
    NSTextField *_memGoto, *_memWrite;
    uint32_t _memBase[2];        /* the dump's first address, per space */
    BOOL _memHl;                 /* a range is highlighted: Go to, a hit, a watch */
    int _memHlSpace;
    uint32_t _memHlLo, _memHlHi;
    BOOL _holdMemOnce;           /* COCO_MEM_GOTO: the first stop leaves the dump alone */
    NSPopUpButton *_srchKind;
    NSTextField *_srchEntry, *_srchNote;
    uint32_t _physSize;          /* the CoCo 3's physical RAM, 0 on a CoCo 1/2 (last paused look) */

    uint64_t _seenStop;          /* the stop serial last acted on */

    /* Breakpoints */
    NSPopUpButton *_bpType;
    NSButton *_bpPhys;
    NSTextField *_bpStart, *_bpEnd, *_bpCond, *_bpErr, *_bpStop;
    NSStackView *_bpList;
    NSString *_bpSig;

    /* Watches */
    NSStackView *_wtList;
    NSString *_wtSig;
    NSTextField *_wtEntry;
    NSPopUpButton *_wtSpace;

    NSTextView *_cartText;
    NSTextView *_soundText;
    BOOL _soundWatching;

    /* Trace (0) and History (1) share one ring */
    NSButton *_hRec[2];
    NSPopUpButton *_hFilter[2];
    NSTextField *_hRange[2], *_hNote[2];
    NSTextView *_hText[2];

    /* Symbols */
    NSTextField *_symFilter, *_symCount, *_symName, *_symAddr;
    LineTextView *_symList;
    NSMutableArray<NSNumber *> *_symAddrs;      /* the address of each line shown */
    int _symSelected;                           /* the selected line, -1 for none */

    NSTabViewItem *_items[NUM_TABS];    /* each tab's item, in order */
    NSWindow *_popped[NUM_TABS];        /* its own window while popped out */

    /* C Source */
    NSTextField *_srcNote;
    NSPopUpButton *_srcFiles;
    LineTextView *_srcView;
    NSTextView *_locals;
    NSString *_srcCachedPath;
    NSArray<NSString *> *_srcCachedLines;
    int _srcShownFile;          /* the file the combo shows, -1 for none */
    int _pcFile, _pcLine;       /* where PC is, independent of the file being browsed */
    BOOL _srcDirty;

    CocoVideoTab *_video;       /* the Video tab (VideoTab.m) */
    BOOL _videoShown;           /* it was on show at the last refresh */

    /* FujiNet */
    NSTextField *_fujiNote;
    NSButton *_fujiHideReady;
    NSTextView *_fujiText, *_fujiState;
    uint64_t _fujiLast;
    int _fujiCount;             /* what the log shows: -1 forces a rebuild */
}

/* COCO_DEBUGGER_TAB takes an index or the start of one of these. */
static const char *const kTabNames[NUM_TABS] = {
    "console", "disassembly", "csource", "memory", "breakpoints", "watches", "cart", "trace", "history", "video",
    "sound", "symbols", "fujinet"
};
static NSString *const kTabTitles[NUM_TABS] = {
    @"Console", @"Disassembly", @"C Source", @"Memory", @"Breakpoints", @"Watches", @"Cart & I/O", @"Trace",
    @"History", @"Video", @"Sound", @"Symbols", @"FujiNet"
};
/* Help's contexts (cocohelp.h), one per tab */
static NSString *const kHelpTabs[NUM_TABS] = {
    @"console", @"disassembly", @"csource", @"memory", @"breakpoints", @"watches", @"cart", @"trace", @"history",
    @"video", @"sound", @"symbols", @"fujinet"
};

/* The register fields, in strip order: label, hex digits, cocodebug_reg. */
static NSString *const kRegNames[NUM_REG_FIELDS] = {
    @"PC", @"A", @"B", @"DP", @"X", @"Y", @"U", @"S", @"CC", @"E", @"F", @"W", @"V", @"MD"
};
static const int kRegDigits[NUM_REG_FIELDS] = { 4, 2, 2, 2, 4, 4, 4, 4, 2, 2, 2, 4, 4, 2 };
static const int kRegIds[NUM_REG_FIELDS] = {
    COCODEBUG_REG_PC, COCODEBUG_REG_A, COCODEBUG_REG_B, COCODEBUG_REG_DP, COCODEBUG_REG_X, COCODEBUG_REG_Y,
    COCODEBUG_REG_U, COCODEBUG_REG_S, COCODEBUG_REG_CC, COCODEBUG_REG_E, COCODEBUG_REG_F, COCODEBUG_REG_W,
    COCODEBUG_REG_V, COCODEBUG_REG_MD
};
/* The CC flags, bit 7 down to bit 0. */
static NSString *const kFlagNames[NUM_FLAGS] = { @"E", @"F", @"H", @"I", @"N", @"Z", @"V", @"C" };
static const unsigned kFlagMasks[NUM_FLAGS] = {
    COCODEBUG_CC_E, COCODEBUG_CC_F, COCODEBUG_CC_H, COCODEBUG_CC_I, COCODEBUG_CC_N, COCODEBUG_CC_Z, COCODEBUG_CC_V,
    COCODEBUG_CC_C
};

/* A tab by index or by the start of its name; -1 for none. */
static int tabFromText(const char *tab)
{
    if (!tab || !*tab)
        return -1;
    if (tab[0] >= '0' && tab[0] <= '9')
        return atoi(tab);
    const size_t len = strlen(tab);
    if (!strncasecmp(tab, "cpu", len) && len > 1)
        return TAB_DISASM;
    if (!strncasecmp(tab, "prompt", len) && len > 1)
        return TAB_CONSOLE;
    if (!strncasecmp(tab, "io", 2) || !strncasecmp(tab, "i/o", 3))
        return TAB_CART;
    if (!strncasecmp(tab, "source", len) || !strncasecmp(tab, "c source", len))
        return TAB_SOURCE;
    for (int tabIndex = 0; tabIndex < NUM_TABS; tabIndex++)
        if (!strncasecmp(tab, kTabNames[tabIndex], len))
            return tabIndex;
    return -1;
}

/* ---- helpers --------------------------------------------------------------- */

/* The engine's text as a string: control bytes dropped (a console's CRs and
 * escapes), UTF-8 if it is, otherwise Latin-1. */
static NSString *cstr(const char *text)
{
    if (!text)
        return @"";
    const size_t length = strlen(text);
    char *copy = malloc(length + 1);
    if (!copy)
        return @"";
    size_t used = 0;
    for (size_t pos = 0; pos < length; pos++) {
        const unsigned char code = (unsigned char)text[pos];
        if (code >= 0x20 || code == '\n' || code == '\t')
            copy[used++] = (char)code;
    }
    copy[used] = '\0';
    NSString *result = [NSString stringWithUTF8String:copy];
    if (!result)
        result = [[NSString alloc] initWithBytes:copy length:used encoding:NSISOLatin1StringEncoding];
    free(copy);
    return result ?: @"";
}

/* Pads a column to width (never cuts a longer one). */
static NSString *padTo(NSString *text, NSUInteger width)
{
    return text.length >= width ? text : [text stringByPaddingToLength:width withString:@" " startingAtIndex:0];
}

static NSString *trimmed(NSString *text)
{
    return [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
}

/* $hex, 0xhex, hexh, #dec, or bare hex. */
static BOOL parseNum(NSString *text, long *out)
{
    NSString *word = trimmed(text);
    int base = 16;
    if ([word hasPrefix:@"$"])
        word = [word substringFromIndex:1];
    else if ([word.lowercaseString hasPrefix:@"0x"])
        word = [word substringFromIndex:2];
    else if ([word hasPrefix:@"#"]) {
        word = [word substringFromIndex:1];
        base = 10;
    } else if (word.length > 1 && [word.lowercaseString hasSuffix:@"h"])
        word = [word substringToIndex:word.length - 1];
    if (word.length == 0)
        return NO;
    char *end;
    const long value = strtol(word.UTF8String, &end, base);
    if (*end)
        return NO;
    *out = value;
    return YES;
}

static NSFont *monoFont(void)
{
    return [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
}

static NSDictionary *monoAttrs(void)
{
    return @{ NSFontAttributeName: monoFont(), NSForegroundColorAttributeName: NSColor.textColor };
}

/* The accent fill with the dark ink on it: the PC's line, a marked row. */
static NSDictionary *accentAttrs(void)
{
    return @{ NSBackgroundColorAttributeName: accentFill(), NSForegroundColorAttributeName: accentInk() };
}

static NSTextView *monoView(NSScrollView **scrollOut, BOOL wrap)
{
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    scroll.hasHorizontalScroller = !wrap;
    NSTextView *view = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
    view.editable = NO;
    view.richText = NO;
    view.font = monoFont();
    if (wrap) {
        view.autoresizingMask = NSViewWidthSizable;
    } else {
        view.horizontallyResizable = YES;
        view.textContainer.widthTracksTextView = NO;
        view.textContainer.containerSize = NSMakeSize(FLT_MAX, FLT_MAX);
        view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    }
    scroll.documentView = view;
    *scrollOut = scroll;
    return view;
}

/* A monospace row view in a scroll view with no vertical bar: the wheel and
 * the clicks are the view's own (the disassembly, the memory dump). */
static DasmTextView *rowView(NSScrollView **scrollOut)
{
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = NO;
    scroll.hasHorizontalScroller = YES;
    DasmTextView *view = [[DasmTextView alloc] initWithFrame:NSMakeRect(0, 0, 600, 600)];
    view.editable = NO;
    view.richText = NO;
    view.selectable = NO;
    view.font = monoFont();
    view.horizontallyResizable = YES;
    view.textContainer.widthTracksTextView = NO;
    view.textContainer.containerSize = NSMakeSize(FLT_MAX, FLT_MAX);
    view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    scroll.documentView = view;
    *scrollOut = scroll;
    return view;
}

/* A click-reporting line list in a scroll view (Symbols, C Source). */
static LineTextView *lineView(NSScrollView **scrollOut)
{
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    scroll.hasHorizontalScroller = YES;
    LineTextView *view = [[LineTextView alloc] initWithFrame:NSMakeRect(0, 0, 700, 500)];
    view.editable = NO;
    view.richText = YES;
    view.selectable = NO;
    view.font = monoFont();
    view.minSize = NSMakeSize(0, 0);
    view.maxSize = NSMakeSize(FLT_MAX, FLT_MAX);
    view.verticallyResizable = YES;
    view.horizontallyResizable = YES;
    view.textContainer.widthTracksTextView = NO;
    view.textContainer.containerSize = NSMakeSize(FLT_MAX, FLT_MAX);
    view.autoresizingMask = NSViewWidthSizable;
    scroll.documentView = view;
    *scrollOut = scroll;
    return view;
}

/* Whether a view's reader is at its end (a log then follows new lines). */
static BOOL atEnd(NSTextView *view)
{
    NSScrollView *scroll = view.enclosingScrollView;
    if (!scroll)
        return YES;
    return NSMaxY(scroll.contentView.documentVisibleRect) >= NSHeight(view.frame) - 4;
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
static void setText(NSTextView *view, NSString *text, BOOL follow)
{
    if (![view.string isEqualToString:text]) {
        NSScrollView *scroll = view.enclosingScrollView;
        const NSPoint where = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
        view.string = text;
        if (scroll && !follow) {
            [scroll.contentView scrollToPoint:where];
            [scroll reflectScrolledClipView:scroll.contentView];
        }
    }
    if (follow)
        [view scrollRangeToVisible:NSMakeRange(view.string.length, 0)];
}

/* The same for styled text, always where the reader left it. */
static void setStyledText(NSTextView *view, NSAttributedString *text)
{
    NSScrollView *scroll = view.enclosingScrollView;
    const NSPoint where = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
    [view.textStorage setAttributedString:text];
    if (scroll) {
        [scroll.contentView scrollToPoint:where];
        [scroll reflectScrolledClipView:scroll.contentView];
    }
}

- (NSButton *)button:(NSString *)title action:(SEL)sel
{
    NSButton *button = [NSButton buttonWithTitle:title target:self action:sel];
    [button setRefusesFirstResponder:YES];
    return button;
}

- (NSTextField *)label:(NSString *)text
{
    return [NSTextField labelWithString:text];
}

- (NSTextField *)hint:(NSString *)text
{
    NSTextField *label = [self label:text];
    label.textColor = NSColor.secondaryLabelColor;
    /* a long hint gives way to a narrow window instead of widening it */
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    [label setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                    forOrientation:NSLayoutConstraintOrientationHorizontal];
    return label;
}

- (NSTextField *)heading:(NSString *)text
{
    NSTextField *label = [self label:text];
    label.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize]];
    return label;
}

- (NSTextField *)field:(NSString *)placeholder width:(CGFloat)width action:(SEL)sel
{
    NSTextField *field = [[NSTextField alloc] init];
    field.placeholderString = placeholder;
    field.font = monoFont();
    field.target = self;
    field.action = sel;
    if (width > 0)
        [field.widthAnchor constraintEqualToConstant:width].active = YES;
    return field;
}

static NSStackView *hstack(NSArray<NSView *> *views)
{
    NSStackView *stack = [NSStackView stackViewWithViews:views];
    stack.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    stack.spacing = 6;
    return stack;
}

static NSStackView *vstack(NSArray<NSView *> *views)
{
    NSStackView *stack = [NSStackView stackViewWithViews:views];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 6;
    return stack;
}

static void fill(NSStackView *stack, NSView *view)
{
    [view.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
}

/* A value as the fields take it: a symbol, an expression (registers, [addr],
 * C operators: the console's), or $hex, 0xhex, hexh, #dec, bare hex. On
 * failure the reason is in *why. */
- (BOOL)evaluate:(NSString *)text into:(long *)out why:(NSString **)why
{
    NSString *word = trimmed(text);
    if (word.length == 0) {
        if (why)
            *why = @"nothing entered";
        return NO;
    }
    uint16_t symbolAddr;
    if (cocodebug_symbol_find(_dbg, word.UTF8String, &symbolAddr)) {
        *out = symbolAddr;
        return YES;
    }
    if (parseNum(word, out))
        return YES;
    char err[160];
    err[0] = '\0';
    long value;
    if (cocodebug_eval(_dbg, word.UTF8String, &value, err, (int)sizeof err) == 0) {
        *out = value;
        return YES;
    }
    if (why)
        *why = err[0] ? cstr(err) : @"not a number, symbol or expression";
    return NO;
}

/* An address: the above, within $0000-$FFFF. -1 when it is not one. */
- (int)resolveAddr:(NSString *)text why:(NSString **)why
{
    long value;
    if (![self evaluate:text into:&value why:why])
        return -1;
    if (value < 0 || value > 0xFFFF) {
        if (why)
            *why = @"out of range ($0000-$FFFF)";
        return -1;
    }
    return (int)value;
}

/* ---- lifetime -------------------------------------------------------------- */

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
    [g_debugger show];
}

- (void)show
{
    if (_window.isMiniaturized)
        [_window deminiaturize:nil];
    [_window makeKeyAndOrderFront:nil];
    for (int tab = 0; tab < NUM_TABS; tab++)
        if (_popped[tab])
            [_popped[tab] orderFront:nil];
    [self settleFrom:cocodebug_generation(_dbg) - 1];
    [self refreshNow];
    if ([self currentTab] == TAB_CONSOLE)
        [_window makeFirstResponder:_conIn];
}

- (instancetype)initWithSession:(cocosession *)session
{
    self = [super init];
    if (!self)
        return nil;
    _session = session;
    _dbg = cocosession_debugger(session);
    _fujiCount = -1;
    _wasPaused = -1;
    _pcFile = -1;
    _pcLine = -1;
    _srcShownFile = -1;
    _srcDirty = YES;
    _symSelected = -1;
    _symAddrs = [NSMutableArray array];
    _history = [NSMutableArray array];
    [self buildWindow];

    /* Engaging switches the emulator loop to the path that honours
     * breakpoints. Pausing immediately is what a user opening a debugger
     * expects -- otherwise the window opens showing nothing at all. */
    cocodebug_set_engaged(_dbg, 1);
    cocodebug_pause(_dbg);

    /* For captures and tests, like COCO_DEBUGGER_TAB */
    {
        const char *watch = getenv("COCO_WATCH");
        const char *watchBreak = getenv("COCO_WATCH_BREAK");
        const char *gotoText = getenv("COCO_MEM_GOTO");
        const char *history = getenv("COCO_HISTORY");
        const char *popOut = getenv("COCO_POPOUT");
        uint32_t low, high;
        if (watch && *watch) {
            for (NSString *item in [cstr(watch) componentsSeparatedByString:@","]) {
                NSString *text = trimmed(item);
                if (text.length)
                    cocowatch_add(text.UTF8String, COCODEBUG_SPACE_CPU);
            }
        }
        if (watchBreak && *watchBreak) {
            cocowatch_add(watchBreak, COCODEBUG_SPACE_CPU);
            if (cocosym_parse_range(_dbg, COCODEBUG_SPACE_CPU, watchBreak, &low, &high))
                [self breakOnChangeSpace:COCODEBUG_SPACE_CPU low:low high:high];
        }
        if (gotoText && *gotoText && cocosym_parse_range(_dbg, COCODEBUG_SPACE_CPU, gotoText, &low, &high)) {
            _holdMemOnce = YES;
            [self showSpace:COCODEBUG_SPACE_CPU low:low high:high];
        }
        if (history && *history)
            cocodebug_trace_enable(_dbg, 1);
        if (popOut && *popOut) {
            const int tab = tabFromText(popOut);
            if (tab >= 0 && tab < NUM_TABS)
                [self popOutTab:tab];
        }
    }

    __weak DebuggerWindow *weakSelf = self;
    _tick = [NSTimer timerWithTimeInterval:0.1 repeats:YES block:^(NSTimer *timer) {
        (void)timer;
        [weakSelf onTick];
    }];
    [[NSRunLoop mainRunLoop] addTimer:_tick forMode:NSRunLoopCommonModes];

    /* F5/F6/F7/F8 wherever the focus is in this window, the console's
     * field included */
    _keyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                                        handler:^NSEvent *(NSEvent *event) {
        DebuggerWindow *strongSelf = weakSelf;
        if (!strongSelf || ![strongSelf ownsWindow:event.window] || !strongSelf->_window.isVisible)
            return event;
        return [strongSelf handleKey:event] ? nil : event;
    }];
    return self;
}

- (void)dealloc
{
    [_tick invalidate];
    if (_keyMonitor)
        [NSEvent removeMonitor:_keyMonitor];
}

/* windowShouldClose: fires whenever the window's close control (or Window >
 * Close) is used. Closing a popped-out tab's window docks the tab again.
 * Closing the main window keeps it alive but hidden, like GNOME/Windows do,
 * so F12 reopens it instantly with its state intact -- but disengages RIGHT
 * HERE rather than relying on -dealloc, which for this hide-not-destroy shape
 * would only ever run at process exit. Leaving the debugger engaged after
 * every close would otherwise run the emulator on the slow instruction-hooked
 * path forever with the window merely hidden. */
- (BOOL)windowShouldClose:(NSWindow *)sender
{
    for (int tab = 0; tab < NUM_TABS; tab++)
        if (sender == _popped[tab]) {
            [self dockTab:tab];
            return NO;
        }
    [self soundWatch:NO];
    cocodebug_set_engaged(_dbg, 0);
    _wasPaused = -1;
    for (int tab = 0; tab < NUM_TABS; tab++)
        [_popped[tab] orderOut:nil];     /* they come back with the window */
    [_window orderOut:nil];
    return NO;
}

/* The debugger's main window or one of its popped-out tabs. */
- (BOOL)ownsWindow:(NSWindow *)window
{
    if (!window)
        return NO;
    if (window == _window)
        return YES;
    for (int tab = 0; tab < NUM_TABS; tab++)
        if (_popped[tab] == window)
            return YES;
    return NO;
}

/* The tab the main window's notebook shows, by its place in the full order
 * (-1 if none). */
- (NSInteger)currentTab
{
    NSTabViewItem *selected = _tabs.selectedTabViewItem;
    for (int tab = 0; tab < NUM_TABS; tab++)
        if (_items[tab] == selected)
            return tab;
    return -1;
}

/* On screen: the notebook's page, or a window of its own. */
- (BOOL)tabVisible:(int)tab
{
    return _popped[tab] != nil || [self currentTab] == tab;
}

- (int)memSpaceIndex
{
    const NSInteger index = _memSpace.indexOfSelectedItem;
    return index == COCODEBUG_SPACE_PHYS ? COCODEBUG_SPACE_PHYS : COCODEBUG_SPACE_CPU;
}

/* A new stop (a pause, a breakpoint, a step): every view goes to the current
 * spot, even if the reader scrolled away. The History and Trace logs always
 * follow their newest line. */
- (void)recentreViews:(const cocodebug_stop *)stop
{
    _scrolledAway = NO;
    _followPc.state = NSControlStateValueOn;
    _pcFile = -1;
    _pcLine = -1;
    _srcDirty = YES;
    if (_holdMemOnce) {
        _holdMemOnce = NO;      /* COCO_MEM_GOTO's first look stays */
        return;
    }
    if (stop->pc >= 0) {
        _memBase[COCODEBUG_SPACE_CPU] = ((uint32_t)stop->pc & ~0xFu) - 2 * 16;
        if ((int32_t)_memBase[COCODEBUG_SPACE_CPU] < 0)
            _memBase[COCODEBUG_SPACE_CPU] = 0;
    }
    _memHl = NO;
}

/* The poll: refresh on a stop, a resume or a command (the generation), and
 * the visible tab's live values twice a second while the machine runs. The
 * FujiNet tab also follows its packets twice a second while stopped, as they
 * arrive on their own. */
- (void)onTick
{
    if (![_window isVisible])
        return;
    cocodebug_stop stop;
    cocodebug_stop_snapshot(_dbg, &stop);
    const unsigned gen = cocodebug_generation(_dbg);
    if (gen != _seenGen || stop.paused != _wasPaused || stop.serial != _seenStop) {
        [self refreshNow];
    } else if (++_liveTicks >= LIVE_TICKS) {
        _liveTicks = 0;
        if (!stop.paused) {
            [self refreshStatus];
            [self refreshTab];
        } else if ([self tabVisible:TAB_FUJINET]) {
            [self refreshFujinet:NO];
        }
    } else if (!stop.paused && [self tabVisible:TAB_TRACE] && ++_traceTicks >= TRACE_TICKS) {
        _traceTicks = 0;        /* the Trace tab follows the machine */
        [self refreshHist:0];
    }
}

/* Waits up to ~40 ms for a stop to land after the engine was asked to move:
 * the generation moves on and the machine reads as paused. */
- (void)settleFrom:(unsigned)gen
{
    for (int attempt = 0; attempt < 20; attempt++) {
        if (cocodebug_generation(_dbg) != gen && cocodebug_is_paused(_dbg))
            break;
        usleep(2000);
    }
}

- (void)updateHelpContext
{
    const NSInteger index = [self currentTab];
    if (index >= 0 && index < NUM_TABS)
        CocoSetHelpContext(_window, [@"debugger/" stringByAppendingString:kHelpTabs[index]]);
}

- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    (void)tabView;
    (void)item;
    [self updateHelpContext];
    [self refreshTab];
    if ([self currentTab] == TAB_CONSOLE)
        [_window makeFirstResponder:_conIn];
}

/* ---- keys -------------------------------------------------------------------- */

- (BOOL)handleKey:(NSEvent *)event
{
    NSString *chars = event.charactersIgnoringModifiers;
    if (chars.length != 1)
        return NO;
    const NSEventModifierFlags mods = event.modifierFlags;
    if (mods & (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption))
        return NO;
    const BOOL shift = (mods & NSEventModifierFlagShift) != 0;
    switch ([chars characterAtIndex:0]) {
    case NSF5FunctionKey:
        [self toggleRun:nil];
        return YES;
    case NSF6FunctionKey:
        if (shift)
            [self stepOverLine:nil];
        else
            [self stepLine:nil];
        return YES;
    case NSF7FunctionKey:
        [self step:nil];
        return YES;
    case NSF8FunctionKey:
        if (shift)
            [self stepOut:nil];
        else
            [self stepOver:nil];
        return YES;
    default:
        return NO;
    }
}

/* ---- construction ------------------------------------------------------------ */

- (NSView *)buildConsole
{
    NSScrollView *outScroll;
    _conOut = monoView(&outScroll, YES);
    _conOut.string = @"Debugger console. Type 'help' for every command; Tab completes, Up/Down walk the history.\n";
    _conIn = [self field:@"command (help, step, break $A1CB, bpw $0400, regs, mem $0400, find 8E 04, video, ...) -- "
                         @"Tab completes, up/down history"
                   width:0
                  action:@selector(runCommand:)];
    _conIn.delegate = self;

    NSStackView *page = vstack(@[outScroll, _conIn]);
    fill(page, outScroll);
    fill(page, _conIn);
    return page;
}

/* The registers and flags, under the tabs on every one of them. The 6309's
 * extra registers sit on a row of their own, shown only on a 6309. */
- (NSView *)buildRegStrip
{
    NSMutableArray *row = [NSMutableArray array];
    NSMutableArray *extra = [NSMutableArray array];
    for (int field = 0; field < NUM_REG_FIELDS; field++) {
        NSMutableArray *target = field < NUM_MAIN_REGS ? row : extra;
        [target addObject:[self label:kRegNames[field]]];
        _reg[field] = [self field:@"" width:(kRegDigits[field] == 4 ? 52 : 36) action:@selector(applyRegister:)];
        _reg[field].tag = field;
        _reg[field].toolTip = @"Edit while the machine is stopped (hex, $hex, a symbol or an expression), then Enter";
        [target addObject:_reg[field]];
        if (field == 2) {
            _dLabel = [self hint:@""];
            [target addObject:_dLabel];
        }
    }
    NSMutableArray *flags = [NSMutableArray arrayWithObject:[self label:@"CC"]];
    for (int flag = 0; flag < NUM_FLAGS; flag++) {
        _flag[flag] = [NSButton checkboxWithTitle:kFlagNames[flag] target:self action:@selector(flagClicked:)];
        _flag[flag].tag = flag;
        [flags addObject:_flag[flag]];
    }
    [row addObjectsFromArray:flags];
    _extraRegs = hstack(extra);
    _extraRegs.hidden = YES;
    return vstack(@[hstack(row), _extraRegs]);
}

- (NSView *)buildMemory
{
    NSScrollView *memScroll;
    _mem = rowView(&memScroll);
    __weak DebuggerWindow *weakSelf = self;
    _mem.onScroll = ^(int rows) {
        DebuggerWindow *strongSelf = weakSelf;
        if (!strongSelf || rows == 0)
            return;
        [strongSelf scrollMemBy:rows * 16];
    };
    _memSpace = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_memSpace addItemsWithTitles:@[@"CPU", @"PHYS"]];
    _memSpace.target = self;
    _memSpace.action = @selector(memSpaceChanged:);
    _memSpace.toolTip = @"CPU: the 64K the CPU sees. PHYS (CoCo 3 only): the physical RAM, whatever the MMU maps where";
    [_memSpace setRefusesFirstResponder:YES];
    _memGoto = [self field:@"address, symbol, expression or lo-hi" width:240 action:@selector(gotoMem:)];
    NSButton *goBtn = [self button:@"Go To" action:@selector(gotoMem:)];
    NSButton *prev = [self button:@"◀" action:@selector(memPrev:)];
    NSButton *next = [self button:@"▶" action:@selector(memNext:)];
    NSStackView *memHead = hstack(@[[self label:@"Space"], _memSpace, [self label:@"Go to"], _memGoto, goBtn, prev, next]);

    _srchKind = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_srchKind addItemsWithTitles:@[@"Hex", @"Text"]];
    _srchKind.toolTip = @"Hex bytes (8E 04) or text, one byte per character, not case-sensitive";
    [_srchKind setRefusesFirstResponder:YES];
    _srchEntry = [self field:@"hex bytes or text" width:220 action:@selector(searchNext:)];
    _srchNote = [self hint:@""];
    NSStackView *searchRow = hstack(@[[self label:@"Search"], _srchKind, _srchEntry,
                                      [self button:@"Find Next" action:@selector(searchNext:)],
                                      [self button:@"Find Previous" action:@selector(searchPrev:)], _srchNote]);

    _memWrite = [self field:@"address value [value...]" width:200 action:@selector(writeMem:)];
    NSStackView *edit = hstack(@[[self label:@"Poke"], _memWrite,
                                 [self hint:@"RAM only: a ROM or I/O write is skipped. Several values go to consecutive "
                                            @"addresses. Reading never touches a device."]]);

    NSStackView *page = vstack(@[memHead, searchRow, memScroll, edit]);
    fill(page, memScroll);
    return page;
}

- (NSView *)buildDisasm
{
    _followPc = [NSButton checkboxWithTitle:@"Follow PC" target:self action:@selector(followToggled:)];
    _followPc.state = NSControlStateValueOn;
    _jump = [self field:@"address, symbol or expression" width:200 action:@selector(jumpTo:)];
    NSStackView *row = hstack(@[_followPc, [self label:@"Jump to"], _jump,
                                [self hint:@"Click a line to toggle its breakpoint (red dot on, hollow grey off); "
                                           @"scroll to browse"]]);

    NSScrollView *scroll;
    _disasm = rowView(&scroll);
    __weak DebuggerWindow *weakSelf = self;
    _disasm.onClick = ^(int line, int column, NSInteger clicks) {
        (void)column;
        (void)clicks;
        DebuggerWindow *strongSelf = weakSelf;
        if (!strongSelf || line < 0 || line >= strongSelf->_lineCount)
            return;
        if (cocodebug_breakpoint_toggle(strongSelf->_dbg, strongSelf->_lines[line].address) < 0)
            [strongSelf message:@"The breakpoint table is full" console:NO];
        [strongSelf refreshDisasm];
        if ([strongSelf tabVisible:TAB_BREAKS])
            [strongSelf refreshBps];
    };
    _disasm.onScroll = ^(int rows) {
        DebuggerWindow *strongSelf = weakSelf;
        if (!strongSelf || rows == 0)
            return;
        strongSelf->_followPc.state = NSControlStateValueOff;
        strongSelf->_scrolledAway = YES;
        const int target = cocodebug_row_address(strongSelf->_dbg, strongSelf->_disasmAddr, rows);
        strongSelf->_disasmAddr = (uint16_t)target;
        [strongSelf refreshDisasm];
    };

    NSStackView *page = vstack(@[row, scroll]);
    fill(page, row);
    fill(page, scroll);
    return page;
}

/* ---- the C Source tab -------------------------------------------------------- */

- (NSView *)buildSource
{
    _srcFiles = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    _srcFiles.target = self;
    _srcFiles.action = @selector(srcFileChosen:);
    _srcFiles.toolTip = @"The source file shown (the file the PC is in is followed at every stop)";
    _srcFiles.enabled = NO;
    [_srcFiles setRefusesFirstResponder:YES];
    NSStackView *bar = hstack(@[[self button:@"Load C Source…" action:@selector(loadSource:)],
                                [self button:@"Clear C Source" action:@selector(clearSource:)], _srcFiles]);
    _srcNote = [self hint:@""];

    NSScrollView *scroll;
    _srcView = lineView(&scroll);
    __weak DebuggerWindow *weakSelf = self;
    _srcView.onClickLine = ^(int line, NSInteger clicks) {
        (void)clicks;
        DebuggerWindow *strongSelf = weakSelf;
        if (!strongSelf || strongSelf->_srcShownFile < 0)
            return;
        const int result = cocodebug_source_breakpoint_toggle(strongSelf->_dbg, strongSelf->_srcShownFile, line + 1);
        if (result == -2)
            [strongSelf message:[NSString stringWithFormat:@"Line %d has no code", line + 1] console:NO];
        else if (result < 0)
            [strongSelf message:@"The breakpoint table is full" console:NO];
        strongSelf->_srcDirty = YES;
        [strongSelf refreshSource];
        if ([strongSelf tabVisible:TAB_DISASM])
            [strongSelf refreshDisasm];
        if ([strongSelf tabVisible:TAB_BREAKS])
            [strongSelf refreshBps];
    };

    NSScrollView *localsScroll;
    _locals = monoView(&localsScroll, NO);
    [localsScroll.heightAnchor constraintEqualToConstant:130].active = YES;
    NSTextField *hint = [self hint:@"Click a line to toggle a breakpoint. Step Line (F6) and Step Over Line (Shift+F6) "
                                   @"go by C statement. A program built with `cmoc -i` keeps the listing and map."];

    NSStackView *page = vstack(@[bar, _srcNote, scroll, [self heading:@"Locals"], localsScroll, hint]);
    fill(page, bar);
    fill(page, _srcNote);
    fill(page, scroll);
    fill(page, localsScroll);
    [scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    return page;
}

/* The Video tab: a controller (core cocovidtab.h) behind its own views. */
- (NSView *)buildVideo
{
    _video = [[CocoVideoTab alloc] initWithDebugger:_dbg];
    return _video.view;
}

/* One table cell: a fixed-width monospace label, or (width 0) the flexible
 * one that takes what is left. */
static NSTextField *tableCell(NSString *text, CGFloat width, BOOL bold)
{
    NSTextField *label = [NSTextField labelWithString:text];
    label.font = bold ? [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightSemibold] : monoFont();
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    if (width > 0) {
        [label.widthAnchor constraintEqualToConstant:width].active = YES;
    } else {
        [label setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
        [label setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                        forOrientation:NSLayoutConstraintOrientationHorizontal];
    }
    return label;
}

/* A stack of rows that scrolls: the document view is pinned to the top and to
 * the clip view's width so rows keep the tab's width. */
static NSScrollView *rowsScroll(NSStackView *rows)
{
    rows.translatesAutoresizingMaskIntoConstraints = NO;
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    scroll.drawsBackground = NO;
    scroll.documentView = rows;
    NSClipView *clip = scroll.contentView;
    [rows.topAnchor constraintEqualToAnchor:clip.topAnchor].active = YES;
    [rows.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor].active = YES;
    [rows.widthAnchor constraintEqualToAnchor:clip.widthAnchor].active = YES;
    return scroll;
}

- (NSView *)buildBreaks
{
    _bpStop = [self label:@""];
    _bpStop.textColor = NSColor.secondaryLabelColor;
    NSStackView *header = hstack(@[tableCell(@"On  #", 70, YES), tableCell(@"Breakpoint", 0, YES),
                                   tableCell(@"Hits", 60, YES), tableCell(@"", 70, YES)]);
    header.distribution = NSStackViewDistributionFill;
    _bpList = vstack(@[]);
    NSScrollView *bpScroll = rowsScroll(_bpList);

    _bpType = [[NSPopUpButton alloc] init];
    [_bpType addItemsWithTitles:@[@"Execute", @"Read", @"Write", @"Access"]];
    _bpType.target = self;
    _bpType.action = @selector(bpTypeChanged:);
    _bpPhys = [NSButton checkboxWithTitle:@"PHYS" target:self action:@selector(bpTypeChanged:)];
    _bpPhys.toolTip = @"CoCo 3, Read/Write/Access only: the range is physical RAM addresses";
    _bpPhys.enabled = NO;
    _bpStart = [self field:@"$A1CB, lo-hi, symbol or expression" width:250 action:@selector(addBreakpoint:)];
    _bpEnd = [self field:@"end (optional)" width:120 action:@selector(addBreakpoint:)];
    _bpCond = [self field:@"condition (optional): value != old" width:0 action:@selector(addBreakpoint:)];
    NSButton *add = [self button:@"Add" action:@selector(addBreakpoint:)];
    NSButton *clearHits = [self button:@"Clear Hits" action:@selector(clearHits:)];
    NSButton *clear = [self button:@"Clear Breakpoints" action:@selector(clearBreaks:)];
    NSStackView *row = hstack(@[_bpType, _bpPhys, [self label:@"Start"], _bpStart, [self label:@"End"], _bpEnd,
                                [self label:@"Condition"], _bpCond, add, clearHits, clear]);
    row.distribution = NSStackViewDistributionFill;
    _bpErr = [self label:@""];
    _bpErr.textColor = NSColor.systemRedColor;
    NSTextField *hint = [self hint:@"Untick to disable. A condition is a C expression: registers, flags, [addr], symbols; "
                                   @"on a Read/Write/Access break value, addr and old work too (value != old). "
                                   @"The end defaults to the start."];

    NSStackView *page = vstack(@[_bpStop, header, bpScroll, hint, row, _bpErr]);
    fill(page, header);
    fill(page, bpScroll);
    fill(page, row);
    [bpScroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    return page;
}

/* Watches: an entry, then a row for each with its value and three buttons. */
- (NSView *)buildWatches
{
    _wtSpace = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_wtSpace addItemsWithTitles:@[@"CPU", @"PHYS"]];
    _wtSpace.toolTip = @"The space the watch reads (PHYS: CoCo 3 physical RAM)";
    [_wtSpace setRefusesFirstResponder:YES];
    _wtEntry = [self field:@"address, symbol, expression or lo-hi" width:0 action:@selector(addWatch:)];
    NSStackView *row = hstack(@[[self label:@"Watch"], _wtSpace, _wtEntry, [self button:@"Add" action:@selector(addWatch:)]]);
    row.distribution = NSStackViewDistributionFill;

    _wtList = vstack(@[]);
    NSScrollView *scroll = rowsScroll(_wtList);
    NSTextField *hint = [self hint:@"Values show while the machine is stopped. Show in Memory shows the watch in the Memory "
                                   @"tab; Break on Change stops when the machine writes a different value there."];
    NSStackView *page = vstack(@[row, scroll, hint]);
    fill(page, row);
    fill(page, scroll);
    [scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    return page;
}

/* The cartridge, the memory map, the PIAs, the SAM, the GIME and the
 * joysticks in one text. */
- (NSView *)buildCart
{
    NSScrollView *scroll;
    _cartText = monoView(&scroll, NO);
    return scroll;
}

- (NSView *)buildSound
{
    NSScrollView *scroll;
    _soundText = monoView(&scroll, NO);
    return scroll;
}

/* Trace (idx 0) and History (idx 1): two views of the engine's one ring. */
- (NSView *)buildHist:(int)idx
{
    _hRec[idx] = [self button:@"Record" action:@selector(histRec:)];
    [_hRec[idx] setButtonType:NSButtonTypePushOnPushOff];
    _hRec[idx].toolTip = @"Record every instruction the CPU runs (the machine runs a little slower)";
    _hFilter[idx] = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_hFilter[idx] addItemsWithTitles:@[@"All instructions", @"Branches only", @"In range"]];
    _hFilter[idx].target = self;
    _hFilter[idx].action = @selector(histChanged:);
    [_hFilter[idx] setRefusesFirstResponder:YES];
    _hRange[idx] = [self field:@"range: lo-hi or a symbol" width:180 action:@selector(histChanged:)];
    _hNote[idx] = [self hint:@""];
    NSStackView *bar = hstack(@[_hRec[idx], _hFilter[idx], _hRange[idx],
                                [self button:@"Clear" action:@selector(histClear:)], _hNote[idx]]);
    NSScrollView *scroll;
    _hText[idx] = monoView(&scroll, NO);
    NSStackView *page = vstack(@[bar, scroll]);
    fill(page, bar);
    fill(page, scroll);
    return page;
}

/* Symbols: a filter over the table (built-ins and loaded). A click selects a
 * row, a double-click (or Show in Disassembly) goes to its address. */
- (NSView *)buildSymbols
{
    _symFilter = [self field:@"Filter by name (Enter to apply)" width:260 action:@selector(symFilterChanged:)];
    _symCount = [self hint:@""];
    NSStackView *bar = hstack(@[[self label:@"Filter"], _symFilter, _symCount,
                                [self button:@"Show in Disassembly" action:@selector(symShow:)],
                                [self button:@"Break at" action:@selector(symBreak:)],
                                [self button:@"Load Symbols…" action:@selector(loadSymbols:)],
                                [self button:@"Clear Symbols" action:@selector(clearSymbols:)]]);
    _symName = [self field:@"label name" width:180 action:@selector(defineSymbol:)];
    _symAddr = [self field:@"address" width:100 action:@selector(defineSymbol:)];
    NSStackView *define = hstack(@[[self label:@"Define"], _symName, [self label:@"="], _symAddr,
                                   [self button:@"Set Label" action:@selector(defineSymbol:)]]);

    NSScrollView *scroll;
    _symList = lineView(&scroll);
    __weak DebuggerWindow *weakSelf = self;
    _symList.onClickLine = ^(int line, NSInteger clicks) {
        DebuggerWindow *strongSelf = weakSelf;
        if (!strongSelf || line < 0 || line >= (int)strongSelf->_symAddrs.count)
            return;
        strongSelf->_symSelected = line;
        if (clicks >= 2)
            [strongSelf showDisassemblyAt:(uint16_t)strongSelf->_symAddrs[(NSUInteger)line].intValue];
        else
            [strongSelf refreshSymbols];
    };

    NSTextField *hint = [self hint:@"Click a symbol, then Show in Disassembly (or double-click it) or Break at. Load Symbols "
                                   @"reads an LWASM --symbol-dump file; Clear Symbols drops what was loaded and keeps the "
                                   @"built-in Color BASIC entry points."];
    NSStackView *page = vstack(@[bar, scroll, define, hint]);
    fill(page, bar);
    fill(page, scroll);
    return page;
}

/* The DriveWire packets left (a note, Hide READY probes and Clear over them),
 * the Becker port's state right. */
- (NSView *)buildFujinet
{
    _fujiNote = [self label:@""];
    _fujiNote.textColor = NSColor.secondaryLabelColor;
    _fujiNote.lineBreakMode = NSLineBreakByTruncatingTail;
    [_fujiNote setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_fujiNote setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                        forOrientation:NSLayoutConstraintOrientationHorizontal];
    _fujiHideReady = [NSButton checkboxWithTitle:@"Hide READY probes" target:self action:@selector(fujiHideReady:)];
    _fujiHideReady.state = NSControlStateValueOn;
    _fujiHideReady.toolTip = @"fujinet-lib asks E2 00 before every call";
    NSStackView *row = hstack(@[_fujiNote, _fujiHideReady, [self button:@"Clear" action:@selector(fujiClear:)]]);
    row.distribution = NSStackViewDistributionFill;

    NSScrollView *fujiScroll, *stateScroll;
    _fujiText = monoView(&fujiScroll, NO);
    _fujiState = monoView(&stateScroll, NO);
    NSStackView *left = vstack(@[[self heading:@"DriveWire packets"], row, fujiScroll]);
    left.distribution = NSStackViewDistributionFill;   /* the log takes the column's height */
    fill(left, row);
    fill(left, fujiScroll);
    NSStackView *right = vstack(@[[self heading:@"Becker port"], stateScroll]);
    right.distribution = NSStackViewDistributionFill;
    fill(right, stateScroll);

    NSStackView *page = hstack(@[left, right]);
    page.alignment = NSLayoutAttributeTop;
    page.spacing = 12;
    [left.widthAnchor constraintGreaterThanOrEqualToConstant:520].active = YES;
    [right.widthAnchor constraintEqualToConstant:340].active = YES;
    [left.heightAnchor constraintEqualToAnchor:page.heightAnchor].active = YES;
    [right.heightAnchor constraintEqualToAnchor:page.heightAnchor].active = YES;
    return page;
}

- (void)buildWindow
{
    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 1180, 860)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"Debugger";
    _window.releasedWhenClosed = NO;
    _window.delegate = self;

    /* Toolbar rows. The keys are handled by the key monitor, not as the
     * buttons' key equivalents, so they work wherever the focus is. */
    _runBtn = [self button:@"Pause (F5)" action:@selector(toggleRun:)];
    NSButton *stepBtn = [self button:@"Step (F7)" action:@selector(step:)];
    NSButton *overBtn = [self button:@"Step Over (F8)" action:@selector(stepOver:)];
    NSButton *outBtn = [self button:@"Step Out (⇧F8)" action:@selector(stepOut:)];
    _stepLineBtn = [self button:@"Step Line (F6)" action:@selector(stepLine:)];
    _stepOverLineBtn = [self button:@"Step Over Line (⇧F6)" action:@selector(stepOverLine:)];
    NSButton *clearBps = [self button:@"Clear Breakpoints" action:@selector(clearBreaks:)];
    NSButton *popBtn = [self button:@"Pop Out" action:@selector(popOutCurrent:)];
    popBtn.toolTip = @"Move the tab that shows into a window of its own; Dock puts it back";
    NSButton *helpBtn = CocoHelpButton(nil);
    [helpBtn setRefusesFirstResponder:YES];
    NSStackView *toolbar = hstack(@[_runBtn, stepBtn, overBtn, outBtn, _stepLineBtn, _stepOverLineBtn, clearBps, popBtn,
                                    helpBtn]);

    _breakAt = [self field:@"symbol or hex address" width:170 action:@selector(breakAt:)];
    NSButton *breakBtn = [self button:@"Break" action:@selector(breakAt:)];
    breakBtn.toolTip = @"Set an execute breakpoint at the symbol or address";
    NSStackView *toolbar2 = hstack(@[[self label:@"Break at:"], _breakAt, breakBtn,
                                     [self button:@"Load Symbols…" action:@selector(loadSymbols:)],
                                     [self button:@"Clear Symbols" action:@selector(clearSymbols:)],
                                     [self button:@"Load C Source…" action:@selector(loadSource:)],
                                     [self button:@"Clear C Source" action:@selector(clearSource:)]]);

    /* the stop reason gets a row of its own, so it is never cut short */
    _status = [self label:@"Running"];
    _status.textColor = NSColor.secondaryLabelColor;
    _status.lineBreakMode = NSLineBreakByTruncatingTail;
    [_status setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                      forOrientation:NSLayoutConstraintOrientationHorizontal];

    _tabs = [[NSTabView alloc] init];
    NSArray *pages = @[ [self buildConsole], [self buildDisasm], [self buildSource], [self buildMemory],
                        [self buildBreaks], [self buildWatches], [self buildCart], [self buildHist:0],
                        [self buildHist:1], [self buildVideo], [self buildSound], [self buildSymbols],
                        [self buildFujinet] ];
    for (int tab = 0; tab < NUM_TABS; tab++) {
        NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:kHelpTabs[tab]];
        item.label = kTabTitles[tab];
        NSView *content = pages[(NSUInteger)tab];
        NSView *holder = [[NSView alloc] init];
        [holder addSubview:content];
        content.translatesAutoresizingMaskIntoConstraints = NO;
        [content.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor constant:6].active = YES;
        [content.trailingAnchor constraintEqualToAnchor:holder.trailingAnchor constant:-6].active = YES;
        [content.topAnchor constraintEqualToAnchor:holder.topAnchor constant:6].active = YES;
        [content.bottomAnchor constraintEqualToAnchor:holder.bottomAnchor constant:-6].active = YES;
        item.view = holder;
        _items[tab] = item;
        [_tabs addTabViewItem:item];
    }
    const int first = tabFromText(getenv("COCO_DEBUGGER_TAB"));
    if (first >= 0 && first < (int)_tabs.numberOfTabViewItems)
        [_tabs selectTabViewItemAtIndex:first];
    _tabs.delegate = self;
    [self updateHelpContext];

    /* the registers sit under every tab */
    NSView *strip = [self buildRegStrip];
    NSStackView *root = vstack(@[toolbar, toolbar2, _status, _tabs, strip]);
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [toolbar.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [toolbar2.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [_status.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [_tabs.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [strip.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    _window.contentView = root;
    [_window center];
}

/* ---- actions ------------------------------------------------------------------ */

/* A line for the status label (and the console's log, when asked). */
- (void)message:(NSString *)text console:(BOOL)console
{
    _status.stringValue = text;
    if (console)
        [self appendConsole:[text stringByAppendingString:@"\n"]];
}

- (void)toggleRun:(id)sender
{
    (void)sender;
    const unsigned gen = cocodebug_generation(_dbg);
    if (cocodebug_is_paused(_dbg))
        cocodebug_resume(_dbg);
    else
        cocodebug_pause(_dbg);
    [self moved:gen];
}

/* An action that moves the machine: the stop settles (or the poll catches
 * it, for a long run), then everything shown is read back. */
- (void)moved:(unsigned)gen
{
    [self settleFrom:gen];
    [self refreshNow];
}

- (void)step:(id)sender
{
    (void)sender;
    const unsigned gen = cocodebug_generation(_dbg);
    cocodebug_step(_dbg);
    [self moved:gen];
}

- (void)stepOver:(id)sender
{
    (void)sender;
    const unsigned gen = cocodebug_generation(_dbg);
    cocodebug_step_over(_dbg);
    [self moved:gen];
}

- (void)stepOut:(id)sender
{
    (void)sender;
    const unsigned gen = cocodebug_generation(_dbg);
    cocodebug_step_out(_dbg);
    [self moved:gen];
}

- (void)stepLine:(id)sender
{
    (void)sender;
    const unsigned gen = cocodebug_generation(_dbg);
    cocodebug_step_line(_dbg);
    [self moved:gen];
}

- (void)stepOverLine:(id)sender
{
    (void)sender;
    const unsigned gen = cocodebug_generation(_dbg);
    cocodebug_step_over_line(_dbg);
    [self moved:gen];
}

- (void)clearBreaks:(id)sender
{
    (void)sender;
    cocodebug_breakpoint_clear_all(_dbg);
    _srcDirty = YES;
    [self refreshNow];
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

- (void)appendConsole:(NSString *)text
{
    const BOOL follow = atEnd(_conOut);     /* a reader scrolled back keeps their place */
    [_conOut.textStorage appendAttributedString:[[NSAttributedString alloc] initWithString:text attributes:monoAttrs()]];
    if (follow)
        [_conOut scrollRangeToVisible:NSMakeRange(_conOut.string.length, 0)];
}

- (void)runCommand:(id)sender
{
    (void)sender;
    static char out[65536];
    NSString *command = trimmed(_conIn.stringValue);
    if (command.length == 0)
        return;
    if (![_history.lastObject isEqualToString:command])
        [_history addObject:command];
    _historyPos = (NSInteger)_history.count;
    [self appendConsole:[NSString stringWithFormat:@"> %@\n", command]];
    out[0] = '\0';
    const unsigned gen = cocodebug_generation(_dbg);
    cocodebug_command(_dbg, command.UTF8String, out, (int)sizeof out);
    NSString *text = cstr(out);
    if (text.length)
        [self appendConsole:[text hasSuffix:@"\n"] ? text : [text stringByAppendingString:@"\n"]];
    _conIn.stringValue = @"";
    _srcDirty = YES;
    [self settleFrom:gen];
    [self refreshNow];
}

/* In the console, Tab completes instead of moving the focus, and Up/Down
 * walk the history. */
- (BOOL)control:(NSControl *)control textView:(NSTextView *)textView doCommandBySelector:(SEL)sel
{
    (void)textView;
    if (control != _conIn)
        return NO;
    if (sel == @selector(moveUp:) || sel == @selector(moveDown:)) {
        const NSInteger count = (NSInteger)_history.count;
        if (count == 0)
            return YES;
        _historyPos += sel == @selector(moveUp:) ? -1 : 1;
        if (_historyPos < 0)
            _historyPos = 0;
        if (_historyPos > count)
            _historyPos = count;
        _conIn.stringValue = _historyPos < count ? _history[(NSUInteger)_historyPos] : @"";
        [_conIn.currentEditor setSelectedRange:NSMakeRange(_conIn.stringValue.length, 0)];
        return YES;
    }
    if (sel != @selector(insertTab:))
        return NO;
    NSString *text = _conIn.stringValue;
    const NSRange space = [text rangeOfString:@" " options:NSBackwardsSearch];
    NSString *word = space.location == NSNotFound ? text : [text substringFromIndex:space.location + 1];
    char completions[4096];
    completions[0] = '\0';
    const int count = cocodebug_completions(_dbg, word.UTF8String, completions, (int)sizeof completions);
    NSString *list = cstr(completions);
    if (count == 1) {
        NSString *completion = [list componentsSeparatedByString:@"\n"][0];
        NSString *head = space.location == NSNotFound ? @"" : [text substringToIndex:space.location + 1];
        _conIn.stringValue = [NSString stringWithFormat:@"%@%@ ", head, completion];
        [_conIn.currentEditor setSelectedRange:NSMakeRange(_conIn.stringValue.length, 0)];
    } else if (count > 1) {
        if (![list hasSuffix:@"\n"])
            list = [list stringByAppendingString:@"\n"];
        [self appendConsole:list];
    }
    return YES;
}

/* An open panel, no content-type filter: LWASM's --symbol-dump and CMOC's .lst
 * have no registered type, and leaving allowedContentTypes unset (rather than
 * an empty array, which would allow nothing) is what shows every file. */
- (void)loadSymbols:(id)sender
{
    (void)sender;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.title = @"Load Symbols";
    if ([panel runModal] != NSModalResponseOK || !panel.URL)
        return;
    const int loaded = cocodebug_symbols_load(_dbg, panel.URL.path.UTF8String);
    if (loaded < 0)
        [self message:@"Could not open symbol file" console:YES];
    else
        [self message:[NSString stringWithFormat:@"Loaded %d symbol%s", loaded, loaded == 1 ? "" : "s"] console:YES];
    [self refreshNow];
}

- (void)clearSymbols:(id)sender
{
    (void)sender;
    cocodebug_symbols_clear(_dbg);
    [self message:@"Loaded symbols cleared" console:YES];
    [self refreshNow];
}

/* One or more .lst files, one per module of a multi-file CMOC program --
 * cocodebug_source_load() adds to what is already loaded rather than
 * replacing it (cmoc names the .map after the linked executable, not any one
 * .lst, so every module's listing wants the very .map its neighbours do), so
 * loading them all here in one panel is exactly the multi-file case. */
- (void)loadSource:(id)sender
{
    (void)sender;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.title = @"Load C Source";
    panel.allowsMultipleSelection = YES;
    if ([panel runModal] != NSModalResponseOK || panel.URLs.count == 0)
        return;
    int loaded = 0, failed = 0;
    for (NSURL *url in panel.URLs) {
        if (cocodebug_source_load(_dbg, url.path.UTF8String) < 0)
            failed++;
        else
            loaded++;
    }
    if (failed && !loaded)
        [self message:@"Could not open listing or its .map (both need `cmoc -i`)" console:YES];
    else if (failed)
        [self message:@"Some listings could not be opened" console:YES];
    else
        [self message:[NSString stringWithFormat:@"Loaded %d listing%s", loaded, loaded == 1 ? "" : "s"] console:YES];
    _srcCachedPath = nil;       /* the file list may have changed */
    _srcShownFile = -1;
    _srcDirty = YES;
    [self refreshNow];
}

- (void)clearSource:(id)sender
{
    (void)sender;
    cocodebug_source_clear(_dbg);
    _srcCachedPath = nil;
    _srcCachedLines = nil;
    _pcFile = -1;
    _pcLine = -1;
    _srcShownFile = -1;
    _srcDirty = YES;
    [self refreshNow];
}

- (void)srcFileChosen:(id)sender
{
    (void)sender;
    _srcShownFile = (int)_srcFiles.indexOfSelectedItem;
    _srcDirty = YES;
    [self refreshSource];
}

/* A Symbols-tab edit: name = address. */
- (void)defineSymbol:(id)sender
{
    (void)sender;
    NSString *name = trimmed(_symName.stringValue);
    NSString *why = nil;
    const int address = [self resolveAddr:_symAddr.stringValue why:&why];
    if (name.length == 0 || address < 0) {
        [self message:@"Define: a label name and an address" console:NO];
        return;
    }
    cocodebug_symbol_add(_dbg, (uint16_t)address, name.UTF8String);
    _symName.stringValue = @"";
    _symAddr.stringValue = @"";
    [self message:[NSString stringWithFormat:@"%@ = $%04X", name, address] console:NO];
    [self refreshNow];
}

- (void)applyRegister:(NSTextField *)sender
{
    long value;
    NSString *why = nil;
    [_window makeFirstResponder:nil];       /* leave the field, so the refresh may rewrite it */
    if (!cocodebug_is_paused(_dbg)) {
        [self message:@"Pause the machine (F5) to edit registers" console:NO];
    } else if (sender.tag < 0 || sender.tag >= NUM_REG_FIELDS) {
        /* not one of ours */
    } else if ([self evaluate:sender.stringValue into:&value why:&why]) {
        if (!cocodebug_regs_set(_dbg, kRegIds[sender.tag], (unsigned)value))
            [self message:[NSString stringWithFormat:@"%@ cannot be set", kRegNames[sender.tag]] console:NO];
    } else {
        [self message:[NSString stringWithFormat:@"Register: %@", why] console:NO];
    }
    [self refreshCpu];
    _srcDirty = YES;
    if ([self tabVisible:TAB_DISASM])
        [self refreshDisasm];
    if ([self tabVisible:TAB_SOURCE])
        [self refreshSource];
}

- (void)flagClicked:(NSButton *)sender
{
    if (sender.tag >= 0 && sender.tag < NUM_FLAGS && cocodebug_is_paused(_dbg))
        cocodebug_flag_set(_dbg, kFlagMasks[sender.tag], sender.state == NSControlStateValueOn);
    else
        [self message:@"Pause the machine (F5) to edit flags" console:NO];
    [self refreshCpu];
}

/* ---- Memory ----------------------------------------------------------------- */

- (uint32_t)spaceSize:(int)space
{
    if (space == COCODEBUG_SPACE_PHYS)
        return _physSize;
    return 0x10000;
}

/* Brings a range in space into the dump (a couple of rows down from the top)
 * and highlights it; the space chooser follows. */
- (void)showSpace:(int)space low:(uint32_t)low high:(uint32_t)high
{
    if (space == COCODEBUG_SPACE_PHYS && _physSize == 0)
        space = COCODEBUG_SPACE_CPU;        /* no physical RAM on a CoCo 1/2 */
    if (high < low)
        high = low;
    if ((int)_memSpace.indexOfSelectedItem != space)
        [_memSpace selectItemAtIndex:space];
    const uint32_t row = low & ~0xFu;
    _memBase[space] = row >= 2 * 16 ? row - 2 * 16 : 0;
    _memHl = YES;
    _memHlSpace = space;
    _memHlLo = low;
    _memHlHi = high;
    [self refreshMem];
}

- (void)memSpaceChanged:(id)sender
{
    (void)sender;
    if ([self memSpaceIndex] == COCODEBUG_SPACE_PHYS && _physSize == 0) {
        [_memSpace selectItemAtIndex:COCODEBUG_SPACE_CPU];
        [self message:@"PHYS needs a CoCo 3 (a CoCo 1/2 has no physical RAM space)" console:NO];
    }
    [_wtSpace selectItemAtIndex:[self memSpaceIndex]];
    [self refreshMem];
}

- (void)scrollMemBy:(int)bytes
{
    const int space = [self memSpaceIndex];
    const long target = (long)_memBase[space] + bytes;
    _memBase[space] = target < 0 ? 0 : (uint32_t)target;
    [self refreshMem];
}

- (void)gotoMem:(id)sender
{
    (void)sender;
    const int space = [self memSpaceIndex];
    uint32_t low, high;
    if (!cocosym_parse_range(_dbg, space, _memGoto.stringValue.UTF8String, &low, &high)) {
        [self message:[NSString stringWithFormat:@"Go to %@: not an address, symbol, expression or range",
                                                 _memGoto.stringValue]
              console:NO];
        return;
    }
    [self showSpace:space low:low high:high];
}

- (void)memPrev:(id)sender
{
    (void)sender;
    [self scrollMemBy:-MEM_BYTES];
}

- (void)memNext:(id)sender
{
    (void)sender;
    [self scrollMemBy:MEM_BYTES];
}

/* "address value [value...]": the values land at consecutive addresses of the
 * chosen space; ROM and I/O bytes are skipped by the engine. */
- (void)writeMem:(id)sender
{
    (void)sender;
    const int space = [self memSpaceIndex];
    NSMutableArray<NSString *> *tokens = [NSMutableArray array];
    for (NSString *word in [_memWrite.stringValue componentsSeparatedByCharactersInSet:
                                [NSCharacterSet whitespaceCharacterSet]])
        if (word.length)
            [tokens addObject:word];
    uint32_t first = 0, last;
    if (tokens.count < 2 || !cocosym_parse_range(_dbg, space, tokens[0].UTF8String, &first, &last)) {
        [self message:[NSString stringWithFormat:@"Poke: address value [value...] in %s", cocomem_space_name(space)]
              console:NO];
        return;
    }
    uint8_t bytes[64];
    int count = 0;
    for (NSUInteger index = 1; index < tokens.count && count < (int)sizeof bytes; index++) {
        long value;
        NSString *why = nil;
        if (![self evaluate:tokens[index] into:&value why:&why] || value < 0 || value > 0xFF) {
            [self message:[NSString stringWithFormat:@"Not a byte: %@", tokens[index]] console:NO];
            return;
        }
        bytes[count++] = (uint8_t)value;
    }
    if (!cocodebug_is_paused(_dbg)) {
        [self message:@"Pause the machine (F5) to poke memory" console:NO];
        return;
    }
    const int stored = cocodebug_mem_write(_dbg, space, first, bytes, count);
    if (stored < count)
        [self message:[NSString stringWithFormat:@"Stored %d of %d bytes (ROM and I/O are skipped)", stored, count]
              console:NO];
    _memWrite.stringValue = @"";
    [self showSpace:space low:first high:first + (uint32_t)(count > 0 ? count - 1 : 0)];
}

- (void)searchBackward:(BOOL)backward
{
    uint8_t pattern[COCOSEARCH_MAX], mask = 0xFF;
    int length = 0, flags = 0;
    const int space = [self memSpaceIndex];
    if (!cocosearch_parse((int)_srchKind.indexOfSelectedItem == 1 ? COCOSEARCH_TEXT : COCOSEARCH_HEX,
                          _srchEntry.stringValue.UTF8String, pattern, &length, &mask, &flags) ||
        length <= 0) {
        _srchNote.stringValue = @"Type hex bytes (8E 04) or some text.";
        return;
    }
    if (!cocodebug_is_paused(_dbg)) {
        _srchNote.stringValue = @"Pause the machine first.";
        return;
    }
    uint32_t from = _memBase[space];
    if (_memHl && _memHlSpace == space)
        from = backward ? _memHlLo : _memHlLo + 1;
    uint32_t found = 0;
    const int result = cocodebug_search_next(_dbg, space, pattern, length, mask, flags, from, backward ? 1 : 0, &found);
    if (result < 0) {
        _srchNote.stringValue = @"Pause the machine first.";
    } else if (result == 0) {
        _srchNote.stringValue = [NSString stringWithFormat:@"Not found in %s.", cocomem_space_name(space)];
    } else {
        _srchNote.stringValue = [NSString stringWithFormat:@"Found at $%04X", found];
        [self showSpace:space low:found high:found + (uint32_t)length - 1];
    }
}

- (void)searchNext:(id)sender
{
    (void)sender;
    [self searchBackward:NO];
}

- (void)searchPrev:(id)sender
{
    (void)sender;
    [self searchBackward:YES];
}

/* ---- Disassembly -------------------------------------------------------------- */

- (void)followToggled:(id)sender
{
    (void)sender;
    _scrolledAway = NO;
    [self refreshDisasm];
}

- (void)jumpTo:(id)sender
{
    (void)sender;
    NSString *why = nil;
    const int addr = [self resolveAddr:_jump.stringValue why:&why];
    if (addr < 0) {
        [self message:[NSString stringWithFormat:@"Jump to %@: %@", _jump.stringValue, why] console:NO];
        return;
    }
    [self showDisassemblyAt:(uint16_t)addr];
}

/* The Disassembly from an address on (off the PC, until the next stop). */
- (void)showDisassemblyAt:(uint16_t)addr
{
    _disasmAddr = addr;
    _followPc.state = NSControlStateValueOff;
    _scrolledAway = YES;
    [self showTab:TAB_DISASM];
    [self refreshDisasm];
}

/* The toolbar's "Break at:": a symbol or hex address gets an execute
 * breakpoint (one already there is left as it is). */
- (void)breakAt:(id)sender
{
    (void)sender;
    NSString *why = nil;
    const int addr = [self resolveAddr:_breakAt.stringValue why:&why];
    if (addr < 0) {
        [self message:[NSString stringWithFormat:@"Break at %@: %@", _breakAt.stringValue, why] console:NO];
        return;
    }
    [self breakExecuteAt:(uint16_t)addr];
    _breakAt.stringValue = @"";
}

- (void)breakExecuteAt:(uint16_t)addr
{
    if (cocodebug_breakpoint_is_set(_dbg, addr)) {
        [self message:[NSString stringWithFormat:@"A breakpoint is already set at $%04X", addr] console:NO];
        return;
    }
    if (cocodebug_breakpoint_toggle(_dbg, addr) < 0)
        [self message:@"The breakpoint table is full" console:NO];
    else
        [self message:[NSString stringWithFormat:@"Breakpoint at $%04X", addr] console:NO];
    [self refreshNow];
}

/* ---- Symbols ------------------------------------------------------------------ */

- (void)symFilterChanged:(id)sender
{
    (void)sender;
    _symSelected = -1;
    [self refreshSymbols];
}

- (void)symShow:(id)sender
{
    (void)sender;
    if (_symSelected < 0 || _symSelected >= (int)_symAddrs.count) {
        [self message:@"Click a symbol first" console:NO];
        return;
    }
    [self showDisassemblyAt:(uint16_t)_symAddrs[(NSUInteger)_symSelected].intValue];
}

- (void)symBreak:(id)sender
{
    (void)sender;
    if (_symSelected < 0 || _symSelected >= (int)_symAddrs.count) {
        [self message:@"Click a symbol first" console:NO];
        return;
    }
    [self breakExecuteAt:(uint16_t)_symAddrs[(NSUInteger)_symSelected].intValue];
}

/* ---- Breakpoints --------------------------------------------------------------- */

- (void)addBreakpoint:(id)sender
{
    (void)sender;
    static const int types[4] = { COCODEBUG_BP_EXEC, COCODEBUG_BP_READ, COCODEBUG_BP_WRITE, COCODEBUG_BP_ACCESS };
    const NSInteger sel = _bpType.indexOfSelectedItem;
    int type = types[sel >= 0 && sel < 4 ? sel : 0];
    const BOOL phys = type != COCODEBUG_BP_EXEC && _bpPhys.state == NSControlStateValueOn;
    const int space = phys ? COCODEBUG_SPACE_PHYS : COCODEBUG_SPACE_CPU;
    if (phys)
        type |= COCODEBUG_BP_PHYS;
    NSString *startText = trimmed(_bpStart.stringValue);
    NSString *endText = trimmed(_bpEnd.stringValue);
    if (startText.length == 0) {
        _bpErr.stringValue = @"Give an address, symbol, expression or lo-hi range";
        return;
    }
    uint32_t start = 0, end = 0;
    BOOL valid;
    if (endText.length) {
        uint32_t ignored;
        valid = cocosym_parse_range(_dbg, space, startText.UTF8String, &start, &ignored) &&
                cocosym_parse_range(_dbg, space, endText.UTF8String, &end, &ignored);
    } else {
        /* one field: a number, a symbol, an expression or a range lo-hi */
        valid = cocosym_parse_range(_dbg, space, startText.UTF8String, &start, &end);
    }
    if (!valid || end < start) {
        _bpErr.stringValue = [NSString stringWithFormat:@"Bad range: %@%@%@ (%s space)", startText,
                                                         endText.length ? @" - " : @"", endText,
                                                         cocomem_space_name(space)];
        return;
    }
    const int ident = cocodebug_breakpoint_add(_dbg, type, start, end, trimmed(_bpCond.stringValue).UTF8String);
    if (ident < 0) {
        _bpErr.stringValue = @"Refused: the table is full, or the condition does not parse";
        [self message:@"The breakpoint was refused" console:NO];
        return;
    }
    _bpErr.stringValue = @"";
    _bpStart.stringValue = @"";
    _bpEnd.stringValue = @"";
    _bpCond.stringValue = @"";
    _srcDirty = YES;
    [self refreshNow];
}

- (void)bpEnabled:(NSButton *)sender
{
    cocodebug_breakpoint_enable(_dbg, (int)sender.tag, sender.state == NSControlStateValueOn);
    _srcDirty = YES;
    [self refreshNow];
}

- (void)bpRemove:(NSButton *)sender
{
    cocodebug_breakpoint_remove(_dbg, (int)sender.tag);
    _srcDirty = YES;
    [self refreshNow];
}

/* PHYS applies to Read/Write/Access on a CoCo 3 only. */
- (void)bpTypeChanged:(id)sender
{
    (void)sender;
    const BOOL access = _bpType.indexOfSelectedItem > 0;
    _bpPhys.enabled = access && _physSize > 0;
    if (!_bpPhys.enabled)
        _bpPhys.state = NSControlStateValueOff;
}

- (void)clearHits:(id)sender
{
    (void)sender;
    cocodebug_breakpoint_clear_hits(_dbg);
    _bpSig = nil;       /* the hit counts redraw */
    [self refreshNow];
}

/* ---- Watches, trace, pop out ---------------------------------------------------- */

- (void)addWatch:(id)sender
{
    (void)sender;
    NSString *text = trimmed(_wtEntry.stringValue);
    const int space = (int)_wtSpace.indexOfSelectedItem == COCODEBUG_SPACE_PHYS && _physSize > 0
                          ? COCODEBUG_SPACE_PHYS
                          : COCODEBUG_SPACE_CPU;
    if (text.length == 0 || cocowatch_add(text.UTF8String, space) < 0) {
        [self message:@"Type an address, symbol, expression or lo-hi range to watch" console:NO];
        return;
    }
    _wtEntry.stringValue = @"";
    [self refreshWatches];
    if ([self tabVisible:TAB_MEMORY])
        [self refreshMem];
}

/* A Write breakpoint over low..high that fires when the value changes. */
- (void)breakOnChangeSpace:(int)space low:(uint32_t)low high:(uint32_t)high
{
    const int type = COCODEBUG_BP_WRITE | (space == COCODEBUG_SPACE_PHYS ? COCODEBUG_BP_PHYS : 0);
    if (cocodebug_breakpoint_add(_dbg, type, low, high, "value != old") < 0) {
        [self message:@"The breakpoint was refused" console:NO];
        return;
    }
    [self message:@"Breaking when it changes" console:NO];
    [self refreshNow];
}

/* Selects a tab, or raises the window it was popped out into. */
- (void)showTab:(int)tab
{
    if (_popped[tab]) {
        [_popped[tab] makeKeyAndOrderFront:nil];
        return;
    }
    [_tabs selectTabViewItem:_items[tab]];
}

- (void)wtMemory:(NSButton *)sender
{
    uint32_t low, high;
    if (!cocowatch_range(_dbg, (int)sender.tag, &low, &high)) {
        [self message:@"That watch does not resolve" console:NO];
        return;
    }
    [self showTab:TAB_MEMORY];
    [self showSpace:cocowatch_space((int)sender.tag) low:low high:high];
}

- (void)wtBreak:(NSButton *)sender
{
    uint32_t low, high;
    if (!cocowatch_range(_dbg, (int)sender.tag, &low, &high)) {
        [self message:@"That watch does not resolve" console:NO];
        return;
    }
    [self breakOnChangeSpace:cocowatch_space((int)sender.tag) low:low high:high];
}

- (void)wtRemove:(NSButton *)sender
{
    cocowatch_remove((int)sender.tag);
    [self refreshWatches];
}

/* Both Record buttons follow the engine's one recorder. */
- (void)syncRecord:(BOOL)active
{
    for (int which = 0; which < 2; which++) {
        const NSInteger want = active ? NSControlStateValueOn : NSControlStateValueOff;
        if (_hRec[which].state != want) {
            _hRec[which].state = want;
            setButtonAccent(_hRec[which], active);
        }
    }
}

- (void)histRec:(NSButton *)sender
{
    cocodebug_trace_enable(_dbg, sender.state == NSControlStateValueOn);
    [self syncRecord:cocodebug_trace_enabled(_dbg) != 0];
    [self refreshTab];
}

- (void)histChanged:(id)sender
{
    (void)sender;
    [self refreshTab];
}

- (void)histClear:(id)sender
{
    (void)sender;
    cocodebug_trace_clear(_dbg);
    [self refreshTab];
}

- (void)popOutCurrent:(id)sender
{
    (void)sender;
    const NSInteger tab = [self currentTab];
    if (tab >= 0)
        [self popOutTab:(int)tab];
}

/* Moves a tab into a window of its own. One tab always stays. */
- (void)popOutTab:(int)tab
{
    if (tab < 0 || tab >= NUM_TABS || _popped[tab] || _tabs.numberOfTabViewItems <= 1)
        return;
    NSTabViewItem *item = _items[tab];
    NSView *view = item.view;
    [_tabs removeTabViewItem:item];

    NSWindow *win = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 820, 600)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    win.title = [@"Debugger - " stringByAppendingString:kTabTitles[tab]];
    win.releasedWhenClosed = NO;
    win.delegate = self;
    NSButton *dockBtn = [self button:@"Dock" action:@selector(dockButton:)];
    dockBtn.tag = tab;
    dockBtn.toolTip = @"Put this tab back in the debugger";
    NSView *root = [[NSView alloc] init];
    dockBtn.translatesAutoresizingMaskIntoConstraints = NO;
    view.translatesAutoresizingMaskIntoConstraints = NO;
    [root addSubview:dockBtn];
    [root addSubview:view];
    [dockBtn.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:8].active = YES;
    [dockBtn.topAnchor constraintEqualToAnchor:root.topAnchor constant:8].active = YES;
    [view.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:8].active = YES;
    [view.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-8].active = YES;
    [view.topAnchor constraintEqualToAnchor:dockBtn.bottomAnchor constant:6].active = YES;
    [view.bottomAnchor constraintEqualToAnchor:root.bottomAnchor constant:-8].active = YES;
    win.contentView = root;
    CocoSetHelpContext(win, [@"debugger/" stringByAppendingString:kHelpTabs[tab]]);
    _popped[tab] = win;
    [win center];
    [win makeKeyAndOrderFront:nil];
    [self updateHelpContext];
    [self refreshTab];
}

- (void)dockButton:(id)sender
{
    [self dockTab:(int)((NSButton *)sender).tag];
}

/* Puts a popped-out tab back where it was in the order. */
- (void)dockTab:(int)tab
{
    if (tab < 0 || tab >= NUM_TABS)
        return;
    NSWindow *win = _popped[tab];
    if (!win)
        return;
    NSTabViewItem *item = _items[tab];
    NSView *view = item.view;
    int position = 0;
    _popped[tab] = nil;
    win.delegate = nil;
    [view removeFromSuperview];
    view.translatesAutoresizingMaskIntoConstraints = YES;     /* the tab view sizes it again */
    item.view = view;
    for (int before = 0; before < tab; before++)
        if (!_popped[before])
            position++;
    [_tabs insertTabViewItem:item atIndex:position];
    [_tabs selectTabViewItem:item];
    [win orderOut:nil];
    [self updateHelpContext];
    [self refreshTab];
}

/* The Sound tab asks the engine to record the DAC writes only while it shows. */
- (void)soundWatch:(BOOL)active
{
    if (active == _soundWatching)
        return;
    _soundWatching = active;
    cocodebug_sound_watch(_dbg, active ? 1 : 0);
}

/* ---- refreshers ------------------------------------------------------------------ */

/* Everything the window shows, read back now: the status and the visible
 * tabs. The poll compares against what was seen here. */
- (void)refreshNow
{
    cocodebug_stop stop;
    cocodebug_stop_snapshot(_dbg, &stop);
    _seenGen = cocodebug_generation(_dbg);
    _wasPaused = stop.paused;
    if (stop.paused && stop.serial != _seenStop) {
        /* a new stop: every view goes back to where the machine is */
        _seenStop = stop.serial;
        [self recentreViews:&stop];
    }
    _liveTicks = 0;
    [self refreshStatus];
    [self refreshTab];
}

/* The registers, and each tab on show (the notebook's page and any popped-out
 * window): the others refresh when they are selected. */
- (void)refreshTab
{
    [self refreshCpu];
    if (![self tabVisible:TAB_VIDEO])
        _videoShown = NO;
    [self soundWatch:[self tabVisible:TAB_SOUND]];
    for (int tab = 0; tab < NUM_TABS; tab++)
        if ([self tabVisible:tab])
            [self refreshTabAt:tab];
}

- (void)refreshTabAt:(int)tab
{
    switch (tab) {
    case TAB_DISASM: [self refreshDisasm]; break;
    case TAB_SOURCE: [self refreshSource]; break;
    case TAB_MEMORY: [self refreshMem]; break;
    case TAB_BREAKS: [self refreshBps]; break;
    case TAB_WATCHES: [self refreshWatches]; break;
    case TAB_CART: [self refreshCart]; break;
    case TAB_TRACE: [self refreshHist:0]; break;
    case TAB_HIST: [self refreshHist:1]; break;
    case TAB_VIDEO: [self refreshVideo]; break;
    case TAB_SOUND: [self refreshSound]; break;
    case TAB_SYMBOLS: [self refreshSymbols]; break;
    case TAB_FUJINET: [self refreshFujinet:NO]; break;
    default: break;
    }
}

- (void)refreshStatus
{
    cocodebug_stop stop;
    cocodebug_stop_snapshot(_dbg, &stop);
    NSString *why = cstr(stop.reason);
    _status.stringValue = stop.paused ? [NSString stringWithFormat:@"Stopped%@%@", why.length ? @": " : @"", why]
                                      : @"Running";
    _runBtn.title = stop.paused ? @"Run (F5)" : @"Pause (F5)";
    setButtonAccent(_runBtn, stop.paused);    /* the accent while stopped */
    const BOOL source = cocodebug_source_available(_dbg) != 0;
    _stepLineBtn.enabled = source;
    _stepOverLineBtn.enabled = source;
}

/* The register strip. Blank while running: the machine can only be read
 * while it is parked. */
- (void)refreshCpu
{
    cocodebug_regs regs;
    memset(&regs, 0, sizeof regs);
    const BOOL paused = cocodebug_regs_get(_dbg, &regs) != 0;
    if (paused) {
        const uint32_t phys = cocodebug_space_size(_dbg, COCODEBUG_SPACE_PHYS);
        if (phys != _physSize)
            _physSize = phys;
    }
    [self applySpaceAvailability];
    const unsigned values[NUM_REG_FIELDS] = {
        regs.pc, regs.a, regs.b, regs.dp, regs.x, regs.y, regs.u, regs.s, regs.cc, regs.e, regs.f, regs.w, regs.v,
        regs.md
    };
    for (int field = 0; field < NUM_REG_FIELDS; field++) {
        if (_reg[field].currentEditor)
            continue;                       /* never over a field being edited */
        _reg[field].stringValue = paused ? [NSString stringWithFormat:@"%0*X", kRegDigits[field], values[field]] : @"";
        _reg[field].enabled = paused;
    }
    for (int flag = 0; flag < NUM_FLAGS; flag++) {
        _flag[flag].state = (paused && (regs.cc & kFlagMasks[flag])) ? NSControlStateValueOn : NSControlStateValueOff;
        _flag[flag].enabled = paused;
    }
    _extraRegs.hidden = !(paused && regs.is6309);
    _dLabel.stringValue = paused ? [NSString stringWithFormat:@"D=%02X%02X", regs.a, regs.b] : @"";
}

/* PHYS is offered only where there is a physical RAM space (a CoCo 3). */
- (void)applySpaceAvailability
{
    const BOOL hasPhys = _physSize > 0;
    _memSpace.autoenablesItems = NO;
    _wtSpace.autoenablesItems = NO;
    [_memSpace itemAtIndex:COCODEBUG_SPACE_PHYS].enabled = hasPhys;
    [_wtSpace itemAtIndex:COCODEBUG_SPACE_PHYS].enabled = hasPhys;
    if (!hasPhys && [self memSpaceIndex] == COCODEBUG_SPACE_PHYS)
        [_memSpace selectItemAtIndex:COCODEBUG_SPACE_CPU];
    if (!hasPhys && _wtSpace.indexOfSelectedItem == COCODEBUG_SPACE_PHYS)
        [_wtSpace selectItemAtIndex:COCODEBUG_SPACE_CPU];
    [self bpTypeChanged:nil];
}

/* The space's bytes a page at a time; each CPU row ends with the name of the
 * region it lies in (one cocodebug_memory_map per refresh). The highlighted
 * range is the accent fill, the watched ranges the selection colour. */
- (void)refreshMem
{
    static cocodebug_region regions[MAX_REGIONS];
    uint8_t data[MEM_BYTES];
    const int space = [self memSpaceIndex];
    if (!cocodebug_is_paused(_dbg)) {
        setStyledText(_mem, [[NSAttributedString alloc] initWithString:@"(the machine is running: pause it to read memory)\n"
                                                            attributes:monoAttrs()]);
        return;
    }
    const uint32_t size = (uint32_t)cocodebug_space_size(_dbg, space);
    if (size == 0) {
        setStyledText(_mem, [[NSAttributedString alloc] initWithString:@"(no such memory space on this machine)\n"
                                                            attributes:monoAttrs()]);
        return;
    }
    uint32_t base = _memBase[space];
    if (base + MEM_BYTES > size)
        base = size > MEM_BYTES ? size - MEM_BYTES : 0;
    base &= ~0xFu;
    _memBase[space] = base;
    memset(data, 0, sizeof data);
    cocodebug_mem_read(_dbg, space, base, data, MEM_BYTES);
    int regionCount = 0;
    if (space == COCODEBUG_SPACE_CPU) {
        regionCount = cocodebug_memory_map(_dbg, regions, MAX_REGIONS);
        if (regionCount < 0)
            regionCount = 0;
        if (regionCount > MAX_REGIONS)
            regionCount = MAX_REGIONS;
    }

    uint32_t watchLo[COCOWATCH_MAX], watchHi[COCOWATCH_MAX];
    int watchCount = 0;
    for (int watch = 0; watch < cocowatch_count() && watchCount < COCOWATCH_MAX; watch++) {
        uint32_t low, high;
        if (cocowatch_space(watch) == space && cocowatch_range(_dbg, watch, &low, &high)) {
            watchLo[watchCount] = low;
            watchHi[watchCount] = high;
            watchCount++;
        }
    }

    const int digits = space == COCODEBUG_SPACE_PHYS ? 6 : 4;
    NSMutableString *text = [NSMutableString stringWithFormat:@"%*s0  1  2  3  4  5  6  7   8  9  A  B  C  D  E  F\n",
                                                             digits + 2, ""];
    NSMutableArray<NSValue *> *marks = [NSMutableArray array];          /* the highlighted bytes' cells */
    NSMutableArray<NSValue *> *watchMarks = [NSMutableArray array];     /* the watched ones */
    const BOOL highlight = _memHl && _memHlSpace == space;
    for (int row = 0; row < MEM_ROWS; row++) {
        const uint32_t rowBase = base + (uint32_t)row * 16;
        [text appendFormat:@"%0*X  ", digits, rowBase];
        for (int pass = 0; pass < 2; pass++) {
            for (int col = 0; col < 16; col++) {
                const uint32_t addr = rowBase + (uint32_t)col;
                if (pass == 0 && col == 8)
                    [text appendString:@" "];
                NSMutableArray<NSValue *> *target = nil;
                if (highlight && addr >= _memHlLo && addr <= _memHlHi) {
                    target = marks;
                } else {
                    for (int watch = 0; watch < watchCount; watch++)
                        if (addr >= watchLo[watch] && addr <= watchHi[watch]) {
                            target = watchMarks;
                            break;
                        }
                }
                const uint8_t byte = data[row * 16 + col];
                if (target)
                    [target addObject:[NSValue valueWithRange:NSMakeRange(text.length, pass == 0 ? 2 : 1)]];
                if (pass == 0)
                    [text appendFormat:@"%02X ", byte];
                else
                    [text appendFormat:@"%c", (byte >= 0x20 && byte < 0x7F) ? (char)byte : '.'];
            }
            if (pass == 0)
                [text appendString:@" "];
        }
        char region[64];
        region[0] = '\0';
        if (space == COCODEBUG_SPACE_CPU)
            cocomem_region_name(regions, regionCount, (uint16_t)rowBase, region, (int)sizeof region);
        [text appendFormat:@"  %s\n", region];
    }
    NSMutableAttributedString *styled = [[NSMutableAttributedString alloc] initWithString:text attributes:monoAttrs()];
    for (NSValue *value in watchMarks)
        [styled addAttribute:NSBackgroundColorAttributeName value:NSColor.selectedTextBackgroundColor
                       range:value.rangeValue];
    for (NSValue *value in marks)
        [styled addAttributes:accentAttrs() range:value.rangeValue];
    setStyledText(_mem, styled);
}

- (void)refreshDisasm
{
    if (!cocodebug_is_paused(_dbg)) {
        _lineCount = 0;
        setStyledText(_disasm, [[NSAttributedString alloc] initWithString:@"(the machine is running: pause it to disassemble)\n"
                                                              attributes:monoAttrs()]);
        return;
    }
    cocodebug_regs regs;
    if (!cocodebug_regs_get(_dbg, &regs))
        return;
    int pcLine = -1;
    if (_followPc.state == NSControlStateValueOn) {
        const int back = cocodebug_row_address(_dbg, regs.pc, -DISASM_WINDOW / 3);
        _disasmAddr = (uint16_t)back;
    }
    int count = cocodebug_disassemble_ex(_dbg, _disasmAddr, _lines, DISASM_WINDOW, &pcLine);
    if (_followPc.state == NSControlStateValueOn && pcLine < 0) {
        _disasmAddr = regs.pc;                /* the back-guess lost the PC: start on it */
        count = cocodebug_disassemble_ex(_dbg, _disasmAddr, _lines, DISASM_WINDOW, &pcLine);
    }
    if (count < 0)
        count = 0;
    if (count > DISASM_WINDOW)
        count = DISASM_WINDOW;
    _lineCount = count;

    /* "●> A1CB  B6 04 00    LABEL            LDA   $0400   ; note": the ranges
     * are the line and its label, for the colours */
    NSMutableString *text = [NSMutableString string];
    NSRange ranges[DISASM_WINDOW], labels[DISASM_WINDOW];
    for (int row = 0; row < count; row++) {
        /* a red dot for a breakpoint, a hollow grey one for a disabled one */
        NSString *gutter = _lines[row].has_breakpoint ? @"●" : (_lines[row].bp_disabled ? @"○" : @" ");
        NSString *label = cstr(_lines[row].label);
        NSMutableString *line = [NSMutableString stringWithFormat:@"%@%c %04X  ", gutter,
                                 _lines[row].is_pc ? '>' : ' ', _lines[row].address];
        [line appendString:padTo(cstr(_lines[row].bytes), 12)];
        [line appendString:@"  "];
        labels[row] = NSMakeRange(text.length + line.length, label.length);
        [line appendString:padTo(label, 16)];
        [line appendString:@" "];
        [line appendString:padTo(cstr(_lines[row].disasm), 24)];
        NSString *note = cstr(_lines[row].comment);
        if (note.length)
            [line appendFormat:@" ; %@", note];
        ranges[row] = NSMakeRange(text.length, line.length);
        [text appendString:line];
        [text appendString:@"\n"];
    }
    if (count == 0)
        [text appendString:@"(no disassembly)\n"];

    NSMutableAttributedString *styled = [[NSMutableAttributedString alloc] initWithString:text attributes:monoAttrs()];
    for (int row = 0; row < count; row++) {
        if (labels[row].length)
            [styled addAttribute:NSForegroundColorAttributeName value:NSColor.systemGrayColor range:labels[row]];
        /* the gutter: a breakpoint red, a disabled one grey */
        if (_lines[row].has_breakpoint || _lines[row].bp_disabled)
            [styled addAttribute:NSForegroundColorAttributeName
                           value:_lines[row].has_breakpoint ? breakpointRed() : NSColor.systemGrayColor
                           range:NSMakeRange(ranges[row].location, 1)];
    }
    /* the PC's line in the accent colour with dark ink, last so its colours win */
    for (int row = 0; row < count; row++)
        if (_lines[row].is_pc)
            [styled addAttributes:accentAttrs() range:ranges[row]];
    [_disasm.textStorage setAttributedString:styled];
    NSScrollView *scroll = _disasm.enclosingScrollView;
    [scroll.contentView scrollToPoint:NSMakePoint(scroll.contentView.bounds.origin.x, 0)];
    [scroll reflectScrolledClipView:scroll.contentView];
}

/* ---- C Source ------------------------------------------------------------------ */

/* Loads (and caches) a file's text as lines, for the source pane. */
- (NSArray<NSString *> *)sourceLinesOf:(int)fileId
{
    const char *path = cocodebug_source_file_path(_dbg, fileId);
    NSString *nsPath = path ? [NSString stringWithUTF8String:path] : nil;
    if (![nsPath isEqualToString:_srcCachedPath]) {
        _srcCachedPath = nsPath;
        NSString *contents = nil;
        if (nsPath) {
            contents = [NSString stringWithContentsOfFile:nsPath encoding:NSUTF8StringEncoding error:nil];
            if (!contents)
                contents = [NSString stringWithContentsOfFile:nsPath encoding:NSISOLatin1StringEncoding error:nil];
        }
        NSMutableArray<NSString *> *lines =
            [[[contents ?: @"" stringByReplacingOccurrencesOfString:@"\r\n" withString:@"\n"]
                componentsSeparatedByString:@"\n"] mutableCopy];
        if (lines.count && [lines.lastObject length] == 0)
            [lines removeLastObject];
        _srcCachedLines = lines;
    }
    return _srcCachedLines;
}

/* The file picker follows cocodebug_source_file_count()/_file_path(): the
 * selected index and the file id are the same number. */
- (void)rebuildSrcFiles:(int)count
{
    [_srcFiles removeAllItems];
    for (int fileId = 0; fileId < count; fileId++) {
        const char *path = cocodebug_source_file_path(_dbg, fileId);
        NSString *full = path ? cstr(path) : @"";
        NSString *base = full.lastPathComponent;
        [_srcFiles addItemWithTitle:base.length ? base : full];
    }
    _srcFiles.enabled = count > 0;
}

/* Redraws the listing with '>' on the line PC is at (if PC's file is the one
 * showing) and a red dot on any line carrying a breakpoint. A new PC follows
 * the machine into its file and centres the line, even if the reader had
 * scrolled away. */
- (void)refreshSource
{
    const BOOL available = cocodebug_source_available(_dbg) != 0;
    const BOOL paused = cocodebug_is_paused(_dbg) != 0;
    const int count = available ? cocodebug_source_file_count(_dbg) : 0;
    if (count != (int)_srcFiles.numberOfItems) {
        [self rebuildSrcFiles:count];
        _srcDirty = YES;
        _srcCachedPath = nil;
        if (_srcShownFile >= count)
            _srcShownFile = -1;
    }
    if (!available || count == 0) {
        _srcShownFile = -1;
        _srcNote.stringValue = @"Load C Source reads the .lst listings of a program built with `cmoc -i` (and its .map).";
        if (_srcView.string.length)
            _srcView.string = @"";
        if (_locals.string.length)
            _locals.string = @"";
        return;
    }
    if (_srcShownFile < 0) {
        _srcShownFile = 0;
        _srcDirty = YES;
    }

    BOOL pcMoved = NO;
    if (paused) {
        cocodebug_regs regs;
        int fileId, line;
        if (cocodebug_regs_get(_dbg, &regs) && cocodebug_source_line_at(_dbg, regs.pc, &fileId, &line)) {
            if (fileId != _pcFile || line != _pcLine) {
                _pcFile = fileId;
                _pcLine = line;
                pcMoved = YES;
                if (fileId != _srcShownFile && fileId < count) {
                    _srcShownFile = fileId;       /* followed into another file */
                    _srcDirty = YES;
                }
            }
        } else if (_pcFile >= 0) {
            _pcFile = -1;                         /* mid-prologue, or code with no line info */
            _pcLine = -1;
            _srcDirty = YES;
        }
    } else if (_pcFile >= 0) {
        _pcFile = -1;
        _pcLine = -1;
        _srcDirty = YES;
    }
    if (_srcFiles.indexOfSelectedItem != _srcShownFile)
        [_srcFiles selectItemAtIndex:_srcShownFile];

    if (_srcDirty || pcMoved) {
        _srcDirty = NO;
        NSArray<NSString *> *lines = [self sourceLinesOf:_srcShownFile];
        NSMutableAttributedString *text = [[NSMutableAttributedString alloc] init];
        NSRange pcRange = NSMakeRange(NSNotFound, 0);
        for (NSUInteger row = 0; row < lines.count; row++) {
            const int lineNo = (int)row + 1;
            const BOOL isPc = _srcShownFile == _pcFile && lineNo == _pcLine;
            const BOOL hasBp = cocodebug_source_breakpoint_is_set(_dbg, _srcShownFile, lineNo) != 0;
            NSString *line = [NSString stringWithFormat:@"%@%c %5d  %@", hasBp ? @"●" : @" ", isPc ? '>' : ' ', lineNo,
                                                        lines[row]];
            NSMutableAttributedString *styled =
                [[NSMutableAttributedString alloc] initWithString:line attributes:monoAttrs()];
            if (hasBp)
                [styled addAttribute:NSForegroundColorAttributeName value:breakpointRed() range:NSMakeRange(0, 1)];
            if (isPc) {
                [styled addAttributes:accentAttrs() range:NSMakeRange(0, styled.length)];
                pcRange = NSMakeRange(text.length, styled.length);
            }
            [text appendAttributedString:styled];
            [text appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n" attributes:monoAttrs()]];
        }
        setStyledText(_srcView, text);
        if (pcMoved && pcRange.location != NSNotFound) {
            /* centre the line */
            NSScrollView *scroll = _srcView.enclosingScrollView;
            NSLayoutManager *layout = _srcView.layoutManager;
            const NSRange glyphs = [layout glyphRangeForCharacterRange:pcRange actualCharacterRange:NULL];
            const NSRect box = [layout boundingRectForGlyphRange:glyphs inTextContainer:_srcView.textContainer];
            NSClipView *clip = scroll.contentView;
            NSRect bounds = clip.bounds;
            bounds.origin.y = box.origin.y + box.size.height / 2 - bounds.size.height / 2;
            bounds = [clip constrainBoundsRect:bounds];
            [clip scrollToPoint:bounds.origin];
            [scroll reflectScrolledClipView:clip];
        }
    }
    NSString *path = cstr(cocodebug_source_file_path(_dbg, _srcShownFile));
    _srcNote.stringValue = [NSString stringWithFormat:@"%d file%s: %@", count, count == 1 ? "" : "s", path];
    [self refreshLocals];
}

- (void)refreshLocals
{
    if (!cocodebug_is_paused(_dbg)) {
        _locals.string = @"";
        return;
    }
    cocodebug_local locals[LOCALS_MAX];
    const int count = cocodebug_locals_get(_dbg, locals, LOCALS_MAX);
    if (count <= 0) {
        /* Not necessarily an error: PC may just be before the function's
         * first statement (mid-prologue), where there is nothing correct to
         * show yet. */
        setText(_locals, @"(no locals in scope)", NO);
        return;
    }
    NSMutableString *text = [NSMutableString string];
    for (int index = 0; index < count; index++) {
        const cocodebug_local *local = &locals[index];
        if (local->has_value)
            [text appendFormat:@"%-12s %-10s %s= %u ($%X)\n", local->name, local->type,
                               local->is_param ? "(param) " : "", local->value, local->value];
        else
            [text appendFormat:@"%-12s %-10s %s= ?\n", local->name, local->type, local->is_param ? "(param) " : ""];
    }
    setText(_locals, text, NO);
}

/* The Video tab: the controller says what changed. Forced when the tab
 * has just come on show. */
- (void)refreshVideo
{
    [_video refreshForce:!_videoShown];
    _videoShown = YES;
}

- (void)refreshBps
{
    static cocodebug_breakpoint bps[MAX_BPS];
    int count = cocodebug_breakpoints_list(_dbg, bps, MAX_BPS);
    if (count < 0)
        count = 0;
    if (count > MAX_BPS)
        count = MAX_BPS;

    /* the first line: why it stopped, or that it runs */
    cocodebug_stop stop;
    cocodebug_stop_snapshot(_dbg, &stop);
    _bpStop.stringValue = stop.paused ? [NSString stringWithFormat:@"Stopped: %@", cstr(stop.reason)] : @"Running.";

    /* the rows are rebuilt only when something in them changed (a refresh
     * must not eat a click) */
    NSMutableString *signature = [NSMutableString string];
    NSMutableArray<NSString *> *descriptions = [NSMutableArray array];
    for (int index = 0; index < count; index++) {
        char desc[256];
        desc[0] = '\0';
        cocobp_describe(_dbg, &bps[index], desc, (int)sizeof desc);
        [descriptions addObject:cstr(desc)];
        [signature appendFormat:@"%d %d %u %@\n", bps[index].id, bps[index].enabled, bps[index].hits,
                                descriptions[(NSUInteger)index]];
    }
    if ([signature isEqualToString:_bpSig])
        return;
    _bpSig = signature;

    for (NSView *view in [_bpList.arrangedSubviews copy])
        [_bpList removeView:view];
    if (count == 0)
        [_bpList addArrangedSubview:[self label:@"No breakpoints"]];
    for (int index = 0; index < count; index++) {
        NSButton *box = [NSButton checkboxWithTitle:[NSString stringWithFormat:@"#%d", bps[index].id]
                                             target:self
                                             action:@selector(bpEnabled:)];
        box.state = bps[index].enabled ? NSControlStateValueOn : NSControlStateValueOff;
        box.tag = bps[index].id;
        box.font = monoFont();
        [box.widthAnchor constraintEqualToConstant:70].active = YES;
        NSButton *remove = [self button:@"Remove" action:@selector(bpRemove:)];
        remove.tag = bps[index].id;
        [remove.widthAnchor constraintEqualToConstant:70].active = YES;
        NSStackView *row = hstack(@[box, tableCell(descriptions[(NSUInteger)index], 0, NO),
                                    tableCell([NSString stringWithFormat:@"%u", bps[index].hits], 60, NO), remove]);
        row.distribution = NSStackViewDistributionFill;
        [_bpList addArrangedSubview:row];
        [row.widthAnchor constraintEqualToAnchor:_bpList.widthAnchor].active = YES;
    }
}

- (void)refreshWatches
{
    const int count = cocowatch_count();
    NSMutableArray<NSString *> *lines = [NSMutableArray array];
    NSMutableString *signature = [NSMutableString string];
    for (int index = 0; index < count; index++) {
        char buffer[256];
        buffer[0] = '\0';
        cocowatch_format(_dbg, index, buffer, (int)sizeof buffer);
        [lines addObject:cstr(buffer)];
        [signature appendFormat:@"%d %@\n", cocowatch_space(index), lines[(NSUInteger)index]];
    }
    [signature appendFormat:@"%u", cocowatch_generation()];
    if ([signature isEqualToString:_wtSig])
        return;      /* a refresh must not eat a click */
    _wtSig = signature;

    for (NSView *view in [_wtList.arrangedSubviews copy])
        [_wtList removeView:view];
    if (count == 0)
        [_wtList addArrangedSubview:[self label:@"No watches"]];
    for (int index = 0; index < count; index++) {
        NSButton *mem = [self button:@"Show in Memory" action:@selector(wtMemory:)];
        mem.tag = index;
        NSButton *brk = [self button:@"Break on Change" action:@selector(wtBreak:)];
        brk.tag = index;
        NSButton *remove = [self button:@"Remove" action:@selector(wtRemove:)];
        remove.tag = index;
        NSStackView *row = hstack(@[tableCell(lines[(NSUInteger)index], 0, NO), mem, brk, remove]);
        row.distribution = NSStackViewDistributionFill;
        [_wtList addArrangedSubview:row];
        [row.widthAnchor constraintEqualToAnchor:_wtList.widthAnchor].active = YES;
    }
}

static NSString *piaText(const char *name, const cocodebug_pia *pia)
{
    return [NSString stringWithFormat:
        @"%s  A: CR %02X  DDR %02X  OR %02X  pins %02X   CA1 %d CA2 %d  IRQ %d  flag %02X\n"
        @"      B: CR %02X  DDR %02X  OR %02X  pins %02X   CB1 %d CB2 %d  IRQ %d  flag %02X\n",
        name, pia->cra, pia->ddra, pia->ora, pia->pina, pia->ca1, pia->ca2, pia->irqa, pia->irq_flags_a, pia->crb,
        pia->ddrb, pia->orb, pia->pinb, pia->cb1, pia->cb2, pia->irqb, pia->irq_flags_b];
}

/* The cartridge, the memory map (as the SAM/GIME map it now), the PIAs, the
 * SAM, on a CoCo 3 the GIME, the joysticks and the Becker link. */
- (void)refreshCart
{
    static cocodebug_region regions[MAX_REGIONS];
    char info[256];
    info[0] = '\0';
    cocodebug_cart_info(_dbg, info, (int)sizeof info);
    NSMutableString *text = [NSMutableString stringWithFormat:@"%@\n", cstr(info)];

    if (!cocodebug_is_paused(_dbg)) {
        [text appendString:@"\n(the machine is running: pause it to read the memory map and the chips)\n"];
        setText(_cartText, text, NO);
        return;
    }
    [text appendString:@"\nMemory map ($0000-$FFFF, as the SAM/GIME map it now)\n"];
    int regionCount = cocodebug_memory_map(_dbg, regions, MAX_REGIONS);
    if (regionCount < 0)
        regionCount = 0;
    if (regionCount > MAX_REGIONS)
        regionCount = MAX_REGIONS;
    if (regionCount == 0)
        [text appendString:@"  (not available)\n"];
    for (int index = 0; index < regionCount; index++) {
        NSString *phys = regions[index].physical_base >= 0
                             ? [NSString stringWithFormat:@"  phys %05X", regions[index].physical_base]
                             : @"";
        [text appendFormat:@"  %04X-%04X  %@  %@%@\n", regions[index].lo, regions[index].hi,
                           regions[index].writable ? @"rw" : @"r-", cstr(regions[index].name), phys];
    }

    cocodebug_io chips;
    memset(&chips, 0, sizeof chips);
    if (!cocodebug_io_get(_dbg, &chips)) {
        setText(_cartText, text, NO);
        return;
    }
    [text appendFormat:@"\nCPU  %@\n", chips.is6309 ? @"HD6309" : @"MC6809"];
    [text appendString:@"\nPIAs ($FF00 and $FF20)\n"];
    [text appendString:piaText("PIA0", &chips.pia0)];
    [text appendString:piaText("PIA1", &chips.pia1)];
    if (chips.sam_valid)
        [text appendFormat:@"\nSAM  V %u  F %02X  P %u  R %u  M %u  TY %u   video address $%04X\n", chips.sam_v, chips.sam_f,
                           chips.sam_p, chips.sam_r, chips.sam_m, chips.sam_ty, chips.sam_video_address];
    if (chips.gime_valid) {
        [text appendString:@"\nGIME ($FF90-$FF9F)\n   "];
        for (int reg = 0; reg < 16; reg++)
            [text appendFormat:@" %02X", chips.gime_reg[reg]];
        [text appendFormat:@"\n   scanline %u  row %u   IRQ %d  FIRQ %d\nPalette ($FFB0, 6-bit)\n   ", chips.gime_scanline,
                           chips.gime_row, chips.gime_irq, chips.gime_firq];
        for (int slot = 0; slot < 16; slot++)
            [text appendFormat:@" %02X", chips.gime_palette[slot]];
        [text appendString:@"\nMMU ($FFA0, task 0 then task 1)\n   "];
        for (int bank = 0; bank < 16; bank++)
            [text appendFormat:@" %02X", chips.gime_mmu[bank]];
        [text appendString:@"\n"];
    }
    [text appendString:@"\nJoysticks (axes 0-65535)\n"];
    for (int port = 0; port < 2; port++)
        [text appendFormat:@"  %@  X %5d  Y %5d  buttons %d %d\n", port == 0 ? @"Right" : @"Left", chips.joy_axis[port][0],
                           chips.joy_axis[port][1], chips.joy_button[port][0], chips.joy_button[port][1]];
    [text appendFormat:@"\nBecker link %@, %d byte%s waiting to the CoCo\n", chips.becker_link_up ? @"up" : @"down",
                       chips.becker_rx_pending, chips.becker_rx_pending == 1 ? "" : "s"];
    setText(_cartText, text, NO);
}

/* The DAC, the single-bit sound, the multiplexer and the recent writes to
 * them, with a bar of the DAC level. */
- (void)refreshSound
{
    static cocodebug_sound_write writes[SOUND_SHOWN];
    NSMutableString *text = [NSMutableString string];
    cocodebug_sound snd;
    memset(&snd, 0, sizeof snd);
    if (cocodebug_sound_get(_dbg, &snd)) {
        NSMutableString *bar = [NSMutableString string];
        for (int cell = 0; cell < 32; cell++)
            [bar appendString:(snd.dac * 32 / 64) > cell ? @"█" : @"░"];
        [text appendFormat:@"6-bit DAC        %2d/63  %@\n", snd.dac, bar];
        [text appendFormat:@"Single-bit sound %d\n", snd.single_bit];
        [text appendFormat:@"Sound enable     %@  (PIA1 CB2: the multiplexer's output reaches the speaker)\n",
                           snd.sound_enable ? @"on" : @"off"];
        [text appendFormat:@"Mux source       %d  %@\n", snd.mux_source, cstr(snd.mux_name)];
        [text appendFormat:@"Cassette motor   %@\n", snd.motor ? @"on" : @"off"];
        [text appendFormat:@"Cassette input   %d\n", snd.cassette_in];
    } else {
        [text appendString:@"(the machine is running: pause it to read the sound state)\n"];
    }
    const int count = cocodebug_sound_history(_dbg, writes, SOUND_SHOWN);
    [text appendString:@"\nRecent DAC and single-bit writes, newest first (recorded while this tab shows)\n"];
    if (count <= 0) {
        [text appendString:@"  (none yet: run the machine with this tab open)\n"];
    } else {
        static NSString *const levels[9] = { @" ", @"▁", @"▂", @"▃", @"▄", @"▅", @"▆", @"▇", @"█" };
        NSMutableString *wave = [NSMutableString string];
        for (int index = count - 1; index >= 0; index--)
            [wave appendString:levels[(writes[index].dac * 8) / 63]];
        [text appendFormat:@"  DAC level, oldest to newest: %@\n\n", wave];
        for (int index = 0; index < count; index++)
            [text appendFormat:@"  tick %10u  %@ = $%02X  DAC %2d\n", writes[index].tick,
                               writes[index].port == 0 ? @"$FF20" : @"$FF22", writes[index].value, writes[index].dac];
    }
    setText(_soundText, text, NO);
}

/* The symbol table, filtered: "AAAA  name", ordered by address. The table's
 * own order is unstable, so each row's address is cached when the list is
 * built and activation reads that. */
- (void)refreshSymbols
{
    const int total = cocodebug_symbols_count(_dbg);
    NSString *filter = trimmed(_symFilter.stringValue);
    int *indices = total > 0 ? malloc((size_t)total * sizeof *indices) : NULL;
    int shown = 0;
    if (indices)
        shown = cocodebug_symbols_filter(_dbg, filter.UTF8String, indices, total);
    if (shown < 0)
        shown = 0;
    NSMutableString *text = [NSMutableString string];
    [_symAddrs removeAllObjects];
    for (int row = 0; row < shown; row++) {
        uint16_t address = 0;
        char name[96];
        name[0] = '\0';
        if (!cocodebug_symbols_get(_dbg, indices[row], &address, name, (int)sizeof name))
            continue;
        [text appendFormat:@"%04X  %@\n", address, cstr(name)];
        [_symAddrs addObject:@(address)];
    }
    free(indices);
    if (_symAddrs.count == 0)
        [text appendString:@"(no symbols)\n"];
    if (_symSelected >= (int)_symAddrs.count)
        _symSelected = -1;
    NSMutableAttributedString *styled = [[NSMutableAttributedString alloc] initWithString:text attributes:monoAttrs()];
    if (_symSelected >= 0) {
        NSUInteger start = 0;
        for (int row = 0; row < _symSelected; row++)
            start = NSMaxRange([text lineRangeForRange:NSMakeRange(start, 0)]);
        [styled addAttributes:accentAttrs() range:[text lineRangeForRange:NSMakeRange(start, 0)]];
    }
    setStyledText(_symList, styled);
    _symCount.stringValue = [NSString stringWithFormat:@"%lu of %d symbols%@", (unsigned long)_symAddrs.count, total,
                                                       filter.length ? @" (filtered)" : @""];
}

/* Trace (idx 0) and History (idx 1): the newest HIST_SHOWN lines of the
 * engine's ring that pass the filter, oldest first. History adds where the
 * beam was at each instruction. */
- (void)refreshHist:(int)idx
{
    static cocodebug_trace_entry ring[HIST_RING];
    static int hit[HIST_SHOWN];
    const BOOL active = cocodebug_trace_enabled(_dbg) != 0;
    [self syncRecord:active];

    uint32_t low = 0, high = 0xFFFF;
    int filter = COCODEBUG_TRACE_ALL;
    BOOL badRange = NO;
    switch (_hFilter[idx].indexOfSelectedItem) {
    case 1:
        filter = COCODEBUG_TRACE_BRANCHES;
        break;
    case 2:
        if (cocosym_parse_range(_dbg, COCODEBUG_SPACE_CPU, _hRange[idx].stringValue.UTF8String, &low, &high))
            filter = COCODEBUG_TRACE_RANGE;
        else
            badRange = YES;
        break;
    default:
        break;
    }
    const int total = cocodebug_trace_count(_dbg);
    /* newest first: unfiltered, only the lines shown are needed */
    int count = cocodebug_trace_read(_dbg, ring, filter == COCODEBUG_TRACE_ALL ? HIST_SHOWN : HIST_RING);
    if (count < 0)
        count = 0;
    int shown = 0;
    for (int index = 0; index < count && shown < HIST_SHOWN; index++)
        if (cocodebug_trace_match(&ring[index], filter, (uint16_t)low, (uint16_t)high))
            hit[shown++] = index;

    NSMutableString *text = [NSMutableString string];
    for (int pos = shown - 1; pos >= 0; pos--) {
        char line[256];
        line[0] = '\0';
        cocodebug_trace_format(&ring[hit[pos]], line, (int)sizeof line);
        [text appendString:cstr(line)];
        if (idx == 1)
            [text appendFormat:@"   line %d  hpos %d  frame %u", ring[hit[pos]].line, ring[hit[pos]].hpos,
                               ring[hit[pos]].frame];
        [text appendString:@"\n"];
    }
    NSString *note;
    if (!active && total == 0)
        note = @"recording off";
    else
        note = [NSString stringWithFormat:@"%@, %d instructions%@", active ? @"recording" : @"recording off", total,
                                          shown < total ? [NSString stringWithFormat:@" (showing %d)", shown] : @""];
    if (badRange)
        note = [note stringByAppendingString:@" (range not understood: showing all)"];
    _hNote[idx].stringValue = note;
    setText(_hText[idx], text, YES);
}

/* The packet log is rebuilt only when it changed: a new packet, or a
 * repeat (which moves the newest serial on). */
- (void)refreshFujinet:(BOOL)force
{
    static coco_fuji_packet packets[FUJI_PACKETS_MAX];
    char state[1024];
    const int count = cocodebug_fujinet_trace(_dbg, packets, FUJI_PACKETS_MAX);
    const BOOL hide = _fujiHideReady.state == NSControlStateValueOn;

    if (!cocosession_fujinet_running(_session) && !cocosession_fujinet_external(_session))
        _fujiNote.stringValue = @"FujiNet is off (Settings ▸ FujiNet).";
    else
        _fujiNote.stringValue = [NSString stringWithFormat:@"%@ FujiNet on the Becker port %s, %@.",
                                 cocosession_fujinet_external(_session) ? @"External" : @"Built-in",
                                 cocosession_becker_endpoint(_session),
                                 cocosession_fujinet_link(_session) ? @"connected" : @"not connected"];
    state[0] = '\0';
    cocodebug_fujinet_state(_dbg, state, (int)sizeof state);
    setText(_fujiState, cstr(state), NO);

    if (!force && count == _fujiCount && (count == 0 || packets[count - 1].serial == _fujiLast))
        return;
    const BOOL follow = force || _fujiCount < 0 || atEnd(_fujiText);
    _fujiCount = count;
    _fujiLast = count > 0 ? packets[count - 1].serial : 0;
    NSMutableString *text = [NSMutableString string];
    int shown = 0;
    for (int index = 0; index < count; index++) {
        if (hide && cocodebug_fujinet_is_ready(&packets[index]))
            continue;
        if (shown++)
            [text appendString:@"\n"];
        [text appendString:cstr(packets[index].text)];
        if (packets[index].repeat > 1)
            [text appendFormat:@" ×%u", packets[index].repeat];
        [text appendString:@"\n     "];
        for (int byte = 0; byte < packets[index].nbytes && byte < (int)sizeof packets[index].data; byte++)
            [text appendFormat:@"%02X ", packets[index].data[byte]];
        if (packets[index].nbytes > (int)sizeof packets[index].data)
            [text appendString:@"…"];
    }
    if (shown == 0)
        [text setString:count ? @"(only READY probes since the trace was cleared)"
                              : @"(no DriveWire traffic since the trace was cleared)"];
    setText(_fujiText, text, follow);
}

@end
