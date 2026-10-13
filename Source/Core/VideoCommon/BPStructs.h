// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

void BPInit();
void BPReload();

#include "Common/CommonTypes.h"

// Field-drawn video fix (GFX_HACK_FIELD_DRAWN_VIDEO): whether the XFB at addr was
// last written by one EFB copy covering at least `lines` lines - a game that draws
// every field as its own full frame, as Metroid Prime's attract movies do.
bool XFBWasSingleFullCopy(u32 addr, u32 lines);
