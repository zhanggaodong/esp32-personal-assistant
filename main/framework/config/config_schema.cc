#include "config_schema.h"

#include <string.h>

#ifdef CONFIG_APP_MODE_HEADLESS_VOICE

// 无屏语音助手只保留 AI 服务配置：后端地址、账号、密码、音色、采样率。
// 不再有标题/壁纸/字体/息屏/菜单/按键映射等屏幕相关项；
// ai.enabled 由"地址+账号+密码是否完整"隐式决定。
const ConfigItem kConfigSchema[] = {
    {"ai.backend_url", "后端地址 http(s)://ip:port", ConfigType::kString, "", nullptr, "AI服务"},
    {"ai.account", "后端账号(邮箱/手机号)", ConfigType::kString, "", nullptr, "AI服务"},
    {"ai.password", "后端密码", ConfigType::kPassword, "", nullptr, "AI服务"},
    {"ai.voice", "朗读音色", ConfigType::kEnum, "mimo_default",
     "mimo_default|冰糖|茉莉|苏打|白桦|Mia|Chloe|Milo|Dean", "AI服务"},
    {"ai.sample_rate", "录音采样率", ConfigType::kEnum, "16000", "8000|16000", "AI服务"},
};

#else  // 屏幕版（Framework / 原始小智回退构建）

// 初始 Schema。每新增一个可配置项，就在此加一行：自动出现在网页表单。
const ConfigItem kConfigSchema[] = {
    // 分组：通用
    {"general.title", "主标题文字", ConfigType::kString, "个人桌面助手", nullptr, "通用"},
    {"general.subtitle", "副标题文字", ConfigType::kString, "可扩展桌面助手", nullptr, "通用"},
    {"general.title_color", "标题颜色", ConfigType::kColor, "#FFFFFF", nullptr, "通用"},
    {"general.font_size", "标题字号", ConfigType::kInt, "24", nullptr, "通用"},
    {"general.title_position", "标题位置", ConfigType::kEnum, "center",
     "left|center|right", "通用"},
    {"general.title_scroll", "跑马灯滚动", ConfigType::kBool, "0", nullptr, "通用"},
    {"general.title_scroll_speed", "滚动速度(秒/圈)", ConfigType::kInt, "5", nullptr, "通用"},

    // 分组：壁纸（Phase C-1 ScreenHome 使用）
    {"wallpaper.file", "壁纸图片(上传)", ConfigType::kImage, "", nullptr, "壁纸"},
    {"wallpaper.enabled", "启用壁纸", ConfigType::kBool, "0", nullptr, "壁纸"},

    // 分组：按键（Phase A-4 InputRouter 读取）
    {"keys.vol_up_click", "音量+ 单击", ConfigType::kEnum, "nav_up",
     "none|nav_up|nav_down|confirm|back|home|menu", "按键"},
    {"keys.vol_down_click", "音量- 单击", ConfigType::kEnum, "nav_down",
     "none|nav_up|nav_down|confirm|back|home|menu", "按键"},
    {"keys.power_click", "电源键 单击", ConfigType::kEnum, "menu",
     "none|nav_up|nav_down|confirm|back|home|menu", "按键"},
    {"keys.power_long", "电源键 长按", ConfigType::kEnum, "home",
     "none|nav_up|nav_down|confirm|back|home|menu", "按键"},

    // 分组：息屏（Phase C-2 ScreenScreensaver 使用）
    {"screensaver.enabled", "启用息屏显示", ConfigType::kBool, "1", nullptr, "息屏"},
    {"screensaver.background", "息屏背景图(上传)", ConfigType::kImage, "", nullptr, "息屏"},
    {"screensaver.timeout_sec", "空闲进入息屏(秒)", ConfigType::kInt, "60", nullptr, "息屏"},
    {"screensaver.content", "显示内容", ConfigType::kString, "time,date,custom",
     "time|date|custom", "息屏"},
    {"screensaver.custom_text", "自定文本", ConfigType::kString, "深夜勿扰，休息中", nullptr, "息屏"},
    {"screensaver.font_color", "息屏字体颜色", ConfigType::kColor, "#FFFFFF", nullptr, "息屏"},
    {"screensaver.font_size", "息屏字号", ConfigType::kInt, "40", nullptr, "息屏"},
    {"screensaver.align", "文字对齐", ConfigType::kEnum, "center",
     "left|center|right", "息屏"},
    {"screensaver.clock_24h", "24小时制", ConfigType::kBool, "1", nullptr, "息屏"},
    {"screensaver.keep_on_sec", "息屏常亮时长(秒,0=一直)", ConfigType::kInt, "0", nullptr, "息屏"},

    // 分组：AI 对话（Phase D 使用；网页填入后端地址+账号+密码，设备运行时登录拿 JWT）
    {"ai.enabled", "启用AI对话", ConfigType::kBool, "0", nullptr, "AI对话"},
    {"ai.backend_url", "后端地址 http(s)://ip:port", ConfigType::kString, "", nullptr, "AI对话"},
    {"ai.account", "后端账号(邮箱/手机号)", ConfigType::kString, "", nullptr, "AI对话"},
    {"ai.password", "后端密码", ConfigType::kString, "", nullptr, "AI对话"},
    {"ai.voice", "朗读音色", ConfigType::kEnum, "mimo_default",
     "mimo_default|冰糖|茉莉|苏打|白桦|Mia|Chloe|Milo|Dean", "AI对话"},
    {"ai.sample_rate", "录音采样率", ConfigType::kEnum, "16000", "8000|16000", "AI对话"},
};

#endif  // CONFIG_APP_MODE_HEADLESS_VOICE

const int kConfigSchemaSize = sizeof(kConfigSchema) / sizeof(kConfigSchema[0]);

const ConfigItem* ConfigSchemaFind(const char* key) {
    for (int i = 0; i < kConfigSchemaSize; ++i) {
        if (strcmp(kConfigSchema[i].key, key) == 0) {
            return &kConfigSchema[i];
        }
    }
    return nullptr;
}