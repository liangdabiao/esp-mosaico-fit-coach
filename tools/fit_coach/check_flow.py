#!/usr/bin/env python3
"""fit_coach 模拟器回放校验：分段 headless 回放 + 状态/像素断言。

与 astock_quote/tools/astock/check_flow.py 同机制：同一份确定性
replay_flow.json，用不同 --frames 截断运行，对关键阶段做状态断言
（页面流转、演示模式计数递增）与像素断言（按钮配色存在）。

用法：python tools/fit_coach/check_flow.py
"""
from __future__ import annotations

import json
import shutil
import subprocess
import sys
from pathlib import Path

import PIL.Image

ROOT = Path(__file__).resolve().parents[2]          # projects/fit_coach
REPLAY = ROOT / "tools" / "fit_coach" / "replay_flow.json"
BUILD = ROOT / "build-host"
RUN = ROOT.parent.parent / "mosaico.py"

# 页面：0 主页 / 1 训练 / 2 结果 / 3 设置 / 4 历史
STAGES = [
    (4,   "t1_home",    {"page": 0, "mode": 0, "target": 30}),
    (60,  "t2_train1",  {"page": 1, "streaming": True}),
    (322, "t3_result1", {"page": 2, "reps": lambda v: isinstance(v, int) and v >= 3}),
    (332, "t4_home2",   {"page": 0}),
    (342, "t5_setting", {"page": 3}),
    (352, "t6_home3",   {"page": 0}),
    (362, "t7_train2",  {"page": 1}),
    (695, "t8_reps2",   {"page": 1, "reps": lambda v: isinstance(v, int) and v >= 3}),
    (720, "t9_home4",   {"page": 0}),
]

# 像素断言：(stage名, 颜色, 区域, 最少像素)
C_BLUE = (110, 160, 255)   # 开始训练按钮
C_RED = (235, 76, 76)      # 训练页结束按钮
PIXEL_CHECKS = [
    ("t1_home", C_BLUE, (140, 240, 340, 300), 2000),    # 主页大按钮
    ("t2_train1", C_RED, (270, 410, 440, 450), 2000),   # 训练页结束按钮
]


def run_stage(frames: int) -> dict:
    out = BUILD / f"stage_{frames:03d}.json"
    cmd = [sys.executable, str(RUN), "game", "sim", "--project", str(ROOT),
           "--headless", "--frames", str(frames),
           "--replay", str(REPLAY), "--state-output", str(out)]
    subprocess.run(cmd, check=True, capture_output=True, text=True)
    shutil.copyfile(BUILD / "frame.png", BUILD / f"stage_{frames:03d}.png")
    return json.loads(out.read_text(encoding="utf-8"))


def state_match(state: dict, expect: dict) -> bool:
    for k, v in expect.items():
        got = state.get(k)
        if callable(v):
            if not v(got):
                return False
        elif got != v:
            return False
    return True


def ink_pixels(png: Path, target: tuple[int, int, int], x0: int, y0: int,
                x1: int, y1: int, step: int = 1) -> int:
    im = PIL.Image.open(png).convert("RGB")
    n = 0
    for y in range(y0, y1, step):
        for x in range(x0, x1, step):
            p = im.getpixel((x, y))
            if abs(p[0] - target[0]) <= 18 and abs(p[1] - target[1]) <= 18 \
               and abs(p[2] - target[2]) <= 18:
                n += 1
    return n


def main() -> int:
    BUILD.mkdir(parents=True, exist_ok=True)
    failures = 0
    pngs: dict[str, Path] = {}
    states: dict[str, dict] = {}
    for frames, name, expect in STAGES:
        state = run_stage(frames)
        pngs[name] = BUILD / f"stage_{frames:03d}.png"
        states[name] = state
        ok = state_match(state, expect)
        print(f"  {'ok  ' if ok else 'FAIL'} {name}: expect={expect} "
              f"reps={state.get('reps')} page={state.get('page')} "
              f"stage={state.get('stage')} angle={state.get('angle')}")
        if not ok:
            failures += 1

    for name, color, box, minimum in PIXEL_CHECKS:
        n = ink_pixels(pngs[name], color, *box)
        ok = n >= minimum
        print(f"  {'ok  ' if ok else 'FAIL'} pixels {name}: {n} >= {minimum}")
        if not ok:
            failures += 1

    if failures:
        print(f"FAILED: {failures}")
        return 1
    print("ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
