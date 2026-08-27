#include "voice/device_voice_client.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <web_socket.h>

// opus 下行解码（78/esp-opus-encoder 组件；解码需大栈，故在专用任务中执行）
#include <opus_decoder.h>

#include "../ai/ai_client.h"
#include "../device_log.h"
#include "board.h"

#define TAG "VoiceClient"

namespace {

// 连接握手等待 hello 的超时。
constexpr int kHelloTimeoutMs = 10000;
// 空闲关闭阈值：超过后下一次 EnsureConnected 会主动重建（服务端 120s 关闭）。
constexpr int64_t kIdleCloseMs = 120000;
// 建连用的 connect_id（沿用项目惯例用 1）。
constexpr int kConnectId = 1;
// 握手事件位。
constexpr EventBits_t kHelloOkBit = BIT0;
constexpr EventBits_t kHelloFailBit = BIT1;

}  // namespace

DeviceVoiceClient& DeviceVoiceClient::Instance() {
    static DeviceVoiceClient instance;
    return instance;
}

DeviceVoiceClient::~DeviceVoiceClient() = default;

int64_t DeviceVoiceClient::NowMs() {
    return esp_timer_get_time() / 1000;
}

void DeviceVoiceClient::UpdateConfig() {
    std::unique_ptr<WebSocket> old;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        old = std::move(ws_);  // 锁内只移交所有权
        // 作废旧代次：旧连接迟到的任何回调都会被代次检查丢弃。
        connection_generation_.fetch_add(1);
        active_connection_generation_ = 0;
        disconnected_notified_ = false;
        socket_cleanup_pending_ = false;
    }
    old.reset();  // 锁外析构
    active_turn_.store(0);
    {
        std::lock_guard<std::mutex> dec_lock(dec_mutex_);
        dec_queue_.clear();  // 丢弃未解码的旧格式帧
    }
}

bool DeviceVoiceClient::IsConnected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ws_ && ws_->IsConnected();
}

void DeviceVoiceClient::SetPcmCallback(OnPcmFn cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_pcm_ = std::move(cb);
}

void DeviceVoiceClient::SetEventCallback(OnEventFn cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_event_ = std::move(cb);
}

void DeviceVoiceClient::SetDisconnectedCallback(OnDisconnectedFn cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_disconnected_ = std::move(cb);
}

// 统一断线/失败出口：只"标记 + 一次性通知控制器"，绝不析构连接对象。
// 本方法可能运行在 WebSocket 接收任务上下文（OnDisconnected / bad_frame），
// 在该任务里等待其自身退出会触发 EspSsl "Failed to wait for receive task
// exit" 断言；析构统一延后到控制器任务调用 ReapDisconnectedSocket()。
void DeviceVoiceClient::HandleSocketDisconnected(uint32_t generation,
                                                 const char* reason) {
    OnDisconnectedFn cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 代次为 0 说明还没有任何建连，属于误触发；旧代次迟到的回调同样忽略。
        if (generation == 0 || generation != active_connection_generation_ ||
            disconnected_notified_) {
            return;
        }
        disconnected_notified_ = true;
        socket_cleanup_pending_ = true;
        active_turn_.store(0);
        cb = on_disconnected_;
    }
    ESP_LOGW(TAG, "socket disconnected generation=%u reason=%s",
             (unsigned)generation, reason);
    DeviceLog::Log('W', "VoiceClient", "ws 已断开: generation=%u reason=%s",
                   (unsigned)generation, reason);

    if (cb) {
        cb(reason);  // 锁外回调，避免死锁
    }
}

void DeviceVoiceClient::ReapDisconnectedSocket() {
    std::unique_ptr<WebSocket> old;
    uint32_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!socket_cleanup_pending_) {
            return;  // 幂等：没有待回收的断线残留
        }
        old = std::move(ws_);
        socket_cleanup_pending_ = false;
        generation = active_connection_generation_;
    }

    const int64_t reap_start_ms = NowMs();
    old.reset();  // 析构发生在控制器任务，可安全等待接收任务退出

    // 固件启用 CONFIG_NEWLIB_NANO_FORMAT：printf 不支持 64 位格式符，
    // 耗时一律以 unsigned 打印（毫秒级耗时远在范围内）。
    ESP_LOGI(TAG, "socket cleanup complete generation=%u took=%ums",
             (unsigned)generation,
             (unsigned)(NowMs() - reap_start_ms));
    DeviceLog::Log('I', "VoiceClient", "ws 延迟回收完成: generation=%u",
                   (unsigned)generation);
}

bool DeviceVoiceClient::DoConnect() {
    if (hello_evt_ == nullptr) {
        hello_evt_ = xEventGroupCreate();
        if (hello_evt_ == nullptr) {
            ESP_LOGE(TAG, "failed to create event group");
            return false;
        }
    }
    xEventGroupClearBits(hello_evt_, kHelloOkBit | kHelloFailBit);

    AiClient& ai = AiClient::Instance();
    const std::string token = ai.AccessToken();
    if (token.empty()) {
        ESP_LOGW(TAG, "no token before websocket connect");
        return false;
    }

    auto network = Board::GetInstance().GetNetwork();
    auto ws = network->CreateWebSocket(kConnectId);
    if (ws == nullptr) {
        ESP_LOGE(TAG, "failed to create websocket");
        return false;
    }

    // 新连接获得新代次：旧连接的迟到回调经代次检查直接丢弃。
    const uint32_t generation = connection_generation_.fetch_add(1) + 1;

    ws->SetHeader("Authorization", ("Bearer " + token).c_str());
    ws->SetHeader("Device-Protocol-Version",
                  std::to_string(voice::kProtocolVersion).c_str());
    ws->OnData([this, generation](const char* data, size_t len, bool binary) {
        if (generation != connection_generation_.load()) {
            return;  // 旧连接迟到的回调：忽略
        }
        last_activity_ms_.store(NowMs());
        if (binary) {
            HandleBinary(data, len);
        } else {
            HandleText(data, len);
        }
        last_activity_ms_.store(NowMs());
    });
    ws->OnDisconnected([this, generation]() {
        xEventGroupSetBits(hello_evt_, kHelloFailBit);
        HandleSocketDisconnected(generation, "server_closed");
    });

    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_connection_generation_ = generation;
        disconnected_notified_ = false;
        socket_cleanup_pending_ = false;
        ws_ = std::move(ws);
    }

    const std::string url = ai.VoiceSocketUrl();
    ESP_LOGI(TAG, "connecting to voice websocket ...");
    DeviceLog::Log('I', "VoiceClient", "ws 正在连接服务器(升级握手)");
    if (ws_ == nullptr || !ws_->Connect(url.c_str())) {
        ESP_LOGE(TAG, "websocket connect failed, code=%d",
                 ws_ ? ws_->GetLastError() : -1);
        DeviceLog::Log('E', "VoiceClient", "ws 连接失败(TCP/TLS 层)");
        // 建连失败发生在调用方任务（绝不是该 socket 的接收任务），可就地回收；
        // 规则不变：锁内移交，锁外析构。
        std::unique_ptr<WebSocket> failed;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            failed = std::move(ws_);
        }
        failed.reset();
        return false;
    }

    // 等待服务端 hello（握手完成）。
    EventBits_t bits = xEventGroupWaitBits(hello_evt_, kHelloOkBit | kHelloFailBit,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(kHelloTimeoutMs));
    if (bits & kHelloOkBit) {
        ESP_LOGI(TAG, "websocket handshake complete");
        DeviceLog::Log('I', "VoiceClient", "ws 握手成功");
        return true;
    }
    ESP_LOGE(TAG, "websocket handshake timeout/failed, bits=%u", (unsigned)bits);
    DeviceLog::Log('E', "VoiceClient",
                   bits & kHelloFailBit ? "ws 握手被服务器拒绝" : "ws 握手超时(多为反代未放行 WebSocket)");
    HandleSocketDisconnected(generation, "handshake_failed");
    ReapDisconnectedSocket();
    return false;
}

bool DeviceVoiceClient::EnsureConnected() {
    // 已连接但空闲过久：服务端多半已关，主动重建。
    {
        std::unique_ptr<WebSocket> stale;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (ws_ && ws_->IsConnected()) {
                if (NowMs() - last_activity_ms_.load() > kIdleCloseMs) {
                    ESP_LOGI(TAG, "voice socket idle, rebuild");
                    stale = std::move(ws_);  // 锁内移交，锁外析构
                } else {
                    return true;
                }
            }
        }
        stale.reset();  // 调用方任务析构，可安全等待接收任务退出
    }

    // 至多重试一次；首次失败后清掉 token，第二次以新 token 真正重登重连。
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!AiClient::Instance().Login()) {
            return false;
        }
        if (DoConnect()) {
            return true;
        }
        std::unique_ptr<WebSocket> leftover;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            leftover = std::move(ws_);
        }
        leftover.reset();
        if (attempt == 0) {
            AiClient::Instance().InvalidateToken();
            ESP_LOGW(TAG, "voice connect failed, re-login and retry once");
        }
    }
    // 全部失败：按当前代次标记断线并就地回收（调用方是控制器任务）。
    HandleSocketDisconnected(connection_generation_.load(), "connect_failed");
    ReapDisconnectedSocket();
    return false;
}

bool DeviceVoiceClient::StartTurn(uint32_t turn_id,
                                  const std::string& conversation_id,
                                  const std::string& voice,
                                  const std::string& language,
                                  uint32_t max_record_seconds) {
    const std::string msg =
        voice::BuildTurnStart(turn_id, conversation_id, voice, language,
                              max_record_seconds);
    if (msg.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ws_ || !ws_->IsConnected() || !ws_->Send(msg)) {
        return false;
    }
    active_turn_.store(turn_id);
    return true;
}

bool DeviceVoiceClient::StopTurn(uint32_t turn_id) {
    const std::string msg = voice::BuildTurnStop(turn_id);
    if (msg.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ws_ || !ws_->IsConnected()) {
        return false;
    }
    return ws_->Send(msg);
}

bool DeviceVoiceClient::CancelTurn(uint32_t turn_id, const std::string& reason) {
    active_turn_.store(0);  // 立即使旧轮迟到输出失效
    const std::string msg = voice::BuildTurnCancel(turn_id, reason);
    if (msg.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ws_ || !ws_->IsConnected()) {
        return false;
    }
    return ws_->Send(msg);
}

bool DeviceVoiceClient::SendInputPcm(uint32_t turn_id, uint32_t sequence,
                                     bool first, bool last, const int16_t* pcm,
                                     size_t count) {
    std::string frame;
    uint8_t flags = 0;
    if (first) flags |= voice::kFrameFlagFirst;
    if (last) flags |= voice::kFrameFlagLast;
    if (pcm == nullptr || count == 0 ||
        count * sizeof(int16_t) > voice::kMaxPayloadBytes) {
        return false;
    }
    if (!voice::EncodeBinaryFrame(
            voice::FrameType::kInputPcm, flags, turn_id, sequence,
            reinterpret_cast<const uint8_t*>(pcm), count * sizeof(int16_t),
            frame)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!ws_ || !ws_->IsConnected()) {
        return false;
    }
    return ws_->Send(frame.data(), frame.size(), true, true);
}

void DeviceVoiceClient::Disconnect() {
    std::unique_ptr<WebSocket> old;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_turn_.store(0);
        old = std::move(ws_);  // 锁内移交，锁外析构
        disconnected_notified_ = false;
        socket_cleanup_pending_ = false;
    }
    old.reset();
}

void DeviceVoiceClient::HandleBinary(const char* data, size_t len) {
    voice::ParsedFrame frame;
    const voice::FrameParseResult rc =
        voice::DecodeBinaryFrame(reinterpret_cast<const uint8_t*>(data), len,
                                 frame);
    if (rc != voice::FrameParseResult::kOk) {
        ESP_LOGW(TAG, "bad binary frame rc=%d, closing", static_cast<int>(rc));
        // 接收任务上下文：只标记断线，对象延后由控制器任务回收。
        HandleSocketDisconnected(connection_generation_.load(), "bad_frame");
        return;
    }
    if (frame.type == voice::FrameType::kOutputOpus) {
        // opus 帧：只入队，解码在专用大栈任务中做（libopus 吃栈，
        // WS 接收任务仅 4KB 栈，绝不能在这里直接解码）。
        if (frame.turn_id == active_turn_.load()) {
            EnqueueOpusFrame(frame.turn_id, frame.payload, frame.payload_size);
        }
        return;
    }
    if (frame.type != voice::FrameType::kOutputPcm) {
        return;  // 上行帧不会由服务端下发，忽略
    }
    // turn_id 不匹配当前轮：静默丢弃，绝不能进入播放。
    if (frame.turn_id != active_turn_.load()) {
        return;
    }
    const size_t samples = frame.payload_size / sizeof(int16_t);
    OnPcmFn cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = on_pcm_;
    }
    if (cb) {
        cb(frame.turn_id, frame.sequence, frame.first(), frame.last(),
           reinterpret_cast<const int16_t*>(frame.payload), samples);
    }
}

void DeviceVoiceClient::EnqueueOpusFrame(uint32_t turn_id,
                                         const uint8_t* data, size_t len) {
    if (len == 0) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(dec_mutex_);
        if (dec_queue_.size() >= kMaxQueuedOpusFrames) {
            // 解码跟不上时丢最旧：追上实时比保住历史更重要
            dec_queue_.pop_front();
        }
        PendingOpusFrame frame;
        frame.data.assign(data, data + len);
        frame.turn_id = turn_id;
        dec_queue_.push_back(std::move(frame));
    }
    if (dec_task_ == nullptr) {
        // 惰性创建、常驻。26KB 栈与小智的 opus 编解码任务一致（libopus 需要）。
        if (xTaskCreate(OpusDecodeTaskTrampoline, "opus_dec", 26624, this, 5,
                        &dec_task_) != pdPASS) {
            ESP_LOGE(TAG, "failed to create opus decode task");
            return;
        }
    }
    xTaskNotifyGive(dec_task_);
}

void DeviceVoiceClient::OpusDecodeTaskTrampoline(void* arg) {
    static_cast<DeviceVoiceClient*>(arg)->OpusDecodeLoop();
    vTaskDelete(nullptr);
}

void DeviceVoiceClient::OpusDecodeLoop() {
    for (;;) {
        ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(100));
        for (;;) {
            PendingOpusFrame frame;
            {
                std::lock_guard<std::mutex> lock(dec_mutex_);
                if (dec_queue_.empty()) {
                    break;
                }
                frame = std::move(dec_queue_.front());
                dec_queue_.pop_front();
            }
            if (!tts_decoder_) {
                tts_decoder_ = std::make_unique<OpusDecoderWrapper>(24000, 1, 60);
            }
            if (frame.turn_id != decoded_turn_) {
                decoded_turn_ = frame.turn_id;
                tts_decoder_->ResetState();  // 新一轮从干净解码状态开始
            }
            std::vector<int16_t> pcm;
            if (!tts_decoder_->Decode(std::move(frame.data), pcm) ||
                pcm.empty()) {
                continue;
            }
            // 插话/取消后 active_turn 已变，过期输出直接丢弃（不入播放队列）
            if (frame.turn_id != active_turn_.load()) {
                continue;
            }
            OnPcmFn cb;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                cb = on_pcm_;
            }
            if (cb) {
                cb(frame.turn_id, 0, false, false, pcm.data(), pcm.size());
            }
        }
    }
}

void DeviceVoiceClient::HandleText(const char* data, size_t len) {
    const voice::MessageType mt = voice::ClassifyMessage(data, len);
    if (mt == voice::MessageType::kHello) {
        xEventGroupSetBits(hello_evt_, kHelloOkBit);
        return;  // hello 只用于握手，不再外发
    }

    voice::ServerMessage msg;
    if (!voice::ParseServerMessage(data, len, msg)) {
        return;
    }

    // turn.done / turn.cancelled 表示活动轮已结束，清掉活动轮标记。
    if (mt == voice::MessageType::kTurnDone ||
        mt == voice::MessageType::kTurnCancelled) {
        active_turn_.store(0);
    }

    OnEventFn cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = on_event_;
    }
    if (cb) {
        cb(msg);
    }
}
