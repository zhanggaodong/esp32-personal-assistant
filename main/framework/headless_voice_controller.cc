#include "headless_voice_controller.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string.h>

#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>

#include "board.h"
#include "audio/audio_codec.h"
#include "config/config_store.h"
#include "event_bus.h"
#include "ai/ai_client.h"
#include "headless_led_controller.h"
#include "headless_network_controller.h"
#include "audio_prompt_player.h"
#include "device_log.h"
#include "voice/voice_diagnostics.h"

#define TAG "HeadlessVoice"

namespace {
constexpr uint64_t kPttClickHoldMs = 250;
constexpr uint64_t kPttClickWindowMs = 1200;

// 录音时长边界（legacy）：最短 200ms，最长 8s（达到上限自动按"松开"路径提交）
constexpr size_t kMinRecordMs = 200;
constexpr size_t kLegacyMaxRecordMs = 8000;

// 单次麦克风读取块（采样点，20ms @24kHz）
constexpr size_t kMicChunkSamples = 480;

// 流式路径一帧 = 100ms @16kHz = 1600 采样点（16bit/单声道 = 3200 字节）
constexpr size_t kFrameSamples16k = 1600;

// stream_v1 起播预缓冲：后端逐句串行合成时句间会插入 MiMo 的
// firstAudio 延迟（实测 42~682ms 波动），起播线过薄会让播放队列在
// 句间打空，表现为"断断续续"。600ms 与 legacy 路径一致。
constexpr size_t kStreamPrebufferMs = 600;

// 等待 turn.done 的看门狗上限（超过则强制结束本轮）
constexpr uint64_t kTurnDeadlineMs = 120000;

// 收到 turn.progress 心跳后的看门狗超时。后端下发音频期间每 5s 发一次心跳
// （VOICE_TURN_PROGRESS_INTERVAL_MS），这里容许连续丢失 3 次再判死。
//
// 这样超长回复不再受"按下 PTT 起 120s"的墙钟约束 —— 只要后端还在正常下发，
// 看门狗就一直续期；而后端一旦卡住，20s 内就能发现，比原来干等 120s 灵敏得多。
constexpr uint64_t kTurnProgressTimeoutMs = 20000;

// turn.error 展示给网页日志的 message 上限（UTF-8 字节），防止异常响应撑爆日志。
constexpr size_t kMaxTurnErrorMessageBytes = 160;

// 用户最终识别文本的展示上限：约前 100 个汉字（UTF-8 下 300 字节）。
constexpr size_t kMaxUserTextLogBytes = 300;

// 播放出队块：80ms @24kHz
constexpr size_t kPlaybackChunkSamples = 1920;

int64_t NowMs() {
    return esp_timer_get_time() / 1000;
}

// 线性重采样（only legacy）：src(采样率 src_rate) -> out(dst_rate)。
void Resample(const std::vector<int16_t>& src, int src_rate, int dst_rate,
              std::vector<int16_t>& out) {
    out.clear();
    if (src.empty() || dst_rate <= 0) {
        return;
    }
    if (src_rate == dst_rate) {
        out = src;
        return;
    }
    const double ratio = (double)src_rate / (double)dst_rate;
    size_t n = (size_t)(src.size() * (double)dst_rate / (double)src_rate);
    out.resize(n);
    for (size_t i = 0; i < n; ++i) {
        double pos = (double)i * ratio;
        size_t i0 = (size_t)pos;
        size_t i1 = i0 + 1;
        double frac = pos - (double)i0;
        int32_t v0 = (i0 < src.size()) ? src[i0] : src.back();
        int32_t v1 = (i1 < src.size()) ? src[i1] : v0;
        out[i] = (int16_t)(((int64_t)v0 * (1.0 - frac)) + ((int64_t)v1 * frac));
    }
}

bool AiConfigComplete() {
    return AiClient::Instance().IsConfigured();
}

}  // namespace

// 协议切换后延迟重启：worker 主循环的模式在 Start() 时确定，
// 运行中切换只会得到"半初始化"状态；延迟 1.5s 让网页响应先送达。
void ProtocolChangeRestartTask(void*) {
    DeviceLog::Log('I', "HeadlessVoice", "语音协议已切换，设备重启生效");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

HeadlessVoiceController& HeadlessVoiceController::Instance() {
    static HeadlessVoiceController instance;
    return instance;
}

bool HeadlessVoiceController::UseStream() const {
    return ConfigStore::Instance().Get("ai.voice_protocol") == "stream_v1";
}

void HeadlessVoiceController::Start() {
    auto& board = Board::GetInstance();
    auto* codec = board.GetAudioCodec();
    if (codec == nullptr) {
        ESP_LOGE(TAG, "no audio codec, headless voice disabled");
        return;
    }
    codec->Start();
    codec->EnableInput(false);
    codec->EnableOutput(false);

    AudioPromptPlayer::Instance().Init(codec, [this]() {
        // 正在录音或播报 TTS 时提示音让路，避免占用 ES8311 与喇叭
        return speaking_.load() || ptt_held_.load();
    });
    AiClient::Instance().UpdateConfig();
    DeviceVoiceClient::Instance().UpdateConfig();

    // 配置变更（ai.*）即时刷新登录参数与语言客户端会话；
    // 地址/账号/密码变化会让旧 token 失效，VoiceClient 的旧 WebSocket 一并作废。
    EventBus::Instance().Subscribe(kEventConfigChanged, [](int32_t, void* data) {
        if (data == nullptr || strstr(static_cast<const char*>(data), "ai.") != nullptr) {
            AiClient::Instance().UpdateConfig();
            DeviceVoiceClient::Instance().UpdateConfig();
            if (data != nullptr && strstr(static_cast<const char*>(data),
                                          "ai.voice_protocol") != nullptr) {
                // 事件回调线程不能直接 esp_restart（会掐断网页响应），
                // 交给独立任务延迟执行。
                xTaskCreate(ProtocolChangeRestartTask, "proto_restart", 4096,
                            nullptr, 3, nullptr);
            }
        }
    });

    const bool use_stream = UseStream();
    if (use_stream) {
        DeviceVoiceClient& vc = DeviceVoiceClient::Instance();
        vc.SetPcmCallback([this](uint32_t turn_id, uint32_t sequence, bool first,
                                 bool last, const int16_t* pcm, size_t count) {
            OnPcm(turn_id, sequence, first, last, pcm, count);
        });
        vc.SetEventCallback([this](const voice::ServerMessage& msg) { OnEvent(msg); });
        vc.SetDisconnectedCallback([this](const char* reason) { OnDisconnected(reason); });
        vc.SetOutputDrainedCallback(
            [this](uint32_t turn_id) { OnOutputDrained(turn_id); });

        event_queue_ = xQueueCreate(16, sizeof(Event));
        if (event_queue_ == nullptr) {
            ESP_LOGE(TAG, "failed to create event queue");
            DeviceLog::Log('E', "HeadlessVoice", "stream: 事件队列创建失败");
            return;
        }
        ESP_LOGI(TAG, "stream initialized, waiting for WiFi readiness");
        DeviceLog::Log('I', "HeadlessVoice", "stream: 已初始化，等待 WiFi 就绪");
    }

    // 播放线程两种模式共用：stream_v1 下行走 playback_ 队列，legacy TTS
    // 边收边播也走同一队列（参考 xiaozhi 的"深缓冲+阻塞写扬声器"结构）。
    if (xTaskCreate(PlaybackTask, "headless_playback", 4096, this, 4,
                    &playback_task_) != pdPASS) {
        ESP_LOGE(TAG, "failed to create playback task");
        playback_task_ = nullptr;
    }

    state_ = State::kWaitWifi;
    // 栈 26KB：opus 解码（libopus）需要大栈，小智同款配置为 2048*13=26624。
    // 之前用 8KB，Opus 上线后解码阶段必然栈溢出（表现为 TTS 一起播就重启）。
    // ASR/Chat/TTS HTTP 路径栈占用小（水位 >3.5KB），大栈只是保险。
    if (xTaskCreate(WorkerTask, "headless_voice", 26624, this, 5, &worker_) !=
        pdPASS) {
        ESP_LOGE(TAG, "failed to create headless voice worker");
        DeviceLog::Log('E', "HeadlessVoice", "语音工作线程创建失败，PTT 已禁用");
        return;
    }
    booted_ = true;
    ESP_LOGI(TAG, "headless voice started (PTT: press=record, release=submit, protocol=%s)",
             use_stream ? "stream_v1" : "legacy");
}

// ---------------------------------------------------------------------------
// 按键回调（iot_button 线程，非阻塞，只投递事件/标记）
// ---------------------------------------------------------------------------

void HeadlessVoiceController::OnPttPressed() {
    if (!booted_.load()) {
        return;  // 启动阶段忽略
    }
    // 新一轮尚未发生松手，不能沿用上一轮的短击结果。否则按住直到录音
    // 上限自动截止时，会被上一轮残留的 true 误判为 short_click。
    last_release_was_short_.store(false);
    ptt_press_started_ms_.store((uint64_t)(esp_timer_get_time() / 1000));
    if (UseStream()) {
        ptt_held_.store(true);
        Event e = Event::kPress;
        if (event_queue_ == nullptr) {
            ESP_LOGE(TAG, "stream PTT ignored: event queue is not initialized");
            DeviceLog::Log('E', "HeadlessVoice",
                           "stream: PTT 被忽略，事件队列未初始化（请确认切换后已重启）");
            return;
        }
        if (xQueueSend(event_queue_, &e, 0) != pdTRUE) {
            ESP_LOGW(TAG, "stream PTT ignored: event queue is full");
            DeviceLog::Log('W', "HeadlessVoice", "stream: PTT 被忽略，事件队列已满");
        } else {
            ESP_LOGI(TAG, "stream PTT press queued");
            DeviceLog::Log('I', "HeadlessVoice", "stream: PTT 按下事件已入队");
        }
        return;
    }
    // legacy：对话/播报中再次按电源键 = 立即停止，不启动新一轮录音。
    // 网络请求在下一块流数据处退出；播放队列立即清空，播放线程最多 40ms
    // 关闭扬声器输出。
    if (conversation_active_.load()) {
        legacy_cancel_requested_.store(true);
        AiClient::Instance().CancelCurrentRequest();
        if (auto* codec = Board::GetInstance().GetAudioCodec(); codec != nullptr) {
            codec->EnableOutput(false);
        }
        playback_.Clear();
        ESP_LOGI(TAG, "legacy conversation cancelled by power button");
        DeviceLog::Log('I', "HeadlessVoice", "legacy: 电源键停止当前回复");
        ptt_held_ = false;
        return;
    }
    ptt_held_ = true;
    if (worker_ != nullptr) {
        xTaskNotifyGive(worker_);
    }
}

void HeadlessVoiceController::OnPttReleased() {
    const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    const uint64_t started_ms = ptt_press_started_ms_.load();
    const bool short_press = started_ms != 0 && now_ms >= started_ms &&
                             now_ms - started_ms < kPttClickHoldMs;
    last_release_was_short_.store(short_press);
    if (short_press) {
        if (short_click_window_started_ms_ == 0 ||
            now_ms - short_click_window_started_ms_ > kPttClickWindowMs) {
            short_click_window_started_ms_ = now_ms;
            short_click_count_ = 0;
        }
        short_click_count_++;
        if (short_click_count_ >= 3) {
            shutdown_requested_.store(true);
            short_click_count_ = 0;
            short_click_window_started_ms_ = 0;
        }
    } else {
        short_click_count_ = 0;
        short_click_window_started_ms_ = 0;
    }
    ptt_held_.store(false);  // 录音循环据此停止采集；流式路径由状态机感知
    if (UseStream() && event_queue_ != nullptr) {
        Event e = Event::kRelease;
        if (xQueueSend(event_queue_, &e, 0) != pdTRUE) {
            ESP_LOGW(TAG, "stream PTT release event dropped: event queue is full");
            DeviceLog::Log('W', "HeadlessVoice", "stream: PTT 松开事件丢失，事件队列已满");
        } else {
            ESP_LOGI(TAG, "stream PTT release queued");
            DeviceLog::Log('I', "HeadlessVoice", "stream: PTT 松开事件已入队");
        }
    }
}

void HeadlessVoiceController::SetShutdownCallback(std::function<void()> callback) {
    shutdown_callback_ = std::move(callback);
}

void HeadlessVoiceController::OnNetworkConnectivityChanged(bool connected) {
    network_connected_.store(connected);
    if (!booted_.load() || !UseStream()) {
        return;
    }
    if (event_queue_ == nullptr) {
        ESP_LOGE(TAG, "stream network event dropped: event queue is not initialized");
        DeviceLog::Log('E', "HeadlessVoice", "stream: 网络状态事件丢失，事件队列未初始化");
        return;
    }
    Event e = connected ? Event::kNetworkConnected : Event::kNetworkUnavailable;
    if (xQueueSend(event_queue_, &e, 0) != pdTRUE) {
        ESP_LOGW(TAG, "stream network event dropped: event queue is full");
        DeviceLog::Log('W', "HeadlessVoice", "stream: 网络状态事件丢失，事件队列已满");
    }
}

// ---------------------------------------------------------------------------
// 工作线程：legacy 阻塞编排 / stream_v1 事件状态机
// ---------------------------------------------------------------------------

void HeadlessVoiceController::WorkerTask(void* arg) {
    static_cast<HeadlessVoiceController*>(arg)->WorkerLoop();
    vTaskDelete(nullptr);
}

void HeadlessVoiceController::WorkerLoop() {
    for (;;) {
        if (!UseStream()) {
            // legacy：等待"电源键按下"通知
            if (ulTaskNotifyTake(pdFALSE, portMAX_DELAY) == 0) {
                continue;
            }
            HandlePressLegacy();
            if (shutdown_requested_.exchange(false)) {
                RequestShutdown();
            }
            continue;
        }
        if (event_queue_ == nullptr) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        // stream：等事件；等待回复态使用短超时喂看门狗
        Event e;
        bool in_wait_state = (state_ == State::kWaitingAsr || state_ == State::kStreamingReply ||
                              state_ == State::kSpeaking || state_ == State::kCancelling);
        TickType_t wait = in_wait_state ? pdMS_TO_TICKS(200) : portMAX_DELAY;
        if (xQueueReceive(event_queue_, &e, wait) != pdTRUE) {
            if (in_wait_state && NowMs() >= (int64_t)turn_deadline_ms_) {
                ESP_LOGE(TAG, "turn timeout waiting turn.done, forcing end");
                // 不要写死秒数：等待中的超时可能是初始看门狗(120s)，
                // 也可能是收到过心跳后的心跳超时(20s)，写死会误导排查。
                DeviceLog::Log('E', "HeadlessVoice",
                               "stream: 等待 turn.done 超时，强制结束本轮");
                CancelActiveTurn();
                HandleTurnEnd(false);
            }
            continue;
        }
        HandleEvent(e);
        if (shutdown_requested_.exchange(false)) {
            RequestShutdown();
        }
    }
}

void HeadlessVoiceController::HandleEvent(Event e) {
    switch (state_) {
        case State::kReady:
            if (e == Event::kPress) {
                HandlePressStream();  // 内部同步完成一轮（录音→等回复→播完）
            } else if (e == Event::kNetworkUnavailable) {
                state_ = State::kWaitWifi;
                ESP_LOGW(TAG, "stream state ready -> wait_wifi");
                DeviceLog::Log('W', "HeadlessVoice", "stream: WiFi 不可用，暂停 PTT");
            } else if (e == Event::kDisconnected) {
                // 空闲被服务端关闭：只回收旧 socket，保持 Ready，
                // 下一次 PTT 在 EnsureConnected 中自动重连。
                DeviceVoiceClient::Instance().ReapDisconnectedSocket();
            }
            break;
        case State::kWaitingAsr:
        case State::kStreamingReply:
        case State::kSpeaking:
            if (e == Event::kPress) {
                // 回答/播报中单击电源键只停止当前轮，不自动进入下一轮录音。
                // 下一轮需要松手后再次按住，避免一次按键既停止又误触发录音。
                ESP_LOGI(TAG, "stream conversation cancelled by power button");
                DeviceLog::Log('I', "HeadlessVoice", "stream: 电源键停止当前回复");
                CancelActiveTurn();
                conversation_active_.store(false);
                current_turn_id_ = 0;
                ReturnToIdleState();
            } else if (e == Event::kTurnDone) {
                // conversationId 已在 OnEvent 中设置；EOS 等解码排空回调
                // （OnOutputDrained）标记，之后播放队列排空才 kPlaybackEnd
                turn_deadline_ms_ = NowMs() + kTurnDeadlineMs;
            } else if (e == Event::kTurnError) {
                HandleTurnEnd(false);
            } else if (e == Event::kDisconnected) {
                DeviceVoiceClient::Instance().ReapDisconnectedSocket();
                CancelActiveTurn();
                HandleTurnEnd(false);
            } else if (e == Event::kNetworkUnavailable) {
                ESP_LOGW(TAG, "stream network lost, cancelling active turn");
                DeviceLog::Log('W', "HeadlessVoice", "stream: WiFi 已断开，取消当前轮次");
                CancelActiveTurn();
                HandleTurnEnd(false);
            } else if (e == Event::kPlaybackEnd) {
                HandleTurnEnd(true);
            }
            break;
        case State::kWaitWifi:
            if (e == Event::kNetworkConnected) {
                state_ = State::kReady;
                ESP_LOGI(TAG, "stream state wait_wifi -> ready, PTT enabled");
                DeviceLog::Log('I', "HeadlessVoice", "stream: WiFi 已就绪，PTT 可用");
            } else if (e == Event::kPress) {
                ESP_LOGW(TAG, "stream PTT ignored: waiting for WiFi");
                DeviceLog::Log('W', "HeadlessVoice", "stream: PTT 被忽略，正在等待 WiFi");
            } else if (e == Event::kDisconnected) {
                // 只回收断线残留；网络不可用的提示音由 WiFi 断开路径负责，不重复播放。
                DeviceVoiceClient::Instance().ReapDisconnectedSocket();
            }
            break;
        case State::kRecording:
        case State::kBoot:
        case State::kCancelling:
        case State::kProcessing:
            break;  // 录音在 BeginRecording 内同步进行；其余事件忽略
    }
}

// ---------------------------------------------------------------------------
// 流式路径
// ---------------------------------------------------------------------------

void HeadlessVoiceController::HandlePressStream() {
    auto& prompt = AudioPromptPlayer::Instance();
    auto& net = HeadlessNetworkController::Instance();
    auto& led = HeadlessLedController::Instance();

    // 前置：联网且不在配网；配置完整
    if (!net.IsConnected() || net.IsProvisioning()) {
        ESP_LOGW(TAG, "stream PTT rejected: network is not ready");
        DeviceLog::Log('W', "HeadlessVoice", "stream: 网络未就绪，拒绝按键");
        led.ShowError();
        prompt.Play(AudioPromptPlayer::Prompt::kNetworkError);
        led.ShowReady();
        return;
    }
    if (!AiConfigComplete()) {
        ESP_LOGW(TAG, "stream PTT rejected: AI config incomplete");
        DeviceLog::Log('W', "HeadlessVoice", "stream: AI 配置不完整，拒绝按键");
        led.ShowError();
        prompt.Play(AudioPromptPlayer::Prompt::kNeedServiceConfig);
        led.ShowReady();
        return;
    }
    ESP_LOGI(TAG, "stream starting voice websocket connection");
    DeviceLog::Log('I', "HeadlessVoice", "stream: 开始连接 voice websocket");
    BeginRecording();  // 状态迁移在内部完成
}

void HeadlessVoiceController::BeginRecording() {
    auto& led = HeadlessLedController::Instance();
    auto& prompt = AudioPromptPlayer::Instance();
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        ESP_LOGE(TAG, "stream cannot record: audio codec is unavailable");
        DeviceLog::Log('E', "HeadlessVoice", "stream: 音频设备不可用，无法录音");
        led.ShowError();
        prompt.Play(AudioPromptPlayer::Prompt::kNetworkError);
        led.ShowReady();
        return;
    }

    // 插话：先停旧轮（cancel + 关输出 + 清播放队列），再开新麦
    if (state_ == State::kWaitingAsr || state_ == State::kStreamingReply ||
        state_ == State::kSpeaking) {
        CancelActiveTurn();
    }

    DeviceVoiceClient& vc = DeviceVoiceClient::Instance();
    if (!vc.EnsureConnected()) {
        ESP_LOGE(TAG, "voice websocket connect failed");
        DeviceLog::Log('E', "HeadlessVoice", "stream: voice websocket 连接失败(检查服务器 ws 升级配置)");
        led.ShowError();
        prompt.Play(AudioPromptPlayer::Prompt::kNetworkError);
        current_turn_id_ = 0;
        conversation_active_.store(false);
        ReturnToIdleState();
        return;
    }

    current_turn_id_ = next_turn_id_++;
    conversation_active_.store(true);
    led.ShowRecording();

    timeline_.Reset(0);
    DiagMark(voice_diag::Stage::kRecording, "ptt_down");

    std::string conv;
    {
        std::lock_guard<std::mutex> lk(conv_mutex_);
        conv = conversation_id_;
    }
    std::string voice = ConfigStore::Instance().Get("ai.voice");
    if (voice.empty()) {
        voice = "mimo_default";
    }
    int max_rec = ConfigStore::Instance().GetInt("ai.max_record_seconds", 30);
    max_rec = std::max(15, std::min(60, max_rec));  // 固件硬上限 60s

    if (!vc.StartTurn(current_turn_id_, conv, voice, "zh-CN", (uint32_t)max_rec)) {
        ESP_LOGE(TAG, "turn.start failed");
        DeviceLog::Log('E', "HeadlessVoice", "stream: turn.start 发送失败");
        led.ShowError();
        prompt.Play(AudioPromptPlayer::Prompt::kNetworkError);
        current_turn_id_ = 0;
        conversation_active_.store(false);
        ReturnToIdleState();
        return;
    }
    ESP_LOGI(TAG, "stream turn.start sent: turn=%u", (unsigned)current_turn_id_);
    DeviceLog::Log('I', "HeadlessVoice", "stream: turn.start 已发送，开始录音");

    // —— 录音与上行 ——
    const size_t mic_rate = (size_t)codec->input_sample_rate();
    if (!resampler_.Configure(mic_rate, 16000)) {
        ESP_LOGE(TAG, "stream resampler config failed: %u -> 16000",
                 (unsigned)mic_rate);
        DeviceLog::Log('E', "HeadlessVoice",
                       "stream: 重采样器配置失败(%u -> 16000)",
                       (unsigned)mic_rate);
        vc.CancelTurn(current_turn_id_, "resampler_config_error");
        led.ShowError();
        current_turn_id_ = 0;
        conversation_active_.store(false);
        ReturnToIdleState();
        return;
    }
    ESP_LOGI(TAG, "stream resampler ready: %u -> 16000",
             (unsigned)mic_rate);
    DeviceLog::Log('I', "HeadlessVoice", "stream: 重采样器已就绪(%u -> 16000)",
                   (unsigned)mic_rate);
    codec->EnableInput(true);
    state_ = State::kRecording;

    frame_buf_.clear();
    resample_buf_.clear();
    turn_seq_ = 0;
    // 复位下行队列：清掉上一轮残留的 end_of_stream 标记。否则新一轮首帧
    // 到达前的瞬时空队列会被误判为"上一轮已播完"，提前发出 kPlaybackEnd。
    playback_.Clear();
    playback_.set_prebuffer_samples(24000 * kStreamPrebufferMs / 1000);
    rec_start_ms_ = (uint64_t)NowMs();
    const size_t chunk_samples = mic_rate / 50;  // 20ms
    const uint64_t max_ms = (uint64_t)max_rec * 1000;
    std::vector<int16_t> chunk(chunk_samples, 0);
    std::vector<int16_t> out;
    const char* capture_error = nullptr;
    bool sent_any = false;

    while (ptt_held_.load() && network_connected_.load()) {
        if (((uint64_t)NowMs() - rec_start_ms_) >= max_ms) {
            break;  // 达到配置上限，自动走"松开"路径
        }
        chunk.assign(chunk_samples, 0);
        if (!codec->InputData(chunk)) {
            capture_error = "mic_read_error";
            break;
        }
        resampler_.Process(chunk.data(), chunk.size(), out);
        frame_buf_.insert(frame_buf_.end(), out.begin(), out.end());
        out.clear();
        // 每满一帧（1600 样本）立即上行
        while (frame_buf_.size() >= kFrameSamples16k) {
            if (!vc.SendInputPcm(current_turn_id_, turn_seq_++, !sent_any, false,
                                 frame_buf_.data(), kFrameSamples16k)) {
                capture_error = "uplink_send_error";
                goto capture_done;
            }
            sent_any = true;
            frame_buf_.erase(frame_buf_.begin(), frame_buf_.begin() + kFrameSamples16k);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
capture_done:
    codec->EnableInput(false);
    const uint64_t elapsed_ms = (uint64_t)NowMs() - rec_start_ms_;
    if (!network_connected_.load()) {
        capture_error = "network_lost";
    }

    if (capture_error == nullptr) {
        resampler_.Flush(out);
        frame_buf_.insert(frame_buf_.end(), out.begin(), out.end());
        out.clear();
    }

    if (capture_error == nullptr && !sent_any && frame_buf_.empty()) {
        capture_error = "no_pcm";
    }
    if (capture_error != nullptr) {
        // 输入异常 / 空数据 / 网络中断：丢弃本轮
        ESP_LOGE(TAG, "stream capture failed: reason=%s fed=%u produced=%u",
                 capture_error, (unsigned)resampler_.fed(),
                 (unsigned)resampler_.produced());
        DeviceLog::Log('E', "HeadlessVoice",
                       "stream: 录音上行失败(%s, 输入=%u, 输出=%u)",
                       capture_error, (unsigned)resampler_.fed(),
                       (unsigned)resampler_.produced());
        vc.CancelTurn(current_turn_id_, capture_error);
        led.ShowError();
        current_turn_id_ = 0;
        conversation_active_.store(false);
        ReturnToIdleState();
        timeline_.Log(TAG);
        return;
    }
    if (last_release_was_short_.load()) {
        vc.CancelTurn(current_turn_id_, "short_click");
        current_turn_id_ = 0;
        conversation_active_.store(false);
        ReturnToIdleState();
        return;
    }
    if (elapsed_ms < kMinRecordMs) {
        ESP_LOGW(TAG, "recording too short: %u ms", (unsigned)elapsed_ms);
        vc.CancelTurn(current_turn_id_, "too_short");
        if (!last_release_was_short_.load()) {
            prompt.Play(AudioPromptPlayer::Prompt::kNoSpeech);
        }
        led.ShowError();
        current_turn_id_ = 0;
        conversation_active_.store(false);
        ReturnToIdleState();
        return;
    }
    // 尾帧：不足一帧的剩余做最后一片（last=true）；恰好对齐则无尾帧，靠 turn.stop 收尾
    if (!frame_buf_.empty() &&
        !vc.SendInputPcm(current_turn_id_, turn_seq_++, !sent_any, true,
                         frame_buf_.data(), frame_buf_.size())) {
        ESP_LOGE(TAG, "stream tail PCM send failed");
        DeviceLog::Log('E', "HeadlessVoice", "stream: 录音尾帧发送失败");
        vc.CancelTurn(current_turn_id_, "uplink_tail_send_error");
        led.ShowError();
        current_turn_id_ = 0;
        conversation_active_.store(false);
        ReturnToIdleState();
        return;
    }
    if (!vc.StopTurn(current_turn_id_)) {
        ESP_LOGE(TAG, "stream turn.stop send failed");
        DeviceLog::Log('E', "HeadlessVoice", "stream: turn.stop 发送失败");
        vc.CancelTurn(current_turn_id_, "turn_stop_error");
        led.ShowError();
        current_turn_id_ = 0;
        conversation_active_.store(false);
        ReturnToIdleState();
        return;
    }
    ESP_LOGI(TAG, "stream turn.stop sent: fed=%u produced=%u",
             (unsigned)resampler_.fed(), (unsigned)resampler_.produced());
    DeviceLog::Log('I', "HeadlessVoice",
                   "stream: turn.stop 已发送(输入=%u, 输出=%u)",
                   (unsigned)resampler_.fed(),
                   (unsigned)resampler_.produced());
    DiagMark(voice_diag::Stage::kRecording, "ptt_up");

    // 等待后端校验/首字并并行播放
    first_chat_delta_logged_ = false;
    turn_deadline_ms_ = NowMs() + kTurnDeadlineMs;
    state_ = State::kWaitingAsr;
    led.ShowProcessing();
}

void HeadlessVoiceController::CancelActiveTurn() {
    auto& vc = DeviceVoiceClient::Instance();
    if (current_turn_id_ != 0) {
        vc.CancelTurn(current_turn_id_, "barge_in");
    }
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec != nullptr) {
        codec->EnableOutput(false);  // 先关输出，再开输入，避免把 AI 尾音录进新问题
    }
    playback_.Clear();
    speaking_.store(false);
}

void HeadlessVoiceController::RequestShutdown() {
    ESP_LOGI(TAG, "PTT triple short-click shutdown requested");
    CancelActiveTurn();
    HeadlessLedController::Instance().ShowPowerOff();
    vTaskDelay(pdMS_TO_TICKS(950));
    if (shutdown_callback_) {
        shutdown_callback_();
    } else {
        ESP_LOGE(TAG, "shutdown callback is not configured");
    }
}

void HeadlessVoiceController::HandleTurnEnd(bool success) {
    auto& led = HeadlessLedController::Instance();
    auto& prompt = AudioPromptPlayer::Instance();
    if (!success) {
        if (current_turn_id_ != 0) {
            DeviceVoiceClient::Instance().CancelTurn(current_turn_id_, "error");
        }
        CancelActiveTurn();
        led.ShowError();
        prompt.Play(AudioPromptPlayer::Prompt::kNetworkError);
    }
    conversation_active_.store(false);
    current_turn_id_ = 0;
    ReturnToIdleState();
}

void HeadlessVoiceController::ReturnToIdleState() {
    auto& led = HeadlessLedController::Instance();
    if (network_connected_.load()) {
        state_ = State::kReady;
        led.ShowReady();
    } else {
        state_ = State::kWaitWifi;
        led.ShowWifiConnecting();
    }
}

void HeadlessVoiceController::OnPcm(uint32_t turn_id, uint32_t /*sequence*/,
                                    bool /*first*/, bool /*last*/,
                                    const int16_t* pcm, size_t count) {
    // 队列满时阻塞等待而非丢弃：与解码队列的阻塞入队共同构成完整背压链
    // （播放队列满 → 解码停 → 解码队列满 → WS 接收停读 → TCP 流控 →
    // 服务器节流），长回答音频零丢失。
    // 每次入队前先复查轮次：活动轮或待排空轮才接受；等待期间被插话/
    // 取消的旧轮，剩余帧直接丢弃。
    for (;;) {
        if (!DeviceVoiceClient::Instance().IsTurnOutputAlive(turn_id)) {
            return;
        }
        if (playback_.Push(pcm, count) !=
            voice::PcmPlaybackQueue::PushResult::kFull) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (playback_task_ != nullptr) {
        xTaskNotifyGive(playback_task_);
    }
}

// 解码任务确认某轮下行音频已全部解码入播放队列（排空语义终点）。
// 仅当该轮仍是当前轮时才标记 EOS：看门狗强制结束/插话后的陈旧回调
// 不得污染新一轮的播放队列（新一轮在 BeginRecording 里 Clear 复位）。
void HeadlessVoiceController::OnOutputDrained(uint32_t turn_id) {
    if (turn_id != 0 && turn_id == current_turn_id_) {
        playback_.MarkEndOfStream();
    }
}

void HeadlessVoiceController::OnEvent(const voice::ServerMessage& msg) {
    switch (msg.type) {
        case voice::MessageType::kAsrPartial:
            // 录音期间滚动预热文本：仅日志，不驱动任何播放
            ESP_LOGI(TAG, "asr partial: %.*s", (int)std::min<size_t>(msg.text.size(), 200),
                     msg.text.c_str());
            break;
        case voice::MessageType::kAsrFinal: {
            // 最终识别文本：网页日志可见（约前 100 字，防止超长撑爆日志）。
            const int shown = static_cast<int>(
                std::min<size_t>(msg.text.size(), kMaxUserTextLogBytes));
            ESP_LOGI(TAG, "asr final: %.*s", shown, msg.text.c_str());
            DeviceLog::Log('I', "HeadlessVoice", "stream: ASR 完成: %.*s",
                           shown, msg.text.c_str());
            break;
        }
        case voice::MessageType::kChatDelta: {
            // 仅日志，后端才累积完整答案
            if (!first_chat_delta_logged_) {
                first_chat_delta_logged_ = true;
                ESP_LOGI(TAG, "first chat delta");
            }
            ESP_LOGD(TAG, "chat delta: %.*s", (int)std::min<size_t>(msg.text.size(), 200),
                     msg.text.c_str());
            break;
        }
        case voice::MessageType::kTurnProgress: {
            // 后端还在正常下发音频：把看门狗续期到"心跳超时"。
            // 严格匹配 turnId —— 旧轮迟到的心跳绝不能给新轮续期，
            // 否则新轮真卡死时会被旧心跳一直吊着，永远检测不到。
            if (msg.has_turn_id && msg.turn_id == current_turn_id_) {
                turn_deadline_ms_ = NowMs() + kTurnProgressTimeoutMs;
            }
            break;
        }
        case voice::MessageType::kTurnDone:
            {
                std::lock_guard<std::mutex> lk(conv_mutex_);
                conversation_id_ = msg.conversation_id;  // 下一轮复用
            }
            // EOS 不在这里标记：此刻解码队列里往往还压着数秒未解码的尾部
            // 音频（后端合成远快于实时播放），提前 EOS 会让播放队列
            // "播完即结束"，长回答尾部被整批截断。由解码任务排空后经
            // OnOutputDrained 标记（DeviceVoiceClient 排空语义）。
            if (event_queue_ != nullptr) {
                Event e = Event::kTurnDone;
                xQueueSend(event_queue_, &e, 0);
            }
            break;
        case voice::MessageType::kTurnError: {
            // 服务端错误详情必须可见：phase/code/受限 message，便于网页日志定位。
            const int shown =
                static_cast<int>(std::min<size_t>(msg.message.size(),
                                                  kMaxTurnErrorMessageBytes));
            ESP_LOGE(TAG, "turn error: phase=%s code=%s message=%.*s",
                     msg.phase.c_str(), msg.code.c_str(), shown,
                     msg.message.c_str());
            DeviceLog::Log('E', "HeadlessVoice",
                           "stream: 服务端失败(%s/%s): %.*s",
                           msg.phase.c_str(), msg.code.c_str(), shown,
                           msg.message.c_str());
            if (event_queue_ != nullptr) {
                Event e = Event::kTurnError;
                xQueueSend(event_queue_, &e, 0);
            }
            break;
        }
        case voice::MessageType::kTurnCancelled:
        case voice::MessageType::kTurnReady:
        case voice::MessageType::kTtsSentence:
        case voice::MessageType::kHello:
        default:
            break;
    }
}

void HeadlessVoiceController::OnDisconnected(const char* reason) {
    ESP_LOGW(TAG, "voice disconnected: %s", reason);
    if (event_queue_ != nullptr) {
        Event e = Event::kDisconnected;
        xQueueSend(event_queue_, &e, 0);
    }
}

// ---------------------------------------------------------------------------
// 播放线程（stream_v1 边收边播）
// ---------------------------------------------------------------------------

void HeadlessVoiceController::PlaybackTask(void* arg) {
    static_cast<HeadlessVoiceController*>(arg)->PlaybackLoop();
    vTaskDelete(nullptr);
}

void HeadlessVoiceController::PlaybackLoop() {
    auto* codec = Board::GetInstance().GetAudioCodec();
    std::vector<int16_t> chunk;
    bool output_on = false;
    bool end_posted = false;
    int64_t last_starve_log_ms = 0;
    uint32_t last_generation = playback_.generation();
    // 本轮播放对账统计（generation 变化即新一轮归零）：播放时长与后端
    // ttsFramesSent×60ms 比对，可定位"文字完整却只读一半"丢在哪一段。
    size_t played_samples = 0;
    uint32_t starve_count = 0;
    for (;;) {
        xTaskNotifyWait(0, 0xFFFFFFFFUL, nullptr, pdMS_TO_TICKS(40));  // 轮询兜底

        // 队列被清空（插话取消/新一轮 BeginRecording 的 Clear）时必须复位
        // 本任务的输出状态：CancelActiveTurn 会直接关掉codec输出，若本任务
        // 的 output_on 仍为 true，下一轮回复会"以为输出还开着"而整段静音。
        const uint32_t generation = playback_.generation();
        if (generation != last_generation) {
            last_generation = generation;
            played_samples = 0;
            starve_count = 0;
            if (output_on && codec != nullptr) {
                codec->EnableOutput(false);
                output_on = false;
            }
            speaking_.store(false);
            end_posted = false;
        }

        // PopChunk 是追加语义（头文件约定 out 调用前 clear）：
        // 不清空会导致 chunk 无限增长、OutputData 反复重播旧数据并耗尽内存。
        chunk.clear();
        size_t n = playback_.PopChunk(chunk, kPlaybackChunkSamples);
        if (n > 0) {
            played_samples += n;
            if (!output_on && codec != nullptr) {
                codec->EnableOutput(true);
                output_on = true;
                speaking_.store(true);
            }
            if (codec != nullptr) {
                codec->OutputData(chunk);
            }
            end_posted = false;
            vTaskDelay(pdMS_TO_TICKS(5));  // 轻微节流，防 DMA 写满
        } else if (playback_.EndReached()) {
            if (codec != nullptr && output_on) {
                codec->EnableOutput(false);
                output_on = false;
                speaking_.store(false);
            }
            if (!end_posted) {
                end_posted = true;
                DeviceLog::Log('I', "HeadlessVoice",
                               "stream: 本轮播完 samples=%u(约%ums) 断档=%u次",
                               (unsigned)played_samples,
                               (unsigned)(played_samples / 24),
                               (unsigned)starve_count);
                if (event_queue_ != nullptr) {
                    Event e = Event::kPlaybackEnd;
                    xQueueSend(event_queue_, &e, 0);
                }
            }
        } else if (output_on) {
            // 正在播放却取不到数据 = 上游供给断档（下溢）。限频 1s 记录，
            // 网页日志可见——"断断续续"类反馈可直接据此定位。
            const int64_t now = NowMs();
            if (now - last_starve_log_ms >= 1000) {
                last_starve_log_ms = now;
                starve_count += 1;
                ESP_LOGW(TAG, "playback underrun: buffered=%u samples",
                         (unsigned)playback_.BufferedSamples());
                DeviceLog::Log('W', "HeadlessVoice", "播放缓冲断档(等待音频)");
            }
        }
    }
}

// ---------------------------------------------------------------------------
// legacy 路径（Task 9 回滚路径，保持不变）
// ---------------------------------------------------------------------------

void HeadlessVoiceController::HandlePressLegacy() {
    auto& led = HeadlessLedController::Instance();
    auto& prompt = AudioPromptPlayer::Instance();
    auto& net = HeadlessNetworkController::Instance();

    if (state_ == State::kProcessing || state_ == State::kRecording) {
        prompt.Play(AudioPromptPlayer::Prompt::kWaitAnswer);
        led.ShowProcessing();
        return;
    }
    if (!net.IsConnected() || net.IsProvisioning()) {
        led.ShowError();
        prompt.Play(AudioPromptPlayer::Prompt::kNetworkError);
        led.ShowReady();
        return;
    }
    if (!AiConfigComplete()) {
        led.ShowError();
        prompt.Play(AudioPromptPlayer::Prompt::kNeedServiceConfig);
        led.ShowReady();
        return;
    }

    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        led.ShowError();
        prompt.Play(AudioPromptPlayer::Prompt::kNetworkError);
        led.ShowReady();
        return;
    }

    state_ = State::kRecording;
    conversation_active_.store(true);
    legacy_cancel_requested_.store(false);
    AiClient::Instance().ResetCancellation();
    led.ShowRecording();

    current_turn_id_ = next_turn_id_++;
    timeline_.Reset(0);
    DiagMark(voice_diag::Stage::kRecording, "ptt_down");

    std::vector<int16_t> mic24k;
    const size_t max_samples = kLegacyMaxRecordMs * (size_t)codec->input_sample_rate() / 1000;
    codec->EnableInput(true);
    bool mic_error = false;
    while (ptt_held_.load() && mic24k.size() < max_samples) {
        std::vector<int16_t> chunk(kMicChunkSamples);
        if (!codec->InputData(chunk)) {
            mic_error = true;
            break;
        }
        mic24k.insert(mic24k.end(), chunk.begin(), chunk.end());
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    codec->EnableInput(false);
    ptt_held_ = false;

    if (mic_error || mic24k.empty()) {
        ESP_LOGE(TAG, "mic error or no data");
        led.ShowError();
        conversation_active_.store(false);
        state_ = State::kReady;
        led.ShowReady();
        return;
    }
    if (last_release_was_short_.load()) {
        conversation_active_.store(false);
        state_ = State::kReady;
        led.ShowReady();
        return;
    }
    size_t record_ms = mic24k.size() * 1000 / (size_t)codec->input_sample_rate();
    if (record_ms < kMinRecordMs) {
        ESP_LOGW(TAG, "recording too short: %u ms", (unsigned)record_ms);
        led.ShowError();
        prompt.Play(AudioPromptPlayer::Prompt::kNoSpeech);
        conversation_active_.store(false);
        state_ = State::kReady;
        led.ShowReady();
        return;
    }
    ESP_LOGI(TAG, "recording done: %u ms, %u samples",
             (unsigned)record_ms, (unsigned)mic24k.size());
    DiagMark(voice_diag::Stage::kRecording, "ptt_up");

    state_ = State::kProcessing;
    led.ShowProcessing();
    ProcessConversation(mic24k);

    timeline_.Log(TAG);
    conversation_active_.store(false);
    state_ = State::kReady;
    led.ShowReady();
}

void HeadlessVoiceController::ProcessConversation(const std::vector<int16_t>& mic24k) {
    auto& ai = AiClient::Instance();
    auto* codec = Board::GetInstance().GetAudioCodec();

    int target = ConfigStore::Instance().GetInt("ai.sample_rate", 16000);
    std::vector<int16_t> pcm16k;
    Resample(mic24k, codec != nullptr ? codec->input_sample_rate() : 24000,
             target, pcm16k);
    std::vector<uint8_t> wav;
    if (pcm16k.empty() || !BuildWav(pcm16k, target, wav)) {
        HandleAiFailure();
        return;
    }

    DiagMark(voice_diag::Stage::kAsr, "asr_request_start");
    std::string question;
    if (!ai.Transcribe(wav, question) || question.empty()) {
        if (legacy_cancel_requested_.load()) {
            return;
        }
        ESP_LOGE(TAG, "asr failed or empty");
        HandleAiFailure();
        return;
    }
    ESP_LOGI(TAG, "asr ok: %s", question.c_str());
    DiagMark(voice_diag::Stage::kAsr, "asr_final");

    DiagMark(voice_diag::Stage::kChat, "chat_request_start");
    std::string reply;
    std::string conv_id;
    bool first_chat_delta_logged = false;
    bool chat_ok = ai.Chat(question, [&](const char* delta) {
        if (legacy_cancel_requested_.load()) {
            return;
        }
        if (!first_chat_delta_logged) {
            first_chat_delta_logged = true;
            DiagMark(voice_diag::Stage::kChat, "chat_first_delta");
        }
        reply += delta;
    }, conv_id);
    if (legacy_cancel_requested_.load()) {
        return;
    }
    if (!chat_ok || reply.empty()) {
        ESP_LOGE(TAG, "chat failed or empty reply");
        HandleAiFailure();
        return;
    }
    ESP_LOGI(TAG, "chat ok, reply len=%u", (unsigned)reply.size());
    DiagMark(voice_diag::Stage::kChat, "chat_done");

    DiagMark(voice_diag::Stage::kTts, "tts_request_start");
    // 边收边播（参考 xiaozhi 的"深缓冲 + 阻塞写扬声器"结构）：与 stream_v1
    // 共用固定容量播放队列与播放线程。预缓冲 400ms 吸收网络抖动；队列满时
    // 生产者等待（背压），不丢块；EOS 后播放线程排空到空。喂入速率低于实时
    // 时表现为首声稍晚，而不是直写模式那种饥饿断流/缺词。
    playback_.Clear();
    playback_.set_prebuffer_samples(24000 * 1500 / 1000);  // 1.5s 预缓冲，吸收分段 TTS/网络短暂停顿
    bool first_tts_pcm = false;
    bool tts_ok = ai.Synthesize(reply, [&](const int16_t* pcm, size_t n) {
        if (legacy_cancel_requested_.load()) {
            return;
        }
        if (!first_tts_pcm) {
            first_tts_pcm = true;
            DiagMark(voice_diag::Stage::kTts, "tts_first_pcm");
        }
        while (!legacy_cancel_requested_.load() &&
               playback_.Push(pcm, n) ==
                   voice::PcmPlaybackQueue::PushResult::kFull) {
            // 队列满：等播放线程消化（背压），绝不丢音频块
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (legacy_cancel_requested_.load()) {
            // 取消可能发生在队列满等待期间；再次清空，避免竞争窗口里刚入队
            // 的最后一块音频让扬声器重新启动。
            playback_.Clear();
        }
    });
    if (legacy_cancel_requested_.load()) {
        playback_.Clear();
        return;
    }
    if (!tts_ok || !first_tts_pcm) {
        // 中途失败：用 EOS 让播放线程排空残留并自行关输出/清 speaking_
        // （直接 Clear 会留下"输出已开但永远等不到数据"的悬挂状态），
        // 排空后再播错误提示，避免提示音被 speaking_ 门挡掉。
        playback_.MarkEndOfStream();
        const int64_t fail_deadline = NowMs() + 10000;
        while (!playback_.EndReached() && NowMs() < fail_deadline) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        playback_.Clear();
        ESP_LOGE(TAG, "tts failed or empty pcm");
        HandleAiFailure();
        return;
    }
    playback_.MarkEndOfStream();
    // 等播放线程把缓冲排空（含预缓冲与尾音），超时兜底防卡死
    const int64_t drain_deadline = NowMs() + 90000;
    while (!legacy_cancel_requested_.load() &&
           !playback_.EndReached() && NowMs() < drain_deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (legacy_cancel_requested_.load()) {
        playback_.Clear();
        return;
    }
    if (!playback_.EndReached()) {
        ESP_LOGW(TAG, "playback drain timeout, force stop");
        auto* c = Board::GetInstance().GetAudioCodec();
        if (c != nullptr) {
            c->EnableOutput(false);
        }
        speaking_.store(false);
    }
    playback_.Clear();
    DiagMark(voice_diag::Stage::kTts, "tts_done");
    DiagMark(voice_diag::Stage::kPlayback, "speaker_done");
}

void HeadlessVoiceController::HandleAiFailure() {
    auto& prompt = AudioPromptPlayer::Instance();
    auto& led = HeadlessLedController::Instance();
    DiagMark(voice_diag::Stage::kCancel, "turn_error");
    timeline_.Log(TAG);
    if (!AiClient::Instance().TokenAvailable()) {
        led.ShowError();
        prompt.Play(AudioPromptPlayer::Prompt::kLoginFail);
    } else {
        led.ShowError();
        prompt.Play(AudioPromptPlayer::Prompt::kNetworkError);
    }
    state_ = State::kReady;
    led.ShowReady();
}

void HeadlessVoiceController::DiagMark(voice_diag::Stage stage, const char* point) {
    timeline_.Mark(point);
    voice_diag::ResourceSnapshot res = voice_diag::CaptureResources(worker_);
    voice_diag::SaveRtcSnapshot(stage, current_turn_id_, res);
    voice_diag::LogResources(TAG, point, res);
}

bool HeadlessVoiceController::BuildWav(const std::vector<int16_t>& pcm, int rate,
                                       std::vector<uint8_t>& wav) {
    if (pcm.empty() || rate <= 0) {
        return false;
    }
    const uint32_t data_bytes = (uint32_t)pcm.size() * 2;
    wav.clear();
    wav.reserve(44 + data_bytes);

    auto put32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) { wav.push_back((uint8_t)((v >> (8 * i)) & 0xFF)); }
    };
    auto put16 = [&](uint16_t v) {
        wav.push_back((uint8_t)(v & 0xFF));
        wav.push_back((uint8_t)((v >> 8) & 0xFF));
    };
    auto putc = [&](const char* s, int len) {
        for (int i = 0; i < len; ++i) { wav.push_back((uint8_t)s[i]); }
    };

    putc("RIFF", 4);
    put32(36 + data_bytes);
    putc("WAVE", 4);
    putc("fmt ", 4);
    put32(16);
    put16(1);                // PCM
    put16(1);                // mono
    put32((uint32_t)rate);
    put32((uint32_t)rate * 2);
    put16(2);                // block align
    put16(16);               // bits
    putc("data", 4);
    put32(data_bytes);
    for (int16_t s : pcm) {
        wav.push_back((uint8_t)(s & 0xFF));
        wav.push_back((uint8_t)((uint16_t)s >> 8));
    }
    return true;
}
