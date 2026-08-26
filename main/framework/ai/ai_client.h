#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <vector>

// AiClient：封装与用户自定义 NestJS 后端（ai-agent-assistant）的对接。
// 鉴权方式：网页配置里填后端地址 + 账号 + 密码，设备运行时自行调 /api/auth/login
// 拿 JWT，携带 Authorization: Bearer <token> 访问各接口；token 失效时自动重登。
//
// 依赖的四个接口（后端全局前缀 /api）：
//   - POST /api/auth/login             {account,password} -> {accessToken}
//   - POST /api/asr/transcribe         multipart(audio=wav) -> {text}
//   - POST /api/chat/device            SSE: data:{"text":"增量"} / event:done
//   - POST /api/tts/synthesize/stream  SSE: metadata / audio{audioBase64} / done
//
// 401 语义：任一接口返回 401 时清除 token → 重新登录一次 → 仅重试当前阶段
// 一次；再次失败则由调用方提示用户检查账号密码（不会死循环）。
class AiClient {
public:
    using OnDeltaFn = std::function<void(const char* delta)>;
    using OnAudioPcmFn = std::function<void(const int16_t* pcm, size_t count)>;

    static AiClient& Instance();

    // 从 ConfigStore 刷新后端地址/账号/密码/音色（配置变更后调用）。
    // 地址/账号/密码任一变化都会使内存中的旧 token 立即失效。
    void UpdateConfig();

    // 后端配置（地址+账号+密码）是否完整，用于无屏按键前的"未配置"判断
    bool IsConfigured() const {
        return !backend_url_.empty() && !account_.empty() && !password_.empty();
    }

    // 登录拿 JWT（token 已存在且仍在用时立即返回 true；并发登录已加锁）
    bool Login();

    bool TokenAvailable() const { return !access_token_.empty(); }
    const std::string& AccessToken() const { return access_token_; }

    // 使内存中的 JWT 立即失效（下次访问会重新登录）。
    void InvalidateToken() { access_token_.clear(); }

    // 语音 WebSocket 地址：把 backend_url 的 http(s) 换成 ws(s)，并拼 /api/voice/device。
    std::string VoiceSocketUrl() const;

    // 语音识别：wav(16k/16bit/mono) -> out_text
    bool Transcribe(const std::vector<uint8_t>& wav, std::string& out_text);

    // AI 对话：流式纯文本增量经 on_delta 回调；out_conversation_id 用于续聊。
    bool Chat(const std::string& text, const OnDeltaFn& on_delta,
              std::string& out_conversation_id);

    // 语音合成：PCM16(24000Hz/单声道/16bit) 分片经 on_pcm 回调。
    bool Synthesize(const std::string& text, const OnAudioPcmFn& on_pcm);

private:
    AiClient() = default;

    bool DoLogin();

    bool DoTranscribe(const std::vector<uint8_t>& wav, std::string& out_text);
    bool DoChat(const std::string& text, const OnDeltaFn& on_delta,
                std::string& out_conversation_id);
    bool DoSynthesize(const std::string& text, const OnAudioPcmFn& on_pcm);

    // 底层 HTTP POST。返回取到的整段响应体（非 SSE）到 out_body。
    bool httpPostJson(const std::string& path, const std::string& json_body,
                      std::string& out_status_err, std::string& code_body_or_err);

    std::string backend_url_;
    std::string account_;
    std::string password_;
    std::string voice_;
    std::string access_token_;
    std::mutex login_mutex_;  // 登录并发锁（仅保护 access_token_ 的获取）
};