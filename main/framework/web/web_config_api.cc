#include "web_config_api.h"

#include <string.h>
#include <vector>

#include <esp_log.h>

#include "board.h"
#include "config/config_store.h"
#include "config/config_schema.h"
#include "app/app_manager.h"
#include "storage/littlefs_store.h"
#include "json_util.h"
#include "web_ui_page.h"

#define TAG "ConfigApi"

namespace {

void SendJson(httpd_req_t* req, const std::string& body, int status = HTTPD_200) {
    httpd_resp_set_status(req, status == HTTPD_200 ? HTTPD_200 : HTTPD_400);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, body.c_str(), body.length());
}

std::string BuildConfigJson() {
    auto& store = ConfigStore::Instance();

    // schema 数组
    std::string schema = "[";
    for (int i = 0; i < kConfigSchemaSize; ++i) {
        const ConfigItem& it = kConfigSchema[i];
        if (i > 0) {
            schema += ",";
        }
        schema += "{\"key\":\"" + json_util::Escape(it.key) +
                  "\",\"label\":\"" + json_util::Escape(it.label ? it.label : "") +
                  "\",\"type\":\"" + json_util::TypeName(static_cast<int>(it.type)) +
                  "\",\"default\":\"" + json_util::Escape(it.default_value ? it.default_value : "") +
                  "\",\"group\":\"" + json_util::Escape(it.group ? it.group : "") + "\"";
        if (it.type == ConfigType::kEnum && it.options != nullptr) {
            schema += ",\"options\":\"" + json_util::Escape(it.options) + "\"";
        }
        schema += "}";
    }
    schema += "]";

    // values 对象
    std::string values = "{";
    for (int i = 0; i < kConfigSchemaSize; ++i) {
        if (i > 0) {
            values += ",";
        }
        std::string v = store.Get(kConfigSchema[i].key);
        values += "\"" + json_util::Escape(kConfigSchema[i].key) + "\":\"" + json_util::Escape(v) + "\"";
    }
    values += "}";

    return "{\"schema\":" + schema + ",\"values\":" + values + "}";
}

esp_err_t HandleRoot(httpd_req_t* req) {
    // 内嵌配置页（Phase B-3，见 web_ui/index.html 内容随固件内嵌）
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t HandleGetConfig(httpd_req_t* req) {
    SendJson(req, BuildConfigJson());
    return ESP_OK;
}

esp_err_t HandlePutConfig(httpd_req_t* req) {
    const int total = req->content_len;
    if (total <= 0 || total > 4096) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body size");
        return ESP_FAIL;
    }
    std::string body(total, '\0');
    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, &body[received], total - received);
        if (ret <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv error");
            return ESP_FAIL;
        }
        received += ret;
    }

    std::map<std::string, std::string> updates;
    if (!json_util::ParseFlatObject(body.c_str(), updates)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_FAIL;
    }

    auto& store = ConfigStore::Instance();
    for (const auto& kv : updates) {
        store.Set(kv.first.c_str(), kv.second.c_str());
    }
    SendJson(req, "{\"ok\":true,\"updated\":" + std::to_string(updates.size()) + "}");
    return ESP_OK;
}

esp_err_t HandleGetStatus(httpd_req_t* req) {
    std::string app = AppManager::Instance().CurrentId();
    std::string out = "{\"ok\":true,\"device\":\"" + json_util::Escape(Board::GetInstance().GetBoardType()) +
                      "\",\"mode\":\"framework\",\"app\":\"" + json_util::Escape(app) + "\"}";
    SendJson(req, out);
    return ESP_OK;
}

// 解析 /api/upload?name=xxx.jpg 中的文件名参数
bool GetQueryName(httpd_req_t* req, std::string& name) {
    if (req->query_string == nullptr) {
        return false;
    }
    std::string q = req->query_string;
    size_t pos = 0;
    while (pos < q.size()) {
        size_t eq = q.find('=', pos);
        if (eq == std::string::npos) {
            break;
        }
        std::string key = q.substr(pos, eq - pos);
        size_t ampersand = q.find('&', eq + 1);
        if (ampersand == std::string::npos) {
            ampersand = q.size();
        }
        std::string val = q.substr(eq + 1, ampersand - eq - 1);
        if (key == "name") {
            name = val;
            return !name.empty();
        }
        pos = ampersand + 1;
    }
    return false;
}

// POST /api/upload?name=wallpaper.jpg  —— body 为原始图片字节，写入 LittleFS
esp_err_t HandleUpload(httpd_req_t* req) {
    std::string name;
    if (!GetQueryName(req, name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing name param");
        return ESP_FAIL;
    }
    if (req->content_len <= 0 || req->content_len > (2 * 1024 * 1024)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad size");
        return ESP_FAIL;
    }

    std::vector<uint8_t> buf;
    buf.resize(req->content_len);
    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, reinterpret_cast<char*>(&buf[received]), req->content_len - received);
        if (ret <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv error");
            return ESP_FAIL;
        }
        received += ret;
    }

    if (!LittleFsStore::WriteFile(name.c_str(), buf.data(), buf.size())) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
        return ESP_FAIL;
    }
    SendJson(req, "{\"ok\":true,\"file\":\"" + json_util::Escape(name) + "\",\"size\":" + std::to_string(buf.size()) + "}");
    return ESP_OK;
}

// GET /api/image/{name} —— 读取 LittleFS 图片字节返回给客户端
esp_err_t HandleGetImage(httpd_req_t* req) {
    std::string uri = req->uri;
    const char* prefix = "/api/image/";
    size_t pp = uri.find(prefix);
    if (pp == std::string::npos) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_FAIL;
    }
    std::string name = uri.substr(pp + strlen(prefix));
    if (name.empty()) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_FAIL;
    }

    std::vector<uint8_t> data;
    if (!LittleFsStore::ReadFile(name.c_str(), data)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_FAIL;
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_send(req, reinterpret_cast<const char*>(data.data()), data.size());
    return ESP_OK;
}

}  // namespace

void RegisterConfigApi(httpd_handle_t server) {
    static const httpd_uri_t root = {
        .uri = "/", .method = HTTP_GET, .handler = HandleRoot, .user_ctx = nullptr};
    static const httpd_uri_t get_config = {
        .uri = "/api/config", .method = HTTP_GET, .handler = HandleGetConfig, .user_ctx = nullptr};
    static const httpd_uri_t put_config = {
        .uri = "/api/config", .method = HTTP_PUT, .handler = HandlePutConfig, .user_ctx = nullptr};
    static const httpd_uri_t get_status = {
        .uri = "/api/status", .method = HTTP_GET, .handler = HandleGetStatus, .user_ctx = nullptr};
    static const httpd_uri_t upload = {
        .uri = "/api/upload", .method = HTTP_POST, .handler = HandleUpload, .user_ctx = nullptr};
    static const httpd_uri_t get_image = {
        .uri = "/api/image/*", .method = HTTP_GET, .handler = HandleGetImage, .user_ctx = nullptr};

    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &get_config);
    httpd_register_uri_handler(server, &put_config);
    httpd_register_uri_handler(server, &get_status);
    httpd_register_uri_handler(server, &upload);
    httpd_register_uri_handler(server, &get_image);
}