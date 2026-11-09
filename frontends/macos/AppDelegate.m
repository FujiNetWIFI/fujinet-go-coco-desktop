/*
 * AppDelegate: builds the menu bar and main window, owns the session
 * lifecycle, and hosts the FujiNet configuration (WKWebView) and console
 * log windows. Mirrors the GNOME/KDE/Windows frontends over the same
 * cocosession API, with the native debugger window in
 * debugger/DebuggerWindow.m.
 *
 * Ported from fujinet-go-adam-desktop's AppDelegate.m, not copied
 * line-for-line: the Machine/Display/Artifact-Colour/CPU menus are CoCo's
 * own live radio surface (cocosession_set_machine/_tv_input/_ccr/_cpu),
 * built at runtime by walking the session's own cocosession_*_name(idx)
 * tables -- the same shape as GNOME's radio_menu()/Windows'
 * append_radio_menu() -- with no ADAM equivalent to port from; ADAM's
 * ColecoVision-cartridge and machine-settings-window content (with no
 * CoCo counterpart) is dropped rather than translated. See
 * DisplayView.m's file comment for the video/keyboard differences.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "AppDelegate.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <WebKit/WebKit.h>

#import "DisplayView.h"
#import "controllers/ControllersWindow.h"
#import "help/HelpWindow.h"
#import "debugger/DebuggerWindow.h"

#include <stdlib.h>
#include <string.h>

/* How long a gamepad connect/disconnect message stays over the picture. */
#define TOAST_SECONDS 4.0
/* The most pads the Input page lists. */
#define SETTINGS_PAD_ROWS 8

static NSArray<UTType *> *typesForExtensions(NSArray<NSString *> *exts)
{
    NSMutableArray<UTType *> *types = [NSMutableArray array];
    for (NSString *ext in exts) {
        UTType *t = [UTType typeWithFilenameExtension:ext];
        if (t)
            [types addObject:t];
    }
    return types;
}

@interface AppDelegate () <NSWindowDelegate, NSTabViewDelegate, WKUIDelegate>
@end

@implementation AppDelegate {
    cocosession *_session;
    NSWindow *_window;
    DisplayView *_display;
    NSWindow *_configWindow;
    NSWindow *_logWindow;
    NSTextView *_logView;
    NSTimer *_logTimer;
    NSWindow *_settingsWindow;
    BOOL _sessionDirty;
    NSTextField *_externalField;
    NSView *_noRoms;
    NSMutableArray<NSTextField *> *_romStatus;
    NSString *_logShown;

    CocoControllersWindow *_controllers;
    NSTimer *_sysactTimer;
    NSTextField *_toast;                  /* the transient message over the picture */
    unsigned _toastGeneration;
    NSUInteger _toastSerial;
    BOOL _launched;

    NSStackView *_padBox;                 /* the Input page's pad rows */
    NSMutableArray<NSTextField *> *_padPorts;
    NSTimer *_padTimer;
    unsigned _padGeneration;
}

- (instancetype)initWithSession:(cocosession *)session
{
    self = [super init];
    if (self)
        _session = session;
    return self;
}

/* ---- session helpers ------------------------------------------------------ */

- (void)restartSession
{
    cocosession_stop(_session);
    cocosession_start_opts opts;
    cocosession_default_opts(_session, &opts);
    if (cocosession_start(_session, &opts) != 0)
        NSLog(@"session restart: %s", cocosession_last_error(_session));
    /* the controls start centred and released; pads that come back are no news */
    _toastGeneration = cocosession_gamepad_generation(_session);
    [_controllers refresh];
    [self showPage];
}

/* In place of the picture until the machine has its system ROMs. */
- (NSView *)buildNoRoms:(NSRect)frame
{
    NSImageView *icon =
        [NSImageView imageViewWithImage:[NSApp applicationIconImage]];
    [icon.widthAnchor constraintEqualToConstant:96].active = YES;
    [icon.heightAnchor constraintEqualToConstant:96].active = YES;
    NSTextField *title = [NSTextField labelWithString:@"No CoCo BIOS"];
    title.font = [NSFont boldSystemFontOfSize:22];
    NSTextField *desc = [NSTextField
        wrappingLabelWithString:
            @"The CoCo needs its system ROMs (BASIC), which are not part of "
            @"the app. Import the ones you have -- files, a zip such as "
            @"MAME's coco or coco3 set, or a folder -- and the CoCo starts."];
    desc.alignment = NSTextAlignmentCenter;
    desc.textColor = NSColor.secondaryLabelColor;
    desc.preferredMaxLayoutWidth = 460;
    [desc.widthAnchor constraintEqualToConstant:460].active = YES;
    NSButton *files = [NSButton buttonWithTitle:@"Import BIOS…"
                                         target:self
                                         action:@selector(importBios:)];
    files.bezelColor = NSColor.controlAccentColor;
    files.keyEquivalent = @"\r";
    NSButton *folder = [NSButton buttonWithTitle:@"Import a Folder…"
                                          target:self
                                          action:@selector(importBiosFromFolder:)];
    NSButton *helpBtn = CocoHelpButton(@"nobios");
    NSStackView *buttons =
        [NSStackView stackViewWithViews:@[ files, folder, helpBtn ]];
    buttons.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    buttons.spacing = 12;
    NSStackView *column =
        [NSStackView stackViewWithViews:@[ icon, title, desc, buttons ]];
    column.orientation = NSUserInterfaceLayoutOrientationVertical;
    column.alignment = NSLayoutAttributeCenterX;
    column.spacing = 14;
    column.translatesAutoresizingMaskIntoConstraints = NO;

    NSView *page = [[NSView alloc] initWithFrame:frame];
    page.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [page addSubview:column];
    [column.centerXAnchor constraintEqualToAnchor:page.centerXAnchor].active =
        YES;
    [column.centerYAnchor constraintEqualToAnchor:page.centerYAnchor].active =
        YES;
    return page;
}

/* The picture while the machine can run, else the No-BIOS page. */
- (void)showPage
{
    BOOL ok = cocosession_is_running(_session) ||
              cocosession_has_system_roms(_session,
                                          cocosession_machine(_session));
    _display.hidden = !ok;
    _noRoms.hidden = ok;
    CocoSetHelpContext(_window, ok ? @"main" : @"nobios");
    if (ok)
        [_window makeFirstResponder:_display];
}

/* Starts the machine once its ROMs are there (after an import). A machine
 * that is running was rebuilt in place by the core: nothing to do. */
- (void)startMachine
{
    if (!cocosession_is_running(_session) &&
        cocosession_has_system_roms(_session, cocosession_machine(_session)))
        [self restartSession];
    [self showPage];
}

- (void)applyDisplaySettings
{
    [_display setAspectMode:cocosession_get_int(_session, "aspect_mode", 0)];
    [_display setSmooth:cocosession_get_int(_session, "smooth_scaling", 0)];
}

/* ---- settings ----------------------------------------------------------------
 * Aspect ratio and smooth scaling apply live; FujiNet/Sound/Gamepads decide
 * what the session starts with, so -- like GNOME's Preferences dialog
 * (frontends/gnome/prefs.c), same keys and defaults, so a machine
 * configured in one frontend shows up the same in another -- those three
 * are collected while the window is open and applied in one restart when it
 * closes, mirroring ADAM's own Settings window
 * (fujinet-go-adam-desktop/frontends/macos/AppDelegate.m's showSettings:).
 * Machine/TV input/Artifact Colour/CPU are not duplicated here: they
 * already live in the main menu as live radio groups; ADAM's machine/
 * palette/expansion/joystick-mode/keypad rows have no CoCo equivalent at
 * all and are dropped rather than translated. */

static NSArray<NSString *> *sessionSettingKeys(void)
{
    static NSArray<NSString *> *keys;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
      keys = @[ @"enable_fujinet", @"enable_audio", @"enable_gamepad",
              @"fujinet_external", @"fujinet_external_addr" ];
    });
    return keys;
}

- (NSTextField *)sectionLabel:(NSString *)title
{
    NSTextField *label = [NSTextField labelWithString:title];
    label.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
    return label;
}

- (NSPopUpButton *)popUpForKey:(const char *)key
                       fallback:(int)def
                          items:(NSArray<NSString *> *)items
{
    NSPopUpButton *popup = [[NSPopUpButton alloc] init];
    [popup addItemsWithTitles:items];
    NSInteger current = cocosession_get_int(_session, key, def);
    if (current < 0 || current >= (NSInteger)items.count)
        current = def;
    [popup selectItemAtIndex:current];
    popup.identifier = @(key);
    popup.target = self;
    popup.action = @selector(settingChanged:);
    return popup;
}

- (NSButton *)checkBoxForKey:(const char *)key fallback:(int)def
{
    NSButton *box = [NSButton checkboxWithTitle:@""
                                         target:self
                                         action:@selector(settingChanged:)];
    box.state = cocosession_get_int(_session, key, def)
                    ? NSControlStateValueOn
                    : NSControlStateValueOff;
    box.identifier = @(key);
    return box;
}

- (void)settingChanged:(id)sender
{
    NSControl *control = sender;
    NSString *key = control.identifier;
    int value;

    if ([control isKindOfClass:[NSPopUpButton class]])
        value = (int)((NSPopUpButton *)control).indexOfSelectedItem;
    else
        value = ((NSButton *)control).state == NSControlStateValueOn ? 1 : 0;

    if ([key isEqualToString:@"analog_joystick"]) {
        /* live, and remembered by the core */
        cocosession_set_analog(_session, value);
        return;
    }
    cocosession_set_int(_session, key.UTF8String, value);
    if ([key isEqualToString:@"fujinet_external"])
        _externalField.enabled = value != 0;

    if ([sessionSettingKeys() containsObject:key])
        _sessionDirty = YES;
    else
        [self applyDisplaySettings];
}

/* The external-FujiNet address field: valid text (or none, meaning
 * 127.0.0.1:65504) is saved; anything else is shown in red and not saved. */
- (void)externalAddrChanged:(id)sender
{
    NSTextField *field = sender;
    NSString *text = [field.stringValue
        stringByTrimmingCharactersInSet:NSCharacterSet
                                            .whitespaceAndNewlineCharacterSet];
    char host[64];
    int port;
    if (cocosession_parse_hostport(text.UTF8String, host, sizeof host,
                                   &port) != 0) {
        field.textColor = NSColor.systemRedColor;
        return;
    }
    field.textColor = NSColor.controlTextColor;
    cocosession_set_str(_session, "fujinet_external_addr", text.UTF8String);
    _sessionDirty = YES;
}

/* One Settings page: the rows in a grid, an optional note under them, and the
 * page's Help button. */
- (NSView *)settingsPage:(NSArray<NSArray<NSView *> *> *)rows
                    note:(NSString *)note
                 context:(NSString *)context
{
    NSGridView *grid = [NSGridView gridViewWithViews:rows];
    grid.rowSpacing = 8;
    grid.columnSpacing = 12;
    [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;

    NSMutableArray<NSView *> *views = [NSMutableArray arrayWithObject:grid];
    if (note.length) {
        NSTextField *label = [NSTextField labelWithString:note];
        label.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
        label.textColor = NSColor.secondaryLabelColor;
        [views addObject:label];
    }
    NSStackView *stack = [NSStackView stackViewWithViews:views];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 12;
    stack.translatesAutoresizingMaskIntoConstraints = NO;

    NSView *holder = [[NSView alloc] init];
    [holder addSubview:stack];
    [stack.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor constant:16].active = YES;
    [stack.topAnchor constraintEqualToAnchor:holder.topAnchor constant:16].active = YES;

    NSButton *helpBtn = CocoHelpButton(context);
    helpBtn.translatesAutoresizingMaskIntoConstraints = NO;
    [holder addSubview:helpBtn];
    [helpBtn.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor constant:16].active = YES;
    [helpBtn.bottomAnchor constraintEqualToAnchor:holder.bottomAnchor constant:-12].active = YES;
    return holder;
}

/* The General page: the picture and the FujiNet. */
- (NSView *)generalPage
{
    _externalField = [NSTextField textFieldWithString:@(
        cocosession_get_str(_session, "fujinet_external_addr", ""))];
    _externalField.placeholderString = @"127.0.0.1:65504";
    _externalField.preferredMaxLayoutWidth = 240;
    _externalField.enabled =
        cocosession_get_int(_session, "fujinet_external", 0) != 0;
    _externalField.target = self;
    _externalField.action = @selector(externalAddrChanged:);
    [_externalField.cell setSendsActionOnEndEditing:YES];

    NSArray<NSArray<NSView *> *> *rows = @[
        @[ [self sectionLabel:@"Display"], [NSTextField labelWithString:@""] ],
        @[
            [NSTextField labelWithString:@"Aspect ratio"],
            [self popUpForKey:"aspect_mode"
                     fallback:0
                        items:@[
                            @"TV (4:3)", @"Square pixels", @"Integer scale"
                        ]]
        ],
        @[
            [NSTextField labelWithString:@"Smooth scaling"],
            [self checkBoxForKey:"smooth_scaling" fallback:0]
        ],
        @[ [self sectionLabel:@"FujiNet"], [NSTextField labelWithString:@""] ],
        @[
            [NSTextField labelWithString:@"FujiNet"],
            [self checkBoxForKey:"enable_fujinet" fallback:1]
        ],
        @[
            [NSTextField labelWithString:@"Use external FujiNet"],
            [self checkBoxForKey:"fujinet_external" fallback:0]
        ],
        @[
            [NSTextField labelWithString:@"Address:port"],
            _externalField
        ],
        @[
            [NSTextField labelWithString:@"Sound"],
            [self checkBoxForKey:"enable_audio" fallback:1]
        ],
    ];
    return [self settingsPage:rows
                         note:@"Display options apply immediately. FujiNet options "
                              @"take effect when this window is closed.\nAn external "
                              @"FujiNet is reached at the address above."
                      context:@"prefs/Machine"];
}

/* The Input page: gamepads on or off, the stick, the port of each pad, and the
 * way to the Controllers window, where the keys and buttons are mapped. */
- (NSView *)inputPage
{
    NSButton *resetBtn = [NSButton buttonWithTitle:@"Reset Controls"
                                            target:self
                                            action:@selector(resetControls:)];
    NSButton *openBtn = [NSButton buttonWithTitle:@"Open Controllers"
                                           target:self
                                           action:@selector(openControllers:)];
    NSStackView *buttons = [NSStackView stackViewWithViews:@[ resetBtn, openBtn ]];
    buttons.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    buttons.spacing = 8;

    _padBox = [NSStackView stackViewWithViews:@[]];
    _padBox.orientation = NSUserInterfaceLayoutOrientationVertical;
    _padBox.alignment = NSLayoutAttributeLeading;
    _padBox.spacing = 6;
    [self rebuildPadRows];

    NSArray<NSArray<NSView *> *> *rows = @[
        @[ [self sectionLabel:@"Input"], [NSTextField labelWithString:@""] ],
        @[
            [NSTextField labelWithString:@"Gamepads"],
            [self checkBoxForKey:"enable_gamepad" fallback:1]
        ],
        @[
            [NSTextField labelWithString:@"Analog joystick"],
            [self checkBoxForKey:"analog_joystick" fallback:1]
        ],
        @[ [NSTextField labelWithString:@"Pads"], _padBox ],
        @[ [NSTextField labelWithString:@"Controls"], buttons ],
    ];
    return [self settingsPage:rows
                         note:@"Analog joystick: a pad's left stick moves the joystick "
                              @"in proportion; off, it is an eight-way switch.\nKeys and "
                              @"gamepad buttons are mapped in the Controllers window "
                              @"(View \u25B8 Controllers, Cmd+Shift+K). Gamepads on or off "
                              @"takes effect when this window is closed."
                      context:@"prefs/Input"];
}

/* The ROMs page: one row per item under its group, refreshed after an import. */
- (NSView *)romsPage
{
    NSMutableArray<NSArray<NSView *> *> *all = [NSMutableArray array];
    _romStatus = [NSMutableArray array];
    const char *group = "";
    for (int i = 0; i < cocosession_rom_item_count(); i++) {
        const coco_rom_item *it = cocosession_rom_item(i);
        if (strcmp(group, it->group) != 0) {
            group = it->group;
            NSTextField *g = [NSTextField
                labelWithString:[NSString stringWithUTF8String:group]];
            g.textColor = NSColor.secondaryLabelColor;
            [all addObject:@[ g, [NSTextField labelWithString:@""] ]];
        }
        NSTextField *st = [NSTextField labelWithString:@""];
        [_romStatus addObject:st];
        [all addObject:@[
            [NSTextField
                labelWithString:[NSString stringWithUTF8String:it->name]],
            st
        ]];
    }
    NSButton *impFiles = [NSButton buttonWithTitle:@"Import…"
                                            target:self
                                            action:@selector(importBios:)];
    NSButton *impFolder = [NSButton buttonWithTitle:@"Import a Folder…"
                                             target:self
                                             action:@selector(importBiosFromFolder:)];
    NSStackView *impRow =
        [NSStackView stackViewWithViews:@[ impFiles, impFolder ]];
    impRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    impRow.spacing = 8;
    [all addObject:@[ [NSTextField labelWithString:@""], impRow ]];
    [self refreshRomRows];
    return [self settingsPage:all note:nil context:@"prefs/ROMs"];
}

static NSString *const kSettingsContexts[] = {
    @"prefs/Machine", @"prefs/Input", @"prefs/ROMs"
};

- (void)showSettings:(id)sender
{
    (void)sender;
    if (_settingsWindow) {
        [self rebuildPadRows];
        [self startPadTimer];
        [_settingsWindow makeKeyAndOrderFront:nil];
        return;
    }

    NSArray<NSArray *> *pages = @[
        @[ @"General", [self generalPage] ],
        @[ @"Input", [self inputPage] ],
        @[ @"ROMs", [self romsPage] ],
    ];
    CGFloat wide = 0, high = 0;
    NSTabView *tabs = [[NSTabView alloc] init];
    for (NSArray *page in pages) {
        NSView *holder = page[1];
        [holder layoutSubtreeIfNeeded];
        const NSSize fit = ((NSView *)holder.subviews.firstObject).fittingSize;
        wide = MAX(wide, fit.width);
        high = MAX(high, fit.height);
        NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:page[0]];
        item.label = page[0];
        item.view = holder;
        [tabs addTabViewItem:item];
    }
    /* room for the tabs themselves, the margins and the Help button */
    const NSSize size = NSMakeSize(MAX(wide + 56, 440), high + 100);
    tabs.frame = NSMakeRect(0, 0, size.width, size.height);
    tabs.delegate = self;
    _settingsWindow = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, size.width, size.height)
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _settingsWindow.title = @"Settings";
    _settingsWindow.releasedWhenClosed = NO;
    _settingsWindow.delegate = self;
    _settingsWindow.contentView = tabs;
    CocoSetHelpContext(_settingsWindow, kSettingsContexts[0]);
    [_settingsWindow center];
    [self startPadTimer];
    [_settingsWindow makeKeyAndOrderFront:nil];
}

/* Command+? and the Help button follow the page showing. */
- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    const NSInteger idx = [tabView indexOfTabViewItem:item];
    if (idx >= 0 && idx < 3)
        CocoSetHelpContext(_settingsWindow, kSettingsContexts[idx]);
}

/* ---- gamepads on the Input page ---------------------------------------------- */

- (void)refreshPadPorts
{
    for (NSUInteger row = 0; row < _padPorts.count; row++) {
        const int port = cocosession_gamepad_effective_port(_session, (int)row);
        NSString *text = port == 0 ? @"drives the right joystick"
                       : (port == 1 ? @"drives the left joystick" : @"drives nothing");
        if (![_padPorts[row].stringValue isEqualToString:text])
            _padPorts[row].stringValue = text;
    }
}

/* One row per connected pad: its name, the joystick it is set to
 * (Automatic, Right, Left) and the one it drives now. Rebuilt when the
 * generation changes (a pad came or went). */
- (void)rebuildPadRows
{
    for (NSView *row in [_padBox.arrangedSubviews copy]) {
        [_padBox removeArrangedSubview:row];
        [row removeFromSuperview];
    }
    _padPorts = [NSMutableArray array];
    _padGeneration = cocosession_gamepad_generation(_session);
    const int count = MIN(cocosession_gamepad_count(_session), SETTINGS_PAD_ROWS);
    if (count <= 0) {
        NSTextField *none = [NSTextField labelWithString:@"No gamepads connected"];
        none.textColor = NSColor.secondaryLabelColor;
        [_padBox addArrangedSubview:none];
        return;
    }
    for (int idx = 0; idx < count; idx++) {
        char name[128];
        name[0] = '\0';
        cocosession_gamepad_name(_session, idx, name, (int)sizeof name);
        NSTextField *label = [NSTextField labelWithString:[NSString stringWithUTF8String:name]
                                                      ?: @""];
        label.lineBreakMode = NSLineBreakByTruncatingTail;
        label.toolTip = label.stringValue;
        [label.widthAnchor constraintEqualToConstant:200].active = YES;

        NSPopUpButton *popup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        [popup addItemsWithTitles:@[ @"Automatic", @"Right joystick", @"Left joystick" ]];
        const int assign = cocosession_gamepad_assignment(_session, idx);
        [popup selectItemAtIndex:(assign < 0 || assign > 1 ? 0 : assign + 1)];
        popup.tag = idx;
        popup.target = self;
        popup.action = @selector(padAssignChanged:);

        NSTextField *eff = [NSTextField labelWithString:@""];
        eff.textColor = NSColor.secondaryLabelColor;
        [_padPorts addObject:eff];

        NSStackView *row = [NSStackView stackViewWithViews:@[ label, popup, eff ]];
        row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
        row.spacing = 8;
        [_padBox addArrangedSubview:row];
    }
    [self refreshPadPorts];
}

- (void)padAssignChanged:(NSPopUpButton *)sender
{
    const int sel = (int)sender.indexOfSelectedItem;
    cocosession_gamepad_assign(_session, (int)sender.tag, sel <= 0 ? -1 : sel - 1);
    [self refreshPadPorts];
}

/* Hot-plugged pads show up in the list while the window is open. */
- (void)startPadTimer
{
    if (_padTimer) return;
    __weak AppDelegate *weakSelf = self;
    _padTimer = [NSTimer scheduledTimerWithTimeInterval:0.5
                                                repeats:YES
                                                  block:^(NSTimer *timer) {
        (void)timer;
        AppDelegate *strongSelf = weakSelf;
        if (!strongSelf) return;
        if (cocosession_gamepad_generation(strongSelf->_session) != strongSelf->_padGeneration)
            [strongSelf rebuildPadRows];
        else
            [strongSelf refreshPadPorts];
    }];
    _padTimer.tolerance = 0.25;
}

- (void)resetControls:(id)sender
{
    (void)sender;
    cocosession_bindings_reset(_session);
    [_controllers refresh];
}

- (void)openControllers:(id)sender
{
    (void)sender;
    if (![_controllers isVisible])
        [_controllers toggle];
}

- (void)refreshRomRows
{
    for (NSUInteger i = 0; i < _romStatus.count; i++)
        _romStatus[i].stringValue =
            cocosession_rom_item_available(_session, (int)i) ? @"Imported"
                                                             : @"Not imported";
}

/* FujiNet options are collected while the window is open and applied in one
 * restart when it closes. */
- (void)windowWillClose:(NSNotification *)note
{
    if (note.object == _settingsWindow) {
        [_settingsWindow makeFirstResponder:nil]; /* commit a field edit */
        [_padTimer invalidate];
        _padTimer = nil;
    }
    if (note.object != _settingsWindow || !_sessionDirty)
        return;
    _sessionDirty = NO;
    [self restartSession];
}

/* ---- actions ---------------------------------------------------------------- */

- (void)resetMachine:(id)sender
{
    (void)sender;
    if (cocosession_is_running(_session))
        cocosession_reset(_session);
}

- (void)importMedia:(id)sender
{
    (void)sender;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.title = @"Import Disk or Tape Image";
    panel.allowedContentTypes =
        typesForExtensions(@[ @"dsk", @"os9", @"vdk", @"cas", @"wav" ]);
    if ([panel runModal] != NSModalResponseOK || !panel.URL)
        return;
    char dest[1024];
    if (cocosession_import_media(_session, panel.URL.path.UTF8String, dest,
                                 sizeof(dest)) != 0)
        NSLog(@"import: %s", cocosession_last_error(_session));
}

/* Each chosen path through the import (the core rebuilds a running machine
 * in place); the machine starts if it now can, then one alert says what came
 * of it all. */
- (void)importBiosURLs:(NSArray<NSURL *> *)urls
{
    NSMutableArray<NSString *> *log = [NSMutableArray array];
    int imported = 0;
    for (NSURL *u in urls) {
        char msg[1024];
        msg[0] = '\0';
        if (cocosession_rom_import(_session, u.fileSystemRepresentation, msg,
                                   (int)sizeof msg) == 0)
            imported++;
        if (msg[0])
            [log addObject:@(msg)];
    }
    [self startMachine];
    [self refreshRomRows];
    if (log.count) {
        NSAlert *a = [[NSAlert alloc] init];
        a.messageText = imported ? @"BIOS Imported" : @"Nothing Imported";
        a.informativeText = [log componentsJoinedByString:@"\n"];
        [a runModal];
    }
}

- (void)importBiosChoosingFolder:(BOOL)folder
{
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.message = folder ? @"Import BIOS from a Folder (MAME's coco set works)"
                           : @"Import BIOS: ROM images, zips or folders";
    panel.canChooseDirectories = YES;
    panel.canChooseFiles = !folder;
    panel.allowsMultipleSelection = !folder;
    if ([panel runModal] != NSModalResponseOK || panel.URLs.count == 0)
        return;
    [self importBiosURLs:panel.URLs];
}

- (void)importBios:(id)sender
{
    (void)sender;
    [self importBiosChoosingFolder:NO];
}

- (void)importBiosFromFolder:(id)sender
{
    (void)sender;
    [self importBiosChoosingFolder:YES];
}

- (void)showFujiNetConfig:(id)sender
{
    (void)sender;
    if (cocosession_fujinet_external(_session)) {
        NSAlert *alert = [[NSAlert alloc] init];
        alert.messageText = @"FujiNet is external";
        alert.informativeText = @"Use the web UI of the FujiNet instance "
                                @"this app connects to.";
        [alert runModal];
        return;
    }
    if (_configWindow) {
        [_configWindow makeKeyAndOrderFront:nil];
        return;
    }
    NSRect frame = NSMakeRect(0, 0, 1000, 760);
    /* Miniaturizable so Window ▸ Minimize (and the yellow button) apply to
     * the auxiliary windows too, not just the machine window. */
    _configWindow = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable |
                            NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _configWindow.title = @"FujiNet Configuration";
    _configWindow.releasedWhenClosed = NO;
    WKWebView *web = [[WKWebView alloc] initWithFrame:frame];
    web.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    web.UIDelegate = self;
    NSString *url = [NSString
        stringWithUTF8String:cocosession_fujinet_webui_url(_session)];
    [web loadRequest:[NSURLRequest
                         requestWithURL:[NSURL URLWithString:url]]];
    _configWindow.contentView = web;
    [_configWindow center];
    [_configWindow makeKeyAndOrderFront:nil];
}

/* The FujiNet web UI's OneDrive/Google Drive "Authorize" buttons open the
 * provider's consent page via window.open(). WKWebView has no popup window
 * of its own to show it in unless a UIDelegate supplies one, so hand the URL
 * to the system browser instead: Google and Microsoft both reject OAuth
 * flows from an embedded webview's user-agent anyway. */
- (WKWebView *)webView:(WKWebView *)webView
    createWebViewWithConfiguration:(WKWebViewConfiguration *)configuration
               forNavigationAction:(WKNavigationAction *)navigationAction
                    windowFeatures:(WKWindowFeatures *)windowFeatures
{
    (void)webView;
    (void)configuration;
    (void)windowFeatures;
    if (navigationAction.request.URL)
        [[NSWorkspace sharedWorkspace] openURL:navigationAction.request.URL];
    return nil;
}

- (void)showFujiNetLog:(id)sender
{
    (void)sender;
    if (cocosession_fujinet_external(_session)) {
        NSAlert *alert = [[NSAlert alloc] init];
        alert.messageText = @"FujiNet is external";
        alert.informativeText = @"Its console output is in the process "
                                @"that is running it.";
        [alert runModal];
        return;
    }
    if (_logWindow) {
        [_logWindow makeKeyAndOrderFront:nil];
        return;
    }
    NSRect frame = NSMakeRect(0, 0, 820, 560);
    _logWindow = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable |
                            NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _logWindow.title = @"FujiNet Console Log";
    _logWindow.releasedWhenClosed = NO;

    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 36, frame.size.width, frame.size.height - 36)];
    scroll.hasVerticalScroller = YES;
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _logView = [[NSTextView alloc] initWithFrame:frame];
    _logView.editable = NO;
    _logView.font = [NSFont monospacedSystemFontOfSize:11
                                                weight:NSFontWeightRegular];
    scroll.documentView = _logView;
    {
        /* the log over a strip with the Help button */
        NSView *box = [[NSView alloc] initWithFrame:frame];
        NSButton *help = CocoHelpButton(@"fujilog");
        [help setFrameOrigin:NSMakePoint(10, 6)];
        help.autoresizingMask = NSViewMaxXMargin | NSViewMaxYMargin;
        [box addSubview:scroll];
        [box addSubview:help];
        _logWindow.contentView = box;
    }
    CocoSetHelpContext(_logWindow, @"fujilog");

    _logTimer = [NSTimer
        scheduledTimerWithTimeInterval:1.0
                               repeats:YES
                                 block:^(NSTimer *timer) {
                                   (void)timer;
                                   [self refreshLog];
                                 }];
    [self refreshLog];
    [_logWindow center];
    [_logWindow makeKeyAndOrderFront:nil];
    /* the window's layout is final only now: land on the tail */
    dispatch_async(dispatch_get_main_queue(), ^{
      [self->_logView scrollToEndOfDocument:nil];
    });
}

/* Replaces the text only when it changed; follows the tail only while the
 * reader is at the bottom (the first fill starts there). */
- (void)refreshLog
{
    static char buf[128 * 1024];
    int n = cocosession_fujinet_copy_log(_session, buf, sizeof(buf));
    NSString *text = n > 0 ? [NSString stringWithUTF8String:buf]
                           : @"(no FujiNet output yet)";
    if (!text || [text isEqualToString:_logShown])
        return;
    NSScrollView *scroll = _logView.enclosingScrollView;
    NSRect vis = scroll.documentVisibleRect;
    BOOL bottom = !_logShown || NSMaxY(vis) >= NSMaxY(_logView.bounds) - 4;
    _logShown = text;
    _logView.string = text;
    if (bottom) {
        [_logView.layoutManager
            ensureLayoutForTextContainer:_logView.textContainer];
        [_logView scrollToEndOfDocument:nil];
    } else {
        [_logView scrollPoint:vis.origin];
    }
}

- (void)showDebugger:(id)sender
{
    (void)sender;
    [DebuggerWindow showForSession:_session];
}

- (void)toggleControllers:(id)sender
{
    (void)sender;
    [_controllers toggle];
}

/* ---- the sysaction timer and the toast ---------------------------------------
 * The gamepad thread and a key bound to a system action cannot touch AppKit:
 * they post the action, and this timer (50 ms, while the machine's window can
 * be seen) takes it. The same tick shows a gamepad coming or going. */

- (void)runSysaction:(int)sact
{
    if (sact == COCO_SYSACT_DEBUG_STOP) {
        /* engages and pauses; the debugger window then opens on it */
        cocosession_sysaction(_session, sact);
        [DebuggerWindow showForSession:_session];
    }
}

- (void)sysactTick
{
    int sact;
    while (cocosession_sysaction_take(_session, &sact))
        [self runSysaction:sact];
    const unsigned gen = cocosession_gamepad_generation(_session);
    if (gen != _toastGeneration) {
        _toastGeneration = gen;
        char msg[160];
        if (cocosession_gamepad_last_event(_session, msg, sizeof msg) > 0)
            [self showToast:[NSString stringWithUTF8String:msg]];
    }
}

- (BOOL)windowCanBeSeen:(NSWindow *)win
{
    return win && [win isVisible] && ![win isMiniaturized] &&
           ([win occlusionState] & NSWindowOcclusionStateVisible) && ![NSApp isHidden];
}

/* The timer runs while the machine's window can be seen; it catches up once
 * when it starts again. */
- (void)updateTimers
{
    const BOOL want = _launched && [self windowCanBeSeen:_window];
    if (want && !_sysactTimer) {
        __weak AppDelegate *weakSelf = self;
        _sysactTimer = [NSTimer scheduledTimerWithTimeInterval:0.05
                                                       repeats:YES
                                                         block:^(NSTimer *timer) {
            (void)timer;
            [weakSelf sysactTick];
        }];
        _sysactTimer.tolerance = 0.025;
        [self sysactTick];
    } else if (!want && _sysactTimer) {
        [_sysactTimer invalidate];
        _sysactTimer = nil;
    }
}

- (void)visibilityChanged:(NSNotification *)note
{
    (void)note;
    [self updateTimers];
}

- (void)showToast:(NSString *)text
{
    if (!text) return;
    _toast.stringValue = text;
    _toast.hidden = NO;
    const NSUInteger serial = ++_toastSerial;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(TOAST_SECONDS * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        /* a newer message restarts the clock */
        if (serial == self->_toastSerial) self->_toast.hidden = YES;
    });
}

/* Losing key status with keys held would leave the machine believing they
 * are still down (a pad's holds and the Controllers panel's are not touched). */
- (void)windowDidResignKey:(NSNotification *)note
{
    if (note.object == _window)
        cocosession_release_all(_session);
}

/* ---- live machine / video / CPU radio menus --------------------------------
 * Built at runtime by walking the session's own cocosession_*_name(idx)
 * tables until NULL -- no fixed menu resource, so a new entry added to
 * cocosession.h needs no matching edit here. `supported` may be NULL; when
 * set, it decides which indices are offered (that is how RGB disappears on
 * a CoCo 2). Whichever changed, the whole menu bar is rebuilt afterwards --
 * simplest way to keep the Display submenu limited to exactly the TV inputs
 * the current machine has, matching GNOME's rebuild_menu. */

- (void)appendRadioItems:(NSMenu *)menu
                   action:(SEL)action
                   nameAt:(const char *(*)(int))nameAt
                supported:(int (*)(int, int))supported
                  context:(int)context
                  current:(int)current
{
    const char *label;
    for (int i = 0; (label = nameAt(i)) != NULL; i++) {
        NSMenuItem *item;
        if (supported && !supported(context, i))
            continue;
        item = [menu addItemWithTitle:[NSString stringWithUTF8String:label]
                                action:action
                         keyEquivalent:@""];
        item.target = self;
        item.tag = i;
        item.state = (i == current) ? NSControlStateValueOn
                                    : NSControlStateValueOff;
    }
}

- (void)selectMachine:(NSMenuItem *)sender
{
    cocosession_set_machine(_session, (int)sender.tag);
    [_controllers refresh];
    [self startMachine];
    [self rebuildMenus];
}

- (void)selectTvInput:(NSMenuItem *)sender
{
    cocosession_set_tv_input(_session, (int)sender.tag);
    [self rebuildMenus];
}

- (void)selectCcr:(NSMenuItem *)sender
{
    cocosession_set_ccr(_session, (int)sender.tag);
    [self rebuildMenus];
}

- (void)selectCpu:(NSMenuItem *)sender
{
    cocosession_set_cpu(_session, (int)sender.tag);
    [self rebuildMenus];
}

/* ---- menus ------------------------------------------------------------------ */

- (NSMenuItem *)item:(NSString *)title action:(SEL)sel key:(NSString *)key
{
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title
                                                  action:sel
                                           keyEquivalent:key];
    item.target = self;
    return item;
}

- (void)buildMenus
{
    NSString *appName = @"FujiNet Go CoCo";
    NSMenu *menubar = [[NSMenu alloc] init];
    int machine = cocosession_machine(_session);

    /* A menu bar built in code gets nothing for free: the standard items
     * every Mac app is expected to have (Services, Hide/Hide Others/Show
     * All, Minimize/Zoom, the window list) exist only if they are added
     * here -- and Services and the window list stay empty until AppKit is
     * told which menus they belong to (servicesMenu / windowsMenu). */
    NSMenuItem *appItem = [[NSMenuItem alloc] init];
    NSMenu *appMenu = [[NSMenu alloc] init];
    [appMenu addItemWithTitle:[@"About " stringByAppendingString:appName]
                       action:@selector(orderFrontStandardAboutPanel:)
                keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];

    [appMenu addItem:[self item:@"Settings…"
                         action:@selector(showSettings:)
                            key:@","]];
    [appMenu addItem:[NSMenuItem separatorItem]];

    NSMenu *servicesMenu = [[NSMenu alloc] initWithTitle:@"Services"];
    NSMenuItem *servicesItem = [appMenu addItemWithTitle:@"Services"
                                                  action:NULL
                                           keyEquivalent:@""];
    servicesItem.submenu = servicesMenu;
    NSApp.servicesMenu = servicesMenu;
    [appMenu addItem:[NSMenuItem separatorItem]];

    [appMenu addItemWithTitle:[@"Hide " stringByAppendingString:appName]
                       action:@selector(hide:)
                keyEquivalent:@"h"];
    NSMenuItem *hideOthers =
        [appMenu addItemWithTitle:@"Hide Others"
                           action:@selector(hideOtherApplications:)
                    keyEquivalent:@"h"];
    hideOthers.keyEquivalentModifierMask =
        NSEventModifierFlagOption | NSEventModifierFlagCommand;
    [appMenu addItemWithTitle:@"Show All"
                       action:@selector(unhideAllApplications:)
                keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];

    [appMenu addItemWithTitle:[@"Quit " stringByAppendingString:appName]
                       action:@selector(terminate:)
                keyEquivalent:@"q"];
    appItem.submenu = appMenu;
    [menubar addItem:appItem];

    /* Machine: the live radio group plus Reset. */
    NSMenuItem *machineItem = [[NSMenuItem alloc] init];
    NSMenu *machineMenu = [[NSMenu alloc] initWithTitle:@"Machine"];
    NSMenu *machineSub = [[NSMenu alloc] initWithTitle:@"Machine"];
    [self appendRadioItems:machineSub
                     action:@selector(selectMachine:)
                     nameAt:cocosession_machine_name
                  supported:NULL
                    context:0
                    current:machine];
    NSMenuItem *machineSubItem = [[NSMenuItem alloc] init];
    machineSubItem.title = @"Machine";
    machineSubItem.submenu = machineSub;
    [machineMenu addItem:machineSubItem];
    [machineMenu addItem:[self item:@"Reset"
                             action:@selector(resetMachine:)
                                key:@""]];
    [machineMenu addItem:[NSMenuItem separatorItem]];
    [machineMenu addItem:[self item:@"Import BIOS…"
                             action:@selector(importBios:)
                                key:@""]];
    [machineMenu addItem:[self item:@"Import BIOS from a Folder…"
                             action:@selector(importBiosFromFolder:)
                                key:@""]];
    machineItem.submenu = machineMenu;
    [menubar addItem:machineItem];

    NSMenuItem *mediaItem = [[NSMenuItem alloc] init];
    NSMenu *media = [[NSMenu alloc] initWithTitle:@"Media"];
    [media addItem:[self item:@"Import Disk or Tape Image…"
                       action:@selector(importMedia:)
                          key:@"i"]];
    mediaItem.submenu = media;
    [menubar addItem:mediaItem];

    NSMenuItem *fujiItem = [[NSMenuItem alloc] init];
    NSMenu *fuji = [[NSMenu alloc] initWithTitle:@"FujiNet"];
    [fuji addItem:[self item:@"Configuration…"
                      action:@selector(showFujiNetConfig:)
                         key:@""]];
    [fuji addItem:[self item:@"Console Log…"
                      action:@selector(showFujiNetLog:)
                         key:@""]];
    fujiItem.submenu = fuji;
    [menubar addItem:fujiItem];

    /* View: Display (TV input, filtered to what this machine has), Artifact
     * Colour, CPU -- built for the machine running now, exactly like the
     * GNOME/Windows frontends. */
    NSMenuItem *viewItem = [[NSMenuItem alloc] init];
    NSMenu *view = [[NSMenu alloc] initWithTitle:@"View"];

    NSMenu *displaySub = [[NSMenu alloc] initWithTitle:@"Display"];
    [self appendRadioItems:displaySub
                     action:@selector(selectTvInput:)
                     nameAt:cocosession_tv_input_name
                  supported:cocosession_tv_input_supported
                    context:machine
                    current:cocosession_tv_input(_session)];
    NSMenuItem *displaySubItem = [[NSMenuItem alloc] init];
    displaySubItem.title = @"Display";
    displaySubItem.submenu = displaySub;
    [view addItem:displaySubItem];

    NSMenu *ccrSub = [[NSMenu alloc] initWithTitle:@"Artifact Colour"];
    [self appendRadioItems:ccrSub
                     action:@selector(selectCcr:)
                     nameAt:cocosession_ccr_name
                  supported:NULL
                    context:0
                    current:cocosession_ccr(_session)];
    NSMenuItem *ccrSubItem = [[NSMenuItem alloc] init];
    ccrSubItem.title = @"Artifact Colour";
    ccrSubItem.submenu = ccrSub;
    [view addItem:ccrSubItem];

    NSMenu *cpuSub = [[NSMenu alloc] initWithTitle:@"CPU"];
    [self appendRadioItems:cpuSub
                     action:@selector(selectCpu:)
                     nameAt:cocosession_cpu_name
                  supported:NULL
                    context:0
                    current:cocosession_cpu(_session)];
    NSMenuItem *cpuSubItem = [[NSMenuItem alloc] init];
    cpuSubItem.title = @"CPU";
    cpuSubItem.submenu = cpuSub;
    [view addItem:cpuSubItem];

    [view addItem:[NSMenuItem separatorItem]];
    NSMenuItem *fs = [view addItemWithTitle:@"Toggle Full Screen"
                                     action:@selector(toggleFullScreen:)
                              keyEquivalent:@"f"];
    fs.keyEquivalentModifierMask =
        NSEventModifierFlagControl | NSEventModifierFlagCommand;
    NSMenuItem *dbg = [self item:@"Debugger"
                          action:@selector(showDebugger:)
                             key:[NSString stringWithFormat:@"%C",
                                           (unichar)NSF12FunctionKey]];
    dbg.keyEquivalentModifierMask = 0;
    [view addItem:dbg];
    /* Cmd+Shift+K: every plain key, F9 and Ctrl+Shift+letter among them, is
     * the CoCo's */
    NSMenuItem *ctl = [self item:@"Controllers"
                          action:@selector(toggleControllers:)
                             key:@"k"];
    ctl.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [view addItem:ctl];
    viewItem.submenu = view;
    [menubar addItem:viewItem];

    /* Window menu. AppKit appends the list of open windows (the main
     * window, the debugger, the FujiNet console log) to whichever menu is
     * handed to windowsMenu, and manages their check marks -- that list is
     * what "show or hide windows" means to a Mac user. */
    NSMenuItem *windowItem = [[NSMenuItem alloc] init];
    NSMenu *windowMenu = [[NSMenu alloc] initWithTitle:@"Window"];
    /* Close lives here rather than in a File menu: there is no File menu,
     * and the debugger / config / log windows need a keyboard close. */
    [windowMenu addItemWithTitle:@"Close"
                          action:@selector(performClose:)
                   keyEquivalent:@"w"];
    [windowMenu addItemWithTitle:@"Minimize"
                          action:@selector(performMiniaturize:)
                   keyEquivalent:@"m"];
    [windowMenu addItemWithTitle:@"Zoom"
                          action:@selector(performZoom:)
                   keyEquivalent:@""];
    [windowMenu addItem:[NSMenuItem separatorItem]];
    [windowMenu addItemWithTitle:@"Bring All to Front"
                          action:@selector(arrangeInFront:)
                   keyEquivalent:@""];
    windowItem.submenu = windowMenu;
    [menubar addItem:windowItem];
    NSApp.windowsMenu = windowMenu;

    NSMenuItem *helpItem = [[NSMenuItem alloc] init];
    NSMenu *help = [[NSMenu alloc] initWithTitle:@"Help"];
    /* openHelp:, not showHelp: (AppKit's: "Help isn't available") */
    [help addItem:[self item:@"FujiNet Go CoCo Help" action:@selector(openHelp:) key:@"?"]];
    helpItem.submenu = help;
    [menubar addItem:helpItem];

    NSApp.mainMenu = menubar;
    NSApp.helpMenu = help;
    /* the Help menu's search field finds the Help's sections too */
    [NSApp registerUserInterfaceItemSearchHandler:[CocoHelpWindow shared]];
}

/* For the window in front: Settings, the debugger's tab ... */
- (void)openHelp:(id)sender
{
    (void)sender;
    [[CocoHelpWindow shared] showFor:[NSApp keyWindow]];
}

- (void)rebuildMenus
{
    [self buildMenus];
}

/* ---- lifecycle --------------------------------------------------------------- */

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
    (void)note;
    [self buildMenus];

    NSRect frame = NSMakeRect(0, 0, 1120, 840);
    _window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable |
                            NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"FujiNet Go CoCo";
    _window.delegate = self;

    _display = [[DisplayView alloc] initWithSession:_session];
    _display.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _display.frame = ((NSView *)_window.contentView).bounds;
    [_window.contentView addSubview:_display];
    _noRoms = [self buildNoRoms:((NSView *)_window.contentView).bounds];
    [_window.contentView addSubview:_noRoms];

    /* The toast: a label over the top of the picture. */
    NSView *content = _window.contentView;
    _toast = [NSTextField labelWithString:@""];
    _toast.font = [NSFont boldSystemFontOfSize:13];
    _toast.textColor = NSColor.whiteColor;
    _toast.drawsBackground = YES;
    _toast.backgroundColor = [NSColor colorWithWhite:0.0 alpha:0.7];
    _toast.alignment = NSTextAlignmentCenter;
    _toast.hidden = YES;
    _toast.frame = NSMakeRect(0, content.bounds.size.height - 34, content.bounds.size.width, 26);
    _toast.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    [content addSubview:_toast];
    _toastGeneration = cocosession_gamepad_generation(_session);
    [self applyDisplaySettings];

    _controllers = [[CocoControllersWindow alloc] initWithSession:_session];

    [_window center];
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:_display];
    [_display start];
    [self showPage];

    /* Developer affordance shared with the other frontends. */
    if (getenv("COCO_OPEN_CONTROLLERS"))
        [_controllers toggle];
    if (getenv("COCO_OPEN_DEBUGGER"))
        [DebuggerWindow showForSession:_session];

    /* the sysaction timer follows the window's visibility */
    _launched = YES;
    NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
    for (NSString *name in @[ NSWindowDidChangeOcclusionStateNotification,
                              NSWindowDidMiniaturizeNotification,
                              NSWindowDidDeminiaturizeNotification,
                              NSApplicationDidHideNotification,
                              NSApplicationDidUnhideNotification ])
        [center addObserver:self
                   selector:@selector(visibilityChanged:)
                       name:name
                     object:nil];
    [self updateTimers];
}

- (void)applicationWillTerminate:(NSNotification *)note
{
    (void)note;
    [_logTimer invalidate];
    [_sysactTimer invalidate];
    [_padTimer invalidate];
    [_display stop];
    cocosession_stop(_session);
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:
    (NSApplication *)sender
{
    (void)sender;
    return YES;
}

@end
