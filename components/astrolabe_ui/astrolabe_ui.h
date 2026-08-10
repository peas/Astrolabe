#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "esphome/components/api/custom_api_device.h"
#include "esphome/components/ledc/ledc_output.h"
#include "esphome/core/component.h"

#include "hal_display.hpp"
#include "hal_tp.hpp"
#include "launcher_render.hpp"
#include "smooth_menu.h"

namespace esphome {
namespace astrolabe_ui {

/** M5Dial のリング型ランチャー。
 *
 * ## スレッドの持ち方（⚠️ ここを崩さないこと）
 *
 * - **描画タスク（core 1）が menu / canvas / タッチを所有する。** 他から触らない。
 * - **ESPHomeのメインループ（core 0）は状態の書き込みだけ**を行う。
 *   状態は `std::atomic` なのでロックは要らない。
 * - **描画タスクから Home Assistant を呼ばない。** 操作の意図は**キューへ積み**、
 *   メインループが取り出して `call_homeassistant_service` する。
 *   ⚠️ ESPHomeのAPIはメインループのタスクから呼ぶ前提。別タスクから直接叩くと壊れる。
 *   ⚠️ 同型の穴でヒープ破壊を踏んだ実績がある。
 */
class AstrolabeUI : public Component, public api::CustomAPIDevice {
 public:
  /** スロットの種別。⚠️ **Python側 `SLOT_TYPES` と一緒に増やす。**
   * ⚠️ 数値をYAMLの見た目の順に依存させない——codegen が名前で渡す。 */
  enum class SlotType : uint8_t { LIGHT = 0 };

  /** codegen から呼ばれる。**setup() より前に全件揃う。**
   * @param icon 42x42 の RGB565（**バイト入れ替え済み**）。フラッシュ常駐で、所有しない。
   *        ⚠️ 入れ替えの理屈は `icons.py` の `render_icon` の注意を読むこと。 */
  void add_slot(uint8_t type, const std::string &entity_id, const std::string &tag_up, const std::string &tag_down,
                int x, int y, const uint16_t *icon);

  void setup() override;
  void loop() override;
  void dump_config() override;

  /** ⚠️ **`LATE` ではなく `HARDWARE`（800）。**
   * タッチのリセットは**LCDのリセットピンと共有**されている。既定順のままだと
   * **画面の初期化がタッチより後**に来てタッチ設定を消しにいく。下げないこと。 */
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  /* ESPHome側のコンポーネント（rotary_encoder / binary_sensor）からの入口。
     ⚠️ メインループから呼ばれる。**menu_ を直接触らない**——キュー経由にする。 */
  void go_next();
  void go_last();
  void on_button_pressed();

  /** 手応え用のブザー。⚠️ **省略できる**——鳴らないだけで、他は変わらない。
   * ⚠️ 型が `LEDCOutput` なのは**音程を変えるため**（`update_frequency`）。
   * 素の `FloatOutput` では周波数を実行時に変えられない。 */
  void set_buzzer(ledc::LEDCOutput *buzzer) { this->buzzer_ = buzzer; }

  /* タッチの計器。⚠️ **メインループ（template sensor のラムダ）から呼ばれる。**
     タッチドライバ本体は描画タスクの持ち物なので、**ここは写しを読む**——
     直接 `tp_` を触ると、所有の約束に例外ができる。
     用途: 放置でタッチだけ死ぬ故障の観測。既知の未解決問題で、根治していない。 */

  /** 直近に読み返した G_CTRL。`-1` ＝ 読めなかった。**正常時は 0**。 */
  int touch_g_ctrl() const { return this->g_ctrl_last_.load(); }
  /** 起動直後の G_CTRL。⚠️ 起動時から異常なのか、動作中に化けたのかを分ける。 */
  int touch_g_ctrl_boot() const { return this->g_ctrl_boot_.load(); }
  /** 0以外を観測した累計。**0より大きくなったら、その故障が起きたということ。** */
  int touch_g_ctrl_bad() const { return this->g_ctrl_bad_.load(); }
  /** 読み返した累計。⚠️ **これが止まったら描画タスクが止まっている**——
   * `loop_time` はメインループの生死しか示さないので、こちらが要る。 */
  int touch_g_ctrl_polls() const { return this->g_ctrl_polls_.load(); }

 protected:
  /** リングに置ける上限。⚠️ **Python側 `ring.py` の `SLOTS_MAX` と一致させること。** */
  static constexpr int SLOTS_MAX = 10;
  /** ESPHomeのメインループと同じ優先度。上げると描画がネットワークを押しのける。 */
  static constexpr int UI_TASK_PRIORITY = 1;
  /** 約30fps。 */
  static constexpr int UI_PERIOD_MS = 33;
  /** 描画タスクのスタック（バイト）。⚠️ **余裕は実測して決める**——
   * 下の `STACK_REPORT_MS` が残量を定期的に吐くので、長期稼働の記録から詰められる。 */
  static constexpr int UI_TASK_STACK = 8192;
  /** スタック残量を吐く間隔。⚠️ **長期稼働（A2）で「じわじわ減っていないか」を見るためのもの。**
   * 一度だけ測っても、深い呼び出しがまれにしか通らなければ見逃す。 */
  static constexpr uint32_t STACK_REPORT_MS = 60000;
  /** 無操作でアイドル（時計）へ落ちるまで。 */
  static constexpr uint32_t IDLE_TIMEOUT_MS = 20000;
  /** 時計の書き換え間隔。分表示なので秒単位で描く必要がない。 */
  static constexpr uint32_t CLOCK_RENDER_MS = 500;

  /** 明るさの1目盛り。 */
  static constexpr int BRIGHTNESS_STEP = 8;
  /** 回している間の送信間引き。
   * ⚠️ これが無いとノブ1目盛りごとにHAを叩く。 */
  static constexpr uint32_t PUBLISH_INTERVAL_MS = 120;
  /** ローカル操作の直後にHAからのこだまを無視する時間。
   * ⚠️ これが無いと、回している最中に古い値が返ってきて**ノブと綱引きになる**。 */
  static constexpr uint32_t LOCAL_CHANGE_GUARD_MS = 2000;
  /** 手応えの長さ。⚠️ **短くする**——「鳴った」と分かればよく、長いと操作の邪魔になる。 */
  static constexpr uint32_t BEEP_MS = 20;
  /** 鳴らす強さ（デューティ）。 */
  static constexpr float BEEP_LEVEL = 0.5f;

  /* ⚠️ **音程で入力の種類が分かるようにしてある。**
   * とくに回転は左右で音が違い、**目を離していても回した向きが分かる**。
   * 数値は引き継いだもので、変えると手が覚えた対応が壊れる。 */
  static constexpr uint32_t BEEP_HZ_ROTATE_CW = 6000;
  static constexpr uint32_t BEEP_HZ_ROTATE_CCW = 7000;
  /** ノブのボタンと、ランチャーの中央タップ（**同じ「決定」なので同じ音**）。 */
  static constexpr uint32_t BEEP_HZ_PRESS = 2000;
  /** 起床とトグル——**画面の中で何かが起きた**ことを示す。 */
  static constexpr uint32_t BEEP_HZ_ACTION = 4000;

  /** ランチャーで「開く」と見なす中央円の半径。
   * ⚠️ これが無いと**リング上のアイコンを触っただけで開く**。 */
  static constexpr int CENTER_TAP_RADIUS = 50;

  /** いま出ている画面。⚠️ **描画タスクだけが持ち、描画タスクだけが変える。** */
  enum class Screen : uint8_t { LAUNCHER, CLOCK, APP };

  /** ⚠️ **メインループ → 描画タスクへ渡すのは「生の入力」であって「意図」ではない。**
   *
   * 以前は `GO_NEXT` / `GO_LAST`（＝**ランチャーの意味**）を積んでいた。それだと
   * **時計にいる間に回した分がキューに残り、ランチャーへ戻った瞬間に選択が動く**——
   * 「ボタンでランチャーへ戻ると選択が左右に1個動く」が起きる。
   *
   * 入力を溜め込む側で空読みして塞ぐこともできるが、**入力の解釈を
   * 「画面を知っている唯一の所有者」へ寄せれば、その経路がそもそも存在しなくなる**——
   * フラッシュも排他も要らない。 */
  enum class Input : uint8_t { ROTATE_CW, ROTATE_CCW, BUTTON };

  /** 描画タスク → メインループ。**Home Assistant を呼ぶ意図。**
   * ⚠️ `SET_LIGHT` は `slot` と `brightness` を伴うので、単なる列挙ではなく構造体で渡す。 */
  enum class ActionKind : uint8_t { TOGGLE_SLOT, SET_LIGHT, BEEP };

  struct Action {
    ActionKind kind;
    /** `BEEP` のときの音程。他の種類では見ない。 */
    uint16_t hz;
    uint8_t slot;
    /** `SET_LIGHT` のときの 0-255。`TOGGLE_SLOT` では見ない。 */
    int16_t brightness;
    bool is_on;
  };

  struct SlotConfig {
    SlotType type;
    std::string entity_id;
    int x;
    int y;
  };

  /** 調光画面の編集中の値。⚠️ **描画タスクの持ち物**——
   * HAから来る値（`states_` / `brightness_`）とは別に持つ。
   * 混ぜると、回している最中にこだまが割り込んで**ノブと綱引きになる**。 */
  struct LightAppState {
    int brightness;
    bool is_on;
    bool state_received;
    uint32_t last_local_change_ms;
    uint32_t last_publish_ms;
    bool publish_pending;
  };

  static void ui_task_trampoline(void *arg);
  void ui_task_();
  /** 生の入力を、**いまの画面の意味**へ翻訳して適用する。⚠️ 描画タスクからのみ呼ぶ。 */
  void apply_input_(Input in, uint32_t now);
  /** スロット `index` のアプリを開く。⚠️ **種別で分岐する**——
   * ここに `LIGHT` を直書きしないこと。直書きすると
   * 「0番に別種別を入れても時計からはライトが開く」を踏む。
   * @return 開けたら true。未対応の種別なら false（呼び元はランチャーに留まる）。 */
  bool open_app_(int index, uint32_t now);
  /** 手応えを鳴らす意図を積む。⚠️ **描画タスクから直接 output を触らない**——
   * ESPHomeのコンポーネントはメインループの持ち物。積むだけにする。 */
  void beep_(uint32_t hz);
  /** 調光画面での入力。⚠️ 描画タスクからのみ。 */
  void light_app_input_(Input in, uint32_t now);
  /** 間引きつきの送信。⚠️ 積むだけで、呼ぶのはメインループ。 */
  void light_app_publish_(uint32_t now, bool force);
  void on_ha_state_(std::string entity_id, std::string state);
  /** ⚠️ **属性は素の値の文字列で来る**（`"255"`）。JSONではないので解析しない。
   * ⚠️ 消灯するとHAは `brightness` 属性ごと落とし、**そのとき何も送ってこない**
   * （`manager.py:435` が早期return）。よってここは「最後に届いた明るさ」を保つ。 */
  void on_ha_brightness_(std::string entity_id, std::string value);

  LGFX_StampRing display_;
  /** ⚠️ **`display_` は描画タスクの持ち物**。`dump_config()`（メインループ）から
   * `width()`/`height()` を読むと所有の約束に例外ができるので、
   * `setup()` で写し取っておく。**「例外が1つある規則」は守りにくい。** */
  int panel_w_{0};
  int panel_h_{0};
  FT3267::TP_FT3267 tp_;
  LGFX_Sprite *canvas_{nullptr};
  SMOOTH_MENU::Simple_Menu *menu_{nullptr};
  LauncherRender *render_{nullptr};

  std::vector<SlotConfig> slots_;
  /** 描画側へ渡す見出し。⚠️ `slots_` と**常に同数**であることを `add_slot` が保つ。 */
  std::vector<RenderSlot> render_slots_;
  /** ⚠️ **可変長にしない。** `std::atomic` はコピーも移動もできず vector に載らない。 */
  std::atomic<uint8_t> states_[SLOTS_MAX];
  /** HAから届いた明るさ 0-255。`-1` ＝ **まだ一度も届いていない**。
   * ⚠️ **0 と区別する**——0は「点いているが最小」、-1は「知らない」。 */
  std::atomic<int16_t> brightness_[SLOTS_MAX];

  /* タッチ計器の写し。**描画タスクが書き、メインループが読む。**
   * ⚠️ 生の値はタッチドライバの中にあるが、そこは描画タスクの持ち物なので直接読ませない。 */
  std::atomic<int16_t> g_ctrl_last_{-1};
  std::atomic<int16_t> g_ctrl_boot_{-1};
  std::atomic<uint16_t> g_ctrl_bad_{0};
  std::atomic<uint32_t> g_ctrl_polls_{0};

  /** 描画タスク → メインループ（Home Assistant を呼ぶ意図）。 */
  QueueHandle_t action_queue_{nullptr};
  /** メインループ → 描画タスク（**生の入力**。上の `Input` の注意を読むこと）。 */
  QueueHandle_t input_queue_{nullptr};
  TaskHandle_t ui_task_handle_{nullptr};
  ledc::LEDCOutput *buzzer_{nullptr};
  /** 鳴らし終わる時刻。`0` ＝ 鳴っていない。⚠️ **メインループの持ち物。** */
  uint32_t beep_until_ms_{0};
  uint32_t last_touch_ms_{0};

  /* ⚠️ ここから下は**描画タスクの持ち物**。他のタスクから読まないこと（atomicにしていない）。 */
  Screen screen_{Screen::LAUNCHER};
  /** 最後に「利用者が何かした」時刻。⚠️ **時計にいる間は更新しない**——
   * 時計が毎フレーム「今操作された」と記録すると、タイムアウトを自分で無効化してしまう。
   * **時計はアイドル画面そのもの**なので、
   * そこにタイムアウトを持たせるのは合わない機械に通していた証拠だった。 */
  uint32_t last_activity_ms_{0};
  uint32_t last_clock_render_ms_{0};
  uint32_t last_stack_report_ms_{0};
  /** `Screen::APP` のとき、どのスロットを開いているか。 */
  int app_slot_{-1};
  LightAppState light_app_{};
};

}  // namespace astrolabe_ui
}  // namespace esphome
