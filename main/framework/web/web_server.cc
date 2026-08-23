#include "web_server.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <esp_log.h>

#include "web_config_api.h"

#define TAG "WebServer"

#define RETRY_DELAY_MS 5000

namespace {
httpd_handle_t s_server = nullptr;
SemaphoreHandle_t s_mutex = nullptr;

// 尝试启动一次配置页服务器。返回句柄；失败返回 nullptr（端口被占用）。
httpd_handle_t TryStart() {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    // 支持通配符匹配（为后续 /api/image/{name} 预留）
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 16;
    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        return nullptr;
    }
    RegisterConfigApi(server);
    ESP_LOGI(TAG, "Web server started on port %d", cfg.server_port);
    return server;
}

// 后台任务：端口被配网服务器（同为 80）占用时，定期重试直到端口释放。
void ServerTask(void* arg) {
    while (true) {
        httpd_handle_t server = TryStart();
        if (server != nullptr) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            if (s_server == nullptr) {
                s_server = server;
            }
            xSemaphoreGive(s_mutex);
            break;
        }
        ESP_LOGW(TAG, "Port busy (provisioning?), retry in %dms", RETRY_DELAY_MS);
        vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
    }
    vTaskDelete(nullptr);
}
}  // namespace

namespace WebServer {

esp_err_t Start() {
    if (s_mutex == nullptr) {
        s_mutex = xSemaphoreCreateMutex();
    }
    if (s_server != nullptr) {
        return ESP_OK;
    }
    // 网络正常时直接启动成功。
    if (TryStart() != nullptr) {
        return ESP_OK;
    }
    // 端口被配网服务器占用：转为后台重试，配网完成后自动起来。
    if (xTaskCreate(ServerTask, "web_server", 4096, nullptr, 5, nullptr) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

httpd_handle_t Handle() {
    return s_server;
}

}  // namespace WebServer