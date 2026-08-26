#include "headless_voice_controller.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string.h>

#include <esp_log.h>
#include <esp_timer.h>

#include "board.h"
#include "audio/audio_codec.h"
#include "config/config_store.h"
#include "event_bus.h"
#include "ai/ai_client.h"
#include "headless_led_controller.h"
#include "headless_network_controller.h"
#include "audio_prompt_player.h"
#include "voice/voice_diagnostics.h"

#define TAG "HeadlessVoice"

namespace {

// 录音时长边界（legacy）：最短 200ms，最长 8s（达到上限自动按"松开"路径提交）
constexpr size_t kMinRecordMs = 200;
constexpr size_t kLegacyMaxRecordMs = 8000;

// 单次麦克风读取块（采样点，20ms @24kHz）
constexpr size_t kMicChunkSamples = 480;

// 流式路径一帧 = 100ms @16kHz = 1600 采样点（16bit/单声道 = 3200 字节）
constexpr size_t kFrameSamples16k = 1600;

// 等待 turn.done 的看门狗上限（超过则强制结束本轮）
constexpr uint64_t kTurnDeadlineMs = 120000;

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
        }
    });

    if (UseStream()) {
        DeviceVoiceClient& vc = DeviceVoiceClient::Instance();
        vc.SetPcmCallback([this](uint32_t turn_id, uint32_t sequence, bool first,
                                 bool last, const int16_t* pcm, size_t count) {
            OnPcm(turn_id, sequence, first, last, pcm, count);
        });
        vc.SetEventCallback([this](const voice::ServerMessage& msg) { OnEvent(msg); });
        vc.SetDisconnectedCallback([this](const char* reason) { OnDisconnected(reason); });

        event_queue_ = xQueueCreate(16, sizeof(Event));
        if (event_queue_ == nullptr) {
            ESP_LOGE(TAG, "failed to create event queue");
            return;
        }
    }

    // 播放线程两种模式共用：stream_v1 下行走 playback_ 队列，legacy TTS
    // 边收边播也走同一队列（参考 xiaozhi 的"深缓冲+阻塞写扬声器"结构）。
    if (xTaskCreate(PlaybackTask, "headless_playback", 4096, this, 4,
                    &playback_task_) != pdPASS) {
        ESP_LOGE(TAG, "failed to create playback task");
        playback_task_ = nullptr;
    }

    state_ = State::kWaitWifi;
    booted_ = true;
    if (xTaskCreate(WorkerTask, "headless_voice", 8192, this, 5, &worker_) != pdPASS) {
        ESP_LOGE(TAG, "failed to create headless voice worker");
        return;
    }
    ESP_LOGI(TAG, "headless voice started (PTT: press=record, release=submit)");
}

// ---------------------------------------------------------------------------
// 按键回调（iot_button 线程，非阻塞，只投递事件/标记）
// ---------------------------------------------------------------------------

void HeadlessVoiceController::OnPttPressed() {
    if (!booted_.load()) {
        return;  // 启动阶段忽略
    }
    if (UseStream()) {
        ptt_held_.store(true);
        Event e = Event::kPress;
        if (event_queue_ != nullptr) {
            xQueueSend(event_queue_, &e, 0);
        }
        return;
    }
    // legacy：一轮回答（录音/ASR/Chat/TTS/播报）尚未结束：忽略并短提示
    if (conversation_active_.load()) {
        AudioPromptPlayer::Instance().Play(AudioPromptPlayer::Prompt::kWaitAnswer);
        ptt_held_ = false;
        return;
    }
    ptt_held_ = true;
    if (worker_ != nullptr) {
        xTaskNotifyGive(worker_);
    }
}

void HeadlessVoiceController::OnPttReleased() {
    ptt_held_.store(false);  // 录音循环据此停止采集；流式路径由状态机感知
    if (UseStream() && event_queue_ != nullptr) {
        Event e = Event::kRelease;
        xQueueSend(event_queue_, &e, 0);
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
                CancelActiveTurn();
                HandleTurnEnd(false);
            }
            continue;
        }
        HandleEvent(e);
    }
}

void HeadlessVoiceController::HandleEvent(Event e) {
    switch (state_) {
        case State::kReady:
            if (e == Event::kPress) {
                HandlePressStream();  // 内部同步完成一轮（录音→等回复→播完）
            }
            break;
        case State::kWaitingAsr:
        case State::kStreamingReply:
        case State::kSpeaking:
            if (e == Event::kPress) {
                // 插话：取消旧轮并立即开始新一轮（由 BeginRecording 内部处理）
                HandlePressStream();
            } else if (e == Event::kTurnDone) {
                // conversationId 与 EOS 已在 OnEvent 中设置/标记，等待播放排空
                turn_deadline_ms_ = NowMs() + kTurnDeadlineMs;
            } else if (e == Event::kTurnError) {
                HandleTurnEnd(false);
            } else if (e == Event::kDisconnected) {
                CancelActiveTurn();
                HandleTurnEnd(false);
            } else if (e == Event::kPlaybackEnd) {
                HandleTurnEnd(true);
            }
            break;
        case State::kRecording:
        case State::kBoot:
        case State::kWaitWifi:
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
    BeginRecording();  // 状态迁移在内部完成
}

void HeadlessVoiceController::BeginRecording() {
    auto& led = HeadlessLedController::Instance();
    auto& prompt = AudioPromptPlayer::Instance();
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
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
        led.ShowError();
        prompt.Play(AudioPromptPlayer::Prompt::kNetworkError);
        state_ = State::kReady;
        conversation_active_.store(false);
        led.ShowReady();
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
        led.ShowError();
        prompt.Play(AudioPromptPlayer::Prompt::kNetworkError);
        state_ = State::kReady;
        conversation_active_.store(false);
        led.ShowReady();
        return;
    }

    // —— 录音与上行 ——
    codec->EnableInput(true);
    state_ = State::kRecording;

    resampler_.Configure((size_t)codec->input_sample_rate(), 16000);
    resampler_.Reset();
    frame_buf_.clear();
    resample_buf_.clear();
    turn_seq_ = 0;
    // 复位下行队列：清掉上一轮残留的 end_of_stream 标记。否则新一轮首帧
    // 到达前的瞬时空队列会被误判为"上一轮已播完"，提前发出 kPlaybackEnd。
    playback_.Clear();
    playback_.set_prebuffer_samples(voice::PcmPlaybackQueue::kPrebufferSamples);
    rec_start_ms_ = (uint64_t)NowMs();
    const size_t mic_rate = (size_t)codec->input_sample_rate();
    const size_t chunk_samples = mic_rate / 50;  // 20ms
    const uint64_t max_ms = (uint64_t)max_rec * 1000;
    std::vector<int16_t> chunk(chunk_samples, 0);
    std::vector<int16_t> out;
    bool mic_err = false;
    bool sent_any = false;

    while (ptt_held_.load()) {
        if (((uint64_t)NowMs() - rec_start_ms_) >= max_ms) {
            break;  // 达到配置上限，自动走"松开"路径
        }
        chunk.assign(chunk_samples, 0);
        if (!codec->InputData(chunk)) {
            mic_err = true;
            break;
        }
        resampler_.Process(chunk.data(), chunk.size(), out);
        frame_buf_.insert(frame_buf_.end(), out.begin(), out.end());
        out.clear();
        // 每满一帧（1600 样本）立即上行
        while (frame_buf_.size() >= kFrameSamples16k) {
            if (!vc.SendInputPcm(current_turn_id_, turn_seq_++, !sent_any, false,
                                 frame_buf_.data(), kFrameSamples16k)) {
                mic_err = true;  // WebSocket 断开：SendInputPcm 失败
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

    if (mic_err || (!sent_any && frame_buf_.empty())) {
        // 输入异常 / 空数据 / 网络中断：丢弃本轮
        ESP_LOGE(TAG, "mic error or uplink failure");
        vc.CancelTurn(current_turn_id_, "capture_error");
        led.ShowError();
        state_ = State::kReady;
        conversation_active_.store(false);
        led.ShowReady();
        timeline_.Log(TAG);
        return;
    }
    if (elapsed_ms < kMinRecordMs) {
        ESP_LOGW(TAG, "recording too short: %u ms", (unsigned)elapsed_ms);
        vc.CancelTurn(current_turn_id_, "too_short");
        prompt.Play(AudioPromptPlayer::Prompt::kNoSpeech);
        led.ShowError();
        state_ = State::kReady;
        conversation_active_.store(false);
        led.ShowReady();
        return;
    }
    // 尾帧：不足一帧的剩余做最后一片（last=true）；恰好对齐则无尾帧，靠 turn.stop 收尾
    if (!frame_buf_.empty()) {
        vc.SendInputPcm(current_turn_id_, turn_seq_++, !sent_any, true,
                        frame_buf_.data(), frame_buf_.size());
    }
    vc.StopTurn(current_turn_id_);
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
    state_ = State::kReady;
    led.ShowReady();
}

void HeadlessVoiceController::OnPcm(uint32_t /*turn_id*/, uint32_t /*sequence*/,
                                    bool /*first*/, bool /*last*/,
                                    const int16_t* pcm, size_t count) {
    // turn_id 过滤已由 DeviceVoiceClient 依据 active_turn 完成；此处只入队
    playback_.Push(pcm, count);  // 固定容量，满了丢弃（背压）
    if (playback_task_ != nullptr) {
        xTaskNotifyGive(playback_task_);
    }
}

void HeadlessVoiceController::OnEvent(const voice::ServerMessage& msg) {
    switch (msg.type) {
        case voice::MessageType::kAsrPartial:
            // 录音期间滚动预热文本：仅日志，不驱动任何播放
            ESP_LOGI(TAG, "asr partial: %.*s", (int)std::min<size_t>(msg.text.size(), 200),
                     msg.text.c_str());
            break;
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
        case voice::MessageType::kTurnDone:
            {
                std::lock_guard<std::mutex> lk(conv_mutex_);
                conversation_id_ = msg.conversation_id;  // 下一轮复用
            }
            playback_.MarkEndOfStream();  // 不再来新音频；播完排空即结束
            if (event_queue_ != nullptr) {
                Event e = Event::kTurnDone;
                xQueueSend(event_queue_, &e, 0);
            }
            break;
        case voice::MessageType::kTurnError:
            if (event_queue_ != nullptr) {
                Event e = Event::kTurnError;
                xQueueSend(event_queue_, &e, 0);
            }
            break;
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
    for (;;) {
        xTaskNotifyWait(0, 0xFFFFFFFFUL, nullptr, pdMS_TO_TICKS(40));  // 轮询兜底

        // PopChunk 是追加语义（头文件约定 out 调用前 clear）：
        // 不清空会导致 chunk 无限增长、OutputData 反复重播旧数据并耗尽内存。
        chunk.clear();
        size_t n = playback_.PopChunk(chunk, kPlaybackChunkSamples);
        if (n > 0) {
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
                if (event_queue_ != nullptr) {
                    Event e = Event::kPlaybackEnd;
                    xQueueSend(event_queue_, &e, 0);
                }
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
        if (!first_chat_delta_logged) {
            first_chat_delta_logged = true;
            DiagMark(voice_diag::Stage::kChat, "chat_first_delta");
        }
        reply += delta;
    }, conv_id);
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
    playback_.set_prebuffer_samples(24000 * 400 / 1000);  // 400ms @24kHz
    bool first_tts_pcm = false;
    bool tts_ok = ai.Synthesize(reply, [&](const int16_t* pcm, size_t n) {
        if (!first_tts_pcm) {
            first_tts_pcm = true;
            DiagMark(voice_diag::Stage::kTts, "tts_first_pcm");
        }
        while (playback_.Push(pcm, n) ==
               voice::PcmPlaybackQueue::PushResult::kFull) {
            // 队列满：等播放线程消化（背压），绝不丢音频块
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    });
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
    while (!playback_.EndReached() && NowMs() < drain_deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
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
