#include "astrolabe_ui.h"

#include "driver/gpio.h"
/* ⚠️ `millis()` は `esphome/core/hal.h` の `esphome::millis()`。
   これを入れないと LovyanGFX 側の `lgfx::v1::millis` を勧められて落ちる。 */
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include "clock_render.hpp"
#include "light_render.hpp"

namespace esphome {
namespace astrolabe_ui {

static const char *const TAG = "astrolabe_ui";

/* ESPHome の汎用S3ボード定義は M5Dial を知らないので、
   ここで明示しないと電源保持が効かない。 */
static constexpr gpio_num_t PIN_PWR_HOLDING = GPIO_NUM_46;

/** タッチの連打よけ。⚠️ FT3267 は押している間ずっと点を返すので、
    これが無いと1タップで何十回もトグルする。 */
static constexpr uint32_t TOUCH_DEBOUNCE_MS = 400;

void AstrolabeUI::add_slot(uint8_t type, const std::string &entity_id, const std::string &tag_up,
                           const std::string &tag_down, int x, int y, const uint16_t *icon) {
  if (this->slots_.size() >= SLOTS_MAX) {
    /* Python側で弾いているので通常ここへは来ない。来たら黙って捨てず記録する。 */
    ESP_LOGE(TAG, "slot overflow (max %d); dropping %s", SLOTS_MAX, entity_id.c_str());
    return;
  }
  this->slots_.push_back(SlotConfig{static_cast<SlotType>(type), entity_id, x, y});
  this->render_slots_.push_back(RenderSlot{tag_up, tag_down, icon});
}

void AstrolabeUI::setup() {
  /* 電源保持を最初に立てる。 */
  gpio_reset_pin(PIN_PWR_HOLDING);
  gpio_set_direction(PIN_PWR_HOLDING, GPIO_MODE_OUTPUT);
  gpio_set_pull_mode(PIN_PWR_HOLDING, GPIO_PULLUP_PULLDOWN);
  gpio_set_level(PIN_PWR_HOLDING, 1);

  for (auto &s : this->states_) {
    s.store(static_cast<uint8_t>(SlotState::UNKNOWN));
  }
  for (auto &b : this->brightness_) {
    b.store(-1);  /* ⚠️ 0ではない。「まだ知らない」を0%と混ぜない。 */
  }

  ESP_LOGI(TAG, "display init");
  this->display_.init();
  /* ⚠️ ここで写しておく。以後 `display_` に触れてよいのは描画タスクだけ。 */
  this->panel_w_ = this->display_.width();
  this->panel_h_ = this->display_.height();

  /* ⚠️ **タッチはLCDの直後**。リセットピンを共有しているので順序が意味を持つ
     （リセットピンの共有はM5Dialの回路の都合）。
     この順を保つために本コンポーネントは `setup_priority::HARDWARE`。

     ⚠️⚠️ **I2Cは `lgfx::i2c` を使う。旧 `driver/i2c.h` へ戻すとブートループする。**
     LovyanGFX 1.2.26 が ESP-IDF 5.5 の**新**I2Cドライバを掴んでおり、旧ドライバの
     シンボルが**リンクされるだけで**IDFが起動時にabortする（呼び出しを `#if 0` で
     消しても直らない。実機で踏んで確認済み）。
     ⚠️ ESPHomeの `touchscreen:` は `DEPENDENCIES = ["display"]` で使えない。 */
  lgfx::i2c::init(0, HAL_PIN_TP_I2C_SDA, HAL_PIN_TP_I2C_SCL);
  this->tp_.init();

  /* 暗いまま作って、絵ができてから明るくする。 */
  this->display_.setBrightness(0);

  /* canvas: 240x240x16bit = 112.5KB。**このボードにPSRAMは無い**ので内部RAMから取る。 */
  this->canvas_ = new LGFX_Sprite(&this->display_);  // NOLINT
  this->canvas_->createSprite(this->display_.width(), this->display_.height());

  this->menu_ = new SMOOTH_MENU::Simple_Menu;  // NOLINT
  this->render_ = new LauncherRender;          // NOLINT
  this->render_->set_canvas(this->canvas_);
  this->render_->set_slots(&this->render_slots_);

  this->menu_->init(240, 240);
  this->menu_->setRenderCallback(this->render_);
  this->menu_->setMenuLoopMode(true);

  /* overshoot（行き過ぎて戻る動き）が Astrolabe の見た目の正体。 */
  auto cfg = this->menu_->getSelector()->config();
  cfg.animPath_x = LVGL::overshoot;
  cfg.animPath_y = LVGL::overshoot;
  cfg.animTime_x = 300;
  cfg.animTime_y = 300;
  this->menu_->getSelector()->config(cfg);

  /* 座標は codegen（ring.py）が計算済み。**ここで再計算しない**——
     Python側の検証（重なり判定）と食い違う余地を作らないため。 */
  for (auto &s : this->slots_) {
    this->menu_->getMenu()->addItem("", s.x, s.y, 22, 22);
  }
  this->menu_->getSelector()->goToItem(0);

  /* ⚠️ 要素の型を取り違えないこと。**行き先が違えば型も違う**——
     `action_queue_` は Home Assistant を呼ぶ意図、`input_queue_` は生の入力。 */
  this->action_queue_ = xQueueCreate(8, sizeof(Action));
  this->input_queue_ = xQueueCreate(8, sizeof(Input));

  /* ⚠️ **購読は setup() で登録する。** ESPHomeのAPIは購読リストを読むカーソルが
     接続時に一度走って止まるので、後から足しても Home Assistant へ告げられない
     （`api_connection.cpp` の `state_subs_at_ = -1`）。
     ここで全部登録しておけば接続時にまとめて告げられる。 */
  for (auto &s : this->slots_) {
    this->subscribe_homeassistant_state(&AstrolabeUI::on_ha_state_, s.entity_id);
    ESP_LOGCONFIG(TAG, "  subscribe %s", s.entity_id.c_str());
    if (s.type == SlotType::LIGHT) {
      /* ⚠️ **属性ごとに別のメンバ関数が要る。** 2引数版が渡してくれるのは
         `entity_id` だけで、**どの属性の値なのかは来ない**——
         1つの関数に集約すると state と brightness を区別できない。 */
      this->subscribe_homeassistant_state(&AstrolabeUI::on_ha_brightness_, s.entity_id, "brightness");
      ESP_LOGCONFIG(TAG, "  subscribe %s.brightness", s.entity_id.c_str());
    }
  }

  this->menu_->update(millis());
  this->canvas_->pushSprite(0, 0);
  this->display_.setBrightness(128);

  xTaskCreatePinnedToCore(&AstrolabeUI::ui_task_trampoline, "astrolabe_ui", UI_TASK_STACK, this, UI_TASK_PRIORITY,
                          &this->ui_task_handle_, 1);
  ESP_LOGI(TAG, "ready: %d slots, ui task on core 1", static_cast<int>(this->slots_.size()));
}

void AstrolabeUI::on_ha_state_(std::string entity_id, std::string state) {
  /* ⚠️ **メインループ（core 0）から呼ばれる。** ここで触ってよいのは atomic だけ。 */
  for (size_t i = 0; i < this->slots_.size(); i++) {
    if (this->slots_[i].entity_id != entity_id) {
      continue;
    }
    SlotState v;
    if (state == "on") {
      v = SlotState::ON;
    } else if (state == "off") {
      v = SlotState::OFF;
    } else {
      /* ⚠️ `unavailable` / `unknown` を **off に丸めない**。
         「消えている」と「届いていない」は利用者にとって別の意味。 */
      v = SlotState::UNKNOWN;
    }
    this->states_[i].store(static_cast<uint8_t>(v));
    ESP_LOGD(TAG, "%s -> %s", entity_id.c_str(), state.c_str());
    return;
  }
}

void AstrolabeUI::on_ha_brightness_(std::string entity_id, std::string value) {
  /* ⚠️ **メインループ（core 0）から呼ばれる。** atomic だけ触る。
     ⚠️ 値は**素の文字列**（`"255"`）。JSONではない。 */
  for (size_t i = 0; i < this->slots_.size(); i++) {
    if (this->slots_[i].entity_id != entity_id) {
      continue;
    }
    /* 消灯中は属性そのものが無く、HAは**何も送ってこない**（`manager.py:435`）。
       ここへ来るのは値がある場合だけだが、`unknown` 等が混じっても
       0 に倒さず**前の値を保つ**（`-1` のままにもしない）。 */
    char *end = nullptr;
    const long v = strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || v < 0 || v > 255) {
      ESP_LOGW(TAG, "%s.brightness: unparsable '%s'; keeping previous", entity_id.c_str(), value.c_str());
      return;
    }
    this->brightness_[i].store(static_cast<int16_t>(v));
    ESP_LOGD(TAG, "%s.brightness -> %ld", entity_id.c_str(), v);
    return;
  }
}

void AstrolabeUI::loop() {
  /* 鳴らし終わりの後始末。⚠️ **キューの処理より先**——
     取り出しで時間を食っても、鳴り終わりが延びないようにする。 */
  if (this->beep_until_ms_ != 0 && millis() >= this->beep_until_ms_) {
    this->beep_until_ms_ = 0;
    if (this->buzzer_ != nullptr) {
      this->buzzer_->set_level(0.0f);
    }
  }

  /* ⚠️ 描画はしない（専用タスクが持っている）。ここでやるのは
     **描画タスクが積んだ意図を Home Assistant へ渡すこと**だけ。
     ⚠️ **どのスロットかは描画タスクが決めて渡してくる。** ここで `getTargetItem()` を
     読み直さないこと——アプリを開いている間に選択が動く余地を作らない。 */
  Action action;
  while (this->action_queue_ != nullptr && xQueueReceive(this->action_queue_, &action, 0) == pdTRUE) {
    if (action.kind != ActionKind::BEEP && action.slot >= this->slots_.size()) {
      continue;
    }
    const std::string &entity = this->slots_[action.slot].entity_id;

    switch (action.kind) {
      case ActionKind::TOGGLE_SLOT:
        ESP_LOGI(TAG, "toggle %s", entity.c_str());
        this->call_homeassistant_service("light.toggle", {{"entity_id", entity}});
        break;

      case ActionKind::BEEP:
        /* ⚠️ ここで待たない。**鳴らし始めるだけ**で、止めるのは下の後始末。
           メインループで20ms待つと、その間HAの処理が全部止まる。 */
        if (this->buzzer_ != nullptr) {
          this->buzzer_->update_frequency(static_cast<float>(action.hz));
          this->buzzer_->set_level(BEEP_LEVEL);
          this->beep_until_ms_ = millis() + BEEP_MS;
        }
        break;

      case ActionKind::SET_LIGHT:
        if (action.is_on) {
          /* ⚠️ 数値も文字列で渡す（HAが型を寄せる）。ここでJSONを組み立てない。 */
          ESP_LOGD(TAG, "set %s brightness=%d", entity.c_str(), action.brightness);
          this->call_homeassistant_service(
              "light.turn_on", {{"entity_id", entity}, {"brightness", to_string(action.brightness)}});
        } else {
          ESP_LOGD(TAG, "set %s off", entity.c_str());
          this->call_homeassistant_service("light.turn_off", {{"entity_id", entity}});
        }
        break;
    }
  }
}

/* ⚠️ この3つは**メインループから呼ばれる**。ここでは画面のことを一切知らない——
   生の入力を積むだけで、意味は描画タスクが決める（`apply_input_`）。
   名前が `go_next` のままなのはYAML側のラムダとの互換のため。中身は「時計回りが1つ来た」。 */
void AstrolabeUI::go_next() {
  const Input i = Input::ROTATE_CW;
  xQueueSend(this->input_queue_, &i, 0);
}

void AstrolabeUI::go_last() {
  const Input i = Input::ROTATE_CCW;
  xQueueSend(this->input_queue_, &i, 0);
}

void AstrolabeUI::on_button_pressed() {
  const Input i = Input::BUTTON;
  xQueueSend(this->input_queue_, &i, 0);
}

bool AstrolabeUI::open_app_(int index, uint32_t now) {
  if (index < 0 || index >= static_cast<int>(this->slots_.size())) {
    ESP_LOGW(TAG, "open_app: no slot at %d", index);
    return false;
  }

  /* ⚠️ **種別で分岐する。** ここに `LIGHT` を直書きしてはいけない——
     アイドルからの起床先に種別を直書きすると、
     「0番に別種別を入れても時計からはライトが開く」を踏む。
     0.1.0 は `light` しか無いので結果は同じだが、**書き方を間違えると0.2.0で再現する。** */
  switch (this->slots_[index].type) {
    case SlotType::LIGHT: {
      const int16_t known = this->brightness_[index].load();
      const auto st = static_cast<SlotState>(this->states_[index].load());
      this->light_app_ = LightAppState{};
      /* ⚠️ 開いた時点でHAの値を写し取る。**まだ届いていない（-1）なら 0% と混ぜない。** */
      this->light_app_.state_received = (st != SlotState::UNKNOWN);
      this->light_app_.is_on = (st == SlotState::ON);
      this->light_app_.brightness = (known >= 0) ? known : 0;
      this->app_slot_ = index;
      this->screen_ = Screen::APP;
      this->last_activity_ms_ = now;
      ESP_LOGI(TAG, "open app: slot %d (%s) br=%d on=%d", index, this->slots_[index].entity_id.c_str(),
               this->light_app_.brightness, static_cast<int>(this->light_app_.is_on));
      return true;
    }
  }
  /* ⚠️ 未対応の種別は**黙って落とさない**。開けなかったと言い、ランチャーに留まる。 */
  ESP_LOGW(TAG, "open_app: slot %d has no app for its type", index);
  return false;
}

void AstrolabeUI::light_app_input_(Input in, uint32_t now) {
  if (in == Input::ROTATE_CW || in == Input::ROTATE_CCW) {
    /* ピンを入れ替えてあるので、素直に CW で明るく。 */
    this->light_app_.brightness += (in == Input::ROTATE_CW) ? BRIGHTNESS_STEP : -BRIGHTNESS_STEP;
    if (this->light_app_.brightness > 255) {
      this->light_app_.brightness = 255;
    }
    if (this->light_app_.brightness < 0) {
      this->light_app_.brightness = 0;
    }
    /* 消えている状態で明るくしたら点ける。 */
    if (!this->light_app_.is_on && this->light_app_.brightness > 0) {
      this->light_app_.is_on = true;
    }
    this->light_app_.last_local_change_ms = now;
    this->light_app_.publish_pending = true;
    ESP_LOGD(TAG, "light: br=%d", this->light_app_.brightness);
  }
}

void AstrolabeUI::light_app_publish_(uint32_t now, bool force) {
  if (!force && !this->light_app_.publish_pending) {
    return;
  }
  /* ⚠️ **間引く。** ノブ1目盛りごとに投げるとHAを叩き続けることになる。 */
  if (!force && now - this->light_app_.last_publish_ms < PUBLISH_INTERVAL_MS) {
    return;
  }
  this->light_app_.last_publish_ms = now;
  this->light_app_.publish_pending = false;

  const Action a{ActionKind::SET_LIGHT, static_cast<uint8_t>(this->app_slot_),
                 static_cast<int16_t>(this->light_app_.brightness), this->light_app_.is_on};
  xQueueSend(this->action_queue_, &a, 0);
}

void AstrolabeUI::beep_(uint32_t hz) {
  const Action a{ActionKind::BEEP, static_cast<uint16_t>(hz), 0, 0, false};
  xQueueSend(this->action_queue_, &a, 0);
}

void AstrolabeUI::apply_input_(Input in, uint32_t now) {
  /* ⚠️ **手応えは画面によらず、入力を受けた時点で鳴らす。**
     旧実装はハードウェアのコールバックで鳴らしていたので、
     ランチャーでもアプリでも時計でも同じように鳴っていた。
     画面ごとの分岐に入れると**必ずどれかを書き忘れる**——実際、
     ランチャーからアプリを開くときだけ無音になっていた。 */
  switch (in) {
    case Input::ROTATE_CW:
      this->beep_(BEEP_HZ_ROTATE_CW);
      break;
    case Input::ROTATE_CCW:
      this->beep_(BEEP_HZ_ROTATE_CCW);
      break;
    case Input::BUTTON:
      this->beep_(BEEP_HZ_PRESS);
      break;
  }

  /* ⚠️ **入力の意味を決めるのはここ1箇所だけ。** 画面を知っているのは描画タスクだけなので、
     「時計で回した分がランチャーの選択になる」経路が構造的に存在しない（B4④）。 */
  switch (this->screen_) {
    case Screen::CLOCK:
      if (in == Input::BUTTON) {
        ESP_LOGI(TAG, "clock -> launcher (button)");
        this->screen_ = Screen::LAUNCHER;
        /* ⚠️ ランチャーへ戻った瞬間から数え直す。ここを忘れると、時計に居た時間が
           そのまま無操作時間として残り、**戻った直後にまた時計へ落ちる**。 */
        this->last_activity_ms_ = now;
      } else {
        /* 起床先は**先頭スロット**。⚠️ 開けなければランチャーへ落とす。 */
        if (!this->open_app_(0, now)) {
          this->screen_ = Screen::LAUNCHER;
          this->last_activity_ms_ = now;
        }
      }
      return;

    case Screen::LAUNCHER:
      this->last_activity_ms_ = now;
      if (in == Input::ROTATE_CW || in == Input::ROTATE_CCW) {
        /* ⚠️ **無音にしない。** 検証時、ランチャーでの回転が
           ログに出ないせいで**アイドル間隔をログだけで検算できなかった**
           （30.18秒に見えたのは、間に無音の回転が入って数え直されたため）。
           長期soak（A2）は**ログを後から読む**試験なので、
           **タイマを触る操作が痕跡を残さないのは計器の穴**。 */
        ESP_LOGD(TAG, "launcher: rotate %s (activity)", in == Input::ROTATE_CW ? "cw" : "ccw");
        if (in == Input::ROTATE_CW) {
          this->menu_->goNext();
        } else {
          this->menu_->goLast();
        }
      } else {
        /* ランチャーのボタンは**選択中のアプリを開く**。 */
        this->open_app_(static_cast<int>(this->menu_->getSelector()->getTargetItem()), now);
      }
      return;

    case Screen::APP:
      this->last_activity_ms_ = now;
      if (in == Input::BUTTON) {
        /* ボタンは戻る。
           ⚠️ **戻る前に溜まっている変更を出し切る**——間引きの待ち時間中に
           閉じると、最後にひねったぶんが**投げられずに消える**。 */
        this->light_app_publish_(now, /*force=*/this->light_app_.publish_pending);
        ESP_LOGI(TAG, "app -> launcher (button)");
        this->screen_ = Screen::LAUNCHER;
        this->app_slot_ = -1;
      } else {
        this->light_app_input_(in, now);
      }
      return;
  }
}

void AstrolabeUI::ui_task_trampoline(void *arg) { static_cast<AstrolabeUI *>(arg)->ui_task_(); }

void AstrolabeUI::ui_task_() {
  /* ⚠️ **UIはESPHomeのloop()ではなく、この専用タスクで回す。**
     素朴に loop() で回した実測は Loop Time 23〜26ms で、
     canvasの押し出しだけで約9ms使っていた。ESPHomeのループ周期目標は16ms。 */
  const TickType_t period = pdMS_TO_TICKS(UI_PERIOD_MS);
  TickType_t last_wake = xTaskGetTickCount();

  this->last_activity_ms_ = millis();

  while (true) {
    const uint32_t now = millis();

    /* メインループから来た**生の入力**を、いまの画面の意味へ翻訳する。
       **menu_ と screen_ を触るのはこのタスクだけ。** */
    Input in;
    while (xQueueReceive(this->input_queue_, &in, 0) == pdTRUE) {
      this->apply_input_(in, now);
    }

    /* タッチ。⚠️ **`getTouchPointsNum()` 経由で読む**——この呼び出しが
       `_poll_g_ctrl()`（タッチ放置死の計器）も回している。読み方を変えると計器が止まる。
       ⚠️ **画面によらず毎周期読む。** 時計のときだけ読まないようにすると計器が止まる。 */
    const bool touched = this->tp_.getTouchPointsNum() > 0;

    /* ⚠️ **計器の写しを取るのはここ。** 上の `getTouchPointsNum()` が
       G_CTRL の見張りも回しているので、その直後が最新。
       メインループはこの atomic だけを読む（`touch_g_ctrl()` 他）。 */
    this->g_ctrl_last_.store(this->tp_.getGCtrlLast());
    this->g_ctrl_boot_.store(this->tp_.getGCtrlBoot());
    this->g_ctrl_bad_.store(this->tp_.getGCtrlBadCount());
    this->g_ctrl_polls_.store(static_cast<uint32_t>(this->tp_.getGCtrlPollCount()));
    if (touched && now - this->last_touch_ms_ > TOUCH_DEBOUNCE_MS) {
      this->last_touch_ms_ = now;
      this->tp_.update();
      const auto pt = this->tp_.getTouchPointBuffer();
      const int dx = pt.x - 120;
      const int dy = pt.y - 120;
      const bool in_center = (dx * dx + dy * dy) <= (CENTER_TAP_RADIUS * CENTER_TAP_RADIUS);

      switch (this->screen_) {
        case Screen::LAUNCHER:
          /* ⚠️ **中央円の中だけが「開く」。**
             全画面で拾うと**リング上のアイコンを触っただけで開く**。
             ⚠️ タップは**アプリを開く**。トグルはアプリの中。 */
          if (in_center) {
            this->last_activity_ms_ = now;
            /* ⚠️ **ボタンと同じ音**。利用者にとっては同じ「決定」なので、
               入口が違うだけで音が変わると混乱する。 */
            this->beep_(BEEP_HZ_PRESS);
            ESP_LOGD(TAG, "launcher: center tap (activity)");
            this->open_app_(static_cast<int>(this->menu_->getSelector()->getTargetItem()), now);
          }
          break;

        case Screen::CLOCK:
          /* 時計のタップも起床。⚠️ こちらは中央に限らない。 */
          this->beep_(BEEP_HZ_PRESS);
          if (!this->open_app_(0, now)) {
            this->screen_ = Screen::LAUNCHER;
            this->last_activity_ms_ = now;
          }
          break;

        case Screen::APP:
          /* アプリの中でのタップはトグル。 */
          this->last_activity_ms_ = now;
          this->light_app_.is_on = !this->light_app_.is_on;
          if (this->light_app_.is_on && this->light_app_.brightness == 0) {
            this->light_app_.brightness = 128;  /* 消灯から点けたときの既定 */
          }
          this->light_app_.last_local_change_ms = now;
          ESP_LOGI(TAG, "light: toggle -> %s", this->light_app_.is_on ? "on" : "off");
          this->beep_(BEEP_HZ_ACTION);
          /* ⚠️ トグルは間引かない。押した手応えが遅れるのが一番きらわれる。 */
          this->light_app_publish_(now, /*force=*/true);
          break;
      }
    }

    switch (this->screen_) {
      case Screen::LAUNCHER:
        /* ⚠️ 無操作の判定は**ランチャーにいるときだけ**。時計はアイドル画面そのもので、
           そこにタイムアウトを持たせない（`astrolabe_ui.h` の `last_activity_ms_` 参照）。 */
        if (now - this->last_activity_ms_ >= IDLE_TIMEOUT_MS) {
          ESP_LOGI(TAG, "launcher -> clock (idle %ums)", static_cast<unsigned>(IDLE_TIMEOUT_MS));
          this->screen_ = Screen::CLOCK;
          /* 次の周期で必ず描くように、前回描画時刻を過去へ倒す。 */
          this->last_clock_render_ms_ = now - CLOCK_RENDER_MS;
          break;
        }
        /* ⚠️ `menu_->update()` は**アニメーションを進める**ので毎周期呼ぶ。
           時計にいる間は呼ばない——止めておけば、戻ったとき同じ盤面から再開する。 */
        this->menu_->update(now);
        this->canvas_->pushSprite(0, 0);
        break;

      case Screen::CLOCK:
        /* 分しか出ないので30fpsで描き直す意味がない。 */
        if (now - this->last_clock_render_ms_ >= CLOCK_RENDER_MS) {
          this->last_clock_render_ms_ = now;
          render_clock(this->canvas_);
          this->canvas_->pushSprite(0, 0);
        }
        break;

      case Screen::APP:
        /* アプリも無操作で時計へ。⚠️ ランチャーではなく**時計へ**戻る。 */
        if (now - this->last_activity_ms_ >= IDLE_TIMEOUT_MS) {
          this->light_app_publish_(now, /*force=*/this->light_app_.publish_pending);
          ESP_LOGI(TAG, "app -> clock (idle)");
          this->screen_ = Screen::CLOCK;
          this->app_slot_ = -1;
          this->last_clock_render_ms_ = now - CLOCK_RENDER_MS;
          break;
        }

        /* HAからのこだまを取り込む。⚠️ **ローカル操作の直後は無視する**——
           これが無いと、回している最中に古い値が返ってきて**ノブと綱引きになる**
           2秒。 */
        if (this->app_slot_ >= 0 && now - this->light_app_.last_local_change_ms >= LOCAL_CHANGE_GUARD_MS) {
          const auto st = static_cast<SlotState>(this->states_[this->app_slot_].load());
          if (st != SlotState::UNKNOWN) {
            this->light_app_.state_received = true;
            this->light_app_.is_on = (st == SlotState::ON);
          }
          const int16_t br = this->brightness_[this->app_slot_].load();
          if (br >= 0) {
            this->light_app_.brightness = br;
          }
        }

        this->light_app_publish_(now, /*force=*/false);
        render_light(this->canvas_, this->light_app_.brightness, this->light_app_.is_on,
                     this->light_app_.state_received);
        this->canvas_->pushSprite(0, 0);
        break;
    }

    /* ⚠️ **スタックの余裕を定期的に吐く。** 描画は呼び出しが深くなりがちで、
       溢れると**再起動という形でしか現れない**——長期稼働の記録から原因を辿れるようにする。 */
    if (now - this->last_stack_report_ms_ >= STACK_REPORT_MS) {
      this->last_stack_report_ms_ = now;
      ESP_LOGD(TAG, "ui task stack: %u bytes free of %d",
               static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)), UI_TASK_STACK);
    }

    vTaskDelayUntil(&last_wake, period);
  }
}

void AstrolabeUI::dump_config() {
  ESP_LOGCONFIG(TAG, "Astrolabe UI:");
  ESP_LOGCONFIG(TAG, "  panel: %dx%d", this->panel_w_, this->panel_h_);
  ESP_LOGCONFIG(TAG, "  slots: %d", static_cast<int>(this->slots_.size()));
  for (auto &s : this->slots_) {
    ESP_LOGCONFIG(TAG, "    - %s", s.entity_id.c_str());
  }
  ESP_LOGCONFIG(TAG, "  free heap: %u", static_cast<unsigned>(esp_get_free_heap_size()));
}

}  // namespace astrolabe_ui
}  // namespace esphome
