#pragma once

#include <cstddef>
#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// 无屏语音链路诊断：复位证据 + 分阶段耗时 + 堆/栈水位。
//
// 目标（对应计划 7.1 / 7.2）：
//   - 启动时记录 esp_reset_reason() 与"上次运行阶段"，用于区分是 Chat 未结束、
//     TTS 建连/接收期间、还是播放启动前发生复位；
//   - 每轮同一 turnId 下，用单调时钟记录关键时间点并打印相对耗时；
//   - 每个阶段同时采集内部堆/PSRAM 空闲与最大连续块、任务栈水位；
//   - 上次阶段快照用 RTC_NOINIT_ATTR 保存，避免每轮写 NVS 造成磨损。
//
// 可测试性：时钟通过 SetClock() 注入（默认 esp_timer 毫秒）；StageName、
// 相对耗时计算都是纯函数，便于脱离硬件做单元验证。
namespace voice_diag {

// 轮次阶段（顺序固定；新增阶段只能追加，不得改名/重排，否则旧 RTC 快照
// 的 stage 序号含义会变化）。
enum class Stage : uint8_t {
    kNone = 0,
    kRecording,   // 录音中（按住电源键）
    kAsr,         // ASR 识别（含暖机滚动识别的等待）
    kChat,        // Chat 流式生成
    kTts,         // TTS 请求建立与 PCM 接收
    kPlayback,    // 扬声器开始输出
    kCancel,      // 插话取消/网络中断清理
};

// 阶段名（纯函数，供日志与 RTC 快照输出使用）。
const char* StageName(Stage stage);

// 单调毫秒时钟（可注入，默认 esp_timer）。返回 -1 表示时钟不可用。
using ClockFn = int64_t (*)();
void SetClock(ClockFn fn);

// ---------------------------------------------------------------------------
// 分阶段时间线
// ---------------------------------------------------------------------------

// 关键时间点上限（覆盖计划 7.2 的全部点，含新协议路径未来要用的）。
constexpr size_t kTimelineMaxPoints = 28;

struct TimelinePoint {
    const char* name;   // 指针指向静态字符串，不做拷贝
    int64_t ts_ms;      // 绝对单调时钟毫秒
};

class Timeline {
public:
    // 重置并对齐起点（可选 offset_ms：记录为相对该值的毫秒）。
    void Reset(int64_t offset_ms = 0);

    // 记录一个时间点；已满时忽略并计数，避免越界。
    void Mark(const char* name);

    // 某个点距今的相对毫秒（未记录返回 0）。
    int64_t ElapsedSince(const char* name) const;

    // 打印整条时间线（相对首点；首点之后的点只打印差值）。
    void Log(const char* tag) const;

    size_t size() const { return count_; }

private:
    const TimelinePoint* Find(const char* name) const;

    TimelinePoint points_[kTimelineMaxPoints];
    size_t count_ = 0;
    int64_t origin_ms_ = 0;
    size_t dropped_ = 0;
};

// ---------------------------------------------------------------------------
// 堆 / 栈资源快照
// ---------------------------------------------------------------------------

struct ResourceSnapshot {
    int32_t internal_free;          // 内部堆空闲字节
    int32_t internal_largest;       // 内部堆最大连续块
    int32_t spiram_free;            // PSRAM 空闲字节
    int32_t spiram_largest;         // PSRAM 最大连续块
    uint32_t task_stack_watermark;  // 任务剩余栈（字节，0 = 未知）
};

// 采集当前资源；可选传入要测量的任务句柄（nullptr 时取当前任务）。
ResourceSnapshot CaptureResources(TaskHandle_t task = nullptr);

void LogResources(const char* tag, const char* phase,
                  const ResourceSnapshot& snapshot);

// ---------------------------------------------------------------------------
// RTC 复位快照
// ---------------------------------------------------------------------------

struct RtcLastRun {
    uint32_t magic;         // 固定魔数，判定 RTC 数据是否有效
    uint32_t version;       // 结构版本
    uint32_t rtc_boot_count; // 每次保存递增，用于确认快照是否"上次运行"写入
    Stage last_stage;       // 上次运行最后进入的阶段
    uint32_t last_turn_id;  // 上次运行轮次 id
    ResourceSnapshot resources;
    uint32_t checksum;      // 简单校验和
};

// 保存"当前阶段 + turnId + 资源"到 RTC（掉电后无效，用 magic 判定）。
void SaveRtcSnapshot(Stage stage, uint32_t turn_id,
                     const ResourceSnapshot& resources);

// 打印 esp_reset_reason()（含中文含义）与上一次运行的 RTC 快照。
// 应在 headless_main 最早可用位置调用一次。
void LogLastRunOnBoot();

}  // namespace voice_diag