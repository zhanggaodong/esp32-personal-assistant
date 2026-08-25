#include "headless_network_controller.h"

#include <esp_log.h>

#include "headless_led_controller.h"
#include "audio_prompt_player.h"

#define TAG "HeadlessNet"

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