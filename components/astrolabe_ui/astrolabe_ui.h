#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "esphome/components/api/custom_api_device.h"
#include "esphome/components/ledc/ledc_output.h"
#include "esphome/core/component.h"

#include "ha_value.h"
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
  enum class SlotType : uint8_t { LIGHT = 0, CLIMATE = 1, COVER = 2, MEDIA_PLAYER = 3, GENERIC = 4 };

  /** `generic` スロットのジェスチャ。⚠️ **Python側 `GESTURES` と一緒に増やす。**
   * ⚠️ 名前は利用者が書くYAMLのキーそのもの——`press_` を付けないのは、
   * **ノブのボタンが「戻る」に割り当て済み**で混同するため。 */
  enum class Gesture : uint8_t { TAP = 0, HOLD = 1, ROTATE_RIGHT = 2, ROTATE_LEFT = 3, COUNT = 4 };

  /** codegen から呼ばれる。**setup() より前に全件揃う。**
   * @param icon 42x42 の RGB565（**バイト入れ替え済み**）。フラッシュ常駐で、所有しない。
   *        ⚠️ 入れ替えの理屈は `icons.py` の `render_icon` の注意を読むこと。 */
  void add_slot(uint8_t type, const std::string &entity_id, const std::string &tag_up, const std::string &tag_down,
                int x, int y, const uint16_t *icon);

  /** `generic` スロットのジェスチャに、呼ぶサービスと対象を結び付ける。
   *
   * ⚠️ **サービス名はPython側で解決済み**（`"script.turn_on"` のような完全な名前）。
   * C++側に表を持たせない——表がPython側にあれば、**押せない組み合わせはビルドで落ちる**。
   * ⚠️ `add_slot` の**あと**に呼ばれる（codegenがその順で出す）。 */
  void add_gesture(int slot, uint8_t gesture, const std::string &service, const std::string &entity_id);

  /** `cover` の布がどちら側から伸びるか。⚠️ **見た目だけ。** */
  void set_cover_opening(int slot, uint8_t opening);
  /** `climate` の運転モードを1つ、YAMLに書かれた順で足す。⚠️ **`setup()` より前に呼ばれる。** */
  void add_climate_mode(int slot, uint8_t mode);

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

  /** いま焼き込まれているスロットの数。
   * ⚠️ **起動ログを取り逃しても分かるようにするための口。** `dump_config` にも出るが、
   * それは起動時にしか流れず、**ログクライアントが繋がる前に終わっている**
   * （実際にこれで2回取り逃した）。**値として出しておけば後からでも読める。**
   * ⚠️ `slots_` は `setup()` より前に埋まり、以後変わらない。だからロックは要らない。 */
  int slot_count() const { return static_cast<int>(this->slots_.size()); }

  /** 直近に読み返した G_CTRL。`-1` ＝ 読めなかった。**正常時は 0**。 */
  int touch_g_ctrl() const { return this->g_ctrl_last_.load(); }
  /** 起動直後の G_CTRL。⚠️ 起動時から異常なのか、動作中に化けたのかを分ける。 */
  int touch_g_ctrl_boot() const { return this->g_ctrl_boot_.load(); }
  /** 0以外を観測した累計。**0より大きくなったら、その故障が起きたということ。** */
  int touch_g_ctrl_bad() const { return this->g_ctrl_bad_.load(); }
  /** 読み返した累計。⚠️ **これが止まったら描画タスクが止まっている**——
   * `loop_time` はメインループの生死しか示さないので、こちらが要る。 */
  int touch_g_ctrl_polls() const { return this->g_ctrl_polls_.load(); }
  /** タッチの読み取りに失敗した累計。
   * ⚠️ **これは0にならない。** 実機で十数秒に1回の割合で観測される。
   * 押している最中に起きると、素朴な作りでは「指が離れた」に化ける——
   * 状態機械はそのフレームを捨てているので、**この数が伸びること自体は異常ではない**。
   * 急に増えたら、バスか配線の方を疑う手がかりになる。 */
  int touch_read_fails() const { return this->tp_read_fail_.load(); }
  /** ⚠️ **キューが詰まって捨てた操作の回数。** 0でなければ、押した/回した回数と
   * Home Assistant が受けた回数が食い違っている。 */
  int dropped_actions() const { return this->dropped_actions_.load(); }

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
  /** カーテンの1目盛り（%）。 */
  static constexpr int COVER_POSITION_STEP = 5;
  /** ⚠️ **カーテンだけ送信の方針が違う。** 回している間は送らず、
   * **手が止まってからこの時間だけ待って、最終位置だけ**送る。
   * 逐次送ると**物理カーテンの移動が毎回割り込まれ、ほとんど動かない**。 */
  static constexpr uint32_t COVER_SETTLE_MS = 400;
  /** タップで開くか閉じるかの境目（%）。 */
  static constexpr int COVER_HALFWAY = 50;

  /** 色温度の1目盛り（K）。⚠️ **範囲と違ってこれは固定**——
   * 範囲は個体ごとに違うので Home Assistant に従うが、刻みの粗さは好みの問題で、
   * 「同じ製品の別の個体で当然に違うもの」ではない。 */
  static constexpr int COLOR_TEMP_STEP = 200;
  /** 設定温度の1目盛り。⚠️ **`target_temp_step` が届かなかったときだけ**使う（Y4）。
   * 実機には 1 の機器と 0.5 の機器の両方があった——固定にすると必ずどちらかと喧嘩する。 */
  static constexpr float CLIMATE_TEMP_STEP_DEFAULT = 0.5f;
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
  /** 長押しでのモード切替。⚠️ **他のどれとも違う音にする**——
   * 同じ場所を押していても起きることが違うので、目を離していたら音でしか分からない。 */
  static constexpr uint32_t BEEP_HZ_MODE = 3000;
  /** モード切替だけ長く鳴らす。**画面の意味が変わる**ので、他より重い手応えにする。 */
  static constexpr uint32_t BEEP_MS_MODE = 40;
  /** ⚠️ **届かなかったときの音。** 低く長い——「効いた」と聞き分けられないと、
   * 繋がっていないことに気づけない。 */
  static constexpr uint32_t BEEP_HZ_OFFLINE = 800;
  /** `generic` が「いま反応した」を見せておく時間。 */
  static constexpr uint32_t GENERIC_RESULT_MS = 1200;

  /** ランチャーで「開く」と見なす中央円の半径。
   * ⚠️ これが無いと**リング上のアイコンを触っただけで開く**。 */
  static constexpr int CENTER_TAP_RADIUS = 50;
  /** アプリの中でタッチを受ける中央円の半径。
   * ⚠️ **全画面で受けない。** 長押しでモードが変わるので、全画面のままだと
   * **ダイヤルの縁を握っただけで切り替わる**。 */
  static constexpr int APP_TAP_RADIUS = 70;

  /** これ未満の接触はゴーストとして捨てる。 */
  static constexpr uint32_t TOUCH_GHOST_MS = 70;
  /** これ以上で長押し。⚠️ **閾値を跨いだ瞬間に撃つ**——離してから判定しない。
   * 離してから決めると「ライトは打ち切って反応するのにボタンはしない」になる。 */
  static constexpr uint32_t TOUCH_LONGPRESS_MS = 500;
  /** 接触がこれ以上続いたら見限る。
   * ⚠️ **タッチ点数レジスタが非0に張り付く故障がある**（`hal_tp.hpp` 参照）。
   * そのとき指が触れていないのに「押しっぱなし」に見えるので、受け皿が要る。 */
  static constexpr uint32_t TOUCH_MAX_PRESS_MS = 10000;

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
  enum class ActionKind : uint8_t {
    TOGGLE_SLOT,
    SET_LIGHT,
    BEEP,
    CALL_GESTURE,
    COVER_ACTION,
    SET_COVER_POSITION,
    SET_CLIMATE_TEMP,
    SET_HVAC_MODE,
  };

  struct Action {
    ActionKind kind;
    /** `BEEP` のときの音程。他の種類では見ない。 */
    uint16_t hz;
    /** `BEEP` のときの長さ。他の種類では見ない。 */
    uint16_t ms;
    uint8_t slot;
    /** `SET_LIGHT` のときの 0-255。`TOGGLE_SLOT` では見ない。 */
    int16_t brightness;
    /** `SET_LIGHT` のときに添える色温度（K）。⚠️ **`-1` ＝ 添えない。**
     * 既定値ではなく逃げ道——**利用者が選んでいない色温度を送り返さない**ため。 */
    int16_t color_temp;
    /** `CALL_GESTURE` のときにどのジェスチャか。 */
    uint8_t gesture;
    /** `COVER_ACTION` のときのサービス名。⚠️ **キューに文字列は載せられない**ので、
     * `cover.` に続く部分を指す**フラッシュ常駐のポインタ**を渡す
     * （`"open_cover"` などの文字列リテラル。所有しない）。 */
    const char *cover_service;
    /** `SET_COVER_POSITION` のときの 0-100。 */
    int16_t position;
    /** `SET_CLIMATE_TEMP` のときの設定温度（℃）。 */
    float temperature;
    /** `SET_HVAC_MODE` のときの `HvacMode`。 */
    uint8_t hvac_mode;
    bool is_on;
  };

  /** ジェスチャの呼び先。⚠️ **`service` が空なら設定されていない。** */
  struct GestureTarget {
    std::string service;
    std::string entity_id;
  };

  /** 布がどちら側から伸びるか。
   * ⚠️ **見た目だけではない**——`RIGHT` は**ノブの向きも反転する**。
   * 布の縁が反時計回りに伸びるので、そのままだと右回しで縁が左へ動く。
   * 詳細は `cover_app_input_`。
   * ⚠️ Python側 `COVER_OPENINGS` と一緒に増やす。 */
  enum class CoverOpening : uint8_t { CENTER = 0, LEFT = 1, RIGHT = 2 };

  /** Home Assistant の `HVACMode` の全語彙。
   * ⚠️ **一次情報から取っている**（`homeassistant/components/climate/const.py`。2026-08-11 確認）。
   * ⚠️ Python側 `CLIMATE_MODES` と**名前で**対応させる（並びに依存しない）。
   * `UNKNOWN` は**HAの語彙ではない**——まだ届いていない／読めなかったとき用。 */
  enum class HvacMode : uint8_t {
    OFF = 0,
    HEAT = 1,
    COOL = 2,
    HEAT_COOL = 3,
    AUTO = 4,
    DRY = 5,
    FAN_ONLY = 6,
    UNKNOWN = 255,
  };

  /** 調光画面のモード。⚠️ **長押しで行き来する。** */
  enum class LightMode : uint8_t { DIMMER, COLOR_TEMP };

  struct SlotConfig {
    SlotType type;
    /** ⚠️ **`generic` では空。** 空なら状態を購読しない。 */
    std::string entity_id;
    int x;
    int y;
    /** ⚠️ `generic` 以外では全部空のまま。 */
    GestureTarget gestures[static_cast<int>(Gesture::COUNT)];
    /** ⚠️ `cover` のときだけ意味がある。 */
    CoverOpening opening;
    /** ⚠️ `climate` のときだけ。**YAMLに書かれた順**の運転モード。長押しで一周する。
     * ⚠️ **空なら長押しは効かず、案内にも出さない**（`cover` の `can_stop` と同じ作法）。
     * ⚠️ **機器が対応しているかはここには入っていない**——対応は `hvac_modes_` を見る。
     * 対応していないモードも並びから外さない（**B'**: 間違いが見えるうえに、無効な値は送らない）。 */
    std::vector<uint8_t> modes;
  };

  /** `generic` の画面の直後の状態。
   *
   * ⚠️ **「送った」ではなく「どのジェスチャとして受け取ったか」を出す。**
   * `call_homeassistant_service` は結果を返さないので「効いた」は言えず、
   * 「送った」は利用者の知りたいことに答えていない。答えられるのは
   * **こちらがどう解釈したか**で、⚠️ **実際に起きる取り違えもそこ**——
   * タップのつもりが長押しになる（閾値500ms）。 */
  /** カーテンの状態。⚠️ ライトの ON/OFF とは別の語彙なので、混ぜない。 */
  enum class CoverState : uint8_t { UNKNOWN, OPEN, CLOSED, OPENING, CLOSING };


  /** ⚠️ **Home Assistant の `supported_features` のビット**（`cover/const.py`）。
   * 持っていない機能は**断る**——0.1.1の色温度と同じ作法。 */
  static constexpr uint32_t COVER_FEAT_OPEN = 1;
  static constexpr uint32_t COVER_FEAT_CLOSE = 2;
  static constexpr uint32_t COVER_FEAT_SET_POSITION = 4;
  static constexpr uint32_t COVER_FEAT_STOP = 8;

  /** 冷暖房の画面の編集中の値。⚠️ **描画タスクの持ち物。** */
  struct ClimateAppState {
    /** 設定温度。⚠️ **範囲つきの値**なので、未着なら回しても動かず、送りもしない。 */
    HaRange target;
    /** いまHAが言っている運転モード。 */
    HvacMode reported;
    /** ⚠️ **YAMLの並びの何番目を指しているか。** 長押しで進む。
     * `slots_[slot].modes` が空なら意味を持たない。 */
    uint8_t cursor;
    /** ⚠️ **カーソルがまだHAの報告に合わせられていない。**
     * 初回だけHAのモードへ寄せ、以後は利用者の操作で動く。 */
    bool cursor_synced;
    uint32_t last_local_change_ms;
    uint32_t last_publish_ms;
    bool publish_pending;
  };

  struct CoverAppState {
    /** 0-100。⚠️ **範囲つきの値**なので、未着なら回しても動かず、送りもしない。 */
    HaRange position;
    CoverState state;
    uint32_t last_local_change_ms;
    bool publish_pending;
  };

  struct GenericAppState {
    /** 直前に撃ったもの。`Gesture::COUNT` ＝ 何も撃っていない。 */
    Gesture fired;
    /** ⚠️ 届いていない。**確実に知っていること**なので、はっきり出す。 */
    bool offline;
    uint32_t at_ms;
  };

  /** 調光画面の編集中の値。⚠️ **描画タスクの持ち物**——
   * HAから来る値（`states_` / `brightness_`）とは別に持つ。
   * 混ぜると、回している最中にこだまが割り込んで**ノブと綱引きになる**。 */
  struct LightAppState {
    int brightness;
    LightMode mode;
    /** 色温度（K）。⚠️ **範囲つきの値**なので、未着なら回しても動かず、送りもしない。
     * 表示と送信を分ける理由（`65280` を報告するライトの話）は `ha_value.h` にある。 */
    HaRange color_temp;
    bool is_on;
    bool state_received;
    uint32_t last_local_change_ms;
    uint32_t last_publish_ms;
    bool publish_pending;
  };

  /** 進行中の接触。⚠️ **描画タスクの持ち物。**
   *
   * ⚠️ 0.1.0 は「触れている」ことだけを見て、押した瞬間に動作を撃っていた。
   * 押下と離上を**別々の出来事**として扱いはじめた時点で、その間に起きることが
   * 全部意味を持つようになる——**画面が変わる／選択が動く／通信がこける**。 */
  struct TouchGesture {
    bool down;
    /** 押し**始めた**位置が中央円の中だったか。⚠️ 離した位置では判定しない。 */
    bool in_center;
    /** 動作を撃った、または取り消した。⚠️ **`stuck_reported` と分ける**——
     * 1つにまとめると、長押しを撃った指を置いたままにしたとき、
     * 見限りの警告が出ないか、毎周期出続けるかのどちらかにしかならない。 */
    bool action_fired;
    bool stuck_reported;
    uint32_t press_ms;
    /** 押し**始めた**画面。⚠️ 離すまでに変わっていたら、その接触は無かったことにする。 */
    Screen screen;
    /** 押し**始めた**ときの対象スロット。
     * ⚠️ ランチャーで押しながらノブを回しても、開くのは**押した方**。 */
    int slot;
  };

  static void ui_task_trampoline(void *arg);
  void ui_task_();
  /** 生の入力を、**いまの画面の意味**へ翻訳して適用する。⚠️ 描画タスクからのみ呼ぶ。 */
  void apply_input_(Input in, uint32_t now);
  /** 画面を変える**唯一の口**。⚠️ `screen_` へ直接代入しないこと——
   * ここで進行中の接触を取り消している。**規則を守るのではなく、破る道を塞ぐ**形にしてある
   * （0.1.0 で入力の解釈を1箇所へ寄せたのと同じ手）。 */
  void set_screen_(Screen next);
  /** 進行中の接触を「この接触ではもう何もしない」印付きにする。
   * ⚠️ 指はまだ触れているので `down` は倒さない——倒すと、そのまま押し続けている指が
   * **新しい押下として数え直される**。 */
  void cancel_touch_(const char *why);
  /** タッチの状態機械。⚠️ 描画タスクから毎周期1回だけ。 */
  void poll_touch_(uint32_t now);
  /** 接触が短押しとして確定したとき（**離した瞬間**）。 */
  void on_touch_short_(uint32_t now);
  /** 接触が長押しの閾値を跨いだとき（**まだ指は触れている**）。 */
  void on_touch_long_(uint32_t now);
  /** スロット `index` のアプリを開く。⚠️ **種別で分岐する**——
   * ここに `LIGHT` を直書きしないこと。直書きすると
   * 「0番に別種別を入れても時計からはライトが開く」を踏む。
   * @return 開けたら true。未対応の種別なら false（呼び元はランチャーに留まる）。 */
  bool open_app_(int index, uint32_t now);
  /** 手応えを鳴らす意図を積む。⚠️ **描画タスクから直接 output を触らない**——
   * ESPHomeのコンポーネントはメインループの持ち物。積むだけにする。 */
  void beep_(uint32_t hz, uint32_t ms = BEEP_MS);
  /** 開いているアプリへ入力を渡す。⚠️ **種別で分岐するのはここ1箇所。** */
  void app_input_(Input in, uint32_t now);
  /** 調光画面での入力。⚠️ 描画タスクからのみ。 */
  void light_app_input_(Input in, uint32_t now);
  /** カーテンでの入力。⚠️ 描画タスクからのみ。 */
  void cover_app_input_(Input in, uint32_t now);
  /** ⚠️ **静定してから最終位置だけ**送る。 */
  void cover_app_publish_(uint32_t now);
  /** `cover.open_cover` などを積む。 */
  void cover_action_(const char *service, uint32_t now);
  /** `generic` のジェスチャを撃つ。⚠️ **設定されていなければ何もせず、鳴らさない。**
   * ⚠️ **回転でも案内を光らせる**——語が明るくなるだけなら回し続ける邪魔にならない
   * （「結果画面を挟むと回せない」という理由には当たらない）。 */
  void fire_gesture_(Gesture g, uint32_t now);
  /** 間引きつきの送信。⚠️ 積むだけで、呼ぶのはメインループ。 */
  void light_app_publish_(uint32_t now, bool force);
  void on_ha_state_(std::string entity_id, std::string state);
  /** ⚠️ **属性は素の値の文字列で来る**（`"255"`）。JSONではないので解析しない。
   * ⚠️ 消灯するとHAは `brightness` 属性ごと落とし、**そのとき何も送ってこない**
   * （`manager.py:435` が早期return）。よってここは「最後に届いた明るさ」を保つ。 */
  void on_ha_brightness_(std::string entity_id, std::string value);
  void on_ha_color_temp_(std::string entity_id, std::string value);
  void on_ha_ct_min_(std::string entity_id, std::string value);
  void on_ha_ct_max_(std::string entity_id, std::string value);
  /** ⚠️ 値は Python の repr の文字列（`"['color_temp']"`）。JSONではない。 */
  void on_ha_color_modes_(std::string entity_id, std::string value);
  void on_ha_cover_position_(std::string entity_id, std::string value);
  void on_ha_cover_features_(std::string entity_id, std::string value);
  void on_ha_climate_target_(std::string entity_id, std::string value);
  void on_ha_climate_current_(std::string entity_id, std::string value);
  void on_ha_climate_min_(std::string entity_id, std::string value);
  void on_ha_climate_max_(std::string entity_id, std::string value);
  void on_ha_climate_step_(std::string entity_id, std::string value);
  /** ⚠️ 値は Python の repr。**部分一致で拾えない**——`heat` は `heat_cool` の部分文字列。
   * 引用符ごと突き合わせる。詳細は実装のコメント。 */
  void on_ha_climate_modes_(std::string entity_id, std::string value);
  /** ⚠️ **同じ entity を複数のスロットに書ける**ので、一致した**全部**へ配る。
   * 最初の一致で打ち切ると、2つ目以降へ状態が永久に届かない
   * （同じカーテンを開き方違いで並べて見比べる、という使い方は普通にある）。 */
  template<typename F> void for_each_slot_(const std::string &entity_id, F fn);
  /** スロット `slot` で**いま**色温度をいじれるか。いじれるなら範囲も返す。
   * ⚠️ **毎回 atomic から読む**（アプリを開いたときの写しを使わない）——
   * 開いている最中に電球が替わっても追随できるようにするため。 */
  bool light_ct_available_(int slot, int *min_k, int *max_k) const;
  /** HAから届いている色温度（範囲と値）を、調光画面の `HaRange` へ写す。
   * ⚠️ **`set_reported` は「利用者が選んだ」印を倒す**ので、
   * 回している最中に呼ばないこと（呼び出し側で `LOCAL_CHANGE_GUARD_MS` を見る）。 */
  void light_ct_sync_(int slot);
  /** HAから届いている設定温度（範囲と値）と運転モードを、冷暖房画面へ写す。
   * ⚠️ `light_ct_sync_` と同じく、**回している最中に呼ばない**。 */
  void climate_sync_(int slot);
  /** ⚠️ **初回だけ**、カーソルをHAが報告しているモードへ寄せる。 */
  void climate_cursor_sync_(int slot);
  /** ⚠️ **機器がそのモードを持っているか。** 届く前は「持っている」と答える——
   * 起動直後に赤字を出さないため。 */
  bool climate_mode_supported_(int slot, uint8_t mode) const;
  /** ⚠️ **いま画面が「機器の持たないモード」を指しているか。**
   * 指している間は**長押し以外を受け付けない**（2026-08-11 11:21 ゆの）。
   * ⚠️ ノブの**ボタン（戻る）だけは通す**——止めるとこの画面から出られなくなる。 */
  bool climate_on_unsupported_mode_() const;
  void climate_app_input_(Input in, uint32_t now);
  /** 長押しで `modes:` の並びを1つ進める。⚠️ **非対応でも並びから外さない**（B'）。 */
  void climate_rotate_mode_(uint32_t now);
  void climate_app_publish_(uint32_t now);

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
  /** HAから届いた色温度（K）を**そのまま**。`-1` ＝ 値が無い（`None` など）。
   * ⚠️ **範囲での検査は使う側で行う**——属性は別々に届くので、
   * 値が先に来て範囲が後から来ることがある。
   * ⚠️ **`int32_t` なのは、範囲外の大きな値を落とさずに持つため**。
   * `int16_t` だと 65280 のような実在する報告値が溢れて別の数に化ける。 */
  std::atomic<int32_t> color_temp_[SLOTS_MAX];
  std::atomic<int16_t> ct_min_[SLOTS_MAX];
  std::atomic<int16_t> ct_max_[SLOTS_MAX];
  /** ⚠️ **いま色温度に対応しているか。** `supported_color_modes` が届くたび作り直す——
   * 一度立てたら降ろさない旗にすると、**電球を替えたときに嘘をつく**
   * （ビルド時に焼き込む案を却下したのと同じ理由が、実行時にもそのまま当てはまる）。 */
  std::atomic<bool> ct_capable_[SLOTS_MAX];

  /* ── カーテン ── */
  /** 現在位置（%）。`-1` ＝ まだ届いていない。 */
  std::atomic<int16_t> cover_position_[SLOTS_MAX];
  std::atomic<uint8_t> cover_state_[SLOTS_MAX];
  /** ⚠️ **できることのビット。** 位置指定を持たない機器でノブを効かせない。 */
  std::atomic<uint32_t> cover_features_[SLOTS_MAX];

  /* ── 冷暖房 ── */
  /** ⚠️ **温度だけは `-1` を「無い」に使えない。** 明るさ・色温度・カーテンの位置と違い、
   * **設定温度は負にも小さい値にもなる**（実機で `min_temp: 7` を観測。氷点下を扱う機器もある）。
   * よって**無いことは NaN で表す**——`std::isnan` でしか真にならないので、
   * 正当な温度と衝突しない。 */
  static_assert(std::atomic<float>::is_always_lock_free,
                "temperature atomics must be lock-free: they cross the API/UI task boundary");
  std::atomic<float> climate_target_[SLOTS_MAX];
  std::atomic<float> climate_current_[SLOTS_MAX];
  std::atomic<float> climate_min_[SLOTS_MAX];
  std::atomic<float> climate_max_[SLOTS_MAX];
  /** 刻み。⚠️ **NaN なら 0.5 を使う**（Y4）。実機では 1 の機器と 0.5 の機器が混在していた。 */
  std::atomic<float> climate_step_[SLOTS_MAX];
  /** いまの運転モード（`HvacMode`）。 */
  std::atomic<uint8_t> climate_mode_[SLOTS_MAX];
  /** ⚠️ **機器が対応しているモードのビット**（`1 << HvacMode`）。
   * ⚠️ **文字列をコアの境界を越えて渡さない**ため、購読側でビットに畳んでから置く。 */
  std::atomic<uint16_t> climate_modes_[SLOTS_MAX];

  /* タッチ計器の写し。**描画タスクが書き、メインループが読む。**
   * ⚠️ 生の値はタッチドライバの中にあるが、そこは描画タスクの持ち物なので直接読ませない。 */
  std::atomic<int16_t> g_ctrl_last_{-1};
  std::atomic<int16_t> g_ctrl_boot_{-1};
  std::atomic<uint16_t> g_ctrl_bad_{0};
  std::atomic<uint32_t> g_ctrl_polls_{0};
  std::atomic<uint16_t> tp_read_fail_{0};
  /** ⚠️ **APIが繋がっているか。** メインループが書き、描画タスクが読む。
   * `generic` は状態を持たないアプリなので、出せるのは「撃った」ことだけ——
   * ⚠️ **`call_homeassistant_service` は撃ちっぱなしで結果が返らない**ので、
   * 「効いた」とは言えない。言えるのは「送った」と「そもそも繋がっていない」の2つだけ。 */
  std::atomic<bool> api_connected_{false};
  /** ⚠️ **キューへ積めずに捨てた回数。** `light` なら1目盛り落ちても見えないが、
   * `generic` は **1目盛り＝1回の呼び出し**なので、落ちた分だけ実行されない。
   * **黙って落とさない**ために数える。 */
  std::atomic<uint16_t> dropped_actions_{0};

  /** 描画タスク → メインループ（Home Assistant を呼ぶ意図）。 */
  QueueHandle_t action_queue_{nullptr};
  /** メインループ → 描画タスク（**生の入力**。上の `Input` の注意を読むこと）。 */
  QueueHandle_t input_queue_{nullptr};
  TaskHandle_t ui_task_handle_{nullptr};
  ledc::LEDCOutput *buzzer_{nullptr};
  /** 鳴らし終わる時刻。`0` ＝ 鳴っていない。⚠️ **メインループの持ち物。** */
  uint32_t beep_until_ms_{0};

  /* ⚠️ ここから下は**描画タスクの持ち物**。他のタスクから読まないこと（atomicにしていない）。 */
  Screen screen_{Screen::LAUNCHER};
  TouchGesture touch_{};
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
  GenericAppState generic_app_{};
  CoverAppState cover_app_{};
  ClimateAppState climate_app_{};
};

}  // namespace astrolabe_ui
}  // namespace esphome
