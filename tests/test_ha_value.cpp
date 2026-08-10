// Host test for HaRange — the one piece of Astrolabe that decides whether a number
// from Home Assistant may be shown, and whether it may be sent back.
//
// It is here because this is exactly the part that could not be tested on the
// device. Reproducing "Home Assistant reports a value outside its own stated
// range" needs a misbehaving light; reproducing "the attribute vanished" needs
// that light to be switched off at the right moment. Both are easy here.
//
// Build and run:
//   g++ -std=c++17 -Wall -Wextra -Werror -o /tmp/test_ha_value tests/test_ha_value.cpp && /tmp/test_ha_value

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../components/astrolabe_ui/ha_value.h"

using esphome::astrolabe_ui::HaRange;
using esphome::astrolabe_ui::parse_number;

static int failures = 0;

static void check(bool ok, const char *what) {
  if (!ok) {
    std::printf("FAIL  %s\n", what);
    failures++;
  } else {
    std::printf("ok    %s\n", what);
  }
}

static bool near(float a, float b) { return std::fabs(a - b) < 0.001f; }

int main() {
  /* ── Nothing known yet ───────────────────────────────────────────────── */
  {
    HaRange v;
    check(!v.usable(), "no bounds, no value: not usable");
    check(!v.sendable(), "no bounds, no value: not sendable");
    check(!v.step(1.0f), "no bounds, no value: the knob does nothing");
  }

  /* Bounds without a value is still nothing. A light that says it can do colour
     temperature but has not said which one yet must not be dialled from an
     invented starting point. */
  {
    HaRange v;
    v.set_bounds(2700.0f, 6500.0f);
    check(!v.usable(), "bounds but no value: not usable");
    check(!v.step(200.0f), "bounds but no value: the knob does nothing");
    check(!v.sendable(), "bounds but no value: not sendable");
  }

  /* A value without bounds is equally unusable: there is nothing to clamp to. */
  {
    HaRange v;
    v.set_reported(4000.0f);
    check(!v.usable(), "value but no bounds: not usable");
    check(!v.step(200.0f), "value but no bounds: the knob does nothing");
  }

  /* ── A value inside the range ────────────────────────────────────────── */
  {
    HaRange v;
    v.set_bounds(2700.0f, 6500.0f);
    v.set_reported(4000.0f);
    check(v.usable(), "in range: usable");
    check(near(v.value(), 4000.0f), "in range: shown as reported");
    check(v.sendable(), "in range: may be sent back");
    float out = 0.0f;
    check(v.to_send(&out) && near(out, 4000.0f), "in range: to_send gives the value");
  }

  /* ── The real one: reported outside its own stated range ─────────────── */
  {
    HaRange v;
    v.set_bounds(2700.0f, 6500.0f);
    v.set_reported(65280.0f);  // observed on a real ceiling light, 32 times in 13 days
    check(v.usable(), "out of range: still usable (we can show something)");
    check(near(v.value(), 6500.0f), "out of range: shown clamped to the near end");
    check(!v.sendable(), "OUT OF RANGE IS NOT SENT BACK");
    float out = -1.0f;
    check(!v.to_send(&out), "out of range: to_send refuses");

    // ...until the user turns the knob. Then it is their value, not a guess.
    check(v.step(-200.0f), "out of range: the knob works from the clamped value");
    check(near(v.value(), 6300.0f), "out of range: one step down from 6500 is 6300");
    check(v.sendable(), "after the user turned it: may be sent");
  }

  /* Below the low end clamps the other way. */
  {
    HaRange v;
    v.set_bounds(2700.0f, 6500.0f);
    v.set_reported(500.0f);
    check(near(v.value(), 2700.0f), "below range: shown clamped to the low end");
    check(!v.sendable(), "below range: not sent back");
  }

  /* ── The attribute vanishing ─────────────────────────────────────────── */
  {
    HaRange v;
    v.set_bounds(0.0f, 1.0f);
    v.set_reported(0.4f);
    v.set_chosen(0.6f);
    check(v.sendable(), "chosen: sendable");
    v.clear_reported();  // the player stopped; volume_level is gone
    check(!v.usable(), "attribute gone: not usable");
    check(!v.sendable(), "attribute gone: NOT sendable, even after being chosen");
    check(!v.step(0.05f), "attribute gone: the knob does nothing");
  }

  /* A fresh report from Home Assistant supersedes what the user picked.
     Without this, choosing a value once would make every later out-of-range
     report sendable — which is the whole thing this class exists to stop. */
  {
    HaRange v;
    v.set_bounds(2700.0f, 6500.0f);
    v.set_reported(4000.0f);
    v.step(-200.0f);
    check(v.sendable(), "user picked a value: sendable");
    v.set_reported(65280.0f);  // the light was switched off and on again
    check(near(v.value(), 6500.0f), "after a bad report: shown clamped");
    check(!v.sendable(), "A BAD REPORT AFTER A USER CHOICE IS STILL NOT SENT BACK");
  }

  /* A good report after a user choice stays sendable — nothing is lost. */
  {
    HaRange v;
    v.set_bounds(2700.0f, 6500.0f);
    v.set_reported(4000.0f);
    v.step(-200.0f);
    v.set_reported(3800.0f);
    check(v.sendable(), "a good report after a choice: still sendable");
    float out = 0.0f;
    check(v.to_send(&out) && near(out, 3800.0f), "a good report wins over the old choice");
  }

  /* ── Clamping on the way in ──────────────────────────────────────────── */
  {
    HaRange v;
    v.set_bounds(0.0f, 100.0f);
    v.set_reported(50.0f);
    for (int i = 0; i < 20; i++) {
      v.step(10.0f);
    }
    check(near(v.value(), 100.0f), "stepping up stops at the high end");
    float out = 0.0f;
    check(v.to_send(&out) && near(out, 100.0f), "clamped-by-stepping is still sendable");
    for (int i = 0; i < 40; i++) {
      v.step(-10.0f);
    }
    check(near(v.value(), 0.0f), "stepping down stops at the low end");
  }

  /* Bounds arriving after the value — the attributes are separate subscriptions
     and Home Assistant does not promise an order. */
  {
    HaRange v;
    v.set_reported(65280.0f);
    check(!v.usable(), "value first: not usable until bounds arrive");
    v.set_bounds(2700.0f, 6500.0f);
    check(v.usable(), "value first: usable once bounds arrive");
    check(near(v.value(), 6500.0f), "value first: clamped once bounds are known");
    check(!v.sendable(), "value first: still not sent back");
  }

  /* Bounds can also go away (the entity was replaced by one that cannot do it). */
  {
    HaRange v;
    v.set_bounds(2700.0f, 6500.0f);
    v.set_reported(4000.0f);
    check(v.sendable(), "before: sendable");
    v.clear_bounds();
    check(!v.usable(), "bounds withdrawn: not usable");
    check(!v.sendable(), "bounds withdrawn: not sendable");
  }

  /* Degenerate bounds are refused rather than dividing by nothing later. */
  {
    HaRange v;
    v.set_bounds(50.0f, 50.0f);
    v.set_reported(50.0f);
    check(!v.have_bounds(), "lo == hi is not a range");
    check(!v.usable(), "lo == hi: not usable");
  }

  /* ── parse_number ────────────────────────────────────────────────────── */
  {
    float f = -1.0f;
    check(parse_number("4000", &f) && near(f, 4000.0f), "parse: plain integer");
    check(parse_number("0.35", &f) && near(f, 0.35f), "parse: decimal");
    check(parse_number("-3", &f) && near(f, -3.0f), "parse: negative");
    check(parse_number("65280", &f) && near(f, 65280.0f), "parse: does NOT reject out-of-range");
    check(!parse_number("None", &f), "parse: 'None' is not a number");
    check(!parse_number("", &f), "parse: empty is not a number");
    check(!parse_number("unavailable", &f), "parse: 'unavailable' is not a number");
    check(!parse_number("12abc", &f), "parse: trailing junk is refused");
  }

  std::printf("\n%s\n", failures == 0 ? "all passed" : "FAILURES ABOVE");
  return failures == 0 ? 0 : 1;
}
