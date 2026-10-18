// SPDX-License-Identifier: Apache-2.0
// main/fc_sfx.h —— 界面音效接口（RTTTL 合成，无素材文件）。
//
// 设备实现走 ES8311 codec（fc_sfx_device.c + rttl_player.c）；
// 宿主实现为静音桩（fc_sfx_host.c）。音效开关在应用层（game.c）判断。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FC_TONE_REP = 0,   /* 完成一次仰卧起坐：短促上行 */
    FC_TONE_TARGET,    /* 达标：上行琶音 */
    FC_TONE_NAV,       /* 页面切换/按钮 */
    FC_TONE_BACK,      /* 返回 */
    FC_TONE_ERR,       /* 无效操作 */
    FC_TONE_COUNT,
} fc_tone_t;

/* 启动播放任务；codec 不可用时返回非零，应用继续静音运行。 */
int fc_sfx_start(void);
void fc_sfx_stop(void);
void fc_sfx_play(fc_tone_t tone);

#ifdef __cplusplus
}
#endif
