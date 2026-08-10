/**
 * @file cover_render.hpp
 * @brief カーテンの画面。開いている割合を円弧で。
 *
 * ⚠️ **位置を知らないうちは円弧を描かない。** `----%` と出す——
 * 0% と「知らない」は別のこと（ライトの明るさ・色温度と同じ作法）。
 */
#pragma once

#include <cstdio>

#include <LovyanGFX.hpp>

namespace esphome {
namespace astrolabe_ui {

static constexpr int COVER_CX = 120;
static constexpr int COVER_CY = 120;
static constexpr float COVER_ARC_START = 135.0f;
static constexpr float COVER_ARC_SWEEP = 270.0f;
static constexpr int COVER_R_OUT = 98;
static constexpr int COVER_R_IN = 78;

/* 布の色。⚠️ 他の画面（真鍮＝light / 青＝色温度 / 紫＝generic）と分けてある。 */
static constexpr uint32_t COVER_BG_COLOR = 0x0A0806;
static constexpr uint32_t COVER_TRACK_COLOR = 0x241C14;
static constexpr uint32_t COVER_FILL_COLOR = 0xB08050;
static constexpr uint32_t COVER_MOVING_COLOR = 0xE0B070;
static constexpr uint32_t COVER_TEXT_COLOR = 0xC09060;
static constexpr uint32_t COVER_DIM_COLOR = 0x4A3A28;
static constexpr uint32_t COVER_HINT_COLOR = 0x222222;

/** カーテンに出すもの。⚠️ **描画は状態を持たない**——毎回これを渡し切る。 */
struct CoverView {
  /** 0-100。⚠️ **`-1` ＝ 位置を知らない。** */
  int position;
  /** 0=不明 / 1=開 / 2=閉 / 3=開いている最中 / 4=閉じている最中 */
  uint8_t state;
  /** ⚠️ 位置指定ができるか。できないならノブの案内を出さない。 */
  bool can_set_position;
  /** ⚠️ 止められるか。できないなら長押しの案内を出さない。 */
  bool can_stop;
};

inline void render_cover(LGFX_Sprite *canvas, const CoverView &v) {
  canvas->fillScreen(COVER_BG_COLOR);
  canvas->fillArc(COVER_CX, COVER_CY, COVER_R_OUT, COVER_R_IN, COVER_ARC_START,
                  COVER_ARC_START + COVER_ARC_SWEEP, COVER_TRACK_COLOR);

  const bool moving = (v.state == 3 || v.state == 4);

  canvas->setFont(&fonts::efontCN_24);
  canvas->setTextSize(1.0f);

  if (v.position >= 0) {
    if (v.position > 0) {
      const float angle = COVER_ARC_START + (v.position / 100.0f) * COVER_ARC_SWEEP;
      /* ⚠️ **動いている間は明るくする。** カーテンは指示してから実際に動き終わるまで
         数秒かかるので、**指示が通ったのか止まっているのか**が分からないと不安になる。 */
      canvas->fillArc(COVER_CX, COVER_CY, COVER_R_OUT, COVER_R_IN, COVER_ARC_START, angle,
                      moving ? COVER_MOVING_COLOR : COVER_FILL_COLOR);
    }
    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", v.position);
    canvas->setTextColor(COVER_TEXT_COLOR);
    canvas->drawCenterString(pct, COVER_CX, COVER_CY - 16);
  } else {
    /* ⚠️ **知らないときは円弧を描かない。** 0% と混ぜない。 */
    canvas->setTextColor(COVER_DIM_COLOR);
    canvas->drawCenterString("----%", COVER_CX, COVER_CY - 16);
  }

  canvas->setTextSize(0.75f);
  const char *label = "";
  switch (v.state) {
    case 1:
      label = "OPEN";
      break;
    case 2:
      label = "CLOSED";
      break;
    case 3:
      label = "OPENING";
      break;
    case 4:
      label = "CLOSING";
      break;
    default:
      /* ⚠️ 届いていないことを、開いているとも閉じているとも言わない。 */
      label = "----";
      break;
  }
  canvas->setTextColor(moving ? COVER_MOVING_COLOR : COVER_DIM_COLOR);
  canvas->drawCenterString(label, COVER_CX, COVER_CY + 14);

  canvas->setFont(&fonts::Font2);
  canvas->setTextSize(1.0f);
  canvas->setTextColor(COVER_HINT_COLOR);
  /* ⚠️ **できることだけ案内する。** 止められない機器で「hold: stop」と書かない。 */
  if (v.can_stop) {
    canvas->drawCenterString("hold: stop", COVER_CX, 197);
  }
  canvas->drawCenterString("press: back", COVER_CX, 210);
}

}  // namespace astrolabe_ui
}  // namespace esphome
