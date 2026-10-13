#pragma once

#include "lvgl.h"
#include "screen_util.h"

class SettingsScreen {
public:
    static lv_obj_t* Create();
    static void LifecycleCallback(screen_lifecycle_event_t event);

    // 当前是否为设置页（含子详情）。
    static bool IsActive();
    // OTA 详情页：硬件 ◁ / 返回应直接回主页，不回设置列表。
    static bool IsOtaDetailActive();
    // 离开设置回主页（供硬件键使用）。
    static void ExitToHome();
};
