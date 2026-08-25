#pragma once

#include <atomic>
#include <cstdint>
#include <vector>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// 无屏语音总状态机：电源键 PTT（按住电源键说话、松开提交）。
// 按键回调只做事件标记（原子量 + 通知），真正执行录音/网络请求/TTS 的
// 独立工作线程按序编排：录音 → ASR → Chat(SSE) → TTS → 播报。
// 松开只代表停止录音，ASR/Chat/TTS 必须在工作线程中继续执行。
class HeadlessVoiceController {
public:
    static HeadlessVoiceController& Instance();

    // 初始化音频与提示音、创建工作线程（headless_main 调用一次）
    void Start();

    // 电源键按下/松开（iot_button 回调线程调用，非阻塞）
    void OnPttPressed();
    void OnPttReleased();

private:
    enum class State {
        kBoot,        // 启动中（忽略按键）
        kWaitWifi,    // 未联网/配网中
        kReady,       // 已联网且配置完整，可按键对话
        kRecording,   // 录音中（电源键按住）
        kProcessing,  // 已松开，ASR/Chat/TTS/播报进行中
    };

    HeadlessVoiceController() = default;

    static void WorkerTask(void* arg);
    void WorkerLoop();

    // 处理一次"按下"：校验前置条件后录音
    void HandlePress();
    void ProcessConversation(const std::vector<int16_t>& mic24k);
    void HandleAiFailure();

    // 把已重采样到目标采样率（ai.sample_rate）的 PCM 封装为 16bit 单声道 WAV
    bool BuildWav(const std::vector<int16_t>& pcm, int rate, std::vector<uint8_t>& wav);

    void SpeakPcm(const std::vector<int16_t>& pcm);

    State state_ = State::kBoot;
    std::atomic_bool booted_{false};      // Start() 完成前忽略按键
    std::atomic_bool ptt_held_{false};    // 电源键是否仍按住
    std::atomic_bool conversation_active_{false};  // 一轮对话（录音→播报）进行中
    std::atomic_bool speaking_{false};    // 正在播报 TTS（供提示音避让）
    TaskHandle_t worker_ = nullptr;
};