#pragma once

#include <map>
#include <string>

// 输入路由：把"按键动作"从板驱动中的写死行为抽成"可配置映射"，并接入框架。
//
// 配置（Schema "按键" 分组）示例：
//   keys.vol_up_click  = nav_up    音量+单击 → 菜单上翻
//   keys.vol_down_click= nav_down  音量-单击 → 菜单下翻
//   keys.power_click   = menu      电源单击 → 显示菜单
//   keys.power_long    = home      电源长按 → 回主页
//
// 说明：jiuchuan 板按键当前在板类内部绑定音量/电源/重置 WiFi 等功能，
// 物理按键的最终绑定需把板层按钮事件桥接到 HandleKeyEvent()（后续接入）。
enum class KeyAction {
    kNone = 0,
    kNavUp,
    kNavDown,
    kConfirm,
    kBack,
    kHome,
    kMenu,
};

class InputRouter {
public:
    static InputRouter& Instance();

    // 从 ConfigStore 读取当前按键映射，并订阅配置变更以便即时重生效
    void Init();

    // 处理某个按键事件：按钮 id（如 "vol_up"/"power"）+ 事件（如 "click"/"long"）
    void HandleKeyEvent(const char* button_id, const char* event);

    // 解析某个配置值字符串为动作
    KeyAction ParseAction(const char* value);

    // 执行动作（转到对应 AppManager 操作）
    void Execute(KeyAction action);

private:
    InputRouter() = default;
    void OnConfigChanged();
    void ReloadMapping();

    std::map<std::string, KeyAction> mapping_;  // "button.event" -> action
};