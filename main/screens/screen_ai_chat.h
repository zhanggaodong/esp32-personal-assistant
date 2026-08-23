#pragma once

#include <atomic>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "framework/app/base_app.h"

// AI 语音对话模块（Phase D）。
//
// 完整语音链路：录音(麦克风) → ASR → AI 对话(流式文字) → TTS → 播报(扬声器)。
// - 物理按键/网页在 ai_chat 屏内按下"确认"（经 InputRouter→kEventAiChatRecord）
//   切换"录音开始/停止"。
// - 后端地址/账号/密码/音色在网页 "AI对话" 分组配置，运行时时登录拿 JWT。
// - 硬音频驱动直接使用板级 codec 的 InputData/OutputData（框架模式无 Application 音频服务）。
class ScreenAiChat : public BaseApp {
public:
    ScreenAiChat();
    ~ScreenAiChat();

    const AppMetadata& metadata() override;
    void onStart() override;
    void onShow() override;
    void onHide() override;
    void onStop() override;
    void onConfigChanged(const char* key) override;

    // 切换"录音开始/停止"（可由 InputRouter 或其它模块调用）
    void ToggleRecording();

private:
    enum class State { kIdle, kListening, kThinking, kSpeaking };

    void BuildUi();
    void SetStatus(const std::string& s);
    void SetReplay(const std::string& s);

    // 工作线程：承载阻塞网络流水线，避免卡住 LVGL/UI 线程
    static void WorkerTask(void* arg);
    void WorkerLoop();
    void RunConversation(const std::vector<int16_t>& mic24k);
    bool BuildWav16k(const std::vector<int16_t>& pcm, std::vector<uint8_t>& wav);
    void SpeakPcm(const std::vector<int16_t>& pcm);

    AppMetadata metadata_;
    bool started_ = false;
    bool visible_ = false;
    State state_ = State::kIdle;

    // 线程同步 + 录音状态
    TaskHandle_t worker_ = nullptr;
    std::atomic_bool listening_{false};   // 正在采集麦克风

    // LVGL 对象
    void* title_label_ = nullptr;
    void* status_label_ = nullptr;
    void* scroll_cont_ = nullptr;
    void* text_label_ = nullptr;
};