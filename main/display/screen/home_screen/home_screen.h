#pragma once

#include "lvgl.h"

class HomeScreen {
public:
    // Create a fullscreen home page (240x240 card pager).
    // One app card per page; swipe left/right to switch. Returns the
    // created LVGL screen object (parent = NULL).
    static lv_obj_t* Create();
    static void RefreshStatusBar();
    // 下次进入主屏时从第一页开始（用于主题切换等场景）。
    static void ResetToFirstPage();
    // 硬件底栏：是否当前在主页；左右翻页；双击中键回聊天首页。
    static bool IsActive();
    static void FlipPage(int dir);
    static void GoHomeToChat();

    // PWR_KEY 长按后弹出 [重启 / 关机] 对话框（须在 LVGL 线程调用）。
    static void ShowPowerOptionsDialog();
    /**
     * @brief 关闭电源选项弹窗（若已打开）
     * @return true 原先开着并已关闭
     */
    static bool TryClosePowerDialog();
    // 软件关机（UI / 空闲策略共用）。
    static void RequestSystemShutdown(const char* reason);

    // 无操作进入待机 / 累计关机时长（分钟），0 表示关闭。已迁移到 idle_power_policy。
    static int GetIdleShutdownMinutes();
    static void SetIdleShutdownMinutes(int minutes);
    static int GetIdleStandbyMinutes();
    static void SetIdleStandbyMinutes(int minutes);
};
