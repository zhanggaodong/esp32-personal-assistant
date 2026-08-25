#pragma once

// 配置 Schema：驱动网页表单生成与 ConfigStore 读写。
enum class ConfigType {
    kString,   // 文本
    kInt,      // 整数
    kNumber,   // 浮点
    kColor,    // 颜色，值如 "#FFFFFF"
    kImage,    // 图片（存 LittleFS）
    kEnum,     // 枚举，options 用 "|" 分隔
    kBool,     // 布尔，值 "0"/"1"
    kPassword, // 密码：GET 掩码返回，PUT 收到掩码值表示"不修改原密码"
};

struct ConfigItem {
    const char* key;           // 如 "screensaver.font_color"
    const char* label;         // 中文标题："息屏字体颜色"
    ConfigType type;
    const char* default_value; // 默认值（字符串形式）
    const char* options;       // kEnum 时 "A|B|C"，否则 nullptr
    const char* group;         // 分组："壁纸" / "息屏" / "通用" / "按键"
};

// 全局 Schema 表：网页据此生成表单，ConfigStore 据此读写。
extern const ConfigItem kConfigSchema[];
extern const int kConfigSchemaSize;

// 便捷查找单个项（按 key）
const ConfigItem* ConfigSchemaFind(const char* key);