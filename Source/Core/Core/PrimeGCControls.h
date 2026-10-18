// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Controller aiming for Metroid Prime (GameCube, GM8E01 revisions 0-00, 0-01, 0-02) and
// Metroid Prime 2: Echoes (GameCube, G2ME01).
//
// Ported from PrimeHack's FpsControls (github.com/shiiion/dolphin, GPL-2.0-or-later): the
// right stick turns Samus directly, the left stick moves and strafes, and the arm cannon
// stops bobbing with the turn. Lock-on, grapple, morph ball and menus stay the game's.

#pragma once

#include "Common/CommonTypes.h"

namespace Core
{
class System;
}

namespace PrimeGCControls
{
struct Settings
{
  bool enabled = false;
  float turn_speed = 15.f;  // PrimeHack's "Camera Sensitivity"
  bool invert_y = false;
  bool smooth = true;       // response curve for fine aiming
  bool crosshair = true;    // the game's free-aim reticle, always shown
};

// Read once per frame by the frontend, axes in -1..1 (y down)
struct Input
{
  float look_x = 0.f;
  float look_y = 0.f;
  bool recentre = false;
};

void SetSettings(const Settings& settings);
void SetInput(const Input& input);

// CPU thread, once per frame (PatchEngine::ApplyFramePatches)
void RunFrame(Core::System& system);

// Savestates never keep the code changes, like MP3Controls
void BeforeStateSave(Core::System& system);
void AfterStateSave(Core::System& system);
void AfterStateLoad();

void Shutdown();
}  // namespace PrimeGCControls
