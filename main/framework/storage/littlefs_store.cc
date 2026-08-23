#include "littlefs_store.h"

#include <cstdio>
#include <string.h>

#include <esp_log.h>
#include <esp_littlefs.h>

#define TAG "LittleFs"

namespace LittleFsStore {

static const char* kPartitionLabel = "littlefs";

bool Mount() {
    // 已挂载则直接返回
    if (esp_littlefs_mounted(kPartitionLabel)) {
        return true;
    }

    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/littlefs",
        .partition_label = kPartitionLabel,
        .format_if_mount_failed = true,   // 首次使用自动格式化
        .dont_mount = false,
        .grow_on_mount = false,
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register littlefs: %s", esp_err_to_name(err));
        return false;
    }

    size_t total = 0, used = 0;
    esp_littlefs_info(kPartitionLabel, &total, &used);
    ESP_LOGI(TAG, "littlefs mounted: total=%u used=%u bytes", (unsigned)total, (unsigned)used);
    return true;
}

bool Exists(const char* name) {
    if (!Mount() || name == nullptr) {
        return false;
    }
    char path[128];
    snprintf(path, sizeof(path), "/littlefs/%s", name);
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        return false;
    }
    fclose(f);
    return true;
}

bool WriteFile(const char* name, const uint8_t* data, size_t len) {
    if (!Mount() || name == nullptr) {
        return false;
    }
    char path[128];
    snprintf(path, sizeof(path), "/littlefs/%s", name);
    FILE* f = fopen(path, "wb");
    if (f == nullptr) {
        ESP_LOGE(TAG, "Cannot open %s for write", path);
        return false;
    }
    bool ok = (len == 0) || (fwrite(data, 1, len, f) == len);
    if (fclose(f) != 0) {
        ok = false;
    }
    return ok;
}

bool ReadFile(const char* name, std::vector<uint8_t>& out) {
    out.clear();
    if (!Mount() || name == nullptr) {
        return false;
    }
    char path[128];
    snprintf(path, sizeof(path), "/littlefs/%s", name);
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) {
        fclose(f);
        return false;
    }
    out.resize(static_cast<size_t>(sz));
    if (sz > 0 && fread(out.data(), 1, static_cast<size_t>(sz), f) != static_cast<size_t>(sz)) {
        fclose(f);
        out.clear();
        return false;
    }
    fclose(f);
    return true;
}

bool ReadText(const char* name, std::string& out) {
    std::vector<uint8_t> data;
    if (!ReadFile(name, data)) {
        return false;
    }
    out.assign(reinterpret_cast<const char*>(data.data()), data.size());
    return true;
}

bool DeleteFile(const char* name) {
    if (!Mount() || name == nullptr) {
        return false;
    }
    char path[128];
    snprintf(path, sizeof(path), "/littlefs/%s", name);
    return remove(path) == 0;
}

}  // namespace LittleFsStore