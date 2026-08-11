/**
 * @file climate_render.hpp
 * @brief 冷暖房の画面。設定温度を円弧で、いまの室温と運転モードを添えて。
 *
 * ⚠️ **設定温度を知らないうちは円弧を描かない。** `--.-` と出す——
 * 0℃ と「知らない」は別のこと（ライトの明るさ・色温度・カーテンの位置と同じ作法）。
 */
#pragma once

#include <cmath>
#include <cstdio>

#include <LovyanGFX.hpp>

namespace esphome {
namespace astrolabe_ui {

static constexpr int CLIMATE_CX = 120;
static constexpr int CLIMATE_CY = 120;
static constexpr float CLIMATE_ARC_START = 135.0f;
static constexpr float CLIMATE_ARC_SWEEP = 270.0f;
static constexpr int CLIMATE_R_OUT = 98;
static constexpr int CLIMATE_R_IN = 78;

/* ⚠️ 他の画面（真鍮＝light / 布＝cover / 紫＝generic）と分けてある。
   暖房と冷房で色を変えるので、既定色は「どちらでもない」ときの灰。 */
static constexpr uint32_t CLIMATE_BG_COLOR = 0x06080A;
static constexpr uint32_t CLIMATE_TRACK_COLOR = 0x141C24;
static constexpr uint32_t CLIMATE_NEUTRAL_COLOR = 0x607080;
static constexpr uint32_t CLIMATE_HEAT_COLOR = 0xE07040;
static constexpr uint32_t CLIMATE_COOL_COLOR = 0x40A0E0;
static constexpr uint32_t CLIMATE_DRY_COLOR = 0xC0A040;
static constexpr uint32_t CLIMATE_FAN_COLOR = 0x60C0A0;
/** ⚠️ `auto` は**薄緑**（2026-08-11 ゆの）。暖房とも冷房とも言わない色。 */
static constexpr uint32_t CLIMATE_AUTO_COLOR = 0x90D8A8;
static constexpr uint32_t CLIMATE_TEXT_COLOR = 0xC0D0E0;
static constexpr uint32_t CLIMATE_DIM_COLOR = 0x405060;
static constexpr uint32_t CLIMATE_HINT_COLOR = 0x222222;
/** ⚠️ **非対応の警告色。** 送らないことが見えている必要がある（B'）。 */
static constexpr uint32_t CLIMATE_UNSUPPORTED_COLOR = 0xE04040;
/** ⚠️ **非対応のときの円弧。** 灰に落として「いまは効かない」を色で言う
 * （2026-08-11 ゆの「グレーアウトのアーク」）。 */
static constexpr uint32_t CLIMATE_GRAYED_COLOR = 0x303538;

/** 冷暖房に出すもの。⚠️ **描画は状態を持たない**——毎回これを渡し切る。 */
struct ClimateView {
  /** 設定温度（℃）。⚠️ **NaN ＝ 知らない。** `-1` は正当な温度なので番兵に使えない。 */
  float target;
  /** いまの室温（℃）。NaN ＝ 知らない。 */
  float current;
  /** 円弧の範囲。⚠️ `have_range` が false なら円弧を描かない。 */
  float min_temp;
  float max_temp;
  bool have_range;
  /** いま画面が指している運転モードの綴り（`"cool"` など）。⚠️ **フラッシュ常駐**で所有しない。
   * `nullptr` ＝ `modes:` が書かれていない（長押しが効かない）。 */
  const char *mode_name;
  /** ⚠️ **いま指しているモードに機器が対応していないか。**
   * 真なら「非対応」と出し、**何も送っていない**ことを見せる（B'）。 */
  bool mode_unsupported;
  /** ⚠️ **HAが報告している運転モード＝いま実際に動いているもの。**
   * 下部にモードカラーで出す。`nullptr` ＝ 届いていない。
   * ⚠️ **弧の色はこれではなく `mode_name`（表示中）に従う**——
   * 色が食い違っていること自体が「まだ送っていない」の合図（N3）。 */
  const char *reported_name;
  /** 運転しているか（＝報告が `off` 以外）。円弧の色をここで決める。 */
  bool is_on;
  /** 報告が届いているか。false なら状態の行を `----` にする。 */
  bool state_received;
};

/** 運転モードの綴りから円弧の色。⚠️ **暖房と冷房を色で分ける**——
 * 数字を読まなくても、どちらで動いているかが分かる。 */
inline uint32_t climate_mode_color(const char *name, bool is_on) {
  if (name == nullptr || !is_on) {
    return CLIMATE_NEUTRAL_COLOR;
  }
  const std::string n(name);
  if (n == "heat") {
    return CLIMATE_HEAT_COLOR;
  }
  if (n == "cool") {
    return CLIMATE_COOL_COLOR;
  }
  if (n == "dry") {
    return CLIMATE_DRY_COLOR;
  }
  if (n == "fan_only") {
    return CLIMATE_FAN_COLOR;
  }
  if (n == "auto") {
    return CLIMATE_AUTO_COLOR;
  }
  /* `heat_cool` は**どちらの色にも寄せない**——嘘になる。 */
  return CLIMATE_NEUTRAL_COLOR;
}

inline void render_climate(LGFX_Sprite *canvas, const ClimateView &v) {
  canvas->fillScreen(CLIMATE_BG_COLOR);
  canvas->fillArc(CLIMATE_CX, CLIMATE_CY, CLIMATE_R_OUT, CLIMATE_R_IN, CLIMATE_ARC_START,
                  CLIMATE_ARC_START + CLIMATE_ARC_SWEEP, CLIMATE_TRACK_COLOR);

  const bool have_target = !std::isnan(v.target);
  /* ⚠️ **非対応のときは灰に落とす。** 運転中の色（暖房＝橙 / 冷房＝青）で塗ると、
     効いていないのに効いているように見える。 */
  /* ⚠️ **弧は「表示中モード」の色**（N3）。下部の動作中モードと色が違っていれば、
     **まだ送っていない**ことが一目で分かる。 */
  const uint32_t col =
      v.mode_unsupported ? CLIMATE_GRAYED_COLOR : climate_mode_color(v.mode_name, v.is_on);

  /* ⚠️ **消えているときは塗らない。** 参照実装がそうしていた——
     止まっている機器で目盛りが伸びていると、動いているように見える。 */
  if (v.is_on && have_target && v.have_range && v.max_temp > v.min_temp) {
    /* 設定温度の位置まで塗る。⚠️ **範囲は機器に従う**ので、目盛りは個体ごとに違う。 */
    float f = (v.target - v.min_temp) / (v.max_temp - v.min_temp);
    if (f < 0.0f) {
      f = 0.0f;
    }
    if (f > 1.0f) {
      f = 1.0f;
    }
    canvas->fillArc(CLIMATE_CX, CLIMATE_CY, CLIMATE_R_OUT, CLIMATE_R_IN, CLIMATE_ARC_START,
                    CLIMATE_ARC_START + f * CLIMATE_ARC_SWEEP, col);
  }

  canvas->setFont(&fonts::efontCN_24);
  canvas->setTextSize(1.0f);
  char buf[16];

  if (v.mode_unsupported) {
    /* ⚠️ **非対応の面では、温度も室温も出さない。**
       どちらも**いま動かせない値**で、出すと「効きそう」に見える。
       代わりに**どのモードを指しているか**と、**なぜ効かないか**だけを出す。
       ⚠️ 長押しだけが効くので、その案内は残す（下）。 */
    canvas->setTextColor(CLIMATE_UNSUPPORTED_COLOR);
    canvas->drawCenterString(v.mode_name != nullptr ? v.mode_name : "----", CLIMATE_CX, CLIMATE_CY - 34);
    canvas->setTextSize(0.75f);
    /* ⚠️ **2行に割る。** 1行だと縁で切れる。 */
    canvas->drawCenterString("UNSUPPORTED", CLIMATE_CX, CLIMATE_CY + 2);
    canvas->drawCenterString("MODE", CLIMATE_CX, CLIMATE_CY + 24);

    canvas->setFont(&fonts::Font2);
    canvas->setTextSize(1.0f);
    /* ⚠️ **非対応を見ている間も、動作中モードは出し続ける。**
       ここを消すと「エアコンがいまどうなっているか」が分からなくなる——
       **止まっているのか動いているのかは、非対応かどうかとは別の話**。 */
    if (v.state_received) {
      if (v.is_on && v.reported_name != nullptr) {
        canvas->setTextColor(climate_mode_color(v.reported_name, true));
        canvas->drawCenterString(v.reported_name, CLIMATE_CX, 176);
      } else {
        canvas->setTextColor(CLIMATE_DIM_COLOR);
        canvas->drawCenterString("OFF", CLIMATE_CX, 176);
      }
    }
    canvas->setTextColor(CLIMATE_HINT_COLOR);
    /* ⚠️ **効くのは長押しだけ。** ここから出る道を書いておく。 */
    canvas->drawCenterString("hold: mode", CLIMATE_CX, 197);
    canvas->drawCenterString("press: back", CLIMATE_CX, 210);
    return;
  }

  if (have_target) {
    /* ⚠️ **0.5刻みの機器があるので小数第1位まで出す。** 整数だけだと半目盛りが消える。 */
    snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(v.target));
    /* ⚠️ **消えているときは暗く。** 参照実装の作法（`is_on ? text_col : dim_col`）。
       設定温度は残っているが、**いま効いてはいない**。 */
    canvas->setTextColor(v.is_on ? CLIMATE_TEXT_COLOR : CLIMATE_DIM_COLOR);
  } else {
    /* ⚠️ **知らないときは円弧も数字も出さない。** 0℃ と混ぜない。 */
    snprintf(buf, sizeof(buf), "--.-");
    canvas->setTextColor(CLIMATE_DIM_COLOR);
  }
  canvas->drawCenterString(buf, CLIMATE_CX, CLIMATE_CY - 30);

  /* いまの室温。⚠️ **設定温度と紛れないよう小さく、下に置く。** */
  canvas->setFont(&fonts::Font2);
  canvas->setTextSize(1.0f);
  if (!std::isnan(v.current)) {
    snprintf(buf, sizeof(buf), "now %.1f", static_cast<double>(v.current));
    canvas->setTextColor(CLIMATE_DIM_COLOR);
    canvas->drawCenterString(buf, CLIMATE_CX, CLIMATE_CY + 2);
  }

  /* 運転モードの行。⚠️ **画面が指しているモードを出す**——長押しで動かす対象がこれ。 */
  canvas->setFont(&fonts::efontCN_24);
  canvas->setTextSize(0.75f);
  if (v.mode_name != nullptr) {
    /* ⚠️ **消えているときは暗く出す。** ⚠️ **消えていても隠さない**——
       `off` はモードではなく電源の状態なので、**選んでいるモードは出したまま**にする
       （参照実装も、消えている間 COOL/HEAT の選択を暗く出し続けていた）。
       2026-08-11 ゆの「並びの先頭を、オフで表示できませんか？」。 */
    canvas->setTextColor(v.is_on ? CLIMATE_TEXT_COLOR : CLIMATE_DIM_COLOR);
    canvas->drawCenterString(v.mode_name, CLIMATE_CX, CLIMATE_CY + 22);
  } else if (v.state_received && v.reported_name != nullptr) {
    /* `modes:` が書かれていないときは、**HAの報告をそのまま見せる**（動かせないが、読める）。 */
    canvas->setTextColor(CLIMATE_DIM_COLOR);
    canvas->drawCenterString(v.reported_name, CLIMATE_CX, CLIMATE_CY + 22);
  } else {
    canvas->setTextColor(CLIMATE_DIM_COLOR);
    canvas->drawCenterString("----", CLIMATE_CX, CLIMATE_CY + 22);
  }

  canvas->setFont(&fonts::Font2);
  canvas->setTextSize(1.0f);
  /* ⚠️ **下部は「いま実際に動いているモード」をモードカラーで**（N2）。
     ⚠️ オフなら `OFF` を淡色——これは**選べるモードとしての off ではなく実態**なので、
     「off は YAML に書かない限り出さない」とは矛盾しない。
     ⚠️ **届いていないときは言わない**——消えているとも点いているとも決めつけない。 */
  if (v.state_received) {
    if (v.is_on && v.reported_name != nullptr) {
      canvas->setTextColor(climate_mode_color(v.reported_name, true));
      canvas->drawCenterString(v.reported_name, CLIMATE_CX, 176);
    } else {
      canvas->setTextColor(CLIMATE_DIM_COLOR);
      canvas->drawCenterString("OFF", CLIMATE_CX, 176);
    }
  }
  canvas->setTextColor(CLIMATE_HINT_COLOR);
  /* ⚠️ **できることだけ案内する。** `modes:` が空なら長押しは効かないので書かない。 */
  if (v.mode_name != nullptr) {
    canvas->drawCenterString("hold: mode", CLIMATE_CX, 197);
  }
  canvas->drawCenterString("press: back", CLIMATE_CX, 210);
}

}  // namespace astrolabe_ui
}  // namespace esphome
