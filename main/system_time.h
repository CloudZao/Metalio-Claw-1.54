#pragma once

#include <string>

// 联网后启动 SNTP（可重复调用；已启动则忽略）。Wi‑Fi 有效；4G 通常无 ESP netif，需另走模组时钟。
void SystemTimeStartSntp();

/**
 * @brief 用模组 AT+CCLK 应答设置系统时间
 * @param response SendCommand("AT+CCLK?") 后的完整应答（含 +CCLK: ...）
 * @return true 解析并 settimeofday 成功
 */
bool SystemTimeApplyModemCcLk(const std::string& response);
