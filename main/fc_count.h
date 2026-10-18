// SPDX-License-Identifier: Apache-2.0
// main/fc_count.h —— 仰卧起坐计数内核（纯 C11，设备/宿主共用）。
//
// 对应需求文档（D:\esp-mo\姿势检测.txt）的状态机算法：
//   角度 > UP_THRESH  → 躺平（stage=UP）
//   角度 < DOWN_THRESH 且上一状态为 UP → 坐起完成一次，计数 +1
// 阈值之间的区间保持原状态（迟滞），防止阈值附近抖动重复计数。
//
// 两种输入形态共用同一内核：
//   * 数值角度样本（MediaPipe 服务器 / IMU 扩展）：angle_deg >= 0，
//     内核先做 EMA 平滑再判状态；
//   * 状态标签样本（VLM 备用模式）：angle_deg < 0、stage 由云端判定，
//     内核做连续两次一致的去抖后再进状态机。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 默认阈值：与需求文档一致（肩-髋-膝三点夹角，度）。 */
#define FC_UP_THRESH_DEFAULT 110
#define FC_DOWN_THRESH_DEFAULT 55
/* 相邻两次计数的最小间隔（毫秒），拒绝残余抖动造成的连计。 */
#define FC_MIN_REP_INTERVAL_MS 600
/* EMA 平滑系数（0..1，越大越跟随原始值）。 */
#define FC_ANGLE_EMA_ALPHA 0.4f
/* 状态标签去抖需要的连续一致次数。 */
#define FC_LABEL_DEBOUNCE 2

typedef enum {
    FC_STAGE_UNKNOWN = 0,
    FC_STAGE_UP,   /* 躺平（躯干与大腿夹角大） */
    FC_STAGE_DOWN, /* 坐起（躯干蜷曲） */
} fc_stage_t;

typedef struct {
    float angle_deg;  /* >=0 有效数值角度；<0 表示本样本只有状态标签 */
    fc_stage_t stage; /* 采样状态（标签模式直给；数值模式内核自判，忽略） */
    bool visible;     /* 画面中是否检测到人（false 时只更新可见性，不喂状态机） */
    float conf;       /* 置信度 0..1；低于门槛的样本被丢弃 */
    uint32_t t_ms;    /* 采集时间戳（设备侧单调时钟，仅用于统计） */
} fc_sample_t;

/* 采样置信度门槛：低于它的样本不参与状态判定（VLM 误判兜底）。 */
#define FC_MIN_CONF 0.5f

typedef struct {
    uint32_t reps;         /* 完成次数 */
    fc_stage_t stage;      /* 平滑后的当前状态 */
    float angle_smooth;    /* 平滑角度（仅显示用；<0 表示尚无有效数值） */
    float angle_last;      /* 最近一个原始角度（<0 无）；状态判定用它 */
    uint32_t last_rep_ms;  /* 上一次计数时刻（0 无）；计数后由回调播报 */
    uint32_t rep_just_now; /* 自上次读取以来新增的次数（供 UI/音效消费后清零） */
    uint32_t last_sample_ms; /* 最近一次可见有效样本时刻 */
    uint32_t samples_seen; /* 有效样本总数（调试/联调用） */
} fc_stats_t;

typedef struct fc_counter fc_counter_t;

fc_counter_t *fc_counter_new(int up_thresh, int down_thresh);
void fc_counter_del(fc_counter_t *counter);

/* 会话重置：计数/统计清零，阈值与平滑状态复位。 */
void fc_counter_reset(fc_counter_t *counter);
void fc_counter_set_thresholds(fc_counter_t *counter, int up_thresh, int down_thresh);

/* 喂一个样本。样本的 t_ms 应单调不减（乱序样本按到达顺序处理，不重排）。 */
void fc_counter_feed(fc_counter_t *counter, const fc_sample_t *sample);

/* 读取统计（只读视图，指针生命周期与 counter 相同）。 */
const fc_stats_t *fc_counter_stats(const fc_counter_t *counter);

/* 取走自上次调用以来新增的计数次数（清零），供 UI/音效消费。 */
uint32_t fc_counter_take_reps(fc_counter_t *counter);

#ifdef __cplusplus
}
#endif
