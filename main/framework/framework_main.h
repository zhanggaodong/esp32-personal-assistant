#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// 个人桌面助手框架入口（CONFIG_APP_MODE_FRAMEWORK=y 时由 app_main 调用，不返回）
void framework_main();

#ifdef __cplusplus
}
#endif