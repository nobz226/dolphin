// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PrimeGCControls.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "Common/Assembler/GekkoAssembler.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PrimeGCStrafePatches.h"
#include "Core/System.h"

namespace PrimeGCControls
{
namespace
{
constexpr float kPi = 3.141592654f;
constexpr float kTurnrateRatio = 0.00498665500569808449206349206349f;
// PrimeHack's controller defaults (Horizontal/Vertical Sensitivity)
constexpr float kStickScaleX = 45.f;
constexpr float kStickScaleY = 35.f;
constexpr float kStickDeadzone = 0.15f;
constexpr u32 kOrbitStateGrapple = 5;
constexpr u32 kGameCrosshairColor = 0x4b7ea331;
constexpr u32 kNop = 0x60000000;

enum Group
{
  ALWAYS,
  CROSSHAIR,  // "show_crosshair": the free-aim reticle without holding R
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

enum class Title
{
  Prime1,
  Prime2,
};

// PrimeHack's AddressDBInit.cpp, NTSC-U columns
struct GameDef
{
  const char* name;
  u32 game_id;  // first four bytes of the disc ID
  u8 revision;
  Title title;
  // A word of the game's own code at a patched address, so another build is left alone
  u32 signature_address;
  u32 signature_value;

  u32 state_manager;
  u32 player_offset;
  u32 camera_manager_offset;
  u32 camera_manager_field;
  u32 menu_state_offset;
  u32 world_offset;  // Prime 2 only: the world's load phase gates the mod

  u32 camera_state;
  u32 orbit_state;
  u32 firstperson_pitch;
  u32 xf_offset;

  u32 tweak_player;     // Prime 1: static address; Prime 2: r13 offset
  u32 crosshair_color;  // Prime 1: static address; Prime 2: r13 offset of TweakGui
  u32 free_memory;      // PrimeHack's GuestAllocator zone
};

constexpr u32 kObjectListOffset = 0x810;

constexpr GameDef kGames[] = {
    {"Metroid Prime (GM8E01 0-00)", 0x474D3845, 0, Title::Prime1, 0x800ea15c, 0x388100a4,
     0x8045a1a8, 0x84c, 0x86c, 0, 0xf90, 0, 0x2f4, 0x304, 0x3ec, 0x34, 0x8045c208, 0x8045b678,
     0x805afc00},
    {"Metroid Prime (GM8E01 0-01)", 0x474D3845, 1, Title::Prime1, 0x800ea1d8, 0x388100a4,
     0x8045a388, 0x84c, 0x86c, 0, 0xf90, 0, 0x2f4, 0x304, 0x3ec, 0x34, 0x8045c3e8, 0x8045b698,
     0x805afc00},
    {"Metroid Prime (GM8E01 0-02)", 0x474D3845, 2, Title::Prime1, 0x800ea6e0, 0x388100a4,
     0x8045b208, 0x84c, 0x86c, 0, 0xf90, 0, 0x304, 0x314, 0x3fc, 0x34, 0x8045d268, 0x8045c6d8,
     0x805b0c00},
    {"Metroid Prime 2: Echoes (G2ME01)", 0x47324D45, 0, Title::Prime2, 0x80042994, 0x3b7b0004,
     0x803db6e0, 0x14fc, 0x151c, 0x14, 0x2470, 0x1604, 0x388, 0x3a4, 0x604, 0x24,
     static_cast<u32>(-0x6e3c), static_cast<u32>(-0x6e20), 0x80420000},
};

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
  const GameDef* game = nullptr;
  bool active = false;
  std::vector<Patch> patches;
  std::array<bool, GROUP_COUNT> group_on{};
  bool all_original = false;
  bool stashed = false;
  bool reload_pitch = true;

  float pitch = 0.f;
  float yaw = 0.f;
  bool interpolating = false;
  int delta = 0;
  float start_pitch = 0.f;
};

std::mutex s_mutex;
Settings s_settings;
Input s_input;
ModState s;
const GameDef* s_failed_game = nullptr;  // its patches didn't assemble: stays off

class Mem
{
public:
  explicit Mem(Core::System& system) : m_memory(system.GetMemory()) {}

  static bool Valid(u32 address, u32 size)
  {
    return address >= 0x80000000 && address + size <= 0x81800000;
  }

  u8 R8(u32 a) const { return Valid(a, 1) ? m_memory.Read_U8(a & 0x3fffffff) : 0; }
  u16 R16(u32 a) const { return Valid(a, 2) ? m_memory.Read_U16(a & 0x3fffffff) : 0; }
  u32 R32(u32 a) const { return Valid(a, 4) ? m_memory.Read_U32(a & 0x3fffffff) : 0; }
  float RF(u32 a) const { return std::bit_cast<float>(R32(a)); }
  void W32(u32 v, u32 a) const
  {
    if (Valid(a, 4))
      m_memory.Write_U32(v, a & 0x3fffffff);
  }
  void WF(float v, u32 a) const { W32(std::bit_cast<u32>(v), a); }

  // [base + offset], 0 when that isn't a valid pointer
  u32 Ptr(u32 address) const
  {
    const u32 p = R32(address);
    return Valid(p, 4) ? p : 0;
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

// PrimeHack's assembler has ".defsym Name, address" for functions the patches call;
// Dolphin's doesn't, so each use becomes the absolute `address` form it does take.
std::string ExpandDefsyms(const std::string& source)
{
  std::vector<std::pair<std::string, std::string>> symbols;
  std::string body;
  size_t pos = 0;
  while (pos < source.size())
  {
    size_t end = source.find('\n', pos);
    if (end == std::string::npos)
      end = source.size();
    const std::string line = source.substr(pos, end - pos);
    const size_t start = line.find_first_not_of(" \t");
    if (start != std::string::npos && line.compare(start, 8, ".defsym ") == 0)
    {
      const size_t comma = line.find(',', start);
      const size_t value_start = line.find_first_not_of(" \t", comma + 1);
      const size_t value_end = line.find_first_of(" \t#", value_start);
      std::string name = line.substr(start + 8, comma - start - 8);
      name.erase(name.find_last_not_of(" \t") + 1);
      symbols.emplace_back(name, line.substr(value_start, value_end - value_start));
    }
    else
    {
      body += line;
      body += '\n';
    }
    pos = end + 1;
  }

  const auto is_ident = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
  for (const auto& [name, value] : symbols)
  {
    const std::string replacement = "`" + value + "`";
    for (size_t at = body.find(name); at != std::string::npos; at = body.find(name, at))
    {
      const bool whole = (at == 0 || !is_ident(body[at - 1])) &&
                         (at + name.size() >= body.size() || !is_ident(body[at + name.size()]));
      if (!whole)
      {
        at += name.size();
        continue;
      }
      body.replace(at, name.size(), replacement);
      at += replacement.size();
    }
  }
  return body;
}

void AddAsm(std::vector<Patch>& p, const std::string& source, Group group, bool& ok)
{
  using namespace Common::GekkoAssembler;
  auto result = Assemble(ExpandDefsyms(source), 0);
  if (IsFailure(result))
  {
    ERROR_LOG_FMT(CORE, "Metroid Prime GameCube controller aiming: assembler error: {}",
                  GetFailure(result).FormatError());
    ok = false;
    return;
  }
  for (const CodeBlock& block : GetT(result))
  {
    for (size_t i = 0; i + 4 <= block.instructions.size(); i += 4)
      p.push_back({block.block_address + static_cast<u32>(i),
                   Common::swap32(&block.instructions[i]), group});
  }
}

// PrimeHack's init_mod_mp1_gc (NTSC-U), init_mod_mp1_gc_r1, init_mod_mp1_gc_r2 and
// init_mod_mp2_gc (NTSC-U). Prime 2's vmcall that clears the player pointer when a
// state manager is torn down becomes a small code cave in the free zone.
std::vector<Patch> BuildPatches(const GameDef& g, bool& ok)
{
  std::vector<Patch> p;
  const auto add = [&p](u32 address, u32 value, Group group = ALWAYS) {
    p.push_back({address, value, group});
  };
  const u32 strafe_buf = (g.free_memory + 3) & ~3u;

  if (g.title == Title::Prime1 && g.revision == 0)
  {
    add(0x800ea15c, 0x38810044);  // output cannon bob only for viewbob
    add(0x8000e538, kNop);
    add(0x80014820, 0x4e800020);
    add(0x8000e73c, kNop);
    add(0x8000f810, 0x48000244);
    // Grapple point yaw fix: only the position follows the grapple point
    add(0x8017a18c, 0x7fa3eb78);
    add(0x8017a190, 0x3881006c);
    add(0x8017a194, 0x4bed8cf9);
    // Show the crosshair without considering the R button
    add(0x80016ee4, 0x3b000001, CROSSHAIR);  // li r24, 1
    add(0x80016ee8, 0x8afd09c4, CROSSHAIR);  // lbz r23, 0x9c4(r29)
    add(0x80016eec, 0x53173672, CROSSHAIR);  // rlwimi r23, r24, 6, 25, 25
    add(0x80016ef0, 0x9afd09c4, CROSSHAIR);  // stb r23, 0x9c4(r29)
    add(0x80016ef4, 0x4e800020, CROSSHAIR);  // blr
    AddAsm(p, PrimeGCStrafe::build_strafe_code_100(strafe_buf), ALWAYS, ok);
  }
  else if (g.title == Title::Prime1 && g.revision == 1)
  {
    add(0x800ea1d8, 0x38810044);
    add(0x8000e5b4, kNop);
    add(0x8001489c, 0x4e800020);
    add(0x8000e7b8, kNop);
    add(0x8000f88c, 0x48000244);
    add(0x8017a208, 0x7fa3eb78);
    add(0x8017a20c, 0x3881006c);
    add(0x8017a210, 0x4bed8cf9);
    add(0x80016f60, 0x3b000001, CROSSHAIR);
    add(0x80016f64, 0x8afd09c4, CROSSHAIR);
    add(0x80016f68, 0x53173672, CROSSHAIR);
    add(0x80016f6c, 0x9afd09c4, CROSSHAIR);
    add(0x80016f70, 0x4e800020, CROSSHAIR);
    AddAsm(p, PrimeGCStrafe::build_strafe_code_101(strafe_buf), ALWAYS, ok);
  }
  else if (g.title == Title::Prime1 && g.revision == 2)
  {
    add(0x800ea6e0, 0x38810044);
    add(0x8000e7f4, kNop);
    add(0x80014aec, 0x4e800020);
    add(0x8000e9f8, kNop);
    add(0x8000facc, 0x4800022c);
    add(0x8017a970, 0x7fa3eb78);
    add(0x8017a974, 0x3881006c);
    add(0x8017a978, 0x4bed885d);
    add(0x800171c4, 0x3b000001, CROSSHAIR);  // li r24, 1
    add(0x800171c8, 0x8afd09d4, CROSSHAIR);  // lbz r23, 0x9d4(r29)
    add(0x800171cc, 0x53173672, CROSSHAIR);  // rlwimi r23, r24, 6, 25, 25
    add(0x800171d0, 0x9afd09d4, CROSSHAIR);  // stb r23, 0x9d4(r29)
    add(0x800171d4, 0x4e800020, CROSSHAIR);  // blr
    AddAsm(p, PrimeGCStrafe::build_strafe_code_102(strafe_buf), ALWAYS, ok);
  }
  else if (g.title == Title::Prime2)
  {
    add(0x800bcd44, 0x38810044);  // output cannon bob only for viewbob
    add(0x801aef58, kNop);
    add(0x800129c8, 0x4e800020);
    add(0x801af160, kNop);
    add(0x801b0248, 0x48000078);
    add(0x801af450, 0x48000a34);
    // Strafing with left/right on the left stick
    add(0x8018846c, 0xc022a5b0);
    add(0x80188104, 0x4800000c);
    // Grapple point yaw fix
    add(0x8011d9c4, 0x389d0054);
    add(0x8011d9c8, 0x4bf2d1fd);
    add(0x80015ed8, 0x3aa00001, CROSSHAIR);  // li r21, 1
    add(0x80015edc, 0x8add1268, CROSSHAIR);  // lbz r22, 0x1268(r29)
    add(0x80015ee0, 0x52b63672, CROSSHAIR);  // rlwimi r22, r21, 6, 25, 25
    add(0x80015ee4, 0x9add1268, CROSSHAIR);  // stb r22, 0x1268(r29)
    add(0x80015ee8, 0x4e800020, CROSSHAIR);  // blr
    add(0x800614f8, 0xc022d3e8);
    // Players are nulled as the state manager destroys them, so the mod never writes
    // into a freed player. r0 is reloaded right after this loop step.
    AddAsm(p,
           fmt::format(R"(
.locate 0x{cave:x}
  li r0, 0
  stw r0, 0x14fc(r27)
  addi r27, r27, 4
  b `0x80042998`
.locate 0x80042994
  b `0x{cave:x}`
)",
                       fmt::arg("cave", g.free_memory)),
           ALWAYS, ok);
  }
  return p;
}

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

// calculate_pitchyaw_delta
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

// update_pitchyaw_locked: follow the camera the game aims while locked on
void PitchYawLocked(const Mem& mem, const GameDef& g)
{
  const u32 object_list = mem.Ptr(g.state_manager + kObjectListOffset);
  const u32 camera_manager = mem.Ptr(g.state_manager + g.camera_manager_offset);
  if (object_list == 0 || camera_manager == 0)
    return;
  const u16 camera_uid = mem.R16(camera_manager + g.camera_manager_field);
  if (camera_uid == 0xffff)
    return;
  const u32 camera = mem.R32(object_list + ((camera_uid & 0x3ff) << 3) + 4);
  if (!Mem::Valid(camera, 4))
    return;
  Transform xf;
  mem.ReadTransform(xf, camera + g.xf_offset);
  s.yaw = std::atan2(xf.FwdY(), xf.FwdX());
  s.pitch = std::clamp(std::asin(std::clamp(xf.FwdZ(), -1.f, 1.f)), -1.52f, 1.52f);
}

// PrimeHack's run_mod_mp1_gc and run_mod_mp2_gc
void RunGame(Core::System& system, const Mem& mem, const Input& in, const Settings& st)
{
  const GameDef& g = *s.game;
  const u32 r13 = system.GetPPCState().gpr[13];

  s.group_on[CROSSHAIR] = st.crosshair;

  if (g.title == Title::Prime2)
  {
    const u32 world = mem.Ptr(g.state_manager + g.world_offset);
    // World loading phase 4: complete
    if (world == 0 || mem.R32(world + 0x4) != 4)
      return;
  }

  const u32 player = mem.Ptr(g.state_manager + g.player_offset);
  if (player == 0)
    return;

  if (st.crosshair)
  {
    if (g.title == Title::Prime1)
      mem.W32(kGameCrosshairColor, g.crosshair_color);
    else if (const u32 tweak_gui = mem.Ptr(mem.R32(r13 + g.crosshair_color)))
      mem.W32(kGameCrosshairColor, tweak_gui + 0x268);
  }

  Transform player_xf;
  mem.ReadTransform(player_xf, player + g.xf_offset);
  const u32 pitch_addr = player + g.firstperson_pitch;

  if (s.reload_pitch)
  {
    s.pitch = mem.RF(pitch_addr);
    if (!std::isfinite(s.pitch))
      s.pitch = 0.f;
    s.pitch = std::clamp(s.pitch, -1.52f, 1.52f);
    s.reload_pitch = false;
  }

  const u32 orbit_state = mem.R32(player + g.orbit_state);
  if (orbit_state != kOrbitStateGrapple && orbit_state != 0)
  {
    PitchYawLocked(mem, g);
    mem.WF(s.pitch, pitch_addr);
    return;
  }

  // Wherever the game turned Samus (doors, cutscenes, the grapple swing), turn from there
  s.yaw = std::atan2(player_xf.FwdY(), player_xf.FwdX());

  if (mem.R32(player + g.camera_state) != 0)
  {
    // Morph ball and other third-person cameras; pitch is 0 after returning
    s.pitch = 0;
    if (g.title == Title::Prime2)
      mem.WF(s.pitch, pitch_addr);  // Prime 2 flickers for a frame after unmorphing otherwise
    return;
  }

  // input_disabled(): pause, map and scan screens
  if (mem.R32(g.state_manager + g.menu_state_offset) == 0)
    PitchYawDelta(in, st);

  mem.WF(s.pitch, pitch_addr);
  player_xf.BuildRotation(s.yaw);
  mem.WriteTransform(player_xf, player + g.xf_offset);

  if (g.title == Title::Prime1)
  {
    // Max pitch angle, and how fast the free look turns
    mem.WF(1.52f, g.tweak_player + 0x134);
    mem.WF(1000.f, g.tweak_player + 0x280);
    mem.WF(1000.f, g.tweak_player + 0x2b0);
  }
  else if (const u32 tweak_player = mem.Ptr(mem.R32(r13 + g.tweak_player)))
  {
    // Free look rotation speed, and its pitch half-angle range (degrees)
    mem.WF(1000.f, tweak_player + 0x188);
    mem.WF(87.0896f, tweak_player + 0x184);
  }
}

const GameDef* FindGame(const Mem& mem)
{
  const u32 id = mem.R32(0x80000000);
  const u8 revision = mem.R8(0x80000007);
  // Disc ID ends in "01" (Nintendo, NTSC-U)
  if (mem.R16(0x80000004) != 0x3031)
    return nullptr;
  for (const GameDef& g : kGames)
  {
    if (g.game_id == id && g.revision == revision)
      return &g;
  }
  return nullptr;
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
  if (system.IsWii())
    return;

  const Mem mem(system);
  const GameDef* game = FindGame(mem);
  if (game != s.game && s.game != nullptr)
    s = ModState{};
  if (game == nullptr || game == s_failed_game)
    return;

  if (s.game == nullptr)
  {
    // Not until the game's own code is in memory, and only on the builds PrimeHack knows
    if (mem.R32(game->signature_address) != game->signature_value)
      return;
    s = ModState{};
    s.game = game;
    bool ok = true;
    s.patches = BuildPatches(*game, ok);
    if (!ok || s.patches.empty())
    {
      s = ModState{};
      s_failed_game = game;
      return;
    }
    for (Patch& p : s.patches)
    {
      const u32 current = mem.R32(p.address);
      p.original_known = current != p.value;
      p.original = current;
    }
    s.group_on.fill(false);
    s.group_on[ALWAYS] = true;
    NOTICE_LOG_FMT(CORE, "Metroid Prime GameCube controller aiming: {} found", game->name);
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

  RunGame(system, mem, in, st);
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
  s.all_original = false;
}

void Shutdown()
{
  s = ModState{};
  s_failed_game = nullptr;
  std::lock_guard lk(s_mutex);
  s_input = Input{};
}
}  // namespace PrimeGCControls
