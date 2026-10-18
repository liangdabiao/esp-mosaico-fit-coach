// SPDX-License-Identifier: Apache-2.0
// main/fc_sfx_host.c —— 宿主音效桩：静音。
#include "fc_sfx.h"

int fc_sfx_start(void) { return 0; }
void fc_sfx_stop(void) {}
void fc_sfx_play(fc_tone_t tone) { (void)tone; }
