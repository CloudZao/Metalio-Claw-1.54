#pragma once

#include <driver/gpio.h>

#include "screen_util.h"

// ---------------------------------------------------------------------------
// PWR_KEY 统一管理
//
// 开机调用 PwrKey_Init() 一次，注册短按 + 长按，之后不再销毁。
// 各页面在 LOAD / UNLOAD 时通过 PwrKey_OnScreenLifecycle() 维护前台栈；
// 按键回调只读栈顶做分发与日志。
//
// 短按进待机仅当栈顶为 "home"。UNLOAD 绝不会回落成 "home"，避免
// 一级页跳二级页时父页销毁把前台误标成首页。
// ---------------------------------------------------------------------------

// 注册短按 / 长按。无 IO 扩展器时走 GPIO（1.54 Power_Dec）。
void PwrKey_Init();
void PwrKey_Init(gpio_num_t gpio_num, bool active_high = false);

// USB 插拔边沿（原始供电检测，满电仍为 true）。插入后短窗口抑制短按毛刺。
void PwrKey_OnUsbPowerChanged(bool usb_powered);

// 当前是否 USB 供电（满电仍为 true；供触控等模块查询）。
bool PwrKey_IsUsbPowered();

// 页面生命周期联动：LOAD 压栈，UNLOAD 移除最靠上的同名页。
void PwrKey_OnScreenLifecycle(const char* name, screen_lifecycle_event_t event);

// 当前前台页面名（永不为空；栈空时为 "none"）。
const char* PwrKey_ActiveScreen();
