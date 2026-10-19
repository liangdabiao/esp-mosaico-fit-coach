// SPDX-License-Identifier: Apache-2.0
// main/fc_voice_host.c —— 宿主语音桩：静音（模拟器/单测不发声）。
#include "fc_voice.h"

int fc_voice_start(bool codec_already_open)
{
    (void)codec_already_open;
    return -1;
}
void fc_voice_stop(void) {}
bool fc_voice_available(void) { return false; }
void fc_voice_play(int clip_id) { (void)clip_id; }
void fc_voice_play2(int a, int b) { (void)a; (void)b; }
void fc_voice_play3(int a, int b, int c) { (void)a; (void)b; (void)c; }
void fc_voice_flush(void) {}
