#include "astrolabe_ui.h"

#include "driver/gpio.h"
/* ⚠️ `millis()` は `esphome/core/hal.h` の `esphome::millis()`。
   これを入れないと LovyanGFX 側の `lgfx::v1::millis` を勧められて落ちる。 */
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include "clock_render.hpp"
#include "cover_render.hpp"
#include "generic_render.hpp"
#include "light_render.hpp"

namespace esphome {
namespace astrolabe_ui {

static const char *const TAG = "astrolabe_ui";

/* ESPHome の汎用S3ボード定義は M5Dial を知らないので、
   ここで明示しないと電源保持が効かない。 */
static constexpr gpio_num_t PIN_PWR_HOLDING = GPIO_NUM_46;

/* ⚠️ かつてここに 400ms の連打よけがあった。FT3267 は押している間ずっと点を返すので、
   「触れている」だけを見る作りではそれが要る。押下と離上を追うようになったので、
   **同じ役目はエッジ検出が果たす**——数え直す必要がなくなった。 */

void AstrolabeUI::add_slot(uint8_t type, const std::string &entity_id, const std::string &tag_up,
                           const std::string &tag_down, int x, int y, const uint16_t *icon) {
  if (this->slots_.size() >= SLOTS_MAX) {
    /* Python側で弾いているので通常ここへは来ない。来たら黙って捨てず記録する。 */
    ESP_LOGE(TAG, "slot overflow (max %d); dropping %s", SLOTS_MAX, entity_id.c_str());
    return;
  }
  /* ⚠️ **位置で並べない。** メンバを1つ挿されると値が1つずつずれ、
     コンパイラは（型が近ければ）警告しか出さない。実際に `Action` でそれを踏み、
     **調光の送信がまるごと捨てられている版を公開した**。名前で書けば起きない。 */
  SlotConfig cfg{};
  cfg.type = static_cast<SlotType>(type);
  cfg.entity_id = entity_id;
  cfg.x = x;
  cfg.y = y;
  this->slots_.push_back(std::move(cfg));

  RenderSlot rs{};
  rs.tag_up = tag_up;
  rs.tag_down = tag_down;
  rs.icon = icon;
  this->render_slots_.push_back(std::move(rs));
}

void AstrolabeUI::add_gesture(int slot, uint8_t gesture, const std::string &service,
                              const std::string &entity_id) {
  /* ⚠️ codegen が `add_slot` のあとに出すので、ここへ来る時点でスロットは存在する。
     それでも見るのは、**存在しない添字で書き込む方が、黙って効かないより悪い**から。 */
  if (slot < 0 || slot >= static_cast<int>(this->slots_.size()) ||
      gesture >= static_cast<uint8_t>(Gesture::COUNT)) {
    ESP_LOGE(TAG, "gesture %u for slot %d has nowhere to go", gesture, slot);
    return;
  }
  auto &target = this->slots_[slot].gestures[gesture];
  target.service = service;
  target.entity_id = entity_id;
}

void AstrolabeUI::set_cover_opening(int slot, uint8_t opening) {
  if (slot < 0 || slot >= static_cast<int>(this->slots_.size())) {
    ESP_LOGE(TAG, "cover opening for slot %d has nowhere to go", slot);
    return;
  }
  this->slots_[slot].opening = static_cast<CoverOpening>(opening);
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
  for (auto &c : this->color_temp_) {
    c.store(-1);  /* ⚠️ 「値が無い」。0 と混ぜない。 */
  }
  for (auto &c : this->ct_min_) {
    c.store(-1);
  }
  for (auto &c : this->ct_max_) {
    c.store(-1);
  }
  for (auto &c : this->cover_position_) {
    c.store(-1);
  }
  for (auto &c : this->cover_state_) {
    c.store(static_cast<uint8_t>(CoverState::UNKNOWN));
  }
  for (auto &c : this->cover_features_) {
    c.store(0);
  }
  for (auto &c : this->ct_capable_) {
    /* ⚠️ **既定は「対応していない」。** 届く前と非対応を同じ扱いにする——
       これが「HAに繋がってから初めて `DIM｜CLR` が出る」の実体。 */
    c.store(false);
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
    /* ⚠️ **`generic` は entity を持たない。** 空の entity を購読しにいかない。 */
    if (s.entity_id.empty()) {
      continue;
    }
    this->subscribe_homeassistant_state(&AstrolabeUI::on_ha_state_, s.entity_id);
    ESP_LOGCONFIG(TAG, "  subscribe %s", s.entity_id.c_str());
    if (s.type == SlotType::LIGHT) {
      /* ⚠️ **属性ごとに別のメンバ関数が要る。** 2引数版が渡してくれるのは
         `entity_id` だけで、**どの属性の値なのかは来ない**——
         1つの関数に集約すると state と brightness を区別できない。 */
      this->subscribe_homeassistant_state(&AstrolabeUI::on_ha_brightness_, s.entity_id, "brightness");
      /* ⚠️ **`supported_color_modes` が能力の現在値。** ライトなら必ず持っている属性なので、
         状態が変わるたび届く——電球を替えれば、その場で降りる。
         min/max は**範囲の供給だけ**に使う（対応していなければそもそも届かない）。 */
      this->subscribe_homeassistant_state(&AstrolabeUI::on_ha_color_modes_, s.entity_id,
                                          "supported_color_modes");
      this->subscribe_homeassistant_state(&AstrolabeUI::on_ha_color_temp_, s.entity_id, "color_temp_kelvin");
      this->subscribe_homeassistant_state(&AstrolabeUI::on_ha_ct_min_, s.entity_id, "min_color_temp_kelvin");
      this->subscribe_homeassistant_state(&AstrolabeUI::on_ha_ct_max_, s.entity_id, "max_color_temp_kelvin");
      ESP_LOGCONFIG(TAG, "  subscribe %s: brightness, color temp (4)", s.entity_id.c_str());
    } else if (s.type == SlotType::COVER) {
      this->subscribe_homeassistant_state(&AstrolabeUI::on_ha_cover_position_, s.entity_id, "current_position");
      /* ⚠️ **できることのビット。** 位置指定を持たない機器でノブを効かせないため。 */
      this->subscribe_homeassistant_state(&AstrolabeUI::on_ha_cover_features_, s.entity_id, "supported_features");
      ESP_LOGCONFIG(TAG, "  subscribe %s: position, features", s.entity_id.c_str());
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
    if (this->slots_[i].type == SlotType::COVER) {
      /* ⚠️ **カーテンの語彙は ON/OFF ではない。** ライトの解釈に混ぜない。 */
      CoverState cs = CoverState::UNKNOWN;
      if (state == "open") {
        cs = CoverState::OPEN;
      } else if (state == "closed") {
        cs = CoverState::CLOSED;
      } else if (state == "opening") {
        cs = CoverState::OPENING;
      } else if (state == "closing") {
        cs = CoverState::CLOSING;
      }
      this->cover_state_[i].store(static_cast<uint8_t>(cs));
      ESP_LOGD(TAG, "%s -> %s", entity_id.c_str(), state.c_str());
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
  }
}

/** ⚠️ **同じ entity を複数のスロットに書ける。**
 *
 * 最初の一致で打ち切ると、2つ目以降のスロットへ**状態が永久に届かない**——
 * 画面には出るのに値が来ない、という一番分かりにくい壊れ方になる
 * （同じカーテンを開き方違いで並べて見比べたい、という使い方は普通にある）。
 * だから**一致した全部へ配る**。
 */
template<typename F> void AstrolabeUI::for_each_slot_(const std::string &entity_id, F fn) {
  for (size_t i = 0; i < this->slots_.size(); i++) {
    if (this->slots_[i].entity_id == entity_id) {
      fn(static_cast<int>(i));
    }
  }
}

/** 属性の素の値をケルビンとして読む。
 *
 * ⚠️ **`long` で受けてから範囲へ落とす。** 先に `int16_t` へ落とすと、
 * 65280 のような値が溢れて**別の数に化ける**。
 * ⚠️ **おかしな値は丸めずに「知らない」を返す。** 実際に `color_temp_kelvin: 65280` を
 * 報告するライトがある。範囲の端へ丸めると、それを利用者が選んだ値として
 * 送り返すことになり、**点けた瞬間に選んでいない色になる**。
 *
 * @return 読めなければ `-1`
 */
static int32_t parse_kelvin_(const std::string &value) {
  char *end = nullptr;
  const long v = strtol(value.c_str(), &end, 10);
  /* ⚠️ **ここでは丸めない。** 範囲は別の属性で遅れて届くので、収まっているかの判断は
     使う側でやる。`None` のように数として読めないものだけを弾く。 */
  if (end == value.c_str() || v <= 0 || v > 1000000) {
    return -1;
  }
  return static_cast<int32_t>(v);
}

void AstrolabeUI::on_ha_color_temp_(std::string entity_id, std::string value) {
  const int32_t k = parse_kelvin_(value);
  this->for_each_slot_(entity_id, [&](int i) { this->color_temp_[i].store(k); });
  {
    if (k < 0) {
      ESP_LOGW(TAG, "%s.color_temp_kelvin: '%s' is not a number; treating as absent",
               entity_id.c_str(), value.c_str());
      return;
    }
    /* ⚠️ 範囲に収まっているかはここで見ない（範囲が遅れて届くため）。使う側で分ける。 */
    ESP_LOGD(TAG, "%s.color_temp_kelvin -> %d", entity_id.c_str(), static_cast<int>(k));
  }
}

void AstrolabeUI::on_ha_ct_min_(std::string entity_id, std::string value) {
  const int32_t k = parse_kelvin_(value);
  this->for_each_slot_(entity_id, [&](int i) { this->ct_min_[i].store(k); });
  ESP_LOGD(TAG, "%s.min_color_temp_kelvin -> %s", entity_id.c_str(), value.c_str());
}

void AstrolabeUI::on_ha_ct_max_(std::string entity_id, std::string value) {
  const int32_t k = parse_kelvin_(value);
  this->for_each_slot_(entity_id, [&](int i) { this->ct_max_[i].store(k); });
  ESP_LOGD(TAG, "%s.max_color_temp_kelvin -> %s", entity_id.c_str(), value.c_str());
}

void AstrolabeUI::on_ha_color_modes_(std::string entity_id, std::string value) {
  /* ⚠️ 値は Python の repr の文字列。**JSONではないので解析しない。**
     実機で観測した形は `[<ColorMode.COLOR_TEMP: 'color_temp'>]` で、
     素朴な `['color_temp']` ではない（enum が repr のまま文字列化されている）。
     色モードの語彙に `color_temp` を含む別の値は無いので、部分一致で足りる——
     **形が変わっても、名前が入っている限り効く**というのがここを解析しない理由。 */
  const bool capable = value.find("color_temp") != std::string::npos;
  this->for_each_slot_(entity_id, [&](int i) {
    if (this->ct_capable_[i].exchange(capable) != capable) {
      ESP_LOGI(TAG, "%s: color temp %s (%s)", entity_id.c_str(), capable ? "available" : "gone",
               value.c_str());
    }
  });
}

void AstrolabeUI::on_ha_cover_position_(std::string entity_id, std::string value) {
  float v = 0.0f;
  /* ⚠️ **位置を持たないカーテンもある**（開閉だけの機器）。その場合ここへは来ないが、
     `None` が来ることはある。**0に倒さない。** */
  const bool ok = parse_number(value, &v);
  if (!ok) {
    ESP_LOGW(TAG, "%s.current_position: '%s' is not a number; treating as absent", entity_id.c_str(),
             value.c_str());
  } else {
    ESP_LOGD(TAG, "%s.current_position -> %d", entity_id.c_str(), static_cast<int>(v));
  }
  this->for_each_slot_(entity_id, [&](int i) {
    this->cover_position_[i].store(ok ? static_cast<int16_t>(v) : -1);
  });
}

void AstrolabeUI::on_ha_cover_features_(std::string entity_id, std::string value) {
  float v = 0.0f;
  if (!parse_number(value, &v)) {
    return;
  }
  const uint32_t bits = static_cast<uint32_t>(v);
  this->for_each_slot_(entity_id, [&](int i) {
    if (this->cover_features_[i].exchange(bits) != bits) {
      ESP_LOGI(TAG, "%s: cover features %u (position=%s stop=%s)", entity_id.c_str(), bits,
               (bits & COVER_FEAT_SET_POSITION) ? "yes" : "no", (bits & COVER_FEAT_STOP) ? "yes" : "no");
    }
  });
}

bool AstrolabeUI::light_ct_available_(int slot, int *min_k, int *max_k) const {
  if (slot < 0 || slot >= static_cast<int>(this->slots_.size())) {
    return false;
  }
  if (!this->ct_capable_[slot].load()) {
    return false;
  }
  const int16_t lo = this->ct_min_[slot].load();
  const int16_t hi = this->ct_max_[slot].load();
  /* 範囲が無ければ刻めない。⚠️ 能力があると言われていても、**範囲が来るまでは断る**。 */
  if (lo < 0 || hi < 0 || hi <= lo) {
    return false;
  }
  if (min_k != nullptr) {
    *min_k = lo;
  }
  if (max_k != nullptr) {
    *max_k = hi;
  }
  return true;
}

void AstrolabeUI::light_ct_sync_(int slot) {
  int lo = 0;
  int hi = 0;
  if (!this->light_ct_available_(slot, &lo, &hi)) {
    /* ⚠️ **範囲が無ければ何もできない値に戻す。** 古い範囲を残すと、
       電球を色温度非対応のものへ替えたあとも刻めてしまう。 */
    this->light_app_.color_temp.clear_bounds();
    return;
  }
  this->light_app_.color_temp.set_bounds(static_cast<float>(lo), static_cast<float>(hi));
  const int32_t raw = this->color_temp_[slot].load();
  if (raw < 0) {
    /* 属性そのものが無い（消灯で落ちた・`None` が来た）。⚠️ 0K と混ぜない。 */
    this->light_app_.color_temp.clear_reported();
  } else {
    this->light_app_.color_temp.set_reported(static_cast<float>(raw));
  }
}

void AstrolabeUI::loop() {
  /* ⚠️ **APIの生死を描画タスクへ渡す。** 描画タスクからAPIを触らせない約束なので、
     ここで写す。`generic` の「送った / 繋がっていない」の判定に使う。 */
  this->api_connected_.store(api::global_api_server != nullptr &&
                             api::global_api_server->is_connected());

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
          this->beep_until_ms_ = millis() + action.ms;
        }
        break;

      case ActionKind::CALL_GESTURE: {
        /* ⚠️ **文字列はキューに載せない**（FreeRTOSのキューは中身をコピーするので
           `std::string` を載せると壊れる）。載せるのは添字だけで、**実体はここで引く**。 */
        if (action.gesture >= static_cast<uint8_t>(Gesture::COUNT)) {
          break;
        }
        const auto &target = this->slots_[action.slot].gestures[action.gesture];
        if (target.service.empty()) {
          break;
        }
        const auto dot = target.service.find('.');
        if (dot == std::string::npos) {
          ESP_LOGE(TAG, "gesture service '%s' has no domain", target.service.c_str());
          break;
        }
        ESP_LOGI(TAG, "gesture -> %s %s", target.service.c_str(), target.entity_id.c_str());
        this->call_homeassistant_service(target.service, {{"entity_id", target.entity_id}});
        break;
      }

      case ActionKind::COVER_ACTION:
        ESP_LOGI(TAG, "cover %s %s", action.cover_service, entity.c_str());
        this->call_homeassistant_service(std::string("cover.") + action.cover_service,
                                         {{"entity_id", entity}});
        break;

      case ActionKind::SET_COVER_POSITION:
        ESP_LOGD(TAG, "set %s position=%d", entity.c_str(), action.position);
        this->call_homeassistant_service("cover.set_cover_position",
                                         {{"entity_id", entity}, {"position", to_string(action.position)}});
        break;

      case ActionKind::SET_LIGHT:
        if (!action.is_on) {
          ESP_LOGD(TAG, "set %s off", entity.c_str());
          this->call_homeassistant_service("light.turn_off", {{"entity_id", entity}});
        } else if (action.color_temp >= 0) {
          /* ⚠️ **色温度を指定する手段は `light.turn_on` しかない。**
             だから色温度を送ることは、消えていれば点けることでもある。 */
          ESP_LOGD(TAG, "set %s ct=%d", entity.c_str(), action.color_temp);
          this->call_homeassistant_service(
              "light.turn_on",
              {{"entity_id", entity}, {"color_temp_kelvin", to_string(action.color_temp)}});
        } else {
          /* ⚠️ 数値も文字列で渡す（HAが型を寄せる）。ここでJSONを組み立てない。 */
          ESP_LOGD(TAG, "set %s brightness=%d", entity.c_str(), action.brightness);
          this->call_homeassistant_service(
              "light.turn_on", {{"entity_id", entity}, {"brightness", to_string(action.brightness)}});
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
      /* ⚠️ **開くときは必ず明るさから。** 色温度は長押しで行く場所であって、
         前回どちらにいたかを覚えていると「開いたら知らない画面だった」になる。 */
      this->light_app_.mode = LightMode::DIMMER;
      /* ⚠️ **範囲外は端へ丸めて「見せる」が、「送ってよい」とはしない**（`HaRange` が担う）。 */
      this->light_app_.color_temp = HaRange{};
      this->light_ct_sync_(index);
      this->app_slot_ = index;
      this->set_screen_(Screen::APP);
      this->last_activity_ms_ = now;
      ESP_LOGI(TAG, "open app: slot %d (%s) br=%d on=%d", index, this->slots_[index].entity_id.c_str(),
               this->light_app_.brightness, static_cast<int>(this->light_app_.is_on));
      return true;
    }

    case SlotType::GENERIC: {
      /* ⚠️ **状態を持たないアプリ。** 購読も無く、開いた時点で出すものは見出しだけ。 */
      this->generic_app_ = GenericAppState{};
      /* ⚠️ ゼロ値は `Gesture::TAP`。**「何も撃っていない」を明示する。** */
      this->generic_app_.fired = Gesture::COUNT;
      this->app_slot_ = index;
      this->set_screen_(Screen::APP);
      this->last_activity_ms_ = now;
      ESP_LOGI(TAG, "open app: slot %d (generic)", index);
      return true;
    }

    case SlotType::COVER: {
      this->cover_app_ = CoverAppState{};
      /* ⚠️ **範囲は 0-100 で固定。** カーテンの位置は割合なので、機器ごとに違わない
         （温度や色温度と違うところ）。 */
      this->cover_app_.position.set_bounds(0.0f, 100.0f);
      const int16_t pos = this->cover_position_[index].load();
      if (pos >= 0) {
        this->cover_app_.position.set_reported(static_cast<float>(pos));
      }
      this->cover_app_.state = static_cast<CoverState>(this->cover_state_[index].load());
      this->app_slot_ = index;
      this->set_screen_(Screen::APP);
      this->last_activity_ms_ = now;
      ESP_LOGI(TAG, "open app: slot %d (cover) pos=%d", index, pos);
      return true;
    }

    case SlotType::CLIMATE:
    case SlotType::MEDIA_PLAYER:
      /* ⚠️ **まだ画面が無い。** 黙って開いて何も出さないより、開かない方がよい。 */
      break;
  }
  /* ⚠️ 未対応の種別は**黙って落とさない**。開けなかったと言い、ランチャーに留まる。 */
  ESP_LOGW(TAG, "open_app: slot %d has no app for its type yet", index);
  return false;
}

void AstrolabeUI::light_app_input_(Input in, uint32_t now) {
  if (in != Input::ROTATE_CW && in != Input::ROTATE_CCW) {
    return;
  }
  /* ピンを入れ替えてあるので、素直に CW で増える。 */
  const bool up = (in == Input::ROTATE_CW);

  int ct_lo = 0;
  int ct_hi = 0;
  if (this->light_app_.mode == LightMode::COLOR_TEMP &&
      this->light_ct_available_(this->app_slot_, &ct_lo, &ct_hi)) {
    /* ⚠️ **範囲だけ入れ直す。** `set_reported` を呼ぶと「利用者が選んだ」印が倒れるので、
       回している最中に呼んではいけない。 */
    this->light_app_.color_temp.set_bounds(static_cast<float>(ct_lo), static_cast<float>(ct_hi));
    if (!this->light_app_.color_temp.step(up ? COLOR_TEMP_STEP : -COLOR_TEMP_STEP)) {
      /* ⚠️ **値を知らないうちは動かさない**（Y1）。かつてはここで範囲の中点へ跳ばしていたが、
         同じ作法を音量へ広げると**停止中のプレーヤーで1目盛り＝50%**になる。夜の枕元でそれが起きる。
         ⚠️ **点灯もさせない**——中点起動は「回したら点く」も連れていた。
         （撤去は red-team の Y1。2026-08-11 10:21 ゆの承認） */
      ESP_LOGD(TAG, "light: color temp unknown; knob does nothing");
      return;
    }
    /* ⚠️ **色温度を送ると、消えていれば点く**（`light.turn_on` しか手段が無い）。
       画面の側も点いた前提に揃えておく——HAのこだまを待つと2秒ちらつく。 */
    this->light_app_.is_on = true;
    ESP_LOGD(TAG, "light: ct=%d", static_cast<int>(this->light_app_.color_temp.value()));
  } else {
    this->light_app_.brightness += up ? BRIGHTNESS_STEP : -BRIGHTNESS_STEP;
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
    ESP_LOGD(TAG, "light: br=%d", this->light_app_.brightness);
  }

  this->light_app_.last_local_change_ms = now;
  this->light_app_.publish_pending = true;
}

void AstrolabeUI::on_touch_long_(uint32_t now) {
  if (this->app_slot_ >= 0 && this->slots_[this->app_slot_].type == SlotType::COVER) {
    if (!(this->cover_features_[this->app_slot_].load() & COVER_FEAT_STOP)) {
      /* ⚠️ **止められない機器では無音で断る**（案内にも出していない）。 */
      ESP_LOGD(TAG, "cover: stop not supported");
      return;
    }
    this->beep_(BEEP_HZ_MODE, BEEP_MS_MODE);
    ESP_LOGI(TAG, "cover: stop");
    this->cover_action_("stop_cover", now);
    /* ⚠️ **止めたら、送りかけの位置は捨てる。** 止めた直後に古い目標が飛ぶと、
       止めたつもりのカーテンがまた動き出す。 */
    this->cover_app_.publish_pending = false;
    return;
  }
  if (this->app_slot_ >= 0 && this->slots_[this->app_slot_].type == SlotType::GENERIC) {
    this->fire_gesture_(Gesture::HOLD, now);
    return;
  }
  if (!this->light_ct_available_(this->app_slot_, nullptr, nullptr)) {
    /* ⚠️ **無音で断る。** 画面にも `DIM｜CLR` を出していないので、
       ここで鳴らすと「何かが起きた」という嘘になる。 */
    ESP_LOGD(TAG, "touch: long refused (no color temp)");
    return;
  }
  this->light_app_.mode =
      (this->light_app_.mode == LightMode::DIMMER) ? LightMode::COLOR_TEMP : LightMode::DIMMER;
  this->last_activity_ms_ = now;
  /* ⚠️ **他のどれとも違う音。** 同じ場所を押していても起きることが違うので、
     目を離していたら音でしか区別がつかない。 */
  this->beep_(BEEP_HZ_MODE, BEEP_MS_MODE);
  ESP_LOGI(TAG, "light: mode -> %s",
           this->light_app_.mode == LightMode::DIMMER ? "DIM" : "CLR");
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

  /* ⚠️ **位置で初期化しない。** かつてここは `Action{kind, slot, brightness, is_on}` と
     並べて書いていた。あとから `hz` を2番目に挿したとき、**位置が1つずつずれて
     `slot` に明るさが入り**、`loop()` の範囲検査に弾かれて送信が全部消えた。
     コンパイラは `-Wnarrowing` の**警告しか出さない**ので、ビルドは通り続ける。
     名前で代入していれば、メンバを足しても壊れない。 */
  Action a{};
  a.kind = ActionKind::SET_LIGHT;
  a.slot = static_cast<uint8_t>(this->app_slot_);
  a.brightness = static_cast<int16_t>(this->light_app_.brightness);
  a.is_on = this->light_app_.is_on;
  /* ⚠️ **知らない色温度は添えない。** 明るさの面にいるときも添えない——
     いま利用者がいじっているのはそちらではない。 */
  float ct_send = 0.0f;
  a.color_temp = (this->light_app_.mode == LightMode::COLOR_TEMP &&
                  this->light_app_.color_temp.to_send(&ct_send))
                     ? static_cast<int16_t>(ct_send)
                     : -1;
  xQueueSend(this->action_queue_, &a, 0);
}

void AstrolabeUI::beep_(uint32_t hz, uint32_t ms) {
  Action a{};
  a.kind = ActionKind::BEEP;
  a.hz = static_cast<uint16_t>(hz);
  a.ms = static_cast<uint16_t>(ms);
  /* ⚠️ 鳴らし損ねは数えない——**音が1つ落ちても、起きたことは変わらない**。
     数えるのは「Home Assistant を呼ぶはずだったのに呼べなかった」ものだけ。 */
  xQueueSend(this->action_queue_, &a, 0);
}

void AstrolabeUI::fire_gesture_(Gesture g, uint32_t now) {
  if (this->app_slot_ < 0) {
    return;
  }
  const auto &target = this->slots_[this->app_slot_].gestures[static_cast<uint8_t>(g)];
  if (target.service.empty()) {
    /* ⚠️ **書かれていないジェスチャは、何もせず鳴らさない。**
       鳴らすと「効いたが対象が悪い」と読めてしまう。 */
    ESP_LOGD(TAG, "gesture %u not configured", static_cast<unsigned>(g));
    return;
  }

  Action a{};
  a.kind = ActionKind::CALL_GESTURE;
  a.slot = static_cast<uint8_t>(this->app_slot_);
  a.gesture = static_cast<uint8_t>(g);
  const bool queued = xQueueSend(this->action_queue_, &a, 0) == pdTRUE;
  if (!queued) {
    /* ⚠️ **黙って落とさない。** `light` の1目盛りと違い、ここは**1目盛り＝1回の実行**。 */
    this->dropped_actions_.fetch_add(1);
    ESP_LOGW(TAG, "gesture dropped: action queue full");
  }

  const bool online = this->api_connected_.load();
  this->beep_(online ? BEEP_HZ_ACTION : BEEP_HZ_OFFLINE, online ? BEEP_MS : BEEP_MS_MODE);
  this->last_activity_ms_ = now;

  /* ⚠️ **撃ったジェスチャを覚える。** 画面はこれを見て案内の語を光らせる。
     回転でも覚える——語が明るくなるだけなら、回し続ける邪魔にならない。 */
  this->generic_app_.fired = g;
  this->generic_app_.offline = !(online && queued);
  this->generic_app_.at_ms = now;
}

void AstrolabeUI::cover_app_input_(Input in, uint32_t now) {
  if (in != Input::ROTATE_CW && in != Input::ROTATE_CCW) {
    return;
  }
  if (!(this->cover_features_[this->app_slot_].load() & COVER_FEAT_SET_POSITION)) {
    /* ⚠️ **位置指定を持たない機器では、ノブを効かせない。** 動かない目盛りを見せない。 */
    ESP_LOGD(TAG, "cover: no set_position; knob refused");
    return;
  }
  /* ⚠️ **ノブは布の縁を引っぱる。** 右に回したら、画面の布の縁も**時計回りに動く**。
     ⚠️ よって「右回し＝閉じる」は**開き方しだいで変わる**:

       - `center` / `left` … 布は左端（円弧の始まり＝左下）から時計回りに伸びる
                             → 右回しで**閉じる**
       - `right`          … 布は右端（円弧の終わり＝右下）から**反時計回りに**伸びる
                             → 右回しで**開く**

     ⚠️ **円弧の向きは動かせない**——布は実際にその側に溜まるので、塗る側を偽れない。
     動かせるのはノブの向きだけなので、こちらを合わせる。
     （2026-08-11 ゆの: 「右開きの場合、アークの挙動とダイヤルの挙動を揃えてください」） */
  const bool right_opening = (this->slots_[this->app_slot_].opening == CoverOpening::RIGHT);
  float delta = (in == Input::ROTATE_CW) ? -COVER_POSITION_STEP : COVER_POSITION_STEP;
  if (right_opening) {
    delta = -delta;
  }
  if (!this->cover_app_.position.step(delta)) {
    /* ⚠️ **位置を知らないうちは動かさない。** 恣意的な起点から動かすと、
       「少し開けたい」が「全開」になりうる。 */
    ESP_LOGD(TAG, "cover: position unknown; knob does nothing");
    return;
  }
  this->cover_app_.last_local_change_ms = now;
  this->cover_app_.publish_pending = true;
  ESP_LOGD(TAG, "cover: target=%d", static_cast<int>(this->cover_app_.position.value()));
}

void AstrolabeUI::cover_app_publish_(uint32_t now) {
  if (!this->cover_app_.publish_pending) {
    return;
  }
  /* ⚠️ **回している間は送らない。** 手が止まって静定してから、最終位置だけ送る——
     逐次送ると**物理カーテンの移動が毎回割り込まれ、ほとんど動かない**。
     ⚠️ ここが他の種別（120msの間引き）と違うところ。 */
  if (now - this->cover_app_.last_local_change_ms < COVER_SETTLE_MS) {
    return;
  }
  float pos = 0.0f;
  if (!this->cover_app_.position.to_send(&pos)) {
    /* ⚠️ **確からしくない値は送らない。** */
    this->cover_app_.publish_pending = false;
    return;
  }
  this->cover_app_.publish_pending = false;

  Action a{};
  a.kind = ActionKind::SET_COVER_POSITION;
  a.slot = static_cast<uint8_t>(this->app_slot_);
  a.position = static_cast<int16_t>(pos);
  if (xQueueSend(this->action_queue_, &a, 0) != pdTRUE) {
    this->dropped_actions_.fetch_add(1);
    ESP_LOGW(TAG, "cover position dropped: action queue full");
  }
}

void AstrolabeUI::cover_action_(const char *service, uint32_t now) {
  Action a{};
  a.kind = ActionKind::COVER_ACTION;
  a.slot = static_cast<uint8_t>(this->app_slot_);
  a.cover_service = service;
  if (xQueueSend(this->action_queue_, &a, 0) != pdTRUE) {
    this->dropped_actions_.fetch_add(1);
    ESP_LOGW(TAG, "cover action dropped: action queue full");
  }
  this->cover_app_.last_local_change_ms = now;
  this->last_activity_ms_ = now;
}

void AstrolabeUI::app_input_(Input in, uint32_t now) {
  if (this->app_slot_ < 0) {
    return;
  }
  /* ⚠️ **種別で分岐するのはここ1箇所。** 呼び先が増えても、分岐は増やさない。 */
  switch (this->slots_[this->app_slot_].type) {
    case SlotType::LIGHT:
      this->light_app_input_(in, now);
      return;
    case SlotType::GENERIC:
      this->fire_gesture_(in == Input::ROTATE_CW ? Gesture::ROTATE_RIGHT : Gesture::ROTATE_LEFT, now);
      return;
    case SlotType::COVER:
      this->cover_app_input_(in, now);
      return;
    case SlotType::CLIMATE:
    case SlotType::MEDIA_PLAYER:
      /* まだ実装していない。⚠️ **開けないので、ここへは来ない**（`open_app_` が断る）。 */
      return;
  }
}

void AstrolabeUI::set_screen_(Screen next) {
  if (this->screen_ == next) {
    return;
  }
  this->screen_ = next;
  /* ⚠️ **進行中の接触を取り消す。** 押下と離上は別の時刻の出来事なので、その間に
     画面が変わると、離した瞬間の動作が**押し始めたのとは違う画面**へ効く。
     `screen_` への代入をこの関数だけに集めてあるのは、
     **規則を守るのではなく、破る道を塞ぐため**。 */
  this->cancel_touch_("screen changed");
}

void AstrolabeUI::cancel_touch_(const char *why) {
  if (!this->touch_.down || this->touch_.action_fired) {
    return;
  }
  ESP_LOGD(TAG, "touch: cancelled (%s)", why);
  /* 指はまだ触れているので `down` は倒さない。**この接触ではもう何もしない**という印だけ立てる。 */
  this->touch_.action_fired = true;
}

void AstrolabeUI::poll_touch_(uint32_t now) {
  /* ⚠️ **`getTouchPointsNum()` 経由で読む。** この呼び出しが `_poll_g_ctrl()`（放置死の計器）
     も回しているので、読み方を変えると計器が止まる。 */
  const bool touched = this->tp_.getTouchPointsNum() > 0;

  /* ⚠️ **読めなかったフレームは、指の有無について何も言っていない。**
     `getTouchPointsNum()` は読み取りに失敗しても `0` を返す。ここで捨てないと、
     **I2Cが1回こけただけで「指を離した」ことになり、短押しが撃たれる**。
     押している時間は流れたままでよいので、`down` もタイマも触らずに戻る。 */
  if (!this->tp_.getTouchReadOk()) {
    return;
  }

  /* ── 押下エッジ ── */
  if (!this->touch_.down) {
    if (!touched) {
      return;
    }
    this->tp_.update();
    const auto pt = this->tp_.getTouchPointBuffer();
    const int dx = pt.x - 120;
    const int dy = pt.y - 120;
    const int radius = (this->screen_ == Screen::LAUNCHER) ? CENTER_TAP_RADIUS : APP_TAP_RADIUS;

    this->touch_ = TouchGesture{};
    this->touch_.down = true;
    this->touch_.press_ms = now;
    this->touch_.screen = this->screen_;
    /* ⚠️ **時計だけは中央に限らない。** 暗い中で手探りで起こす画面なので、
       起こしやすさを取る。長押しが無いので、どこを触っても意味は1つしかない。 */
    this->touch_.in_center =
        (this->screen_ == Screen::CLOCK) || (dx * dx + dy * dy) <= (radius * radius);
    /* ⚠️ **開く先は押した時点で決める。** 離すまでの間に選択が動いても、
       開くのは押した方（ノブは接触を取り消すので通常ここは効かないが、
       「押した位置が意味を持つ」という規則をコードの側に残しておく）。 */
    this->touch_.slot = (this->screen_ == Screen::LAUNCHER)
                            ? static_cast<int>(this->menu_->getSelector()->getTargetItem())
                            : this->app_slot_;

    if (!this->touch_.in_center) {
      /* ⚠️ 中央の外で押し始めたら、**どれだけ長く押しても**何も起こさない。
         リング上のアイコンや、ダイヤルの縁を握った手が拾われないようにする。 */
      ESP_LOGD(TAG, "touch: outside (%d,%d)", pt.x, pt.y);
      this->touch_.action_fired = true;
    }
    return;
  }

  /* ── 押している間 ── */
  if (touched) {
    const uint32_t held = now - this->touch_.press_ms;

    /* ⚠️ **閾値を跨いだ瞬間に撃つ。離すのを待たない。**
       離してから長押しか短押しかを決めると、**画面は押している間ずっと無反応**になる。 */
    if (!this->touch_.action_fired && this->touch_.screen == Screen::APP &&
        held >= TOUCH_LONGPRESS_MS) {
      this->touch_.action_fired = true;
      ESP_LOGD(TAG, "touch: long (%ums)", static_cast<unsigned>(held));
      this->on_touch_long_(now);
    }

    /* 見限り。⚠️ **`action_fired` とは別に数える**——長押しを撃った指をそのまま置いていても
       一度は言う必要があり、しかし毎周期言ってはいけない。 */
    if (!this->touch_.stuck_reported && held >= TOUCH_MAX_PRESS_MS) {
      this->touch_.stuck_reported = true;
      this->touch_.action_fired = true;
      ESP_LOGW(TAG, "touch: stuck (%ums held; controller may be wedged)", static_cast<unsigned>(held));
    }
    return;
  }

  /* ── 離上エッジ ── */
  const uint32_t held = now - this->touch_.press_ms;
  this->touch_.down = false;
  if (this->touch_.action_fired) {
    return;
  }
  if (held < TOUCH_GHOST_MS) {
    ESP_LOGD(TAG, "touch: ghost (%ums)", static_cast<unsigned>(held));
    return;
  }
  ESP_LOGD(TAG, "touch: short (%ums)", static_cast<unsigned>(held));
  this->on_touch_short_(now);
}

void AstrolabeUI::on_touch_short_(uint32_t now) {
  /* ⚠️ **押し始めた画面**で分岐する。`screen_` は `set_screen_` が取り消すので、
     ここへ来ている時点で両者は一致しているが、意味の主語は「押し始めた画面」。 */
  switch (this->touch_.screen) {
    case Screen::LAUNCHER:
      this->last_activity_ms_ = now;
      /* ⚠️ **ボタンと同じ音**。利用者にとっては同じ「決定」なので、
         入口が違うだけで音が変わると混乱する。 */
      this->beep_(BEEP_HZ_PRESS);
      ESP_LOGD(TAG, "launcher: center tap (activity)");
      this->open_app_(this->touch_.slot, now);
      return;

    case Screen::CLOCK:
      /* 時計のタップも起床。 */
      this->beep_(BEEP_HZ_PRESS);
      if (!this->open_app_(0, now)) {
        this->set_screen_(Screen::LAUNCHER);
        this->last_activity_ms_ = now;
      }
      return;

    case Screen::APP:
      if (this->app_slot_ >= 0 && this->slots_[this->app_slot_].type == SlotType::GENERIC) {
        this->fire_gesture_(Gesture::TAP, now);
        return;
      }
      if (this->app_slot_ >= 0 && this->slots_[this->app_slot_].type == SlotType::COVER) {
        /* ⚠️ **半分より開いていれば閉じる、でなければ開く。**
           位置を知らないときは「開く」に倒す——⚠️ **閉じる方が取り返しがつかない**
           （夜に勝手に開くより、朝に勝手に閉まる方が困る）。 */
        const uint32_t feat = this->cover_features_[this->app_slot_].load();
        const auto &pos = this->cover_app_.position;
        const bool closing = pos.usable() && pos.value() >= COVER_HALFWAY;
        const uint32_t need = closing ? COVER_FEAT_CLOSE : COVER_FEAT_OPEN;
        if (!(feat & need)) {
          ESP_LOGD(TAG, "cover: %s not supported", closing ? "close" : "open");
          return;
        }
        this->beep_(BEEP_HZ_ACTION);
        ESP_LOGI(TAG, "cover: %s", closing ? "close" : "open");
        this->cover_action_(closing ? "close_cover" : "open_cover", now);
        /* ⚠️ **こちらの目標も端へ寄せる。** そうしないと、動いている間ずっと
           古い目標値が画面に出続ける。 */
        this->cover_app_.position.set_chosen(closing ? 0.0f : 100.0f);
        this->cover_app_.publish_pending = false;
        return;
      }
      /* アプリの中でのタップはトグル。 */
      this->last_activity_ms_ = now;
      this->light_app_.is_on = !this->light_app_.is_on;
      if (this->light_app_.is_on && this->light_app_.brightness == 0) {
        this->light_app_.brightness = 128; /* 消灯から点けたときの既定 */
      }
      this->light_app_.last_local_change_ms = now;
      ESP_LOGI(TAG, "light: toggle -> %s", this->light_app_.is_on ? "on" : "off");
      this->beep_(BEEP_HZ_ACTION);
      /* ⚠️ トグルは間引かない。押した手応えが遅れるのが一番きらわれる。 */
      this->light_app_publish_(now, /*force=*/true);
      return;
  }
}

void AstrolabeUI::apply_input_(Input in, uint32_t now) {
  /* ⚠️ **ノブやボタンが来たら、進行中の接触は取り消す。**
     押しながら回すと選択が動くので、離した瞬間に**見えているのと違うスロット**が開く。 */
  this->cancel_touch_("input while touching");

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
        this->set_screen_(Screen::LAUNCHER);
        /* ⚠️ ランチャーへ戻った瞬間から数え直す。ここを忘れると、時計に居た時間が
           そのまま無操作時間として残り、**戻った直後にまた時計へ落ちる**。 */
        this->last_activity_ms_ = now;
      } else {
        /* 起床先は**先頭スロット**。⚠️ 開けなければランチャーへ落とす。 */
        if (!this->open_app_(0, now)) {
          this->set_screen_(Screen::LAUNCHER);
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
        this->set_screen_(Screen::LAUNCHER);
        this->app_slot_ = -1;
      } else {
        this->app_input_(in, now);
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

    /* タッチ。⚠️ **画面によらず毎周期読む。** 時計のときだけ読まないようにすると計器が止まる。 */
    this->poll_touch_(now);

    /* ⚠️ **計器の写しを取るのはここ。** 上の `poll_touch_()` が `getTouchPointsNum()` を
       通り、それが G_CTRL の見張りも回しているので、その直後が最新。
       メインループはこの atomic だけを読む（`touch_g_ctrl()` 他）。 */
    this->g_ctrl_last_.store(this->tp_.getGCtrlLast());
    this->g_ctrl_boot_.store(this->tp_.getGCtrlBoot());
    this->g_ctrl_bad_.store(this->tp_.getGCtrlBadCount());
    this->g_ctrl_polls_.store(static_cast<uint32_t>(this->tp_.getGCtrlPollCount()));
    this->tp_read_fail_.store(this->tp_.getTouchReadFailCount());

    switch (this->screen_) {
      case Screen::LAUNCHER:
        /* ⚠️ 無操作の判定は**ランチャーにいるときだけ**。時計はアイドル画面そのもので、
           そこにタイムアウトを持たせない（`astrolabe_ui.h` の `last_activity_ms_` 参照）。 */
        if (now - this->last_activity_ms_ >= IDLE_TIMEOUT_MS) {
          ESP_LOGI(TAG, "launcher -> clock (idle %ums)", static_cast<unsigned>(IDLE_TIMEOUT_MS));
          this->set_screen_(Screen::CLOCK);
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
        /* ⚠️ **`generic` は状態を持たないので、こだまも送信も無い。**
           結果を少し見せてから待受へ戻すだけ。 */
        if (this->app_slot_ >= 0 && this->slots_[this->app_slot_].type == SlotType::GENERIC) {
          if (now - this->last_activity_ms_ >= IDLE_TIMEOUT_MS) {
            ESP_LOGI(TAG, "app -> clock (idle)");
            this->set_screen_(Screen::CLOCK);
            this->app_slot_ = -1;
            this->last_clock_render_ms_ = now - CLOCK_RENDER_MS;
            break;
          }
          if (this->generic_app_.fired != Gesture::COUNT &&
              now - this->generic_app_.at_ms >= GENERIC_RESULT_MS) {
            this->generic_app_.fired = Gesture::COUNT;
            this->generic_app_.offline = false;
          }
          {
            const auto &slot = this->slots_[this->app_slot_];
            const auto &tags = this->render_slots_[this->app_slot_];
            GenericView view{};
            view.tag_up = &tags.tag_up;
            view.tag_down = &tags.tag_down;
            view.fired = static_cast<uint8_t>(this->generic_app_.fired);
            view.offline = this->generic_app_.offline;
            view.has_tap = !slot.gestures[static_cast<int>(Gesture::TAP)].service.empty();
            view.has_hold = !slot.gestures[static_cast<int>(Gesture::HOLD)].service.empty();
            view.has_rotate = !slot.gestures[static_cast<int>(Gesture::ROTATE_RIGHT)].service.empty() ||
                              !slot.gestures[static_cast<int>(Gesture::ROTATE_LEFT)].service.empty();
            render_generic(this->canvas_, view);
          }
          this->canvas_->pushSprite(0, 0);
          break;
        }

        if (this->app_slot_ >= 0 && this->slots_[this->app_slot_].type == SlotType::COVER) {
          if (now - this->last_activity_ms_ >= IDLE_TIMEOUT_MS) {
            /* ⚠️ **戻る前に、溜まっている目標を出し切る。** 静定待ちの最中に閉じると
               最後に回したぶんが投げられずに消える。 */
            this->cover_app_.last_local_change_ms = 0;
            this->cover_app_publish_(now);
            ESP_LOGI(TAG, "app -> clock (idle)");
            this->set_screen_(Screen::CLOCK);
            this->app_slot_ = -1;
            this->last_clock_render_ms_ = now - CLOCK_RENDER_MS;
            break;
          }

          const uint32_t feat = this->cover_features_[this->app_slot_].load();
          this->cover_app_.state = static_cast<CoverState>(this->cover_state_[this->app_slot_].load());
          /* HAからのこだま。⚠️ **ローカル操作の直後は無視する**——
             動いている最中の値で目標を上書きすると、ノブと綱引きになる。 */
          if (now - this->cover_app_.last_local_change_ms >= LOCAL_CHANGE_GUARD_MS) {
            const int16_t pos = this->cover_position_[this->app_slot_].load();
            if (pos >= 0) {
              this->cover_app_.position.set_reported(static_cast<float>(pos));
            } else {
              this->cover_app_.position.clear_reported();
            }
          }
          this->cover_app_publish_(now);

          CoverView view{};
          view.position = this->cover_app_.position.usable()
                              ? static_cast<int>(this->cover_app_.position.value())
                              : -1;
          view.state = static_cast<uint8_t>(this->cover_app_.state);
          view.can_set_position = (feat & COVER_FEAT_SET_POSITION) != 0;
          view.can_stop = (feat & COVER_FEAT_STOP) != 0;
          view.opening = static_cast<CoverOpeningView>(this->slots_[this->app_slot_].opening);
          render_cover(this->canvas_, view);
          this->canvas_->pushSprite(0, 0);
          break;
        }

        /* アプリも無操作で時計へ。⚠️ ランチャーではなく**時計へ**戻る。 */
        if (now - this->last_activity_ms_ >= IDLE_TIMEOUT_MS) {
          this->light_app_publish_(now, /*force=*/this->light_app_.publish_pending);
          ESP_LOGI(TAG, "app -> clock (idle)");
          this->set_screen_(Screen::CLOCK);
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

        {
          /* ⚠️ **能力は毎フレーム読み直す。** 開いたときの写しを使うと、
             電球を色温度非対応のものへ替えたあとも `DIM｜CLR` を出し続ける。 */
          int ct_lo = 0;
          int ct_hi = 0;
          const bool ct_ok = this->light_ct_available_(this->app_slot_, &ct_lo, &ct_hi);

          if (!ct_ok && this->light_app_.mode == LightMode::COLOR_TEMP) {
            /* ⚠️ **色温度の面にいる最中に能力が消えたら、その場で明るさへ戻す。**
               再起動を待たない。 */
            ESP_LOGI(TAG, "light: color temp no longer available; back to DIM");
            this->light_app_.mode = LightMode::DIMMER;
          }

          if (ct_ok && now - this->light_app_.last_local_change_ms >= LOCAL_CHANGE_GUARD_MS) {
            /* ⚠️ **回し終わってから写す。** 回している最中に呼ぶと、HAのこだまが
               「利用者が選んだ」印を倒してノブと綱引きになる。 */
            this->light_ct_sync_(this->app_slot_);
          }

          this->light_app_publish_(now, /*force=*/false);

          LightView view{};
          view.brightness = this->light_app_.brightness;
          view.is_on = this->light_app_.is_on;
          view.state_received = this->light_app_.state_received;
          view.color_temp_mode = (this->light_app_.mode == LightMode::COLOR_TEMP);
          /* ⚠️ **`-1` ＝ 値そのものが無い。** 0K と混ぜない（描画側がそう読む）。 */
          view.color_temp = this->light_app_.color_temp.usable()
                                ? static_cast<int>(this->light_app_.color_temp.value())
                                : -1;
          view.color_temp_min = ct_lo;
          view.color_temp_max = ct_hi;
          view.color_temp_available = ct_ok;
          render_light(this->canvas_, view);
        }
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
