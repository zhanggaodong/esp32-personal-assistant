#include "voice_diagnostics.h"

#include <cstring>

#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>

#define TAG "VoiceDiag"

namespace voice_diag {

namespace {

// RTC 保存的上次运行快照（掉电丢数据属正常，用 magic + checksum 判定有效）。
constexpr uint32_t kRtcMagic = 0x56444731;  // "VDG1"
constexpr uint32_t kRtcVersion = 1;

RTC_NOINIT_ATTR RtcLastRun s_last_run;

int64_t DefaultClock() {
    return esp_timer_get_time() / 1000;  // 单调毫秒，自开机起（不回绕）
}

ClockFn s_clock = DefaultClock;

uint32_t ChecksumBlock(const void* data, size_t len) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    uint32_t sum = 0;
    for (size_t i = 0; i < len; ++i) {
        sum = (sum << 1) ^ bytes[i] ^ (sum >> 31);
    }
    return sum;
}

const char* ResetReasonChinese(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_UNKNOWN:
            return "未知";
        case ESP_RST_POWERON:
            return "上电复位";
        case ESP_RST_EXT:
            return "外部复位引脚";
        case ESP_RST_SW:
            return "软件复位(esp_restart)";
        case ESP_RST_PANIC:
            return "异常/断言(PANIC)";
        case ESP_RST_INT_WDT:
            return "中断看门狗";
        case ESP_RST_TASK_WDT:
            return "任务看门狗";
        case ESP_RST_WDT:
            return "其它看门狗";
        case ESP_RST_DEEPSLEEP:
            return "深度睡眠唤醒";
        case ESP_RST_BROWNOUT:
            return "掉电/欠压(BROWNOUT)";
        case ESP_RST_SDIO:
            return "SDIO 复位";
        default:
            break;
    }
    // IDF 各版本对部分复位原因的枚举命名不一致（如 USB/UART 手动复位在
    // 新旧版本分别叫 ESP_RST_USB / ESP_RST_USB_UART），但数值位置稳定：
    // 11=USB/UART 手动、12=JTAG、13=eFuse 重烧。按数值映射保证可移植。
    switch (static_cast<int>(reason)) {
        case 11:
            return "USB/UART 手动复位(烧录/按复位键)";
        case 12:
            return "JTAG 复位";
        case 13:
            return "eFuse 重烧复位";
        default:
            return "<未知枚举>";
    }
}

}  // namespace

const char* StageName(Stage stage) {
    switch (stage) {
        case Stage::kRecording:
            return "Recording";
        case Stage::kAsr:
            return "ASR";
        case Stage::kChat:
            return "Chat";
        case Stage::kTts:
            return "TTS";
        case Stage::kPlayback:
            return "Playback";
        case Stage::kCancel:
            return "Cancel";
        case Stage::kNone:
            return "None";
    }
    return "?";
}

void SetClock(ClockFn fn) {
    s_clock = fn != nullptr ? fn : DefaultClock;
}

void Timeline::Reset(int64_t offset_ms) {
    count_ = 0;
    dropped_ = 0;
    origin_ms_ = offset_ms != 0 ? offset_ms : (s_clock ? s_clock() : 0);
}

void Timeline::Mark(const char* name) {
    if (name == nullptr) {
        return;
    }
    if (count_ >= kTimelineMaxPoints) {
        ++dropped_;
        return;
    }
    points_[count_++] = {name, s_clock ? s_clock() : 0};
}

int64_t Timeline::ElapsedSince(const char* name) const {
    const TimelinePoint* point = Find(name);
    if (point == nullptr) {
        return 0;
    }
    return point->ts_ms - origin_ms_;
}

void Timeline::Log(const char* tag) const {
    for (size_t i = 0; i < count_; ++i) {
        // 固件启用了 CONFIG_NEWLIB_NANO_FORMAT：printf 不支持 %lld/%zu。
        // %lld 只消费 32 位参数，会让后面的 %s 读到 long long 的高半字
        // （恰为 0）→ strlen(NULL) → LoadProhibited 崩溃（每轮播完即重启
        // 的根因）。时间偏移远小于 2^31ms，用 %d 安全。
        ESP_LOGI(tag, "  [%u] %s +%dms", (unsigned)i, points_[i].name,
                 (int)(points_[i].ts_ms - origin_ms_));
    }
    if (dropped_ > 0) {
        ESP_LOGW(tag, "timeline dropped %u points (cap %u)", (unsigned)dropped_,
                 (unsigned)kTimelineMaxPoints);
    }
}

const TimelinePoint* Timeline::Find(const char* name) const {
    if (name == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < count_; ++i) {
        if (strcmp(points_[i].name, name) == 0) {
            return &points_[i];
        }
    }
    return nullptr;
}

ResourceSnapshot CaptureResources(TaskHandle_t task) {
    ResourceSnapshot snap;
    snap.internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    snap.internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    snap.spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    snap.spiram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    TaskHandle_t handle = task != nullptr ? task : xTaskGetCurrentTaskHandle();
    snap.task_stack_watermark =
        handle != nullptr ? uxTaskGetStackHighWaterMark(handle) : 0;
    return snap;
}

void LogResources(const char* tag, const char* phase,
                  const ResourceSnapshot& snap) {
    ESP_LOGI(tag,
             "%s: int_free=%d int_largest=%d spiram_free=%d spiram_largest=%d "
             "stack_wm=%u",
             phase != nullptr ? phase : "-", snap.internal_free,
             snap.internal_largest, snap.spiram_free, snap.spiram_largest,
             (unsigned)snap.task_stack_watermark);
}

void SaveRtcSnapshot(Stage stage, uint32_t turn_id,
                     const ResourceSnapshot& resources) {
    s_last_run.magic = kRtcMagic;
    s_last_run.version = kRtcVersion;
    s_last_run.rtc_boot_count++;
    s_last_run.last_stage = stage;
    s_last_run.last_turn_id = turn_id;
    s_last_run.resources = resources;
    s_last_run.checksum = ChecksumBlock(&s_last_run, sizeof(s_last_run) - sizeof(uint32_t));
}

void LogLastRunOnBoot() {
    const esp_reset_reason_t reason = esp_reset_reason();
    ESP_LOGI(TAG, "reset reason: %d (%s)", (int)reason, ResetReasonChinese(reason));

    const bool valid =
        s_last_run.magic == kRtcMagic && s_last_run.version == kRtcVersion &&
        s_last_run.checksum ==
            ChecksumBlock(&s_last_run, sizeof(s_last_run) - sizeof(uint32_t));
    if (!valid) {
        ESP_LOGI(TAG, "last run snapshot: invalid/absent (first boot or power loss)");
        return;
    }

    const ResourceSnapshot& r = s_last_run.resources;
    ESP_LOGI(TAG,
             "last run snapshot: stage=%s turn=%u boot_count=%u "
             "int_free=%d int_largest=%d spiram_free=%d spiram_largest=%d "
             "stack_wm=%u",
             StageName(s_last_run.last_stage), (unsigned)s_last_run.last_turn_id,
             (unsigned)s_last_run.rtc_boot_count, r.internal_free,
             r.internal_largest, r.spiram_free, r.spiram_largest,
             (unsigned)r.task_stack_watermark);
}

}  // namespace voice_diag