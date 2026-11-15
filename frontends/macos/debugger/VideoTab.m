/*
 * The debugger's Video tab (AppKit).
 *
 * Everything that decides what to show lives in the core (cocovidtab.h): this file is views.
 * The picture comes from the controller's cached 0xFFRRGGBB buffer and is drawn as a CGImage
 * (kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, no interpolation) over a copy of
 * that buffer; the image is rebuilt only when the controller re-rendered. A mouse move redraws
 * only the marks. Only the section showing is filled in.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#import "VideoTab.h"

#include <stdio.h>
#include <string.h>

#include "cocovidcap.h"
#include "cocovidtab.h"

#define MAX_OVERLAYS 8
#define SWATCH_ROWS 16         /* the GIME has sixteen palette slots, the VDG twelve colours */

/* A C string as an NSString, never nil (invalid UTF-8 reads as empty). */
static NSString *str(const char *text)
{
    NSString *result = text ? [NSString stringWithUTF8String:text] : nil;
    return result ? result : @"";
}

/* A 0xFFRRGGBB word as a colour. */
static NSColor *colorFromRGB(uint32_t rgb)
{
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 255) / 255.0
                               green:((rgb >> 8) & 255) / 255.0
                                blue:(rgb & 255) / 255.0
                               alpha:1.0];
}

/* The ink on an accent fill, and the fill itself (the icon's green). */
static NSColor *accentInk(void)
{
    return colorFromRGB(COCOSESSION_ACCENT_INK_RGB);
}

static NSColor *accentFill(void)
{
    return colorFromRGB(COCOSESSION_ACCENT_RGB);
}

static NSFont *monoFont11(void)
{
    return [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
}

/* A selectable monospace label that wraps at `width` (0: no limit). */
static NSTextField *monoLabel(CGFloat width)
{
    NSTextField *label = [NSTextField wrappingLabelWithString:@""];
    label.font = monoFont11();
    label.selectable = YES;
    if (width > 0) label.preferredMaxLayoutWidth = width;
    return label;
}

/* ---- the picture ------------------------------------------------------------------ */

@interface CocoVideoPicture : NSView
@property (nonatomic, assign) cocovid_tab *tab;
@property (nonatomic, copy) void (^onChanged)(void);        /* the inspector changed */
@property (nonatomic, copy) void (^onBreak)(void);          /* a double-click: break on the pixel */
- (void)sync;                                               /* the controller re-rendered */
@end

@implementation CocoVideoPicture {
    CGImageRef _image;
    NSTrackingArea *_area;
    NSString *_message;
    NSSize _size;
}

- (BOOL)isFlipped { return YES; }

- (void)dealloc
{
    if (_image) CGImageRelease(_image);
}

- (NSSize)intrinsicContentSize { return _size.width > 0 ? _size : NSMakeSize(320, 240); }

- (void)sync
{
    int width = 0, height = 0;
    const uint32_t *pixels = cocovid_tab_picture(_tab, &width, &height);
    if (_image) {
        CGImageRelease(_image);
        _image = NULL;
    }
    if (pixels && width > 0 && height > 0) {
        /* a copy: the controller's buffer lives only until its next refresh */
        CFDataRef data = CFDataCreate(NULL, (const UInt8 *)pixels, (CFIndex)width * height * 4);
        CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        _image = CGImageCreate((size_t)width, (size_t)height, 8, 32, (size_t)width * 4, space,
                               kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, provider, NULL, false,
                               kCGRenderingIntentDefault);
        CGColorSpaceRelease(space);
        CGDataProviderRelease(provider);
        CFRelease(data);
        _message = nil;
        _size = NSMakeSize(width, height);
    } else {
        const char *why = cocovid_tab_why(_tab);
        _message = str(why ? why : "Nothing to show");
        _size = NSMakeSize(0, 0);
    }
    [self invalidateIntrinsicContentSize];
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    CGContextRef context = [[NSGraphicsContext currentContext] CGContext];
    [[NSColor colorWithCalibratedRed:0.07 green:0.07 blue:0.09 alpha:1] setFill];
    NSRectFill(self.bounds);
    if (!_image) {
        NSDictionary *attrs = @{ NSForegroundColorAttributeName: [NSColor colorWithCalibratedWhite:0.6 alpha:1],
                                 NSFontAttributeName: [NSFont systemFontOfSize:13] };
        NSString *message = _message ? _message : @"";
        const NSSize textSize = [message sizeWithAttributes:attrs];
        [message drawAtPoint:NSMakePoint((self.bounds.size.width - textSize.width) / 2,
                                         (self.bounds.size.height - textSize.height) / 2)
              withAttributes:attrs];
        return;
    }
    /* the view is flipped: draw the image upright */
    CGContextSaveGState(context);
    CGContextSetInterpolationQuality(context, kCGInterpolationNone);
    CGContextTranslateCTM(context, 0, _size.height);
    CGContextScaleCTM(context, 1, -1);
    CGContextDrawImage(context, CGRectMake(0, 0, _size.width, _size.height), _image);
    CGContextRestoreGState(context);
    int hover[4] = { 0 }, pin[4] = { 0 };
    const int marks = cocovid_tab_marks(_tab, hover, pin);
    if (marks & 1) {
        [[NSColor colorWithCalibratedWhite:1 alpha:0.9] setStroke];
        NSBezierPath *path = [NSBezierPath bezierPathWithRect:NSMakeRect(hover[0] + 0.5, hover[1] + 0.5,
                                                                         hover[2] - 1, hover[3] - 1)];
        path.lineWidth = 1;
        [path stroke];
    }
    if (marks & 2) {
        [[NSColor colorWithCalibratedRed:1 green:0.85 blue:0.1 alpha:1] setStroke];
        NSBezierPath *path = [NSBezierPath bezierPathWithRect:NSMakeRect(pin[0] + 1, pin[1] + 1,
                                                                         pin[2] - 2, pin[3] - 2)];
        path.lineWidth = 2;
        [path stroke];
    }
}

- (void)updateTrackingAreas
{
    [super updateTrackingAreas];
    if (_area) [self removeTrackingArea:_area];
    _area = [[NSTrackingArea alloc] initWithRect:self.bounds
                                         options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                                                 NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
                                           owner:self userInfo:nil];
    [self addTrackingArea:_area];
}

- (NSPoint)local:(NSEvent *)event { return [self convertPoint:event.locationInWindow fromView:nil]; }

- (void)mouseMoved:(NSEvent *)event
{
    const NSPoint point = [self local:event];
    if (cocovid_tab_hover(_tab, (int)point.x, (int)point.y)) {
        [self setNeedsDisplay:YES];
        if (_onChanged) _onChanged();
    }
}

- (void)mouseExited:(NSEvent *)event
{
    (void)event;
    if (cocovid_tab_hover(_tab, -1, -1)) {
        [self setNeedsDisplay:YES];
        if (_onChanged) _onChanged();
    }
}

- (void)mouseDown:(NSEvent *)event
{
    const NSPoint point = [self local:event];
    if (event.clickCount >= 2) {
        cocovid_tab_hover(_tab, (int)point.x, (int)point.y);
        if (_onBreak) _onBreak();
    } else {
        cocovid_tab_pin(_tab, (int)point.x, (int)point.y);
    }
    [self setNeedsDisplay:YES];
    if (_onChanged) _onChanged();
}
@end

/* ---- a colour swatch ---------------------------------------------------------------- */

@interface CocoSwatchView : NSView
@property (nonatomic, strong) NSColor *color;
@end

@implementation CocoSwatchView
- (NSSize)intrinsicContentSize { return NSMakeSize(22, 16); }

- (void)setColor:(NSColor *)color
{
    _color = color;
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    [(_color ? _color : NSColor.blackColor) setFill];
    NSRectFill(self.bounds);
    [[NSColor colorWithCalibratedWhite:0.5 alpha:1] setStroke];
    NSBezierPath *path = [NSBezierPath bezierPathWithRect:NSInsetRect(self.bounds, 0.5, 0.5)];
    path.lineWidth = 1;
    [path stroke];
}
@end

/* ---- the tab ------------------------------------------------------------------------------ */

@interface CocoVideoTab () <NSTableViewDataSource, NSTableViewDelegate, NSTabViewDelegate>
@end

@implementation CocoVideoTab {
    cocovid_tab *_tab;
    NSView *_root;
    NSPopUpButton *_viewPopup, *_zoomPopup;
    NSButton *_breakButton, *_pixelBreak;
    NSButton *_overlayBox[MAX_OVERLAYS];
    NSTextField *_status, *_note, *_pixelText, *_timingText;
    NSTextField *_swatchLabel[SWATCH_ROWS];
    NSArray<NSView *> *_swatchRows;
    CocoSwatchView *_swatchView[SWATCH_ROWS];
    CocoVideoPicture *_picture;
    NSScrollView *_scroll;
    NSTabView *_sections;
    NSTableView *_regsTable, *_linesTable;
    NSProgressIndicator *_beam;
    NSString *_regsSignature, *_linesSignature;
    BOOL _updating;
    int _regCount, _lineCount;
    int _viewportWidth, _viewportHeight;
    id _resizeObserver;
}

- (NSView *)view { return _root; }

- (instancetype)initWithDebugger:(cocodebug *)debugger
{
    self = [super init];
    if (!self) return nil;
    _tab = cocovid_tab_new(debugger);
    [self build];
    return self;
}

- (void)dealloc
{
    if (_resizeObserver) [[NSNotificationCenter defaultCenter] removeObserver:_resizeObserver];
    if (_tab) cocovid_tab_free(_tab);
    _tab = NULL;
}

- (void)build
{
    int overlayCount = 0;
    const cocovid_overlay_info *overlays = cocovid_overlays(&overlayCount);
    if (overlayCount > MAX_OVERLAYS) overlayCount = MAX_OVERLAYS;
    int zoomCount = 0;
    const int *zooms = cocovid_zooms(&zoomCount);

    _viewPopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (int index = 0; index < COCOVID_V_COUNT; index++) [_viewPopup addItemWithTitle:str(cocovid_view_name(index))];
    _viewPopup.target = self;
    _viewPopup.action = @selector(onView:);
    _viewPopup.toolTip = @"Display: the last published frame, border and all.\n"
                          "Decoded: the display area rebuilt from each line's own mode, bytes and palette.\n"
                          "Beam: this frame above the beam, the last one dimmed below (stopped).\n"
                          "Values: Decoded, each pixel a flat false colour of its colour code.";
    _zoomPopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (int index = 0; index < zoomCount; index++)
        [_zoomPopup addItemWithTitle:zooms[index] == 0 ? @"Fit" : [NSString stringWithFormat:@"%d×", zooms[index]]];
    _zoomPopup.target = self;
    _zoomPopup.action = @selector(onZoom:);
    _breakButton = [NSButton buttonWithTitle:@"Break on writes" target:self action:@selector(onBreak:)];
    _breakButton.toolTip = @"Add a write breakpoint on the RAM byte the inspected pixel was fetched from";
    NSStackView *bar = [NSStackView stackViewWithViews:@[[NSTextField labelWithString:@"View"], _viewPopup,
                                                         [NSTextField labelWithString:@"Zoom"], _zoomPopup,
                                                         _breakButton]];
    bar.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    bar.spacing = 8;

    NSMutableArray<NSView *> *row1 = [NSMutableArray array], *row2 = [NSMutableArray array];
    for (int index = 0; index < overlayCount; index++) {
        _overlayBox[index] = [NSButton checkboxWithTitle:str(overlays[index].label) target:self action:@selector(onOverlay:)];
        [(index < 4 ? row1 : row2) addObject:_overlayBox[index]];
    }
    NSStackView *overlayRow1 = [NSStackView stackViewWithViews:row1];
    NSStackView *overlayRow2 = [NSStackView stackViewWithViews:row2];
    overlayRow1.orientation = overlayRow2.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    overlayRow1.spacing = overlayRow2.spacing = 10;

    _status = monoLabel(0);
    _picture = [[CocoVideoPicture alloc] initWithFrame:NSMakeRect(0, 0, 320, 240)];
    _picture.tab = _tab;
    __weak CocoVideoTab *weak = self;
    _picture.onChanged = ^{ [weak updatePixel]; };
    _picture.onBreak = ^{ [weak breakOnPixel]; };
    _scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 640, 400)];
    _scroll.hasVerticalScroller = YES;
    _scroll.hasHorizontalScroller = YES;
    _scroll.documentView = _picture;
    _scroll.drawsBackground = NO;
    [_scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    [_scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    _note = [NSTextField labelWithString:@""];

    NSStackView *left = [NSStackView stackViewWithViews:@[bar, overlayRow1, overlayRow2, _status, _scroll, _note]];
    left.orientation = NSUserInterfaceLayoutOrientationVertical;
    left.alignment = NSLayoutAttributeLeading;
    left.spacing = 6;
    [_scroll.widthAnchor constraintEqualToAnchor:left.widthAnchor].active = YES;

    /* Fit follows the scroll view's size */
    _scroll.postsFrameChangedNotifications = YES;
    _resizeObserver = [[NSNotificationCenter defaultCenter]
        addObserverForName:NSViewFrameDidChangeNotification
                    object:_scroll
                     queue:nil
                usingBlock:^(NSNotification *note) {
        (void)note;
        [weak viewportChanged];
    }];

    /* ---- the sections, in cocovid_section_name order */
    _sections = [[NSTabView alloc] init];
    _sections.delegate = self;
    {   /* Pixel */
        _pixelText = monoLabel(370);
        _pixelBreak = [NSButton buttonWithTitle:@"Break on writes" target:self action:@selector(onBreak:)];
        NSButton *unpin = [NSButton buttonWithTitle:@"Unpin" target:self action:@selector(onUnpin:)];
        NSStackView *buttons = [NSStackView stackViewWithViews:@[_pixelBreak, unpin]];
        buttons.orientation = NSUserInterfaceLayoutOrientationHorizontal;
        buttons.spacing = 6;
        NSStackView *page = [NSStackView stackViewWithViews:@[_pixelText, buttons]];
        page.orientation = NSUserInterfaceLayoutOrientationVertical;
        page.alignment = NSLayoutAttributeLeading;
        page.spacing = 8;
        [self addSection:@"Pixel" content:page];
    }
    {   /* Registers */
        _regsTable = [[NSTableView alloc] init];
        [self addSection:@"Registers" content:[self scrollForTable:_regsTable
                                                           columns:@[@"name", @"val", @"meaning"]
                                                            titles:@[@"Register", @"Value", @"Meaning"]
                                                            widths:@[@70, @60, @390]]];
    }
    {   /* Colours: the VDG's twelve colours or the GIME's sixteen palette slots */
        NSMutableArray<NSView *> *rows = [NSMutableArray array];
        for (int index = 0; index < SWATCH_ROWS; index++) {
            _swatchView[index] = [[CocoSwatchView alloc] initWithFrame:NSMakeRect(0, 0, 22, 16)];
            _swatchLabel[index] = monoLabel(0);
            _swatchLabel[index].selectable = NO;
            NSStackView *row = [NSStackView stackViewWithViews:@[_swatchView[index], _swatchLabel[index]]];
            row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
            row.spacing = 8;
            [rows addObject:row];
        }
        NSStackView *page = [NSStackView stackViewWithViews:rows];
        page.orientation = NSUserInterfaceLayoutOrientationVertical;
        page.alignment = NSLayoutAttributeLeading;
        page.spacing = 4;
        _swatchRows = rows;
        [self addSection:@"Colours" content:page];
    }
    {   /* Lines */
        _linesTable = [[NSTableView alloc] init];
        [self addSection:@"Lines" content:[self scrollForTable:_linesTable
                                                       columns:@[@"row", @"chip", @"mode", @"addr", @"count", @"data", @"notes"]
                                                        titles:@[@"Line", @"Chip", @"Mode", @"Fetch", @"Bytes", @"Data", @"Notes"]
                                                        widths:@[@36, @36, @170, @60, @40, @130, @60]]];
    }
    {   /* Timing */
        _beam = [[NSProgressIndicator alloc] init];
        _beam.indeterminate = NO;
        _beam.minValue = 0;
        _beam.maxValue = 1000;
        _timingText = monoLabel(370);
        NSStackView *page = [NSStackView stackViewWithViews:@[_beam, _timingText]];
        page.orientation = NSUserInterfaceLayoutOrientationVertical;
        page.alignment = NSLayoutAttributeLeading;
        page.spacing = 8;
        [_beam.widthAnchor constraintEqualToAnchor:page.widthAnchor].active = YES;
        [self addSection:@"Timing" content:page];
    }
    [_sections.widthAnchor constraintEqualToConstant:520].active = YES;

    NSStackView *root = [NSStackView stackViewWithViews:@[left, _sections]];
    root.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    root.alignment = NSLayoutAttributeTop;
    root.spacing = 10;
    _root = root;
    [left setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];

    /* start from the controller's state (the COCO_DEBUGGER_VIDEO_* variables) */
    _updating = YES;
    [_viewPopup selectItemAtIndex:cocovid_tab_view(_tab)];
    for (int index = 0; index < zoomCount; index++)
        if (zooms[index] == cocovid_tab_zoom(_tab)) [_zoomPopup selectItemAtIndex:index];
    for (int index = 0; index < overlayCount; index++)
        _overlayBox[index].state = (cocovid_tab_overlays(_tab) & overlays[index].bit) ? NSControlStateValueOn
                                                                             : NSControlStateValueOff;
    const int section = cocovid_tab_env_section(_tab);
    if (section > 0 && section < (int)_sections.numberOfTabViewItems) [_sections selectTabViewItemAtIndex:section];
    _updating = NO;
}

/* Sets a table up and returns the scroll view that holds it. */
- (NSScrollView *)scrollForTable:(NSTableView *)table
                         columns:(NSArray<NSString *> *)ids
                          titles:(NSArray<NSString *> *)titles
                          widths:(NSArray<NSNumber *> *)widths
{
    for (NSUInteger index = 0; index < ids.count; index++) {
        NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:ids[index]];
        column.title = titles[index];
        column.width = widths[index].doubleValue;
        [table addTableColumn:column];
    }
    table.dataSource = self;
    table.delegate = self;
    table.usesAlternatingRowBackgroundColors = YES;
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
    scroll.hasVerticalScroller = YES;
    scroll.documentView = table;
    return scroll;
}

- (void)addSection:(NSString *)title content:(NSView *)content
{
    NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:title];
    item.label = title;
    NSView *holder = [[NSView alloc] init];
    [holder addSubview:content];
    content.translatesAutoresizingMaskIntoConstraints = NO;
    [content.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor constant:6].active = YES;
    [content.trailingAnchor constraintEqualToAnchor:holder.trailingAnchor constant:-6].active = YES;
    [content.topAnchor constraintEqualToAnchor:holder.topAnchor constant:6].active = YES;
    if ([content isKindOfClass:[NSScrollView class]])
        [content.bottomAnchor constraintEqualToAnchor:holder.bottomAnchor constant:-6].active = YES;
    item.view = holder;
    [_sections addTabViewItem:item];
}

/* ---- actions -------------------------------------------------------------------------- */

- (void)onView:(id)sender
{
    (void)sender;
    if (_updating) return;
    cocovid_tab_set_view(_tab, (int)_viewPopup.indexOfSelectedItem);
    [self refreshView:COCOVID_TAB_PICTURE | COCOVID_TAB_TABLES];
}

- (void)onZoom:(id)sender
{
    (void)sender;
    int zoomCount = 0;
    const int *zooms = cocovid_zooms(&zoomCount);
    const NSInteger index = _zoomPopup.indexOfSelectedItem;
    if (_updating || index < 0 || index >= zoomCount) return;
    cocovid_tab_set_zoom(_tab, zooms[index]);
    [self refreshView:COCOVID_TAB_PICTURE];
}

- (void)onOverlay:(id)sender
{
    (void)sender;
    if (_updating) return;
    int overlayCount = 0;
    const cocovid_overlay_info *overlays = cocovid_overlays(&overlayCount);
    if (overlayCount > MAX_OVERLAYS) overlayCount = MAX_OVERLAYS;
    unsigned mask = 0;
    for (int index = 0; index < overlayCount; index++)
        if (_overlayBox[index].state == NSControlStateValueOn) mask |= overlays[index].bit;
    cocovid_tab_set_overlays(_tab, mask);
    [self refreshView:COCOVID_TAB_PICTURE];
}

/* A write breakpoint on the RAM byte the pixel was fetched from, with a note of what happened. */
- (void)breakOnPixel
{
    const int ident = cocovid_tab_break_on_pixel(_tab);
    _note.stringValue = ident >= 0 ? @"Break on writes to the pixel's byte (see Breakpoints)"
                                   : @"Nothing to break on: the pixel has no fetched byte, or the machine is running";
}

- (void)onBreak:(id)sender
{
    (void)sender;
    [self breakOnPixel];
}

- (void)onUnpin:(id)sender
{
    (void)sender;
    cocovid_tab_unpin(_tab);
    [_picture setNeedsDisplay:YES];
    [self updatePixel];
}

- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    (void)tabView;
    (void)item;
    if (!_updating) [self refreshView:COCOVID_TAB_TABLES];
}

/* The picture's room changed: Fit needs a new picture. */
- (void)viewportChanged
{
    const NSSize viewport = _scroll.contentSize;
    if ((int)viewport.width == _viewportWidth && (int)viewport.height == _viewportHeight) return;
    if (cocovid_tab_zoom(_tab) == 0) {
        [self refreshView:COCOVID_TAB_PICTURE];
    } else {
        _viewportWidth = (int)viewport.width;
        _viewportHeight = (int)viewport.height;
        cocovid_tab_set_viewport(_tab, _viewportWidth > 2 ? _viewportWidth - 2 : 1,
                                _viewportHeight > 2 ? _viewportHeight - 2 : 1);
    }
}

/* ---- the sections ----------------------------------------------------------------------- */

- (void)updatePixel
{
    const cocovid_pixel *pixel = cocovid_tab_pixel(_tab);
    _pixelText.stringValue = pixel ? str(pixel->text)
                                   : @"Move over the picture to inspect a pixel.\nClick to pin it; click again to unpin.\n"
                                      "Double-click to break on writes to it.";
    _pixelBreak.enabled = cocovid_tab_can_break(_tab) != 0;
    _breakButton.enabled = _pixelBreak.enabled;
    /* the swatch rows mark the inspected colour */
    if ([self sectionIndex] == COCOVID_S_COLOURS) [self fillColours];
}

- (NSInteger)sectionIndex
{
    return (NSInteger)[_sections.tabViewItems indexOfObject:_sections.selectedTabViewItem];
}

static NSString *hexBytes(const uint8_t *values, int count)
{
    NSMutableString *text = [NSMutableString string];
    for (int index = 0; index < count; index++) [text appendFormat:@"%s%02X", index ? " " : "", values[index]];
    return text;
}

static NSString *changeNotes(int mask)
{
    NSMutableString *text = [NSMutableString string];
    if (mask & COCOVID_LC_ADDR) [text appendString:@"A "];
    if (mask & COCOVID_LC_MODE) [text appendString:@"M "];
    if (mask & COCOVID_LC_PALETTE) [text appendString:@"P "];
    if (mask & COCOVID_LC_SIZE) [text appendString:@"N "];
    if (mask & COCOVID_LC_MID) [text appendString:@"~ "];
    return text;
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)table
{
    return table == _linesTable ? _lineCount : _regCount;
}

- (NSView *)tableView:(NSTableView *)table viewForTableColumn:(NSTableColumn *)column row:(NSInteger)row
{
    NSString *text = @"";
    BOOL hot = NO;
    if (table == _regsTable) {
        const cocovid_reg *regs = NULL;
        const int count = cocovid_tab_regs(_tab, &regs);
        if (!regs || row < 0 || row >= count) return nil;
        if ([column.identifier isEqualToString:@"name"]) text = str(regs[row].name);
        else if ([column.identifier isEqualToString:@"val"]) text = str(regs[row].value);
        else text = str(regs[row].meaning);
        hot = regs[row].age < COCOVID_REG_FADE;
    } else {
        const cocovid_line_row *lines = NULL;
        const int count = cocovid_tab_lines(_tab, &lines);
        if (!lines || row < 0 || row >= count) return nil;
        NSString *identifier = column.identifier;
        if ([identifier isEqualToString:@"row"]) text = [NSString stringWithFormat:@"%d", lines[row].row];
        else if ([identifier isEqualToString:@"chip"]) text = [NSString stringWithFormat:@"%d", lines[row].chip_line];
        else if ([identifier isEqualToString:@"mode"]) text = lines[row].valid ? str(lines[row].mode) : @"";
        else if (!lines[row].valid) text = [identifier isEqualToString:@"notes"] ? @"no record" : @"";
        else if ([identifier isEqualToString:@"addr"]) text = lines[row].display ? [NSString stringWithFormat:@"%05X", lines[row].addr] : @"";
        else if ([identifier isEqualToString:@"count"]) text = [NSString stringWithFormat:@"%d", lines[row].nbytes];
        else if ([identifier isEqualToString:@"data"])
            text = hexBytes(lines[row].data, lines[row].nbytes < 8 ? lines[row].nbytes : 8);
        else text = [NSString stringWithFormat:@"%@%@", lines[row].fresh ? @"" : @"last ",
                                               changeNotes(lines[row].changed_mask)];
        hot = lines[row].changed_mask != 0;
    }
    NSTextField *field = [table makeViewWithIdentifier:column.identifier owner:self];
    if (![field isKindOfClass:[NSTextField class]]) {
        field = [NSTextField labelWithString:@""];
        field.identifier = column.identifier;
        field.font = monoFont11();
    }
    field.stringValue = text;
    field.drawsBackground = hot;
    field.backgroundColor = hot ? accentFill() : NSColor.clearColor;
    field.textColor = hot ? accentInk() : NSColor.labelColor;
    return field;
}

- (void)fillRegs
{
    const cocovid_reg *regs = NULL;
    const int count = cocovid_tab_regs(_tab, &regs);
    NSMutableString *signature = [NSMutableString string];
    for (int index = 0; regs && index < count; index++)
        [signature appendFormat:@"%s%c", regs[index].value, regs[index].age < COCOVID_REG_FADE ? '*' : '.'];
    [signature appendFormat:@"%d", count];
    if ([signature isEqualToString:_regsSignature]) return;
    _regsSignature = signature;
    _regCount = regs ? count : 0;
    [_regsTable reloadData];
}

- (void)fillLines
{
    const cocovid_line_row *lines = NULL;
    const int count = cocovid_tab_lines(_tab, &lines);
    NSMutableString *signature = [NSMutableString string];
    for (int index = 0; lines && index < count; index++) {
        [signature appendFormat:@"%d:%d:%d:%d:%d:%X:%d:%s:", lines[index].row, lines[index].chip_line, lines[index].valid,
                                lines[index].fresh, lines[index].nbytes, lines[index].addr, lines[index].display, lines[index].mode];
        [signature appendString:hexBytes(lines[index].data, 8)];
        [signature appendFormat:@"%d;", lines[index].changed_mask];
    }
    [signature appendFormat:@"%d", count];
    if ([signature isEqualToString:_linesSignature]) return;
    _linesSignature = signature;
    _lineCount = lines ? count : 0;
    [_linesTable reloadData];
}

- (void)fillColours
{
    const cocovid_swatch *swatches = NULL;
    const int count = cocovid_tab_swatches(_tab, &swatches);
    for (int index = 0; index < SWATCH_ROWS; index++) {
        _swatchRows[(NSUInteger)index].hidden = !swatches || index >= count;
        if (!swatches || index >= count) {
            _swatchLabel[index].stringValue = @"";
            _swatchView[index].color = nil;
            continue;
        }
        _swatchView[index].color = colorFromRGB(swatches[index].rgb);
        _swatchLabel[index].stringValue = [NSString stringWithFormat:@"%@%-8s #%06X  %7ld px  %@",
                                                                 swatches[index].inspected ? @"> " : @"  ",
                                                                 swatches[index].name, swatches[index].rgb & 0xFFFFFF,
                                                                 swatches[index].uses, str(swatches[index].detail)];
    }
}

- (void)fillTiming
{
    const char *const *lines = NULL;
    const int count = cocovid_tab_timing(_tab, &lines);
    NSMutableString *text = [NSMutableString string];
    for (int index = 0; lines && index < count; index++) {
        NSString *line = str(lines[index]);
        const NSRange tab = [line rangeOfString:@"\t"];
        if (tab.location != NSNotFound)
            [text appendFormat:@"%-13s %@\n", [[line substringToIndex:tab.location] UTF8String],
                               [line substringFromIndex:tab.location + 1]];
        else
            [text appendFormat:@"%@\n", line];
    }
    _timingText.stringValue = count ? text : @"The machine is off";
    _beam.doubleValue = cocovid_tab_beam_permille(_tab);
}

/* ---- refresh ------------------------------------------------------------------------------ */

- (void)refreshView:(unsigned)what
{
    if (what & COCOVID_TAB_PICTURE) {
        const NSSize viewport = _scroll.contentSize;
        _viewportWidth = (int)viewport.width;
        _viewportHeight = (int)viewport.height;
        if (viewport.width > 4 && viewport.height > 4)
            cocovid_tab_set_viewport(_tab, (int)viewport.width - 2, (int)viewport.height - 2);
        [_picture sync];
        [_picture setFrameSize:_picture.intrinsicContentSize];
    }
    if (what & COCOVID_TAB_TABLES) {
        _status.stringValue = str(cocovid_tab_status(_tab));
        switch ([self sectionIndex]) {
        case COCOVID_S_REGISTERS: [self fillRegs]; break;
        case COCOVID_S_COLOURS: [self fillColours]; break;
        case COCOVID_S_LINES: [self fillLines]; break;
        case COCOVID_S_TIMING: [self fillTiming]; break;
        default: break;
        }
        const cocovid_pixel *pixel = cocovid_tab_pixel(_tab);
        _pixelText.stringValue = pixel ? str(pixel->text)
                                       : @"Move over the picture to inspect a pixel.\nClick to pin it; click again to unpin.\n"
                                          "Double-click to break on writes to it.";
        _pixelBreak.enabled = cocovid_tab_can_break(_tab) != 0;
        _breakButton.enabled = _pixelBreak.enabled;
    }
}

- (void)refreshForce:(BOOL)force
{
    unsigned what = cocovid_tab_refresh(_tab, force ? 1 : 0);
    if (force) what |= COCOVID_TAB_PICTURE | COCOVID_TAB_TABLES;
    if (what & COCOVID_TAB_OFF) what |= COCOVID_TAB_PICTURE | COCOVID_TAB_TABLES;     /* the message */
    [self refreshView:what];
}

@end
