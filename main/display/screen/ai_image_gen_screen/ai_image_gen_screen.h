#pragma once

#include "lvgl.h"
#include "screen_util.h"

// ---------------------------------------------------------------------------
// AiImageGenScreen — AI 语音生图
//
// 流程：按住说话 → 语音转写 → 创建生图任务 →
//       若响应已含图片地址则直接下载，否则轮询任务状态 → 展示图片。
// 服务地址与接口路径统一由 cloudzao_endpoints.c 配置。
//
// 录音 / HTTP 均在独立 FreeRTOS 任务中执行，不阻塞 LVGL。
// ---------------------------------------------------------------------------
class AiImageGenScreen {
 public:
    static lv_obj_t* Create();
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
