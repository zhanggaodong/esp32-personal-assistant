# 个人桌面助手 · 可扩展功能框架 实施方案

> **For Claude:** REQUIRED SUB-SKILL: 执行时使用 superpowers:executing-plans 按任务逐项实施本方案。

**Goal:** 在 ESP32-S3（九川 S3，240×320 竖屏）上，以"可扩展功能框架"替换小智 AI 应用；AI 对话只是其中一个可选模块，框架支持菜单切换界面，并能通过网页在线上传/修改壁纸、息屏显示等参数（字体/位置/颜色/内容）后即时生效。

**Architecture:** 保留小智的硬件驱动层（屏幕/音频/WiFi/按键/背光）并复用之；新建一层"应用框架"：`App 模块注册表 + UI 管理器(菜单切换) + 事件总线 + Schema 驱动的配置中心 + Web 配置服务器`。每个功能/界面就是一个独立"屏幕模块"，只依赖框架接口，互不耦合。AI 对话作为其中一个屏幕模块，通过 HTTP+SSE 调用用户自己的 NestJS 后端（STT/chat/TTS 及内置记忆、记账、日程工具）。

**Tech Stack:** ESP-IDF（esp32s3 目标）、LVGL 显示、esp_http_server(Web)、NVS + LittleFS(存储)、SSE 流解析、用户后端 NestJS(仅 AI 模块需对接)。构建链：`idf.py`。

**设计原则：** DRY、YAGNI、模块化（新增功能 = 新增一个屏幕模块 + 可选一个网络服务，不动核心）、配置 Schema 驱动（网页表单自动生成）、频繁提交、每阶段可独立验证。

---

## 0. 前置：备份与小智回退点

**目标：** 改造前建立可回退到原始小智固件的安全点，避免无法刷回。

**Files:**
- 仓库根 `.git`（若已是 git 仓库）
- 说明：原仓库仍保留小智全部代码，改造不删原文件；通过分支/提交隔离。

**Git 操作（PowerShell，仓库根目录）：**
```powershell
git status                          # 确认工作区干净
git branch -av                      # 查看现有分支
git switch -c origin-xiaozhi         # 创建"回退点"分支：保留原始小智固件
git switch -c feature/personal-assist  # 在独立分支上做改造
```
预期：`feature/personal-assist` 与原 `origin-xiaozhi` 完全解耦；任何时候 `git switch origin-xiaozhi && idf.py flash` 即可刷回小智。

**旁路备份（可选但推荐）：** 记下本机 `docs/` 外、不被 sdkconfig 构建机制误删的位置；小智源码在 GitHub 亦可持续下载。

---

## 1. 目标架构（分层）

```
┌────────────────────────────────────────────────────────┐
│  网页(浏览器) 访问设备IP → Web配置页(表单/图片上传)       │
└───────────────┬──────────────────────▲────────────────┘
                │ GET/PUT /api/config   │ 事件推送
                │ POST /api/upload      │
┌───────────────▼──────────────────────┴────────────────┐
│  屏幕模块层(Screens)  —— 每个模块一个独立屏             │
│   • ScreenHome(壁纸)  • ScreenScreensaver(息屏)        │
│   • ScreenSettings   • ScreenAiChat(对话)  • ScreenWeather │
└───────────────┬──────────────────────▲────────────────┘
│               │ render()/事件回调    │ App 切换
┌───────────────▼──────────────────────┴────────────────┐
│  框架核心(Framework)                                    │
│   AppRegistry 模块注册表 → AppManager UI切换/菜单         │
│   ConfigStore 配置中心(Schema驱动, NVS+LittleFS)          │
│   EventBus 事件总线 + InputRouter 输入路由(可配置按键)      │
│   WebServer + ConfigApi 配置服务(HTTP)                  │
└───────────────┬──────────────────────┴────────────────┘
│               │
┌───────────────▼───────────────────────────────────────┐
│  平台层(复用自小智): LVGL + 屏驱/音频/背光/WiFi/按键/旋钮      │
└────────────────────────────────────────────────────────┘
```

说明：AI 对话模块 `ScreenAiChat` 位于屏幕层，内部用 `services/ai_client` 调你后端；与框架完全解耦，不上 AI 时其他界面照常工作。

---

## 2. 核心抽象接口（写死的事实，供各任务实现）

**2.1 App 模块（一个可切换的界面/功能）**

`main/framework/app/base_app.h`：
```cpp
// 生命周期：注册→onStart→onShow→(事件回调)→onHide→onStop
struct AppMetadata {
  const char* id;      // 唯一id："home" / "screensaver" / "settings" / "ai_chat"
  const char* name;    // 菜单显示名
  bool isMain;         // 是否开机默认屏
};
class BaseApp {
 public:
  virtual ~BaseApp() = default;
  virtual const AppMetadata& metadata() = 0;
  virtual void onStart();        // 仅首次进入时分配资源
  virtual void onShow();         // 每次切到前台，重建可见 UI
  virtual void onHide();         // 每次切走，隐藏/释放临时资源
  virtual void onStop();         // 框架退出时释放
  virtual void onConfigChanged(const char* key); // 该模块关心的配置变化
  virtual void onAppEvent(int32_t event_id, void* data); // 跨模块消息(EventBus)
};
```

**2.2 注册方式（新功能只需一行注册）**

`main/framework/app/app_registry.h`：
```cpp
class AppRegistry {
 public:
  void Register(BaseApp* app);          // 由各模块自注册
  BaseApp* Get(const char* id);
  void ForEach(std::function<void(BaseApp*)> fn);
};
// 全局单例，各模块在 init 阶段调用：
// AppRegistry::Instance().Register(&myApp);
```

**2.3 配置 Schema（驱动网页表单与存储）**

`main/framework/config/config_schema.h`：
```cpp
enum class ConfigType { kString, kInt, kNumber, kColor, kImage, kEnum, kBool };
struct ConfigItem {
  const char* key;        // 如 "screensaver.font_color"
  const char* label;      // 中文标题："息屏字体颜色"
  ConfigType type;
  const char* default_value;
  const char* options;    // kEnum 时 "A|B|C"
  const char* group;      // 分组："壁纸" / "息屏" / "通用"
};
// 全局 Schema 表：网页据此生成表单，ConfigStore 据此读写
extern const ConfigItem kConfigSchema[];
```

**2.4 配置存储与推送**

- 文本/数值/颜色等标量 → **NVS**（快速、键值）
- 图片（壁纸/头像）→ **LittleFS 分区**（按文件存，`/wallpaper.bin` 等）
- 改动后：`ConfigStore::Set()` → 持久化 → `EventBus` 发 `kEventConfigChanged` → 相关 `BaseApp::onConfigChanged` 重绘

**2.5 Web 配置 API（HTTP，esp_http_server）**

| 方法+路径 | 用途 |
|---|---|
| `GET /` | 返回内嵌配置页(HTML/JS/CSS) |
| `GET /api/config` | 读全部配置(含 Schema) |
| `PUT /api/config` | 更新一组配置(JSON) |
| `POST /api/upload` | 上传图片(壁纸等) → LittleFS |
| `GET /api/image/{name}` | 读取图片供 LVGL 显示/预览 |
| `POST /api/reboot` | 应用需重启的配置后重启 |
| `GET /api/status` | 设备状态(版本/网络/当前屏) |

---

## 3. 阶段与任务

> 每阶段结束可独立编译烧录验证，频繁提交。

### Phase A：框架骨架（无 UI 即可编译通过，随后空主菜单可切换）

**A-1 建目录与骨架文件**
**Files:** Create `main/framework/app/base_app.h`、`main/framework/app/app_registry.{cc,h}`、`main/framework/event_bus.{cc,h}`、`main/CMakeLists.txt`(加入 framework 源码)
步骤：定义 `BaseApp`/`AppRegistry`/`EventBus`(空实现)，注册进构建，`idf.py build` 通过。
验证：`idf.py build` → 无编译错误。

**A-2 主应用改造：从"小智语音"改为"框架启动 + 默认屏"**
**Files:** Modify `main/application.cc`（把语音工作流入口替换为 AppManager 初始化 + 显示 ScreenHome）
步骤：引入 `AppManager::Start()`；暂时关闭小智的 IoT/协议初始化。
验证：烧录后屏幕显示默认屏，不连小智云。

**A-3 AppManager：菜单与切换（含旋钮/按键选择）**
**Files:** Create `main/framework/app/app_manager.{cc,h}`、`main/framework/input/input_router.{cc,h}`
步骤：实现"注册表→列出菜单→旋钮/上下键选择→确认键切换→onHide/onShow"；复用板子按键/旋钮驱动。
验证：硬件上能切换两个临时空屏。

**A-4 输入路由(InputRouter)：可配置按键映射（长按/双击/翻页）**
**Files:** Create `main/framework/input/input_router.{cc,h}`、`main/framework/input/key_action.{cc,h}`、`main/framework/input/key_map_config.h`
背景：板子已有 电源键/音量+键/音量−键，驱动(`iot_button`)原生支持 单击/双击/长按/连击/多连击 事件。现把按键动作从"写死在板子驱动"抽成"可配置映射"，并接入框架。
实现：
- 定义动作枚举：全局动作(`kKeyPower`/`kKeyHome`/`kKeyMenu`/`kWake`) 与 界面动作(`kNavUp`/`kNavDown`/`kConfirm`/`kBack`)。
- 映射表：`(按键 × 事件类型)→动作`，存 ConfigStore；分"全局表"与"每界面表"（上下文感知：同一键在不同界面可不同）。
- `InputRouter` 订阅 `iot_button` 事件 → 查当前激活界面动作/全局动作 → 转发 `AppManager` 或当前 `BaseApp`。
- 事件类型支持：单击/双击/三连击/长按/按下/松开，阈值可从配置读。
配置项(Schema 分组"按键")：每键×每事件→动作下拉、长按/连击时间阈值。
验证：网页配置"短按音量+ = 上翻、音量− = 下翻、单击电源 = 确认、长按电源 = 回主页"→ 硬件按键实现菜单翻页与返回；不同界面下同一键行为可不同。

> 提交点：Phase A 完成 = 可编译、可切界面、按键动作可在网页自定义。紧接 Phase B 打通网页配置，按键映射即随网页生效。

> 提交点：`docs/plans` 说明 + git commit「feat(framework): app manager & registry skeleton」。

### Phase B：配置中心 + Web 配置页（开始实现在线改设置）

**B-1 Schema 驱动的 ConfigStore(NVS + LittleFS)**
**Files:** Create `main/framework/config/config_schema.{cc,h}`、`main/framework/config/config_store.{cc,h}`
步骤：实现读/写/持久化；图片走 LittleFS；带 `CONFIG_STORE_VERSION` 迁移。
验证：单测/日志确认 Set/Get 一致、重启后保留。

**B-2 Web 服务器 + 配置 API**
**Files:** Create `main/framework/web/web_server.{cc,h}`、`main/framework/web/web_config_api.{cc,h}`
步骤：`esp_http_server` 起服务；实现 `/api/config` GET/PUT、`/api/upload`、`/api/status`。
验证：浏览器访问设备 IP，能 GET 到 Schema+配置。

**B-3 内嵌配置页(HTML/JS/CSS)**
**Files:** Create `main/framework/web/web_ui/index.html`(随固件内嵌, 或放 LittleFS)
步骤：根据 Schema 动态渲染表单(文字/数字/取色器/图片上传/开关)；保存→`PUT /api/config`→设备即时重绘。
验证：网页上改"通用标题文字"，设备屏幕立刻变化。

### Phase C：强自定义展示——壁纸 与 息屏显示

**C-1 ScreenHome(壁纸界面)——可在线换壁纸/改跑马灯文字**
**Files:** Create `main/screens/screen_home.{cc,h}`、`main/screens/screen_home_config.h`
配置项：背景图(上传)、主标题文本、主标题字体颜色/字号/位置(x,y)、副标题、滚动开关/速度。
实现：读取 ConfigStore 画背景位图 + 文字；监听 `kEventConfigChanged` 重绘。
验证：网页上传一张图片 → 屏上背景变化；改文字颜色/位置 → 即时变化。

**C-2 ScreenScreensaver(息屏显示)——壁纸/字体/位置/颜色/内容全自定义**
**Files:** Create `main/screens/screen_screensaver.{cc,h}`、`main/screens/screen_screensaver_config.h`
配置项：息屏背景图、显示内容(时间/日期/天气/自定文本 多选)、时间字体颜色/位置(对齐)、显示动画/常亮时间、进入条件(超时/按键)。
实现：空闲超时切到息屏；内容多为变量可选；参数全取配置。
验证：设置息屏显示"时间+自定一句话"、位置/颜色 → 息屏呈现符合；交互触发唤醒回主屏。

**C-3 接入天气(可选，放在息屏/主页展示)**
**Files:** Create `main/services/weather_client.{cc,h}`、`main/screens/screen_home.h` 扩展
配置项：开关键、城市、免鉴权 API(如 wttr.in) 或用你后端代理。
实现：周期拉取，缓存到 ConfigStore/内存，供主页/息屏显示。
验证：主页显示天气，息屏可加天气项。

> 提交点：Phase C 完成后产物 = 一台"网页可在线自定义壁纸与息屏"的桌面助手，无需刷固件改外观。

### Phase D：AI 对话模块（作为其中一个可切换功能）

**D-1 后端对接 Client(HTTP + SSE + JWT)**
**Files:** Create `main/services/ai_client.{cc,h}`、`main/services/ssse_parser.{cc,h}`、`main/services/simple_jwt.{cc,h}`
配置项(Schema)：后端地址、设备账号、设备密码/token、可选音色等。
实现：
- 登录 `POST /api/auth/login` 拿 `access_token`(存 NVS)
- STT：录音→`POST /api/asr/transcribe`(multipart `audio`)→文字
- Chat：`POST /api/chat/stream`(文字)→SSE 解析流式回复(内置记忆/记账/日程)
- TTS：`POST /api/tts/synthesize/stream`(回复文字)→SSE 音频分片→播放
- token 过期自动刷新

**D-2 ScreenAiChat(AI 对话界面)**
**Files:** Create `main/screens/screen_ai_chat.{cc,h}`
步骤：菜单里作为一项；UI = 对话气泡(你说/AI答) + 录音按钮/唤醒按键 + 回复文字滚动 + 播报状态；复用音频驱动(录麦克风、播音频)。
验证：按键→说话→屏显你的话→AI 文字回复→TTS 朗读。

**D-3(可选) 实时优化**
**Files:** Modify `backend` 增加 WebSocket 双向流 + `main/services/ai_client` 支持
说明：如要做接近实时，用 WebSocket 管道把 ASR/Chat/TTS 流水线并行，保留 RAG/记忆完整可用。属增强，默认不做。

**D-4(可选增强) 设备端 MCP 工具——让 AI 反向驱动硬件**
**目的：** 你的后端 AI（大脑）在对话中，通过从 xiaozhi 保留的 MCP 工具机制，反向调用设备端（手/嘴）的硬件动作。属增强，放在核心跑通之后。

**AI 能驱动的硬件动作（能力清单）**
- 屏幕：切界面/菜单、显示指定画面、亮度/息屏
- 扬声器：主动语音提醒、提示音、播放音频
- 背光/LED：唤醒/息屏、处理中亮灯等指示
- WiFi：联网取数并回显；按键/旋钮：变更默认动作
- 本地能力：闹钟/倒计时到点触发（响铃 + 屏显）

**Files:**
- Reuse: `main/mcp_server.{cc,h}`（工具注册/派发），`main/boards/common/press_to_talk_mcp_tool.{cc,h}` 等既有设备工具，其余可按需移植
- Create: `main/framework/app/mcp_bridge.{cc,h}`（承载 MCP 传输，替换小智云 WebSocket/MQTT 通道）
- Modify: `ai_client`(Phase D-1) 增加"从 AI 对话中解析出工具调用并转发给 mcp_bridge"
- Register(示例)：`AddTool("self.screen.switch", "切换当前界面", PropertyList({Property("app_id",kPropertyTypeString)}), ...)`

**实现步骤：**
1. 复用 `McpServer` 的 `AddTool`/`tools/list`/`tools/call` 框架，仅替换其传输通道为自有 TCP/WS（不与小智云通信）。
2. 注册一组设备工具：`self.screen.switch`(切屏)、`self.backlight.set`(亮度/息屏)、`self.lamp.set`(LED)、`self.speaker.say`(主动语音提醒)、`self.timer.set`(倒计时/闹钟)。
3. 在你的后端 AI 侧，把设备 MCP 工具作为工具列表之一（走你自己的通信），AI 对话产出工具调用时 → `mcp_bridge` → 设备执行。
4. 结果回传给后端并入对话。

**验证：** 对设备说"把屏切到天气页 / 明天 8 点提醒我并响一声"→ AI 经 MCP 驱动设备切屏 / 设倒计时到点响铃亮灯。

> 提交点：AI 模块完成 = 可切换的多功能框架下，AI 对话可用且不阻断其他界面。（D-4 为独立增强，另立提交点）

### Phase E：稳定化、通用化与文档

**E-1 菜单体验完善**：图标、返回逻辑、默认屏配置。
**E-2 示例"空模块模板"**：`main/screens/template_screen.{cc,h}` 并写入 README —— 新增功能时复制该模板改 id/注册即可。
**E-3 配置迁移与默认值**：`config_store` 版本迁移、异常兜底。
**E-4 文档**：`README.md`(如何新增一个界面模块、如何接新后端)、`docs/plans` 自查。

---

## 4. 如何"新增一个功能"（框架可扩展性说明）

1. 新建 `main/screens/screen_xxx.{cc,h}`，继承 `BaseApp`，实现 4 个生命周期回调。
2. 需要网络则新建 `main/services/xxx_client.{cc,h}`，在模块内部用。
3. 需要在网页上可配置则向 `kConfigSchema` 加条目（自动出现在网页表单）。
4. 在初始化处调用 `AppRegistry::Register(&screenXxx)`。
5. 想让它出现在主菜单/默认屏，在 AppManager 配置里登记即可。

**核心不动的三件事：** 框架核心、平台驱动、配置中心。新增模块只依赖框架接口，不侵入其他模块。

---

## 5. 风险与注意

- **音频/图片格式**：STT 上传音频与 TTS 返回音频、壁纸图片的编解码需与后端/网页约定一致（易踩坑，最先联调）。
- **JWT 刷新**：设备长期运行需处理 token 过期自动重登。
- **延迟**：AI 为主观可接受量级；如要接近实时再走 Phase D-3 流式。
- **低分屏**：UI 用大字号、少元素，避免细小文字发糊。
- **存储分区**：需在 `sdkconfig`/分区表为 LittleFS 预留空间，注意与现有分区兼容。
- **回退**：任何时刻 `git switch origin-xiaozhi && idf.py flash` 可刷回小智（Phase 0）。

---

## 6. 一个最小验证路径（做完 Phase B）
浏览器开 `http://<设备IP>/` → 改"通用标题文字/颜色" → 保存 → 设备屏幕即时更新。此路径打通即证明"框架+配置+网页+在线更新"端到端可用，后续 Phase C/D 均建立其上。