#include "headless_network_controller.h"

#include <esp_log.h>
#include <esp_netif_sntp.h>

#include "headless_led_controller.h"
#include "audio_prompt_player.h"
#include "device_log.h"

#define TAG "HeadlessNet"

namespace {

// 联网成功后同步系统时间。HTTPS 证书校验依赖正确时间（默认 1970 会握手
// 失败），必须在设备调后端前完成一次 NTP 校时。
void StartSntpIfNeeded() {
    static bool started = false;
    if (started) {
        return;
    }
    started = true;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&cfg);
    DeviceLog::Log('I', "HeadlessNet", "已启动 NTP 校时(pool.ntp.org)");
}

}  // namespace

HeadlessNetworkController& HeadlessNetworkController::Instance() {
    static HeadlessNetworkController instance;
    return instance;
}

void HeadlessNetworkController::OnNetworkEvent(NetworkEvent event,
                                               const std::string& data) {
    auto& led = HeadlessLedController::Instance();
    auto& prompt = AudioPromptPlayer::Instance();

    switch (event) {
        case NetworkEvent::Scanning:
        case NetworkEvent::Connecting:
            connected_ = false;
            led.ShowWifiConnecting();
            ESP_LOGI(TAG, "WiFi connecting... (%s)", data.c_str());
            break;

        case NetworkEvent::Connected:
            connected_ = true;
            provisioning_ = false;
            led.ShowProvisioned();  // 绿色 3 次短闪后常亮
            if (!success_prompted_) {
                success_prompted_ = true;
                prompt.Play(AudioPromptPlayer::Prompt::kProvisionSuccess);
            }
            DeviceLog::Log('I', "HeadlessNet", "WiFi 已连接(%s)", data.c_str());
            // https 证书校验需要正确系统时间：联网后立即校时
            StartSntpIfNeeded();
            ESP_LOGI(TAG, "WiFi connected (%s)", data.c_str());
            break;

        case NetworkEvent::Disconnected:
            // 仅在"曾经联网后掉线"时报网络异常；进配网前的 StopStation 也会
            // 触发 Disconnected，避免与"进入配网"提示音重复打扰。
            if (connected_) {
                led.ShowError();
                prompt.Play(AudioPromptPlayer::Prompt::kNetworkError);
            }
            connected_ = false;
            ESP_LOGW(TAG, "WiFi disconnected");
            break;

        case NetworkEvent::WifiConfigModeEnter:
            provisioning_ = true;
            led.ShowProvisioning();
            prompt.Play(AudioPromptPlayer::Prompt::kEnterProvision);
            ESP_LOGI(TAG, "config mode enter (AP %s)", data.c_str());
            break;

        case NetworkEvent::WifiConfigModeExit:
            provisioning_ = false;
            led.ShowProvisioning();  // 已拿到凭据，等待 station 连接
            ESP_LOGI(TAG, "config mode exit, connecting...");
            break;

        default:
            break;
    }
}