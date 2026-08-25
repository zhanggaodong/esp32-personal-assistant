#pragma once

#include <cstdint>
#include <functional>
#include <mutex>

#include "audio/audio_codec.h"

// 本地语音提示播放器：播放内嵌 WAV 提示音（配网/错误等无网络、无法调用
// 服务端 TTS 的场景）。与 TTS 播报互斥：Play() 非阻塞，若正在播放或语音
// 状态机正处于录音/播报中（busy_check 返回 true）则返回 false，调用方
// 应降级为仅灯光提示（绝不打断正在播报的 PCM）。
class AudioPromptPlayer {
public:
    enum class Prompt {
        kEnterProvision,    // 进入配网模式
        kWaitProvision,     // 正在等待网络配置
        kProvisionSuccess,  // 网络配置成功
        kProvisionFail,     // 网络连接失败，请重新配置
        kNeedServiceConfig, // 请先在网页配置服务端账号和密码
        kLoginFail,         // 服务登录失败，请检查账号密码
        kWaitAnswer,        // 请等待当前回答结束
        kNoSpeech,          // 没有听清，请再试一次
        kNetworkError,      // 网络异常，请稍后再试
    };

    static AudioPromptPlayer& Instance();

    // 绑定板级 codec 与"语音忙"检查回调（录音中/播报中返回 true 时不再插入提示音）
    void Init(AudioCodec* codec, std::function<bool()> busy_check = nullptr);

    // 非阻塞：正在播放任何提示音/语音忙时返回 false，不打断、不排队。
    bool Play(Prompt prompt);

    bool IsPlaying() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return playing_;
    }

private:
    AudioPromptPlayer() = default;

    // 播放一段 WAV（RIFF/16bit PCM）数据；采样率与 codec 输出不一致时线性重采样。
    bool PlayWav(const uint8_t* data, size_t size);

    AudioCodec* codec_ = nullptr;
    std::function<bool()> busy_check_;
    mutable std::mutex mutex_;
    bool playing_ = false;
};