#pragma once

#include <cstdarg>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// 设备端内存日志环：供 Web 配置页 /api/logs 拉取排查（登录/请求/对话日志）。
// 固定容量的环形缓冲，头部覆盖最旧条目；线程安全，任意任务可调用。
namespace DeviceLog {

struct Entry {
    uint32_t ts_ms = 0;      // 开机后的毫秒
    char level = 'I';        // 'I' / 'W' / 'E'
    char tag[12] = {0};
    std::string msg;
};

// 写入一条带 printf 格式的日志（自动取当前 tick 时间戳）
void Log(char level, const char* tag, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

// 取当前全部日志（旧 -> 新）
std::vector<Entry> Snapshot();

// 清空日志
void Clear();

}  // namespace DeviceLog