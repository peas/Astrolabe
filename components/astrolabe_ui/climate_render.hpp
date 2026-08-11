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
static constexpr uint32_t CLIMATE_TEXT_COLOR = 0xC0D0E0;
static constexpr uint32_t CLIMATE_DIM_COLOR = 0x405060;
static constexpr uint32_t CLIMATE_HINT_COLOR = 0x222222;
/** ⚠️ **非対応の警告色。** 送らないことが見えている必要がある（B'）。 */
static constexpr uint32_t CLIMATE_UNSUPPORTED_COLOR = 0xE04040;

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
  /** HAが報告している運転モードの綴り。`nullptr` ＝ 届いていない。 */
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
  /* `heat_cool` / `auto` は**どちらの色にも寄せない**——嘘になる。 */
  return CLIMATE_NEUTRAL_COLOR;
}

inline void render_climate(LGFX_Sprite *canvas, const ClimateView &v) {
  canvas->fillScreen(CLIMATE_BG_COLOR);
  canvas->fillArc(CLIMATE_CX, CLIMATE_CY, CLIMATE_R_OUT, CLIMATE_R_IN, CLIMATE_ARC_START,
                  CLIMATE_ARC_START + CLIMATE_ARC_SWEEP, CLIMATE_TRACK_COLOR);

  const uint32_t col = climate_mode_color(v.reported_name, v.is_on);
  const bool have_target = !std::isnan(v.target);

  if (have_target && v.have_range && v.max_temp > v.min_temp) {
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
  if (have_target) {
    /* ⚠️ **0.5刻みの機器があるので小数第1位まで出す。** 整数だけだと半目盛りが消える。 */
    snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(v.target));
    canvas->setTextColor(CLIMATE_TEXT_COLOR);
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
    canvas->setTextColor(v.mode_unsupported ? CLIMATE_UNSUPPORTED_COLOR : CLIMATE_TEXT_COLOR);
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
  if (v.mode_unsupported) {
    /* ⚠️ **「この機器は持っていない」と言い切る。** 送っていないことが見えていないと、
       「切り替えたのに動かない」に見える——それが一番分かりにくい壊れ方。 */
    canvas->setTextColor(CLIMATE_UNSUPPORTED_COLOR);
    canvas->drawCenterString("not supported", CLIMATE_CX, 184);
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
