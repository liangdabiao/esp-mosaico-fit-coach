// SPDX-License-Identifier: Apache-2.0
// main/fc_demo.h —— 演示模式的合成角度流（纯 C11）。
//
// 无服务器/无摄像头时（模拟器验收、设备演示模式），用正弦发生器合成
// 「肩-髋-膝夹角」时间序列：在 DOWN_THRESH 之下与 UP_THRESH 之上往复，
// 周期约 3 秒/次，模拟标准仰卧起坐节奏，驱动整套 UI/计数链路。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fc_demo fc_demo_t;

fc_demo_t *fc_demo_new(void);
void fc_demo_del(fc_demo_t *demo);

/* 重置相位与计数起点。 */
void fc_demo_reset(fc_demo_t *demo);

/* 推进到 t_ms 时刻，返回该时刻的合成角度（度）。
 * visible 恒为 true；conf 恒为 1。 */
float fc_demo_angle_at(fc_demo_t *demo, uint32_t t_ms);

#ifdef __cplusplus
}
#endif
