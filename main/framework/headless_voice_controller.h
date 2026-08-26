#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "voice/device_voice_client.h"
#include "voice/pcm_playback_queue.h"
#include "voice/streaming_resampler.h"
#include "voice/voice_diagnostics.h"

// 无屏语音总状态机：电源键 PTT（按住电源键说话、松开提交）。
//
// 双路径：
//   - legacy（默认）：整段录音 -> HTTP ASR -> Chat -> TTS -> 一次播完（Task 9 回滚路径）。
//   - stream_v1（ ai.voice_protocol==stream_v1 ）：边录边按 100ms 帧上行 WebSocket，
//     后端并行 ASR/Chat/TTS 并流式下发 PCM，设备固定容量边收边播、可按键插话。
//
// 事件驱动（stream_v1）：网络回调与按键回调只向事件队列投递事件，真正执行
// 录音/切换状态/抬灯的工作线程按状态机消费事件，不长时间阻塞单个循环。
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
        kProcessing,  // 仅 legacy 使用：已松开，ASR/Chat/TTS/播报进行中
        kWaitingAsr,  // 已松开，等后端校验/首字（不阻塞播放）
        kStreamingReply,  // Chat/TTS 流式回复进行中（边收边播）
        kSpeaking,    // 正在播报（本地标记播放中）
        kCancelling,  // 插话/取消清理中
    };

    // stream_v1 事件。
    enum class Event {
        kPress,         // 电源键按下
        kRelease,       // 电源键松开
        kTurnDone,      // 后端 turn.done（携带 conversationId）
        kTurnError,     // 后端 turn.error
        kDisconnected,  // WebSocket 断开
        kPlaybackEnd,   // 播放队列已播空且 EOS（一轮输出结束）
    };

    HeadlessVoiceController() = default;

    // 是否启用 stream_v1 协议（读 ai.voice_protocol）。
    bool UseStream() const;

    // —— 工作线程（状态机）——
    static void WorkerTask(void* arg);
    void WorkerLoop();

    // —— 流式路径 ——
    void OnEvent(const voice::ServerMessage& msg);  // DeviceVoiceClient 事件回调
    void OnPcm(uint32_t turn_id, uint32_t sequence, bool first, bool last,
               const int16_t* pcm, size_t count);  // DeviceVoiceClient PCM 回调
    void OnDisconnected(const char* reason);

    void HandlePressStream();  // kReady/(播报中)按下：开新一轮或插话
    void BeginRecording();     // 开麦 + 建立连接 + turn.start + 流式上行 + 收尾
    void CancelActiveTurn();   // 插话/断线：cancel + 清队列 + 关输出
    void HandleEvent(Event e); // 集中处理状态迁移
    void HandleTurnEnd(bool success);  // 一轮结束：成功(播完排空)或失败(报错)统一收尾

    // —— legacy 路径（保留）——
    void HandlePressLegacy();
    void ProcessConversation(const std::vector<int16_t>& mic24k);
    void HandleAiFailure();
    bool BuildWav(const std::vector<int16_t>& pcm, int rate, std::vector<uint8_t>& wav);
    void SpeakPcm(const std::vector<int16_t>& pcm);

    // —— 播放线程（stream_v1 边收边播）——
    static void PlaybackTask(void* arg);
    void PlaybackLoop();

    // 诊断：记录时间点 + 采集堆/栈水位 + 保存 RTC 阶段快照
    void DiagMark(voice_diag::Stage stage, const char* point);

    State state_ = State::kBoot;
    std::atomic_bool booted_{false};       // Start() 完成前忽略按键
    std::atomic_bool ptt_held_{false};     // 电源键是否仍按住
    std::atomic_bool speaking_{false};     // 正在播报 TTS（供提示音避让）
    std::atomic_bool conversation_active_{false};  // 一轮对话（录音→播报）进行中（legacy 与 stream 共用）
    TaskHandle_t worker_ = nullptr;
    TaskHandle_t playback_task_ = nullptr;
    QueueHandle_t event_queue_ = nullptr;  // stream_v1 事件队列（Event 类型）

    // —— 轮次/会话状态 ——
    uint32_t next_turn_id_ = 1;        // 每轮递增，作为该轮 turnId
    uint32_t current_turn_id_ = 0;     // 当前轮 turnId（供 RTC 快照）
    std::string conversation_id_;      // 最后一个已确认的会话 ID，下一轮复用
    std::mutex conv_mutex_;            // 保护 conversation_id_（工作线程与 WebSocket 回调线程）
    bool first_chat_delta_logged_ = false;  // 首字是否已打过诊断点
    uint64_t turn_deadline_ms_ = 0;    // 等待 turn.done 的看门狗截止时刻（单调毫秒）
    voice_diag::Timeline timeline_;    // 分阶段时间线（单调毫秒）

    // —— 流式音频缓冲 ——
    voice::PcmPlaybackQueue playback_;         // 下行播放队列
    voice::StreamingResampler resampler_;      // 24k -> 16k 有状态重采样
    std::vector<int16_t> resample_buf_;        // 每次 Process 的临时输出
    std::vector<int16_t> frame_buf_;           // 聚合中的 16k 样本（未满 100ms）
    uint32_t turn_seq_ = 0;                    // 上一帧 sequence
    uint64_t rec_start_ms_ = 0;                // 录音起点（单调毫秒）
};