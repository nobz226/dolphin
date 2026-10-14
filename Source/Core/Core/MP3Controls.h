// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Controller aiming for Metroid Prime 3: Corruption (Wii, RM3E01 rev 0).
//
// Ported from PrimeHack's FpsControls (github.com/shiiion/dolphin, GPL-2.0-or-later):
// the right stick turns Samus directly instead of moving the Wii Remote cursor, the
// cursor stays centred and the arm cannon no longer lags behind it. Lock-on, menus,
// the visor wheel and the Ridley fight hand control back to the game.

#pragma once

#include <array>

#include "Common/CommonTypes.h"

namespace Core
{
class System;
}

namespace MP3Controls
{
struct Settings
{
  bool enabled = false;
  float turn_speed = 15.f;  // PrimeHack's "Camera Sensitivity"
  bool invert_y = false;
  bool smooth = true;       // response curve for fine aiming
};

// Read once per frame by the frontend, all axes in -1..1 (y down)
struct Input
{
  float look_x = 0.f;
  float look_y = 0.f;
  float move_y = 0.f;
  std::array<bool, 4> visor{};  // combat, scan, command, x-ray
  bool recentre = false;
  bool lasso = false;
};

void SetSettings(const Settings& settings);
void SetInput(const Input& input);

// CPU thread, once per frame (PatchEngine::ApplyFramePatches)
void RunFrame(Core::System& system);

// Savestates never keep the code changes: they are lifted while a state is
// written and put back after; a loaded state is picked up on the next frame.
void BeforeStateSave(Core::System& system);
void AfterStateSave(Core::System& system);
void AfterStateLoad();

void Shutdown();
}  // namespace MP3Controls
