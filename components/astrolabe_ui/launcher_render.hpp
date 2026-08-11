/**
 * @file launcher_render.hpp
 * @brief ランチャー（リング）の描画。
 *
 * ⚠️ この周辺で過去に踏んだ地雷を持ち込まないこと:
 *   - **境界は `menuItemList.size()` から取る。** 以前は境界をアイコン配列から、
 *     添字をメニュー項目から取っており、両者がずれた瞬間に不正アクセスした。
 *     **境界と添字は同じ配列から取る。**
 *   - 座標計算の `120` は 240x240 の中心。項目座標は**セレクタ軌道 r=60** に置かれており、
 *     ここで `(RING_RADIUS*2)/120` 倍して**アイコン半径 r=95** へ引き伸ばす。
 */
#pragma once

#include <vector>

#include <LovyanGFX.hpp>

#include "smooth_menu.h"

namespace esphome {
namespace astrolabe_ui {

/* Astrolabe の真鍮パレット */
static constexpr uint32_t THEME_COLOR_BG = 0x0D0A04;
static constexpr uint32_t SELECTOR_COLOR = 0xDAA520;
static constexpr uint32_t ICON_TAG_COLOR = 0xD4AA50;
/** アイコンが焼かれていないときの丸。 */
static constexpr uint32_t SLOT_NO_ICON_COLOR = 0x1E1606;

static constexpr int SELECTOR_RADIUS = 5;
static constexpr int ICON_TAG_UP_OFFSET = -24;
static constexpr int ICON_TAG_DOWN_OFFSET = 0;

/* ⚠️ Python側 `ring.py` と一致させること。 */
static constexpr float RING_RADIUS = 95.0f;
static constexpr float ICON_DIAMETER = 42.0f;
static constexpr float ICON_SELECTED_DIAMETER = 47.7f;
/** 焼き込みアイコンの一辺。⚠️ **Python側 `icons.py` の `ICON_PX` と一致させること。** */
static constexpr int ICON_PX = 42;

enum class SlotState : uint8_t { UNKNOWN = 0, OFF = 1, ON = 2 };

struct RenderSlot {
  std::string tag_up;
  std::string tag_down;
  /** 42x42 RGB565（**バイト入れ替え済み**）。フラッシュ常駐・**所有しない**。 */
  const uint16_t *icon{nullptr};
  /** ⚠️ **いまそれが選べるか。** false なら暗く描く。
   * ランチャーのスロットは常に true——**メディアの操作リング**が使う。
   * ⚠️ 「持っていない」ではなく「**いまは無い**」を表す（能力は状態で変わる）。 */
  bool enabled{true};
};

class LauncherRender : public SMOOTH_MENU::SimpleMenuCallback_t {
 public:
  void set_canvas(LGFX_Sprite *canvas) { canvas_ = canvas; }
  /** スロットの見出しとアイコンを渡す。**所有はしない。** */
  void set_slots(const std::vector<RenderSlot> *slots) { slots_ = slots; }

  void renderCallback(const std::vector<SMOOTH_MENU::Item_t *> &menuItemList,
                      const SMOOTH_MENU::RenderAttribute_t &selector,
                      const SMOOTH_MENU::RenderAttribute_t &camera) override {
    if (canvas_ == nullptr || slots_ == nullptr) {
      return;
    }

    canvas_->fillScreen(THEME_COLOR_BG);
    canvas_->fillSmoothCircle(selector.x, selector.y, SELECTOR_RADIUS, SELECTOR_COLOR);

    /* ⚠️ **境界と添字を同じ配列から取る**（過去に踏んだ地雷）。
       スロット表とメニュー項目は同数のはずだが、境界はそれに頼らず小さいほうを使う。 */
    const int item_count = static_cast<int>(menuItemList.size());
    const int slot_count = static_cast<int>(slots_->size());
    const int n = item_count < slot_count ? item_count : slot_count;
    const int icon_r = static_cast<int>(RING_RADIUS * 2.0f);

    for (int i = 0; i < n; i++) {
      const int x = (menuItemList[i]->x - 120) * icon_r / 120 + 120;
      const int y = (menuItemList[i]->y - 120) * icon_r / 120 + 120;

      const bool selected = (i == static_cast<int>(selector.targetItem));
      this->draw_slot_(x, y, (*slots_)[i].icon, selected, (*slots_)[i].enabled);
    }

    /* 中央のタグ。 */
    const int new_r = 20;
    const int tag_x = (selector.x - 120) * new_r / 120 + 120;
    const int tag_y = (selector.y - 120) * new_r / 120 + 120;

    canvas_->setFont(&fonts::efontCN_24);
    canvas_->setTextColor(ICON_TAG_COLOR);
    const int sel = static_cast<int>(selector.targetItem);
    if (sel >= 0 && sel < slot_count) {
      canvas_->drawCenterString((*slots_)[sel].tag_up.c_str(), tag_x, tag_y + ICON_TAG_UP_OFFSET);
      canvas_->drawCenterString((*slots_)[sel].tag_down.c_str(), tag_x, tag_y + ICON_TAG_DOWN_OFFSET);
    } else {
      canvas_->drawCenterString("-. -", tag_x, tag_y + ICON_TAG_UP_OFFSET);
    }
  }

 protected:
  /** スロット1つ。
   *
   * ⚠️ **選択中は拡大する。** `47.7 / 42 ≒ 1.136` 倍。
   * 「選ばれている」は**大きさだけ**で示す。⚠️ **輪や枠は描かない。**
   *
   * ⚠️ **拡大時だけアンチエイリアス版を使う。** `pushImageRotateZoom` は最近傍なので、
   * 1.136倍という半端な倍率だと画素が飛び飛びに複製されてギザギザになる。
   * MDIのグリフは線が細いので目立つ。
   * ⚠️ **等倍側にAAを使わないのは意図的**——倍率1.0でもピボットが半画素に落ちると
   * 全体がぼやける。等倍は画素をそのまま写す経路に残す。
   *
   * ⚠️ **スプライトを持たない。** スロットごとに `LGFX_Sprite` を作る手もあるが、
   * `pushImageRotateZoom` は生データを直接取れるので
   * **10枚ぶん 35KB の内部RAMを使わずに同じ絵が出る**。
   *
   * ⚠️ **ランチャーは状態を表示しない。** 出るのはアイコンだけで、
   * 点いているかどうかはアプリを開いた先で見る。
   * **ここに輪を描き足さないこと**——一度やって「変な枠」になった。 */
  void draw_slot_(int x, int y, const uint16_t *icon, bool selected, bool enabled = true) {
    if (!enabled) {
      /* ⚠️ **いま選べないものは、絵を出さずに輪郭だけ残す。**
         絵を薄く重ねる手もあるが、`pushImage` に減光の引数が無く、
         **アイコンごとに減光した配列を焼く**ことになる——アセットが倍になる。
         ⚠️ 「そこに何かがある」と「いまは使えない」の両方が伝わればよい。 */
      canvas_->fillSmoothCircle(x, y, ICON_DIAMETER / 2.0f, SLOT_NO_ICON_COLOR);
      return;
    }
    if (icon == nullptr) {
      /* 焼き込みが無いとき（本来来ない）。**黙って何も描かない、をしない。** */
      canvas_->fillSmoothCircle(x, y, ICON_DIAMETER / 2.0f, SLOT_NO_ICON_COLOR);
      return;
    }

    /* ⚠️ `TFT_BLACK` は**透明色**。アイコンの四隅（黒）を抜くことで丸く見える。
       ここを別の色にすると四角い板が出る。 */
    if (selected) {
      const float zoom = ICON_SELECTED_DIAMETER / ICON_DIAMETER;
      canvas_->pushImageRotateZoomWithAA(x, y, ICON_PX / 2.0f, ICON_PX / 2.0f, 0.0f, zoom, zoom, ICON_PX, ICON_PX,
                                         icon, (uint16_t) TFT_BLACK);
    } else {
      canvas_->pushImageRotateZoom(x, y, ICON_PX / 2.0f, ICON_PX / 2.0f, 0.0f, 1.0f, 1.0f, ICON_PX, ICON_PX, icon,
                                   (uint16_t) TFT_BLACK);
    }
  }

  LGFX_Sprite *canvas_{nullptr};
  const std::vector<RenderSlot> *slots_{nullptr};
};

}  // namespace astrolabe_ui
}  // namespace esphome
