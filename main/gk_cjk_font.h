// SPDX-License-Identifier: Apache-2.0
// main/gk_cjk_font.h —— 中文位图字模 blit 的对外接口。
//
// 字模表由 tools/gomoku/gen_cjk_font.py 生成到 gk_cjk_glyph_data.h（勿手改），
// 只在 gk_cjk_font.c 内部包含；这里只暴露绘制接口，避免每个编译单元都带一份字表。
#pragma once

#include "mosaico_raylib_fast.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 以左上角 (x,y) 绘制中文串（16px 全角），返回绘制后的下一像素 x。
 * 不在字库的码点（含 ASCII）被跳过——ASCII 请用 DrawText。 */
int gk_cjk_draw(const char *utf8, int x, int y, Color color);

/* 中文串宽度（像素），不计入被跳过的 ASCII。 */
int gk_cjk_width(const char *utf8);

/* 整数倍放大版（scale >= 1）：标题/大字号用。 */
int gk_cjk_draw_scaled(const char *utf8, int x, int y, Color color, int scale);
int gk_cjk_width_scaled(const char *utf8, int scale);

#ifdef __cplusplus
}
#endif
