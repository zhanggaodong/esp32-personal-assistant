#include "ai_client.h"

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <cJSON.h>
#include <cstring>
#include <initializer_list>
#include <utility>

// TTS 低带宽模式解码（78/esp-opus-encoder 组件，60ms 帧单声道）
#include <opus_decoder.h>

#include "../config/config_store.h"
#include "../web/json_util.h"
#include "../device_log.h"
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

bool BuildJsonObject(
        std::initializer_list<std::pair<const char*, std::string>> fields,
        std::string& out) {
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return false;
    }
    for (const auto& field : fields) {
        if (cJSON_AddStringToObject(root, field.first,
                                    field.second.c_str()) == nullptr) {
            cJSON_Delete(root);
            return false;
        }
    }

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == nullptr) {
        return false;
    }
    out.assign(json);
    cJSON_free(json);
    return true;
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
    // 启用全局 CA bundle：访问 https 时必须校验证书（依赖设备已联网校时）。
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

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

    esp_http_client_fetch_headers(client);
    out.status = esp_http_client_get_status_code(client);

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

// 后端全局响应拦截器把成功响应包成 {"success":true,"data":{...},"meta":{...}}。
// 为兼容未来可能去掉包装的响应，同一字段先查顶层、再查 data 内层。
// out_found = 是否在任意一层找到该字段（找不到时 out_value 不变）。
void ReadResponseField(const std::string& body, const std::string& key,
                       std::string& out_value, bool& out_found) {
    out_found = false;
    cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return;
    }

    cJSON* value = cJSON_GetObjectItemCaseSensitive(root, key.c_str());
    if (!cJSON_IsString(value)) {
        cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
        if (cJSON_IsObject(data)) {
            value = cJSON_GetObjectItemCaseSensitive(data, key.c_str());
        }
    }
    if (cJSON_IsString(value) && value->valuestring != nullptr) {
        out_value = value->valuestring;
        out_found = true;
    }
    cJSON_Delete(root);
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

// ---- TTS 低带宽(opus)二进制流 ----
// 线契约（与后端 tts-opus.transform.ts 一致）：
//   头 6B: 'V''O' | ver u8 | channels u8 | sampleRate u16LE
//   帧循环: type u8 (1=opus 2=done 3=error) | len u16LE | payload

// 带缓冲的字节读取器：对 esp_http_client_read 封装"精确读 n 字节"
class HttpByteReader {
public:
    explicit HttpByteReader(esp_http_client_handle_t client)
        : client_(client) {}
    void Seed(const char* data, size_t len) { buf_.append(data, len); }
    bool ReadExactly(size_t n, std::string& out) {
        while (buf_.size() < n) {
            char tmp[1024];
            int r = esp_http_client_read(client_, tmp, sizeof(tmp));
            if (r <= 0) {
                return false;  // 连接关闭/超时
            }
            buf_.append(tmp, (size_t)r);
        }
        out.assign(buf_, 0, n);
        buf_.erase(0, n);
        return true;
    }

private:
    esp_http_client_handle_t client_;
    std::string buf_;
};

// 解析 opus 流（嗅探已确认 "VO" 头并消费前 2 字节）。EOF 视为正常结束。
bool DecodeOpusStream(esp_http_client_handle_t client,
                      const AiClient::OnAudioPcmFn& on_pcm,
                      const std::function<bool()>& is_cancelled) {
    HttpByteReader reader(client);
    std::string hdr;
    if (!reader.ReadExactly(4, hdr)) {
        ESP_LOGE(TAG, "Opus stream header truncated");
        return false;
    }
    const uint8_t channels = (uint8_t)hdr[1];
    int rate = (int)((uint8_t)hdr[2] | ((uint8_t)hdr[3] << 8));
    if (channels != 1 || rate < 8000 || rate > 48000) {
        ESP_LOGE(TAG, "Opus stream unsupported: ch=%u rate=%d", channels, rate);
        return false;
    }
    OpusDecoderWrapper decoder(rate, 1, 60);

    for (;;) {
        if (is_cancelled && is_cancelled()) {
            DeviceLog::Log('I', "AiClient", "TTS 请求已由电源键取消");
            return false;
        }
        std::string fh;
        if (!reader.ReadExactly(3, fh)) {
            break;  // EOF：服务器结束响应
        }
        const uint8_t type = (uint8_t)fh[0];
        uint16_t len = (uint16_t)((uint8_t)fh[1] | ((uint8_t)fh[2] << 8));
        if (len > 4096) {
            ESP_LOGE(TAG, "Opus frame too large: %u", len);
            return false;
        }
        std::string payload;
        if (len > 0 && !reader.ReadExactly(len, payload)) {
            break;
        }
        if (type == 2) {  // done
            DeviceLog::Log('I', "AiClient", "TTS 合成完成");
            break;
        }
        if (type == 3) {  // error
            ESP_LOGE(TAG, "tts server error: %s", payload.c_str());
            DeviceLog::Log('E', "AiClient", "TTS 服务端报错");
            return false;
        }
        if (type != 1 || len == 0) {
            continue;
        }
        std::vector<uint8_t> opus(payload.begin(), payload.end());
        std::vector<int16_t> pcm;
        if (!decoder.Decode(std::move(opus), pcm)) {
            // 单帧解码失败会丢失 60ms 音频（听感为一个字被吞），记录下来便于统计
            ESP_LOGW(TAG, "opus frame decode failed, skipped (len=%u)", len);
            continue;
        }
        if (!pcm.empty() && on_pcm) {
            on_pcm(pcm.data(), pcm.size());
        }
    }
    return true;
}

}  // namespace

AiClient& AiClient::Instance() {
    static AiClient instance;
    return instance;
}

void AiClient::UpdateConfig() {
    std::lock_guard<std::mutex> lock(login_mutex_);
    ConfigStore& store = ConfigStore::Instance();
    std::string old_url = backend_url_;
    std::string old_account = account_;
    std::string old_password = password_;
    backend_url_ = store.Get("ai.backend_url");
    account_ = store.Get("ai.account");
    password_ = store.Get("ai.password");
    voice_ = store.Get("ai.voice");
    // 地址/账号/密码任一变化：立即清除旧 token，下一次对话使用新配置重新登录
    if (old_url != backend_url_ || old_account != account_ ||
        old_password != password_) {
        InvalidateToken();
    }
    if (!IsConfigured()) {
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

    std::string payload;
    if (!BuildJsonObject({{"account", account_}, {"password", password_}}, payload)) {
        ESP_LOGE(TAG, "Failed to build login JSON");
        DeviceLog::Log('E', "AiClient", "登录请求构造失败");
        return false;
    }
    HttpResponse resp;
    std::vector<std::string> headers{"Content-Type: application/json"};
    esp_err_t err = PerformPost(JoinUrl(backend_url_, "/api/auth/login"),
                                headers, payload, "application/json", resp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Login HTTP error: %s", esp_err_to_name(err));
        DeviceLog::Log('E', "AiClient", "登录网络/握手失败(%s)", esp_err_to_name(err));
        return false;
    }
    if (resp.status >= 400) {
        ESP_LOGE(TAG, "Login failed status=%d body=%s", resp.status,
                 resp.body.c_str());
        DeviceLog::Log('E', "AiClient", "登录失败 HTTP %d", resp.status);
        return false;
    }

    std::string token;
    bool found = false;
    ReadResponseField(resp.body, "accessToken", token, found);
    if (!found || token.empty()) {
        ESP_LOGE(TAG, "Login response missing accessToken (body len=%u)",
                 (unsigned)resp.body.size());
        DeviceLog::Log('E', "AiClient", "登录响应缺少 accessToken");
        return false;
    }
    access_token_ = token;
    ESP_LOGI(TAG, "Login ok, token len=%u", (unsigned)access_token_.size());
    DeviceLog::Log('I', "AiClient", "登录成功");
    return true;
}

bool AiClient::Login() {
    if (TokenAvailable()) {
        return true;
    }
    // 并发登录锁：多个任务同时首次登录时只允许一个真正发请求
    std::lock_guard<std::mutex> lock(login_mutex_);
    if (TokenAvailable()) {
        return true;  // 等待期间其它任务已完成登录
    }
    return DoLogin();
}

std::string AiClient::VoiceSocketUrl() const {
    std::string url = backend_url_;
    // http -> ws，https -> wss（按完整 scheme 长度替换，避免残留 "://"）
    if (url.compare(0, 8, "https://") == 0) {
        url.replace(0, 8, "wss://");
    } else if (url.compare(0, 7, "http://") == 0) {
        url.replace(0, 7, "ws://");
    }
    // 归一化尾随斜杠与路径前缀
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    return url + "/api/voice/device";
}

bool AiClient::Transcribe(const std::vector<uint8_t>& wav, std::string& out_text) {
    // 401 语义：清除 token → 重登 → 仅重试一次；再失败由调用方提示账号密码
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!Login()) {
            return false;
        }
        if (DoTranscribe(wav, out_text)) {
            return true;
        }
        if (TokenAvailable()) {
            return false;  // 失败原因不是 401（token 仍在），不重试
        }
        ESP_LOGW(TAG, "transcribe got 401, re-login and retry once");
    }
    return false;
}

bool AiClient::Chat(const std::string& text, const OnDeltaFn& on_delta,
                    std::string& out_conversation_id) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!Login()) {
            return false;
        }
        if (DoChat(text, on_delta, out_conversation_id)) {
            return true;
        }
        if (TokenAvailable()) {
            return false;
        }
        ESP_LOGW(TAG, "chat got 401, re-login and retry once");
    }
    return false;
}

bool AiClient::Synthesize(const std::string& text, const OnAudioPcmFn& on_pcm) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!Login()) {
            return false;
        }
        if (DoSynthesize(text, on_pcm)) {
            return true;
        }
        if (TokenAvailable()) {
            return false;
        }
        ESP_LOGW(TAG, "tts got 401, re-login and retry once");
    }
    return false;
}

bool AiClient::DoTranscribe(const std::vector<uint8_t>& wav, std::string& out_text) {
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
        DeviceLog::Log('E', "AiClient", "ASR 网络失败(%s)", esp_err_to_name(err));
        return false;
    }
    if (resp.status == 401) {
        InvalidateToken();
        ESP_LOGW(TAG, "Transcribe 401, will re-login next call");
        DeviceLog::Log('W', "AiClient", "ASR token 失效，重新登录");
        return false;
    }
    if (resp.status >= 400) {
        ESP_LOGE(TAG, "Transcribe failed status=%d body=%s", resp.status,
                 resp.body.c_str());
        DeviceLog::Log('E', "AiClient", "ASR 失败 HTTP %d", resp.status);
        return false;
    }

    std::string text;
    bool found = false;
    ReadResponseField(resp.body, "text", text, found);
    if (!found || text.empty()) {
        ESP_LOGE(TAG, "Transcribe response missing text: %s", resp.body.c_str());
        DeviceLog::Log('E', "AiClient", "ASR 响应缺少 text");
        return false;
    }
    out_text = text;
    DeviceLog::Log('I', "AiClient", "ASR 识别完成: %s", out_text.c_str());
    return true;
}

bool AiClient::DoChat(const std::string& text, const OnDeltaFn& on_delta,
                      std::string& out_conversation_id) {
    if (!Login()) {
        return false;
    }

    std::string payload;
    if (!BuildJsonObject({{"content", text}}, payload)) {
        ESP_LOGE(TAG, "Failed to build chat JSON");
        DeviceLog::Log('E', "AiClient", "Chat 请求构造失败");
        return false;
    }
    std::vector<std::string> headers{
        "Content-Type: application/json",
        "Authorization: Bearer " + access_token_,
    };

    std::string url = JoinUrl(backend_url_, "/api/chat/device");
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 60000;  // 长流，首包等待放宽
    cfg.buffer_size = 8192;  // SSE 大块吞吐：减少 TLS 解密与拷贝次数
    cfg.crt_bundle_attach = esp_crt_bundle_attach;  // https 证书校验

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
        DeviceLog::Log('E', "AiClient", "Chat 连接失败(%s)", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }
    if (!payload.empty()) {
        esp_http_client_write(client, payload.data(), (int)payload.size());
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status == 401) {
        InvalidateToken();
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        ESP_LOGW(TAG, "Chat 401");
        DeviceLog::Log('W', "AiClient", "Chat token 失效，重新登录");
        return false;
    }
    if (status >= 400) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        ESP_LOGE(TAG, "Chat status=%d", status);
        DeviceLog::Log('E', "AiClient", "Chat 失败 HTTP %d", status);
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
            DeviceLog::Log('I', "AiClient", "Chat 回复结束");
            return;
        }
        if (event == "error") {
            DeviceLog::Log('E', "AiClient", "Chat 服务端报错: %s", data.c_str());
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

    // 仅 headless 工作线程串行调用，用静态缓冲省任务栈；2048 配合客户端
    // 8KB 内部缓冲减少每字节摊销的 TLS 解密调用次数。
    static char buf[2048];
    int n;
    int total = 0;
    while (!request_cancelled_.load() &&
           (n = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
        parser.Feed(buf, (size_t)n);
        total += n;
        if (total > 512 * 1024) break;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return !request_cancelled_.load();
}

bool AiClient::DoSynthesize(const std::string& text, const OnAudioPcmFn& on_pcm) {
    if (!Login()) {
        return false;
    }
    std::string payload;
    if (!BuildJsonObject({{"text", text}, {"voiceMode", "preset"},
                          {"voice", voice_}}, payload)) {
        ESP_LOGE(TAG, "Failed to build TTS JSON");
        DeviceLog::Log('E', "AiClient", "TTS 请求构造失败");
        return false;
    }
    std::vector<std::string> headers{
        "Content-Type: application/json",
        "Authorization: Bearer " + access_token_,
    };

    std::string url = JoinUrl(backend_url_, "/api/tts/synthesize/stream") +
                      "?audio=opus";
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 60000;
    cfg.buffer_size = 8192;  // 音频大块吞吐：减少 TLS 解密与拷贝次数
    cfg.crt_bundle_attach = esp_crt_bundle_attach;  // https 证书校验

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
        DeviceLog::Log('E', "AiClient", "TTS 连接失败(%s)", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }
    if (!payload.empty()) {
        esp_http_client_write(client, payload.data(), (int)payload.size());
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status == 401) {
        InvalidateToken();
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        ESP_LOGW(TAG, "TTS 401");
        DeviceLog::Log('W', "AiClient", "TTS token 失效，重新登录");
        return false;
    }
    if (status >= 400) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        ESP_LOGE(TAG, "TTS status=%d", status);
        DeviceLog::Log('E', "AiClient", "TTS 失败 HTTP %d", status);
        return false;
    }

    // 嗅探前两字节：新后端返回 "VO"（opus 二进制流，约 1/11 带宽），
    // 旧后端/APP 兼容格式为 SSE 文本（"data:..."）。自动适配无需配置。
    std::string sse_seed;
    {
        char head[2] = {0, 0};
        size_t got = 0;
        while (got < sizeof(head)) {
            int r = esp_http_client_read(client, head + got,
                                         sizeof(head) - got);
            if (r <= 0) {
                break;
            }
            got += (size_t)r;
        }
        if (got >= 2 && head[0] == 'V' && head[1] == 'O') {
            bool ok = DecodeOpusStream(client, on_pcm, [this]() {
                return request_cancelled_.load();
            });
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ok;
        }
        sse_seed.assign(head, got);
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
        } else if (ty->second == "done") {
            DeviceLog::Log('I', "AiClient", "TTS 合成完成");
        }
        // metadata 忽略
    });

    // 先把嗅探阶段已读入的字节交给 SSE 解析器，再继续常规读取
    parser.Feed(sse_seed.data(), sse_seed.size());
    // 仅 headless 工作线程串行调用，用静态缓冲省任务栈（同 DoChat）
    static char buf[2048];
    int n;
    while (!request_cancelled_.load() &&
           (n = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
        parser.Feed(buf, (size_t)n);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return !request_cancelled_.load();
}
