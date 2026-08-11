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

#: エンティティを1つ持つ種別 → 期待する entity のドメイン。
#: ⚠️ **増やすときは C++ 側の `SlotType` と、下の `SLOT_TYPE_IDS` を一緒に増やす。**
#: ⚠️ **種別名は entity のドメインと一致させる**——一致していれば検証が表を持たずに済む。
SLOT_TYPE_LIGHT = "light"
SLOT_TYPE_CLIMATE = "climate"
SLOT_TYPE_COVER = "cover"
SLOT_TYPE_MEDIA = "media_player"
#: ⚠️ **`generic` だけ entity を持たない。** ジェスチャごとに呼び先を書く。
SLOT_TYPE_GENERIC = "generic"

SLOT_TYPES = {
    SLOT_TYPE_LIGHT: "light",
    SLOT_TYPE_CLIMATE: "climate",
    SLOT_TYPE_COVER: "cover",
    SLOT_TYPE_MEDIA: "media_player",
}

#: C++ 側 `SlotType` の値。⚠️ **順番ではなく名前で対応させる**——
#: YAMLの見た目やdictの並びに依存させない。
SLOT_TYPE_IDS = {
    SLOT_TYPE_LIGHT: 0,
    SLOT_TYPE_CLIMATE: 1,
    SLOT_TYPE_COVER: 2,
    SLOT_TYPE_MEDIA: 3,
    SLOT_TYPE_GENERIC: 4,
}

#: `icon:` を書かなかったときの既定。⚠️ **種別ごとに持つ。**
DEFAULT_ICONS = {
    SLOT_TYPE_LIGHT: "mdi:lightbulb",
    SLOT_TYPE_CLIMATE: "mdi:air-conditioner",
    SLOT_TYPE_COVER: "mdi:window-shutter",
    SLOT_TYPE_MEDIA: "mdi:music",
    SLOT_TYPE_GENERIC: "mdi:gesture-tap-button",
}

# ── cover の見た目 ────────────────────────────────────────────────────
CONF_OPENING = "opening"

#: ⚠️ **布がどちら側から伸びるか。** 画面の円弧は `ARC_START`(135°)＝左下、
#:    終端(45°)＝右下なので、そのまま左右に対応する。
#: ⚠️ C++ 側 `CoverOpening` と名前で対応させる。
COVER_OPENINGS = {"center": 0, "left": 1, "right": 2}

# ── media_player の操作リング ──────────────────────────────────────────
#: ⚠️ **リング上の位置＝並び順**（`ring.py` の `slot_positions(4)` は12時から時計回り）。
#:    上＝再生/一時停止 / 右＝次 / 下＝停止 / 左＝前。
#:    **次が右・前が左**で空間の感覚と一致する（2026-08-11 ゆの）。
#: ⚠️ **C++ 側 `MediaAction` と並びで対応させる**（ここだけは順番に意味がある）。
MEDIA_RING_ICONS = [
    "mdi:play-pause",
    "mdi:skip-next",
    "mdi:stop",
    "mdi:skip-previous",
]
#: 焼き込む画素配列のID。⚠️ **1組を全スロットで共有する**（スロットごとに焼かない）。
CONF_MEDIA_RING_IDS = "media_ring_ids"

# ── climate の運転モード ────────────────────────────────────────────────
CONF_MODES = "modes"

#: Home Assistant の `HVACMode` の全語彙。
#: ⚠️ **一次情報から取っている**（`homeassistant/components/climate/const.py` の
#:    `class HVACMode(StrEnum)`。2026-08-11 に実物を読んで確認）。
#: ⚠️ **ここに無い綴りはビルドで落とす。** 実行時に黙って効かないより、書いた時点で分かる方がよい（Y5）。
#: ⚠️ C++ 側 `HvacMode` と名前で対応させる。
CLIMATE_MODES = {
    "off": 0,
    "heat": 1,
    "cool": 2,
    "heat_cool": 3,
    "auto": 4,
    "dry": 5,
    "fan_only": 6,
}


def _climate_mode(value):
    """運転モードを1つ検める。

    ⚠️ **YAMLは裸の `off` を真偽値 `False` にする**（`on`/`yes`/`no` も同様。YAML 1.1）。
    `off` は Home Assistant の正当な運転モード名なので、**利用者が
    `modes: [cool, heat, off]` と書くのは自然**——ここで拾わないと
    「Auto-converted this value to boolean」という、原因の見えない失敗になる。
    引用符で括れとだけ言うより、**こちらで受ける**方がよい。
    """
    if value is False:
        return "off"
    if value is True:
        # ⚠️ `on` という運転モードは無い。**黙って何かに読み替えない。**
        raise cv.Invalid(
            "modes: に `on` は書けません（Home Assistant の運転モードに `on` はありません）。"
            f"使えるのは {', '.join(CLIMATE_MODES)} です"
        )
    return cv.one_of(*CLIMATE_MODES, lower=True)(value)


def _validate_modes(config):
    """`modes:` の並びを検める。

    ⚠️ **機器が対応しているかはここでは分からない**——ビルド時にHAへ問い合わせる経路が無い
    （0.1.1で確認済み）。対応の有無は実行時に `hvac_modes` を購読して見る。
    ここで見られるのは**綴りと並び方**だけ。
    """

    modes = config.get(CONF_MODES, [])
    if len(set(modes)) != len(modes):
        raise cv.Invalid(
            f"modes: に同じモードが2回書かれています（{modes}）。"
            "長押しで一周する並びなので、重複すると同じ場所を2度通ります"
        )
    return config


# ── generic のジェスチャ ────────────────────────────────────────────────
CONF_TAP = "tap"
CONF_HOLD = "hold"
CONF_ROTATE_RIGHT = "rotate_right"
CONF_ROTATE_LEFT = "rotate_left"

#: C++ 側 `Gesture` の値。名前で対応させる。
GESTURES = {CONF_TAP: 0, CONF_HOLD: 1, CONF_ROTATE_RIGHT: 2, CONF_ROTATE_LEFT: 3}

#: ⚠️ **「押す」の意味がドメインごとに違う。** toggle で済ませると意図と別のことが起きる。
#:    Home Assistant のサービス一覧を実際に問い合わせて確かめた（2026-08-11）:
#:      - `script.toggle`     … 走っていれば**止める**。起動したいなら turn_on
#:      - `automation.toggle` … 自動化の**有効/無効**の切替。実行ではない
#:      - `button` / `scene`  … そもそも toggle が無い
GESTURE_SERVICES = {
    "button": "button.press",
    "input_button": "input_button.press",
    "scene": "scene.turn_on",
    "script": "script.turn_on",
    "automation": "automation.trigger",
}

#: 上の表に無いドメインのうち、**自前の `toggle` を持つもの**。
#: ⚠️ **`homeassistant.toggle` を使わない。** `cover` と `valve` は `turn_on`/`turn_off` を
#:    持たないので `homeassistant.toggle` では動かない（実物で確認）。`<domain>.toggle` なら動く。
#: ⚠️ ここに無いドメインは**ビルドで落とす**。実行時に黙って失敗させない——
#:    利用者には「設定の誤り」と「通信障害」の区別がつかないため。
GESTURE_TOGGLEABLE = (
    "light",
    "switch",
    "fan",
    "cover",
    "valve",
    "input_boolean",
    "media_player",
    "climate",
    "humidifier",
    "siren",
    "remote",
)


def gesture_service(entity_id: str) -> str:
    """ジェスチャに書かれた entity を、呼ぶべきサービスへ。

    ⚠️ **Python側で解決する。** C++へは「サービス名」と「entity」だけを渡す——
    表がここにあれば、**書けない組み合わせはビルドで落ちる**。
    """
    domain = entity_id.split(".", 1)[0]
    if domain in GESTURE_SERVICES:
        return GESTURE_SERVICES[domain]
    if domain in GESTURE_TOGGLEABLE:
        return f"{domain}.toggle"
    raise cv.Invalid(
        f"{entity_id} は astrolabe_ui のジェスチャから操作できません。"
        f"押したときに何をすべきかが決まらないドメインです（{domain}.）。\n"
        "操作できるのは: "
        + ", ".join(sorted(list(GESTURE_SERVICES) + list(GESTURE_TOGGLEABLE)))
        + "\n⚠️ 値を選ぶ種類のもの（input_number. や select. など）は、"
        "スクリプトに包んで script. を指してください"
    )


CONF_BUZZER = "buzzer"
CONF_SOUND = "sound"
CONF_ICON_BG = "icon_bg"
CONF_ICON_FG = "icon_fg"


# ⚠️ **`cv.typed_schema` は `type` を pop してから内側を検証する**
#    （`config_validation.py:2037,2044-2045`）。つまり**内側のバリデータからは
#    `type` が見えない**。種別を引数で渡す形にしてある——見えないものを読むと
#    `KeyError: 'type'` で落ちる（実際に踏んだ）。


def _entity_domain_validator(type_name):
    """`type` と `entity` のドメインが噛み合っているか。

    ⚠️ ここで弾かないと、**実在しない対象を操作し続ける**ことになる
    （画面には出るが何も起きない、という一番分かりにくい壊れ方）。
    """

    want = SLOT_TYPES[type_name]

    def validate(config):
        domain = config[CONF_ENTITY_ID].split(".", 1)[0]
        if domain != want:
            raise cv.Invalid(
                f"type: {type_name} には {want}. で始まる entity が要ります"
                f"（指定されたのは {config[CONF_ENTITY_ID]}）"
            )
        return config

    return validate


def _default_icon_validator(type_name):
    """`icon:` 未指定なら種別の既定を入れる。⚠️ **既定を1箇所に閉じる。**"""

    def validate(config):
        if CONF_ICON not in config:
            config[CONF_ICON] = DEFAULT_ICONS[type_name]
        return config

    return validate


#: すべての種別に共通の欄。
_COMMON = {
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


def _entity_slot(type_name, schema_extra=None, *extra_validators):
    """entity を1つ持つ種別のスキーマ。

    `extra_validators` は種別固有の検めごと。⚠️ **スキーマの後に走る**ので、
    既定値が入った状態を見られる。
    """
    d = dict(_COMMON)
    d[cv.Required(CONF_ENTITY_ID)] = cv.entity_id
    if schema_extra:
        d.update(schema_extra)
    return cv.All(
        cv.Schema(d),
        _entity_domain_validator(type_name),
        _default_icon_validator(type_name),
        *extra_validators,
    )


def _validate_generic(config):
    """⚠️ **1つもジェスチャが書かれていない `generic` を通さない。**

    通すと、リングに並んで開けるのに**押しても何も起きないスロット**ができる。
    どこが悪いのか利用者には分からない（画面は正常に見える）。
    """
    if not any(g in config for g in GESTURES):
        raise cv.Invalid(
            "type: generic には "
            + " / ".join(GESTURES)
            + " のうち少なくとも1つが要ります（どれも無いと、開けても何も起きません）"
        )
    return config


def _validate_gesture_entities(config):
    """⚠️ **押せないドメインをビルドで落とす。** 実行時に黙って失敗させない。"""
    for g in GESTURES:
        if g in config:
            gesture_service(config[g])  # 決まらなければ cv.Invalid を投げる
    return config


GENERIC_SCHEMA = cv.All(
    cv.Schema(
        {
            **_COMMON,
            # ⚠️ **4つとも任意。** 書かれていないジェスチャは何も起こさず、鳴らない。
            cv.Optional(CONF_TAP): cv.entity_id,
            cv.Optional(CONF_HOLD): cv.entity_id,
            cv.Optional(CONF_ROTATE_RIGHT): cv.entity_id,
            cv.Optional(CONF_ROTATE_LEFT): cv.entity_id,
        }
    ),
    _validate_generic,
    _validate_gesture_entities,
    _default_icon_validator(SLOT_TYPE_GENERIC),
)

#: ⚠️ **種別ごとにスキーマが分かれる。** `generic` は `entity_id` を持たず、
#:    代わりにジェスチャごとの呼び先を持つ——**同じスキーマには収まらない**。
SLOT_SCHEMA = cv.typed_schema(
    {
        SLOT_TYPE_LIGHT: _entity_slot(SLOT_TYPE_LIGHT),
        SLOT_TYPE_CLIMATE: _entity_slot(
            SLOT_TYPE_CLIMATE,
            {
                # ⚠️ **長押しで一周する並び。** 書かなければ長押しは効かず、案内にも出ない。
                #    ⚠️ **温度域（min/max）はここに書かせない**——保存すると買い替え時に
                #    古い値が凍る。モードは利用者が選ぶ、範囲は機器に従う（G3）。
                cv.Optional(CONF_MODES, default=[]): cv.ensure_list(_climate_mode),
            },
            _validate_modes,
        ),
        SLOT_TYPE_COVER: _entity_slot(
            SLOT_TYPE_COVER,
            {
                # ⚠️ **見た目だけの設定。** 操作の意味は変わらない。
                #    既定が `center` なのは、両開きが一番多く、左右非対称にならないため。
                cv.Optional(CONF_OPENING, default="center"): cv.one_of(
                    *COVER_OPENINGS, lower=True
                ),
            },
        ),
        SLOT_TYPE_MEDIA: _entity_slot(SLOT_TYPE_MEDIA),
        SLOT_TYPE_GENERIC: GENERIC_SCHEMA,
    },
    lower=True,
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
            # 操作リングのアイコンを焼くID。⚠️ **利用者は書かない。**
            #    ⚠️ `media_player` スロットが1つも無ければ焼かない（下の `to_code`）。
            cv.GenerateID(f"{CONF_MEDIA_RING_IDS}_0"): cv.declare_id(cg.uint16),
            cv.GenerateID(f"{CONF_MEDIA_RING_IDS}_1"): cv.declare_id(cg.uint16),
            cv.GenerateID(f"{CONF_MEDIA_RING_IDS}_2"): cv.declare_id(cg.uint16),
            cv.GenerateID(f"{CONF_MEDIA_RING_IDS}_3"): cv.declare_id(cg.uint16),
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
    for index, (slot, (x, y)) in enumerate(zip(config[CONF_SLOTS], positions)):
        # ⚠️ **ここで初めてネットワークを使う**（MDIの固定タグから取得・キャッシュあり）。
        #    アイコン名が実在しなければ、ここで名指しで落ちる。
        pixels = icons.render_icon(
            slot[CONF_ICON], slot[CONF_ICON_BG], slot[CONF_ICON_FG]
        )
        arr = cg.progmem_array(slot[CONF_RAW_DATA_ID], pixels)
        cg.add(
            var.add_slot(
                SLOT_TYPE_IDS[slot[CONF_TYPE]],
                # ⚠️ `generic` は entity を持たない。空文字で渡す——
                #    C++側は「空なら状態を購読しない」で扱う。
                slot.get(CONF_ENTITY_ID, ""),
                slot[CONF_TAG_UP],
                slot[CONF_TAG_DOWN],
                x,
                y,
                arr,
            )
        )
        if slot[CONF_TYPE] == SLOT_TYPE_COVER:
            cg.add(var.set_cover_opening(index, COVER_OPENINGS[slot[CONF_OPENING]]))
        if slot[CONF_TYPE] == SLOT_TYPE_CLIMATE:
            # ⚠️ **YAMLに書いた順のまま渡す。** 長押しはこの並びを一周する。
            for mode in slot[CONF_MODES]:
                cg.add(var.add_climate_mode(index, CLIMATE_MODES[mode]))
        # ⚠️ **サービス名はここで解決して渡す。** C++側に表を持たせない——
        #    表がPython側にあれば、**押せない組み合わせはビルドで落ちる**。
        for gesture, gesture_id in GESTURES.items():
            if gesture in slot:
                cg.add(
                    var.add_gesture(
                        index, gesture_id, gesture_service(slot[gesture]), slot[gesture]
                    )
                )

    # ⚠️ **操作リングのアイコンは1組だけ焼く。** スロットごとに焼くと同じ絵が重複する。
    #    ⚠️ `media_player` スロットが1つも無ければ**焼かない**——使わない14KBを載せない。
    if any(sl[CONF_TYPE] == SLOT_TYPE_MEDIA for sl in config[CONF_SLOTS]):
        for i, name in enumerate(MEDIA_RING_ICONS):
            pixels = icons.render_icon(
                name, icons.DEFAULT_ICON_BG, icons.DEFAULT_ICON_FG
            )
            arr = cg.progmem_array(config[f"{CONF_MEDIA_RING_IDS}_{i}"], pixels)
            cg.add(var.add_media_ring_icon(i, arr))
