#pragma once

#include <map>
#include <string>

#include "config_schema.h"

// ConfigStore：Schema 驱动的配置中心。
// - 标量（文本/数值/颜色/枚举/布尔）持久化到 NVS（整块 blob，避免 NVS 15 字符 key 限制）
// - 图片类（壁纸等）经 kImage 配置项存储到 LittleFS（Phase C）
// - Set() 会持久化并通过 EventBus 广播 kEventConfigChanged
class ConfigStore {
public:
    static ConfigStore& Instance();

    // 初始化：确保所有 schema 项都有有效值（缺失则写默认值并持久化）
    void Init();

    std::string Get(const char* key);
    bool Set(const char* key, const char* value);

    std::string GetDefault(const char* key) const;
    const ConfigItem* FindItem(const char* key) const;

    // 类型化读取
    int GetInt(const char* key, int fallback = 0);
    bool GetBool(const char* key, bool fallback = false);

    // Schema 暴露给网页（B-3）
    const ConfigItem* Schema() const { return kConfigSchema; }
    int SchemaSize() const { return kConfigSchemaSize; }

private:
    ConfigStore() = default;
    bool LoadFromStorage();
    bool SaveToStorage();

    std::map<std::string, std::string> values_;
};