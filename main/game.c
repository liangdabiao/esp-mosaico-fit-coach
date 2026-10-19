// SPDX-License-Identifier: Apache-2.0
// main/game.c —— 姿态健身教练：五页状态机 + raylib 图元渲染 + 触摸路由。
//
// 页面：主页 / 训练 / 结果 / 设置 / 历史。
// 数据：演示模式（fc_demo 合成角度）；服务器/VLM 模式由设备 runtime 经
//       game_feed_pose 推送样本（宿主模拟器亦可注入）。
// 渲染：CJK 位图字模（gk_cjk_font，支持整数倍放大）+ ASCII DrawText 混排。
#include "game.h"
#include "game_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fc_demo.h"
#include "fc_sfx.h"
#include "fc_store.h"
#include "fc_time.h"
#include "fc_voice.h"
#include "fc_voice_clips.h"
#include "gk_cjk_font.h"
#include "mosaico_raylib_fast.h"

/* ---------------- 布局与配色 ---------------- */

#define FC_PITCH 18 /* CJK 16px 单元 + 2 间距 */

#define C_BG ((Color){16, 18, 26, 255})
#define C_PANEL ((Color){26, 30, 42, 255})
#define C_PANEL2 ((Color){38, 44, 60, 255})
#define C_BORDER ((Color){70, 78, 100, 255})
#define C_INK ((Color){226, 230, 240, 255})
#define C_DIM ((Color){136, 144, 162, 255})
#define C_RED ((Color){235, 76, 76, 255})
#define C_GREEN ((Color){62, 190, 112, 255})
#define C_BLUE ((Color){110, 160, 255, 255})
#define C_GOLD ((Color){238, 200, 90, 255})

/* ---------------- 应用状态 ---------------- */

#define FC_POSE_MBOX 16 /* SPSC 环：runtime 生产，引擎消费 */

typedef struct game_t game_t;

typedef struct {
    uint32_t session_start_ms;
    uint32_t session_start_tick; /* 演示模式虚拟时钟零点（确定性回放） */
    uint32_t session_end_ms;
    bool paused;
    bool target_hit;       /* 本组达标提示已触发 */
    bool halfway_fired;    /* 过半程鼓励已触发 */
    bool five_left_fired;  /* 还差五个已触发 */
    bool no_signal_fired;  /* 无信号提示已触发（回到镜头即重置） */
    bool confirm_reset;    /* 设置页「重置网络」二次确认 */
    uint32_t last_sample_tick; /* 最近收到真实样本的 tick（无信号判定） */
    /* 点击沿检测（单实例足够；设备与模拟器各只建一个 game） */
    bool tap_armed;
    int32_t tap_x, tap_y;
} fc_session_t;

struct game_t {
    game_config_t config;
    game_snapshot_t snap;
    fc_settings_t settings;
    fc_counter_t *counter;
    fc_demo_t *demo;
    fc_session_t session;
    fc_sample_t mbox[FC_POSE_MBOX];
    volatile uint32_t mbox_head; /* 生产者写 */
    volatile uint32_t mbox_tail; /* 消费者写 */
    uint8_t link; /* fc_link_t，由 runtime 上报 */
};

/* ---------------- 小工具 ---------------- */

static int utf8_seq_len(unsigned char c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    return 4;
}

static int mixed_width(const char *line)
{
    if (!line) return 0;
    int width = 0;
    const char *s = line;
    char ascii_buf[24];
    size_t ascii_len = 0;
    for (;;) {
        const unsigned char c = (unsigned char)*s;
        if (c >= 0x80) {
            if (ascii_len) {
                ascii_buf[ascii_len] = '\0';
                width += MeasureText(ascii_buf, 16);
                ascii_len = 0;
            }
            const size_t n = utf8_seq_len(c);
            char buf[8];
            if (n < sizeof(buf)) {
                memcpy(buf, s, n);
                buf[n] = '\0';
                int w = gk_cjk_width(buf);
                if (w <= 0) w = FC_PITCH;
                width += w;
            }
            s += n;
        } else if (c == '\0') {
            break;
        } else {
            if (ascii_len < sizeof(ascii_buf) - 1) ascii_buf[ascii_len++] = (char)c;
            s++;
        }
    }
    if (ascii_len) {
        ascii_buf[ascii_len] = '\0';
        width += MeasureText(ascii_buf, 16);
    }
    return width;
}

static void draw_mixed(const char *line, int x, int y, Color color)
{
    if (!line) return;
    int pen = x;
    const char *s = line;
    char ascii_buf[24];
    size_t ascii_len = 0;
    for (;;) {
        const unsigned char c = (unsigned char)*s;
        if (c >= 0x80) {
            if (ascii_len) {
                ascii_buf[ascii_len] = '\0';
                DrawText(ascii_buf, pen, y + 3, 16, color);
                pen += MeasureText(ascii_buf, 16);
                ascii_len = 0;
            }
            const size_t n = utf8_seq_len(c);
            char buf[8];
            if (n < sizeof(buf)) {
                memcpy(buf, s, n);
                buf[n] = '\0';
                const int p0 = pen;
                pen = gk_cjk_draw(buf, pen, y, color);
                if (pen == p0) pen = p0 + FC_PITCH;
            }
            s += n;
        } else if (c == '\0') {
            break;
        } else {
            if (ascii_len < sizeof(ascii_buf) - 1) ascii_buf[ascii_len++] = (char)c;
            s++;
        }
    }
    if (ascii_len) {
        ascii_buf[ascii_len] = '\0';
        DrawText(ascii_buf, pen, y + 3, 16, color);
    }
}

static void draw_mixed_center(const char *line, int cx, int y, Color color)
{
    draw_mixed(line, cx - mixed_width(line) / 2, y, color);
}

static bool hit(int x, int y, int bx, int by, int bw, int bh)
{
    return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

static void panel(int x, int y, int w, int h, Color fill)
{
    DrawRectangle(x, y, w, h, fill);
    DrawRectangleLines(x, y, w, h, C_BORDER);
}

static void button(int x, int y, int w, int h, const char *label, Color fill, Color ink)
{
    panel(x, y, w, h, fill);
    draw_mixed_center(label, x + w / 2, y + (h - 16) / 2, ink);
}

static const char *mode_name(uint8_t mode)
{
    switch (mode) {
    case FC_MODE_HTTP: return "服务器";
    case FC_MODE_VLM: return "云端直连";
    default: return "演示模式";
    }
}

static const char *link_name(uint8_t link)
{
    switch (link) {
    case FC_LINK_CONNECTING: return "连接中";
    case FC_LINK_OK: return "正常";
    case FC_LINK_ERROR: return "失败";
    default: return "离线";
    }
}

/* ---------------- 语音反馈 ---------------- */

/* 运动者躺着看屏幕：训练中的反馈主通道是语音（fc_voice），提示音只是
 * 语音不可用/静音模式时的回退。宿主上 fc_voice 不可用，行为退化为哔声。 */
static bool voice_enabled(const game_t *g)
{
    return g->settings.sound_on && g->settings.voice_mode != FC_VOICE_OFF &&
           fc_voice_available();
}

static void voice_num(int n)
{
    if (n >= FC_CLIP_NUM_MIN && n <= FC_CLIP_NUM_MAX) fc_voice_play(n);
}

/* 完成一次后的反馈：语音报数（模式决定每个/每5个）或提示音。 */
static void announce_rep(game_t *g, uint32_t reps)
{
    if (voice_enabled(g)) {
        const int n = (reps <= (uint32_t)FC_CLIP_NUM_MAX) ? (int)reps : -1;
        if (g->settings.voice_mode == FC_VOICE_EVERY) {
            voice_num(n);
            return;
        }
        if (g->settings.voice_mode == FC_VOICE_FIFTH) {
            if (n >= 0 && n % 5 == 0) {
                voice_num(n);
                return;
            }
        }
    }
    if (g->settings.sound_on) fc_sfx_play(FC_TONE_REP);
}

/* ---------------- 会话与点击 ---------------- */

static void session_begin(game_t *g)
{
    fc_counter_reset(g->counter);
    fc_demo_reset(g->demo);
    memset(&g->session, 0, sizeof(g->session));
    g->session.session_start_ms = fc_time_ms();
    g->session.session_start_tick = g->snap.tick;
    g->session.last_sample_tick = g->snap.tick; /* 8 秒宽限后再提示无信号 */
    g->snap.reps = 0;
    g->snap.stage = FC_STAGE_UNKNOWN;
    g->snap.angle = -1;
    g->snap.session_ms = 0;
    fc_voice_flush(); /* 上一组的残留不再播 */
    if (voice_enabled(g)) {
        fc_voice_play(FC_CLIP_TRAIN_START_PREFIX);
        voice_num(g->settings.target_reps);
        fc_voice_play(FC_CLIP_UNIT_GE);
    }
}

static void session_finish(game_t *g)
{
    g->session.session_end_ms = fc_time_ms();
    const uint32_t ms = g->session.session_end_ms - g->session.session_start_ms;
    const uint16_t minutes = (uint16_t)((ms / 60000u) + ((ms % 60000u) ? 1 : 0));
    fc_store_history_append(fc_time_ymd(),
                            (uint16_t)fc_counter_stats(g->counter)->reps, minutes);
    fc_voice_flush();
    if (voice_enabled(g)) {
        fc_voice_play(FC_CLIP_RESULT_PREFIX);
        voice_num(fc_counter_stats(g->counter)->reps);
        fc_voice_play(FC_CLIP_UNIT_GE);
    }
}

static void settings_tap(game_t *g, int32_t x, int32_t y);

static void handle_tap(game_t *g, int32_t x, int32_t y)
{
    switch (g->snap.page) {
    case FC_PAGE_HOME: {
        if (hit(x, y, 120, 230, 240, 84)) { /* 开始训练 */
            if (g->settings.sound_on) fc_sfx_play(FC_TONE_NAV);
            session_begin(g);
            g->snap.page = FC_PAGE_TRAIN;
        } else if (hit(x, y, 40, 400, 190, 60)) { /* 设置 */
            if (g->settings.sound_on) fc_sfx_play(FC_TONE_NAV);
            g->session.confirm_reset = false;
            g->snap.page = FC_PAGE_SETTINGS;
        } else if (hit(x, y, 250, 400, 190, 60)) { /* 历史 */
            if (g->settings.sound_on) fc_sfx_play(FC_TONE_NAV);
            g->snap.page = FC_PAGE_HISTORY;
        }
        break;
    }
    case FC_PAGE_TRAIN: {
        if (hit(x, y, 20, 400, 210, 60)) { /* 暂停/继续 */
            g->session.paused = !g->session.paused;
            if (g->settings.sound_on) {
                if (voice_enabled(g)) {
                    fc_voice_play(g->session.paused ? FC_CLIP_PAUSED
                                                    : FC_CLIP_RESUMED);
                } else {
                    fc_sfx_play(FC_TONE_NAV);
                }
            }
        } else if (hit(x, y, 250, 400, 210, 60)) { /* 结束 */
            if (g->settings.sound_on && !voice_enabled(g)) {
                fc_sfx_play(FC_TONE_BACK);
            }
            session_finish(g);
            g->snap.page = FC_PAGE_RESULT;
        }
        break;
    }
    case FC_PAGE_RESULT: {
        if (hit(x, y, 40, 330, 190, 70)) { /* 再来一组 */
            if (g->settings.sound_on) fc_sfx_play(FC_TONE_NAV);
            session_begin(g);
            g->snap.page = FC_PAGE_TRAIN;
        } else if (hit(x, y, 250, 330, 190, 70)) { /* 完成 */
            if (g->settings.sound_on) fc_sfx_play(FC_TONE_BACK);
            g->snap.page = FC_PAGE_HOME;
        }
        break;
    }
    case FC_PAGE_SETTINGS: {
        settings_tap(g, x, y);
        break;
    }
    case FC_PAGE_HISTORY: {
        if (hit(x, y, 20, 20, 100, 44)) { /* 返回 */
            if (g->settings.sound_on) fc_sfx_play(FC_TONE_BACK);
            g->snap.page = FC_PAGE_HOME;
        }
        break;
    }
    default:
        break;
    }
}

static void settings_tap(game_t *g, int32_t x, int32_t y)
{
    fc_settings_t *s = &g->settings;
    bool changed = false;

    if (hit(x, y, 16, 8, 448, 40)) { /* 返回（顶栏） */
        if (s->sound_on) fc_sfx_play(FC_TONE_BACK);
        g->snap.page = FC_PAGE_HOME;
        return;
    }

    int row_y = 76;
    /* 模式行（整行循环切换） */
    if (hit(x, y, 16, row_y, 448, 44)) {
        s->mode = (s->mode == FC_MODE_DEMO) ? FC_MODE_HTTP
                  : (s->mode == FC_MODE_HTTP) ? FC_MODE_VLM
                                               : FC_MODE_DEMO;
        changed = true;
    }
    row_y += 52;
    /* 目标 ± */
    if (hit(x, y, 350, row_y, 50, 44) && s->target_reps > 5) { s->target_reps -= 5; changed = true; }
    if (hit(x, y, 410, row_y, 50, 44) && s->target_reps < 200) { s->target_reps += 5; changed = true; }
    row_y += 52;
    /* 躺下阈值 ± */
    if (hit(x, y, 350, row_y, 50, 44) && s->up_thresh > s->down_thresh + 10) { s->up_thresh -= 5; changed = true; }
    if (hit(x, y, 410, row_y, 50, 44) && s->up_thresh < 170) { s->up_thresh += 5; changed = true; }
    row_y += 52;
    /* 坐起阈值 ± */
    if (hit(x, y, 350, row_y, 50, 44) && s->down_thresh > 20) { s->down_thresh -= 5; changed = true; }
    if (hit(x, y, 410, row_y, 50, 44) && s->down_thresh < s->up_thresh - 10) { s->down_thresh += 5; changed = true; }
    row_y += 52;
    /* 音效开关（整行） */
    if (hit(x, y, 16, row_y, 448, 44)) {
        s->sound_on = !s->sound_on;
        changed = true;
    }
    row_y += 52;
    /* 语音报数模式（整行循环：关 → 每个 → 每5个） */
    if (hit(x, y, 16, row_y, 448, 44)) {
        s->voice_mode = (s->voice_mode == FC_VOICE_OFF) ? FC_VOICE_EVERY
                        : (s->voice_mode == FC_VOICE_EVERY) ? FC_VOICE_FIFTH
                                                             : FC_VOICE_OFF;
        changed = true;
    }
    row_y += 60;
    /* 重置网络（二次确认） */
    if (hit(x, y, 16, row_y, 448, 52)) {
        if (!g->session.confirm_reset) {
            g->session.confirm_reset = true;
            if (s->sound_on) fc_sfx_play(FC_TONE_ERR);
        } else {
            fc_store_clear_network();
            fc_store_settings_load(s);
            g->session.confirm_reset = false;
            changed = true;
        }
    }

    if (changed) {
        fc_counter_set_thresholds(g->counter, s->up_thresh, s->down_thresh);
        fc_store_settings_save(s);
        if (s->sound_on) fc_sfx_play(FC_TONE_NAV);
    }
}

/* ---------------- 生命周期 ---------------- */

bool game_create(const game_config_t *config, game_handle_t *ret_handle)
{
    if (!config || !ret_handle) return false;
    game_t *g = calloc(1, sizeof(*g));
    if (!g) return false;
    g->config = *config;
    fc_store_settings_load(&g->settings);
    g->counter = fc_counter_new(g->settings.up_thresh, g->settings.down_thresh);
    g->demo = fc_demo_new();
    if (!g->counter || !g->demo) {
        game_delete(g);
        return false;
    }
    g->snap.page = FC_PAGE_HOME;
    g->snap.angle = -1;
    *ret_handle = (game_handle_t)g;
    return true;
}

void game_delete(game_handle_t handle)
{
    game_t *g = (game_t *)handle;
    if (!g) return;
    fc_counter_del(g->counter);
    fc_demo_del(g->demo);
    free(g);
}

void game_reset(game_handle_t handle)
{
    game_t *g = (game_t *)handle;
    if (!g) return;
    fc_counter_reset(g->counter);
    fc_demo_reset(g->demo);
    memset(&g->session, 0, sizeof(g->session));
    g->snap.reps = 0;
    g->snap.page = FC_PAGE_HOME;
}

void game_set_paused(game_handle_t handle, bool paused)
{
    if (handle) ((game_t *)handle)->session.paused = paused;
}

void game_set_pointer(game_handle_t handle, int32_t x, int32_t y, bool pressed)
{
    game_t *g = (game_t *)handle;
    if (!g) return;
    g->snap.pointer_x = x;
    g->snap.pointer_y = y;
    g->snap.pointer_down = pressed;
    /* 点击分发在 game_update 内做（输入状态不在 render 里消费）。 */
}

void game_feed_pose(game_handle_t handle, const fc_sample_t *sample)
{
    game_t *g = (game_t *)handle;
    if (!g || !sample) return;
    const uint32_t head = g->mbox_head;
    const uint32_t next = (head + 1) % FC_POSE_MBOX;
    if (next == g->mbox_tail) {
        g->mbox_tail = (g->mbox_tail + 1) % FC_POSE_MBOX; /* 满：丢最旧 */
    }
    g->mbox[head] = *sample;
    g->mbox_head = next;
}

void game_set_link(game_handle_t handle, uint8_t link)
{
    if (handle) ((game_t *)handle)->link = link;
}

bool game_wants_capture(game_handle_t handle)
{
    game_t *g = (game_t *)handle;
    if (!g) return false;
    return g->snap.page == FC_PAGE_TRAIN && !g->session.paused &&
           g->settings.mode != FC_MODE_DEMO;
}

const fc_settings_t *game_settings_view(game_handle_t handle)
{
    return handle ? &((game_t *)handle)->settings : NULL;
}

/* ---------------- 更新 ---------------- */

void game_update(game_handle_t handle)
{
    game_t *g = (game_t *)handle;
    if (!g) return;
    g->snap.tick++;

    /* 消费 runtime 样本。 */
    for (;;) {
        const uint32_t tail = g->mbox_tail;
        if (tail == g->mbox_head) break;
        fc_counter_feed(g->counter, &g->mbox[tail]);
        g->mbox_tail = (tail + 1) % FC_POSE_MBOX;
        g->session.last_sample_tick = g->snap.tick;
        g->session.no_signal_fired = false; /* 回到镜头，重新武装提示 */
    }

    const bool streaming = g->snap.page == FC_PAGE_TRAIN && !g->session.paused;

    /* 演示模式：自己产样本（10Hz）。相位用 tick 虚拟时钟——设备 30Hz
     * 恒等于真实时间，模拟器回放则可确定复现（replay 断言依赖）。 */
    if (streaming && g->settings.mode == FC_MODE_DEMO && (g->snap.tick % 3) == 0) {
        const uint32_t phase_ms =
            (g->snap.tick - g->session.session_start_tick) * (1000u / GAME_TICK_HZ);
        const fc_sample_t s = {
            .angle_deg = fc_demo_angle_at(g->demo, phase_ms),
            .stage = FC_STAGE_UNKNOWN,
            .visible = true,
            .conf = 1.0f,
            .t_ms = phase_ms,
        };
        fc_counter_feed(g->counter, &s);
    }

    /* 计数反馈：语音报数 + 一次性鼓励短语 + 达标。 */
    const fc_stats_t *st = fc_counter_stats(g->counter);
    if (st->reps != g->snap.reps) {
        const uint32_t reps = st->reps;
        g->snap.reps = reps;
        announce_rep(g, reps);
        const uint32_t target = g->settings.target_reps;
        if (g->settings.sound_on && target > 5 && reps == target - 5 &&
            !g->session.five_left_fired) {
            g->session.five_left_fired = true;
            if (voice_enabled(g)) fc_voice_play(FC_CLIP_FIVE_LEFT);
        }
        if (g->settings.sound_on && target >= 10 && !g->session.halfway_fired &&
            reps * 2 >= target && reps < target) {
            g->session.halfway_fired = true;
            if (voice_enabled(g)) fc_voice_play(FC_CLIP_HALFWAY);
        }
    }
    if (!g->session.target_hit && g->settings.target_reps > 0 &&
        st->reps >= g->settings.target_reps && streaming) {
        g->session.target_hit = true;
        if (voice_enabled(g)) {
            fc_voice_play(FC_CLIP_TARGET_HIT);
        } else if (g->settings.sound_on) {
            fc_sfx_play(FC_TONE_TARGET);
        }
    }
    /* 无信号：>8 秒没有有效样本，语音提醒一次（回到镜头重新武装）。 */
    if (streaming && g->settings.mode != FC_MODE_DEMO &&
        g->settings.sound_on && !g->session.no_signal_fired &&
        (g->snap.tick - g->session.last_sample_tick) > 8u * 30u) {
        g->session.no_signal_fired = true;
        if (voice_enabled(g)) fc_voice_play(FC_CLIP_NO_SIGNAL);
    }

    g->snap.stage = (uint8_t)st->stage;
    g->snap.angle = st->angle_smooth < 0 ? -1 : (int16_t)(st->angle_smooth + 0.5f);
    g->snap.streaming = streaming;
    g->snap.mode = g->settings.mode;
    g->snap.target = g->settings.target_reps;
    g->snap.link = g->settings.mode == FC_MODE_DEMO ? (uint8_t)FC_LINK_OK : g->link;
    if (g->snap.page == FC_PAGE_TRAIN) {
        g->snap.session_ms = fc_time_ms() - g->session.session_start_ms;
    }

    /* 点击分发（按下→抬起沿触发，坐标取按下位置）。 */
    if (g->snap.pointer_down && !g->session.tap_armed) {
        g->session.tap_armed = true;
        g->session.tap_x = g->snap.pointer_x;
        g->session.tap_y = g->snap.pointer_y;
    } else if (!g->snap.pointer_down && g->session.tap_armed) {
        g->session.tap_armed = false;
        handle_tap(g, g->session.tap_x, g->session.tap_y);
    }
}

/* ---------------- 渲染 ---------------- */

static void draw_topbar(const char *title)
{
    DrawRectangle(0, 0, 480, 48, C_PANEL);
    if (title) draw_mixed_center(title, 240, 16, C_INK);
}

static void render_home(game_t *g)
{
    char buf[48];

    draw_topbar(NULL);
    gk_cjk_draw_scaled("姿态健身教练", 240 - gk_cjk_width_scaled("姿态健身教练", 2) / 2,
                       70, C_GOLD, 2);
    draw_mixed_center("仰卧起坐 · 摄像头计数", 240, 130, C_DIM);

    const uint32_t today = fc_store_history_today_reps(fc_time_ymd());
    panel(40, 160, 400, 44, C_PANEL);
    snprintf(buf, sizeof(buf), "今日 %lu 个", (unsigned long)today);
    draw_mixed(buf, 60, 174, C_INK);
    snprintf(buf, sizeof(buf), "目标 %u 个/组", (unsigned)g->settings.target_reps);
    draw_mixed(buf, 464 - mixed_width(buf), 174, C_DIM);

    button(120, 230, 240, 84, "开始训练", C_BLUE, C_INK);

    char mode_line[48];
    snprintf(mode_line, sizeof(mode_line), "%s · %s", mode_name(g->settings.mode),
             link_name(g->snap.link));
    draw_mixed_center(mode_line, 240, 340, C_DIM);
    if (g->settings.mode == FC_MODE_HTTP && g->settings.server_url[0]) {
        draw_mixed_center(g->settings.server_url, 240, 362, C_DIM);
    }

    button(40, 400, 190, 60, "设置", C_PANEL2, C_INK);
    button(250, 400, 190, 60, "历史", C_PANEL2, C_INK);
}

static void render_train(game_t *g)
{
    const fc_stats_t *st = fc_counter_stats(g->counter);
    char buf[48];

    /* 顶栏：模式/链路 + 无信号提示 */
    DrawRectangle(0, 0, 480, 48, C_PANEL);
    draw_mixed(mode_name(g->settings.mode), 16, 16,
               g->settings.mode == FC_MODE_DEMO ? C_DIM : C_BLUE);
    const uint32_t no_signal = g->settings.mode == FC_MODE_DEMO
                                   ? 0
                                   : (g->snap.tick - g->session.last_sample_tick);
    const char *right = (no_signal > 8u * 30u) ? "无信号" : link_name(g->snap.link);
    draw_mixed(right, 464 - mixed_width(right), 16,
               (no_signal > 8u * 30u) ? C_RED : C_DIM);

    /* 时长 */
    snprintf(buf, sizeof(buf), "%lu:%02lu",
             (unsigned long)(g->snap.session_ms / 60000),
             (unsigned long)((g->snap.session_ms / 1000) % 60));
    DrawText(buf, 16, 60, 20, C_DIM);

    /* 大计数 */
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)st->reps);
    DrawText(buf, 240 - MeasureText(buf, 96) / 2, 92, 96, C_INK);

    /* 状态 */
    const char *stage_text = "检测中";
    Color stage_color = C_DIM;
    if (st->stage == FC_STAGE_UP) { stage_text = "躺平"; stage_color = C_BLUE; }
    if (st->stage == FC_STAGE_DOWN) { stage_text = "坐起"; stage_color = C_GREEN; }
    if (g->settings.mode != FC_MODE_DEMO && st->last_sample_ms == 0) { stage_text = "请入镜"; }
    draw_mixed_center(stage_text, 240, 212, stage_color);
    if (g->session.target_hit) {
        draw_mixed("达标", 464 - mixed_width("达标"), 212, C_GREEN);
    }

    /* 进度条（目标） */
    const int pw = 400;
    const int px = 40, py = 262;
    DrawRectangle(px, py, pw, 8, C_PANEL2);
    const uint32_t target = g->settings.target_reps ? g->settings.target_reps : 1;
    int fill = (int)((uint64_t)st->reps * pw / target);
    if (fill > pw) fill = pw;
    DrawRectangle(px, py, fill, 8, st->reps >= target ? C_GREEN : C_GOLD);

    /* 角度仪表：0..180 横条 + 阈值刻度 + 指针 */
    const int gx = 60, gy = 300, gw = 320, gh = 18;
    DrawRectangle(gx, gy, gw, gh, C_PANEL2);
    const int up_x = gx + gw * g->settings.up_thresh / 180;
    const int down_x = gx + gw * g->settings.down_thresh / 180;
    DrawRectangle(up_x, gy - 4, 2, gh + 8, C_BLUE);
    DrawRectangle(down_x, gy - 4, 2, gh + 8, C_GREEN);
    if (st->angle_smooth >= 0) {
        int ax = gx + (int)(gw * st->angle_smooth / 180.0f);
        if (ax < gx) ax = gx;
        if (ax > gx + gw - 6) ax = gx + gw - 6;
        DrawRectangle(ax, gy - 8, 6, gh + 16, C_GOLD);
    }
    char abuf[24];
    snprintf(abuf, sizeof(abuf), "%d",
             (int)(st->angle_smooth < 0 ? 0 : st->angle_smooth + 0.5f));
    DrawText(abuf, gx + gw + 10, gy + 1, 16, C_GOLD);
    draw_mixed("躺下", gx, gy + 26, C_DIM);
    snprintf(buf, sizeof(buf), "%d", (int)g->settings.up_thresh);
    DrawText(buf, gx + mixed_width("躺下") + 6, gy + 29, 16, C_BLUE);
    draw_mixed("坐起", gx + 130, gy + 26, C_DIM);
    snprintf(buf, sizeof(buf), "%d", (int)g->settings.down_thresh);
    DrawText(buf, gx + 130 + mixed_width("坐起") + 6, gy + 29, 16, C_GREEN);

    /* 底部按钮 */
    button(20, 400, 210, 60, g->session.paused ? "继续" : "暂停", C_PANEL2, C_INK);
    button(250, 400, 210, 60, "结束", C_RED, C_INK);
}

static void render_result(game_t *g)
{
    const fc_stats_t *st = fc_counter_stats(g->counter);
    const uint32_t ms = g->session.session_end_ms
                            ? g->session.session_end_ms - g->session.session_start_ms
                            : g->snap.session_ms;
    const bool reached = g->settings.target_reps > 0 &&
                         st->reps >= g->settings.target_reps;
    const Color num_color = reached ? C_GREEN : C_INK;
    char buf[48];

    draw_topbar("本组完成");

    char nbuf[16];
    snprintf(nbuf, sizeof(nbuf), "%lu", (unsigned long)st->reps);
    const int nw = MeasureText(nbuf, 72);
    const int cw = gk_cjk_width("个");
    const int total = nw + 10 + cw;
    DrawText(nbuf, 240 - total / 2, 110, 72, num_color);
    gk_cjk_draw("个", 240 - total / 2 + nw + 10, 110 + 72 - 16, num_color);

    snprintf(buf, sizeof(buf), "用时 %lu 分 %lu 秒",
             (unsigned long)(ms / 60000), (unsigned long)((ms / 1000) % 60));
    draw_mixed_center(buf, 240, 230, C_DIM);

    if (reached) {
        draw_mixed_center("目标达成", 240, 262, C_GREEN);
    } else if (g->settings.target_reps > 0) {
        snprintf(buf, sizeof(buf), "目标 %u 个", (unsigned)g->settings.target_reps);
        draw_mixed_center(buf, 240, 262, C_DIM);
    }

    button(40, 330, 190, 70, "再来一组", C_BLUE, C_INK);
    button(250, 330, 190, 70, "完成", C_PANEL2, C_INK);
}

static void render_settings(game_t *g)
{
    char buf[64];
    draw_topbar("设置");
    draw_mixed("返回", 20, 16, C_INK);

    int y = 76;
    /* 模式 */
    panel(16, y, 448, 44, C_PANEL);
    draw_mixed("模式", 32, y + 14, C_DIM);
    draw_mixed(mode_name(g->settings.mode), 300, y + 14, C_INK);
    y += 52;
    /* 目标 */
    panel(16, y, 448, 44, C_PANEL);
    draw_mixed("每组目标", 32, y + 14, C_DIM);
    snprintf(buf, sizeof(buf), "%u 个", (unsigned)g->settings.target_reps);
    draw_mixed(buf, 250, y + 14, C_INK);
    DrawText("-", 362, y + 12, 24, C_INK);
    DrawText("+", 428, y + 12, 24, C_INK);
    y += 52;
    /* 躺下阈值 */
    panel(16, y, 448, 44, C_PANEL);
    draw_mixed("躺下阈值(度)", 32, y + 14, C_DIM);
    snprintf(buf, sizeof(buf), "%d", (int)g->settings.up_thresh);
    DrawText(buf, 310, y + 14, 16, C_INK);
    DrawText("-", 362, y + 12, 24, C_INK);
    DrawText("+", 428, y + 12, 24, C_INK);
    y += 52;
    /* 坐起阈值 */
    panel(16, y, 448, 44, C_PANEL);
    draw_mixed("坐起阈值(度)", 32, y + 14, C_DIM);
    snprintf(buf, sizeof(buf), "%d", (int)g->settings.down_thresh);
    DrawText(buf, 310, y + 14, 16, C_INK);
    DrawText("-", 362, y + 12, 24, C_INK);
    DrawText("+", 428, y + 12, 24, C_INK);
    y += 52;
    /* 音效 */
    panel(16, y, 448, 44, C_PANEL);
    draw_mixed("音效", 32, y + 14, C_DIM);
    draw_mixed(g->settings.sound_on ? "开" : "关", 300, y + 14,
               g->settings.sound_on ? C_GREEN : C_DIM);
    y += 52;
    /* 语音报数 */
    panel(16, y, 448, 44, C_PANEL);
    draw_mixed("语音报数", 32, y + 14, C_DIM);
    {
        const char *vname = "关";
        Color vcolor = C_DIM;
        if (g->settings.voice_mode == FC_VOICE_EVERY) { vname = "每个"; vcolor = C_GREEN; }
        else if (g->settings.voice_mode == FC_VOICE_FIFTH) { vname = "每5个"; vcolor = C_GREEN; }
        draw_mixed(vname, 300, y + 14, vcolor);
        if (!fc_voice_available() && g->settings.voice_mode != FC_VOICE_OFF) {
            draw_mixed("(无)", 380, y + 14, C_RED);
        }
    }
    y += 60;
    /* 重置网络 */
    panel(16, y, 448, 52, C_PANEL);
    draw_mixed_center(g->session.confirm_reset ? "再按一次确认重置" : "重置网络配置",
                      240, y + 18, g->session.confirm_reset ? C_RED : C_INK);
}

static void render_history(game_t *g)
{
    (void)g;
    draw_topbar("最近记录");
    draw_mixed("返回", 20, 16, C_INK);

    fc_history_entry_t hist[7];
    const size_t count = fc_store_history_get(hist, 7);
    if (count == 0) {
        draw_mixed_center("暂无记录", 240, 220, C_DIM);
        return;
    }
    char buf[64];
    int y = 76;
    for (size_t i = 0; i < count; i++) {
        panel(16, y, 448, 44, C_PANEL);
        if (hist[i].ymd) {
            snprintf(buf, sizeof(buf), "%lu-%02lu-%02lu",
                     (unsigned long)(hist[i].ymd / 10000),
                     (unsigned long)(hist[i].ymd / 100 % 100),
                     (unsigned long)(hist[i].ymd % 100));
        } else {
            snprintf(buf, sizeof(buf), "--");
        }
        DrawText(buf, 32, y + 14, 16, C_DIM);
        snprintf(buf, sizeof(buf), "%lu 个", (unsigned long)hist[i].reps);
        draw_mixed(buf, 200, y + 14, C_INK);
        snprintf(buf, sizeof(buf), "%lu 分", (unsigned long)hist[i].minutes);
        draw_mixed(buf, 464 - mixed_width(buf), y + 14, C_DIM);
        y += 52;
    }
}

bool game_render(game_handle_t handle)
{
    game_t *g = (game_t *)handle;
    if (!g) return false;
    BeginDrawing();
    ClearBackground(C_BG);
    switch (g->snap.page) {
    case FC_PAGE_HOME: render_home(g); break;
    case FC_PAGE_TRAIN: render_train(g); break;
    case FC_PAGE_RESULT: render_result(g); break;
    case FC_PAGE_SETTINGS: render_settings(g); break;
    case FC_PAGE_HISTORY: render_history(g); break;
    default: break;
    }
    EndDrawing();
    return true;
}

game_snapshot_t game_read(game_handle_t handle)
{
    if (!handle) {
        game_snapshot_t zero = {0};
        zero.angle = -1;
        return zero;
    }
    return ((game_t *)handle)->snap;
}
