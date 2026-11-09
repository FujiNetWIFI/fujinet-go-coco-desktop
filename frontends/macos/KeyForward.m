/*
 * KeyForward: the key translation DisplayView.m used to hold, moved here so
 * the Controllers panel forwards typed keys the same way.
 * NSEvent.charactersIgnoringModifiers already gives a layout-aware
 * unshifted character, and the function-key table is X11 keysym values
 * (0xFF52 = XK_Up); the session does the CoCo-key translation.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "KeyForward.h"

static uint32_t keysym_for_char(unichar chr)
{
    switch (chr) {
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
    case 0x1B:                    return COCO_KEYSYM_ESCAPE;
    default:
        if (chr >= NSF1FunctionKey && chr <= NSF1FunctionKey + 5)
            return 0xFFBE + (uint32_t)(chr - NSF1FunctionKey);
        return 0;
    }
}

BOOL CocoKeyFromEvent(NSEvent *event, uint32_t *keysym, uint32_t *unicode)
{
    NSString *chars = event.charactersIgnoringModifiers;
    const unichar chr = chars.length ? [chars characterAtIndex:0] : 0;

    *keysym = keysym_for_char(chr);
    *unicode = (chr >= 0x20 && chr < 0xF700) ? chr : 0;
    return *keysym != 0 || *unicode != 0;
}

int CocoForwardKey(cocosession *session, NSEvent *event, int down)
{
    uint32_t keysym = 0, unicode = 0;

    if (!CocoKeyFromEvent(event, &keysym, &unicode))
        return 0; /* not a key the emulated keyboard has */
    const int ctrl = (event.modifierFlags & NSEventModifierFlagControl) != 0;
    const int shift = (event.modifierFlags & NSEventModifierFlagShift) != 0;
    return cocosession_key(session, down, keysym, unicode, ctrl, shift);
}
