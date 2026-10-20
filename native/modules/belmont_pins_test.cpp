// SPDX-License-Identifier: GPL-3.0-or-later
#include "belmont_pins.h"
#include <iostream>
#include <stdexcept>
using belmont_pins::Store;
void Check(bool value, const char *why) {
  if (!value)
    throw std::runtime_error(why);
}
std::size_t Count(const Store &s) {
  std::size_t n = 0;
  for (const auto &pin : s.All())
    if (pin)
      ++n;
  return n;
}
void Arm(Store &s, int kind) {
  Check(s.Open(), "open chooser");
  Check(s.Picker() && !s.Armed(), "chooser precedes placement");
  Check(s.Choose(kind), "choose native icon");
  Check(s.Armed() && !s.Picker(), "choice arms without chooser");
}
int main() {
  try {
    Store s;
    Check(!s.Open() && !s.Begin(0) && !s.Capture(1, 10, 20),
          "inactive store cannot choose or place");
    s.Context("A", {0, 0, 100, 100}, true);
    Check(!s.Begin(1) && !s.Capture(2, 10, 20) && !s.Picker() && !s.Armed() &&
              Count(s) == 0,
          "unarmed map tap neither opens chooser nor creates pin");
    Check(s.Open() && s.Picker() && !s.Armed() && Count(s) == 0,
          "pin button opens chooser first");
    Check(!s.Begin(2) && !s.Choose(9) && !s.Choose(-1) && s.Picker(),
          "chooser requires valid icon before map placement");
    Check(s.Choose(3) && s.Armed() && !s.Picker() && Count(s) == 0,
          "icon choice defers pin creation");
    Check(!s.Capture(9, 10, 20) && s.Armed(),
          "unsolicited coordinate cannot place chosen icon");
    Check(s.Begin(10) && s.Waiting() && s.Armed(),
          "armed map tap begins fresh coordinate capture");
    Check(!s.Begin(10) && !s.Capture(10, 10, 20) && !s.Capture(9, 10, 20) &&
              s.Waiting(),
          "pending capture rejects repeated begin and stale SDK sequence");
    Check(s.Capture(11, 10, 20) && !s.Armed() && !s.Waiting() && !s.Picker(),
          "next fresh coordinate places and disarms");
    Check(Count(s) == 1 && s.All()[0]->area == "A" && s.All()[0]->x == 10 &&
              s.All()[0]->y == 20 && s.All()[0]->kind == 3,
          "placed coordinate and chosen native icon retained");
    Check(!s.Begin(11) && !s.Capture(12, 30, 40) && Count(s) == 1,
          "placement is single use");
    Check(s.Open(), "new chooser");
    s.Cancel();
    Check(Count(s) == 1 && !s.Picker(),
          "cancel chooser does not mutate existing pins");
    Arm(s, 5);
    Check(s.Begin(12), "capture before cancel");
    s.Cancel();
    Check(!s.Capture(13, 30, 40) && !s.Armed() && !s.Waiting() && Count(s) == 1,
          "cancel clears armed and pending capture");
    Check(s.Select(0) && s.Picker() && s.Editing() && !s.Armed(),
          "existing pin opens edit chooser");
    Check(!s.Begin(13) && s.Choose(8) && s.All()[0]->kind == 8 &&
              s.All()[0]->x == 10 && s.All()[0]->y == 20 && !s.Armed() &&
              !s.Picker(),
          "edit choice applies immediately without new placement");
    Check(s.Select(0), "select edit for cancel");
    s.Cancel();
    Check(s.All()[0]->kind == 8, "edit cancel preserves pin");
    Check(!s.Select(-1) && !s.Select(32), "invalid pin selection rejected");
    Check(s.Select(0) && s.Remove() && Count(s) == 0 && !s.Armed() &&
              !s.Picker() && !s.Remove(),
          "remove applies only to existing edit target");
    Arm(s, 0);
    Check(s.Begin(20) && !s.Capture(21, NAN, 2) && !s.Waiting() && s.Armed(),
          "invalid fresh coordinate leaves chosen icon armed for retry");
    Check(s.Begin(21) && !s.Capture(22, 101, 2) && s.Armed(),
          "out of bounds does not place");
    Check(s.Begin(22) && !s.Capture(23, INFINITY, 2),
          "infinite coordinate rejected");
    Check(s.Begin(23) && s.Capture(24, 100, 0) && Count(s) == 1,
          "valid boundary coordinate places on retry");
    Check(s.Open(), "chooser before area change");
    s.Context("B", {0, 0, 100, 100}, true);
    Check(!s.Picker() && !s.Armed() && !s.Select(0) && Count(s) == 1,
          "area change cancels chooser and scopes existing pins");
    s.Context("A", {0, 0, 100, 100}, true);
    Arm(s, 4);
    s.Context("A", {0, 0, 100, 100}, true);
    Check(s.Armed() && s.Begin(30), "unchanged context preserves armed state");
    s.Context("A", {0, 0, 100, 100}, true);
    Check(s.Waiting() && s.Capture(31, 40, 50) && Count(s) == 2,
          "unchanged context preserves pending SDK capture");
    Arm(s, 2);
    Check(s.Begin(40), "pending before map bounds change");
    s.Context("A", {0, 0, 200, 200}, true);
    Check(!s.Armed() && !s.Waiting() && !s.Capture(41, 1, 2) && Count(s) == 2,
          "map context bounds change cancels pending placement");
    Arm(s, 1);
    s.Context("A", {0, 0, 200, 200}, false);
    Check(!s.Armed() && !s.Open() && !s.Begin(42) && Count(s) == 2,
          "dim loading disabled context cancels and blocks placement");
    s.Context("A", {0, 0, 200, 200}, true);
    Check(s.Select(0), "select before disabling context");
    s.Context("A", {0, 0, 200, 200}, false);
    Check(!s.Remove() && Count(s) == 2, "disabled edit cannot delete pin");
    s.Context("A", {0, 0, 200, 200}, true);
    Arm(s, 1);
    s.Context("", {0, 0, 200, 200}, true);
    Check(!s.Armed() && !s.Open(), "missing area disables armed placement");
    s.Context("A", {0, 0, NAN, 200}, true);
    Check(!s.Open(), "invalid bounds disable interactions");
    s.Context("A", {0, 0, 200, 200}, true);
    s.Clear();
    for (unsigned n = 0; n < 32; ++n) {
      Arm(s, int(n % 9));
      Check(s.Begin(100 + 2 * n) && s.Capture(101 + 2 * n, n + 1, 2),
            "fill bounded session pins");
    }
    Check(Count(s) == 32 && s.Open() && !s.Choose(0) && s.Picker() &&
              !s.Armed() && !s.Begin(200),
          "full capacity cannot arm a 33rd pin");
    s.Cancel();
    Check(s.Select(0) && s.Choose(8) && s.All()[0]->kind == 8 && Count(s) == 32,
          "full store still edits existing pins");
    s.Context("B", {0, 0, 200, 200}, true);
    Check(s.Open() && !s.Choose(1) && Count(s) == 32,
          "capacity applies across session areas");
    s.Context("A", {0, 0, 200, 200}, true);
    Check(s.Select(0) && s.Remove() && Count(s) == 31, "remove frees one slot");
    Arm(s, 7);
    Check(Count(s) == 31 && s.Begin(201) && s.Capture(202, 5, 6) &&
              Count(s) == 32 && s.All()[0]->kind == 7,
          "replacement reuses freed slot only after map placement");
    Check(s.Open(), "chooser before clear");
    s.Clear();
    Check(Count(s) == 0 && !s.Picker() && !s.Armed() && !s.Waiting(),
          "clear discards all pins and interaction state");
    Check(belmont_pins::NativeImage(0).find("1912900965121370566") !=
                  std::string::npos &&
              belmont_pins::NativeImage(9).empty() &&
              belmont_pins::Image(9).empty(),
          "native icon identities remain bounded");
    std::cout << "pin store tests passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
