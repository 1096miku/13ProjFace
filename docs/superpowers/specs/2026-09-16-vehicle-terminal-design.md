# 车载 AI 行车状态监测与语音交互终端 · 设计文档

- 日期：2026-09-16
- 状态：设计定稿（待用户复核）
- 上游计划书：[`docs/计划书.md`](../../计划书.md)（v1.0）
- 术语表：[`CONTEXT.md`](../../../CONTEXT.md)
- 基线提交：`87ee0c8`（去厂商标识后的初始提交）

本文只写**已定的设计**：模块边界、接口、数据流、存储与验收口径。计划书里被本文修正的地方，以本文为准。

---

## 1. 目标与范围

一台车内 ESP32-S3 终端：板载 IMU 判定行车状态与事件，摄像头 + 环境传感器看车内状况，屏幕与语音做交互，联网后把事件送到手机。设备同时是小智 AI 语音助手。

**做**：行车事件判定、环境数据（模拟源起步）、锁车监测与抓拍上传、屏幕四页、离线命令词与播报、巴法云事件上报、局域网原图。

**不做**（沿用计划书 §1.2，本文再次确认）：设备端视觉推理、CAN/OBD、手机 App、公网看原图。

---

## 2. 已定关键决策

| # | 决策 | 理由（要点） |
|---|---|---|
| D1 | 离线命令词走**上游 `CustomWakeWord`（MultiNet）路径**，不自建并行 MN | 命令词非硬指标；自建并行 MN 要改 `application.cc` 状态机，收益不抵成本 |
| D2 | 命令词**仅待机态生效** | MN 只在 `kDeviceStateIdle` 运行（`application.cc:906` 在 listening 关闭、`:921` 在 speaking 只对 AFE 唤醒词开启）。接受此边界，不改状态机 |
| D3 | 行车判定**内联在 `imu_task`**，不单开 `monitor_task` | `DrivingMonitor::Feed()` 是纯流式函数、20 ms 一次、µs 级开销；独立任务只多一层队列与一类时序 bug |
| D4 | 新增独立 `snapshots` spiffs 分区，**用闪存尾部 2.25 MB**，不动现有偏移 | assets 分区加完 MultiNet 模型后只剩约 0.48 MB，装不下 20 张抓拍 + 事件队列 |
| D5 | 播报片段由**用户提供 MP3** → 本地 WSL ffmpeg 转 OGG Opus（16 kHz 单声道 60 ms） | SAPI/手工录音音色更好；仓库已有 `scripts/mp3_to_ogg.sh` 同参数；WSL 已装 ffmpeg 4.4.2 |
| D6 | 数字播报按**中文读数**拼接（0–99 用「十」，如 26 → 二十六），>99 才逐位兜底 | 用户额外提供了「十」片段；本项目全部数值（温度/湿度/事件数）都在 0–99 内，自然读数比逐位自然得多，屏幕同时显示准确数字 |
| D7 | 项目内**不出现厂商标识**（中文品牌名与英文缩写均不得出现） | 已完成：板型符号、板名、全部文档；唯一遗留为 `docs/小智AI移植.docx`（待处置） |
| D8 | 不使用设备端视觉推理 | 承接上一个项目的否定结论，见 `esp32s3-face-recognition-feasibility-research.md` |

---

## 3. 系统架构

### 3.1 任务划分

| 任务 | 周期/触发 | 职责 | 栈 |
|---|---|---|---|
| `imu_task` | 20 ms（50 Hz） | 读 QMI8658A → `monitor_.Feed()` → 事件投递到事件队列 | 4 KB |
| `env_task` | 1 s | 读 `EnvironmentSensor`（模拟源或真实 I2C），更新最新值快照 | 3 KB |
| `camera_task` | 按需（预览开 / 抓拍请求 / 锁车触发） | 抓帧、预览上屏、锁车抓拍 JPEG 落盘 | 4 KB |
| `net_task` | 事件驱动 | 巴法云 MQTT 上报、断网队列补传 | 6 KB |
| HTTP 服务 | `esp_http_server` 自带任务 | `/`、`/latest.jpg`、`/events` | 库默认 |
| UI | LVGL port 任务（已有） | 四页刷新 | 库默认 |
| 语音 | xiaozhi 音频任务（已有） | 唤醒 / MN 命令词 / 播报 | 库默认 |

**纪律**：`imu_task` 内**只做采样 + 判定 + 投队列**，绝不调用 MQTT、HTTP、UI、播报等可能阻塞的接口。`DrivingMonitor` 实例为静态对象（放 `.bss`），不放任务栈。

**栈的位置**：表里的栈大小指**栈容量**，其内存一律用 `heap_caps_malloc(MALLOC_CAP_SPIRAM)` + `xTaskCreateStatic()` 从 PSRAM 出（与上游 `custom_wake_word.cc:222` 的唤醒词编码任务同法）。内部 RAM 只留给必要的控制块与中断/驱动缓冲——本项目内部 RAM 本来就紧（§11），这几个任务栈若全放内部就是 17 KB 起步。

### 3.2 数据流

```
QMI8658A ─20ms─► imu_task ─► DrivingMonitor ─事件─► EventQueue
                                                     │
              ┌──────────────────────────────────────┼───────────────┐
              ▼                                      ▼               ▼
        vehicle_ui（屏幕提示）              voice_command（播报）   net_task（MQTT）
传感器 ─1s─► env_task ─值─┬─► 环境页
（模拟/真实）             ├─► 命令词应答（读最新值→播报）
                          └─► net_task（周期 30 s 上报）
GC0308 ─预览─► camera_task ─► LVGL 预览区
       └抓拍─► snapshot_store（/snap）─► vehicle_http（/latest.jpg）
MN 命令词 ─► CustomWakeWord ─action─► AudioService 回调 ─► vehicle_service ─► 业务动作
```

### 3.3 模块清单

| 模块 | 文件 | 职责 | 依赖 |
|---|---|---|---|
| 判定逻辑 | `main/vehicle/vehicle_types.h`、`driving_monitor.{h,cc}` | 阈值配置、四类事件、停车/锁车状态机（**已存在**） | 仅标准库 |
| 环境抽象 | `main/vehicle/environment_sensor.{h,cc}` | `EnvironmentSensor` 接口 + `SimulatedSensor` | 仅标准库 |
| 事件队列 | `main/vehicle/event_bus.{h,cc}` | 定长环形队列的**纯逻辑部分**（可主机测）+ 设备侧包装 | 仅标准库（设备侧包装在板级） |
| IMU 驱动 | `main/boards/esp32s3/qmi8658a.{h,cc}` | I2C 初始化、50 Hz 突发读、g/dps 换算 | IDF I2C |
| 业务服务 | `main/boards/esp32s3/vehicle_service.{h,cc}` | 组装以上模块、事件分发、命令词动作实现、状态快照 | 上述模块 |
| 抓拍存储 | `main/boards/esp32s3/snapshot_store.{h,cc}` | `/snap` 挂载、环形覆盖、事件队列文件、上报游标 | spiffs、NVS |
| HTTP 服务 | `main/boards/esp32s3/vehicle_http.{h,cc}` | 三个路由，从 `snapshot_store` 取数据 | esp_http_server |
| 巴法云 | `main/boards/esp32s3/bemfa_client.{h,cc}` | 独立 esp-mqtt 客户端、主题封装、断线重连 | esp-mqtt |
| 语音命令 | `main/boards/esp32s3/voice_command.{h,cc}` | `action` → 意图映射、播报片段选择、数字拼接 | `Lang::Sounds`、`Application::PlaySound` |
| 界面 | `main/boards/esp32s3/vehicle_ui.{h,cc}` | 四页 LVGL、切页、事件提示浮层 | LVGL、`SpiLcdDisplay` 底座 |

**模块边界规则**：`main/vehicle/` 下任何文件不得 `#include` ESP-IDF/FreeRTOS 头文件（沿用现有硬规则）；设备侧代码不得把判定逻辑复制一份到板级目录。

---

## 4. 构建接线

1. `main/CMakeLists.txt:42` 的 `INCLUDE_DIRS` 增加 `vehicle`。
2. 源文件收集处增加 `file(GLOB VEHICLE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/vehicle/*.cc)`，并入 `SRCS`。
3. 本板分支（`elseif(CONFIG_BOARD_TYPE_VEHICLE_ESP32S3)`）内新增 `set(VEHICLE_COMMANDS_JSON ${CMAKE_CURRENT_SOURCE_DIR}/boards/esp32s3/voice_commands.json)`，并在调用 `scripts/build_default_assets.py` 的 `BUILD_ARGS` 里条件追加 `--vehicle_commands ${VEHICLE_COMMANDS_JSON}`。
4. 新增/删除 `main/vehicle/*.cc`、`main/boards/esp32s3/*.cc` 后必须 `idf.py reconfigure`（glob 无 `CONFIGURE_DEPENDS`）。往 `main/assets/common/` 增删 ogg 同理。

**对上游的偏离清单**（除此之外不改上游文件）：

| 文件 | 改动 |
|---|---|
| `main/Kconfig.projbuild` | 板型符号 `BOARD_TYPE_VEHICLE_ESP32S3`、显示名 "Vehicle ESP32-S3 Terminal Board"（已改） |
| `main/CMakeLists.txt` | 本板分支的 `BOARD_NAME`、vehicle 源码与 `INCLUDE_DIRS`、命令表传参；**并修掉上游 `gen_lang.py` 的依赖缺陷**：`add_custom_command` 的 `DEPENDS` 只列了 `language.json`，没列 `${LANG_SOUNDS} ${COMMON_SOUNDS}`，导致新增 ogg 不会重生成 `lang_config.h`——新片段没有符号引用，会被 `--gc-sections` 丢掉（app 体积不涨，且代码里 `Lang::Sounds::OGG_xxx` 编译不过）。实测：修复前 app 体积 0x2dbd10 与加片段前完全一致 |
| `main/application.cc` | `CheckNewVersion()` 用 `#ifdef CONFIG_BOARD_TYPE_VEHICLE_ESP32S3` 跳过自动升级（已改） |
| `main/audio/audio_service.cc` | `SetModelsList()` 选路不变，新增命令词回调转发（约 20 行） |
| `main/audio/wake_words/custom_wake_word.{h,cc}` | 非 `wake` 的 `action` 派发出去（约 30 行），不停止 `running_` |
| `scripts/build_default_assets.py` | 新增 `--vehicle_commands`，把命令表并进 `index.json` 的 `multinet_model.commands` |
| `partitions/v2/16m.csv` | 追加 `snapshots` 分区 |
| `sdkconfig.defaults.esp32s3` | 新增本项目默认值（见 §6.1、§5.3） |

---

## 5. 分区与存储

### 5.1 分区表

`partitions/v2/16m.csv` 追加一行，现有分区偏移一律不动：

```
snapshots, data, spiffs, 0xDC0000, 0x240000,
```

`0xDC0000 + 0x240000 = 0x1000000`，正好用满 16 MB 尾部，无需重烧 app，首次刷入后 erase 一次该分区。

### 5.2 容量账（实测值）

| 分区 | 大小 | 现状 | 结论 |
|---|---|---|---|
| `ota_0` | 0x3F0000 = 4,128,768 B | `xiaozhi.bin` 2,997,504 B | 余 1,131,264 B；本项目新增约 250 KB（含播报片段），够 |
| `ota_1` | 0x3F0000 | — | 保留 A/B OTA 能力 |
| `assets` | 0x5C0000 = 6,029,312 B | `generated_assets.bin` 2,851,767 B | 加 `mn7_cn`（约 2.67 MB）后余约 0.48 MB；**不再往 assets 加任何东西** |
| `snapshots` | 0x240000 = 2,359,296 B | 空 | 抓拍环形 32 张（约 640 KB）+ 事件队列 ≤500 条（约 64 KB）+ 余量 |

### 5.3 `/snap` 布局

```
/snap/snap_000.jpg … snap_031.jpg   环形覆盖，JPEG QVGA，约 15–25 KB/张
/snap/events.log                     追加写，每行一条 JSON（与 MQTT payload 同构）
/snap/latest.idx                     最近一张抓拍的文件名（供 /latest.jpg）
```

- 上报游标存 NVS（key `sent_seq`）；事件序号单调递增，幂等键 = `<device_id>-<seq>`。
- 环境数据不上盘（周期上报，丢了无所谓）。
- 挂载失败（分区未 erase、spiffs 损坏）时：`snapshot_store` 进入"内存模式"（只保留最近一张抓拍在 PSRAM），并播报/上屏一条降级提示，**不阻塞**其他功能。

---

## 6. 语音

### 6.1 Kconfig（写入 `sdkconfig.defaults.esp32s3`）

```
CONFIG_USE_CUSTOM_WAKE_WORD=y
CONFIG_CUSTOM_WAKE_WORD="ni hao xiao zhi"
CONFIG_CUSTOM_WAKE_WORD_DISPLAY="你好小智"
CONFIG_CUSTOM_WAKE_WORD_THRESHOLD=20      # 现场按误唤醒率微调
CONFIG_SR_MN_CN_MULTINET7_QUANT=y         # 打包进 assets/srmodels.bin
```

- 与 `CONFIG_USE_AFE_WAKE_WORD` 是互斥选项，切过去后唤醒词由 MultiNet 拼音识别承担；`wn9` 仍留在模型包里（`AFE` 的 NS/VAD 走 `audio_processor_`，与本选项无关，对话链路不受影响）。
- 拼音先按上游默认风格（**小写、空格分隔、不带声调**，与 `CONFIG_CUSTOM_WAKE_WORD` 默认值 `xiao tu dou` 一致）。若实测识别差，再试带声调写法。

### 6.2 命令表（单一来源）

`main/boards/esp32s3/voice_commands.json`：

```json
[
  { "action": "wake",     "command": "ni hao xiao zhi",  "text": "你好小智" },
  { "action": "temp",     "command": "che nei wen du",   "text": "车内温度" },
  { "action": "humid",    "command": "che nei shi du",   "text": "车内湿度" },
  { "action": "light",    "command": "guang zhao duo shao", "text": "光照多少" },
  { "action": "occupancy","command": "you mei you ren",  "text": "有没有人" },
  { "action": "status",   "command": "she bei zhuang tai","text": "设备状态" },
  { "action": "chime",    "command": "bo fang ti shi yin","text": "播放提示音" },
  { "action": "snapshot", "command": "chong xin zhua pai","text": "重新抓拍" },
  { "action": "lock",     "command": "suo che",          "text": "锁车" }
]
```

- 构建期由 `scripts/build_default_assets.py --vehicle_commands` 读入并写进 `assets/index.json` 的 `multinet_model.commands`（上游原本只写 Kconfig 里那一条 wake）。
- 运行期 `CustomWakeWord` 从 `index.json` 读回，逐条 `esp_mn_commands_add()`，所以**改词表不需要重新训练模型**（`CONTEXT.md` 里"改词表要重新出模型"的说法是错的，本次一并更正）。
- C++ 侧 `voice_command.cc` 用 `enum class VoiceIntent` + `{action, intent}` 常量表解释回调；`test/voice_commands_test.cc` 在主机上读同一份 JSON，断言**每个 JSON action 都有 C++ 映射**，防止两处漂移。

### 6.3 派发链路

```
CustomWakeWord::Feed()  ── action != "wake" ──►  命令词回调
        │                                              │
        └─ action == "wake" ──► 现有唤醒流程（不变）    ▼
                                            AudioService 转发
                                                       ▼
                                            VehicleService::OnVoiceCommand(action)
                                                       ▼
                                    VoiceIntent 查表 → 读最新值 / 执行动作 → 播报 + 上屏
```

- `custom_wake_word.cc` 的改动只在 `Feed()` 里：非 `wake` 分支调用新回调、`multinet_->clean()` 后**继续监听**（不设 `running_ = false`）。
- `AudioService::SetVoiceCommandCallback(cb)` 记住回调并转发给当前的 `CustomWakeWord`；未设置回调时行为与上游一致（命令词被忽略）。

### 6.4 播报

- 播放接口：`Application::GetInstance().PlaySound(Lang::Sounds::OGG_XXX)`。
- 片段来源：用户提供的 mp3 放在仓库根目录 `res/`，用 `scripts/convert_voice_prompts.sh`（WSL 里跑）转码成 `main/assets/common/*.ogg`，再由 `main/CMakeLists.txt` 的 `EMBED_FILES ${LANG_SOUNDS} ${COMMON_SOUNDS}` 编入 **app 分区**。
- 实测体积：新增 42 个片段文件合计 **120,679 B**（含合成的 `tone_chime.ogg`）。
  - **链接行为（踩过一次）**：片段数据只有被代码引用时才会进 app。只生成 `Lang::Sounds::OGG_xxx` 常量、没有任何代码用它，那个 `static const std::string_view` 不会被发射，数据被 `--gc-sections` 丢掉——实测此时 `xiaozhi.bin` 体积 **0 增长**（2,997,520 B 不变）。所以"片段明明放进去了但播报没反应"的第一嫌疑是**代码没引用对应常量**，而不是文件格式。
  - Plan C 写完播报代码后，预计 app 余量从 1,131,248 B 降到约 1.01 MB。
- 硬性格式：**OGG Opus / 单声道 / 60 ms 帧 / `-b:a 32k`**。
  - `-b:a 32k`：源 mp3 本身就是 16 kHz / 32 kbps，输出降到 16 kbps 等于再砍一半、白丢一代质量；多出的约 60 KB 换音质划算。
  - `ffprobe` 对 Opus 一律报 `sample_rate=48000`（编解码器内部采样率），上游已能正常播放的 `popup.ogg` 同样如此，不是问题。
  - **60 ms 帧是硬要求**：`AudioService::PlaySound()` 的 `OggDemuxer` 回调里 `frame_duration` 固定按 60 写死。校验方法 `frames × 0.06 ≈ duration`（脚本内置，47 个片段全部通过）。
- `gen_lang.py` 按 `common/<base>.ogg` → `Lang::Sounds::OGG_<BASE>` 生成常量（`lang_config.h` 是生成物，已 gitignore）。因此**文件名必须 ASCII 小写下划线、不能用连字符**。
- 拼接规则：由 `NumberToClipIds(int)`（纯逻辑、主机可测）把数值转成片段序列，顺序恒为 `前缀 → 数字 → 单位`：
  - `0–9` → `d0…d9`
  - `10–19` → `d10` + 个位（10 只发 `d10`）
  - `20–99` → 十位数字 + `d10` + 个位（48 → `d4 d10 d8`）
  - `>99` → 逐位兜底（本项目用不到：温度/湿度/事件数都在 0–99；光照不读数字）
  - **负温度不支持**（没有「零下」片段）：模拟源与实测都在 0 ℃ 以上；万一为负，只上屏不播报
- 光照不做数字播报，改播**档位**（很暗/偏暗/适中/明亮/很强），具体数值上屏。
- 片段清单与情感基调见附录 A。

---

## 7. 网络

### 7.1 巴法云 MQTT

独立 `esp-mqtt` 客户端（与 xiaozhi 自带的 MQTT+UDP 协议无关）：

| 主题 | 内容 | 频率 |
|---|---|---|
| `vehicle/{device_id}/event` | `{"seq":12,"type":"hard_brake","ts":1758000000,"value":-0.52}` | 事件触发 |

`device_id` 取 `SystemInfo::GetMacAddress()` 去掉分隔符后的大写形式（12 位十六进制），全程只此一处定义。
| `vehicle/{device_id}/env` | `{"ts":…,"temp":26.4,"humid":48,"lux":320,"src":"sim"}` | 30 s |
| `vehicle/{device_id}/status` | `{"ts":…,"state":"parked","net":"online","events_today":3}` | 60 s + 状态变化 |

- 断网：事件只写 `/snap/events.log` 并入队，重连后按 `seq` 顺序补传；图片**不走 MQTT**。
- 免费版限制：单条 payload 控制在 256 B 以内，周期上报与事件分开主题。

### 7.2 局域网 HTTP

| 路由 | 内容 |
|---|---|
| `/` | 一页静态 HTML（内嵌字符串，无外部依赖）：最近事件列表 + 一张图 |
| `/latest.jpg` | `/snap/latest.idx` 指向的 JPEG，`Cache-Control: no-store` |
| `/events` | `events.log` 最近 N 条 JSON |

---

## 8. 界面（LVGL 9.4，`SpiLcdDisplay` 底座）

| 页 | 内容 | 刷新 |
|---|---|---|
| 主页 | 温度/湿度/光照（带"模拟"角标）、行车状态、今日事件数、联网状态；可选三轴实时值（设置页开关） | 1 s |
| 实时画面 | QVGA 320×240 画面居中（不放大到全屏，省 SPI 带宽），抓拍按钮 | 预览按需 |
| 事件记录 | 最近 N 条（类型/时间/数值），上下翻页 | 事件触发 |
| 设置 | 阈值查看、环境源（模拟/真实）、手动锁车开关、三轴显示开关 | 手动 |

- 与 xiaozhi 自带聊天界面共存：本项目页面建在**独立 screen** 上，用 `lv_screen_load()` 切换；事件提示用主页上的浮层（`lv_layer_top()`），不改上游 display 类的内部结构。
- 预览与抓拍互斥：同一把 `std::mutex`，预览开启时暂停高频抓拍。

---

## 9. 错误处理与降级

| 场景 | 行为 |
|---|---|
| IMU 初始化失败（WHO_AM_I 不符） | 上屏"IMU 异常"+ 播报降级提示；`imu_task` 不启动，其余功能照常 |
| 基线未标定 | 状态机停在 `kUncalibrated`，上屏提示"请保持静止完成标定"；不产生任何事件 |
| I2C 读超时 | 丢弃该帧并计数，连续 50 帧（1 s）失败则重新初始化 IMU 并重置标定 |
| 环境传感器缺失 | 按 Kconfig 用模拟源（默认），界面标"模拟" |
| 摄像头初始化失败 | 预览页显示占位并上屏提示；锁车抓拍跳过并记事件 |
| `/snap` 挂载失败 | 内存模式（只留最近一张），播报降级提示 |
| 巴法云断线 | 事件入本地队列（上限 500 条，满了丢最旧并计数），恢复后补传 |
| 小智云不可达 | 车载问答不可用；命令词、播报、监测、预览全部照常（离线能力） |
| 唤醒词识别不到 | 阈值现场调，兜底：BOOT 按键切对话（上游已有） |

---

## 10. 测试与验收

### 10.1 主机测试（g++，秒级）

| 测试 | 覆盖 |
|---|---|
| `test/driving_monitor_test.cc` | 已有：标定、四类事件、停车/锁车状态机、冷却、队列（保留） |
| `test/environment_sim_test.cc` | 模拟源范围与缓变、来源标记 |
| `test/event_bus_test.cc` | 环形队列满/空/覆盖语义 |
| `test/voice_commands_test.cc` | JSON 命令表 ↔ C++ 意图表一致性（防漂移） |
| `test/snapshot_ring_test.cc` | 环形索引推进与文件名生成（纯逻辑部分） |

### 10.2 真机验收清单

1. I2C 上 `0x19 / 0x18 / 0x41 / 0x38 / 0x6A` 在线；静止 `az ≈ 1 g`，翻转后 `az ≈ −1 g`。
2. 9 条命令词逐条可识别（待机态）；识别后动作与播报正确。
3. 预览 ≥10 fps；抓拍出 JPEG，`/latest.jpg` 可在手机浏览器打开。
4. 静置 `static_hold_ms` 进入停车；再过 `lock_hold_ms` 进入锁车监测并抓拍；敲击触发异常震动抓拍。
5. 巴法云小程序可远程看到事件与环境数据；断网 5 分钟后恢复，积压事件按序补传且无重复。
6. 全功能并行 2 h 无重启；期间周期打印 Free 内部 RAM / PSRAM。

### 10.3 对计划书 §1.1 指标的修正

| 指标 | 修正 |
|---|---|
| 碰撞事件准确率 | **不做手持实测**（2.5 g 阈值手持难以稳定复现且有损坏风险），改为主机单测覆盖 + 报告写明 |
| 运行内存余量 | 由"Free 内部 RAM ≥ 15 KB"改为 **≥10 KB 且无分配失败**；上个项目实测对话中最低 17 KB，本项目叠加预览/HTTP/第二个 MQTT/MN 后 15 KB 不现实，D7 实测后回填真实值 |
| 光照播报 | 由"播报数值"改为**播报档位**（数值上屏），避免 4–5 位数字拼接 |
| 唤醒词 | 由 WakeNet（wn9）改为 MultiNet 拼音识别，唤醒率指标改为**实测记录**而非硬性 ≥90%（该指标原属 wn9 路径） |

---

## 11. 风险与未决

| 项 | 说明 | 处置 |
|---|---|---|
| 唤醒率下降 | MN 拼音识别弱于专用 WakeNet | D6 实测；若不可接受，回退 `USE_AFE_WAKE_WORD` 并接受"无离线命令词" |
| 命令词误触发 | MN 常驻待机态，车内对话/音响可能命中 | 阈值上调；动作分级（查询类无副作用，抓拍/锁车类加冷却） |
| 内部 RAM | 预览 + HTTP + MQTT + MN 并存 | 全链路缓冲走 PSRAM；D7 打印实测值 |
| SPI/PSRAM 带宽 | 预览与抓拍争抢 | 互斥调度 + 预览区只重绘 320×240 |
| `docs/小智AI移植.docx` | 含 7 处厂商标识，二进制无法文本改写 | **暂不处理**（用户 2026-09-16 决定）：保持原样，作为已知遗留记录在案；交付前再决定移出/删除/清洗 |
| `partitions/v2/16m-attend.csv` | 上个项目遗留死文件（无厂商标识，不影响构建） | 保留；如需清理另行确认 |
| MN 模型与 assets 余量 | 加 mn7 后 assets 仅余约 0.48 MB | 后续任何资源（字体/表情）不得再加进 assets |
| 拼音写法 | 无调 vs 带调识别率未知 | 两者都试，取优者写回 `sdkconfig.defaults.esp32s3` |

---

## 附录 A：播报片段清单（交付给用户制作 MP3）

### A.1 制作要求

- 一条一个文件，文件名**严格照下表**（ASCII 小写下划线，`.mp3`），不要用连字符、不要中文名。
- **普通话、同一位说话人、同一套语气**——数字与短句要拼接，音色/音量/语速必须一致。
- 不要背景音乐、混响、淡入淡出；数字尤其要"干净"且**不带语调起伏**。
- 句首句尾静音 ≤0.15 s（转换时会自动裁首尾静音，但不要留大段空白）。
- 采样率/码率不限（统一重采样到 16 kHz 单声道），建议 44.1 kHz / ≥128 kbps。
- 交货：全部放进一个目录告诉我路径即可，我来转码并落位。

### A.2 情感基调总则

| 组 | 基调 |
|---|---|
| 行车事件 | 短促、干脆、中性偏警示，语速稍快，不要惊慌；"检测到碰撞"稍加重 |
| 状态陈述 | 平静、陈述句 |
| 快照/锁车 | 平静、事务性 |
| 查询应答 | 播报腔，语速中等，友好 |
| 标定/就绪 | 平静指令 / 友好提示 |
| 数字 0–9 | 单字、短、清晰、**语气中立**，字与字之间不连读 |

### A.3 片段表（40 条必录 + 额外提供的 2 条）

| 文件名 | 文字 | 用途 | 基调 |
|---|---|---|---|
| `ev_hard_accel.mp3` | 急加速 | 事件提示 | 短促警示 |
| `ev_hard_brake.mp3` | 急刹车 | 事件提示 | 短促警示 |
| `ev_hard_turn.mp3` | 急转弯 | 事件提示 | 短促警示 |
| `ev_bump.mp3` | 颠簸 | 事件提示 | 短促警示 |
| `ev_crash.mp3` | 检测到碰撞 | 事件提示 | 加重、稍慢 |
| `ev_parked.mp3` | 已停车 | 状态 | 平静陈述 |
| `ev_driving.mp3` | 行驶中 | 状态 | 平静陈述 |
| `ev_motion_parked.mp3` | 检测到异常震动 | 锁车态事件 | 提醒但不惊吓 |
| `lock_entered.mp3` | 已进入锁车监测 | 锁车 | 平静 |
| `lock_exited.mp3` | 已解除锁车监测 | 解锁 | 平静 |
| `snap_start.mp3` | 正在抓拍 | 抓拍 | 平静 |
| `snap_done.mp3` | 抓拍完成 | 抓拍 | 平静 |
| `calib_start.mp3` | 标定中，请保持静止 | 上电标定 | 平静指令 |
| `calib_done.mp3` | 标定完成 | 上电标定 | 平静 |
| `ready.mp3` | 行车监测已就绪 | 上电完成 | 友好 |
| `q_temp.mp3` | 车内温度 | 温度查询前缀 | 播报腔 |
| `q_humid.mp3` | 车内湿度百分之 | 湿度查询前缀 | 播报腔 |
| `q_light_dark.mp3` | 光照很暗 | 光照档位 | 播报腔 |
| `q_light_dim.mp3` | 光照偏暗 | 光照档位 | 播报腔 |
| `q_light_mid.mp3` | 光照适中 | 光照档位 | 播报腔 |
| `q_light_bright.mp3` | 光照明亮 | 光照档位 | 播报腔 |
| `q_light_strong.mp3` | 光照很强 | 光照档位 | 播报腔 |
| `q_no_alert.mp3` | 未检测到异常 | "有没有人"应答 | 播报腔 |
| `q_pending.mp3` | 有遗留提醒待确认 | "有没有人"应答 | 略提醒 |
| `q_status_ok.mp3` | 设备运行正常 | 设备状态 | 播报腔 |
| `q_online.mp3` | 已联网 | 设备状态 | 播报腔 |
| `q_offline.mp3` | 未联网 | 设备状态 | 播报腔 |
| `q_events.mp3` | 今日事件 | 事件数前缀 | 播报腔 |
| `unit_degree.mp3` | 度 | 温度单位 | 中立 |
| `unit_times.mp3` | 次 | 计数单位 | 中立 |
| `d0.mp3` … `d9.mp3` | 零 一 二 三 四 五 六 七 八 九 | 数字读数拼接 | 单字中立 |
| `d10.mp3`（额外提供） | 十 | 10–99 的读数，**已用上** | 单字中立 |
| `d100.mp3`（额外提供） | 百 | **未使用**：光照走档位、其余数值都 ≤99；转码脚本会跳过它 | — |

**不需要录**的两条：

- 「播放提示音」命令的提示音：用 ffmpeg 直接合成（正弦音 + 淡出），文件 `tone_chime.ogg`。
- 唤醒应答：由小智云端处理（设备只上报唤醒文本），无本地片段。

### A.4 转码（已完成）

一键脚本：`scripts/convert_voice_prompts.sh`（在 WSL 里跑，需要 ffmpeg）：

```bash
wsl -- bash /mnt/d/vscode/ESP32Project/13ProjFace/scripts/convert_voice_prompts.sh
```

脚本做的事：把 `res/*.mp3` 逐条裁掉首尾静音（`silenceremove` 前后各一次）、转成 **OGG Opus / 32 kbps / 单声道 / 16 kHz / 60 ms 帧**写入 `main/assets/common/`；跳过 `d100`；另外用 ffmpeg 合成 `tone_chime.ogg`（880 Hz 正弦 + 淡出，给「播放提示音」命令用）；最后自校验编码参数与 60 ms 帧长。

**本次执行结果**：转换 41 条 + 提示音；`main/assets/common` 下 ogg 共 47 个（含上游原有 5 个）；新增 42 个合计 120,679 B；60 ms 帧校验 **47/47 通过，0 个可疑**。

增删片段后的操作顺序：先 `idf.py reconfigure`（`main/assets/common` 的 ogg 列表是 configure 期 glob，文件集合变了要重新求值），再 `idf.py build`。`lang_config.h` 会自动重生成并产出 `Lang::Sounds::OGG_<BASE>` 常量——这靠的是本项目对 `gen_lang.py` 依赖的修复（见 §4 偏离清单的 `main/CMakeLists.txt` 行），上游配置下这一步会静默不发生。
