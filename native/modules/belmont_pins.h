// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
namespace belmont_pins {
inline constexpr std::size_t Capacity = 32, KindCount = 9;
// Native MapMarker_*_DisplayData.markerIcon PPtrs (map-common external CAB2).
// Asset identities only: no game text, pixels or progression table is shipped.
inline constexpr std::array<std::int64_t, KindCount> Sprites{
    1912900965121370566LL, 7944782504571480518LL,  -1464126068056282682LL,
    6194193013819419078LL, -8184545778678978106LL, 5697089712629524934LL,
    932508020663953862LL,  5463948360878696902LL,  -8868760338130556474LL};
inline std::string NativeImage(std::size_t kind) {
  if (kind >= KindCount)
    return {};
  return "module:native:ui-gamemenus_assets_all_"
         "866519f7d6a7057a8ba1c459a5b3f694.bundle#" +
         std::to_string(Sprites[kind]);
}
inline std::string Image(std::size_t kind) {
  return kind < KindCount ? "module:belmont:pin-icon:" + std::to_string(kind)
                          : "";
}
struct Pin {
  std::string area;
  double x{}, y{};
  std::size_t kind{};
};
struct Bounds {
  double x0{}, y0{}, x1{}, y1{};
  bool Valid() const {
    return std::isfinite(x0) && std::isfinite(y0) && std::isfinite(x1) &&
           std::isfinite(y1) && x1 > x0 && y1 > y0;
  }
  bool Contains(double x, double y) const {
    return Valid() && std::isfinite(x) && std::isfinite(y) && x >= x0 &&
           x <= x1 && y >= y0 && y <= y1;
  }
  bool Same(const Bounds &other) const {
    return x0 == other.x0 && y0 == other.y0 && x1 == other.x1 && y1 == other.y1;
  }
};
class Store {
  std::array<std::optional<Pin>, Capacity> pins{};
  std::optional<std::size_t> armed_kind;
  std::string area;
  Bounds bounds;
  bool interactive{}, choosing{}, waiting{};
  std::int64_t old_sequence{-1};
  int editing{-1};
  int FreeSlot() const {
    for (std::size_t n = 0; n < Capacity; ++n)
      if (!pins[n])
        return int(n);
    return -1;
  }

public:
  void Context(std::string current, Bounds b, bool enabled) {
    const bool valid = enabled && !current.empty() && b.Valid();
    if (!valid || area != current || !bounds.Same(b))
      Cancel();
    area = std::move(current);
    bounds = b;
    interactive = valid;
  }
  // Pin button opens the native icon chooser before a map coordinate exists.
  bool Open() {
    if (!interactive)
      return false;
    Cancel();
    choosing = true;
    return true;
  }
  // A map tap can request a fresh SDK coordinate only after an icon is chosen.
  // Do not consume a stale coordinate from the chooser or an earlier map tap.
  bool Begin(std::int64_t sequence) {
    if (!interactive || !armed_kind || choosing || waiting || sequence < -1)
      return false;
    waiting = true;
    old_sequence = sequence;
    return true;
  }
  bool Capture(std::int64_t sequence, double x, double y) {
    if (!interactive || !armed_kind || !waiting || sequence <= old_sequence)
      return false;
    waiting = false;
    if (!bounds.Contains(x, y))
      return false;
    const int target = FreeSlot();
    if (target < 0)
      return false;
    pins[std::size_t(target)] = Pin{area, x, y, *armed_kind};
    Cancel();
    return true;
  }
  bool Select(std::int64_t index) {
    if (!interactive || index < 0 || index >= std::int64_t(Capacity) ||
        !pins[std::size_t(index)] || pins[std::size_t(index)]->area != area)
      return false;
    Cancel();
    editing = int(index);
    choosing = true;
    return true;
  }
  bool Choose(std::int64_t kind) {
    if (!interactive || !choosing || kind < 0 ||
        kind >= std::int64_t(KindCount))
      return false;
    if (editing >= 0) {
      auto &pin = pins[std::size_t(editing)];
      if (!pin || pin->area != area)
        return false;
      pin->kind = std::size_t(kind);
      Cancel();
      return true;
    }
    // At capacity, preserve the chooser for cancel/edit; never arm a 33rd pin.
    if (FreeSlot() < 0)
      return false;
    Cancel();
    armed_kind = std::size_t(kind);
    return true;
  }
  bool Remove() {
    if (!interactive || !choosing || editing < 0)
      return false;
    auto &pin = pins[std::size_t(editing)];
    if (!pin || pin->area != area)
      return false;
    pin.reset();
    Cancel();
    return true;
  }
  void Cancel() {
    armed_kind.reset();
    choosing = waiting = false;
    editing = -1;
    old_sequence = -1;
  }
  void Clear() {
    Cancel();
    for (auto &pin : pins)
      pin.reset();
  }
  bool Waiting() const { return waiting; }
  bool Picker() const { return choosing; }
  bool Editing() const { return choosing && editing >= 0; }
  bool Armed() const { return armed_kind.has_value(); }
  const auto &All() const { return pins; }
};
} // namespace belmont_pins
