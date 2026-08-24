#include "framework_main.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>

#include "board.h"
#include "assets.h"
#include "app/app_registry.h"
#include "app/app_manager.h"
#include "config/config_store.h"
#include "storage/littlefs_store.h"
#include "input/input_router.h"
#include "web/web_server.h"
#include "screens/screen_home.h"
#include "screens/screen_settings.h"
#include "screens/screen_screensaver.h"
#include "screens/screen_ai_chat.h"

#define TAG "framework"

// 屏幕模块实例（由框架首次进入时注册）
ScreenHome g_home_screen;
ScreenSettings g_settings_screen;
ScreenScreensaver g_screensaver_screen;
ScreenAiChat g_ai_chat_screen;

void framework_main() {
    ESP_LOGI(TAG, "Personal Assistant Framework starting...");

    // 平台层：构建并持有屏幕/音频/WiFi/按键/背光等硬件
    auto& board = Board::GetInstance();
    board.StartNetwork();  // 异步联网，供 Web 配置页访问

    // The Action packages font_puhui_common_20_4.bin in assets. Framework
    // mode bypasses Application::Initialize(), so load UI assets explicitly.
    if (!Assets::GetInstance().Apply(false)) {
        ESP_LOGW(TAG, "UI assets unavailable; using built-in fallback font");
    }

    // 配置中心（NVS 持久化）
    ConfigStore::Instance().Init();

    // 壁纸等图片资源（LittleFS 独立分区）
    LittleFsStore::Mount();

    // 注册屏幕模块
    AppRegistry::Instance().Register(&g_home_screen);
    AppRegistry::Instance().Register(&g_settings_screen);
    AppRegistry::Instance().Register(&g_screensaver_screen);
    AppRegistry::Instance().Register(&g_ai_chat_screen);

    // 启动框架并显示默认屏
    AppManager::Instance().Start();

    // Web 配置服务器
    WebServer::Start();

    // 输入路由（按键动作可配置映射）
    InputRouter::Instance().Init();

    ESP_LOGI(TAG, "Framework running");
    while (true) {
        vTaskDelay(portMAX_DELAY);
    }
}
