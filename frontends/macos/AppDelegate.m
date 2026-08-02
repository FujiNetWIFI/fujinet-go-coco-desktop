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
#import "debugger/DebuggerWindow.h"

#include <stdlib.h>

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

@interface AppDelegate () <NSWindowDelegate, WKUIDelegate>
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
      keys = @[ @"enable_fujinet", @"enable_audio", @"enable_gamepad" ];
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

    cocosession_set_int(_session, key.UTF8String, value);

    if ([sessionSettingKeys() containsObject:key])
        _sessionDirty = YES;
    else
        [self applyDisplaySettings];
}

- (void)showSettings:(id)sender
{
    (void)sender;
    if (_settingsWindow) {
        [_settingsWindow makeKeyAndOrderFront:nil];
        return;
    }

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
            [NSTextField labelWithString:@"Sound"],
            [self checkBoxForKey:"enable_audio" fallback:1]
        ],
        @[
            [NSTextField labelWithString:@"Gamepads"],
            [self checkBoxForKey:"enable_gamepad" fallback:1]
        ],
    ];

    NSGridView *grid = [NSGridView gridViewWithViews:rows];
    grid.rowSpacing = 8;
    grid.columnSpacing = 12;
    [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;

    NSTextField *note = [NSTextField
        labelWithString:@"Display options apply immediately. FujiNet options "
                        @"take effect when this window is closed."];
    note.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    note.textColor = NSColor.secondaryLabelColor;

    NSStackView *root = [NSStackView stackViewWithViews:@[ grid, note ]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.spacing = 12;
    root.edgeInsets = NSEdgeInsetsMake(16, 16, 16, 16);

    _settingsWindow = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 420, 300)
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _settingsWindow.title = @"Settings";
    _settingsWindow.releasedWhenClosed = NO;
    _settingsWindow.delegate = self;
    _settingsWindow.contentView = root;
    [_settingsWindow center];
    [_settingsWindow makeKeyAndOrderFront:nil];
}

/* FujiNet options are collected while the window is open and applied in one
 * restart when it closes. */
- (void)windowWillClose:(NSNotification *)note
{
    if (note.object != _settingsWindow || !_sessionDirty)
        return;
    _sessionDirty = NO;
    [self restartSession];
}

/* ---- actions ---------------------------------------------------------------- */

- (void)resetMachine:(id)sender
{
    (void)sender;
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

- (void)importRoms:(id)sender
{
    (void)sender;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.title = @"Import System ROMs";
    /* XRoar resolves ROMs by exact basename (bas13.rom, extbas11.rom,
     * coco3.rom, ...), so whatever is picked keeps its name. */
    panel.allowedContentTypes = typesForExtensions(@[ @"rom", @"bin" ]);
    panel.allowsMultipleSelection = YES;
    if ([panel runModal] != NSModalResponseOK || panel.URLs.count == 0)
        return;

    NSUInteger ok = 0;
    for (NSURL *url in panel.URLs) {
        char dest[1024];
        if (cocosession_import_rom(_session, url.path.UTF8String, dest,
                                   sizeof(dest)) == 0)
            ok++;
    }
    if (ok == 0) {
        NSLog(@"import roms: %s", cocosession_last_error(_session));
        return;
    }
    /* XRoar resolves ROMs when the machine is built, so this needs a
     * restart -- an in-place machine switch would reuse the loaded
     * images. */
    [self restartSession];
}

- (void)showFujiNetConfig:(id)sender
{
    (void)sender;
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

    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:frame];
    scroll.hasVerticalScroller = YES;
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _logView = [[NSTextView alloc] initWithFrame:frame];
    _logView.editable = NO;
    _logView.font = [NSFont monospacedSystemFontOfSize:11
                                                weight:NSFontWeightRegular];
    scroll.documentView = _logView;
    _logWindow.contentView = scroll;

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
}

- (void)refreshLog
{
    static char buf[128 * 1024];
    int n = cocosession_fujinet_copy_log(_session, buf, sizeof(buf));
    NSString *text = n > 0 ? [NSString stringWithUTF8String:buf]
                           : @"(no FujiNet output yet)";
    if (text)
        _logView.string = text;
    [_logView scrollToEndOfDocument:nil];
}

- (void)showDebugger:(id)sender
{
    (void)sender;
    [DebuggerWindow showForSession:_session];
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
    machineItem.submenu = machineMenu;
    [menubar addItem:machineItem];

    NSMenuItem *mediaItem = [[NSMenuItem alloc] init];
    NSMenu *media = [[NSMenu alloc] initWithTitle:@"Media"];
    [media addItem:[self item:@"Import Disk or Tape Image…"
                       action:@selector(importMedia:)
                          key:@"i"]];
    [media addItem:[self item:@"Import System ROMs…"
                       action:@selector(importRoms:)
                          key:@""]];
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

    NSApp.mainMenu = menubar;
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

    _display = [[DisplayView alloc] initWithSession:_session];
    _display.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _display.frame = ((NSView *)_window.contentView).bounds;
    [_window.contentView addSubview:_display];
    [self applyDisplaySettings];

    [_window center];
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:_display];
    [_display start];

    /* Developer affordance shared with the other frontends. */
    if (getenv("COCO_OPEN_DEBUGGER"))
        [DebuggerWindow showForSession:_session];
}

- (void)applicationWillTerminate:(NSNotification *)note
{
    (void)note;
    [_logTimer invalidate];
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
