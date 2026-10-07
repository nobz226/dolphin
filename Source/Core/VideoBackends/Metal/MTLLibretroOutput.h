// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "Common/CommonTypes.h"

// Plain C++, so the libretro frontend can use it without Objective-C.
namespace Metal
{
// libretro has no Metal context to hand a texture to, so when the backend is started for libretro
// (no layer), the backbuffer is an offscreen texture and every presented frame is read back and
// passed to the callback: BGRA8, which is the byte order of RETRO_PIXEL_FORMAT_XRGB8888. Set it
// before the backend initializes, and again when the output size changes.
using LibretroFrameCallback = void (*)(const void* data, u32 width, u32 height, size_t pitch);
void SetLibretroOutput(LibretroFrameCallback callback, u32 width, u32 height);
}  // namespace Metal
