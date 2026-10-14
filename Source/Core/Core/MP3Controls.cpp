// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/MP3Controls.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <initializer_list>
#include <mutex>
#include <vector>

#include "Common/Logging/Log.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace MP3Controls
{
namespace
{
// Addresses for Metroid Prime 3: Corruption, NTSC-U standalone (PrimeHack's
// AddressDBInit.cpp, Game::PRIME_3_STANDALONE, first column)
constexpr u32 kGameId = 0x524D3345;  // "RM3E"
constexpr u32 kCursorBase = 0x8067dc18;
constexpr u32 kCursorDialog = 0x805c70c7;
constexpr u32 kLockonState = 0x805c50e7;
constexpr u32 kStateManager = 0x805c4f98;
constexpr u32 kWorldIdBase = 0x8067dc0c;  // also the beam/visor menu base
constexpr u32 kXfOffset = 0x3c;
constexpr u32 kPowerupsSize = 12;
constexpr u32 kPowerupsOffset = 0x58;

// Free guest memory PrimeHack's allocator uses for this game
constexpr u32 kVarLassoState = 0x80684800;
constexpr u32 kVarHandX = 0x80684804;
constexpr u32 kVarHandY = 0x80684808;
constexpr u32 kVarTriggerGrapple = 0x8068480c;

constexpr u64 kNorionWorldId = 0x6fb8ef2a9523c343;
constexpr u32 kRidleyFightArea = 0x16;

constexpr float kPi = 3.141592654f;
constexpr float kTurnrateRatio = 0.00498665500569808449206349206349f;
// PrimeHack's controller defaults (Horizontal/Vertical Sensitivity)
constexpr float kStickScaleX = 45.f;
constexpr float kStickScaleY = 35.f;
constexpr float kStickDeadzone = 0.15f;
// Cursor travel per frame at full tilt, in the game's normalised units
constexpr float kCursorSpeed = 0.035f;
constexpr float kAspect = 16.f / 9.f;
constexpr float kHalfH = 1.025f;          // NTSC horizontal range
constexpr float kMenuHalfV = 1.f;
constexpr float kReticleHalfV = 0.715f;   // 16:9

constexpr u32 kNop = 0x60000000;

enum Group
{
  ALWAYS,
  GUN,           // "disable_gun_move": arm cannon follows the camera, not the cursor
  LASSO,         // "grapple_lasso": the lasso is driven from our variables
  LASSO_ANIM,    // "grapple_lasso_animation"
  GROUP_COUNT
};

struct Patch
{
  u32 address;
  u32 value;
  Group group;
  u32 original = 0;
  bool original_known = false;
};

constexpr u32 Lis(u32 reg, u32 address)
{
  return (15u << 26) | (reg << 21) | (address >> 16);
}
constexpr u32 Ori(u32 reg, u32 address)
{
  return (24u << 26) | (reg << 21) | (reg << 16) | (address & 0xffff);
}

// PrimeHack's init_mod_mp3_standalone, NTSC-U. Its Wii Remote shake hook (a
// vmcall that turns the jump button into a shake) is left out: shake keeps its
// own button.
std::vector<Patch> BuildPatches()
{
  std::vector<Patch> p;
  const auto add = [&p](u32 address, u32 value, Group group = ALWAYS) {
    p.push_back({address, value, group});
  };

  add(0x80080be8, 0xec010072);
  add(0x801521f0, kNop);
  add(0x801521c8, kNop);
  add(0x80139108, kNop);
  add(0x80138750, kNop);
  add(0x8000ae44, 0x4bffaa3d);  // bl 0x80005880 (the control state hook below)
  add(0x80080e6c, kNop);
  add(0x8007fef0, 0x480000e4);
  add(0x80183288, kNop);

  // Stops the arm cannon stuttering by having the XF updater think it's orbiting
  add(0x80186f98, 0x48000498, GUN);
  add(0x801886f0, kNop, GUN);

  // Grapple lasso (add_grapple_lasso_code_mp3)
  constexpr u32 func1 = 0x800df790, func2 = 0x80174d70, func3 = 0x80175b54;
  add(func1 + 0x0, Lis(11, kVarHandX), LASSO);
  add(func1 + 0x4, Ori(11, kVarHandX), LASSO);
  add(func1 + 0xc, 0xc04b0000, LASSO);  // lfs f2, 0(r11)
  add(func1 + 0x8, 0x40800158, LASSO);
  add(func1 + 0x18, 0x40810148, LASSO);
  add(func1 + 0x54, 0x4800010c, LASSO);
  add(func2 + 0x0, Lis(11, kVarHandX), LASSO_ANIM);
  add(func2 + 0x4, Ori(11, kVarHandX), LASSO_ANIM);
  add(func2 + 0x8, 0xc00b0000, LASSO_ANIM);  // lfs f0, 0(r11)
  add(func2 + 0xc, Lis(11, kVarHandY), LASSO_ANIM);
  add(func2 + 0x10, Ori(11, kVarHandY), LASSO_ANIM);
  add(func2 + 0x14, 0xc04b0000, LASSO_ANIM);  // lfs f2, 0(r11)
  add(func2 + 0x18, 0xd05701c8, LASSO_ANIM);  // stfs f2, 0x1C8(r23)
  add(func1 + 0x160, Lis(30, kVarLassoState), LASSO);
  add(func1 + 0x164, Ori(30, kVarLassoState), LASSO);
  add(func1 + 0x168, 0x83de0000, LASSO);  // lwz r30, 0(r30)
  add(func3 + 0x0, Lis(3, kVarTriggerGrapple), LASSO);
  add(func3 + 0x4, Ori(3, kVarTriggerGrapple), LASSO);
  add(func3 + 0x8, 0x80630000, LASSO);  // lwz r3, 0(r3)
  add(func3 + 0xc, 0x4e800020, LASSO);  // blr

  // Control state hook (add_control_state_hook_mp3, standalone NTSC-U)
  constexpr u32 hook = 0x80005880;
  add(hook + 0x00, 0x3c60805c);  // lis  r3, 0x805c
  add(hook + 0x04, 0x38634f6c);  // addi r3, r3, 0x4f6c
  add(hook + 0x08, 0x8063002c);  // lwz  r3, 0x2c(r3)
  add(hook + 0x0c, kNop);
  add(hook + 0x10, 0x80632184);  // lwz  r3, 0x2184(r3)
  add(hook + 0x14, 0x7c03f800);  // cmpw r3, r31
  add(hook + 0x18, 0x4d820020);  // beqlr
  add(hook + 0x1c, 0x7fe3fb78);  // mr   r3, r31
  add(hook + 0x20, 0x90c30078);  // stw  r6, 0x78(r3)
  add(hook + 0x24, 0x4e800020);  // blr

  // Grapple slide (add_grapple_slide_code_mp3)
  constexpr u32 slide = 0x80182c9c;
  add(slide + 0x00, kNop);
  add(slide + 0x04, kNop);
  add(slide + 0x08, kNop);
  add(slide + 0x0c, 0xc0010240);  // lfs  f0, 0x240(sp)
  add(slide + 0x10, 0xd01f0048);  // stfs f0, 0x48(r31)
  add(slide + 0x14, 0xc0010244);  // lfs  f0, 0x244(sp)
  add(slide + 0x18, 0xd01f0058);  // stfs f0, 0x58(r31)
  add(slide + 0x1c, 0xc0010248);  // lfs  f0, 0x248(sp)
  add(slide + 0x20, 0xd01f0068);  // stfs f0, 0x68(r31)
  add(slide + 0x28, 0x389f003c);  // addi r4, r31, 0x3c

  // Steps over bounds checking on the reticle
  add(0x80017290, 0x48000120);
  return p;
}

struct Transform
{
  float m[3][4]{};

  float FwdX() const { return m[0][1]; }
  float FwdY() const { return m[1][1]; }
  float FwdZ() const { return m[2][1]; }

  void BuildRotation(float yaw)
  {
    yaw -= kPi / 2.f;
    const float sy = std::sin(yaw), cy = std::cos(yaw);
    m[0][0] = cy;
    m[1][0] = sy;
    m[2][0] = 0;
    m[0][1] = -sy;
    m[1][1] = cy;
    m[2][1] = 0;
    m[0][2] = 0;
    m[1][2] = 0;
    m[2][2] = 1.f;
  }
};

struct ModState
{
  bool game_seen = false;
  bool active = false;
  std::vector<Patch> patches;
  std::array<bool, GROUP_COUNT> group_on{};
  bool all_original = false;  // Ridley fight, or the option is off
  bool stashed = false;
  bool reload_pitch = true;

  float pitch = 0.f;
  float yaw = 0.f;
  bool interpolating = false;
  int delta = 0;
  float start_pitch = 0.f;

  float cursor_x = 0.f;
  float cursor_y = 0.f;
  std::array<bool, 4> visor_held{};
  bool was_in_ridley_fight = false;

  float grapple_hand_pos = 0.f;
  float grapple_force = 0.f;
  bool grapple_button_state = false;
};

std::mutex s_mutex;
Settings s_settings;
Input s_input;
ModState s;

class Mem
{
public:
  explicit Mem(Core::System& system) : m_memory(system.GetMemory()) {}

  static bool Valid(u32 address, u32 size)
  {
    return (address >= 0x80000000 && address + size <= 0x81800000) ||
           (address >= 0x90000000 && address + size <= 0x94000000);
  }

  u8 R8(u32 a) const { return Valid(a, 1) ? m_memory.Read_U8(a & 0x3fffffff) : 0; }
  u16 R16(u32 a) const { return Valid(a, 2) ? m_memory.Read_U16(a & 0x3fffffff) : 0; }
  u32 R32(u32 a) const { return Valid(a, 4) ? m_memory.Read_U32(a & 0x3fffffff) : 0; }
  u64 R64(u32 a) const { return Valid(a, 8) ? m_memory.Read_U64(a & 0x3fffffff) : 0; }
  float RF(u32 a) const { return std::bit_cast<float>(R32(a)); }
  void W32(u32 v, u32 a) const
  {
    if (Valid(a, 4))
      m_memory.Write_U32(v, a & 0x3fffffff);
  }
  void WF(float v, u32 a) const { W32(std::bit_cast<u32>(v), a); }

  // [...[[base + o0] + o1] ... ] + oN, like PrimeHack's lookup_dynamic_address;
  // 0 when a pointer along the way isn't valid
  u32 Chain(u32 base, std::initializer_list<u32> offsets) const
  {
    u32 addr = base;
    auto it = offsets.begin();
    for (; it + 1 != offsets.end(); ++it)
    {
      addr = R32(addr + *it);
      if (!Valid(addr, 4))
        return 0;
    }
    return addr + *it;
  }

  void ReadTransform(Transform& t, u32 a) const
  {
    for (u32 i = 0; i < 12; i++)
      t.m[i / 4][i % 4] = RF(a + i * 4);
  }
  void WriteTransform(const Transform& t, u32 a) const
  {
    for (u32 i = 0; i < 12; i++)
      WF(t.m[i / 4][i % 4], a + i * 4);
  }

private:
  Memory::MemoryManager& m_memory;
};

// Radial deadzone, then an optional curve that keeps full speed at full tilt
// but makes small movements finer.
void ShapeStick(float& x, float& y, bool smooth)
{
  const float mag = std::sqrt(x * x + y * y);
  if (mag <= kStickDeadzone)
  {
    x = y = 0.f;
    return;
  }
  float scaled = std::min((mag - kStickDeadzone) / (1.f - kStickDeadzone), 1.f);
  if (smooth)
    scaled = std::pow(scaled, 1.6f);
  x = x / mag * scaled;
  y = y / mag * scaled;
}

void ApplyPatches(Core::System& system, const Mem& mem)
{
  auto& ppc = system.GetPowerPC();
  for (const Patch& p : s.patches)
  {
    const bool want = !s.all_original && s.group_on[p.group];
    if (!want && !p.original_known)
      continue;
    const u32 value = want ? p.value : p.original;
    if (mem.R32(p.address) != value)
    {
      mem.W32(value, p.address);
      ppc.ScheduleInvalidateCacheThreadSafe(p.address);
    }
  }
}

void MoveCursor(const Mem& mem, u32 cursor, const Input& in, bool smooth, float half_w,
                float half_h)
{
  float x = in.look_x, y = in.look_y;
  ShapeStick(x, y, smooth);
  s.cursor_x = std::clamp(s.cursor_x + x * kCursorSpeed, -half_w, half_w);
  s.cursor_y = std::clamp(s.cursor_y + y * kCursorSpeed * kAspect, -half_h, half_h);
  mem.WF(s.cursor_x, cursor + 0x9c);
  mem.WF(s.cursor_y, cursor + 0x15c);
}

// mp3_handle_cursor: locked = gameplay (cursor pinned, gun follows the camera),
// otherwise the stick moves the menu cursor or the reticle.
void HandleCursor(const Mem& mem, u32 cursor, const Input& in, const Settings& st, bool locked,
                  bool for_reticle)
{
  if (cursor == 0)
    return;
  if (locked)
  {
    s.group_on[GUN] = true;
    mem.W32(0, cursor + 0x9c);
    mem.W32(0, cursor + 0x15c);
    return;
  }
  s.group_on[GUN] = false;
  MoveCursor(mem, cursor, in, st.smooth, kHalfH, for_reticle ? kReticleHalfV : kMenuHalfV);
}

void PitchToTarget(float target)
{
  constexpr float margin = 0.05f;
  if (s.pitch >= target - margin && s.pitch <= target + margin)
  {
    s.pitch = target;
    s.interpolating = false;
    return;
  }
  if (!s.interpolating)
  {
    s.delta = 0;
    s.start_pitch = s.pitch;
    s.interpolating = true;
  }
  s.pitch = std::clamp(s.start_pitch + (target - s.start_pitch) * (s.delta / 15.f), -1.52f, 1.52f);
  s.delta++;
}

void PitchYawDelta(const Input& in, const Settings& st)
{
  if (in.recentre)
  {
    PitchToTarget(0.f);
    return;
  }
  s.interpolating = false;

  float x = in.look_x, y = in.look_y;
  ShapeStick(x, y, st.smooth);
  const float sens = st.turn_speed * kTurnrateRatio / 60.f;
  s.pitch += y * kStickScaleY * sens * (st.invert_y ? 1.f : -1.f);
  s.pitch = std::clamp(s.pitch, -1.52f, 1.52f);
  s.yaw += x * kStickScaleX * sens * -1.f;
  s.yaw = (s.yaw > kPi) ? s.yaw - 2 * kPi : ((s.yaw < -kPi) ? s.yaw + 2 * kPi : s.yaw);
}

void PitchYawLocked(const Mem& mem, u32 state_manager)
{
  const u32 object_list = mem.Chain(state_manager, {0, 0x1010, 0});
  const u32 camera_manager = mem.Chain(state_manager, {0x10, 0xc, 0x16});
  if (object_list == 0 || camera_manager == 0)
    return;
  const u16 camera_uid = mem.R16(camera_manager);
  if (camera_uid == 0xffff)
    return;
  const u32 camera = mem.R32(object_list + ((camera_uid & 0x3ff) << 3) + 4);
  if (!Mem::Valid(camera, 4))
    return;
  Transform xf;
  mem.ReadTransform(xf, camera + kXfOffset);
  s.yaw = std::atan2(xf.FwdY(), xf.FwdX());
  s.pitch = std::clamp(std::asin(std::clamp(xf.FwdZ(), -1.f, 1.f)), -1.52f, 1.52f);
}

void HandleLasso(const Mem& mem, u32 grapple_state_addr, const Input& in)
{
  const bool holding = in.lasso;
  if (mem.R32(grapple_state_addr) == 2)
  {
    if (holding && !s.grapple_button_state)
      s.grapple_button_state = true;
    else if (!holding)
    {
      s.grapple_button_state = false;
      s.grapple_force += 1.f;
    }

    constexpr float force_delta = 0.045f;
    if (s.grapple_force > 0)
    {
      s.grapple_hand_pos += force_delta;
      s.grapple_force -= force_delta;
    }
    else
    {
      s.grapple_hand_pos -= force_delta;
      s.grapple_force = 0;
    }
    s.grapple_hand_pos = std::clamp(s.grapple_hand_pos, 0.f, 1.f);

    mem.WF(s.grapple_hand_pos, kVarHandX);
    mem.WF(s.grapple_hand_pos / 4, kVarHandY);
    // 4 completes the grapple (pulls the door from its frame), 2 holds it
    mem.W32(s.grapple_hand_pos >= 1.f ? 4 : 2, kVarLassoState);
  }
  else
  {
    mem.WF(0.f, kVarHandX);
    mem.WF(0.f, kVarHandY);
    mem.W32(0, kVarLassoState);
    s.grapple_hand_pos = 0;
    s.grapple_force = 0;
    s.grapple_button_state = false;
  }
}

void HandleVisors(const Mem& mem, u32 player, const Input& in)
{
  const u32 powerups = mem.Chain(player, {0x35a0, 0});
  if (powerups == 0)
    return;
  const u32 active_visor = powerups + 0x34;
  for (u32 i = 0; i < 4; i++)
  {
    const bool pressed = in.visor[i] && !s.visor_held[i];
    s.visor_held[i] = in.visor[i];
    if (!pressed)
      continue;
    if (i == 0 && mem.R32(active_visor) == 0)
      continue;
    // Visor ids 0-3 have their powerups at 0x0b-0x0e
    if (mem.R32(powerups + (0x0b + i) * kPowerupsSize + kPowerupsOffset))
      mem.W32(i, active_visor);
  }
}

bool InRidleyFight(const Mem& mem, u32 state_manager)
{
  const u32 world_id = mem.Chain(kWorldIdBase, {0, 0x8});
  const u32 area_id = mem.Chain(state_manager, {0, 0x2150});
  if (world_id == 0 || area_id == 0)
    return false;
  return mem.R64(world_id) == kNorionWorldId && mem.R32(area_id) == kRidleyFightArea;
}

// PrimeHack's run_mod_mp3
void RunGame(const Mem& mem, const Input& in, const Settings& st)
{
  // The lasso animation patches would fight Grapple Voltage (pushing the stick)
  s.group_on[LASSO] = true;
  s.group_on[LASSO_ANIM] = !(in.move_y < -0.5f || in.move_y > 0.5f);

  const u32 cursor = mem.Chain(kCursorBase, {0, 0xc54, 0});

  // Menu screens and dialogs
  if (mem.R8(kCursorDialog))
  {
    HandleCursor(mem, cursor, in, st, false, false);
    return;
  }

  const u32 player = mem.Chain(kStateManager, {0, 0x2184, 0});
  if (player == 0)
    return;

  HandleVisors(mem, player, in);

  if (InRidleyFight(mem, kStateManager))
  {
    // Pointer controls throughout the fight, as the game was written for
    s.all_original = true;
    s.was_in_ridley_fight = true;
    HandleCursor(mem, cursor, in, st, false, true);
    return;
  }
  if (s.was_in_ridley_fight)
  {
    s.all_original = false;
    s.was_in_ridley_fight = false;
  }

  mem.W32(in.lasso ? 1 : 0, kVarTriggerGrapple);

  Transform player_xf;
  mem.ReadTransform(player_xf, player + kXfOffset);

  const u32 pitch_addr = player + 0x77c;
  if (s.reload_pitch)
  {
    s.pitch = std::clamp(mem.RF(pitch_addr), -1.52f, 1.52f);
    if (!std::isfinite(s.pitch))
      s.pitch = 0.f;
    s.reload_pitch = false;
  }

  const u32 lockon_type = player + 0x370;
  const u32 beamvisor_menu = mem.Chain(kWorldIdBase, {0, 0x1708});
  const bool in_visor_menu = beamvisor_menu != 0 && mem.R32(beamvisor_menu) == 3;
  if ((mem.R32(lockon_type) == 0 && mem.R8(kLockonState)) || mem.R32(lockon_type) == 1 ||
      in_visor_menu)
  {
    PitchYawLocked(mem, kStateManager);
    if (in_visor_menu)
      HandleCursor(mem, cursor, in, st, false, true);
    mem.WF(s.pitch, pitch_addr);
    return;
  }

  HandleLasso(mem, lockon_type, in);

  HandleCursor(mem, cursor, in, st, true, true);
  s.cursor_x = s.cursor_y = 0.f;

  s.yaw = std::atan2(player_xf.FwdY(), player_xf.FwdX());

  if (mem.R32(player + 0x358) != 0)  // morph ball
  {
    s.pitch = 0;
    return;
  }

  const u32 control_state = mem.Chain(player, {0x3ad8, 0x7cc});
  if (control_state == 0 || mem.R32(control_state) != 1)
    return;

  const u32 menu_state = kStateManager + 0x32c;
  if (mem.R32(menu_state) == 0)
    PitchYawDelta(in, st);

  mem.WF(s.pitch, pitch_addr);
  player_xf.BuildRotation(s.yaw);
  mem.WriteTransform(player_xf, player + kXfOffset);
  // So the game doesn't spin when the option is turned off
  mem.WF(1e-7f, player + 0x174);
}

bool GameRunning(const Mem& mem)
{
  // The exception hook is 0 while a title is still loading
  return mem.R32(0x80000048) != 0 && mem.R32(0x80000000) == kGameId &&
         mem.R8(0x80000007) == 0;
}
}  // namespace

void SetSettings(const Settings& settings)
{
  std::lock_guard lk(s_mutex);
  s_settings = settings;
}

void SetInput(const Input& input)
{
  std::lock_guard lk(s_mutex);
  s_input = input;
}

void RunFrame(Core::System& system)
{
  Settings st;
  Input in;
  {
    std::lock_guard lk(s_mutex);
    st = s_settings;
    in = s_input;
  }
  if (!st.enabled && !s.active)
    return;

  const Mem mem(system);
  if (!GameRunning(mem))
  {
    if (s.game_seen)
      s = ModState{};
    return;
  }

  if (!s.game_seen)
  {
    // The game's own code, before anything is changed
    s = ModState{};
    s.game_seen = true;
    s.patches = BuildPatches();
    for (Patch& p : s.patches)
    {
      const u32 current = mem.R32(p.address);
      p.original_known = current != p.value;
      p.original = current;
    }
    s.group_on.fill(false);
    s.group_on[ALWAYS] = true;
    s.group_on[LASSO] = true;
    s.group_on[LASSO_ANIM] = true;
    NOTICE_LOG_FMT(CORE, "Metroid Prime 3 controller aiming: Corruption (RM3E01) found");
  }

  if (!st.enabled)
  {
    s.all_original = true;
    ApplyPatches(system, mem);
    s.active = false;
    return;
  }
  if (!s.active)
  {
    s.active = true;
    s.reload_pitch = true;
    s.all_original = false;
  }

  RunGame(mem, in, st);
  ApplyPatches(system, mem);
}

void BeforeStateSave(Core::System& system)
{
  if (!s.active || s.stashed)
    return;
  const Mem mem(system);
  const bool was_original = s.all_original;
  s.all_original = true;
  ApplyPatches(system, mem);
  s.all_original = was_original;
  s.stashed = true;
}

void AfterStateSave(Core::System& system)
{
  if (!s.stashed)
    return;
  s.stashed = false;
  ApplyPatches(system, Mem(system));
}

void AfterStateLoad()
{
  // The loaded RAM holds the game's own code; it's patched again next frame
  s.reload_pitch = true;
  s.interpolating = false;
  s.was_in_ridley_fight = false;
  s.all_original = false;
}

void Shutdown()
{
  s = ModState{};
  std::lock_guard lk(s_mutex);
  s_input = Input{};
}
}  // namespace MP3Controls
