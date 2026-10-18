// SPDX-License-Identifier: Apache-2.0
// tests/test_fc_count.c —— 计数内核宿主单测（纯 C11，无平台依赖）。
//
// 覆盖：阈值边界、迟滞、EMA 平滑、抖动不重复计数、标签去抖、
// 不可见/低置信样本、最小计数间隔、会话重置、阈值热改。
#include <stdio.h>
#include <string.h>

#include "fc_count.h"

static int failures;

#define CHECK(cond, name)                                                   \
    do {                                                                    \
        if (cond) {                                                         \
            printf("  ok  %s\n", name);                                     \
        } else {                                                            \
            printf("  FAIL %s (%s:%d)\n", name, __FILE__, __LINE__);        \
            failures++;                                                     \
        }                                                                   \
    } while (0)

/* 喂样本宏：C11 不允许 &对函数返回值，先落到局部变量。 */
#define FEED_NUM(c, angle, t_ms)                        \
    do {                                                \
        fc_sample_t s_ = {(angle), FC_STAGE_UNKNOWN,    \
                          true, 1.0f, (t_ms)};          \
        fc_counter_feed((c), &s_);                      \
    } while (0)

#define FEED_LABEL(c, st, t_ms)                         \
    do {                                                \
        fc_sample_t s_ = {-1.0f, (st), true, 1.0f, (t_ms)}; \
        fc_counter_feed((c), &s_);                      \
    } while (0)

#define FEED_RAW(c, sample) fc_counter_feed((c), &(sample))

/* cosf 的便携替身（避免测试依赖 libm）：6 项泰勒展开 wrapped 到 -pi..pi。 */
static float cosf_s(float x)
{
    while (x > 3.14159265f) x -= 6.28318531f;
    while (x < -3.14159265f) x += 6.28318531f;
    const float x2 = x * x;
    return 1.0f - x2 / 2.0f + x2 * x2 / 24.0f - x2 * x2 * x2 / 720.0f +
           x2 * x2 * x2 * x2 / 40320.0f - x2 * x2 * x2 * x2 * x2 / 3628800.0f;
}

static void test_threshold_and_hysteresis(void)
{
    printf("test_threshold_and_hysteresis\n");
    fc_counter_t *c = fc_counter_new(110, 55);
    CHECK(c != NULL, "create");

    /* 躺平（150）→ 坐起（40）→ 计 1。 */
    FEED_NUM(c, 150, 100);
    FEED_NUM(c, 150, 300);
    CHECK(fc_counter_stats(c)->stage == FC_STAGE_UP, "150deg -> UP");
    FEED_NUM(c, 40, 700);
    FEED_NUM(c, 40, 900);
    CHECK(fc_counter_stats(c)->reps == 1, "UP->DOWN counts once");
    CHECK(fc_counter_stats(c)->stage == FC_STAGE_DOWN, "40deg -> DOWN");

    /* 迟滞区间（80）保持 DOWN，不产生计数。 */
    FEED_NUM(c, 80, 1100);
    CHECK(fc_counter_stats(c)->reps == 1, "hysteresis band keeps count");
    CHECK(fc_counter_stats(c)->stage == FC_STAGE_DOWN, "hysteresis band keeps stage");

    /* 回到 UP 再到 DOWN → 第 2 次。 */
    FEED_NUM(c, 150, 1500);
    FEED_NUM(c, 150, 1700);
    FEED_NUM(c, 40, 2100);
    CHECK(fc_counter_stats(c)->reps == 2, "second rep counted");

    /* 连续 DOWN 抖回迟滞区再回 DOWN：不重复计数。 */
    FEED_NUM(c, 60, 2500);
    FEED_NUM(c, 40, 2700);
    CHECK(fc_counter_stats(c)->reps == 2, "no double count on jitter");

    fc_counter_del(c);
}

static void test_sine_replay(void)
{
    printf("test_sine_replay\n");
    /* 模拟 20 个仰卧起坐（与 fc_demo 同相位的余弦 150↔40，3s 周期），
     * 10Hz 采样 —— 对应需求文档的 MediaPipe 角度流。 */
    fc_counter_t *c = fc_counter_new(110, 55);
    const float mid = 95.0f, amp = 55.0f;
    const float two_pi = 6.2831853f;
    uint32_t t = 0;
    for (int i = 0; i < 20 * 30; i++) { /* 20 周期 x 3s @10Hz */
        const float angle = mid + amp * cosf_s(two_pi * (float)t / 3000.0f);
        FEED_NUM(c, angle, t);
        t += 100;
    }
    CHECK(fc_counter_stats(c)->reps == 20, "20 sit-ups from sine replay");
    fc_counter_del(c);
}

static void test_min_rep_interval(void)
{
    printf("test_min_rep_interval\n");
    fc_counter_t *c = fc_counter_new(110, 55);
    /* 极快抖动：UP→DOWN→UP→DOWN 间隔 100ms —— 只允许计 1 次。 */
    FEED_NUM(c, 150, 0);
    FEED_NUM(c, 150, 50);
    FEED_NUM(c, 40, 150);
    FEED_NUM(c, 150, 250);
    FEED_NUM(c, 40, 350);
    CHECK(fc_counter_stats(c)->reps == 1, "sub-interval reps suppressed");
    /* 间隔超过 600ms 的第二次有效。 */
    FEED_NUM(c, 150, 1000);
    FEED_NUM(c, 40, 1200);
    CHECK(fc_counter_stats(c)->reps == 2, "normal interval counted");
    fc_counter_del(c);
}

static void test_label_mode(void)
{
    printf("test_label_mode\n");
    fc_counter_t *c = fc_counter_new(110, 55);
    /* VLM 标签流：需要连续 2 次一致才切状态；up→down 计数。 */
    FEED_LABEL(c, FC_STAGE_UP, 0);
    FEED_LABEL(c, FC_STAGE_UP, 1000);
    CHECK(fc_counter_stats(c)->stage == FC_STAGE_UP, "2x up -> UP");
    FEED_LABEL(c, FC_STAGE_DOWN, 2000);
    CHECK(fc_counter_stats(c)->stage == FC_STAGE_UP, "1x down not enough (debounce)");
    FEED_LABEL(c, FC_STAGE_DOWN, 3000);
    CHECK(fc_counter_stats(c)->stage == FC_STAGE_DOWN, "2x down -> DOWN");
    CHECK(fc_counter_stats(c)->reps == 1, "label up->down counted");
    /* 重建 UP 需要连续 2 个一致标签，随后 DOWN 计第 2 次。 */
    FEED_LABEL(c, FC_STAGE_UP, 4000);
    FEED_LABEL(c, FC_STAGE_UP, 4500);
    FEED_LABEL(c, FC_STAGE_DOWN, 5000);
    FEED_LABEL(c, FC_STAGE_DOWN, 6000);
    CHECK(fc_counter_stats(c)->reps == 2, "second label rep");
    fc_counter_del(c);
}

static void test_visibility_and_conf(void)
{
    printf("test_visibility_and_conf\n");
    fc_counter_t *c = fc_counter_new(110, 55);
    FEED_NUM(c, 150, 0);
    FEED_NUM(c, 150, 100);
    fc_sample_t gone = {-1.0f, FC_STAGE_UNKNOWN, false, 1.0f, 200};
    gone.angle_deg = 40;
    FEED_RAW(c, gone);
    CHECK(fc_counter_stats(c)->stage == FC_STAGE_UP, "invisible ignored");
    CHECK(fc_counter_stats(c)->reps == 0, "invisible not counted");
    fc_sample_t low = {-1.0f, FC_STAGE_UNKNOWN, true, 0.3f, 300};
    low.angle_deg = 40;
    FEED_RAW(c, low);
    CHECK(fc_counter_stats(c)->reps == 0, "low conf ignored");
    FEED_NUM(c, 40, 500);
    FEED_NUM(c, 40, 600);
    CHECK(fc_counter_stats(c)->reps == 1, "recovered sample counted");
    fc_counter_del(c);
}

static void test_reset_and_thresholds(void)
{
    printf("test_reset_and_thresholds\n");
    fc_counter_t *c = fc_counter_new(110, 55);
    FEED_NUM(c, 150, 0);
    FEED_NUM(c, 40, 800);
    CHECK(fc_counter_stats(c)->reps == 1, "one rep before reset");
    fc_counter_reset(c);
    CHECK(fc_counter_stats(c)->reps == 0, "reset clears reps");
    CHECK(fc_counter_stats(c)->angle_smooth < 0, "reset clears smoothing");

    /* 阈值放宽到 100/60：50 度（原迟滞区之上界外）现在算坐起。 */
    fc_counter_set_thresholds(c, 100, 60);
    FEED_NUM(c, 150, 2000);
    FEED_NUM(c, 150, 2100);
    FEED_NUM(c, 50, 2500);
    FEED_NUM(c, 50, 2600);
    CHECK(fc_counter_stats(c)->reps == 1, "custom thresholds applied");
    CHECK(fc_counter_stats(c)->stage == FC_STAGE_DOWN, "50 under new down thresh");
    fc_counter_del(c);
}

static void test_take_reps(void)
{
    printf("test_take_reps\n");
    fc_counter_t *c = fc_counter_new(110, 55);
    FEED_NUM(c, 150, 0);
    FEED_NUM(c, 150, 100);
    FEED_NUM(c, 40, 700);
    CHECK(fc_counter_take_reps(c) == 1, "take_reps returns new reps");
    CHECK(fc_counter_take_reps(c) == 0, "take_reps clears");
    fc_counter_del(c);
}

int main(void)
{
    test_threshold_and_hysteresis();
    test_sine_replay();
    test_min_rep_interval();
    test_label_mode();
    test_visibility_and_conf();
    test_reset_and_thresholds();
    test_take_reps();
    if (failures) {
        printf("FAILED: %d\n", failures);
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
