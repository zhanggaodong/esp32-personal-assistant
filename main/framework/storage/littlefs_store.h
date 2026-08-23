#pragma once

#include <string>
#include <vector>

// LittleFS 存储：挂载独立 littlefs 分区，持久化壁纸等图片资源。
// 供 ScreenHome 读取壁纸、Web /api/upload 写入、/api/image 读取。
namespace LittleFsStore {

// 挂载 littlefs 分区（幂等）。首次使用前自动格式化不存在/损坏的卷。
bool Mount();

// 检查 is 文件是否存在
bool Exists(const char* name);

// 写入二进制文件（覆盖）
bool WriteFile(const char* name, const uint8_t* data, size_t len);

// 读取整个文件到 out，返回是否成功
bool ReadFile(const char* name, std::vector<uint8_t>& out);

// 读取整个文件为字符串（图片预览等），返回是否成功
bool ReadText(const char* name, std::string& out);

// 删除文件
bool DeleteFile(const char* name);

}  // namespace LittleFsStore