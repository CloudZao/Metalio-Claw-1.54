#pragma once

#include "lvgl.h"

class WeatherScreen {
public:
    // 天气页：district 接口 + 省市区联动；图标见 weather_icon_map / ic_s_weather_*.spng。
    static lv_obj_t* Create();

    /**
     * @brief 硬件 ◁ / 侧滑返回：有城市选择弹窗则先收起
     * @return true 已消费返回，勿退出天气页
     */
    static bool TryHandleNavBack();
};
