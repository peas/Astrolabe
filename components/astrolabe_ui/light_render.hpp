/**
 * @file light_render.hpp
 * @brief 調光画面の描画。
 *
 * ⚠️ **色温度（CLR）モードはまだ無い。** 長押しで明るさ⇔色温度を切り替える形を想定しているが、
 * 長押しの判定基盤（指が離れるまで待つ処理）ができていないため 0.1.1 送り。
 * モードの目印も出していない——**切り替えられないのに切り替えられそうな見た目を出さない。**
 */
#pragma once

#include <cstdio>

#include <LovyanGFX.hpp>

namespace esphome {
namespace astrolabe_ui {

/* 円弧の幾何。 */
static constexpr int LIGHT_CX = 120;
static constexpr int LIGHT_CY = 120;
static constexpr float LIGHT_ARC_START = 135.0f;
static constexpr float LIGHT_ARC_SWEEP = 270.0f;
static constexpr int LIGHT_R_OUT = 98;
static constexpr int LIGHT_R_IN = 78;

/* 真鍮のパレット。ランチャーと揃えてある。 */
static constexpr uint32_t LIGHT_BG_COLOR = 0x0D0A04;
static constexpr uint32_t LIGHT_TRACK_COLOR = 0x2A1E06;
static constexpr uint32_t LIGHT_HINT_COLOR = 0x222222;

/** 明るさの帯を描く1枚。**状態を持たない。**
 *
 * @param brightness 0-255。⚠️ **消灯中も最後の値を保つ**——HAは消灯時に
 *        `brightness` 属性ごと落とすので（`manager.py:435` が早期returnする）、
 *        0に倒すと「点けた瞬間に真っ暗から始まる」ことになる。
 * @param state_received まだHAから何も届いていないなら false。
 *        ⚠️ **届いていないことを 0% と区別して描く**（ランチャーの UNKNOWN と同じ理由）。
 */
inline void render_light(LGFX_Sprite *canvas, int brightness, bool is_on, bool state_received) {
  canvas->fillScreen(LIGHT_BG_COLOR);
  canvas->fillArc(LIGHT_CX, LIGHT_CY, LIGHT_R_OUT, LIGHT_R_IN, LIGHT_ARC_START,
                  LIGHT_ARC_START + LIGHT_ARC_SWEEP, LIGHT_TRACK_COLOR);

  if (is_on && brightness > 0) {
    const float angle = LIGHT_ARC_START + (brightness / 255.0f) * LIGHT_ARC_SWEEP;
    const uint32_t col = (brightness > 200) ? 0xDAA520 : (brightness > 120) ? 0xB8860B : 0x7A5C1E;
    canvas->fillArc(LIGHT_CX, LIGHT_CY, LIGHT_R_OUT, LIGHT_R_IN, LIGHT_ARC_START, angle, col);
  }

  /* 四捨五入。 */
  const int pct = (brightness * 100 + 127) / 255;
  char pct_str[8];
  snprintf(pct_str, sizeof(pct_str), "%d%%", pct);

  canvas->setFont(&fonts::efontCN_24);
  canvas->setTextSize(1.0f);
  canvas->setTextColor(is_on ? 0xDAA520 : 0x4A3810);
  canvas->drawCenterString(pct_str, LIGHT_CX, LIGHT_CY - 16);

  canvas->setTextSize(0.75f);
  canvas->setTextColor(is_on ? 0xB8860B : 0x3A2A08);
  canvas->drawCenterString(is_on ? "ON" : "OFF", LIGHT_CX, LIGHT_CY + 14);

  canvas->setFont(&fonts::Font2);
  canvas->setTextSize(1.0f);
  if (!state_received) {
    canvas->setTextColor(0x4A3810);
    canvas->drawCenterString("connecting...", LIGHT_CX, 197);
  }
  canvas->setTextColor(LIGHT_HINT_COLOR);
  canvas->drawCenterString("press: back", LIGHT_CX, 210);
}

}  // namespace astrolabe_ui
}  // namespace esphome
