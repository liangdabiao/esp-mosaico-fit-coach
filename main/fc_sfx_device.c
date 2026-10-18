// SPDX-License-Identifier: Apache-2.0
// main/fc_sfx_device.c —— 设备音效：rttl_player（内部自管 ES8311 codec）。
#include "fc_sfx.h"

#include "esp_err.h"
#include "rtttl_player.h"

/* RTTTL：名字:默认(时长,八度,速度):音符序列。短、单音、反馈明确。 */
static const char *const TONES[FC_TONE_COUNT] = {
    [FC_TONE_REP] = "rp:d=16,o=5,b=400:c6",
    [FC_TONE_TARGET] = "tg:d=8,o=5,b=220:c,e,g,c6",
    [FC_TONE_NAV] = "nv:d=8,o=5,b=300:e",
    [FC_TONE_BACK] = "bk:d=8,o=5,b=300:g,e",
    [FC_TONE_ERR] = "er:d=4,o=4,b=500:c",
};

static int s_started;

int fc_sfx_start(void)
{
    if (s_started) return 0;
    if (rtttl_player_start() != ESP_OK) return -1;
    s_started = 1;
    return 0;
}

void fc_sfx_stop(void)
{
    if (!s_started) return;
    rtttl_player_stop();
    s_started = 0;
}

void fc_sfx_play(fc_tone_t tone)
{
    if (!s_started || tone < 0 || tone >= FC_TONE_COUNT) return;
    (void)rtttl_player_play(TONES[tone]);
}
