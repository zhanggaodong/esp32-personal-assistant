#include "config_schema.h"

#include <string.h>

// 初始 Schema。每新增一个可配置项，就在此加一行：自动出现在网页表单。
const ConfigItem kConfigSchema[] = {
    // 分组：通用
    {"general.title", "主标题文字", ConfigType::kString, "个人桌面助手", nullptr, "通用"},
    {"general.subtitle", "副标题文字", ConfigType::kString, "可扩展桌面助手", nullptr, "通用"},
    {"general.title_color", "标题颜色", ConfigType::kColor, "#FFFFFF", nullptr, "通用"},
    {"general.font_size", "标题字号", ConfigType::kInt, "24", nullptr, "通用"},
    {"general.title_position", "标题位置", ConfigType::kEnum, "center",
     "left|center|right", "通用"},

    // 分组：按键（Phase A-4 InputRouter 读取）
    {"keys.vol_up_click", "音量+ 单击", ConfigType::kEnum, "nav_up",
     "none|nav_up|nav_down|confirm|back|home|menu", "按键"},
    {"keys.vol_down_click", "音量- 单击", ConfigType::kEnum, "nav_down",
     "none|nav_up|nav_down|confirm|back|home|menu", "按键"},
    {"keys.power_click", "电源键 单击", ConfigType::kEnum, "menu",
     "none|nav_up|nav_down|confirm|back|home|menu", "按键"},
    {"keys.power_long", "电源键 长按", ConfigType::kEnum, "home",
     "none|nav_up|nav_down|confirm|back|home|menu", "按键"},

    // 分组：息屏（Phase C 使用）
    {"screensaver.enabled", "启用息屏显示", ConfigType::kBool, "1", nullptr, "息屏"},
    {"screensaver.font_color", "息屏字体颜色", ConfigType::kColor, "#FFFFFF", nullptr, "息屏"},
    {"screensaver.timeout_sec", "进入息屏超时(秒)", ConfigType::kInt, "60", nullptr, "息屏"},
};

const int kConfigSchemaSize = sizeof(kConfigSchema) / sizeof(kConfigSchema[0]);

const ConfigItem* ConfigSchemaFind(const char* key) {
    for (int i = 0; i < kConfigSchemaSize; ++i) {
        if (strcmp(kConfigSchema[i].key, key) == 0) {
            return &kConfigSchema[i];
        }
    }
    return nullptr;
}