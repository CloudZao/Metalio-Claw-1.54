#pragma once

#include <string>

// 接口返回中文天气描述（如「多云」「小雨」），映射到资源 ic_s_weather_{code}.spng
//（240 屏资源为 24×24，须 1:1 绘制，不可对 SPNG 做 zoom）。
const char* WeatherIconCodeForText(const std::string& text);
