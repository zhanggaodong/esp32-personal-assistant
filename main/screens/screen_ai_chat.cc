#include "screen_ai_chat.h"

#include <algorithm>
#include <cstring>

#include <esp_log.h>

#include <lvgl.h>

#include "board.h"
#include "audio/audio_codec.h"
#include "display/display.h"
#include "config/config_store.h"
#include "framework/event_bus.h"
#include "framework/ai/ai_client.h"

// 中文显示字体（跟随板级配置，见 CMakeLists BUILTIN_TEXT_FONT）
LV_FONT_DECLARE(BUILTIN_TEXT_FONT);

#define TAG "ScreenAiChat"

namespace {

// 录音时长（毫秒）保护：避免长时间录音把整段 PCM 放在内部 RAM 撑爆内存。
// 之后如需长录音，可把缓冲分配到 PSRAM 或改为边录边写 LittleFS。
constexpr size_t kMaxRecordMs = 8000;

// 一块麦克风读取块（采样点）
constexpr size_t kMicChunkSamples = 480;  // 20ms @24kHz

// 线性重采样：把 src（采样率 src_rate）插值到 dst_rate，输出到 out。
void Resample(const std::vector<int16_t>& src, int src_rate, int dst_rate,
              std::vector<int16_t>& out) {
    out.clear();
    if (dst_rate <= 0 || src.empty()) {
        return;
    }
    if (src_rate == dst_rate) {
        out = src;
        return;
    }
    const double ratio = (double)src_rate / (double)dst_rate;  // 源中每输出一个取多少
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

}  // namespace

ScreenAiChat::ScreenAiChat() {
    metadata_ = {"ai_chat", "AI 对话", false};
}

ScreenAiChat::~ScreenAiChat() {
    onStop();
}

const AppMetadata& ScreenAiChat::metadata() {
    return metadata_;
}

void ScreenAiChat::onStart() {
    if (started_) {
        return;
    }
    started_ = true;

    // 配置变更时刷新后端地址/账号/密码/音色
    EventBus::Instance().Subscribe(kEventConfigChanged,
        [this](int32_t, void* data) {
            if (data != nullptr) {
                const char* key = static_cast<const char*>(data);
                if (key == nullptr || strstr(key, "ai.") != nullptr) {
                    AiClient::Instance().UpdateConfig();
                }
            }
        });

    // 硬音频：直接驱动板级 codec，无需 Application 的音频服务
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec != nullptr) {
        codec->Start();
        codec->EnableInput(false);
        codec->EnableOutput(false);
    }

    AiClient::Instance().UpdateConfig();

    // 工作线程：跑 ASR/对话/TTS 阻塞链路
    xTaskCreate(WorkerTask, "ai_worker", 8192, this, 5, &worker_);
    ESP_LOGI(TAG, "AI chat started");
}

void ScreenAiChat::BuildUi() {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard lock(display);
    lv_obj_t* scr = lv_screen_active();

    if (title_label_ == nullptr) {
        title_label_ = lv_label_create(scr);
    }
    lv_label_set_text(title_label_, "AI 语音助手");
    lv_obj_set_style_text_font(title_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(title_label_, lv_color_white(), 0);
    lv_obj_set_width(title_label_, LV_HOR_RES);
    lv_obj_set_style_text_align(title_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(title_label_, 0, 8);

    if (status_label_ == nullptr) {
        status_label_ = lv_label_create(scr);
    }
    lv_obj_set_style_text_font(status_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(status_label_, lv_color_make(160, 224, 255), 0);
    lv_obj_set_width(status_label_, LV_HOR_RES);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(status_label_, 0, 40);
    lv_label_set_text(status_label_, "按确认键开始说话");

    // 可滚动文本区
    if (scroll_cont_ == nullptr) {
        scroll_cont_ = lv_obj_create(scr);
    }
    lv_obj_set_width(scroll_cont_, LV_HOR_RES - 8);
    lv_obj_set_height(scroll_cont_, LV_VER_RES - 72);
    lv_obj_set_pos(scroll_cont_, 4, 62);
    lv_obj_set_scroll_dir(scroll_cont_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroll_cont_, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_border_width(scroll_cont_, 0, 0);

    if (text_label_ == nullptr) {
        text_label_ = lv_label_create(scroll_cont_);
    }
    lv_obj_set_width(text_label_, lv_obj_get_width(scroll_cont_));
    lv_label_set_long_mode(text_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(text_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(text_label_, lv_color_white(), 0);
    lv_label_set_text(text_label_, "你好，我是你的桌面助手，直接说话即可。");
    lv_obj_align(text_label_, LV_ALIGN_TOP_LEFT, 0, 0);
}

void ScreenAiChat::onShow() {
    if (!started_) {
        onStart();
    }
    if (!visible_) {
        visible_ = true;
        BuildUi();
        SetStatus("就绪");
        SetReplay("你好，我是你的桌面助手，直接说话即可。");
    }
    ESP_LOGI(TAG, "AI chat shown");
}

void ScreenAiChat::onHide() {
    if (!visible_) {
        return;
    }
    visible_ = false;
    // 若正在录音则先停止
    listening_ = false;
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard lock(display);
    if (title_label_ != nullptr) lv_obj_add_flag(title_label_, LV_OBJ_FLAG_HIDDEN);
    if (status_label_ != nullptr) lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    if (scroll_cont_ != nullptr) lv_obj_add_flag(scroll_cont_, LV_OBJ_FLAG_HIDDEN);
}

void ScreenAiChat::onStop() {
    if (worker_ != nullptr) {
        listening_ = false;
        vTaskDelete(worker_);
        worker_ = nullptr;
    }
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard lock(display);
    if (text_label_ != nullptr) { lv_obj_delete(text_label_); text_label_ = nullptr; }
    if (scroll_cont_ != nullptr) { lv_obj_delete(scroll_cont_); scroll_cont_ = nullptr; }
    if (status_label_ != nullptr) { lv_obj_delete(status_label_); status_label_ = nullptr; }
    if (title_label_ != nullptr) { lv_obj_delete(title_label_); title_label_ = nullptr; }
    ESP_LOGI(TAG, "AI chat stopped");
}

void ScreenAiChat::onConfigChanged(const char* key) {
    // onStart 已订阅 event bus 处理 ai.* 变更
}

void ScreenAiChat::SetStatus(const std::string& s) {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr || status_label_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display);
    lv_label_set_text(status_label_, s.c_str());
}

void ScreenAiChat::SetReplay(const std::string& s) {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr || text_label_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display);
    lv_label_set_text(text_label_, s.c_str());
    lv_label_set_long_mode(text_label_, LV_LABEL_LONG_WRAP);
    lv_obj_scroll_to_view_recursive(text_label_, LV_ANIM_OFF);
}

void ScreenAiChat::ToggleRecording() {
    if (!started_) {
        return;
    }
    if (!ConfigStore::Instance().GetBool("ai.enabled", false)) {
        SetStatus("AI 对话未启用，请在网页打开");
        return;
    }
    if (worker_ == nullptr) {
        return;
    }
    if (!listening_.load()) {
        listening_ = true;
        xTaskNotifyGive(worker_);
    } else {
        listening_ = false;
    }
}

void ScreenAiChat::WorkerTask(void* arg) {
    static_cast<ScreenAiChat*>(arg)->WorkerLoop();
}

void ScreenAiChat::WorkerLoop() {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // 等"录音开始"

        auto* codec = Board::GetInstance().GetAudioCodec();
        if (codec == nullptr) {
            SetStatus("无音频设备");
            listening_ = false;
            continue;
        }

        SetStatus("录音中…");
        std::vector<int16_t> mic24k;
        const size_t max_samples = kMaxRecordMs * (size_t)codec->input_sample_rate() / 1000;
        codec->EnableInput(true);
        while (listening_.load() && mic24k.size() < max_samples) {
            std::vector<int16_t> chunk(kMicChunkSamples);
            if (codec->InputData(chunk)) {
                mic24k.insert(mic24k.end(), chunk.begin(), chunk.end());
            } else {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        codec->EnableInput(false);
        listening_ = false;

        if (!mic24k.empty()) {
            RunConversation(mic24k);
        }
        SetStatus("就绪");
    }
}

void ScreenAiChat::RunConversation(const std::vector<int16_t>& mic24k) {
    auto& ai = AiClient::Instance();

    // 采样率：ai.sample_rate（8000/16000），默认 16000
    int target = ConfigStore::Instance().GetInt("ai.sample_rate", 16000);
    std::vector<int16_t> pcm16k;
    Resample(mic24k, (int)Board::GetInstance().GetAudioCodec()->input_sample_rate(),
             target, pcm16k);

    std::vector<uint8_t> wav;
    if (!BuildWav16k(pcm16k, wav)) {
        SetStatus("录音为空");
        return;
    }

    // ASR：语音 → 文本
    SetStatus("识别中…");
    std::string question;
    if (!ai.Transcribe(wav, question) || question.empty()) {
        SetStatus("识别失败，请重试");
        return;
    }

    // 对话：流式文本
    SetStatus("思考中…");
    std::string reply;
    std::string conv_id;
    bool chat_ok = ai.Chat(question, [&](const char* delta) {
        reply += delta;
        SetReplay("我：" + question + "\n---\n" + reply);
    }, conv_id);
    if (!chat_ok || reply.empty()) {
        SetStatus("AI 回复失败");
        return;
    }
    SetReplay("我：" + question + "\n---\n" + reply);

    // TTS：文本 → 语音
    SetStatus("播报中…");
    std::vector<int16_t> tts_pcm;
    ai.Synthesize(reply, [&](const int16_t* p, size_t n) {
        tts_pcm.insert(tts_pcm.end(), p, p + n);
    });
    SpeakPcm(tts_pcm);
}

bool ScreenAiChat::BuildWav16k(const std::vector<int16_t>& pcm,
                               std::vector<uint8_t>& wav) {
    if (pcm.empty()) {
        return false;
    }
    const uint32_t data_bytes = (uint32_t)pcm.size() * 2;
    wav.clear();
    wav.reserve(44 + data_bytes);

    uint32_t sample_rate = (uint32_t)ConfigStore::Instance().GetInt("ai.sample_rate", 16000);
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
    put32(sample_rate);
    put32(sample_rate * 2);  // byte rate
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

void ScreenAiChat::SpeakPcm(const std::vector<int16_t>& pcm) {
    if (pcm.empty()) {
        return;
    }
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        return;
    }
    codec->EnableOutput(true);
    const size_t chunk_samples = 1920;  // 80ms @24kHz
    for (size_t off = 0; off < pcm.size(); off += chunk_samples) {
        size_t n = std::min(chunk_samples, pcm.size() - off);
        std::vector<int16_t> chunk(pcm.begin() + off, pcm.begin() + off + n);
        codec->OutputData(chunk);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    codec->EnableOutput(false);
}