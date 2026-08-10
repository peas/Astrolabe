#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>

namespace esphome {
namespace astrolabe_ui {

/** Home Assistant が報告する「範囲のある数値」。
 *
 * ## なぜ型にしてあるか
 *
 * ⚠️ **未検証の値を送り返すコードが書けないようにするため。**
 *
 * 明るさ・色温度・設定温度・カーテンの位置・音量は、どれも同じ形をしている——
 * **Home Assistant が報告する数値で、属性が無いことも、報告された範囲の外のこともある**。
 * 実例として、あるシーリングライトは点け直すたび `color_temp_kelvin: 65280` を報告する
 * （13日間の記録で32/32）。
 *
 * この扱いを種別ごとに手で書くと、同じ約束が何箇所にも散る。実際 0.1.2 の色温度は
 * **5箇所**で範囲判定を手書きしており、**1箇所忘れれば未検証の値が外へ出る**。
 * しかも忘れても何も言われない。
 *
 * ⚠️ **`value()` は「表示してよい値」しか返さない。`sendable()` が真でなければ
 * `to_send()` は値を渡さない。** 送る側がこの型しか受け取らなければ、
 * 「丸めた推測をそのまま送り返す」経路が**そもそも書けない**。
 *
 * ## 表示と送信を分ける理由
 *
 * **見せるのは推測でよい。動くのは推測ではいけない。**
 *
 * 範囲外の報告は、端へ丸めれば**たいてい実物と合う**（65280を報告するライトは、
 * 実際に最大色温度になっている）。だから**見せる**。
 * しかし 65280 が常に「最大」を意味する保証は無く、それはその統合の癖でしかない。
 * だから**送らない**——利用者がノブを回して**自分で選ぶまでは**。
 */
class HaRange {
 public:
  /** 範囲を教える。⚠️ **範囲が無ければ、この値は何もできない**（`usable()` が false）。 */
  void set_bounds(float lo, float hi) {
    this->lo_ = lo;
    this->hi_ = hi;
    this->have_bounds_ = (hi > lo);
  }
  void clear_bounds() { this->have_bounds_ = false; }
  bool have_bounds() const { return this->have_bounds_; }
  float lo() const { return this->lo_; }
  float hi() const { return this->hi_; }

  /** Home Assistant が報告した生の値を入れる。**丸めない。**
   * ⚠️ 範囲は別の属性として遅れて届くことがあるので、**判定はここではしない**。 */
  void set_reported(float raw) {
    this->raw_ = raw;
    this->have_raw_ = true;
    /* ⚠️ **利用者の選択は、新しい報告で失効する。**
       これを忘れると、次の筋を踏む: 利用者がノブで 6300 を選ぶ → ライトを消して点け直す
       → Home Assistant が 65280 を報告する → 「選ばれた値」の印が残っているせいで
       **丸めた 6500 を送ってよいことになる**。防ごうとしているバグそのもの。
       ⚠️ 報告が範囲内なら `sendable()` はどのみち真になるので、失うものは無い。 */
    this->chosen_ = false;
  }
  /** 属性そのものが無い（`None` が来た・消灯で落ちた）。 */
  void clear_reported() { this->have_raw_ = false; }

  /** 利用者が選んだ値。⚠️ **ここを通った値だけが送ってよい値になる。** */
  void set_chosen(float v) {
    this->raw_ = clamp_(v);
    this->have_raw_ = true;
    this->chosen_ = true;
  }

  /** 画面に出したり、刻みの起点にしてよいか。 */
  bool usable() const { return this->have_bounds_ && this->have_raw_; }

  /** 画面に出す値。⚠️ **範囲の端へ丸めてある。** `usable()` が false のときは意味を持たない。 */
  float value() const { return clamp_(this->raw_); }

  /** ⚠️ **Home Assistant へ送り返してよいか。**
   *
   * 真になるのは2つの場合だけ:
   * - 利用者が選んだ（`set_chosen`）
   * - Home Assistant が**範囲の内側の値**を報告した
   *
   * ⚠️ 範囲外の報告を丸めた値は、**見せてよいが送ってはいけない**。 */
  bool sendable() const {
    if (!this->usable()) {
      return false;
    }
    return this->chosen_ || (this->raw_ >= this->lo_ && this->raw_ <= this->hi_);
  }

  /** 送ってよいなら値を書き込んで true。
   * ⚠️ **送る側はこれだけを使う。** `value()` を送信に使わないこと——
   * `value()` は丸めた推測を返しうる。 */
  bool to_send(float *out) const {
    if (!this->sendable()) {
      return false;
    }
    *out = clamp_(this->raw_);
    return true;
  }

  /** 1目盛り動かす。⚠️ **値を知らないうちは動かない**（`false` を返す）。
   *
   * ⚠️ **恣意的な起点を置かない。** かつて色温度では「知らなければ範囲の中点から」と
   * していたが、それを音量へ広げると**停止中のプレーヤーで1目盛り＝50%**になる。
   * 夜の枕元でそれが起きる。**知らないなら動かさず、黙って断る**方がよい。
   *
   * @return 動いたら true。知らなくて動けなければ false */
  bool step(float delta) {
    if (!this->usable()) {
      return false;
    }
    this->set_chosen(clamp_(this->raw_) + delta);
    return true;
  }

 protected:
  float clamp_(float v) const {
    if (!this->have_bounds_) {
      return v;
    }
    if (v < this->lo_) {
      return this->lo_;
    }
    if (v > this->hi_) {
      return this->hi_;
    }
    return v;
  }

  float lo_{0.0f};
  float hi_{0.0f};
  float raw_{0.0f};
  bool have_bounds_{false};
  bool have_raw_{false};
  /** 利用者が選んだ値かどうか。⚠️ Home Assistant からの報告で**倒れる**——
   * 報告が範囲内なら `sendable()` はどのみち真、範囲外なら**推測に戻る**。 */
  bool chosen_{false};
};

/** 属性の素の値を数として読む。⚠️ **丸めない。範囲の判定もしない。**
 * 範囲は別の属性として遅れて届くので、ここで弾くと**値と範囲の到着順に依存**してしまう。
 * @return 数として読めなければ false（`None` などはここに落ちる） */
inline bool parse_number(const std::string &value, float *out) {
  if (value.empty()) {
    return false;
  }
  char *end = nullptr;
  const float v = strtof(value.c_str(), &end);
  if (end == value.c_str() || *end != '\0') {
    return false;
  }
  *out = v;
  return true;
}

}  // namespace astrolabe_ui
}  // namespace esphome
