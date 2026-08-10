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
/** 撃った語を光らせる色。⚠️ 待受の案内（暗い紫）との差で「いま反応した」と分かる。 */
static constexpr uint32_t GENERIC_FIRED_COLOR = 0xA080E0;
static constexpr uint32_t GENERIC_OFFLINE_COLOR = 0xA04040;

/** `generic` に出すもの。⚠️ **描画は状態を持たない**——毎回これを渡し切る。 */
struct GenericView {
  const std::string *tag_up;
  const std::string *tag_down;
  /** ⚠️ **どのジェスチャが設定されているか。** 出来ないことを案内に書かない。 */
  bool has_tap;
  bool has_hold;
  bool has_rotate;
  /** ⚠️ **直前に撃ったジェスチャ**（0=tap / 1=hold / 2,3=回転）。`4` ＝ 何も撃っていない。
   *
   * これを出すのは、**こちらが本当に知っているのがここだけ**だから。
   * 「送った」は言えるが「効いた」は言えない（`call_homeassistant_service` は
   * 結果を返さない）ので、**利用者が知りたいことには元々答えられない**。
   * 代わりに答えられるのは「**どのジェスチャとして受け取ったか**」で、
   * ⚠️ **実際に起きる取り違えはそこ**——タップのつもりが長押しになる（閾値500ms）。 */
  uint8_t fired;
  /** ⚠️ 届いていない。**これはこちらが確実に知っていること**なので、はっきり出す。 */
  bool offline;
};

inline void render_generic(LGFX_Sprite *canvas, const GenericView &v) {
  canvas->fillScreen(GENERIC_BG_COLOR);

  canvas->setFont(&fonts::efontCN_24);
  canvas->setTextSize(1.0f);

  if (v.offline) {
    /* ⚠️ **「失敗した」ではなく「届いていない」。** Home Assistant が受け取った後に
       何をしたかは、こちらからは分からない。分かるのは繋がっていないことだけ。 */
    canvas->setTextColor(GENERIC_OFFLINE_COLOR);
    canvas->drawCenterString("OFFLINE", GENERIC_CX, GENERIC_CY - 12);
    canvas->setFont(&fonts::Font2);
    canvas->setTextColor(GENERIC_HINT_COLOR);
    canvas->drawCenterString("press: back", GENERIC_CX, 210);
    return;
  }

  canvas->setTextColor(GENERIC_TAG_COLOR);
  canvas->drawCenterString(v.tag_up->c_str(), GENERIC_CX, GENERIC_CY - 28);
  canvas->drawCenterString(v.tag_down->c_str(), GENERIC_CX, GENERIC_CY - 4);

  /* ⚠️ **案内の語を、撃ったものだけ明るくする。**
     新しい文言を足さない——案内はもう出ているので、**そこが光れば足りる**。
     ⚠️ 語ごとに色を変えるので `drawCenterString` は使えない（中央揃えは文字列単位）。
     幅を測って左から並べる。 */
  canvas->setFont(&fonts::Font2);
  canvas->setTextSize(1.0f);

  struct Word {
    const char *text;
    bool shown;
    bool lit;
  };
  const Word words[] = {
      {"tap", v.has_tap, v.fired == 0},
      {"hold", v.has_hold, v.fired == 1},
      /* 回転は左右どちらでも同じ語。⚠️ **向きは音で分けている**（6kHz / 7kHz）ので、
         画面で分ける必要がない。 */
      {"turn", v.has_rotate, v.fired == 2 || v.fired == 3},
  };
  static constexpr char SEP[] = " / ";

  int total = 0;
  bool first = true;
  for (const auto &w : words) {
    if (!w.shown) {
      continue;
    }
    if (!first) {
      total += canvas->textWidth(SEP);
    }
    total += canvas->textWidth(w.text);
    first = false;
  }

  int x = GENERIC_CX - total / 2;
  const int y = GENERIC_CY + 28;
  first = true;
  for (const auto &w : words) {
    if (!w.shown) {
      continue;
    }
    if (!first) {
      canvas->setTextColor(GENERIC_HINT_COLOR);
      canvas->drawString(SEP, x, y);
      x += canvas->textWidth(SEP);
    }
    canvas->setTextColor(w.lit ? GENERIC_FIRED_COLOR : GENERIC_HINT_COLOR);
    canvas->drawString(w.text, x, y);
    x += canvas->textWidth(w.text);
    first = false;
  }

  canvas->setTextColor(GENERIC_HINT_COLOR);
  canvas->drawCenterString("press: back", GENERIC_CX, 210);
}

}  // namespace astrolabe_ui
}  // namespace esphome
