#include "config_store.h"

#include <stdlib.h>
#include <string.h>

#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>

#include "event_bus.h"

#define TAG "ConfigStore"

#define CONFIG_NVS_NAMESPACE "appcfg"
#define CONFIG_NVS_KEY "config"
#define CONFIG_BLOB_VERSION "V1"
#define CONFIG_MAX_BLOB 4096

// 序列化格式：首行 "V1"，随后 "key=value" 每行一条
static std::string Serialize(const std::map<std::string, std::string>& values) {
    std::string out = CONFIG_BLOB_VERSION "\n";
    for (const auto& kv : values) {
        out += kv.first;
        out += '=';
        out += kv.second;
        out += '\n';
    }
    return out;
}

static bool Deserialize(const std::string& blob, std::map<std::string, std::string>& out) {
    out.clear();
    size_t pos = 0;
    int line = 0;
    while (pos < blob.size()) {
        size_t nl = blob.find('\n', pos);
        if (nl == std::string::npos) {
            nl = blob.size();
        }
        std::string ln = blob.substr(pos, nl - pos);
        pos = nl + 1;
        if (line == 0) {
            if (ln != CONFIG_BLOB_VERSION) {
                return false;  // 版本不匹配，整体重建（迁移兜底）
            }
        } else {
            if (!ln.empty()) {
                size_t eq = ln.find('=');
                if (eq != std::string::npos) {
                    out[ln.substr(0, eq)] = ln.substr(eq + 1);
                }
            }
        }
        ++line;
    }
    return true;
}

ConfigStore& ConfigStore::Instance() {
    static ConfigStore instance;
    return instance;
}

bool ConfigStore::LoadFromStorage() {
    nvs_handle_t handle;
    if (nvs_open(CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS namespace");
        return false;
    }
    size_t len = 0;
    esp_err_t err = nvs_get_blob(handle, CONFIG_NVS_KEY, nullptr, &len);
    bool ok = false;
    if (err == ESP_OK && len > 0 && len <= CONFIG_MAX_BLOB) {
        char* buf = new char[len + 1];
        err = nvs_get_blob(handle, CONFIG_NVS_KEY, buf, &len);
        if (err == ESP_OK) {
            buf[len] = '\0';
            std::string blob(buf, len);
            delete[] buf;
            ok = Deserialize(blob, values_);
        } else {
            delete[] buf;
        }
    }
    nvs_close(handle);
    return ok;
}

bool ConfigStore::SaveToStorage() {
    std::string blob = Serialize(values_);
    if (blob.size() > CONFIG_MAX_BLOB) {
        ESP_LOGE(TAG, "Config blob too large: %u", (unsigned)blob.size());
        return false;
    }
    nvs_handle_t handle;
    if (nvs_open(CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_blob(handle, CONFIG_NVS_KEY, blob.data(), blob.size());
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_OK;
}

const ConfigItem* ConfigStore::FindItem(const char* key) const {
    return ConfigSchemaFind(key);
}

std::string ConfigStore::GetDefault(const char* key) const {
    const ConfigItem* item = FindItem(key);
    if (item != nullptr) {
        return item->default_value != nullptr ? item->default_value : "";
    }
    return "";
}

void ConfigStore::Init() {
    bool loaded = LoadFromStorage();
    bool changed = false;

    if (!loaded) {
        values_.clear();
        for (int i = 0; i < kConfigSchemaSize; ++i) {
            const char* def = kConfigSchema[i].default_value;
            values_[kConfigSchema[i].key] = def != nullptr ? def : "";
        }
        changed = true;
    } else {
        // 补全新增的 schema 项默认值
        for (int i = 0; i < kConfigSchemaSize; ++i) {
            if (values_.find(kConfigSchema[i].key) == values_.end()) {
                const char* def = kConfigSchema[i].default_value;
                values_[kConfigSchema[i].key] = def != nullptr ? def : "";
                changed = true;
            }
        }
    }

    if (changed) {
        SaveToStorage();
    }
    ESP_LOGI(TAG, "ConfigStore initialized with %u items", (unsigned)values_.size());
}

std::string ConfigStore::Get(const char* key) {
    auto it = values_.find(key);
    if (it != values_.end()) {
        return it->second;
    }
    return GetDefault(key);
}

bool ConfigStore::Set(const char* key, const char* value) {
    std::string v = value != nullptr ? value : "";
    if (FindItem(key) == nullptr) {
        ESP_LOGW(TAG, "Unknown config key: %s", key);
        return false;
    }
    bool existed = values_.find(key) != values_.end();
    bool unchanged = existed && values_[key] == v;
    values_[key] = v;
    if (!unchanged) {
        SaveToStorage();
    }
    // 通过事件总线通知关心该配置的模块即时重绘
    EventBus::Instance().Post(kEventConfigChanged, const_cast<char*>(key));
    return true;
}

int ConfigStore::GetInt(const char* key, int fallback) {
    std::string v = Get(key);
    if (v.empty()) {
        return fallback;
    }
    char* end = nullptr;
    long val = strtol(v.c_str(), &end, 10);
    if (end == v.c_str()) {
        return fallback;
    }
    return static_cast<int>(val);
}

bool ConfigStore::GetBool(const char* key, bool fallback) {
    std::string v = Get(key);
    if (v == "1" || v == "yes" || v == "true") {
        return true;
    }
    if (v == "0" || v == "no" || v == "false") {
        return false;
    }
    return fallback;
}