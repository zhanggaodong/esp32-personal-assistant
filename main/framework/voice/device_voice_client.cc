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
    finished_turn_.store(0);
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

void DeviceVoiceClient::SetOutputDrainedCallback(OnOutputDrainedFn cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_drained_ = std::move(cb);
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
        finished_turn_.store(0);  // 断线无排空可言，残余帧全部失效
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
    // 新一轮开始：上一轮若还在等排空（finished_turn_ 未清），立即作废——
    // 插话打断的语义是丢弃旧轮残余，而不是继续播完。
    finished_turn_.store(0);
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
    active_turn_.store(0);    // 立即使旧轮迟到输出失效
    finished_turn_.store(0);  // 取消语义：残余帧丢弃，不做排空
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
        finished_turn_.store(0);
        old = std::move(ws_);  // 锁内移交，锁外析构
        disconnected_notified_ = false;
        socket_cleanup_pending_ = false;
    }
    old.reset();
}

bool DeviceVoiceClient::IsTurnOutputAlive(uint32_t turn_id) const {
    if (turn_id == 0) {
        return false;
    }
    // 活动轮正常接受；finished_turn_ 是已收 turn.done、等待解码排空的轮，
    // 其残余音频必须继续放行（否则长回答尾部被整批丢弃）。
    return turn_id == active_turn_.load() || turn_id == finished_turn_.load();
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
        // 活动轮与"待排空轮"（已收 turn.done）都接受。
        if (IsTurnOutputAlive(frame.turn_id)) {
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
    // 队列满：阻塞等待解码消费，绝不丢帧。旧实现"丢最旧"会在长回答时
    // 挖掉紧邻播放位置的音频（中段吞字）。这里也是唯一能让流控反向节流
    // 服务器的环节：WS 接收任务停读 → TCP 窗口收窄 → 服务器发送被节流。
    // 等待由三个条件打破：解码出队腾位（常规，约一个帧时长内）、轮次失效
    // （插话/取消/新轮）、连接断开。
    for (;;) {
        bool queued = false;
        {
            std::lock_guard<std::mutex> lock(dec_mutex_);
            if (dec_queue_.size() < kMaxQueuedOpusFrames) {
                PendingOpusFrame frame;
                frame.data.assign(data, data + len);
                frame.turn_id = turn_id;
                dec_queue_.push_back(std::move(frame));
                queued = true;
            }
        }
        if (queued) {
            break;
        }
        if (!IsTurnOutputAlive(turn_id) || !IsConnected()) {
            return;  // 轮次已失效或连接已断：迟到帧无意义，直接丢弃
        }
        vTaskDelay(pdMS_TO_TICKS(20));
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
            bool has_frame = false;
            {
                std::lock_guard<std::mutex> lock(dec_mutex_);
                if (!dec_queue_.empty()) {
                    frame = std::move(dec_queue_.front());
                    dec_queue_.pop_front();
                    has_frame = true;
                }
            }
            if (!has_frame) {
                // 队列已空：若有一轮在等待排空（turn.done 已到且无新轮接管），
                // 此刻即"该轮全部下行音频已解码入播放队列"的确切时点，
                // 通知控制器标记 EOS。在此之前绝不 EOS——提前标记会被
                // "播完即结束"误判，长回答尾部被整批截断（断字断句）。
                if (active_turn_.load() == 0) {
                    FireOutputDrained();
                }
                break;
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
            // 插话/取消/新轮后不再被接受的轮，过期输出直接丢弃（不入播放队列）
            if (!IsTurnOutputAlive(frame.turn_id)) {
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

void DeviceVoiceClient::FireOutputDrained() {
    const uint32_t turn_id = finished_turn_.exchange(0);
    if (turn_id == 0) {
        return;  // 幂等：没有待排空的轮
    }
    ESP_LOGD(TAG, "output drained: turn=%u", (unsigned)turn_id);
    OnOutputDrainedFn cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = on_drained_;
    }
    if (cb) {
        cb(turn_id);  // 锁外回调；回调只做 MarkEndOfStream 级别的轻量标记
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

    // turn.done：本轮音频已全部下发，活动轮立即失效，但转入 finished_turn_
    // 等待解码排空——turn.done 时解码队列里往往还压着数秒未解码的尾部
    // 音频（后端合成远快于实时播放），必须播完，否则"断字断句"。
    // turn.cancelled（插话/异常）：立即失效并丢弃残余，不排空。
    // 仅当 turnId 匹配当前活动轮才生效：旧轮迟到的 done/cancelled 绝不能
    // 误伤新轮（旧实现无条件清零，会把新轮的全部下行音频静默丢掉）。
    if (mt == voice::MessageType::kTurnDone ||
        mt == voice::MessageType::kTurnCancelled) {
        const uint32_t done_id = msg.turn_id;
        // CAS 的 expected 参数是非 const 引用（失败时回写实际值），cur 不能声明为 const
        uint32_t cur = active_turn_.load();
        bool effective = false;
        if (done_id != 0) {
            // turnId 严格匹配才生效：旧轮迟到的 done/cancelled 绝不能误伤
            // 新轮（旧实现无条件清零，会把新轮全部下行音频静默丢掉）。
            effective = (done_id == cur);
        } else {
            // 后端未带 turnId（防御）：退回旧行为，仅当确有活动轮时生效。
            effective = (cur != 0);
        }
        if (effective &&
            active_turn_.compare_exchange_strong(cur, 0)) {
            // CAS 防止与 worker 任务的 StartTurn（插话开新轮）竞争：
            // 抢不到说明活动轮已被新轮接管，本消息按陈旧消息忽略。
            if (mt == voice::MessageType::kTurnDone) {
                finished_turn_.store(cur);
            } else {
                finished_turn_.store(0);
            }
            if (dec_task_ != nullptr) {
                // 唤醒解码循环尽快评估"排空完成"（dec_task_ 与本回调
                // 同在 WS 接收任务创建，此处读取无竞争）。
                xTaskNotifyGive(dec_task_);
            } else {
                // 本会话从未有过 opus 帧（纯 PCM 模式/无音频轮）：
                // 无解码尾部可言，直接宣布排空。
                FireOutputDrained();
            }
        }
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
