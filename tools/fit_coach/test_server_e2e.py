#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""fit-coach 服务器端到端联调（宿主，纯标准库）。

前置：服务器已启动（mediapipe 或 mock 模式）。
覆盖：/health、无人帧、真人帧、坏 JPEG、空 body、X-FC-TS 回显、
      连续 30 帧延迟分布（p50/p95/max）与角度一致性。

用法（server/ 目录）：
  python ../tools/fit_coach/test_server_e2e.py [base_url]
默认 http://127.0.0.1:8000
"""
from __future__ import annotations

import json
import statistics
import sys
import time
import urllib.error
import urllib.request

BASE = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:8000"
failures = 0


def check(name: str, ok: bool, detail: str = "") -> None:
    global failures
    print(f"  {'ok  ' if ok else 'FAIL'} {name} {detail}")
    if not ok:
        failures += 1


def post_frame(body: bytes, ts: int):
    req = urllib.request.Request(
        BASE + "/frame", data=body, method="POST",
        headers={"Content-Type": "image/jpeg", "X-FC-TS": str(ts)})
    with urllib.request.urlopen(req, timeout=30) as resp:
        return resp.status, json.loads(resp.read().decode())


def main() -> int:
    print(f"== fit-coach server e2e against {BASE} ==")

    # health
    with urllib.request.urlopen(BASE + "/health", timeout=10) as r:
        health = json.loads(r.read().decode())
    check("health", health.get("ok") is True,
          f"mode={health.get('mode')} frames={health.get('frames')}")
    mode = health.get("mode")

    noperson = open("test_noperson.jpg", "rb").read()
    try:
        person = open("test_person.jpg", "rb").read()
    except FileNotFoundError:
        person = None

    # no-person frame
    status, data = post_frame(noperson, 111)
    check("no-person -> invisible", status == 200 and data.get("visible") is False,
          str(data))
    check("ts echoed", data.get("t_ms") == 111, f"t_ms={data.get('t_ms')}")

    # bad jpeg
    try:
        status, data = post_frame(b"\x00" * 1000, 222)
        check("bad jpeg -> 400", status == 400, f"status={status}")
    except urllib.error.HTTPError as e:
        check("bad jpeg -> 400", e.code == 400, f"status={e.code}")

    # empty body
    try:
        status, data = post_frame(b"", 333)
        check("empty body -> 400", status == 400, f"status={status}")
    except urllib.error.HTTPError as e:
        check("empty body -> 400", e.code == 400, f"status={e.code}")

    if mode != "mediapipe":
        print("  (mock mode: person-frame checks skipped)")
        return report()

    if person is None:
        print("  SKIP person frame (no test_person.jpg)")
        return report()

    # person frame (first request also warms the model)
    t0 = time.perf_counter()
    status, data = post_frame(person, 444)
    cold_ms = (time.perf_counter() - t0) * 1000
    check("person -> visible", status == 200 and data.get("visible") is True,
          f"cold={cold_ms:.0f}ms {data}")

    # sustained: 30 frames, latency + angle consistency
    lats, angles, vis = [], [], 0
    for i in range(30):
        t0 = time.perf_counter()
        status, data = post_frame(person, 1000 + i)
        lats.append((time.perf_counter() - t0) * 1000)
        if data.get("visible"):
            vis += 1
            if isinstance(data.get("angle_deg"), (int, float)) and data["angle_deg"] >= 0:
                angles.append(data["angle_deg"])
    lats.sort()
    p50 = lats[len(lats) // 2]
    p95 = lats[int(len(lats) * 0.95)]
    print(f"  info latency p50={p50:.0f}ms p95={p95:.0f}ms max={lats[-1]:.0f}ms "
          f"visible={vis}/30 angles={len(angles)}")
    check("sustained >= 3fps capable", p95 < 333,
          f"p95={p95:.0f}ms < 333ms")
    check("person mostly visible", vis >= 27, f"{vis}/30")
    if angles:
        check("angle stable & sane",
              10 <= statistics.mean(angles) <= 175 and statistics.pstdev(angles) < 10,
              f"mean={statistics.mean(angles):.1f} sd={statistics.pstdev(angles):.1f}")
        print(f"  info angle mean={statistics.mean(angles):.1f} "
              f"sd={statistics.pstdev(angles):.1f}")

    return report()


def report() -> int:
    if failures:
        print(f"FAILED: {failures}")
        return 1
    print("ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
