#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include "voice/voice_protocol.h"

class WebSocket;
class OpusDecoderWrapper;

// 设备语音 WebSocket 客户端（协议 v1，对应计划 Task 5 与第 4 节）。
//
// 职责：
//   - 复用 AiClient 的账号登录与 JWT；
//   - 建立/复用后端 /api/voice/device 的 WebSocket 会话并完成 hello 握手；
//   - 发送控制消息（turn.start/stop/cancel）与上行 PCM 二进制帧；
//   - 解析并分发服务端事件与下行 output_pcm / output_opus；
//   - 会话跨多轮复用、120s 空闲关闭、401/握手失败时清 token 并单次重登。
//
// 职责边界：本类是"传输 + 协议客户端"，不做完整轮次状态机与音频播放
// （由 HeadlessVoiceController / PlaybackTask 在 Task 6-8 负责）。
// 回调在 WebSocket 数据任务线程触发，调用方不得在其中长时间阻塞。
//
// opus 下行（type=3，60ms/24kHz/单声道）：WS 收帧线程只把原始帧入队，
// 由本类持有的专用解码任务（26KB 大栈，libopus 解码需要）解码后回调
// on_pcm。设备在 turn.start 里声明 audioCodec=opus，旧后端会忽略该字段
// 并继续下发 PCM 帧（type=2），两种格式本类都能处理。
class DeviceVoiceClient {
public:
    using OnPcmFn = std::function<void(uint32_t turn_id, uint32_t sequence,
                                       bool first, bool last,
                                       const int16_t* pcm, size_t count)>;
    using OnEventFn = std::function<void(const voice::ServerMessage& msg)>;
    using OnDisconnectedFn = std::function<void(const char* reason)>;

    static DeviceVoiceClient& Instance();

    // 刷新后端地址/账号/密码（配置变更后调用）。已建立的会话会被作废，
    // 下次 EnsureConnected 时以新配置重建。
    void UpdateConfig();

    bool IsConnected() const;

    // 建立（或复用）到后端语音 WebSocket 的会话，等待 hello 完成。
    // 返回最终连接是否可用；401/握手失败时自动重登并重试一次。
    bool EnsureConnected();

    // 开始一轮：发送 turn.start，并把 turn_id 记为当前活动轮。
    bool StartTurn(uint32_t turn_id, const std::string& conversation_id,
                   const std::string& voice, const std::string& language,
                   uint32_t max_record_seconds);

    // 停止一轮（松手）：发送 turn.stop。仍接受该轮下行输出直至 turn.done。
    bool StopTurn(uint32_t turn_id);

    // 取消一轮（插话/网络中断）：发送 turn.cancel，并立即使该轮输出失效
    // （后续迟到 output_pcm 因 turn_id != ActiveTurnId() 被丢弃）。
    bool CancelTurn(uint32_t turn_id, const std::string& reason);

    // 发送一帧上行 PCM（16bit 单声道；count 为样本数）。依赖 flags 位标记首/末帧。
    bool SendInputPcm(uint32_t turn_id, uint32_t sequence, bool first, bool last,
                      const int16_t* pcm, size_t count);

    // 主动断开（空闲/退出清理）。
    void Disconnect();

    void SetPcmCallback(OnPcmFn cb);
    void SetEventCallback(OnEventFn cb);
    void SetDisconnectedCallback(OnDisconnectedFn cb);

    // 由控制器任务在收到 kDisconnected 后调用：延迟回收已断开的 WebSocket。
    // 绝不在网络接收回调任务内析构连接对象（那会让接收任务等待自己退出，
    // 触发 EspSsl/EventGroup assert）。可安全重复调用（幂等）。
    void ReapDisconnectedSocket();

    // 当前活动轮 turn id（0 = 无活动轮）。
    uint32_t ActiveTurnId() const { return active_turn_.load(); }

private:
    DeviceVoiceClient() = default;
    // 必须在 .cc 中实现（彼处 WebSocket 完整），否则隐式析构在 .h 触发
    // 对不完整类型 unique_ptr<WebSocket> 求 sizeof 导致编译错误。
    ~DeviceVoiceClient();

    bool DoConnect();  // 建连 + 等 hello（不含重登重试）
    void HandleText(const char* data, size_t len);
    void HandleBinary(const char* data, size_t len);
    // 断线通知：只做"标记 + 一次性回调"，不析构任何对象（可从接收任务调用）。
    void HandleSocketDisconnected(uint32_t generation, const char* reason);
    static int64_t NowMs();

    // ---- opus 下行解码（专用大栈任务；WS 线程只入队） ----
    struct PendingOpusFrame {
        std::vector<uint8_t> data;
        uint32_t turn_id = 0;
    };
    static constexpr size_t kMaxQueuedOpusFrames = 96;  // ≈5.8 秒音频
    static void OpusDecodeTaskTrampoline(void* arg);
    void OpusDecodeLoop();
    void EnqueueOpusFrame(uint32_t turn_id, const uint8_t* data, size_t len);

    std::unique_ptr<WebSocket> ws_;
    mutable std::mutex mutex_;  // 保护 ws_ 与回调字段
    EventGroupHandle_t hello_evt_ = nullptr;

    // 连接代次：DoConnect 每次建连自增。回调整获代次后忽略旧连接迟到事件。
    std::atomic<uint32_t> connection_generation_{0};
    uint32_t active_connection_generation_ = 0;  // mutex_ 保护：当前建连代次

    std::atomic<uint32_t> active_turn_{0};
    std::atomic<int64_t> last_activity_ms_{0};  // 供 120s 空闲关闭判断
    bool disconnected_notified_ = false;        // mutex_ 保护：同一代次只通知一次
    bool socket_cleanup_pending_ = false;       // mutex_ 保护：待 ReapDisconnectedSocket 回收

    OnPcmFn on_pcm_;
    OnEventFn on_event_;
    OnDisconnectedFn on_disconnected_;

    std::mutex dec_mutex_;  // 保护 dec_queue_
    std::deque<PendingOpusFrame> dec_queue_;
    TaskHandle_t dec_task_ = nullptr;    // 惰性创建，常驻
    uint32_t decoded_turn_ = 0;          // 仅解码任务访问
    std::unique_ptr<OpusDecoderWrapper> tts_decoder_;  // 仅解码任务访问
};