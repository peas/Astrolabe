"""MDIアイコンを 42x42 RGB565 へ焼く（ビルド時）。

描画の作り（供給過多サンプリング・LANCZOS・円マスク・近黒への逃がし）は
**実機で確定させたもの**。下の2つの ⚠️ は自力で踏み直す価値がない。

## 取得方法の決定

**MDIの固定タグからビルド時に落とす。**同梱もせず、`master` も参照しない。

- ⚠️ `master` 参照（ESPHomeの `image:` が `download_gh_svg` でやっている方法）だと
  **版を固定できない**。source-only配布では利用者側の再現性の穴になる。
- ⚠️ リポジトリ同梱（1.5MB）は**版を固定できるがリポジトリが重くなる**。
  そして `external_components` は**submoduleを初期化しない**（`external_components/__init__.py:41`
  が `git.clone_or_update()` に `submodules` を渡していない）ので、submoduleでは逃げられない。
- → **固定タグのURLを `download_content()` で取る。** ネットワークは
  `external_components` の時点で既に要るので**追加コストが無い**。
  キャッシュは ESPHome が持つ（`has_remote_file_changed` がHEADを見る）。

⚠️ **`external_files.download_content` はESPHomeの内部API。** ただし
`download_gh_svg` より1段下の層で、`image:`/`font:` の両方が乗っている土台。
**壊れたときの検出器は週次CI**（アイコンの追従と兼務・増分8）。
"""

from __future__ import annotations

import re
from pathlib import Path

import esphome.config_validation as cv
from esphome.external_files import compute_local_file_dir, download_content

#: ⚠️ **固定する。** ここを `master` にしたら、この移植の意味が半分無くなる。
#: フォントのバイト数が期待どおりであることを確認済み（1,307,660 B）。
MDI_VERSION = "7.4.47"

_BASE = f"https://raw.githubusercontent.com/Templarian/MaterialDesign-Webfont/v{MDI_VERSION}"
_FONT_URL = f"{_BASE}/fonts/materialdesignicons-webfont.ttf"
#: 名前→コードポイント表。`meta.json`（3.2MB）ではなくこちら（210KB）を使う。
_MAP_URL = f"{_BASE}/scss/_variables.scss"

#: アイコンの一辺。⚠️ **正本は C++ 側 `launcher_render.hpp` の `ICON_DIAMETER`（42）。**
#: 食い違うと、描画時に寸法が合わずアイコンが崩れる。
ICON_PX = 42

#: 既定色。実機で調整したもの。
#: ⚠️ 背景は当初 `#D6D6D6` だったが、**黒画面の上ではほぼ白に見えた**ため
#: 実機で見ると**黒画面の上ではほぼ白に見えた**ため1段暗くしてある（Material grey-500 相当）。
DEFAULT_ICON_BG = "#9E9E9E"
DEFAULT_ICON_FG = "#141414"

#: 描いてから縮める倍率。アンチエイリアスはこれで作る。
_SUPERSAMPLE = 4
#: 円の中に収めるグリフの箱（42pxのうち26px）。
_GLYPH_BOX_PX = 26
#: 円マスクの内側半径。⚠️ 下の「穴あき」対策の適用範囲。
_INNER_RADIUS = 19.0
#: ⚠️ **真っ黒（0x0000）を円の内側に残さない。**
#: 残すと機器側で穴が開き、**アプリを開くアニメで下の色が透ける**（旧実装 I2）。
_NEAR_BLACK = 0x0821

#: YAMLに書ける形。**ここが「任意画像を焼かせない」唯一の関門。**
MDI_PREFIX = "mdi:"

_name_map: dict[str, int] | None = None
_font_path: Path | None = None
_cache: dict[tuple[str, str, str], list[int]] = {}


def _local_dir() -> Path:
    return compute_local_file_dir("astrolabe_ui")


def font_path() -> Path:
    """MDIのTrueTypeを固定タグから取る（キャッシュあり）。"""
    global _font_path
    if _font_path is None:
        path = _local_dir() / f"materialdesignicons-webfont-{MDI_VERSION}.ttf"
        download_content(_FONT_URL, path)
        _font_path = path
    return _font_path


def name_map() -> dict[str, int]:
    """MDI名 → コードポイント（7,447件）。

    `_variables.scss` の `  "ab-testing": F01C9,` という行を拾うだけ。
    ⚠️ `meta.json`（3.2MB）を使わない——**15倍の転送量で同じ情報**。
    """
    global _name_map
    if _name_map is None:
        path = _local_dir() / f"mdi-variables-{MDI_VERSION}.scss"
        raw = download_content(_MAP_URL, path).decode("utf-8")
        _name_map = {
            m.group(1): int(m.group(2), 16)
            for m in re.finditer(r'"([a-z0-9-]+)":\s*([0-9A-Fa-f]{4,6})', raw)
        }
        if not _name_map:
            raise cv.Invalid(
                f"MDIの名前表を読めませんでした（{_MAP_URL}）。"
                "上流の書式が変わった可能性があります"
            )
    return _name_map


def validate_icon(value):
    """YAMLの `icon:` を検証する。**ここではネットワークを使わない。**

    ⚠️ 名前が実在するかは `to_code`（＝ビルド時）で見る。
    そうしないと **`esphome config` がネットワーク必須**になり、
    CIやオフラインでの構文確認ができなくなる。
    """
    value = cv.string_strict(value)
    if not value.startswith(MDI_PREFIX):
        raise cv.Invalid(f"icon は `{MDI_PREFIX}` で始めてください（指定は {value!r}）")
    name = value[len(MDI_PREFIX) :]
    if not re.fullmatch(r"[a-z0-9-]+", name):
        raise cv.Invalid(f"MDIのアイコン名として読めません: {name!r}")
    return value


def parse_hex(value: str) -> tuple[int, int, int]:
    m = re.fullmatch(r"#?([0-9A-Fa-f]{6})", value.strip())
    if not m:
        raise cv.Invalid(f"色は #RRGGBB で書いてください（指定は {value!r}）")
    v = int(m.group(1), 16)
    return ((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)


def render_icon(icon: str, bg: str = DEFAULT_ICON_BG, fg: str = DEFAULT_ICON_FG) -> list[int]:
    """`mdi:name` を 42x42 の `uint16_t` 配列にする。

    ⚠️⚠️ **値はバイト入れ替え済みのRGB565**（真の色 `0xD6BA` なら `0xBAD6` を入れる）。
    理屈: LovyanGFX の `pushImage(..., const uint16_t*)` はメモリを
    **上位バイトが先**（swap565）として読む。ESP32はリトルエンディアンなので、
    `uint16_t` に入れる値は真の色の**上下を入れ替えたもの**になる。
    よって配列には `0xBAD6` のような**上下が入れ替わって見える値**が並ぶ。

    ⚠️ **ここを触るなら実機で確認すること。** 旧実装が実機で確定させた挙動で
    （逆に入れたら**紫に転び、アンチエイリアスの中間色まで飛んで輪郭がザリザリ**になった）。
    """
    key = (icon, bg, fg)
    if key in _cache:
        return _cache[key]

    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError as err:  # pragma: no cover
        raise cv.Invalid(
            "Pillow が要ります（ESPHomeの必須依存なので通常は入っています）"
        ) from err

    name = icon[len(MDI_PREFIX) :]
    codepoint = name_map().get(name)
    if codepoint is None:
        raise cv.Invalid(
            f"MDI {MDI_VERSION} に {name!r} というアイコンはありません"
        )

    bg_rgb = parse_hex(bg)
    fg_rgb = parse_hex(fg)
    glyph = chr(codepoint)
    size = ICON_PX * _SUPERSAMPLE
    path = str(font_path())

    # グリフの実寸は形ごとに違う（横長・縦長がある）ので、一度測ってから箱に収める。
    # ⚠️ 決め打ちにすると欠けるか小さすぎる。
    probe = ImageFont.truetype(path, size)
    left, top, right, bottom = probe.getbbox(glyph)
    span = max(right - left, bottom - top)
    if span <= 0:
        raise cv.Invalid(f"{name!r} のグリフが空です")
    font = ImageFont.truetype(path, max(1, int(size * (_GLYPH_BOX_PX * _SUPERSAMPLE) / span)))

    canvas = Image.new("RGB", (size, size), (0, 0, 0))
    draw = ImageDraw.Draw(canvas)
    # 円は画像いっぱい。四隅は黒のまま＝背景として沈む。
    draw.ellipse((0, 0, size - 1, size - 1), fill=bg_rgb)
    left, top, right, bottom = font.getbbox(glyph)
    draw.text(
        ((size - (right + left)) / 2, (size - (bottom + top)) / 2),
        glyph,
        font=font,
        fill=fg_rgb,
    )
    canvas = canvas.resize((ICON_PX, ICON_PX), Image.LANCZOS)

    out = _to_swapped_rgb565(canvas.load())
    _cache[key] = out
    return out


def _to_swapped_rgb565(pixels) -> list[int]:
    center = (ICON_PX - 1) / 2.0
    out: list[int] = []
    for y in range(ICON_PX):
        dy = y - center
        for x in range(ICON_PX):
            r, g, b = pixels[x, y]
            value = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            if value == 0x0000:
                dx = x - center
                if dx * dx + dy * dy <= _INNER_RADIUS * _INNER_RADIUS:
                    value = _NEAR_BLACK
            # ⚠️ 上下を入れ替えて格納する（上の render_icon の注意を読むこと）。
            out.append(((value & 0xFF) << 8) | ((value >> 8) & 0xFF))
    return out
