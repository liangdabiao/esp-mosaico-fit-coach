// 由 tools/fit_coach/gen_voice.py 自动生成 —— 勿手改。
// blob 里的索引表是权威来源；这里只声明片段 id 常量。

#pragma once

#include <stdint.h>

#define FC_VOICE_CLIP_COUNT 111
#define FC_VOICE_SAMPLE_RATE 16000

/* 0..100：数字片段，id 与数值一一对应。 */
#define FC_CLIP_NUM_MIN 0
#define FC_CLIP_NUM_MAX 100

#define FC_CLIP_TRAIN_START_PREFIX 101
#define FC_CLIP_UNIT_GE 102
#define FC_CLIP_FIVE_LEFT 103
#define FC_CLIP_TARGET_HIT 104
#define FC_CLIP_NO_SIGNAL 105
#define FC_CLIP_RESULT_PREFIX 106
#define FC_CLIP_PAUSED 107
#define FC_CLIP_RESUMED 108
#define FC_CLIP_HALFWAY 109
#define FC_CLIP_SESSION_END 110
