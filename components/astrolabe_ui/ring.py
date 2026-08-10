"""リング配置の計算。**ホストで単体テストできるように、ESPHomeから独立させてある。**

⚠️ ここに置く理由: この計算まわりで**過去に地雷を2つ踏んでいる**。

  1. 描画側の境界をアイコン配列から取り、添字をメニュー項目から取っていた
     （両者がずれた瞬間に不正アクセス）
  2. 除数が `int n = 7;` の直書きで、8個目以降が一周を越えて重なっていた

どちらも**実機なしで再現できる種類**の誤りなので、機械が判定できる場所へ出す。
`esphome-audio-node` が `pcm_stream_policy.c` で同じことをしている
（純ロジックを切り出してホストテストを当てる）。
"""

from __future__ import annotations

import math

#: セレクタが回る軌道の半径。⚠️ **アイコンが描かれる r=95 ではない。**
#: 描画側が項目座標を (RING_RADIUS*2)/120 倍してアイコン位置を導くため、
#: ⚠️ ここに95を渡すと二重に拡大されて画面外へ飛ぶ。
SELECTOR_TRACK_RADIUS = 60.0
#: メニュー項目の当たり判定の一辺
SELECTOR_ITEM_SIZE = 22
#: 240x240 の中心
RING_CENTER = 120
#: アイコンが実際に描かれる半径（重なり判定はこれで見る）
ICON_RING_RADIUS = 95.0
#: アイコンの直径（等倍）
ICON_DIAMETER = 42.0
#: **選択中のアイコンの実描画径。** 選択されると 1.135 倍に拡大される。
#: ⚠️ **重なり判定にはこちらを使う**——どのアイコンも選択されうるので、
#: 等倍(42)で判定すると「選んだ瞬間に隣とぶつかる」構成が通ってしまう。
ICON_SELECTED_DIAMETER = 47.7
#: 実機の見た目で決めた上限。
#: ⚠️ **幾何学的には12まで載るが、10で既に窮屈**というのが実機での所見。
#: 幾何の判定だけに任せず、この上限も併せて効かせる。
SLOTS_MAX = 10


def slot_positions(n: int) -> list[tuple[int, int]]:
    """n個のスロットを円周へ等間隔に置く。12時の位置から時計回り。

    ⚠️ **除数は必ず実際の件数 n を使う。** 定数を直書きしない（過去に踏んだ地雷）。
    """
    if n <= 0:
        return []
    r = SELECTOR_TRACK_RADIUS
    out = []
    for i in range(n):
        angle = 2.0 * math.pi * i / n - math.pi / 2.0
        out.append(
            (
                RING_CENTER + int(r * math.cos(angle)),
                RING_CENTER + int(r * math.sin(angle)),
            )
        )
    return out


def icon_center_distance(n: int) -> float:
    """隣り合うアイコンの中心間距離。n<2 なら無限大（重なりようがない）。"""
    if n < 2:
        return float("inf")
    return 2.0 * ICON_RING_RADIUS * math.sin(math.pi / n)


def ring_fits(n: int) -> bool:
    """n個のアイコンがリング上で重ならずに収まるか。

    ⚠️ **等号を許さない**——中心間距離がちょうど径だと接する。
    ⚠️ **比べる相手は `ICON_SELECTED_DIAMETER`**（拡大時）であって等倍の径ではない。
    """
    if n <= 0:
        return False
    if n > SLOTS_MAX:
        return False
    if n == 1:
        return True  # 隣接が無いので必ず収まる
    return icon_center_distance(n) > ICON_SELECTED_DIAMETER


def max_slots() -> int:
    """置ける最大数。**ハードコードせずここから引く。**

    幾何の限界と `SLOTS_MAX` の小さいほうになる。
    """
    n = 1
    while ring_fits(n + 1):
        n += 1
    return n
