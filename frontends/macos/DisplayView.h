/*
 * DisplayView: the emulator video view for the macOS frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import <Cocoa/Cocoa.h>

#include "cocosession.h"

@interface DisplayView : NSView

- (instancetype)initWithSession:(cocosession *)session;
- (void)start;
- (void)stop;
- (void)setAspectMode:(int)mode;
- (void)setSmooth:(BOOL)smooth;

@end
