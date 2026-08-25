# 无屏幕按键语音助手实施方案

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 将九川 ESP32-S3 固件从带屏幕的个人桌面助手收敛为一个无屏幕、按住电源键说话、松开后由服务端完成识别与回答并通过喇叭播报的语音助手。

**Architecture:** 保留九川板的 ES8311 麦克风/喇叭、Wi-Fi、按键、电源和单颗 WS2812 RGB 指示灯；移除 LVGL、屏幕模块、菜单、壁纸和息屏逻辑。新增一个无屏语音状态机负责按键录音、网络请求、TTS 播放和灯光状态，配网使用现有 Wi-Fi 配置机制并增加本地语音提示，设备配置网页只保留服务端地址、账号、密码和少量音频参数。

**Tech Stack:** ESP-IDF 5.5、ESP32-S3、ES8311、`iot_button`、`led_strip`/WS2812、`esp_http_client`、HTTP multipart、SSE、JWT、NVS、GitHub Actions。

---

## 1. 已确认的硬件事实

当前仓库的九川板配置和驱动已经给出以下事实，后续实现必须以此为准：

| 能力 | 当前定义 | 结论 |
|---|---|---|
| 麦克风/喇叭 | ES8311，输入/输出 24 kHz | 可以实现按键录音、TTS 播报 |
| 对话键 | `PWR_BUTTON_GPIO = GPIO_NUM_3`，高电平有效 | 电源键改为按住说话键 |
| 其它按键 | `WIFI_BUTTON_GPIO = GPIO_NUM_6`、`CMD_BUTTON_GPIO = GPIO_NUM_7` | 不参与对话主流程，可保留音量或备用功能 |
| 指示灯 | `BUILTIN_LED_GPIO = GPIO_NUM_10` | 当前 `SingleLed` 按单颗 WS2812 初始化，支持 RGB 颜色 |
| 灯颜色顺序 | `LED_STRIP_COLOR_COMPONENT_FMT_GRB` | 逻辑颜色仍使用 RGB，驱动负责 GRB 排列 |
| 屏幕 | 当前代码初始化 GC9309/LVGL | 无屏版本必须完全跳过显示初始化 |
| Wi-Fi | `WifiBoard::StartNetwork()` + `WifiManager` | 保留连接、热点配网和状态回调，但移除对 `Application` 显示/调度的依赖 |

当前 `SingleLed::OnStateChanged()` 依赖旧的 `Application::GetDeviceState()`，颜色已经包含蓝、红、绿等状态，但 Framework 入口没有完整运行旧 Application 状态机。因此不能只改颜色常量，必须增加无屏状态机的 LED 控制入口。

## 2. 明确的产品行为

### 2.1 开机与待机

1. 开机初始化音频、Wi-Fi、按键和 RGB 灯。
2. 有已保存 Wi-Fi 时尝试连接；没有 Wi-Fi 或连接超时则自动进入热点配网。
3. Wi-Fi 和服务端配置均正常时进入待机：灯绿色常亮，麦克风关闭，不会自动录音，不会主动连接聊天接口。
4. 未联网、未配置账号密码或服务端不可用时，灯红色短闪后回到待机/等待状态；不得因为错误而自动打开麦克风。

### 2.2 电源键按住说话

电源键是唯一的对话触发键，采用 `BUTTON_PRESS_DOWN` 和 `BUTTON_PRESS_UP`，不使用单击切换页面。

| 时序 | 设备动作 | 灯光 |
|---|---|---|
| 按下且未满足联网/登录条件 | 播放“请先配置网络或服务”提示，不打开麦克风 | 红色短闪，随后回绿色 |
| 按下且可对话 | 清除上一轮临时状态，打开麦克风输入 | 橙色常亮或低幅呼吸 |
| 持续按住 | 持续采集 PCM，达到最大录音时长时自动停止 | 橙色呼吸 |
| 松开 | 立即关闭麦克风并提交 WAV 给 ASR | 蓝色慢呼吸，表示处理中 |
| ASR/聊天/TTS 进行中 | 不再采集新一轮录音，避免并发占用 ES8311 | 蓝色呼吸；播报时随播放节奏变化 |
| TTS 完成 | 清理本轮缓存 | 恢复绿色常亮 |
| 本轮失败 | 播放可听见的失败提示，记录串口错误 | 红色闪烁 2 次，恢复绿色 |

“松开后 AI 继续回答”是硬性要求。松开只代表停止录音，不代表取消请求；ASR、聊天 SSE 和 TTS 必须在独立工作任务中继续执行。

在一轮回答尚未结束时再次按电源键，第一版建议忽略本次按键并播放短提示“请等待当前回答结束”，不允许并发录音、并发 HTTP 或打断正在播放的 PCM。后续如需要打断，可另立需求实现。

### 2.3 录音边界与防误触

- 按下事件直接进入录音，不等待“长按”事件；这样符合“什么时候按就什么时候可以说话”。
- 松开事件直接结束录音；不使用 `BUTTON_SINGLE_CLICK` 作为录音触发。
- 最大单轮录音时长建议 8 秒，达到上限自动停止并按“松开”路径提交。
- 录音长度低于最小阈值（建议 200 ms）时不上传，播放“请说话后松开”。
- 录音过程中检测到空数据、I2S 错误或输入设备不存在时，关闭输入、红灯提示并回待机。
- 电源键长按不能触发原有深睡关机；深睡关机逻辑在无屏语音版本中移除或改到明确的 BOOT 键维护流程。

## 3. RGB 指示灯设计

### 3.1 颜色语义

颜色只是无屏反馈，不承载必须依赖视觉才能完成的流程；每个关键异常必须同时有语音或明确的声音提示。

| 状态 | 默认颜色 | 模式 | 说明 |
|---|---|---|---|
| Ready | 绿色 `(0, 16, 0)` | 常亮 | 已联网并允许按键对话 |
| Wi-Fi 连接中 | 蓝色 `(0, 0, 8)` | 慢速呼吸 | 正在连接已保存 Wi-Fi |
| 配网模式 | 蓝色 `(0, 0, 16)` | 500 ms 闪烁 | 等待手机连接设备热点 |
| 录音中 | 橙色 `(16, 8, 0)` | 低幅呼吸 | 电源键仍被按住 |
| 处理中/播报中 | 蓝色 `(0, 0, 16)` | 呼吸或随 PCM 活动 | 松开后等待 ASR/聊天/TTS |
| 未配置/认证失败 | 红色 `(16, 0, 0)` | 2 次短闪 | 不允许录音 |
| 配网成功 | 绿色 `(0, 16, 0)` | 3 次短闪后常亮 | 已取得 IP |

颜色值必须集中在无屏 LED 控制器中，不散落在按键回调、AI 客户端和 Wi-Fi 客户端中。亮度需限制在当前板子安全范围，避免 WS2812 过亮影响功耗或喇叭噪声。

### 3.2 对“呼吸灯”的处理

当前仓库的 `GpioLed` 有 PWM 渐变实现，但九川板返回的是 `SingleLed`，其 WS2812 实现主要是亮灭闪烁。无屏版本应在 RGB 控制器中实现统一的 `SetColor`、`SetBreathing`、`SetBlink`、`Off` 接口；不要再调用依赖 `Application::GetDeviceState()` 的 `OnStateChanged()`。

播报时不要求精确分析音量波形。第一版采用定时渐变即可；如果需要“说一下灯变一下”的效果，再把 TTS PCM 输出块作为活动信号，收到每个非静音块时提升亮度，连续静音时降低亮度。灯光线程不得阻塞音频输出。

## 4. 配网与本地语音提示

### 4.1 配网入口

- 首次开机没有 Wi-Fi 凭据：自动进入热点配网。
- 已保存凭据但连接超时：停止当前连接尝试，进入热点配网。
- 已联网设备的重新配网：使用 BOOT 键的维护事件或网页提供“清除 Wi-Fi 并重启配网”操作。电源键保留给 PTT，不能用电源键长按/三击承担配网，以免和录音冲突。
- 热点配网的设备热点名称、访问地址继续由 `WifiManager` 生成；网页端显示动态 SSID/IP，设备端只播放固定引导语，避免为每一个动态字符串重新生成语音。

### 4.2 必须提供的本地提示音

配网阶段不能依赖用户服务端登录，也不能依赖服务端 TTS，因此提示音必须作为固件本地 OGG/PCM 资源打包：

| 事件 | 推荐提示词 | 触发时机 |
|---|---|---|
| 进入配网 | “进入配网模式，请连接设备热点并打开配置页面” | `ConfigModeEnter` |
| 等待配置 | “正在等待网络配置” | 进入后只播放一次，避免循环打扰 |
| 配网成功 | “网络配置成功” | 收到 Wi-Fi connected 和 IP |
| 配网失败/超时 | “网络连接失败，请重新配置” | 连接超时或认证失败 |
| 服务端配置缺失 | “请先在网页配置服务端账号和密码” | 按键触发对话但配置不完整 |
| 登录失败 | “服务登录失败，请检查账号密码” | `/api/auth/login` 返回错误 |
| 等待当前回答 | “请等待当前回答结束” | TTS/聊天未完成时再次按键 |
| 无有效语音 | “没有听清，请再试一次” | 录音太短或 ASR 为空 |
| 对话网络失败 | “网络异常，请稍后再试” | ASR、Chat 或 TTS 请求失败 |

语音资源建议放在 `main/assets/common/` 或中文语言资源目录，更新 `main/CMakeLists.txt` 的 `COMMON_SOUNDS/LANG_SOUNDS` 收集逻辑。不能使用屏幕文字作为唯一提示，也不能在没有网络时调用服务端 TTS 生成配网提示。

### 4.3 配网服务与设备配置服务的边界

配网热点页面负责 Wi-Fi SSID/密码；设备连上局域网后，设备 Web 配置页负责 AI 服务配置。两者都可能使用 HTTP 80 端口：

1. 配网模式中由 Wi-Fi 配网服务占用端口。
2. Framework Web 配置服务器启动失败时进入后台重试。
3. 配网退出并取得 IP 后，配置服务器必须最终启动。
4. 串口必须记录 `config mode enter/exit`、IP 和 Web server started，便于无屏设备排查。

## 5. AI 服务登录和对话协议

### 5.1 网页配置项

无屏版本只保留以下 AI 配置，不再保留标题、壁纸、字体、息屏、菜单和按键映射项：

| Key | 类型 | 用途 |
|---|---|---|
| `ai.backend_url` | string | NestJS 服务地址，例如 `http://192.168.0.10:3000` |
| `ai.account` | string | 登录账号 |
| `ai.password` | password | 登录密码；GET 配置时必须掩码 |
| `ai.voice` | enum/string | TTS 音色，默认值由后端支持列表决定 |
| `ai.sample_rate` | enum | ASR 采样率，建议 `16000` |

`ai.enabled` 可以删除；当地址、账号和密码三项完整时即视为允许尝试对话，否则按未配置处理。若为了兼容已有 NVS，可保留该 key，但网页默认隐藏并由配置完整性决定最终状态。

### 5.2 正确的登录流程

设备需要登录，但不需要每次对话都提交账号密码：

1. 网页保存地址、账号和密码到 NVS。
2. 首次按键对话前调用 `POST /api/auth/login`，请求体为 `{account,password}`。
3. 从响应读取 `accessToken`，只保存在设备内存；第一版不把 JWT 写入网页，也不在 `/api/config` 返回。
4. ASR、聊天和 TTS 请求全部携带 `Authorization: Bearer <accessToken>`。
5. 任一接口返回 401 时清除 token，重新登录一次并重试当前阶段一次；再次失败则提示用户检查账号密码。
6. 设备重启后 token 丢失，下一次按键对话时重新登录，这是正常行为。
7. 修改地址、账号或密码时立即清除旧 token，下一次对话使用新配置。

当前 `main/framework/ai/ai_client.cc` 已基本实现上述模型，但后续必须补足“401 后当前请求只重试一次”、登录并发锁、密码不回显和请求取消/超时处理。

### 5.3 服务端接口契约

沿用当前客户端约定，开发时必须和 NestJS 后端逐项联调：

| 请求 | Header/Body | 响应 |
|---|---|---|
| `POST /api/auth/login` | JSON `{account,password}` | JSON `{accessToken}` |
| `POST /api/asr/transcribe` | multipart `audio=rec.wav` + Bearer | JSON `{text}` |
| `POST /api/chat/device` | JSON `{content}` + Bearer | SSE 增量文本，`done` 事件带 `conversationId` |
| `POST /api/tts/synthesize/stream` | JSON `{text,voiceMode,voice}` + Bearer | SSE 音频块 `audioBase64`，结束事件 `done` |

客户端必须验证 HTTP 状态、Content-Type、SSE 结束事件和音频数据长度，不能把 200 但内容为空当成成功。

## 6. 无屏幕软件架构

### 6.1 保留模块

- `main/boards/jiuchuan-s3/jiuchuan_dev_board.cc`：音频 codec、GPIO 按键、RGB 灯、电源和 Wi-Fi 板级能力。
- `main/audio/`：ES8311 输入输出和 PCM 播放。
- `main/boards/common/wifi_board.cc`：Wi-Fi 连接和配网底层，但需要移除旧 Application 调用。
- `main/framework/ai/ai_client.{cc,h}`：登录、ASR、Chat SSE、TTS SSE，按本方案补强。
- `main/framework/config/config_store.{cc,h}`：NVS 保存 AI 配置。
- `main/framework/web/web_server.{cc,h}`、`web_config_api.{cc,h}`：局域网配置页和 API。
- `main/framework/event_bus.{cc,h}`：网络、录音、回答、灯光状态事件。

### 6.2 删除或停用模块

- `main/screens/screen_home.*`
- `main/screens/screen_screensaver.*`
- `main/screens/screen_settings.*`
- `main/framework/app/*` 的注册表、菜单和切屏逻辑
- `main/framework/input/input_router.*` 的菜单映射逻辑
- LVGL/LCD 初始化、GC9309 面板、背光和字体/壁纸资源
- `wallpaper.*`、`screensaver.*`、标题字号/颜色/跑马灯等 Schema 项
- 天气、MCP 屏幕控制、截图、显示主题和字体资源

删除应以“构建依赖确认”为前提，不要直接物理删除仍被原始小智模式引用的公共文件。推荐增加独立 `CONFIG_APP_MODE_HEADLESS_VOICE`，只在该模式下裁剪 Framework 源码；原始小智模式仍可作为回退构建。

### 6.3 建议新增模块

| 模块 | 责任 |
|---|---|
| `headless_voice_controller.{cc,h}` | 总状态机、按键事件、录音任务、AI 工作任务编排 |
| `headless_led_controller.{cc,h}` | RGB 颜色、常亮、闪烁、呼吸和播报活动灯效 |
| `audio_prompt_player.{cc,h}` | 播放本地配网/错误提示音，管理与 TTS 的互斥 |
| `headless_network_controller.{cc,h}` | Wi-Fi 事件转语音状态、配网提示和 Web 服务启动时序 |
| `headless_config_schema` | 只输出 AI 地址、账号、密码、音色、采样率 |

状态机建议使用单一工作线程处理一轮对话，按键回调只发送事件，不直接执行 HTTP 或阻塞录音。建议状态如下：`Booting`、`WifiConnecting`、`Provisioning`、`Ready`、`Recording`、`Transcribing`、`Chatting`、`Speaking`、`Error`。

## 7. Wi-Fi 与 Framework 边界改造要求

当前 `WifiBoard::StartWifiConfigMode()` 和 `EnterWifiConfigMode()` 仍调用 `Application::SetDeviceState()`、`Application::Schedule()`、`Application::Alert()`，这在 Framework/无屏入口中是不可靠的。必须改为：

- 通过 `WifiBoard::SetNetworkEventCallback()` 把进入配网、退出配网、连接成功、断开、超时事件交给无屏控制器。
- 无屏控制器负责播放本地提示和更新 LED，不调用 Display。
- 配网 AP 启动、手机提交凭据、Station 获取 IP 都由 `WifiManager` 完成。
- 设备配置 WebServer 不依赖屏幕，也不依赖 `Application::Run()`。
- Wi-Fi 断开时停止新的 PTT 录音；正在播放的 TTS允许自然结束或在明确超时后停止。
- 自动重连成功后回到绿色 Ready；多次失败进入配网并播放失败提示。

## 8. 安全与数据处理

- `/api/config` GET 不得返回 `ai.password` 明文，只返回空值或掩码，例如 `********`。
- PUT 更新时若收到掩码值，表示“不修改原密码”；只有真实新值才写 NVS。
- Web 配置页至少显示局域网访问提示；后续可增加一次性配对码或配置口令。
- 日志中禁止打印账号、密码、完整 JWT 和音频内容；只记录状态码、token 长度和错误类型。
- AI 后端地址只允许 `http`/`https`，限制长度，拒绝控制字符。
- 音频缓冲在本轮结束后释放，不持久化用户录音。

## 9. 实施阶段与文件边界

### Phase 0：建立回退点

**Files:** Git 分支和现有文档，不改功能代码。

**任务：** 保留原始小智构建作为回退点；确认 GitHub Actions 的九川构建入口和产物文件名。任何无屏版本失败时可重新刷回原固件。

### Phase 1：裁剪无屏入口

**Files:**

- Modify: `main/main.cc`
- Modify: `main/framework/framework_main.cc`
- Modify: `main/boards/jiuchuan-s3/jiuchuan_dev_board.cc`
- Modify: `main/CMakeLists.txt`
- Modify: `main/boards/jiuchuan-s3/config.json`

**任务：** 不初始化 GC9309/LVGL；只启动音频、Wi-Fi、按键、RGB 灯和无屏控制器。确保启动日志不再出现 LCD/LVGL 初始化错误。

**验收：** 无屏硬件冷启动不崩溃；串口能看到 Framework/headless 启动、音频初始化和 Wi-Fi 状态。

### Phase 2：实现电源键 PTT

**Files:**

- Modify: `main/boards/jiuchuan-s3/jiuchuan_dev_board.cc`
- Create/Modify: `main/framework/headless_voice_controller.{cc,h}`
- Modify: `main/boards/common/button.{cc,h}`（仅在现有事件不足时）

**任务：** 电源键按下开始录音、松开结束录音；取消原电源键单击聊天切换、长按关机和菜单映射。录音必须使用独立任务，按键回调不能阻塞。

**验收：** 未按键时麦克风输入关闭；按下后立即出现录音状态；松开后即使用户不再按键，后续 ASR/Chat/TTS 仍继续。

### Phase 3：实现 RGB 灯状态机

**Files:**

- Create: `main/framework/headless_led_controller.{cc,h}`
- Modify: `main/led/single_led.{cc,h}` 或增加无屏专用 LED 类
- Modify: `main/boards/jiuchuan-s3/jiuchuan_dev_board.cc`

**任务：** 集中实现绿色待机、橙色录音、蓝色处理/播报、红色错误、蓝色配网闪烁。灯效任务不能持有音频锁，不能阻塞 Wi-Fi/HTTP。

**验收：** 用串口事件日志和实物确认颜色、亮度、呼吸周期；若实物颜色与逻辑颜色相反，记录 WS2812 安装方向/颜色顺序并调整一次。

### Phase 4：实现本地语音提示与配网

**Files:**

- Create: `main/framework/audio_prompt_player.{cc,h}`
- Modify: `main/boards/common/wifi_board.cc`
- Modify: `main/framework/headless_network_controller.{cc,h}`
- Add: `main/assets/common/*` 或中文语言音频资源
- Modify: `main/CMakeLists.txt`

**任务：** 去掉配网路径中的 Display/Application 调用，接入本地提示音；处理配网 WebServer 与设备 WebServer 的端口占用和自动重试。

**验收：** 无 Wi-Fi 冷启动可听到“进入配网”；手机配网成功后可听到“网络配置成功”；连接超时可听到失败提示；网页最终能在设备 IP 打开。

### Phase 5：收敛配置和 JWT 客户端

**Files:**

- Modify: `main/framework/config/config_schema.cc`
- Modify: `main/framework/web/web_config_api.cc`
- Modify: `main/framework/web/web_ui_page.h`
- Modify: `main/framework/ai/ai_client.{cc,h}`

**任务：** 只保留后端地址、账号、密码、音色、采样率；密码输入为 password，GET 掩码；补足 401 单次重登、并发登录锁、超时和空响应判断。

**验收：** 网页保存后设备不重启即可使用新配置；首次对话触发一次登录；同一轮 ASR/Chat/TTS 不重复登录；token 失效后只自动重登一次。

### Phase 6：端到端稳定性验收

**Files:** 测试记录和必要的日志修复，不新增 UI。

**任务：** 使用 GitHub Actions 构建、烧录到无屏实机，以状态矩阵逐项验收。

## 10. 端到端验收矩阵

| 场景 | 操作 | 预期声音 | 预期灯光 | 预期网络/音频行为 |
|---|---|---|---|---|
| 首次开机 | 无 Wi-Fi 凭据上电 | 进入配网提示 | 蓝色闪烁 | AP 页面可配置 Wi-Fi |
| 配网成功 | 手机提交正确凭据 | 配网成功 | 绿色闪烁后常亮 | 获取 IP，设备 Web 配置页可访问 |
| 未填 AI 配置 | 不填账号密码按电源键 | 配置服务提示 | 红色短闪后绿色 | 麦克风不打开 |
| 正常录音 | 按住电源键说话 | 无需持续提示 | 橙色呼吸 | PCM 持续采集 |
| 松开提交 | 松开电源键 | 无额外打断 | 蓝色呼吸 | WAV 上传 ASR |
| 正常回答 | 等待服务端回复 | TTS 播放 | 蓝色活动呼吸 | Chat SSE、TTS SSE、喇叭播报 |
| 回到待机 | 播报完成 | 无 | 绿色常亮 | 麦克风关闭 |
| 服务端 401 | 修改/失效 token 后对话 | 登录失败或恢复提示 | 红色闪烁 | 只重登一次，不能死循环 |
| Wi-Fi 断开 | 对话前断网 | 网络异常提示 | 蓝色/红色按状态 | 不上传空请求，恢复后可重试 |
| 再次按键 | TTS 尚未完成时按键 | 等待当前回答提示 | 保持蓝色 | 不创建第二个录音任务 |
| 超长录音 | 按住超过上限 | 可选“录音结束”提示 | 转蓝色处理 | 自动提交，不无限占内存 |
| 无效短按 | 快速按下松开 | 没有听清提示 | 红色短闪 | 不调用 ASR |

## 11. 明确不做的功能

本方案不再实现以下内容：

- 任何屏幕、LVGL、菜单、主页、设置页、息屏页、壁纸、字体配置。
- 天气、截图、显示主题、屏幕 MCP 工具。
- 自动唤醒词对话。没有按住电源键时不得录音、不得把麦克风数据上传到服务端。
- 设备端长期保存对话录音。
- 设备端每轮使用账号密码；密码只用于登录或 token 失效后的重登。
- 依赖网络才能播放的配网提示音。

## 12. 当前实现与本方案的差距

截至文档编写时，仓库已有 `AiClient` 的登录、ASR、Chat SSE、TTS SSE 雏形，也已有九川 GPIO10 的 WS2812 `SingleLed` 驱动和 GPIO3 电源键事件，但仍存在以下未完成项：

1. Framework 入口仍注册屏幕模块并初始化显示，尚未裁剪为无屏入口。
2. 电源键当前仍路由到菜单/旧 Application 逻辑，尚未改为按下/松开 PTT。
3. `SingleLed` 的颜色接口为私有，且状态依赖旧 Application，需要无屏 LED 控制器。
4. Wi-Fi 配网代码仍调用 `Application::SetDeviceState/Schedule/Alert`，需要无屏事件回调和本地音频提示。
5. 配网语音资源尚未确定文件、音量和播放互斥策略。
6. Web 配置页仍包含屏幕相关 Schema，密码仍需要掩码处理。
7. AI 客户端的 401 重试、并发登录、请求取消和异常状态还需要按本方案补齐。
8. 以上内容均未在无屏实物上完成 GitHub Actions 构建、烧录和端到端验收。

这份文档取代原“个人桌面助手/可扩展屏幕框架”方案，后续开发以本文件为唯一功能范围和验收依据。
