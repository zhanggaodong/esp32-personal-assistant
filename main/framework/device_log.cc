#include "device_log.h"

#include <stdio.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace DeviceLog {
namespace {

constexpr size_t kCapacity = 200;  // 环形容量：最多保留最近 200 条
std::mutex s_mutex;
Entry s_entries[kCapacity];
size_t s_count = 0;         // 已写入条数（含被覆盖的，用于单调递增 seq）
uint32_t s_cleared_mark = 0;  // 最近一次 Clear 时的 tick，供前端判断"已清空"

std::string Format(const char* fmt, va_list args) {
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, args);
    buf[sizeof(buf) - 1] = '\0';
    return std::string(buf);
}

}  // namespace

void Log(char level, const char* tag, const char* fmt, ...) {
    if (fmt == nullptr) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    std::string msg = Format(fmt, args);
    va_end(args);

    Entry e;
    e.ts_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    e.level = level;
    if (tag != nullptr) {
        snprintf(e.tag, sizeof(e.tag), "%s", tag);
    }
    e.msg = std::move(msg);

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_entries[s_count % kCapacity] = std::move(e);
        ++s_count;
    }
}

std::vector<Entry> Snapshot() {
    std::vector<Entry> out;
    std::lock_guard<std::mutex> lock(s_mutex);
    size_t start = s_count > kCapacity ? s_count - kCapacity : 0;
    size_t n = s_count > kCapacity ? kCapacity : s_count;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(s_entries[(start + i) % kCapacity]);
    }
    return out;
}

void Clear() {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_count = 0;
    s_cleared_mark = static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

}  // namespace DeviceLog