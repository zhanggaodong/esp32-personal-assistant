#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// 有状态流式线性插值重采样器（默认 24 kHz -> 16 kHz，16 bit 单声道）。
//
// 与"每个 20ms 块独立从零重采样"不同：本类跨块保留相位与边界采样，
// 保证块与块之间没有重复、丢样或相位跳变（对应计划 6.1）。
//
// 用法：
//   voice::StreamingResampler resampler;
//   resampler.Configure(24000, 16000);
//   ... 每收一个 20ms 块：
//   std::vector<int16_t> out;
//   resampler.Process(block.data(), block.size(), out);   // out 立即可用
//   ... 输入结束时：
//   resampler.Flush(out);                                  // 冲刷尾部（用末采样补齐）
namespace voice {

class StreamingResampler {
public:
    StreamingResampler() = default;

    // 配置源/目标采样率；支持非整数比例降采样（如 24k -> 16k）。
    // 失败（非法参数或升采样）返回 false 并保持原状态。
    bool Configure(size_t src_rate, size_t dst_rate);

    // 恢复到初始状态（清空相位与边界采样）。
    void Reset();

    // 送入一块输入 PCM（16 bit signed），把本块可立即产出的输出采样追加到 out。
    // 调用方保证 out 先 clear 或自行 append；本函数不会自动清空 out。
    void Process(const int16_t* in, size_t in_count, std::vector<int16_t>& out);

    // 输入结束后的尾部冲刷：最后一个采样无法再“插值到下一采样”时，
    // 用末采样补齐剩余输出，保证采样数与时长一致。
    void Flush(std::vector<int16_t>& out);

    size_t src_rate() const { return src_rate_; }
    size_t dst_rate() const { return dst_rate_; }
    bool configured() const { return configured_; }

    // 总共产出的输出采样数（自上次 Reset 起，含 Flush）。
    int64_t produced() const { return produced_; }

    // 已消耗的输入采样数（自上次 Reset 起）。
    int64_t fed() const { return fed_; }

private:
    // 读取“绝对输入序号 abs_index”处的采样；若落在上一块结尾，用保存的边界采样。
    int16_t SampleAt(int64_t abs_index) const;

    size_t src_rate_ = 24000;
    size_t dst_rate_ = 16000;
    bool configured_ = false;

    double ratio_ = 1.0;   // 每个输出采样消耗的输入采样数（src / dst）
    double pos_ = 0.0;     // 当前输出位置对应的输入绝对浮点序号（只进不退）
    int64_t fed_ = 0;      // 已送入的输入采样总数（绝对）
    int64_t produced_ = 0; // 已产出的输出采样总数

    // 跨块插值边界：最近两块的最后 1 个采样。
    bool has_prev_ = false;
    int16_t prev_sample_ = 0;
    bool has_prev2_ = false;
    int16_t prev2_sample_ = 0;
};

}  // namespace voice
