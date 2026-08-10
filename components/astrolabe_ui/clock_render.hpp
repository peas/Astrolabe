/**
 * @file clock_render.hpp
 * @brief アイドル時の時計画面。
 *
 * ⚠️ **ESPHomeの `time:` オブジェクトを参照していない。** 参照する必要が無いため——
 * `time/real_time_clock.cpp:51,60,76-77` が `settimeofday()` と
 * `setenv("TZ", ...)` + `tzset()` を実際に呼んでいるので、
 * **標準の `time()` / `localtime_r()` がそのままローカル時刻を返す**（上流を読んで確認済み）。
 * おかげで**描画タスク（core 1）からESPHomeのオブジェクトを触らずに済む**——
 * 触っていたらスレッド所有の約束（`astrolabe_ui.h` の冒頭）を破っていた。
 *
 * ⚠️ ただし `time:` が**YAMLに無いと永遠に `--:--`** のままなので、
 * `__init__.py` の `FINAL_VALIDATE_SCHEMA` が名指しで落とす（`api:` の2フラグと同じ扱い）。
 */
#pragma once

#include <cstdio>
#include <ctime>

#include <LovyanGFX.hpp>

namespace esphome {
namespace astrolabe_ui {

/* ランチャーの真鍮とは意図的に変えてある（暗い部屋で眩しくない青） */
static constexpr uint32_t CLOCK_BG_COLOR = 0x020408;
static constexpr uint32_t CLOCK_TIME_COLOR = 0xA0C0FF;
static constexpr uint32_t CLOCK_DATE_COLOR = 0x405060;
static constexpr uint32_t CLOCK_WDAY_COLOR = 0x507080;
static constexpr uint32_t CLOCK_HINT_COLOR = 0x181818;
static constexpr uint32_t CLOCK_SYNCING_COLOR = 0x303030;
static constexpr uint32_t CLOCK_SYNCING_SUB_COLOR = 0x203040;

/** 時計を1枚描く。**状態を持たない**——呼ぶ側（描画タスク）が間隔を決める。 */
inline void render_clock(LGFX_Sprite *canvas) {
  static const char *const WDAY[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  constexpr int CX = 120;

  time_t now;
  struct tm t;
  time(&now);
  localtime_r(&now, &t);
  /* ⚠️ `tm_year` は1900年起点なので、100（＝西暦2000年）を超えていれば
     同期済みとみなす。**同期前に「00:00」と出さない**——止まった時計は
     「時刻が分からない」より悪い（利用者が信じてしまう）。 */
  const bool time_valid = (t.tm_year > 100);

  canvas->fillScreen(CLOCK_BG_COLOR);
  canvas->setFont(&fonts::efontCN_24);

  if (time_valid) {
    char hhmm[6];
    snprintf(hhmm, sizeof(hhmm), "%02d:%02d", t.tm_hour, t.tm_min);
    canvas->setTextSize(1.5f);
    canvas->setTextColor(CLOCK_TIME_COLOR);
    canvas->drawCenterString(hhmm, CX, 72);

    char date[11];
    snprintf(date, sizeof(date), "%04d/%02d/%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    canvas->setTextSize(1.0f);
    canvas->setTextColor(CLOCK_DATE_COLOR);
    canvas->drawCenterString(date, CX, 126);

    canvas->setTextColor(CLOCK_WDAY_COLOR);
    canvas->drawCenterString(WDAY[t.tm_wday], CX, 154);
  } else {
    canvas->setTextSize(1.5f);
    canvas->setTextColor(CLOCK_SYNCING_COLOR);
    canvas->drawCenterString("--:--", CX, 72);
    canvas->setTextSize(0.75f);
    canvas->setTextColor(CLOCK_SYNCING_SUB_COLOR);
    canvas->drawCenterString("syncing...", CX, 140);
  }

  canvas->setFont(&fonts::Font2);
  canvas->setTextSize(1.0f);
  canvas->setTextColor(CLOCK_HINT_COLOR);
  canvas->drawCenterString("press: back", CX, 210);
}

}  // namespace astrolabe_ui
}  // namespace esphome
