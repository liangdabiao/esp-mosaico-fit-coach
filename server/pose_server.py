#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""fit-coach 姿态服务器：设备推 JPEG 帧，服务器回传肩-髋-膝角度与状态。

算法与需求文档（Python+MediaPipe 仰卧起坐计数）一致：
  * MediaPipe Pose 提取 33 关键点；
  * 左肩(11) - 左髋(23) - 左膝(25) 三点夹角；
  * 状态机（up=躺平/down=坐起）与计数在设备端完成（fc_count.c），
    服务器只做无状态的逐帧判定 —— 设备断线重连不丢会话。

端点：
  POST /frame   body=JPEG，头 X-FC-TS=设备采集时间戳(ms)
                -> {"angle_deg": float|-1, "stage": "up"|"down"|null,
                    "visible": bool, "t_ms": 回显}
  GET  /health  -> {"ok": true, "mode": "mediapipe"|"mock", "frames": n}

用法：
  uvicorn pose_server:app --host 0.0.0.0 --port 8000
  MOCK 模式（不装 mediapipe 也能联调设备端）：
  FC_MOCK=1 uvicorn pose_server:app --host 0.0.0.0 --port 8000
"""
from __future__ import annotations

import math
import os
import time

# ---- 屏蔽 MediaPipe / TF 刷屏日志（必须在 import mediapipe 之前，需求文档坑位）----
os.environ.setdefault("GLOG_minloglevel", "2")
os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "3")
os.environ.setdefault("ABSL_LOGGING_LEVEL", "error")

import cv2  # noqa: E402
from fastapi import FastAPI, Header, Request, Response  # noqa: E402

MOCK = os.environ.get("FC_MOCK", "") not in ("", "0")

# 设备端计数阈值（度）：仅用于 stage 标签；设备端也有一份可调阈值。
UP_THRESH = 110
DOWN_THRESH = 55

app = FastAPI(title="fit-coach pose server")

_state = {"frames": 0, "t0": time.time()}


class PoseDetector:
    """需求文档 PoseDetector 的服务器版：只保留推理与角度，去掉绘制。"""

    def __init__(self, mode: bool = False, smooth: bool = True,
                 detection_con: float = 0.5, track_con: float = 0.5) -> None:
        import mediapipe as mp  # 延迟导入：mock 模式不依赖

        self.mp_pose = mp.solutions.pose
        self.pose = self.mp_pose.Pose(
            static_image_mode=mode,
            smooth_landmarks=smooth,
            min_detection_confidence=detection_con,
            min_tracking_confidence=track_con,
        )

    def landmarks(self, img):
        results = self.pose.process(cv2.cvtColor(img, cv2.COLOR_BGR2RGB))
        if not results.pose_landmarks:
            return None
        h, w, _ = img.shape
        pts = []
        for lm in results.pose_landmarks.landmark:
            pts.append((lm.x * w, lm.y * h))
        return pts

    @staticmethod
    def angle(p1, p2, p3) -> float:
        """p1-p2-p3 夹角（度），限制到 0..180（需求文档 findAngle 逻辑）。"""
        x1, y1 = p1
        x2, y2 = p2
        x3, y3 = p3
        a = math.degrees(
            math.atan2(y3 - y2, x3 - x2) - math.atan2(y1 - y2, x1 - x2)
        )
        if a < 0:
            a += 360
        if a > 180:
            a = 360 - a
        return a


_detector: PoseDetector | None = None


def _decode_jpeg(data: bytes):
    """JPEG 字节 -> BGR 图像；空/坏数据返回 None（统一走 400）。
    mediapipe 链路的 numpy 在此延迟引入。"""
    if not data:
        return None
    import numpy as np

    try:
        return cv2.imdecode(np.frombuffer(data, dtype="uint8"), cv2.IMREAD_COLOR)
    except cv2.error:
        return None


def get_detector() -> PoseDetector | None:
    global _detector
    if MOCK:
        return None
    if _detector is None:
        _detector = PoseDetector()
    return _detector


def mock_angle() -> float:
    """MOCK 模式：3 秒周期的 150↔40 余弦，与设备演示模式同相位。"""
    t = (time.time() - _state["t0"]) % 3.0
    w = 2 * math.pi * t / 3.0
    return 95.0 + 55.0 * math.cos(w)


@app.post("/frame")
async def frame(request: Request, x_fc_ts: int = Header(default=0)) -> Response:
    body = await request.body()
    _state["frames"] += 1

    if MOCK:
        angle = mock_angle()
    else:
        detector = get_detector()
        if detector is None:
            return Response(
                content='{"error":"mediapipe unavailable"}',
                status_code=503,
                media_type="application/json",
            )
        img = _decode_jpeg(body)
        if img is None:
            return Response(
                content='{"error":"bad jpeg"}',
                status_code=400,
                media_type="application/json",
            )
        pts = detector.landmarks(img)
        if pts is None or len(pts) <= 25:
            return Response(
                content='{"angle_deg":-1,"stage":null,"visible":false,'
                        f'"t_ms":{x_fc_ts}}}',
                media_type="application/json",
            )
        # 左肩(11) - 左髋(23) - 左膝(25)
        angle = PoseDetector.angle(pts[11], pts[23], pts[25])

    stage = "up" if angle > UP_THRESH else ("down" if angle < DOWN_THRESH else None)
    import json

    return Response(
        content=json.dumps(
            {"angle_deg": round(float(angle), 1), "stage": stage,
             "visible": True, "t_ms": x_fc_ts},
            separators=(",", ":"),
        ),
        media_type="application/json",
    )


@app.get("/health")
def health() -> Response:
    import json

    return Response(
        content=json.dumps(
            {"ok": True, "mode": "mock" if MOCK else "mediapipe",
             "frames": _state["frames"]},
        ),
        media_type="application/json",
    )
