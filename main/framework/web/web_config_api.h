#pragma once

#include <esp_http_server.h>

// 注册 Web 配置 API 的 URI 处理器到指定服务器。
// 处理器：GET / 、GET /api/config 、PUT /api/config 、GET /api/status
void RegisterConfigApi(httpd_handle_t server);