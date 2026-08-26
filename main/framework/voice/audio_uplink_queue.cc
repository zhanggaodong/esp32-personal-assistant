#include "audio_uplink_queue.h"

#include <chrono>

namespace voice {

AudioUplinkQueue::PushResult AudioUplinkQueue::Push(std::vector<uint8_t>&& frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (frames_.size() >= kMaxFrames) {
        return PushResult::kFull;
    }
    frames_.push_back(std::move(frame));
    cv_.notify_one();
    return PushResult::kOk;
}

bool AudioUplinkQueue::Pop(std::vector<uint8_t>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (frames_.empty()) {
        return false;
    }
    out = std::move(frames_.front());
    frames_.pop_front();
    return true;
}

bool AudioUplinkQueue::TryPopFor(size_t timeout_ms, std::vector<uint8_t>& out) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                      [this] { return !frames_.empty(); })) {
        return false;
    }
    out = std::move(frames_.front());
    frames_.pop_front();
    return true;
}

void AudioUplinkQueue::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    frames_.clear();
}

bool AudioUplinkQueue::IsFull() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_.size() >= kMaxFrames;
}

bool AudioUplinkQueue::IsEmpty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_.empty();
}

size_t AudioUplinkQueue::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_.size();
}

}  // namespace voice