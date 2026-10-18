// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "fc_count.h"
#include "fc_store.h"

typedef struct game_t *game_handle_t;
typedef struct {
    uint16_t width;
    uint16_t height;
} game_config_t;

/* 页面枚举（宿主回放校验用）。 */
typedef enum {
    FC_PAGE_HOME = 0,
    FC_PAGE_TRAIN,
    FC_PAGE_RESULT,
    FC_PAGE_SETTINGS,
    FC_PAGE_HISTORY,
} fc_page_t;

/* 链路状态（由设备 runtime 上报；演示模式恒 OK）。 */
typedef enum {
    FC_LINK_OFF = 0,    /* 未联网/未配置 */
    FC_LINK_CONNECTING, /* 连接中 */
    FC_LINK_OK,         /* 样本流动 */
    FC_LINK_ERROR,      /* 失败（服务器不可达/无响应） */
} fc_link_t;

/* 供宿主回放校验的可观测快照。 */
typedef struct {
    uint32_t tick;
    int32_t pointer_x;
    int32_t pointer_y;
    bool pointer_down;
    uint8_t page;      /* fc_page_t */
    uint32_t reps;     /* 当前组计数 */
    uint8_t stage;     /* 0 未知 / 1 躺平 / 2 坐起 */
    int16_t angle;     /* 最近平滑角度（-1 无） */
    bool streaming;    /* 训练中且未暂停 */
    uint8_t link;      /* fc_link_t */
    uint8_t mode;      /* FC_MODE_* */
    uint8_t target;    /* 目标个数 */
    uint32_t session_ms; /* 本组已进行时长 */
} game_snapshot_t;

/* 便携核心：设备与宿主同一份。一个循环独占实例并在该循环上调用这些方法。 */
bool game_create(const game_config_t *config, game_handle_t *ret_handle);
void game_delete(game_handle_t handle);
void game_reset(game_handle_t handle);
void game_set_paused(game_handle_t handle, bool paused);
void game_set_pointer(game_handle_t handle, int32_t x, int32_t y, bool pressed);
void game_update(game_handle_t handle);
bool game_render(game_handle_t handle);
game_snapshot_t game_read(game_handle_t handle);

/* ---- 设备 runtime → game 的输入口（SPSC 安全：runtime 任务生产，引擎任务消费）---- */

/* 喂一个姿态样本（服务器/VLM 回传）。队列满时丢弃最旧样本。 */
void game_feed_pose(game_handle_t handle, const fc_sample_t *sample);

/* 上报链路状态。 */
void game_set_link(game_handle_t handle, uint8_t link);

/* runtime 轮询：当前是否需要摄像头采集（训练页且未暂停且非演示模式）。 */
bool game_wants_capture(game_handle_t handle);

/* runtime 读取当前设置（含模式/服务器地址；返回内部指针，勿跨会话保存）。 */
const fc_settings_t *game_settings_view(game_handle_t handle);
