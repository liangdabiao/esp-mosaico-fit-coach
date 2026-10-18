// SPDX-License-Identifier: Apache-2.0
// main/fc_settings.c —— 默认设置（设备/宿主共用的便携实现）。
#include "fc_store.h"

#include <string.h>

void fc_settings_defaults(fc_settings_t *settings)
{
    if (!settings) return;
    memset(settings, 0, sizeof(*settings));
    settings->mode = FC_MODE_DEMO;
    settings->target_reps = 30;
    settings->sound_on = 1;
    settings->up_thresh = 110;   /* 需求文档默认阈值 */
    settings->down_thresh = 55;
}
