#include "ai_client.h"

#include <esp_http_client.h>
#include <esp_log.h>
#include <cstring>

#include "../config/config_store.h"
#include "../web/json_util.h"
#include "base64.h"

#define TAG "AiClient"

// ---------- 基础 HTTP / Base64 / SSE 工具 ----------

namespace {

// URL 拼接：保证 backend_url 尾随斜杠与 path 前导斜杠不重复。
std::string JoinUrl(const std::string& base, const std::string& path) {
    std::string b = base;
    while (!b.empty() && b.back() == '/') {
        b.pop_back();
    }
    if (b.empty()) {
        return path;
    }
    std::string p = path;
    while (!p.empty() && p.front() == '/') {
        p.erase(0, 1);
    }
    return b + "/" + p;
}

// 一次同步 HTTP POST。headers 逐行（如 "Content-Type: application/json"）。
// 响应体整段写入 out_body；HTTP 状态写入 *out_status。
struct HttpResponse {
    int status = 0;
    std::string body;
};

esp_err_t PerformPost(const std::string& url,
                      const std::vector<std::string>& headers,
                      const std::string& body,
                      const char* accept,
                      HttpResponse& out) {
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 15000;  // 连接/首包超时；SSE 长流由 open/read 循环处理
    cfg.buffer_size = 2048;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_OK;

    for (const auto& h : headers) {
        size_t colon = h.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        esp_http_client_set_header(client, h.substr(0, colon).c_str(),
                                   h.substr(colon + 1).c_str());
    }
    if (accept != nullptr) {
        esp_http_client_set_header(client, "Accept", accept);
    }
    esp_http_client_set_header(client, "Connection", "keep-alive");

    err = esp_http_client_open(client, (int)body.size());
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }
    if (!body.empty()) {
        int w = esp_http_client_write(client, body.data(), (int)body.size());
        if (w < 0) {
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
    }

    out.status = esp_http_client_fetch_headers(client);
    esp_http_client_get_status_code(client, &out.status);

    char buf[1024];
    int total = 0;
    int n;
    while ((n = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
        out.body.append(buf, (size_t)n);
        total += n;
        if (total > 64 * 1024) {
            break;  // 保护内存
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_OK;
}

// SSE 行级解析器：Feed() 送入响应字节流，逐行触发回调。
class SseParser {
public:
    using OnEvent = std::function<void(const std::string& event,
                                       const std::string& data)>;
    SseParser(OnEvent cb) : cb_(std::move(cb)) {}

    void Feed(const char* data, size_t len) {
        buf_.append(data, len);
        size_t pos = 0;
        while (true) {
            size_t nl = buf_.find('\n', pos);
            if (nl == std::string::npos) {
                buf_.erase(0, pos);
                break;
            }
            std::string line = buf_.substr(pos, nl - pos);
            pos = nl + 1;
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            HandleLine(line);
        }
    }

private:
    void HandleLine(const std::string& line) {
        if (line.empty()) {
            if (!event_.empty() || !data_.empty()) {
                cb_(event_, data_);
            }
            event_.clear();
            data_.clear();
            return;
        }
        if (line.compare(0, 6, "event:") == 0) {
            event_ = Trim(line.substr(6));
        } else if (line.compare(0, 5, "data:") == 0) {
            std::string v = Trim(line.substr(5));
            if (!data_.empty()) data_ += "\n";
            data_ += v;
        }
        // 其它字段（id/retry/注释）忽略
    }

    static std::string Trim(std::string s) {
        size_t b = s.find_first_not_of(" \t");
        if (b == std::string::npos) {
            return "";
        }
        size_t e = s.find_last_not_of(" \t");
        return s.substr(b, e - b + 1);
    }

    OnEvent cb_;
    std::string buf_;
    bool flushed_ = false;
    std::string event_;
    std::string data_;
};

}  // namespace

AiClient& AiClient::Instance() {
    static AiClient instance;
    return instance;
}

void AiClient::UpdateConfig() {
    ConfigStore& store = ConfigStore::Instance();
    backend_url_ = store.Get("ai.backend_url");
    account_ = store.Get("ai.account");
    password_ = store.Get("ai.password");
    voice_ = store.Get("ai.voice");
    if (backend_url_.empty() || account_.empty() || password_.empty()) {
        InvalidateToken();
    }
}

bool AiClient::DoLogin() {
    if (backend_url_.empty() || account_.empty() || password_.empty()) {
        ESP_LOGW(TAG, "AI config incomplete (%s/%s/%s)",
                 backend_url_.c_str(), account_.empty() ? "(空)" : "***",
                 password_.empty() ? "(空)" : "***");
        return false;
    }

    std::string payload = "{\"account\":" + json_util::Escape(account_) +
                          ",\"password\":" + json_util::Escape(password_) + "}";
    HttpResponse resp;
    std::vector<std::string> headers{"Content-Type: application/json"};
    esp_err_t err = PerformPost(JoinUrl(backend_url_, "/api/auth/login"),
                                headers, payload, "application/json", resp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Login HTTP error: %s", esp_err_to_name(err));
        return false;
    }
    if (resp.status >= 400) {
        ESP_LOGE(TAG, "Login failed status=%d body=%s", resp.status,
                 resp.body.c_str());
        return false;
    }

    std::map<std::string, std::string> obj;
    if (!json_util::ParseFlatObject(resp.body.c_str(), obj)) {
        ESP_LOGE(TAG, "Login response unparseable: %s", resp.body.c_str());
        return false;
    }
    auto it = obj.find("accessToken");
    if (it == obj.end() || it->second.empty()) {
        ESP_LOGE(TAG, "Login response missing accessToken");
        return false;
    }
    access_token_ = it->second;
    ESP_LOGI(TAG, "Login ok, token len=%u", (unsigned)access_token_.size());
    return true;
}

bool AiClient::Login() {
    if (TokenAvailable()) {
        return true;
    }
    return DoLogin();
}

bool AiClient::Transcribe(const std::vector<uint8_t>& wav, std::string& out_text) {
    if (!Login()) {
        return false;
    }
    if (wav.empty()) {
        ESP_LOGE(TAG, "Empty wav");
        return false;
    }

    // multipart/form-data 上传 <name=audio>
    const std::string boundary = "----XiaoZhiFrameworkBoundary";
    std::string body;
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"audio\"; filename=\"rec.wav\"\r\n";
    body += "Content-Type: audio/wav\r\n\r\n";
    body.append((const char*)wav.data(), wav.size());
    body += "\r\n--" + boundary + "--\r\n";

    std::vector<std::string> headers{
        "Content-Type: multipart/form-data; boundary=" + boundary,
        "Authorization: Bearer " + access_token_,
    };
    HttpResponse resp;
    esp_err_t err = PerformPost(JoinUrl(backend_url_, "/api/asr/transcribe"),
                                headers, body, "application/json", resp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Transcribe HTTP error: %s", esp_err_to_name(err));
        return false;
    }
    if (resp.status == 401) {
        InvalidateToken();
        ESP_LOGW(TAG, "Transcribe 401, will re-login next call");
        return false;
    }
    if (resp.status >= 400) {
        ESP_LOGE(TAG, "Transcribe failed status=%d body=%s", resp.status,
                 resp.body.c_str());
        return false;
    }

    std::map<std::string, std::string> obj;
    if (!json_util::ParseFlatObject(resp.body.c_str(), obj)) {
        ESP_LOGE(TAG, "Transcribe response unparseable");
        return false;
    }
    auto it = obj.find("text");
    if (it == obj.end()) {
        ESP_LOGE(TAG, "Transcribe response missing text");
        return false;
    }
    out_text = it->second;
    return true;
}

bool AiClient::Chat(const std::string& text, const OnDeltaFn& on_delta,
                    std::string& out_conversation_id) {
    if (!Login()) {
        return false;
    }

    std::string payload = "{\"content\":" + json_util::Escape(text) + "}";
    std::vector<std::string> headers{
        "Content-Type: application/json",
        "Authorization: Bearer " + access_token_,
    };

    std::string url = JoinUrl(backend_url_, "/api/chat/device");
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 60000;  // 长流，首包等待放宽
    cfg.buffer_size = 2048;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        return false;
    }
    for (const auto& h : headers) {
        size_t colon = h.find(':');
        if (colon == std::string::npos) continue;
        esp_http_client_set_header(client, h.substr(0, colon).c_str(),
                                   h.substr(colon + 1).c_str());
    }
    esp_http_client_set_header(client, "Accept", "text/event-stream");
    esp_http_client_set_header(client, "Connection", "keep-alive");

    esp_err_t err = esp_http_client_open(client, (int)payload.size());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Chat open error: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }
    if (!payload.empty()) {
        esp_http_client_write(client, payload.data(), (int)payload.size());
    }

    int status = esp_http_client_fetch_headers(client);
    esp_http_client_get_status_code(client, &status);
    if (status == 401) {
        InvalidateToken();
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        ESP_LOGW(TAG, "Chat 401");
        return false;
    }
    if (status >= 400) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        ESP_LOGE(TAG, "Chat status=%d", status);
        return false;
    }

    SseParser parser([&](const std::string& event, const std::string& data) {
        if (event == "done") {
            std::map<std::string, std::string> obj;
            if (json_util::ParseFlatObject(data.c_str(), obj)) {
                auto it = obj.find("conversationId");
                if (it != obj.end()) {
                    out_conversation_id = it->second;
                }
            }
            return;
        }
        // 默认 data 事件：{"text":"增量"}
        std::map<std::string, std::string> obj;
        if (json_util::ParseFlatObject(data.c_str(), obj)) {
            auto it = obj.find("text");
            if (it != obj.end() && !it->second.empty()) {
                if (on_delta) {
                    on_delta(it->second.c_str());
                }
            }
        }
    });

    char buf[1024];
    int n;
    int total = 0;
    while ((n = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
        parser.Feed(buf, (size_t)n);
        total += n;
        if (total > 512 * 1024) break;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return true;
}

bool AiClient::Synthesize(const std::string& text, const OnAudioPcmFn& on_pcm) {
    if (!Login()) {
        return false;
    }
    std::string payload = "{\"text\":" + json_util::Escape(text) +
                          ",\"voiceMode\":\"preset\",\"voice\":" +
                          json_util::Escape(voice_) + "}";
    std::vector<std::string> headers{
        "Content-Type: application/json",
        "Authorization: Bearer " + access_token_,
    };

    std::string url = JoinUrl(backend_url_, "/api/tts/synthesize/stream");
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 60000;
    cfg.buffer_size = 2048;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        return false;
    }
    for (const auto& h : headers) {
        size_t colon = h.find(':');
        if (colon == std::string::npos) continue;
        esp_http_client_set_header(client, h.substr(0, colon).c_str(),
                                   h.substr(colon + 1).c_str());
    }
    esp_http_client_set_header(client, "Accept", "text/event-stream");
    esp_http_client_set_header(client, "Connection", "keep-alive");

    esp_err_t err = esp_http_client_open(client, (int)payload.size());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TTS open error: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }
    if (!payload.empty()) {
        esp_http_client_write(client, payload.data(), (int)payload.size());
    }

    int status = esp_http_client_fetch_headers(client);
    esp_http_client_get_status_code(client, &status);
    if (status == 401) {
        InvalidateToken();
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        ESP_LOGW(TAG, "TTS 401");
        return false;
    }
    if (status >= 400) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        ESP_LOGE(TAG, "TTS status=%d", status);
        return false;
    }

    SseParser parser([&](const std::string& event, const std::string& data) {
        if (event != "") {
            return;  // 关注默认 data 事件
        }
        std::map<std::string, std::string> obj;
        if (!json_util::ParseFlatObject(data.c_str(), obj)) {
            return;
        }
        auto ty = obj.find("type");
        if (ty == obj.end()) {
            return;
        }
        if (ty->second == "audio") {
            auto b64 = obj.find("audioBase64");
            if (b64 != obj.end() && !b64->second.empty()) {
                std::vector<uint8_t> pcm;
                if (base64::Decode(b64->second, pcm) && pcm.size() % 2 == 0 && on_pcm) {
                    on_pcm((const int16_t*)pcm.data(), pcm.size() / 2);
                }
            }
        }
        // metadata / done 忽略
    });

    char buf[1024];
    int n;
    while ((n = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
        parser.Feed(buf, (size_t)n);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return true;
}