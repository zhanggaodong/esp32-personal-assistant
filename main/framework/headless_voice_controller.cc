#include "headless_voice_controller.h"

#include <algorithm>
#include <cstring>
#include <string.h>

#include <esp_log.h>

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

// 录音时长边界：最短 200ms，最长 8s（达到上限自动按"松开"路径提交）
constexpr size_t kMinRecordMs = 200;
constexpr size_t kMaxRecordMs = 8000;

// 单次麦克风读取块（采样点，20ms @24kHz）
constexpr size_t kMicChunkSamples = 480;

// 线性重采样：src(采样率 src_rate) -> out(dst_rate)。
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

    // 配置变更（ai.*）即时刷新登录参数；地址/账号/密码变化会让旧 token 失效
    EventBus::Instance().Subscribe(kEventConfigChanged, [](int32_t, void* data) {
        if (data == nullptr || strstr(static_cast<const char*>(data), "ai.") != nullptr) {
            AiClient::Instance().UpdateConfig();
        }
    });

    state_ = State::kWaitWifi;
    booted_ = true;
    if (xTaskCreate(WorkerTask, "headless_voice", 8192, this, 5, &worker_) != pdPASS) {
        ESP_LOGE(TAG, "failed to create headless voice worker");
        return;
    }
    ESP_LOGI(TAG, "headless voice started (PTT: press=record, release=submit)");
}

void HeadlessVoiceController::OnPttPressed() {
    if (!booted_.load()) {
        return;  // 启动阶段忽略
    }
    // 一轮回答（录音/ASR/Chat/TTS/播报）尚未结束：忽略本次按键并短提示，
    // 不允许并发录音、并发 HTTP 或打断正在播放的 PCM（提示音自身会让路）。
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
    ptt_held_ = false;  // 工作线程据此停止采集并提交
}

void HeadlessVoiceController::WorkerTask(void* arg) {
    static_cast<HeadlessVoiceController*>(arg)->WorkerLoop();
    vTaskDelete(nullptr);
}

void HeadlessVoiceController::WorkerLoop() {
    for (;;) {
        // 等待"电源键按下"通知（松开由录音循环直接感知 ptt_held_）
        if (ulTaskNotifyTake(pdFALSE, portMAX_DELAY) == 0) {
            continue;
        }
        HandlePress();
    }
}

void HeadlessVoiceController::HandlePress() {
    auto& led = HeadlessLedController::Instance();
    auto& prompt = AudioPromptPlayer::Instance();
    auto& net = HeadlessNetworkController::Instance();

    // 一轮回答尚未结束：不并发录音/HTTP/打断播放
    if (state_ == State::kProcessing || state_ == State::kRecording) {
        prompt.Play(AudioPromptPlayer::Prompt::kWaitAnswer);
        led.ShowProcessing();
        return;
    }

    // 前置条件：联网且不在配网；配置完整
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

    // ---- 录音 ----
    state_ = State::kRecording;
    conversation_active_.store(true);
    led.ShowRecording();

    // 诊断（Task 1）：本轮到当前 turnId，时间线从按下电源键开始
    current_turn_id_ = next_turn_id_++;
    timeline_.Reset(0);
    DiagMark(voice_diag::Stage::kRecording, "ptt_down");

    std::vector<int16_t> mic24k;
    const size_t max_samples = kMaxRecordMs * (size_t)codec->input_sample_rate() / 1000;
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
    ptt_held_ = false;  // 进入处理，忽略残留松开

    if (mic_error || mic24k.empty()) {
        // 输入设备异常/空数据：关闭输入、红灯提示、回待机（不播"网络"误导提示）
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

    // ---- 松开后链路：ASR → Chat → TTS → 播报 ----
    state_ = State::kProcessing;
    led.ShowProcessing();
    ProcessConversation(mic24k);

    // 本轮流结束：打印整条时间线（含各子阶段耗时）
    timeline_.Log(TAG);

    conversation_active_.store(false);
    state_ = State::kReady;
    led.ShowReady();
}

void HeadlessVoiceController::ProcessConversation(const std::vector<int16_t>& mic24k) {
    auto& ai = AiClient::Instance();
    auto* codec = Board::GetInstance().GetAudioCodec();

    // 重采样到网页配置的 ASR 采样率（8000/16000）并封装为 WAV
    int target = ConfigStore::Instance().GetInt("ai.sample_rate", 16000);
    std::vector<int16_t> pcm16k;
    Resample(mic24k, codec != nullptr ? codec->input_sample_rate() : 24000,
             target, pcm16k);
    std::vector<uint8_t> wav;
    if (pcm16k.empty() || !BuildWav(pcm16k, target, wav)) {
        HandleAiFailure();
        return;
    }

    // ASR：语音 → 文本
    DiagMark(voice_diag::Stage::kAsr, "asr_request_start");
    std::string question;
    if (!ai.Transcribe(wav, question) || question.empty()) {
        ESP_LOGE(TAG, "asr failed or empty");
        HandleAiFailure();
        return;
    }
    ESP_LOGI(TAG, "asr ok: %s", question.c_str());
    DiagMark(voice_diag::Stage::kAsr, "asr_final");

    // Chat：SSE 流式文本（增量在此累加，供 TTS 使用）
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

    // TTS：文本 → PCM，随后播报
    DiagMark(voice_diag::Stage::kTts, "tts_request_start");
    std::vector<int16_t> tts_pcm;
    bool first_tts_pcm_logged = false;
    bool tts_ok = ai.Synthesize(reply, [&](const int16_t* pcm, size_t n) {
        if (!first_tts_pcm_logged) {
            first_tts_pcm_logged = true;
            DiagMark(voice_diag::Stage::kTts, "tts_first_pcm");
        }
        tts_pcm.insert(tts_pcm.end(), pcm, pcm + n);
    });
    if (!tts_ok || tts_pcm.empty()) {
        ESP_LOGE(TAG, "tts failed or empty pcm");
        HandleAiFailure();
        return;
    }
    ESP_LOGI(TAG, "tts ok, pcm=%u samples", (unsigned)tts_pcm.size());
    DiagMark(voice_diag::Stage::kTts, "tts_done");
    SpeakPcm(tts_pcm);
    DiagMark(voice_diag::Stage::kPlayback, "speaker_done");
}

void HeadlessVoiceController::HandleAiFailure() {
    // 401 已由 AiClient 内部"清除 token → 重登 → 重试一次"消化；
    // 仍失败时按 token 是否还在区分登录失败与网络异常。
    auto& prompt = AudioPromptPlayer::Instance();
    auto& led = HeadlessLedController::Instance();
    // 诊断：本轮异常结束，标注 Cancel 阶段并打印时间线/资源
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

void HeadlessVoiceController::SpeakPcm(const std::vector<int16_t>& pcm) {
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr || pcm.empty()) {
        return;
    }
    // 播报标记先行置位：提示音播放器见"录音/播报中"会让路
    speaking_.store(true);
    // 本地提示音若刚好在播（配网/错误音），等它结束再播，避免喇叭混音
    auto& prompt = AudioPromptPlayer::Instance();
    while (prompt.IsPlaying()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    codec->EnableOutput(true);
    DiagMark(voice_diag::Stage::kPlayback, "speaker_first_pcm");
    const size_t chunk_samples = 1920;  // 80ms @24kHz
    for (size_t off = 0; off < pcm.size() && codec->output_enabled(); off += chunk_samples) {
        size_t n = std::min(chunk_samples, pcm.size() - off);
        std::vector<int16_t> chunk(pcm.begin() + off, pcm.begin() + off + n);
        codec->OutputData(chunk);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    codec->EnableOutput(false);
    speaking_.store(false);
}