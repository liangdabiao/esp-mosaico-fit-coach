// SPDX-License-Identifier: Apache-2.0
// main/fc_count.c —— 计数内核实现。纯 C11，无 IDF/平台依赖，宿主单测覆盖。
#include "fc_count.h"

#include <stdlib.h>

struct fc_counter {
    int up_thresh;
    int down_thresh;
    fc_stats_t stats;
    /* 标签去抖：连续一致计数与候选标签 */
    fc_stage_t label_candidate;
    uint8_t label_run;
};

fc_counter_t *fc_counter_new(int up_thresh, int down_thresh)
{
    fc_counter_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->up_thresh = up_thresh > 0 ? up_thresh : FC_UP_THRESH_DEFAULT;
    c->down_thresh = down_thresh > 0 ? down_thresh : FC_DOWN_THRESH_DEFAULT;
    if (c->down_thresh >= c->up_thresh) {
        c->down_thresh = FC_DOWN_THRESH_DEFAULT;
        c->up_thresh = FC_UP_THRESH_DEFAULT;
    }
    c->stats.angle_smooth = -1.0f;
    c->stats.angle_last = -1.0f;
    return c;
}

void fc_counter_del(fc_counter_t *counter)
{
    free(counter);
}

void fc_counter_reset(fc_counter_t *counter)
{
    if (!counter) return;
    uint32_t samples = counter->stats.samples_seen;
    fc_stats_t *st = &counter->stats;
    st->reps = 0;
    st->stage = FC_STAGE_UNKNOWN;
    st->angle_smooth = -1.0f;
    st->angle_last = -1.0f;
    st->last_rep_ms = 0;
    st->rep_just_now = 0;
    st->last_sample_ms = 0;
    st->samples_seen = samples; /* 历史累计保留，便于联调观察 */
    counter->label_candidate = FC_STAGE_UNKNOWN;
    counter->label_run = 0;
}

void fc_counter_set_thresholds(fc_counter_t *counter, int up_thresh, int down_thresh)
{
    if (!counter || up_thresh <= 0 || down_thresh <= 0 || down_thresh >= up_thresh) return;
    counter->up_thresh = up_thresh;
    counter->down_thresh = down_thresh;
}

/* 由数值角度判定阶段（迟滞：阈值区间内保持原状态）。 */
static fc_stage_t stage_from_angle(float angle, int up, int down, fc_stage_t prev)
{
    if (angle > (float)up) return FC_STAGE_UP;
    if (angle < (float)down) return FC_STAGE_DOWN;
    return prev;
}

void fc_counter_feed(fc_counter_t *counter, const fc_sample_t *sample)
{
    if (!counter || !sample) return;
    fc_stats_t *st = &counter->stats;

    if (!sample->visible) return; /* 不可见样本不推进任何状态 */
    if (sample->conf < FC_MIN_CONF) return;
    st->last_sample_ms = sample->t_ms;
    st->samples_seen++;

    fc_stage_t next = st->stage;

    if (sample->angle_deg >= 0.0f) {
        /* 数值模式：状态判定用原始角度 + 迟滞带（与需求文档一致，
         * 55..110 的宽间隙本身就是防抖；EMA 只做显示平滑，避免滞后
         * 拖慢状态切换、漏掉短促的坐起相位）。 */
        st->angle_last = sample->angle_deg;
        st->angle_smooth = st->angle_smooth < 0.0f
                               ? sample->angle_deg
                               : st->angle_smooth + FC_ANGLE_EMA_ALPHA *
                                       (sample->angle_deg - st->angle_smooth);
        next = stage_from_angle(sample->angle_deg, counter->up_thresh,
                                counter->down_thresh, st->stage);
    } else {
        /* 标签模式：连续一致去抖后才切换。 */
        if (sample->stage == FC_STAGE_UNKNOWN) return;
        if (sample->stage == counter->label_candidate) {
            if (counter->label_run < 255) counter->label_run++;
        } else {
            counter->label_candidate = sample->stage;
            counter->label_run = 1;
        }
        if (counter->label_run >= FC_LABEL_DEBOUNCE) next = counter->label_candidate;
    }

    /* 状态机计数：UP → DOWN 完成一次仰卧起坐（与需求文档一致）。 */
    if (next == FC_STAGE_DOWN && st->stage == FC_STAGE_UP) {
        const uint32_t since = st->last_rep_ms == 0 ? FC_MIN_REP_INTERVAL_MS + 1u
                                                    : sample->t_ms - st->last_rep_ms;
        if (since >= (uint32_t)FC_MIN_REP_INTERVAL_MS) {
            st->reps++;
            st->rep_just_now++;
            st->last_rep_ms = sample->t_ms;
        }
    }
    st->stage = next;
}

const fc_stats_t *fc_counter_stats(const fc_counter_t *counter)
{
    return counter ? &counter->stats : NULL;
}

uint32_t fc_counter_take_reps(fc_counter_t *counter)
{
    if (!counter) return 0;
    const uint32_t reps = counter->stats.rep_just_now;
    counter->stats.rep_just_now = 0;
    return reps;
}
