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
    if (samples_.size() < kPrebufferSamples || samples_.empty() ||
        max_samples == 0) {
        return 0;
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
    return samples_.size() >= kPrebufferSamples;
}

void PcmPlaybackQueue::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    samples_.clear();
    end_of_stream_ = false;
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