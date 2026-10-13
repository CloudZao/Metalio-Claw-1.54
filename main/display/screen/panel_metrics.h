#pragma once

// 星智 1.54 Metal：240x240。各 App 布局统一从这里取尺寸。
namespace panel {

constexpr int kSize = 240;
constexpr int kWidth = 240;
constexpr int kHeight = 240;

constexpr int kStatusBarH = 28;
constexpr int kIndicatorH = 16;
constexpr int kHeaderH = 36;
// 返回热区 / 图标：对齐 HTML preview（图标约 18px），较原 32 等比缩小约 3/4。
constexpr int kBackBtn = 24;
constexpr int kBackIconScale = 144;  // LVGL 256=1x；约 18/32
constexpr int kPad = 8;
// 顶栏小图标最小触摸边长；可视更小时用 lv_obj_set_ext_click_area 补足。
constexpr int kMinTouch = 40;
constexpr int kTouchExt = 10;
// 顶栏返回键：相对 LEFT_MID(8,0) 再左移，当前较常见位置再左 6px。
constexpr int kBackX = -3;
constexpr int kBackY = 2;
constexpr int kTabBarH = 36;
constexpr int kFooterH = 56;

// 主题图标是 128px SPNG。切片格式不支持 LVGL zoom/rotate，必须 1:1 绘制。
constexpr int kIconSrcPx = 128;
constexpr int kIconDrawPx = 128;
constexpr int kIconScale = 256;  // LVGL 256=1x；勿对 SPNG 设其它缩放

}  // namespace panel
