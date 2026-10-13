#pragma once

#include "lvgl.h"
#include "screen_util.h"

class StandbyScreen {
public:
    // 全屏待机页：翻页时钟、日期星期；屏幕/底栏触摸或侧面键短按返回首页。
    static lv_obj_t* Create();
    static void LifecycleCallback(screen_lifecycle_event_t event);

    // 切到待机页（须在 LVGL 线程调用）。
    static void Show();
    static bool IsActive();
    // 屏幕与底栏触摸共用进入待机后的防误触窗口（须在 LVGL 线程调用）。
    static bool WakeFromTouch();
    // 从待机回到首页（须在 LVGL 线程调用）。
    static void ReturnHome();
};
