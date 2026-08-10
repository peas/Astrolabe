/**
 * @file generic_render.hpp
 * @brief `generic` スロットの画面。**状態を持たないアプリ。**
 *
 * ⚠️ 出せるのは「撃った」ことだけ。`call_homeassistant_service` は撃ちっぱなしで
 * 結果が返らないので、**「効いた」とは言えない**。言えるのは
 * 「送った」と「そもそも繋がっていない」の2つ。
 */
#pragma once

#include <LovyanGFX.hpp>

#include <string>

namespace esphome {
namespace astrolabe_ui {

static constexpr int GENERIC_CX = 120;
static constexpr int GENERIC_CY = 120;

/* 紫寄りの面。⚠️ 他の画面（真鍮＝light / 青＝色温度）と**一目で違う**ようにしてある——
   状態を持たないアプリなので、見間違えると「反応していない」と誤解する。 */
static constexpr uint32_t GENERIC_BG_COLOR = 0x04020A;
static constexpr uint32_t GENERIC_TAG_COLOR = 0x3A2860;
static constexpr uint32_t GENERIC_HINT_COLOR = 0x1A1230;
static constexpr uint32_t GENERIC_SENT_COLOR = 0x6040A0;
static constexpr uint32_t GENERIC_OFFLINE_COLOR = 0xA04040;

/** `generic` に出すもの。⚠️ **描画は状態を持たない**——毎回これを渡し切る。 */
struct GenericView {
  const std::string *tag_up;
  const std::string *tag_down;
  /** 0＝待受 / 1＝送った / 2＝繋がっていない */
  uint8_t result;
  /** ⚠️ **どのジェスチャが設定されているか。** 出来ないことを案内に書かない。 */
  bool has_tap;
  bool has_hold;
  bool has_rotate;
};

inline void render_generic(LGFX_Sprite *canvas, const GenericView &v) {
  canvas->fillScreen(GENERIC_BG_COLOR);

  canvas->setFont(&fonts::efontCN_24);
  canvas->setTextSize(1.0f);

  if (v.result == 1) {
    canvas->setTextColor(GENERIC_SENT_COLOR);
    canvas->drawCenterString("SENT", GENERIC_CX, GENERIC_CY - 12);
  } else if (v.result == 2) {
    /* ⚠️ **「失敗した」ではなく「届いていない」。** Home Assistant が受け取った後に
       何をしたかは、こちらからは分からない。分かるのは繋がっていないことだけ。 */
    canvas->setTextColor(GENERIC_OFFLINE_COLOR);
    canvas->drawCenterString("OFFLINE", GENERIC_CX, GENERIC_CY - 12);
  } else {
    canvas->setTextColor(GENERIC_TAG_COLOR);
    canvas->drawCenterString(v.tag_up->c_str(), GENERIC_CX, GENERIC_CY - 28);
    canvas->drawCenterString(v.tag_down->c_str(), GENERIC_CX, GENERIC_CY - 4);

    /* ⚠️ **書かれていないジェスチャは案内に出さない。** 4つ並べておいて
       3つが無反応だと、壊れているように見える。 */
    std::string hint;
    if (v.has_tap) {
      hint += "tap";
    }
    if (v.has_hold) {
      if (!hint.empty()) {
        hint += " / ";
      }
      hint += "hold";
    }
    if (v.has_rotate) {
      if (!hint.empty()) {
        hint += " / ";
      }
      hint += "turn";
    }

    canvas->setFont(&fonts::Font2);
    canvas->setTextSize(1.0f);
    canvas->setTextColor(GENERIC_HINT_COLOR);
    canvas->drawCenterString(hint.c_str(), GENERIC_CX, GENERIC_CY + 28);
  }

  canvas->setFont(&fonts::Font2);
  canvas->setTextSize(1.0f);
  canvas->setTextColor(GENERIC_HINT_COLOR);
  canvas->drawCenterString("press: back", GENERIC_CX, 210);
}

}  // namespace astrolabe_ui
}  // namespace esphome
