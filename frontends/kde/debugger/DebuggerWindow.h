/*
 * The 6809/6309 debugger window for the KDE/Qt frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QWidget>

#include "cocosession.h"

namespace DebuggerWindow {
void show(QWidget *parent, cocosession *session);
}
