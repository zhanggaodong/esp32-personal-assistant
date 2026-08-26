#include "pcm_playback_queue.h"

#include <algorithm>

namespace voice {

PcmPlaybackQueue::PushResult PcmPlaybackQueue::Push(const int16_t* pcm,
                                                    size_t count) {
    if (pcm == nullptr || count == 0) {
        return PushResult::kOk;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (samples_.size() + count > kMaxSamples) {
        return PushResult::kFull;  // 背压信号：不扩容、不覆盖
    }
    samples_.insert(samples_.end(), pcm, pcm + count);
    return PushResult::kOk;
}

size_t PcmPlaybackQueue::PopChunk(std::vector<int16_t>& out,
                                  size_t max_samples) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (samples_.empty() || max_samples == 0) {
        return 0;
    }
    // 预缓冲只在"开播前"生效：攒够 prebuffer_samples_ 才允许第一次出队，
    // 抵抗网络抖动。一旦开播就不再回退到预缓冲线——否则句间网络抖动会让
    // 播放反复停等重新攒缓冲（插入长静音），且 turn.done 后剩余不足
    // 预缓冲线的尾音永远无法出队，EndReached 永不成立（收尾死锁）。
    if (!playing_) {
        const bool enough = samples_.size() >= prebuffer_samples_;
        if (!enough && !end_of_stream_) {
            return 0;
        }
        playing_ = true;
    }
    const size_t take = std::min(max_samples, samples_.size());
    out.reserve(out.size() + take);
    for (size_t i = 0; i < take; ++i) {
        out.push_back(samples_.front());
        samples_.pop_front();
    }
    return take;
}

size_t PcmPlaybackQueue::BufferedSamples() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return samples_.size();
}

bool PcmPlaybackQueue::Ready() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return samples_.size() >= prebuffer_samples_;
}

void PcmPlaybackQueue::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    samples_.clear();
    end_of_stream_ = false;
    playing_ = false;  // 新一轮重新走预缓冲
    ++generation_;
}

void PcmPlaybackQueue::MarkEndOfStream() {
    std::lock_guard<std::mutex> lock(mutex_);
    end_of_stream_ = true;
}

bool PcmPlaybackQueue::EndReached() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return end_of_stream_ && samples_.empty();
}

}  // namespace voice
