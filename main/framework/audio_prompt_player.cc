#include "audio_prompt_player.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "AudioPrompt"

// 内嵌 WAV 提示音符号：文件名（小写、下划线）-> _binary_<name>_wav_start/_end
#define EMBED_WAV(name)                                                          \
    extern const char name##_wav_start[] asm("_binary_" #name "_wav_start");      \
    extern const char name##_wav_end[] asm("_binary_" #name "_wav_end")

EMBED_WAV(pkg_enter_config);
EMBED_WAV(pkg_wait_network);
EMBED_WAV(pkg_network_success);
EMBED_WAV(pkg_network_fail);
EMBED_WAV(pkg_need_service);
EMBED_WAV(pkg_login_fail);
EMBED_WAV(pkg_wait_answer);
EMBED_WAV(pkg_no_speech);
EMBED_WAV(pkg_network_error);

namespace {

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

uint32_t ReadU32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

uint16_t ReadU16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

bool IsFourcc(const uint8_t* p, const char* cc) {
    return memcmp(p, cc, 4) == 0;
}

}  // namespace

AudioPromptPlayer& AudioPromptPlayer::Instance() {
    static AudioPromptPlayer instance;
    return instance;
}

void AudioPromptPlayer::Init(AudioCodec* codec, std::function<bool()> busy_check) {
    codec_ = codec;
    busy_check_ = std::move(busy_check);
}

bool AudioPromptPlayer::Play(Prompt prompt) {
    if (codec_ == nullptr) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (playing_) {
            return false;  // 提示音播放中：不打断
        }
        if (busy_check_ && busy_check_()) {
            return false;  // 录音/播报中：不插入提示音，避免喇叭混音
        }
        playing_ = true;
    }

    bool ok = false;
    switch (prompt) {
        case Prompt::kEnterProvision:
            ok = PlayWav((const uint8_t*)pkg_enter_config_wav_start,
                         pkg_enter_config_wav_end - pkg_enter_config_wav_start);
            break;
        case Prompt::kWaitProvision:
            ok = PlayWav((const uint8_t*)pkg_wait_network_wav_start,
                         pkg_wait_network_wav_end - pkg_wait_network_wav_start);
            break;
        case Prompt::kProvisionSuccess:
            ok = PlayWav((const uint8_t*)pkg_network_success_wav_start,
                         pkg_network_success_wav_end - pkg_network_success_wav_start);
            break;
        case Prompt::kProvisionFail:
            ok = PlayWav((const uint8_t*)pkg_network_fail_wav_start,
                         pkg_network_fail_wav_end - pkg_network_fail_wav_start);
            break;
        case Prompt::kNeedServiceConfig:
            ok = PlayWav((const uint8_t*)pkg_need_service_wav_start,
                         pkg_need_service_wav_end - pkg_need_service_wav_start);
            break;
        case Prompt::kLoginFail:
            ok = PlayWav((const uint8_t*)pkg_login_fail_wav_start,
                         pkg_login_fail_wav_end - pkg_login_fail_wav_start);
            break;
        case Prompt::kWaitAnswer:
            ok = PlayWav((const uint8_t*)pkg_wait_answer_wav_start,
                         pkg_wait_answer_wav_end - pkg_wait_answer_wav_start);
            break;
        case Prompt::kNoSpeech:
            ok = PlayWav((const uint8_t*)pkg_no_speech_wav_start,
                         pkg_no_speech_wav_end - pkg_no_speech_wav_start);
            break;
        case Prompt::kNetworkError:
            ok = PlayWav((const uint8_t*)pkg_network_error_wav_start,
                         pkg_network_error_wav_end - pkg_network_error_wav_start);
            break;
    }

    if (!ok) {
        ESP_LOGW(TAG, "prompt play failed (prompt=%d)", (int)prompt);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        playing_ = false;
    }
    return ok;
}

bool AudioPromptPlayer::PlayWav(const uint8_t* p, size_t size) {
    if (p == nullptr || size < 12) {
        return false;
    }
    if (!IsFourcc(p, "RIFF") || !IsFourcc(p + 8, "WAVE")) {
        ESP_LOGE(TAG, "not a RIFF/WAVE file");
        return false;
    }

    size_t offset = 12;
    int sample_rate = 16000;
    uint16_t channels = 1;
    uint16_t bits = 16;
    const uint8_t* data = nullptr;
    size_t data_len = 0;

    while (offset + 8 <= size) {
        const uint8_t* chunk = p + offset;
        uint32_t len = ReadU32(chunk + 4);
        const uint8_t* body = chunk + 8;
        if (body + len > p + size) {
            ESP_LOGW(TAG, "wav chunk overrun, truncating");
            len = (uint32_t)((p + size) - body);
        }
        if (IsFourcc(chunk, "fmt ") && len >= 16) {
            channels = ReadU16(body + 2);
            sample_rate = (int)ReadU32(body + 4);
            bits = ReadU16(body + 14);
        } else if (IsFourcc(chunk, "data")) {
            data = body;
            data_len = len;
            break;
        }
        offset = offset + 8 + len + (len & 1);  // 块按 2 字节对齐
    }

    if (data == nullptr || data_len == 0) {
        ESP_LOGE(TAG, "wav has no data chunk");
        return false;
    }
    if (channels != 1 || bits != 16) {
        ESP_LOGE(TAG, "unsupported wav format: ch=%u bits=%u", channels, bits);
        return false;
    }

    const int16_t* samples = (const int16_t*)data;
    size_t sample_count = data_len / 2;
    std::vector<int16_t> resampled;
    const int16_t* play = samples;
    size_t play_count = sample_count;

    std::vector<int16_t> src(samples, samples + sample_count);
    if (sample_rate != codec_->output_sample_rate()) {
        std::vector<int16_t> dst;
        Resample(src, sample_rate, codec_->output_sample_rate(), dst);
        resampled = std::move(dst);
        play = resampled.data();
        play_count = resampled.size();
    }

    codec_->EnableOutput(true);
    const size_t kChunkSamples = 1920;  // 80ms @24kHz
    size_t off = 0;
    while (off < play_count && codec_->output_enabled()) {
        size_t n = std::min(kChunkSamples, play_count - off);
        std::vector<int16_t> chunk(play + off, play + off + n);
        codec_->OutputData(chunk);
        off += n;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    codec_->EnableOutput(false);
    return true;
}