// SPDX-License-Identifier: Apache-2.0
// main/fc_voice.h —— 语音反馈接口（设备播放 / 宿主静音桩）。
//
// 运动者躺着看屏幕——训练中的反馈主通道是语音。素材是构建期由
// tools/fit_coach/gen_voice.py 生成的 ADPCM blob（数字 0..100 + 短语），
// 16kHz 单声道，与 rtttl_player 共用同一条 I2S 出口（时序互斥，不混音）。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 片段 id：0..100 数字；短语见生成的 main/fc_voice_clips.h。 */
#define FC_CLIP_NUM_MIN 0
#define FC_CLIP_NUM_MAX 100

/* 启动（设备：codec/解码/任务；宿主：空实现）。返回 0 可用。
 * codec_already_open：rttl_player 是否已按 16kHz 打开 codec（true 时不重开）。 */
int fc_voice_start(bool codec_already_open);
void fc_voice_stop(void);

/* 语音是否可用（宿主恒 false，调用方可据此回退到提示音）。 */
bool fc_voice_available(void);

/* 入队播放。序列版本用于「训练开始，目标X个」这类拼接。
 * 队列满时丢弃最旧；正在播放的不打断（片段都很短）。 */
void fc_voice_play(int clip_id);
void fc_voice_play2(int a, int b);
void fc_voice_play3(int a, int b, int c);

/* 丢弃未播的排队片段（如会话重置）。 */
void fc_voice_flush(void);

#ifdef __cplusplus
}
#endif
