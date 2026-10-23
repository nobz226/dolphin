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

// Field-drawn video fix: whether the last full-frame XFB copies the presenter checked hold
// different pictures in their two fields (the movies). Set on the GPU thread from a delayed
// readback, read by the video interface on the CPU thread.
void SetXFBFieldsDiffer(bool differ);
bool XFBFieldsDiffer();
