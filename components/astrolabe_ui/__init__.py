"""Astrolabe — M5Dial（ESP32-S3）のHome Assistantコントローラ。

スロット（ダイヤルに並ぶ操作対象）は**YAMLに宣言し、ビルド時にファームへ焼く**。
実行時にHome Assistantから差し替える経路は作らない（設計上の決定）。

  astrolabe_ui:
    slots:
      - type: light
        entity_id: light.study
        tag_up: "スタディ"
        tag_down: "照明"

⚠️⚠️ **このディレクトリを整理してサブディレクトリに戻さないこと。**
ESPHomeの外部コンポーネントローダは**同じ階層のファイルしか運ばない**——
`esphome/loader.py` の `ComponentManifest.resources` の docstring が
"does not look through subdirectories" と明言しており、`iterdir()` の `r.is_file()` で弾いている。
実測でもサブディレクトリが丸ごと消えてビルドが落ちた。
"""

from __future__ import annotations

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components.ledc.output import LEDCOutput
from esphome.const import CONF_ENTITY_ID, CONF_ICON, CONF_ID, CONF_RAW_DATA_ID, CONF_TYPE

from . import icons, ring

CODEOWNERS = ["@Khronos31"]
# ⚠️ `api` は必須。状態購読（subscribe_homeassistant_state）と
#    操作（call_homeassistant_service）の両方がこれに乗っている。
DEPENDENCIES = ["api"]
# ⚠️ ブザーの音程を変えるので `ledc` の型が要る（素の `output` では周波数を
#    実行時に変えられない）。使わない構成でもヘッダが要るので AUTO_LOAD で引き込む。
AUTO_LOAD = ["ledc"]

astrolabe_ui_ns = cg.esphome_ns.namespace("astrolabe_ui")
AstrolabeUI = astrolabe_ui_ns.class_("AstrolabeUI", cg.Component)

CONF_SLOTS = "slots"
CONF_TAG_UP = "tag_up"
CONF_TAG_DOWN = "tag_down"

#: 0.1.0 が扱う種別 → 期待する entity のドメイン。
#: ⚠️ **増やすときは C++ 側の `SlotType` と、下の `SLOT_TYPE_IDS` を一緒に増やす。**
#: ⚠️ 0.2.0 で足す種別は `climate` / `cover` / `media_player` / `generic`（決定D4）。
#:    **種別名は entity のドメインと一致させる**——一致していれば下の検証が表を持たずに済む。
SLOT_TYPE_LIGHT = "light"
SLOT_TYPES = {SLOT_TYPE_LIGHT: "light"}

#: C++ 側 `SlotType` の値。⚠️ **順番ではなく名前で対応させる**——
#: YAMLの見た目やdictの並びに依存させない。
SLOT_TYPE_IDS = {SLOT_TYPE_LIGHT: 0}

#: `icon:` を書かなかったときの既定。⚠️ **種別ごとに持つ**——
#: 種別ごとに既定を持つ。
DEFAULT_ICONS = {SLOT_TYPE_LIGHT: "mdi:lightbulb"}

CONF_BUZZER = "buzzer"
CONF_SOUND = "sound"
CONF_ICON_BG = "icon_bg"
CONF_ICON_FG = "icon_fg"


def _validate_entity_domain(config):
    """`type` と `entity` のドメインが噛み合っているか。

    ⚠️ ここで弾かないと、**実機が存在しない対象を操作し続ける**ことになる
    （画面には出るが何も起きない、という一番分かりにくい壊れ方）。
    """
    domain = config[CONF_ENTITY_ID].split(".", 1)[0]
    want = SLOT_TYPES[config[CONF_TYPE]]
    if domain != want:
        raise cv.Invalid(
            f"type: {config[CONF_TYPE]} には {want}. で始まる entity が要ります"
            f"（指定されたのは {config[CONF_ENTITY_ID]}）"
        )
    return config


def _apply_default_icon(config):
    """`icon:` 未指定なら種別の既定を入れる。⚠️ **既定を1箇所に閉じる。**"""
    if CONF_ICON not in config:
        config[CONF_ICON] = DEFAULT_ICONS[config[CONF_TYPE]]
    return config


SLOT_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Required(CONF_TYPE): cv.one_of(*SLOT_TYPES, lower=True),
            cv.Required(CONF_ENTITY_ID): cv.entity_id,
            cv.Required(CONF_TAG_UP): cv.string,
            cv.Optional(CONF_TAG_DOWN, default=""): cv.string,
            # ⚠️ ここでは**書式しか見ない**。実在するかはビルド時（`to_code`）。
            #    実在確認をここへ持ってくると `esphome config` がネットワーク必須になる。
            cv.Optional(CONF_ICON): icons.validate_icon,
            cv.Optional(CONF_ICON_BG, default=icons.DEFAULT_ICON_BG): cv.string,
            cv.Optional(CONF_ICON_FG, default=icons.DEFAULT_ICON_FG): cv.string,
            # 焼き込む画素配列のID。利用者は書かない（ESPHomeの `image:` と同じ作り）。
            cv.GenerateID(CONF_RAW_DATA_ID): cv.declare_id(cg.uint16),
        }
    ),
    _validate_entity_domain,
    _apply_default_icon,
)


def _validate_ring(config):
    """リングに載る数か。**旧実装が踏んだ地雷を、機械が判定できる場所へ出したもの。**

    旧実装は除数を `int n = 7;` と直書きしており、8個目以降が一周を越えて重なっていた。
    ここで落とせば実機に焼く前に分かる。
    """
    n = len(config[CONF_SLOTS])
    if ring.ring_fits(n):
        return config

    # ⚠️ **落ちた理由を取り違えて伝えない。** 上限に当たったのか、幾何で重なるのかは別。
    if n > ring.SLOTS_MAX:
        raise cv.Invalid(
            f"slots は最大 {ring.SLOTS_MAX} 件です（指定は {n} 件）。"
            f"これは実機の見た目で決めた上限で、幾何学的にはもう少し載りますが窮屈になります",
            path=[CONF_SLOTS],
        )
    raise cv.Invalid(
        f"slots が {n} 件ではリング上でアイコンが重なります"
        f"（隣り合う中心の距離 {ring.icon_center_distance(n):.1f}px ≦ "
        f"選択時のアイコン径 {ring.ICON_SELECTED_DIAMETER:.1f}px）。"
        f"⚠️ 選択されたアイコンは拡大されるので、等倍では収まっていても足りません",
        path=[CONF_SLOTS],
    )


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(AstrolabeUI),
            cv.Required(CONF_SLOTS): cv.All(
                cv.ensure_list(SLOT_SCHEMA), cv.Length(min=1)
            ),
            # 手応えを鳴らすか。⚠️ **利用者向けの唯一の音の設定。**
            #    置き場所（枕元か机か）で答えが変わるので、個体ごとに違って当たり前。
            #    ⚠️ **音量や音程は出さない**——音の設計は製品の一部で、
            #      変えたい場合は `packages/` を自分のリポジトリへ写して使う。
            cv.Optional(CONF_SOUND, default=True): cv.boolean,
            # ブザーの出力先。⚠️ **package が埋めるので利用者は書かない。**
            #    音程を入力の種類ごとに変えるので `LEDCOutput`（素の `output` では
            #    実行時に周波数を変えられない）。
            cv.Optional(CONF_BUZZER): cv.use_id(LEDCOutput),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_ring,
)


def _final_validate(config):
    """⚠️ **`api:` の2つのフラグは既定オフ**（`api/__init__.py:190-191`）。

    これらが `USE_API_HOMEASSISTANT_STATES` / `USE_API_HOMEASSISTANT_SERVICES` の
    define を切り替えており、`subscribe_homeassistant_state` も
    `call_homeassistant_service` も**その `#ifdef` の中**にある。
    書き忘れるとリンクで落ちるか、落ちずに黙って何も起きない。
    **利用者が原因に辿り着けないので、ここで名指しで落とす。**
    """
    full = fv.full_config.get()
    api_conf = full.get("api")
    if api_conf is None:
        raise cv.Invalid("astrolabe_ui には `api:` が要ります")
    missing = [
        k
        for k in ("homeassistant_states", "homeassistant_services")
        if not api_conf.get(k)
    ]
    if missing:
        raise cv.Invalid(
            "astrolabe_ui には次の api オプションが要ります（既定はオフです）: "
            + ", ".join(f"{k}: true" for k in missing)
        )

    # ⚠️ **時計は `time:` が無いと永遠に `--:--`** のまま。しかも
    #    「壊れている」ようには見えず「同期待ちに見える画面」が延々出るだけなので、
    #    利用者が原因に辿り着けない。**ここで名指しで落とす。**
    #    C++側は `time_id` を取らない——ESPHomeの `time:` が `settimeofday()` と
    #    `setenv("TZ")` を呼ぶので、標準の `localtime_r()` がそのまま使える
    #    （`time/real_time_clock.cpp:51,60,76-77`）。よって**必要なのは存在確認だけ**。
    if not full.get("time"):
        raise cv.Invalid(
            "astrolabe_ui には `time:` が要ります（アイドル時の時計に使います）。"
            "例:\n  time:\n    - platform: homeassistant"
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # vendored 1.1.7 を持ち込まず PlatformIO に取らせる。
    # ⚠️ **完全固定する**（`^` を使わない）。source-only 配布では、
    #    固定していない依存が利用者側の再現性の穴になる。
    cg.add_library("lovyan03/LovyanGFX", "1.2.26")
    # LovyanGFX v1 系のAPIを使う指定。明示しないと落ちる。
    cg.add_build_flag("-DLGFX_USE_V1")

    # ⚠️ `sound: false` のときは**繋がない**。鳴らす経路はすべて
    #    「ブザーが設定されていれば」で守られているので、これだけで黙る。
    if config[CONF_SOUND] and CONF_BUZZER in config:
        cg.add(var.set_buzzer(await cg.get_variable(config[CONF_BUZZER])))

    positions = ring.slot_positions(len(config[CONF_SLOTS]))
    for slot, (x, y) in zip(config[CONF_SLOTS], positions):
        # ⚠️ **ここで初めてネットワークを使う**（MDIの固定タグから取得・キャッシュあり）。
        #    アイコン名が実在しなければ、ここで名指しで落ちる。
        pixels = icons.render_icon(
            slot[CONF_ICON], slot[CONF_ICON_BG], slot[CONF_ICON_FG]
        )
        arr = cg.progmem_array(slot[CONF_RAW_DATA_ID], pixels)
        cg.add(
            var.add_slot(
                SLOT_TYPE_IDS[slot[CONF_TYPE]],
                slot[CONF_ENTITY_ID],
                slot[CONF_TAG_UP],
                slot[CONF_TAG_DOWN],
                x,
                y,
                arr,
            )
        )
