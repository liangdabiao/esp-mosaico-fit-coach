// SPDX-License-Identifier: Apache-2.0
// main/fc_demo.c —— 合成角度流实现。
#include "fc_demo.h"

#include <math.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* 一个完整仰卧起坐周期（毫秒）：躺→坐→躺。 */
#define FC_DEMO_PERIOD_MS 3000
/* 角度摆幅区间：峰 150（躺平），谷 40（坐起），围绕需求文档阈值设计。 */
#define FC_DEMO_ANGLE_MAX 150.0f
#define FC_DEMO_ANGLE_MIN 40.0f

struct fc_demo {
    uint32_t base_ms; /* 相位零点 */
};

fc_demo_t *fc_demo_new(void)
{
    fc_demo_t *d = calloc(1, sizeof(*d));
    return d;
}

void fc_demo_del(fc_demo_t *demo)
{
    free(demo);
}

void fc_demo_reset(fc_demo_t *demo)
{
    if (demo) demo->base_ms = 0;
}

float fc_demo_angle_at(fc_demo_t *demo, uint32_t t_ms)
{
    if (!demo) return FC_DEMO_ANGLE_MAX;
    const uint32_t phase = (t_ms - demo->base_ms) % FC_DEMO_PERIOD_MS;
    /* 余弦从峰值出发：0ms=躺平(150)，半程=坐起(40)，全程平滑往返。 */
    const float w = 2.0f * (float)M_PI * (float)phase / (float)FC_DEMO_PERIOD_MS;
    const float cosv = cosf(w);
    return (FC_DEMO_ANGLE_MAX + FC_DEMO_ANGLE_MIN) * 0.5f +
           (FC_DEMO_ANGLE_MAX - FC_DEMO_ANGLE_MIN) * 0.5f * cosv;
}
