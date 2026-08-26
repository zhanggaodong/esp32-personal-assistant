#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

// 上行音频发送队列（计划 6.1）：
//   CaptureTask 把 100ms 重采样后的 16kHz PCM 帧入队；
//   VoiceSocketTask 出队并通过 WebSocket 发送。
//
// 固定容量上限（默认 5 帧 = 500ms）：队列满说明网络跟不上，Push 返回 kFull，
// 由控制器取消本轮并提示网络错误，不允许无界增长。
namespace voice {

class AudioUplinkQueue {
public:
    static constexpr size_t kMaxFrames = 5;

    enum class PushResult { kOk, kFull };

    // 入队一帧；队列已满返回 kFull（不覆盖、不丢旧帧）。
    PushResult Push(std::vector<uint8_t>&& frame);

    // 非阻塞出队；空队列返回 false。
    bool Pop(std::vector<uint8_t>& out);

    // 带超时出队：空队列最多等 timeout_ms；超时仍空返回 false。
    bool TryPopFor(size_t timeout_ms, std::vector<uint8_t>& out);

    // 清空（插话取消时调用）。
    void Clear();

    bool IsFull() const;
    bool IsEmpty() const;
    size_t Size() const;
    size_t Capacity() const { return kMaxFrames; }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::vector<uint8_t>> frames_;
};

}  // namespace voice