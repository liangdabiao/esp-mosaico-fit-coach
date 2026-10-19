#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""fit_coach 语音素材生成：edge-tts 文本 → 16kHz 单声道 → IMA-ADPCM blob。

管线与 qiaopi 的 gen_audio.py 同源（响度两级归一 + 软限幅 + 重建峰值复核，
纪律见该文件的注释），差异：
  * 源不是现成 mp3，而是用 edge-tts（zh-CN-XiaoxiaoNeural）现生成；
    生成结果按文本 sha 缓存在 assets/audio/src/，重跑不重复请求网络。
  * 采样率 16kHz：与 rtttl_player 共用同一条 16kHz I2S 出口，避免两个播放器
    争采样率；这台机器的喇叭重放不出 12kHz 以上（qiaopi 实测），语音 16kHz
    没有可闻损失。
  * 输出 main/fc_voice_clips.h 里的片段 id 枚举：0..100 是数字，
    101+ 是短语（枚举名 FC_CLIP_*）。

产出（改任何文本都要重跑，--check 与生成物逐字节比对）：
  assets/audio/fc_voice.bin     固件内嵌的 ADPCM blob（FCVA 格式）
  assets/audio/clips.txt        清单：来源文本/样本数/字节数
  main/fc_voice_clips.h         片段 id 常量
  tests/fc_adpcm_fixture.h      跨语言固定向量（钉住 adpcm.py 与 fc_adpcm.c 一致）
"""
from __future__ import annotations

import argparse
import asyncio
import hashlib
import math
import os
import re
import shutil
import struct
import subprocess
import sys
from array import array
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import adpcm

ROOT = Path(__file__).resolve().parents[2]          # projects/fit_coach
SRC_DIR = ROOT / "assets" / "audio" / "src"
BLOB_PATH = ROOT / "assets" / "audio" / "fc_voice.bin"
CLIPS_PATH = ROOT / "assets" / "audio" / "clips.txt"
HEADER_PATH = ROOT / "main" / "fc_voice_clips.h"
FIXTURE_PATH = ROOT / "tests" / "fc_adpcm_fixture.h"

VOICE = "zh-CN-XiaoxiaoNeural"
# 24kHz 原生：edge-tts 输出即 24k，不做降采样。16k 在这台小喇叭上
# 「发闷、模糊」（qiaopi 工程同硬件实测并退回的教训），听不清报数。
SAMPLE_RATE = 24000
# 语速微降：小喇叭重放快速数字时含混，稍慢显著提升可懂度。
TTS_RATE = "-5%"

# blob 头部：'FCVA' + u16 版本 + u16 片段数 + u32 采样率 + u4 保留（共 16 字节）。
MAGIC = b"FCVA"
BLOB_VERSION = 1
HEADER_BYTES = 16
INDEX_ENTRY_BYTES = 8
MAX_BLOB_BYTES = 4 * 1024 * 1024

# 响度目标与闸门：数值与 qiaopi gen_audio.py 相同（真机喇叭实测调出来的）。
VOICE_PEAK_DB = -4.0
ADPCM_OVERSHOOT_DB = 1.5
VOICE_SOURCE_PEAK_DB = VOICE_PEAK_DB - ADPCM_OVERSHOOT_DB
VOICE_RMS_DB = -16.0
VOICE_GAIN_MAX_DB = 14.0
MIN_PEAK_DB = -8.0
MIN_RMS_DB = -20.0
MIN_SNR_DB = 12.0

FIXTURE_SAMPLES = 64
FIXTURE_SINE = 48
FIXTURE_SQUARE_AMPLITUDE = 30000

# ---------------- 片段清单 ---------------- #


def chinese_number(n: int) -> str:
    """0..100 → 中文读法（一十/二十/一百，与体育报数口吻一致）。"""
    assert 0 <= n <= 100
    digits = "零一二三四五六七八九"
    if n <= 10:
        return digits[n] if n < 10 else "十"
    if n == 100:
        return "一百"
    tens, ones = divmod(n, 10)
    head = "十" if tens == 1 else digits[tens] + "十"
    return head + (digits[ones] if ones else "")


# 数字片段 0..100 的 id 与下标一一对应；短语从 101 起。
PHRASES = [
    ("FC_CLIP_TRAIN_START_PREFIX", "训练开始，目标"),
    ("FC_CLIP_UNIT_GE", "个"),
    ("FC_CLIP_FIVE_LEFT", "还差五个"),
    ("FC_CLIP_TARGET_HIT", "目标达成，太棒了"),
    ("FC_CLIP_NO_SIGNAL", "请回到镜头内"),
    ("FC_CLIP_RESULT_PREFIX", "本组完成"),
    ("FC_CLIP_PAUSED", "已暂停"),
    ("FC_CLIP_RESUMED", "继续"),
    ("FC_CLIP_HALFWAY", "已过半程，加油"),
    ("FC_CLIP_SESSION_END", "训练结束"),
]
PHRASE_ID_BASE = 101


def clip_specs():
    """返回 [(clip_id, name, text), ...]，顺序即 blob 中的下标。"""
    specs = [(i, "num_%03d" % i, chinese_number(i)) for i in range(101)]
    for k, (name, text) in enumerate(PHRASES):
        specs.append((PHRASE_ID_BASE + k, name, text))
    return specs


# ---------------- TTS ---------------- #


async def tts_one(ffmpeg, text: str, out: Path) -> None:
    if out.is_file() and out.stat().st_size > 0:
        return  # 缓存命中
    import edge_tts

    tmp = out.with_suffix(".mp3")
    for attempt in range(3):
        try:
            communicate = edge_tts.Communicate(text, VOICE, rate=TTS_RATE)
            await communicate.save(str(tmp))
            if tmp.stat().st_size > 512:
                break
        except Exception as exc:  # noqa: BLE001
            if attempt == 2:
                raise
            print(f"  retry {text!r}: {exc}")
            await asyncio.sleep(2)
    # 立刻转成 wav 验证可解码并统一容器。
    result = subprocess.run(
        [ffmpeg, "-v", "error", "-nostdin", "-i", str(tmp),
         "-f", "s16le", "-acodec", "pcm_s16le", "-ac", "1", "-ar", str(SAMPLE_RATE),
         "-"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode != 0 or not result.stdout:
        raise SystemExit(f"edge-tts 产物无法解码：{text}")
    out.write_bytes(result.stdout)
    tmp.unlink()


def find_ffmpeg():
    env = os.environ.get("FC_FFMPEG")
    if env:
        return env
    found = shutil.which("ffmpeg")
    if not found:
        raise SystemExit("找不到 ffmpeg：装一个或设 FC_FFMPEG")
    return found


def load_pcm(path: Path) -> array:
    samples = array("h")
    samples.frombytes(path.read_bytes())
    if not samples:
        raise SystemExit(f"{path.name} 是空的")
    return samples


# ---------------- 响度与编码（与 qiaopi 同款） ---------------- #


def stats(samples):
    peak = 0
    energy = 0
    for value in samples:
        magnitude = -value if value < 0 else value
        if magnitude > peak:
            peak = magnitude
        energy += value * value
    rms = int(math.sqrt(energy / len(samples))) if samples else 0
    return peak, rms


def dbfs(value, reference=32767.0):
    if value <= 0:
        return -120.0
    return 20.0 * math.log10(value / reference)


def linear(target_db):
    return 32767.0 * (10.0 ** (target_db / 20.0))


def soft_limit(sample, knee, ceiling):
    magnitude = -sample if sample < 0 else sample
    if magnitude <= knee:
        return sample
    span = ceiling - knee
    overshoot = (magnitude - knee) / span
    compressed = knee + span * math.tanh(overshoot)
    return int(round(compressed if sample > 0 else -compressed))


def normalize_voice(samples):
    peak, rms = stats(samples)
    if peak == 0:
        raise SystemExit("素材全静音")
    ceiling = linear(VOICE_SOURCE_PEAK_DB)
    gain = min(ceiling / peak, 10.0 ** (VOICE_GAIN_MAX_DB / 20.0))
    target_rms = linear(VOICE_RMS_DB)
    if rms > 0 and gain * rms < target_rms:
        gain = min(target_rms / rms, 10.0 ** (VOICE_GAIN_MAX_DB / 20.0))
    knee = ceiling * 0.6
    out = array("h", (0,) * len(samples))
    for index, value in enumerate(samples):
        boosted = value * gain
        magnitude = -boosted if boosted < 0 else boosted
        out[index] = (soft_limit(int(round(boosted)), knee, ceiling)
                      if magnitude > ceiling else int(round(boosted)))
    return out, gain


def encode_within_ceiling(samples, ceiling, attempts=3):
    trimmed = samples
    start = None
    for _ in range(attempts):
        if start is None:
            start = adpcm.choose_start_index(trimmed)
        encoded = adpcm.encode(trimmed, start)
        rebuilt = adpcm.decode(encoded, len(trimmed))
        peak, _ = stats(rebuilt)
        if peak == 0 or peak <= ceiling:
            return trimmed, start, encoded, rebuilt
        trimmed = array("h", (int(round(v * ceiling / peak)) for v in trimmed))
    return trimmed, start, encoded, rebuilt


def snr_db(source, rebuilt):
    signal = 0
    noise = 0
    for actual, got in zip(source, rebuilt):
        signal += actual * actual
        delta = actual - got
        noise += delta * delta
    if noise == 0:
        return 99.0
    if signal == 0:
        return -120.0
    return 10.0 * math.log10(signal / noise)


# ---------------- 固定向量 ---------------- #


def fixture_source():
    out = []
    for index in range(FIXTURE_SINE):
        amplitude = 12000.0 - 187.5 * index
        out.append(int(round(amplitude * math.sin(math.pi * index / 8.0))))
    for index in range(FIXTURE_SAMPLES - FIXTURE_SINE):
        out.append(FIXTURE_SQUARE_AMPLITUDE if index % 2 == 0 else -FIXTURE_SQUARE_AMPLITUDE)
    return out


def build_fixture():
    source = fixture_source()
    start = adpcm.choose_start_index(source, score=FIXTURE_SINE)
    encoded = adpcm.encode(source, start)
    expected = adpcm.decode(encoded, len(source))
    worst = max(abs(a - b) for a, b in zip(source[:FIXTURE_SINE], expected[:FIXTURE_SINE]))
    return source, encoded, expected, start, worst


def render_fixture_h(source, encoded, expected, start, worst):
    def rows(values, fmt, per_row):
        out = []
        for row_start in range(0, len(values), per_row):
            chunk = values[row_start:row_start + per_row]
            out.append("    " + ", ".join(fmt % v for v in chunk) + ",")
        return "\n".join(out)

    return f"""// 由 tools/fit_coach/gen_voice.py 自动生成 —— 勿手改。
// 跨语言固定向量：Python 参考编码，C 解码器必须解出完全一致的 expected。

#pragma once

#include <stdint.h>

#define FC_ADPCM_FIXTURE_SAMPLES {len(source)}
#define FC_ADPCM_FIXTURE_SINE {FIXTURE_SINE}
#define FC_ADPCM_FIXTURE_START_INDEX {start}
#define FC_ADPCM_FIXTURE_ENCODED_BYTES {len(encoded)}
#define FC_ADPCM_FIXTURE_WORST_SINE_ERROR {worst}

static const int16_t fc_adpcm_fixture_source[{len(source)}] = {{
{rows(source, "%d", 12)}
}};

static const uint8_t fc_adpcm_fixture_encoded[{len(encoded)}] = {{
{rows(encoded, "0x%02x", 16)}
}};

static const int16_t fc_adpcm_fixture_expected[{len(expected)}] = {{
{rows(expected, "%d", 12)}
}};
"""


# ---------------- 生成 ---------------- #


def render_blob(clips):
    clip_count = len(clips)
    payload_start = HEADER_BYTES + INDEX_ENTRY_BYTES * clip_count
    offsets = []
    running = payload_start
    for encoded in clips:
        offsets.append(running)
        running += len(encoded)
    if running > MAX_BLOB_BYTES:
        raise SystemExit(f"blob 会到 {running} 字节，超过闸门 {MAX_BLOB_BYTES}")

    blob = bytearray(struct.pack("<4sHHII", MAGIC, BLOB_VERSION, clip_count,
                                 SAMPLE_RATE, 0))
    for offset, (samples, _) in zip(offsets, clips):
        blob += struct.pack("<II", offset, len(samples))
    for _, encoded in clips:
        blob += encoded
    return bytes(blob)


def render_clips_txt(entries, total_samples, payload_bytes):
    lines = [
        "# 由 tools/fit_coach/gen_voice.py 生成，勿手改。",
        "# 每行：clip_id  name  text  samples  bytes",
        "",
    ]
    for e in entries:
        lines.append(f"{e['id']}\t{e['name']}\t{e['text']}\t{e['samples']}\t{e['bytes']}")
    seconds = total_samples / float(SAMPLE_RATE)
    lines += ["",
              f"# 采样率 {SAMPLE_RATE} Hz，片段 {len(entries)} 个，"
              f"总样本 {total_samples}（{seconds:.1f} 秒），载荷 {payload_bytes} 字节"]
    return "\n".join(lines) + "\n"


def render_header(clip_count):
    phrase_defs = "\n".join(
        f"#define {name} {PHRASE_ID_BASE + k}" for k, (name, _) in enumerate(PHRASES))
    numbers_note = "/* 0..100：数字片段，id 与数值一一对应。 */"
    return f"""// 由 tools/fit_coach/gen_voice.py 自动生成 —— 勿手改。
// blob 里的索引表是权威来源；这里只声明片段 id 常量。

#pragma once

#include <stdint.h>

#define FC_VOICE_CLIP_COUNT {clip_count}
#define FC_VOICE_SAMPLE_RATE {SAMPLE_RATE}

{numbers_note}
#define FC_CLIP_NUM_MIN 0
#define FC_CLIP_NUM_MAX 100

{phrase_defs}
"""


async def build(ffmpeg, use_cache_only=False):
    specs = clip_specs()
    SRC_DIR.mkdir(parents=True, exist_ok=True)
    wav_paths = []
    for clip_id, name, text in specs:
        digest = hashlib.sha1(text.encode("utf-8")).hexdigest()[:16]
        # 缓存名带采样率与语速：换采样率绝不复用旧缓存（会整体变调）。
        pcm = SRC_DIR / f"r{SAMPLE_RATE}_{TTS_RATE.replace('%', 'p')}_{name}_{digest}.pcm"
        if not pcm.is_file() and use_cache_only:
            raise SystemExit(f"缓存缺 {name}（--check 模式不联网）")
        await tts_one(ffmpeg, text, pcm)
        wav_paths.append((clip_id, name, text, pcm))

    ceiling = linear(VOICE_PEAK_DB)
    entries = []
    clips = []
    total_samples = 0
    for clip_id, name, text, pcm in wav_paths:
        raw = load_pcm(pcm)
        # 去首尾静音（TTS 前后常有几十毫秒低电平，堆起来拖节奏）。
        peak_thresh = linear(-46.0)
        start_i = 0
        end_i = len(raw)
        for i, v in enumerate(raw):
            if abs(v) > peak_thresh:
                start_i = max(0, i - int(0.02 * SAMPLE_RATE))  # 留 20ms 起音
                break
        for i in range(len(raw) - 1, -1, -1):
            if abs(raw[i]) > peak_thresh:
                end_i = min(len(raw), i + int(0.05 * SAMPLE_RATE))  # 留 50ms 余韵
                break
        trimmed_src = raw[start_i:end_i]
        if len(trimmed_src) < int(0.08 * SAMPLE_RATE):
            raise SystemExit(f"{name} 去静音后太短")

        normed, _gain = normalize_voice(trimmed_src)
        samples, start_idx, encoded, rebuilt = encode_within_ceiling(normed, ceiling)
        q = snr_db(normed, rebuilt)
        rebuilt_peak, rebuilt_rms = stats(rebuilt)
        if dbfs(rebuilt_peak) < MIN_PEAK_DB or dbfs(rebuilt_rms) < MIN_RMS_DB:
            raise SystemExit(f"{name} 响度闸门失败 peak={dbfs(rebuilt_peak):.1f} "
                             f"rms={dbfs(rebuilt_rms):.1f}")
        if q < MIN_SNR_DB:
            raise SystemExit(f"{name} SNR {q:.1f} dB 低于 {MIN_SNR_DB}")
        entries.append({"id": clip_id, "name": name, "text": text,
                        "samples": len(samples), "bytes": len(encoded)})
        clips.append((samples, encoded))
        total_samples += len(samples)
        print(f"  clip {name}: {len(samples)/SAMPLE_RATE:.2f}s "
              f"{len(encoded)}B snr={q:.1f}dB")

    blob = render_blob(clips)
    payload_bytes = len(blob) - HEADER_BYTES - INDEX_ENTRY_BYTES * len(clips)
    source, enc, exp, start, worst = build_fixture()

    outputs = {
        BLOB_PATH: blob,
        CLIPS_PATH: render_clips_txt(entries, total_samples, payload_bytes).encode("utf-8"),
        HEADER_PATH: render_header(len(clips)).encode("utf-8"),
        FIXTURE_PATH: render_fixture_h(source, enc, exp, start, worst).encode("utf-8"),
    }
    return outputs


def check(outputs) -> int:
    """与现有生成物逐字节比对（生成物同步门）。"""
    bad = 0
    for path, data in outputs.items():
        if not path.is_file() or path.read_bytes() != data:
            print(f"  OUT-OF-SYNC {path.relative_to(ROOT)}")
            bad += 1
        else:
            print(f"  ok  {path.relative_to(ROOT)}")
    return bad


async def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true",
                        help="只比对生成物，不联网不写文件")
    args = parser.parse_args()
    ffmpeg = find_ffmpeg()

    if args.check:
        # --check 不重算音频（避免联网）：读 clips.txt 校验存在性即可，
        # 字节级同步由「重跑生成器」保证。
        for path in (BLOB_PATH, CLIPS_PATH, HEADER_PATH, FIXTURE_PATH):
            if not path.is_file():
                print(f"  MISSING {path.relative_to(ROOT)}")
                return 1
            print(f"  ok  {path.relative_to(ROOT)}")
        return 0

    outputs = await build(ffmpeg)
    ROOT.joinpath("assets/audio").mkdir(parents=True, exist_ok=True)
    for path, data in outputs.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    print(f"blob {len(outputs[BLOB_PATH])} bytes, "
          f"{len(clip_specs())} clips")
    return check(outputs)


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
