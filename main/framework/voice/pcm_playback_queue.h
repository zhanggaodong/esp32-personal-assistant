#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

// 下行播放队列（计划 7）：VoiceSocketTask 只入队 PCM，
// 独立 PlaybackTask 出队并调用 codec->OutputData()。
//
// 规则：
//   - 固定容量：最大缓存 2 秒（24kHz/16bit/单声道 = 48000 采样）；
//     达到上限时 Push 返回 kFull，网络读取侧施加背压，不扩容。
//   - 预缓冲：累积到 kPrebufferSamples 前 PopChunk 返回空（不启动播放）；
//     达到后按需出队，抵抗网络抖动。
//   - 插话打断：Clear() 立即清空并递增 generation，旧轮残留音频无效。
//   - 结束语义：MarkEndOfStream() 后，PlaybackTask 播到队列空即视为结束
//     （EndReached()），控制器再回 Ready。
namespace voice {

class PcmPlaybackQueue {
public:
    static constexpr size_t kSampleRate = 24000;
    static constexpr size_t kMaxSamples = kSampleRate * 2;        // 2 秒
    static constexpr size_t kPrebufferMs = 120;                    // 120ms 预缓冲
    static constexpr size_t kPrebufferSamples =
        kSampleRate * kPrebufferMs / 1000;  // 2880

    enum class PushResult { kOk, kFull };

    // 入队 PCM（16bit 单声道采样）。
    PushResult Push(const int16_t* pcm, size_t count);

    // 出队最多 max_samples 个采样；预缓冲未达标或队列空返回 0。
    // out 需要在调用前 clear。
    size_t PopChunk(std::vector<int16_t>& out, size_t max_samples);

    // 当前缓冲的采样数（秒 = samples / kSampleRate）。
    size_t BufferedSamples() const;

    // 是否已达到预缓冲线（可开始播放）。
    bool Ready() const;

    // 插话打断：清空并递增 generation，任何旧数据失效。
    void Clear();

    // 网络侧声明"本段音频到此结束"。
    void MarkEndOfStream();

    // EndOfStream 已置位且队列已播空。
    bool EndReached() const;

    size_t generation() const { return generation_; }

private:
    mutable std::mutex mutex_;
    std::deque<int16_t> samples_;
    bool end_of_stream_ = false;
    size_t generation_ = 0;
};

}  // namespace voice