#pragma once

#include <esp_http_server.h>

namespace WebServer {

// 启动 HTTP 服务器并注册配置 API。已启动则直接返回 ESP_OK。
esp_err_t Start();

// 返回当前服务器的句柄，未启动返回 nullptr。
httpd_handle_t Handle();

}  // namespace WebServer