#pragma once

#include "lvgl.h"
#include "screen_util.h"

// ---------------------------------------------------------------------------
// TranslateScreen — Sinicloud 实时同声传译
//
// 流程：选择源/目标语言 → 按住说话 →
//       获取会话凭据与 WebSocket 地址（期间预缓冲 PCM）→
//       WebSocket 建连（4G 上松手不打断）→ 冲刷缓冲 / 实时推 16kHz PCM →
//       展示识别原文与译文。
//
// 服务地址与接口路径统一由 cloudzao_endpoints.c 配置。
// ---------------------------------------------------------------------------
class TranslateScreen {
public:
    static lv_obj_t* Create();
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
