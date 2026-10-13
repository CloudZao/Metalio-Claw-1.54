#pragma once

#include "lvgl.h"
#include "screen_util.h"

// Wi-Fi configuration: scan on request, manage saved networks, and switch
// transport. Viewing the page leaves WifiManager's driver running; scanning or
// connecting temporarily takes ownership and restores it when leaving.
class NetworkScreen {
public:
    static lv_obj_t* Create();
    static void LifecycleCallback(screen_lifecycle_event_t event);
    static bool IsNavigationBlocked();
    static bool TryHandleNavBack();
};
