/**
 * @file media_render.hpp
 * @brief メディアの音量ページ。音量を円弧で、再生状態を添えて。
 *
 * ⚠️ **曲名もアーティストも出さない。** 任意の文字列をきれいに出すにはフォントの同梱が要り、
 * それはこのプロジェクトの方針の外（`design_astrolabe_no_bundled_assets`）。
 * ⚠️ **その決定の副産物として、文字列がコアの境界を越えない**——
 * red-team の Y2（`std::string` を跨いで読むとヒープ破壊）が**そもそも成立しない**。
 *
 * ⚠️ **操作リングのページは `LauncherRender` が描く。** ここには無い——
 * リングは既に汎用の描画器があるので、作り直さない。
 */
#pragma once

#include <cmath>
#include <cstdio>

#include <LovyanGFX.hpp>

namespace esphome {
namespace astrolabe_ui {

static constexpr int MEDIA_CX = 120;
static constexpr int MEDIA_CY = 120;
static constexpr float MEDIA_ARC_START = 135.0f;
static constexpr float MEDIA_ARC_SWEEP = 270.0f;
static constexpr int MEDIA_R_OUT = 98;
static constexpr int MEDIA_R_IN = 78;

/* ⚠️ 他の画面（真鍮＝light / 布＝cover / 青＝climate / 紫＝generic）と分けてある。 */
static constexpr uint32_t MEDIA_BG_COLOR = 0x0A0610;
static constexpr uint32_t MEDIA_TRACK_COLOR = 0x1E1428;
static constexpr uint32_t MEDIA_FILL_COLOR = 0x9060D0;
static constexpr uint32_t MEDIA_PAUSED_COLOR = 0x50406A;
static constexpr uint32_t MEDIA_TEXT_COLOR = 0xC0A0E0;
static constexpr uint32_t MEDIA_DIM_COLOR = 0x504060;
static constexpr uint32_t MEDIA_HINT_COLOR = 0x222222;

/** 音量ページに出すもの。⚠️ **描画は状態を持たない**——毎回これを渡し切る。 */
struct MediaView {
  /** 音量（0.0-1.0）。⚠️ **NaN ＝ 知らない。** 0.0 は「消音」という正当な値なので番兵にできない。 */
  float volume;
  /** 再生中か。円弧の色と中央の印をここで決める。 */
  bool is_playing;
  /** 状態が届いているか。false なら「まだ何も言えない」。 */
  bool state_received;
  /** ⚠️ **いま音量を変えられるか。** できないならノブの案内を出さない。 */
  bool can_set_volume;
  /** ⚠️ **いまタップで再生/一時停止できるか。**
   * できないなら**できるように見せない**（2026-08-11 ゆの）。 */
  bool can_play_pause;
};

inline void render_media(LGFX_Sprite *canvas, const MediaView &v) {
  canvas->fillScreen(MEDIA_BG_COLOR);
  canvas->fillArc(MEDIA_CX, MEDIA_CY, MEDIA_R_OUT, MEDIA_R_IN, MEDIA_ARC_START,
                  MEDIA_ARC_START + MEDIA_ARC_SWEEP, MEDIA_TRACK_COLOR);

  const bool have_volume = !std::isnan(v.volume);
  if (have_volume) {
    /* ⚠️ **止まっている間は色を落とす。** 鳴っていないのに満ちた弧が明るいと、
       音が出ている画面に見える（`climate` で消えているとき塗らないのと同じ考え）。 */
    const uint32_t col = v.is_playing ? MEDIA_FILL_COLOR : MEDIA_PAUSED_COLOR;
    float f = v.volume;
    if (f < 0.0f) {
      f = 0.0f;
    }
    if (f > 1.0f) {
      f = 1.0f;
    }
    if (f > 0.0f) {
      canvas->fillArc(MEDIA_CX, MEDIA_CY, MEDIA_R_OUT, MEDIA_R_IN, MEDIA_ARC_START,
                      MEDIA_ARC_START + f * MEDIA_ARC_SWEEP, col);
    }
  }

  canvas->setFont(&fonts::efontCN_24);
  canvas->setTextSize(1.0f);
  char buf[8];
  if (have_volume) {
    /* ⚠️ **%で出す。** 0.0-1.0 のままだと、どちらに回せば大きくなるかが読み取りにくい。 */
    snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(v.volume * 100.0f + 0.5f));
    canvas->setTextColor(MEDIA_TEXT_COLOR);
  } else {
    /* ⚠️ **知らないときは円弧も数字も出さない。** 0% と混ぜない。 */
    snprintf(buf, sizeof(buf), "---%%");
    canvas->setTextColor(MEDIA_DIM_COLOR);
  }
  canvas->drawCenterString(buf, MEDIA_CX, MEDIA_CY - 30);

  /* 再生状態。⚠️ **曲名の代わりに出せる、確かなことがこれ。** */
  canvas->setTextSize(0.75f);
  const char *label = "----";
  if (v.state_received) {
    label = v.is_playing ? "PLAYING" : "PAUSED";
  }
  canvas->setTextColor(v.is_playing ? MEDIA_TEXT_COLOR : MEDIA_DIM_COLOR);
  canvas->drawCenterString(label, MEDIA_CX, MEDIA_CY + 14);

  canvas->setFont(&fonts::Font2);
  canvas->setTextSize(1.0f);
  canvas->setTextColor(MEDIA_HINT_COLOR);
  /* ⚠️ **できることだけ案内する。**
     ⚠️ とくに `tap` は、**できないのに書くと「押せば止まる」と誤解させる**
     （2026-08-11 ゆの「アクションがない時は、タップでplay/pauseを切り替えられるように見せないこと」）。 */
  if (v.can_play_pause) {
    canvas->drawCenterString("tap: play/pause", MEDIA_CX, 184);
  }
  canvas->drawCenterString("hold: actions", MEDIA_CX, 197);
  canvas->drawCenterString("press: back", MEDIA_CX, 210);
}

}  // namespace astrolabe_ui
}  // namespace esphome
