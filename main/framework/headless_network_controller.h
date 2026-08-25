#pragma once

#include <string>

#include "board.h"

// 无屏网络控制器：把 WifiBoard 的统一网络事件（配网进入/退出、连接成功/
// 断开/超时）转成无屏 LED 状态与本地语音提示，不依赖 Display/Application。
// 设备配置 WebServer 的启动时序在 headless_main 中处理（WebServer 自带端口
// 占用后台重试，配网结束释放 80 端口后自动起来）。
class HeadlessNetworkController {
public:
    static HeadlessNetworkController& Instance();

    void OnNetworkEvent(NetworkEvent event, const std::string& data);

    // 是否已联网且不在配网（允许按键对话）
    bool IsConnected() const { return connected_; }
    bool IsProvisioning() const { return provisioning_; }

private:
    HeadlessNetworkController() = default;

    bool connected_ = false;
    bool provisioning_ = false;
    bool success_prompted_ = false;  // 每次开机只播一次"配网成功"
};