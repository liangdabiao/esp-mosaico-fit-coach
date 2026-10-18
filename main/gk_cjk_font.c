// SPDX-License-Identifier: Apache-2.0
// main/gk_cjk_font.c —— 中文位图字模 blit。
//
// raylib-lite 的 DrawText 只覆盖 5x7 ASCII；这里把 gen_cjk_font.py 光栅化好的
// 16x16 二值字模逐像素画到 RGB565 表面。只支持字库内的汉字（与页面文案一一对应）；
// 不在字库的码点被跳过，ASCII 请继续用 DrawText。
#include "gk_cjk_font.h"

#include <stddef.h>

#include "gk_cjk_glyph_data.h"

/* 字间距（像素）。 */
#define GK_CJK_PITCH (GK_CJK_CELL + 2)

static const gk_cjk_glyph_t *find_glyph(uint32_t cp)
{
    for (uint16_t i = 0; i < GK_CJK_GLYPH_COUNT; i++) {
        if (GK_CJK_GLYPHS[i].cp == cp) return &GK_CJK_GLYPHS[i];
    }
    return NULL;
}

/* 解一个 UTF-8 码点；返回字节数，*cp 写出。非法按 1 字节处理。 */
static int utf8_next(const char *s, uint32_t *cp)
{
    const unsigned char *u = (const unsigned char *)s;
    if (u[0] < 0x80) {
        *cp = u[0];
        return 1;
    }
    if ((u[0] & 0xE0) == 0xC0) {
        *cp = ((uint32_t)(u[0] & 0x1F) << 6) | (uint32_t)(u[1] & 0x3F);
        return 2;
    }
    if ((u[0] & 0xF0) == 0xE0) {
        *cp = ((uint32_t)(u[0] & 0x0F) << 12) | ((uint32_t)(u[1] & 0x3F) << 6) |
              (uint32_t)(u[2] & 0x3F);
        return 3;
    }
    *cp = u[0];
    return 1;
}

/* 以左上角 (x,y) 绘制中文串，返回绘制后的下一像素 x。 */
int gk_cjk_draw(const char *utf8, int x, int y, Color color)
{
    if (!utf8) return x;
    const char *s = utf8;
    int pen = x;
    while (*s) {
        uint32_t cp = 0;
        const int step = utf8_next(s, &cp);
        s += step;
        const gk_cjk_glyph_t *g = find_glyph(cp);
        if (!g) continue; // 不在字库（含 ASCII）：跳过，交给 DrawText。
        for (int r = 0; r < GK_CJK_CELL; r++) {
            const uint16_t row = g->rows[r];
            if (!row) continue;
            for (int c = 0; c < GK_CJK_CELL; c++) {
                if (row & (1 << (15 - c))) {
                    DrawRectangle(pen + c, y + r, 1, 1, color);
                }
            }
        }
        pen += GK_CJK_PITCH;
    }
    return pen;
}

/* 量宽：每字一个 pitch，ASCII 不计入（由调用方另行用 DrawText 排）。 */
int gk_cjk_width(const char *utf8)
{
    if (!utf8) return 0;
    int n = 0;
    const char *s = utf8;
    while (*s) {
        uint32_t cp = 0;
        const int step = utf8_next(s, &cp);
        s += step;
        if (find_glyph(cp)) n++;
    }
    return n * GK_CJK_PITCH;
}

/* 整数倍放大绘制：每个字模像素画 scale x scale 方块。 */
int gk_cjk_draw_scaled(const char *utf8, int x, int y, Color color, int scale)
{
    if (!utf8 || scale < 1) return x;
    if (scale == 1) return gk_cjk_draw(utf8, x, y, color);
    const char *s = utf8;
    int pen = x;
    while (*s) {
        uint32_t cp = 0;
        const int step = utf8_next(s, &cp);
        s += step;
        const gk_cjk_glyph_t *g = find_glyph(cp);
        if (!g) continue;
        for (int r = 0; r < GK_CJK_CELL; r++) {
            const uint16_t row = g->rows[r];
            if (!row) continue;
            for (int c = 0; c < GK_CJK_CELL; c++) {
                if (row & (1 << (15 - c))) {
                    DrawRectangle(pen + c * scale, y + r * scale, scale, scale, color);
                }
            }
        }
        pen += (GK_CJK_CELL + 2) * scale;
    }
    return pen;
}

int gk_cjk_width_scaled(const char *utf8, int scale)
{
    if (scale < 1) scale = 1;
    return gk_cjk_width(utf8) * scale;
}
