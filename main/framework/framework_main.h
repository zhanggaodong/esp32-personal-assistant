#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// 个人桌面助手框架入口（CONFIG_APP_MODE_FRAMEWORK=y 时由 app_main 调用，不返回）
void framework_main();

// 无屏按键语音助手入口（CONFIG_APP_MODE_HEADLESS_VOICE=y 时由 app_main 调用，不返回）
// 不初始化 LCD/LVGL，只启动音频、Wi-Fi、按键、RGB 灯与无屏语音状态机。
void headless_main();

#ifdef __cplusplus
}
#endif