#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Lightweight, asynchronous RTTTL playback for short UI/game effects. */
esp_err_t rtttl_player_start(void);
/* Cancels playback; the worker remains idle so codec locks are never abandoned. */
void rtttl_player_stop(void);
/* Starts immediately and replaces an older queued/playing effect. */
esp_err_t rtttl_player_play(const char *song);
/* Lets microphone-driven features suppress speaker feedback. */
bool rtttl_player_is_playing(void);

/* 等待 player_task 完成 codec 初始化（最多 timeout_ms），返回 speaker 是否可用。
 * 调用方必须先 rtttl_player_start()。没有它，紧随其后的第二个 codec 初始化
 * 会与 player_task 竞态抢 I2S0，输家整机无声（真机日志定位过）。 */
bool rtttl_player_wait_ready(uint32_t timeout_ms);
