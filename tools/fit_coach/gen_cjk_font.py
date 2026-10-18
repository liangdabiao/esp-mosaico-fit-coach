#!/usr/bin/env python3
"""生成 fit_coach 用中文位图字模子集（gk_cjk_glyph_data.h）。

与 astock_quote/tools/astock/gen_cjk_font.py 同一机制：Pillow 从
Noto Sans CJK SC 母字体把页面用到的汉字光栅化成 16x16 位图，打包成
C 表（每行 16bit），由 main/gk_cjk_font.c blit。

字符来源：
  * UI_STRINGS（动态拼出的文案兜底）；
  * main/*.c 里全部 C 字符串字面量中的 CJK 与全角标点（含 fc_net.c 的
    配网页面文案——多出的字模无害）。
母字体缺失的码点会报错。

用法（仓库根目录或本目录均可）：
    python tools/fit_coach/gen_cjk_font.py
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "main" / "gk_cjk_glyph_data.h"
SRC_DIR = ROOT / "main"
FONT_PATH = Path("D:/esp/fontsrc/NotoSansCJKsc-Regular.otf")

# 动态拼接文案的兜底清单（字面量里的会被自动扫描收录）。
UI_STRINGS = [
    "姿态健身教练",
    "仰卧起坐",
    "今日", "目标", "个", "组", "分", "秒", "用时",
    "躺平", "坐起", "检测中", "请入镜", "无信号", "达标", "目标达成",
    "服务器", "云端直连", "演示模式", "正常", "失败", "离线", "连接中",
    "开始训练", "暂停", "继续", "结束", "再来一组", "完成", "返回",
    "设置", "历史", "最近记录", "暂无记录",
    "模式", "每组目标", "躺下阈值", "坐起阈值", "音效", "开", "关",
    "重置网络配置", "再按一次确认重置", "度",
]

CELL = 16
SIZE = 16


def _is_wanted(c: str) -> bool:
    return ord(c) >= 0x2E80 or c in "·『』、，。（）：；？"


def collect_chars() -> list[str]:
    chars: set[str] = set()
    for s in UI_STRINGS:
        for c in s:
            if _is_wanted(c):
                chars.add(c)
    for path in sorted(SRC_DIR.glob("*.c")):
        src = path.read_text(encoding="utf-8")
        for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', src):
            for c in lit:
                if _is_wanted(c):
                    chars.add(c)
    return sorted(chars)


def rasterize(font, ch: str) -> list[int]:
    """返回 CELL 行、每行 16bit（bit15 = 最左列）。"""
    from PIL import Image, ImageDraw
    im = Image.new("L", (CELL, CELL), 0)
    d = ImageDraw.Draw(im)
    bbox = font.getbbox(ch)
    w = bbox[2] - bbox[0]
    h = bbox[3] - bbox[1]
    ox = (CELL - w) // 2 - bbox[0]
    oy = (CELL - h) // 2 - bbox[1]
    d.text((ox, oy), ch, font=font, fill=255)
    px = im.load()
    rows = []
    for y in range(CELL):
        bits = 0
        for x in range(CELL):
            if px[x, y] > 96:
                bits |= 1 << (15 - x)
        rows.append(bits)
    return rows


def main() -> int:
    if not FONT_PATH.is_file():
        print(f"找不到母字体：{FONT_PATH}", file=sys.stderr)
        return 1
    from PIL import ImageFont
    font = ImageFont.truetype(str(FONT_PATH), SIZE)

    chars = collect_chars()
    glyphs = [(ch, rasterize(font, ch)) for ch in chars]

    lines = [
        "// SPDX-License-Identifier: Apache-2.0",
        "// 由 tools/fit_coach/gen_cjk_font.py 自动生成 —— 勿手改。",
        f"// 母字体：Noto Sans CJK SC Regular（SIL OFL 1.1）；{len(chars)} 个字模，{CELL}x{CELL}。",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        f"#define GK_CJK_CELL {CELL}",
        "",
        "typedef struct { uint16_t cp; uint16_t rows[GK_CJK_CELL]; } gk_cjk_glyph_t;",
        "",
        f"#define GK_CJK_GLYPH_COUNT {len(glyphs)}",
        "static const gk_cjk_glyph_t GK_CJK_GLYPHS[GK_CJK_GLYPH_COUNT] = {",
    ]
    for ch, rows in glyphs:
        cp = ord(ch)
        rows_str = ",".join(f"0x{r:04x}" for r in rows)
        lines.append(f"    {{ 0x{cp:04x}, {{ {rows_str} }} }},  /* {ch} */")
    lines.append("};")
    lines.append("")

    OUT.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"wrote {OUT.relative_to(ROOT)}: {len(chars)} glyphs, "
          f"{len(chars) * CELL * 2} bytes bitmap")
    return 0


if __name__ == "__main__":
    sys.exit(main())
