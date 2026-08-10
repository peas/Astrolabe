/**
 * @file light_render.hpp
 * @brief 調光画面の描画。明るさ（DIM）と色温度（CLR）の2枚。
 *
 * ⚠️ **`DIM｜CLR` の目印は、切り替えられるときだけ出す。**
 * 色温度に対応していないライトで目印を出すと、
 * **切り替えられないのに切り替えられそうな見た目**になる。
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

/* 色温度の面は**冷たい側**へ振ってある。真鍮のままだと、
   温かい色を選んでいるのか背景の色なのかが見分けにくい。 */
static constexpr uint32_t LIGHT_CT_BG_COLOR = 0x08090D;
static constexpr uint32_t LIGHT_CT_TRACK_COLOR = 0x181A22;

/** 調光画面に出すもの。⚠️ **描画は状態を持たない**——毎回これを渡し切る。 */
struct LightView {
  /** 0-255。⚠️ **消灯中も最後の値を保つ**——HAは消灯時に `brightness` 属性ごと落とし、
   * そのとき何も送ってこない（`manager.py:435` が早期returnする）。
   * 0に倒すと「点けた瞬間に真っ暗から始まる」ことになる。 */
  int brightness;
  bool is_on;
  /** まだHAから何も届いていないなら false。
   * ⚠️ **届いていないことを 0% と区別して描く**（ランチャーの UNKNOWN と同じ理由）。 */
  bool state_received;
  /** 色温度の面を描くか。 */
  bool color_temp_mode;
  /** 色温度（K）。⚠️ **`-1` ＝ 確からしい値を知らない**——このとき円弧を描かない。 */
  int color_temp;
  int color_temp_min;
  int color_temp_max;
  /** ⚠️ **`DIM｜CLR` の目印を出すか。** 切り替えられないなら出さない。 */
  bool color_temp_available;
};

inline void render_light(LGFX_Sprite *canvas, const LightView &v) {
  if (v.color_temp_mode) {
    canvas->fillScreen(LIGHT_CT_BG_COLOR);
    canvas->fillArc(LIGHT_CX, LIGHT_CY, LIGHT_R_OUT, LIGHT_R_IN, LIGHT_ARC_START,
                    LIGHT_ARC_START + LIGHT_ARC_SWEEP, LIGHT_CT_TRACK_COLOR);

    canvas->setFont(&fonts::efontCN_24);
    canvas->setTextSize(1.0f);

    if (v.color_temp >= 0 && v.color_temp_max > v.color_temp_min) {
      const float t = static_cast<float>(v.color_temp - v.color_temp_min) /
                      static_cast<float>(v.color_temp_max - v.color_temp_min);
      /* 暖色（濃いオレンジ）から寒色（青白）へ。 */
      const uint8_t r = static_cast<uint8_t>(0xFF - static_cast<int>(0x37 * t));
      const uint8_t g = static_cast<uint8_t>(0xA0 + static_cast<int>(0x40 * t));
      const uint8_t b = static_cast<uint8_t>(0x40 + static_cast<int>(0xBF * t));
      const uint32_t col = (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;

      canvas->fillArc(LIGHT_CX, LIGHT_CY, LIGHT_R_OUT, LIGHT_R_IN, LIGHT_ARC_START,
                      LIGHT_ARC_START + t * LIGHT_ARC_SWEEP, col);

      char k_str[12];
      snprintf(k_str, sizeof(k_str), "%dK", v.color_temp);
      canvas->setTextColor(col);
      canvas->drawCenterString(k_str, LIGHT_CX, LIGHT_CY - 16);

      canvas->setTextSize(0.75f);
      const uint32_t dim_col =
          (static_cast<uint32_t>(r / 3) << 16) | (static_cast<uint32_t>(g / 3) << 8) | (b / 3);
      canvas->setTextColor(dim_col);
      canvas->drawCenterString(t < 0.5f ? "WARM" : "COOL", LIGHT_CX, LIGHT_CY + 14);
    } else {
      /* ⚠️ **知らないときは円弧を描かない。** 中点あたりを塗ると
         「いまこの色です」と言ったことになる。数字も伏せる。 */
      canvas->setTextColor(0x2A2A3A);
      canvas->drawCenterString("----K", LIGHT_CX, LIGHT_CY - 16);
    }
  } else {
    canvas->fillScreen(LIGHT_BG_COLOR);
    canvas->fillArc(LIGHT_CX, LIGHT_CY, LIGHT_R_OUT, LIGHT_R_IN, LIGHT_ARC_START,
                    LIGHT_ARC_START + LIGHT_ARC_SWEEP, LIGHT_TRACK_COLOR);

    if (v.is_on && v.brightness > 0) {
      const float angle = LIGHT_ARC_START + (v.brightness / 255.0f) * LIGHT_ARC_SWEEP;
      const uint32_t col =
          (v.brightness > 200) ? 0xDAA520 : (v.brightness > 120) ? 0xB8860B : 0x7A5C1E;
      canvas->fillArc(LIGHT_CX, LIGHT_CY, LIGHT_R_OUT, LIGHT_R_IN, LIGHT_ARC_START, angle, col);
    }

    /* 四捨五入。 */
    const int pct = (v.brightness * 100 + 127) / 255;
    char pct_str[8];
    snprintf(pct_str, sizeof(pct_str), "%d%%", pct);

    canvas->setFont(&fonts::efontCN_24);
    canvas->setTextSize(1.0f);
    canvas->setTextColor(v.is_on ? 0xDAA520 : 0x4A3810);
    canvas->drawCenterString(pct_str, LIGHT_CX, LIGHT_CY - 16);

    canvas->setTextSize(0.75f);
    canvas->setTextColor(v.is_on ? 0xB8860B : 0x3A2A08);
    canvas->drawCenterString(v.is_on ? "ON" : "OFF", LIGHT_CX, LIGHT_CY + 14);
  }

  canvas->setFont(&fonts::Font2);
  canvas->setTextSize(1.0f);

  /* ⚠️ **切り替えられるときだけ目印を出す。** 出ていないことが
     「長押ししても何も起きない」の説明になっている。 */
  if (v.color_temp_available) {
    canvas->setTextColor(v.color_temp_mode ? 0x2A1E06 : 0xDAA520);
    canvas->drawCenterString("DIM", LIGHT_CX - 22, 183);
    canvas->setTextColor(0x3A3A3A);
    canvas->drawCenterString("|", LIGHT_CX, 183);
    canvas->setTextColor(v.color_temp_mode ? 0xA0C8FF : 0x1A2030);
    canvas->drawCenterString("CLR", LIGHT_CX + 22, 183);
  }

  if (!v.state_received) {
    canvas->setTextColor(v.color_temp_mode ? 0x2A2A3A : 0x4A3810);
    canvas->drawCenterString("connecting...", LIGHT_CX, 197);
  }
  canvas->setTextColor(LIGHT_HINT_COLOR);
  canvas->drawCenterString("press: back", LIGHT_CX, 210);
}

}  // namespace astrolabe_ui
}  // namespace esphome
