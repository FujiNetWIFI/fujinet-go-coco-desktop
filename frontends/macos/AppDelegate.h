/*
 * AppDelegate: application lifecycle for the macOS frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import <Cocoa/Cocoa.h>

#include "cocosession.h"

@interface AppDelegate : NSObject <NSApplicationDelegate>

- (instancetype)initWithSession:(cocosession *)session;

@end
