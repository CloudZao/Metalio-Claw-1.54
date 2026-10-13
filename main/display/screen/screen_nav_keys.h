#pragma once

#include <cstdint>

// 硬件底栏三键（CST816 虚拟键）：◁ / ○ / ▷
// 主页：◁▷ 翻页；○ 双击回聊天首页。
// 聊天页：按住 ○ = 按住说话（PTT）。

enum class ScreenNavKey {
    Prev = 0,  // ◁
    Home = 1,  // ○
    Next = 2,  // ▷
};

// 由 touch_feed 在识别到底栏键时调用（可在 LVGL 读回调线程）。
// 待机时消费本次底栏按下，仅唤醒，不执行导航或 PTT。
bool screen_nav_on_standby_press();
// screen_nav_on_home_press 返回 true 表示已进入聊天 PTT。
bool screen_nav_on_home_press();
void screen_nav_on_home_release(uint32_t held_ms);
void screen_nav_on_chat_home_click();  // 聊天页单击 ○：打断说话 / 结束聆听
void screen_nav_on_hardware_key(ScreenNavKey key, bool double_click);

// 底栏 ○ 是否仍按住（PTT）；StopListening 在按住期间应忽略，避免刚开听就被冲掉。
bool screen_nav_ptt_held();

