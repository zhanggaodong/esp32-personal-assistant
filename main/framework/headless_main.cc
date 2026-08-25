#include "framework_main.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>

#include "board.h"
#include "config/config_store.h"
#include "ai/ai_client.h"
#include "web/web_server.h"
#include "headless_led_controller.h"
#include "headless_network_controller.h"
#include "headless_voice_controller.h"

#define TAG "Headless"

// 无屏按键语音助手入口（CONFIG_APP_MODE_HEADLESS_VOICE）。
// 不初始化 GC9309/LVGL/背光/深睡；只启动音频、Wi-Fi、按键、RGB 灯与
// 无屏语音状态机，并保留局域网 Web 配置页（服务端地址/账号/密码等）。
void headless_main() {
    ESP_LOGI(TAG, "Headless Voice Assistant starting...");

    // 平台层：构建板对象。板级无屏裁剪在 boards/*/ 构造函数中完成：
    // 不初始化 LCD/LVGL、不初始化背光、禁用板级深睡关闭；同时把电源键
    // 注册为 PTT（按住说话/松开提交），BOOT 键注册为重新配网。
    auto& board = Board::GetInstance();

    // 配置中心（NVS 持久化）
    ConfigStore::Instance().Init();

    // 网络事件 → 无屏控制器（本地提示音 + LED），不再依赖 Application/Display
    board.SetNetworkEventCallback([](NetworkEvent event, const std::string& data) {
        HeadlessNetworkController::Instance().OnNetworkEvent(event, data);
    });

    // 无屏语音状态机：初始化音频，启动录音/ASR/Chat/TTS 工作线程
    HeadlessVoiceController::Instance().Start();

    // Web 配置服务器：配网 AP 占用 80 端口时后台重试，配网结束自动启动
    WebServer::Start();

    // 异步联网：无已保存 Wi-Fi 自动进入热点配网
    board.StartNetwork();

    ESP_LOGI(TAG, "Headless voice assistant running");
    while (true) {
        vTaskDelay(portMAX_DELAY);
    }
}