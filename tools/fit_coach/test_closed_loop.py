#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""fit-coach 全链路闭环联调（宿主）：服务器 → HTTP → 设备计数内核。

模拟设备 runtime 的真实行为：以 300ms 节流（设备 http 模式采样率）POST 帧，
把服务器回传的 JSON 按 fc_provider_http.c 的同款映射规则折算成 fc_sample_t，
喂进**真实编译的设备计数内核**（tests/fc_count.dll，源码同 main/fc_count.c）。

场景：mock 服务器输出 3 秒周期正弦角度（与设备演示模式同相位），
90 秒 ≈ 30 次仰卧起坐。断言网络往返后计数内核输出与预期一致。

前置：
  * FC_MOCK=1 服务器已在 :8000 运行
  * tests/fc_count.dll 已构建（gcc -shared main/fc_count.c）
用法（项目根目录）：
  python tools/fit_coach/test_closed_loop.py [seconds]
"""
from __future__ import annotations

import ctypes
import json
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DLL = ROOT / "tests" / "fc_count.dll"
BASE = "http://127.0.0.1:8000"
DURATION_S = int(sys.argv[1]) if len(sys.argv) > 1 else 90
CADENCE_MS = 300  # 设备 http 模式节流

# ---- ctypes 绑定（结构布局必须与 fc_count.h 一致） ----


class FcSample(ctypes.Structure):
    _fields_ = [
        ("angle_deg", ctypes.c_float),
        ("stage", ctypes.c_int),      # fc_stage_t
        ("visible", ctypes.c_bool),
        ("conf", ctypes.c_float),
        ("t_ms", ctypes.c_uint32),
    ]


class FcStats(ctypes.Structure):
    _fields_ = [
        ("reps", ctypes.c_uint32),
        ("stage", ctypes.c_int),
        ("angle_smooth", ctypes.c_float),
        ("angle_last", ctypes.c_float),
        ("last_rep_ms", ctypes.c_uint32),
        ("rep_just_now", ctypes.c_uint32),
        ("last_sample_ms", ctypes.c_uint32),
        ("samples_seen", ctypes.c_uint32),
    ]


FC_STAGE_UNKNOWN, FC_STAGE_UP, FC_STAGE_DOWN = 0, 1, 2


def load_core():
    lib = ctypes.CDLL(str(DLL))
    lib.fc_counter_new.argtypes = [ctypes.c_int, ctypes.c_int]
    lib.fc_counter_new.restype = ctypes.c_void_p
    lib.fc_counter_feed.argtypes = [ctypes.c_void_p, ctypes.POINTER(FcSample)]
    lib.fc_counter_stats.argtypes = [ctypes.c_void_p]
    lib.fc_counter_stats.restype = ctypes.POINTER(FcStats)
    return lib


def stage_from_str(s):
    """与 fc_provider_http.c dispatch_sample 同款映射。"""
    if s == "up":
        return FC_STAGE_UP
    if s == "down":
        return FC_STAGE_DOWN
    return FC_STAGE_UNKNOWN


def post(body: bytes, ts: int) -> dict:
    req = urllib.request.Request(
        BASE + "/frame", data=body, method="POST",
        headers={"Content-Type": "image/jpeg", "X-FC-TS": str(ts)})
    with urllib.request.urlopen(req, timeout=10) as resp:
        return json.loads(resp.read().decode())


def main() -> int:
    lib = load_core()
    counter = lib.fc_counter_new(110, 55)
    assert counter

    fake_jpeg = b"\xff\xd8\xff\xe0fakeframe"
    t0 = time.time()
    sent = visible = 0
    lats = []
    next_at = t0
    while True:
        now = time.time()
        if now - t0 >= DURATION_S:
            break
        if now < next_at:
            time.sleep(0.005)
            continue
        next_at += CADENCE_MS / 1000.0

        ts_ms = int((now - t0) * 1000)
        s0 = time.perf_counter()
        data = post(fake_jpeg, ts_ms)
        lats.append((time.perf_counter() - s0) * 1000)
        sent += 1

        sample = FcSample()
        sample.angle_deg = data.get("angle_deg", -1)
        if sample.angle_deg is None or sample.angle_deg < 0:
            sample.angle_deg = -1.0
        sample.stage = stage_from_str(data.get("stage"))
        sample.visible = bool(data.get("visible"))
        sample.conf = data.get("conf", 1.0) or 1.0
        sample.t_ms = data.get("t_ms", ts_ms) or ts_ms
        if sample.visible:
            visible += 1
        lib.fc_counter_feed(counter, ctypes.byref(sample))

    stats = lib.fc_counter_stats(counter).contents
    elapsed = time.time() - t0
    expected = DURATION_S / 3.0  # mock 正弦 3s/次

    lats.sort()
    p95 = lats[int(len(lats) * 0.95)] if lats else 0
    print(f"  info sent={sent} visible={visible} elapsed={elapsed:.1f}s "
          f"lat p50={lats[len(lats)//2]:.1f}ms p95={p95:.1f}ms")
    print(f"  info reps={stats.reps} expected≈{expected:.1f} "
          f"stage={stats.stage} angle={stats.angle_smooth:.1f}")

    ok_reps = abs(stats.reps - expected) <= 1.5
    ok_feed = visible == sent and sent >= DURATION_S * 1000 / CADENCE_MS - 3
    print(f"  {'ok  ' if ok_reps else 'FAIL'} reps within tolerance")
    print(f"  {'ok  ' if ok_feed else 'FAIL'} all frames delivered at device cadence")
    if ok_reps and ok_feed:
        print("ALL PASS")
        return 0
    print("FAILED")
    return 1


if __name__ == "__main__":
    sys.exit(main())
