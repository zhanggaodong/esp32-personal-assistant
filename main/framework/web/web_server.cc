#include "web_server.h"

#include <esp_log.h>

#include "web_config_api.h"

#define TAG "WebServer"

namespace {
httpd_handle_t s_server = nullptr;
}

namespace WebServer {

esp_err_t Start() {
    if (s_server != nullptr) {
        return ESP_OK;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    // 支持通配符匹配（为后续 /api/image/{name} 预留）
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 16;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server: %s", esp_err_to_name(err));
        return err;
    }
    RegisterConfigApi(s_server);
    ESP_LOGI(TAG, "Web server started on port %d", cfg.server_port);
    return ESP_OK;
}

httpd_handle_t Handle() {
    return s_server;
}

}  // namespace WebServer