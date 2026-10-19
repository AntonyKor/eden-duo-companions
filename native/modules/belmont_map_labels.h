// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "chained_echoes_unity.h"
#include <mutex>
namespace belmont_map_labels {
// Explicit initial-fixture language. Does not claim the player's active locale.
// Text is read from installed native district/display/localization records.
class Reader {
public:
  explicit Reader(ce_unity::RangeReader romfs);
  explicit Reader(const EdenDsmodHostApi *host);
  std::optional<std::string> LoadFirstRoomAreaEnglish();
  void ClearCache();

private:
  std::mutex mutex;
  ce_unity::Reader unity;
  std::optional<std::string> cache;
};
} // namespace belmont_map_labels
