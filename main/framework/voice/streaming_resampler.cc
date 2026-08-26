#include "streaming_resampler.h"

#include <algorithm>
#include <cmath>

namespace voice {

namespace {

inline int32_t Clamp16(int32_t v) {
    return std::max(-32768, std::min(32767, v));
}

}  // namespace

bool StreamingResampler::Configure(size_t src_rate, size_t dst_rate) {
    if (src_rate == 0 || dst_rate == 0 || src_rate < dst_rate ||
        src_rate % dst_rate != 0) {
        // 只支持整数倍降采样（24k->16k、48k->16k 等），避免一般有理数重采样的
        // 额外复杂度；后续需要更细粒度时可以单独扩展。
        return false;
    }
    src_rate_ = src_rate;
    dst_rate_ = dst_rate;
    ratio_ = (double)src_rate_ / (double)dst_rate_;
    configured_ = true;
    Reset();
    return true;
}

void StreamingResampler::Reset() {
    pos_ = 0.0;
    fed_ = 0;
    produced_ = 0;
    has_prev_ = false;
    prev_sample_ = 0;
    has_prev2_ = false;
    prev2_sample_ = 0;
}

int16_t StreamingResampler::SampleAt(int64_t abs_index) const {
    if (abs_index == fed_ - 1) {
        return has_prev_ ? prev_sample_ : 0;  // 上一块最后一个采样
    }
    return 0;  // 只允许读上一块结尾；当前块内采样由 Process 直接取用
}

void StreamingResampler::Process(const int16_t* in, size_t in_count,
                                 std::vector<int16_t>& out) {
    if (!configured_ || in == nullptr || in_count == 0) {
        return;
    }

    const int64_t block_start = fed_;        // 当前块第一个采样的绝对序号
    const int64_t last = block_start + (int64_t)in_count - 1;  // 最后一个

    auto sample_at = [&](int64_t idx) -> int16_t {
        if (idx == block_start - 1) {
            return has_prev_ ? prev_sample_ : 0;  // 边界：上一块末尾
        }
        if (idx >= block_start && idx <= last) {
            return in[idx - block_start];
        }
        return 0;  // 不会发生（调用前已确保 i1 <= last）
    };

    // 连续产出：v1 可用（插值对的第二个采样存在）之前不产出，
    // 保证相位跨块持续推进，不重复、不丢样。
    while (true) {
        const int64_t i0 = (int64_t)std::floor(pos_);
        const int64_t i1 = i0 + 1;
        if (i1 > last) {
            break;  // 还差末尾采样，等下一块
        }
        const double frac = pos_ - (double)i0;
        const int32_t v0 = sample_at(i0);
        const int32_t v1 = sample_at(i1);
        out.push_back((int16_t)Clamp16(v0 + (int32_t)((int64_t)(v1 - v0) * frac)));
        ++produced_;
        pos_ += ratio_;
    }

    // 更新跨块边界状态
    if (in_count > 0) {
        prev2_sample_ = prev_sample_;
        prev_sample_ = in[in_count - 1];
        has_prev2_ = has_prev_;
        has_prev_ = true;
    }
    fed_ = last + 1;
}

void StreamingResampler::Flush(std::vector<int16_t>& out) {
    if (!configured_ || fed_ == 0 || !has_prev_) {
        return;
    }

    // Process 停止时 floor(pos_) == fed_-1（缺 v1）。Flush 用"末采样"当作
    // v1 补齐，只影响最后不到一个输出周期，保证输出时长与输入一致。
    while (true) {
        const int64_t i0 = (int64_t)std::floor(pos_);
        if (i0 > (int64_t)fed_ - 1) {
            break;
        }
        const double frac = pos_ - (double)i0;
        const int32_t v0 = (i0 == (int64_t)fed_ - 1 || !has_prev2_)
                               ? prev_sample_
                               : prev2_sample_;
        const int32_t v1 = prev_sample_;
        out.push_back((int16_t)Clamp16(v0 + (int32_t)((int64_t)(v1 - v0) * frac)));
        ++produced_;
        pos_ += ratio_;
    }
}

}  // namespace voice