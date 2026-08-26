#include "framework_main.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_netif_sntp.h>

#include "board.h"
#include "config/config_store.h"
#include "ai/ai_client.h"
#include "web/web_server.h"
#include "wifi_manager.h"
#include "headless_led_controller.h"
#include "headless_network_controller.h"
#include "headless_voice_controller.h"
#include "voice/voice_diagnostics.h"

#define TAG "Headless"

namespace {

// 开机自动登录：等待"已联网 + 网页端已配置账号密码"，再等 NTP 校时完成
// （HTTPS 证书校验依赖正确时间），然后后台预登录一次。避免第一次按键说话
// 时才付出登录 + TLS 握手的数秒延迟；账号未配置或联网失败时静默跳过，
// 之后的登录仍走各请求内的懒加载重试。
void AutoLoginTask(void* arg) {
    auto& net = HeadlessNetworkController::Instance();
    auto& ai = AiClient::Instance();

    // 最多等 60s：覆盖正常开机（NVS 已有配置，秒级就绪）与常规配网流程
    for (int i = 0; i < 600; ++i) {
        if (net.IsConnected() && !net.IsProvisioning() && ai.IsConfigured()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!(net.IsConnected() && !net.IsProvisioning() && ai.IsConfigured())) {
        ESP_LOGW(TAG, "auto login skipped (network or AI config not ready)");
        vTaskDelete(nullptr);
        return;
    }
    // NTP 未同步时最多等 15s；超时也尝试登录（失败由懒加载路径兜底）
    esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000));
    if (ai.Login()) {
        ESP_LOGI(TAG, "auto login ok");
    } else {
        ESP_LOGW(TAG, "auto login failed, will retry on first use");
    }
    vTaskDelete(nullptr);
}

}  // namespace

// 无屏按键语音助手入口（CONFIG_APP_MODE_HEADLESS_VOICE）。
// 不初始化 GC9309/LVGL/背光/深睡；只启动音频、Wi-Fi、按键、RGB 灯与
// 无屏语音状态机，并保留局域网 Web 配置页（服务端地址/账号/密码等）。
void headless_main() {
    ESP_LOGI(TAG, "Headless Voice Assistant starting...");

    // 复位证据（Task 1）：最早位置记录本次复位原因与上次运行阶段/资源快照，
    // 用于区分 Chat 未结束、TTS 建连/接收期间、还是播放启动前发生复位。
    voice_diag::LogLastRunOnBoot();

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

    // 先联网：WifiManager::Initialize 内部创建 esp_netif/tcpip 线程，
    // WebServer::Start 依赖 lwIP（httpd），必须在其之后启动。
    board.StartNetwork();

    // 关闭 Wi-Fi 省电（modem sleep）。省电唤醒间隔会把往返延迟放大到百毫秒级，
    // 叠加 lwIP 默认 5760 字节 TCP 窗口后，实测到服务器的双向吞吐只有
    // 15~25 KB/s，ASR 上行与 TTS 下行都被压慢。代价是空闲电流升高，
    // 若以后要优先续航，可改为仅在对话期间切换 PERFORMANCE。
    WifiManager::GetInstance().SetPowerSaveLevel(WifiPowerSaveLevel::PERFORMANCE);

    // 后台自动登录（联网 + 已配置账号后），不等第一次按键
    if (xTaskCreate(AutoLoginTask, "auto_login", 8192, nullptr, 3, nullptr) !=
        pdPASS) {
        ESP_LOGE(TAG, "failed to create auto login task");
    }

    // Web 配置服务器：配网 AP 占用 80 端口时后台重试，配网结束自动启动
    WebServer::Start();

    ESP_LOGI(TAG, "Headless voice assistant running");
    while (true) {
        vTaskDelay(portMAX_DELAY);
    }
}