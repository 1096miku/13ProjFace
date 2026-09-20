# D6–D7 验收记录（巴法云 MQTT 上报 + 离线命令词与播报 + 联调）

- 日期：**D6 = 2026-09-20 下午**（本轮）；D7 的量化指标**大部分未测**，见 §6
- 仓库：`D:\vscode\ESP32Project\13ProjFace`，分支 `main`；基线 HEAD `9ac4030`，**本轮改动尚未提交**
- 固件版本：App `xiaozhi` 2.2.4 / ESP-IDF `v5.5.3`
  - `build/xiaozhi.bin` = **3,181,664 B**；`build/xiaozhi.elf` SHA256 `457BE230C811E84A7B1515A0153D245022E9569747753BE9E9801749947C538F`
  - `build/generated_assets.bin` = **5,243,614 B** / assets 分区 6,029,312 B（**余量 785 KB**，`mn7_cn` 已打包进模型包）
- 硬件：自定义 ESP32-S3 开发板（N16R8 / 480×320 / GC0308 / QMI8658A / PCA9557）
- 串口：**COM10 已不可用**（`Could not open COM10, the port is busy or doesn't exist`，本轮全部用 **COM13** 烧录 + 抓取）
- 网络：手机热点 `iQOO Z10 Turbo`，板子 `192.168.202.227`
  - ! **本机（PC）在有线网 `192.168.8.20`，与板子不在同一网段**，所以 `http://192.168.202.227:8080/` 从 PC 打不开（这是网络拓扑，不是固件问题）。HTTP 路由的真机验证必须用**手机浏览器**，见 §5 未验证项
- 证据文件（都在 `build/`，gitignored）：
  - `d6_task1_build.log` —— 命令表进 assets（`vehicle commands: 9 …`、`multinet models: mn7_cn, fst`、`wakenet … skipping`）
  - `d6_task1_boot.log` —— **9 行 `CustomWakeWord: Command: …` + `Quantized MultiNet7: … name:mn7_cn`**
  - `d6_task4_build.log` / `d6_task5_build.log` / `d6_task5b_build.log` / `d6_task4b_build.log` —— 逐次构建（零 warning，均含新增 `.cc.obj`）
  - `d6_task4_voice.log` —— **命令词真机验收主证据**（8 条全部识别 + 执行；含 BUG-040 静默现场与 BUG-006 崩溃现场）
  - `d6_task4c_voice.log` —— 加逐段诊断日志后：「设备状态」**4/4 有声**、事件播报有声
  - `d6_task5_mqtt.log` / `d6_task5_mqtt_wait.log` / `d6_task5_boot2.log` / `d6_task5_stable.log` —— 端点与重连对照（BUG-038）
  - `d6_task5c_uplink.log` —— **上行落地后的稳定运行**（400 s，`已发=73 失败=0 丢弃=0 积压=0 连接=是`，零固件复位）
  - `d6_ram_diag.log` —— 内部 RAM **逐点差分**（=2048 版）：一个 esp-mqtt 客户端独占 7768 B，我们自己只花 ~750 B
  - `d6_ram512_diag.log` / `d6_ram512_verify.log` —— 同一差分（=512 版）：初始化后可用 19,311 → **38,311 B**，低水位 6167 → **19,179~21,915 B**（见 BUG-041）

---

## 1. 主机单元测试（11 套全绿）

`C:\mingw64\bin` 在 PATH 上，`-Wall -Wextra` **零 warning**，全部 `exit=0`。

| 套件 | 结果 | 备注 |
|---|---|---|
| `driving_monitor_test` | 全部通过（失败 0 项） | D3 既有 |
| `imu_convert_test` | all passed | D3 既有 |
| `event_history_test` | all passed | D3 既有 |
| `environment_sim_test` | all passed | D5 既有 |
| `snapshot_ring_test` | all passed | D4 既有 |
| `event_json_test` | all passed | **D5 落盘格式回归**：`EventToJson` 加默认参数后逐字节不变 |
| `event_text_test` | all passed | 既有 |
| `voice_intent_test` | 全部通过（失败 0） | **本轮新增**：JSON 命令表 ↔ C++ 意图表双向一致（防漂移） |
| `voice_reply_test` | 全部通过（失败 0） | **本轮新增**：数字读数拼接 0/7/10/11/19/20/26/48/90/99/100/123/负数 + 档位 + 状态序列 |
| `pending_queue_test` | 全部通过（失败 0） | **本轮新增**：环形队列满/空/丢最旧/回绕/超长截断 |
| `report_json_test` | 全部通过（失败 0） | **本轮新增**：上报 JSON 字段与顺序、`ParseSeqFromJsonLine` 边界、`AddReportFields` 幂等 |

> `event_text_test` 的编译命令在计划里写漏了依赖：它还需要 `main/vehicle/event_json.cc`（`ToDecimal`）
> 与 `main/vehicle/driving_monitor.cc`（`ToString(EventType)`），否则链接报 undefined reference。

---

## 2. 任务 1 验收：9 条命令词真的进了 MN（✅）

「唤醒词换成 MN 拼音识别」是本轮最大风险点，**已验掉**。`build/d6_task1_boot.log`：

```
I (6141) WifiStation: Got IP: 192.168.202.227
create static modelsI (6151) MODEL_LOADER: Successfully load srmodels
I (6891) VehicleHttp: 手机浏览器打开：http://192.168.202.227:8080/
I (9301) CustomWakeWord: Command: ni hao xiao zhi, Text: 你好小智, Action: wake
I (9301) CustomWakeWord: Command: che nei wen du, Text: 车内温度, Action: temp
I (9311) CustomWakeWord: Command: che nei shi du, Text: 车内湿度, Action: humid
I (9311) CustomWakeWord: Command: guang zhao duo shao, Text: 光照多少, Action: light
I (9321) CustomWakeWord: Command: you mei you ren, Text: 有没有人, Action: occupancy
I (9331) CustomWakeWord: Command: she bei zhuang tai, Text: 设备状态, Action: status
I (9341) CustomWakeWord: Command: bo fang ti shi yin, Text: 播放提示音, Action: chime
I (9351) CustomWakeWord: Command: chong xin zhua pai, Text: 重新抓拍, Action: snapshot
I (9351) CustomWakeWord: Command: suo che, Text: 锁车, Action: lock
Quantized MultiNet7:rnnt_ctc_2.0, name:mn7_cn, (Dec 16 2025 14:45:55)
9 active speech commands:  →  Command 1..9（逐条列出）
```

`idf.py build` 侧的 assets 生成日志（`d6_task1_build.log`）：

```
  Note: Found wakenet models ['wn9_nihaoxiaozhi_tts'] but wake word type is not ESP/AFE, skipping
  multinet models: mn7_cn, fst (will be packaged)
  vehicle commands: 9 条（wake, temp, humid, light, occupancy, status, chime, snapshot, lock）
```

即设计文档 §6.1 那句"切到 MN 后 wn9 仍留在模型包里"是**错的**（见 **BUG-036**）。

---

## 3. 命令词识别与播报实机验收（✅ 8/8 识别；播报 1 条曾间歇静默）

**判据**：`VC_ACTION`（派发到板级）与 `VC_DONE`（执行并播报完）由 `esp32s3_board.cc` 与 `voice_command.cc` 各打一次。
`build/d6_task4_voice.log` 里两者**数量完全相等（40 = 40）**，即"识别到的每一条都执行了"。

| 命令 | 实测命中次数 | VC_DONE | 用户听到 |
|---|---|---|---|
| 车内温度 `temp` | 9 | 9 | ✅ |
| 车内湿度 `humid` | 14 | 14 | ✅ |
| 光照多少 `light` | 2 | 2 | ✅ |
| 有没有人 `occupancy` | 1 | 1 | ✅ |
| 设备状态 `status` | 2 | 2 | ❌ **这一版静默**（见下） |
| 播放提示音 `chime` | 3 | 3 | ✅ |
| 重新抓拍 `snapshot` | 2 | 2 | ✅（`CameraCapture: 抓拍完成：原因=手动 … 落盘=是`） |
| 锁车 `lock` | 7 | 7 | ✅（进入 + 解除都播了） |

- **唤醒链路未受影响**：说「你好小智」能正常唤醒并进入小智对话（用户确认）
- **非待机态不响应**：**未做对照实测**。机制上成立（MN 只在 `kDeviceStateIdle` 跑，上游 `application.cc` 用 `IsAfeWakeWord()` 判断，`CustomWakeWord` 不是 AFE），但本轮没有"对话进行中故意说命令词、再数有没有 `VC_ACTION`"这一次对照
  - ! 反倒抓到一个相关现象：用户第一次测时，「有没有人」「播放提示音」被小智当成对话内容回答"没有这个功能"——即**当时设备处在对话态、音频走云端**。结合下面 §6 的"命中概率普遍偏低"，更像是**MN 把命令词误判成了唤醒词**（`action == "wake"` 会走唤醒分支并停止 MN），而不是"待机态外也会响应"
- **事件自动播报可用**：`d6_task4c_voice.log` 里 `ev_driving` / `ev_hard_brake` / `ev_hard_accel` / `ev_parked` 逐条播出（四种片段字节数 2.5~2.6 KB）
- **播报片段真的在 app 里**：逐段诊断日志给出字节数，例如
  `播放 q_status_ok（4806 B，内部 RAM 余 19651 B）`…`播放 unit_times（1204 B，内部 RAM 余 18287 B）`

### 「设备状态」静默 = **BUG-040（间歇性，根因未定性）**

- **旧固件（`d6_task4_voice.log`）**：「设备状态」说 2 次、**2 次都没声音**，但 `VC_ACTION`/`VC_DONE` 齐全（用户报告 + 串口一致）
- **新固件（`d6_task4c_voice.log`）**：只加了逐段诊断日志（**逻辑没改**），「设备状态」**4/4 全有声**（t=29690 / 38970 / 47690 / 66970）
- 已排除：片段缺失、容器格式（都是 `OggS+OpusHead`）、解包失败（`ogg_demuxer` 无堆分配且失败会打日志，实测零告警）、解码队列塞满（`wait=true`）
- 怀疑方向（**未验**）：`AudioService::CheckAndUpdateAudioPowerState()` 与 `PlaySound()` 都能开关功放、**没有互斥**。详见 **docs/BUGS.md BUG-040**

---

## 4. 巴法云 MQTT 上报验收（✅ 上行已落地）

### 4.1 连接与认证

| 项 | 实测 |
|---|---|
| 端点 | **`mqttv2.bemfa.com:2023`**（控制台"连接地址"列明示；官方文档那组 `bemfa.com:9501` 也能连上但数据不落地，见 BUG-039） |
| 认证 | `client_id = username = BEMFA_APP_ID`（= 私钥/UID），`password = BEMFA_SECRET_KEY`，keepalive 60 s、clean session、无 will —— 与用户 STM32 工程跑通的那份一致 |
| 主题 | `wUV1aTSNK005`（名称 vtevt01）/ `8l15Z3ah7005`（vtenv01）/ `v31h2zVHO005`（vtsta01） |
| 连接结果 | `BemfaClient: 巴法云已连接（积压 N 条待补传）`；`连接=是` 长期稳定 |

### 4.2 上报统计（`d6_task5c_uplink.log`，**400 s 连续**）

```
I (30260)  BemfaClient: 上报统计 已发=55  失败=0 丢弃=0 积压=0 连接=是
I (90460)  BemfaClient: 上报统计 已发=58  失败=0 丢弃=0 积压=0 连接=是
I (180500) BemfaClient: 上报统计 已发=63  失败=0 丢弃=0 积压=0 连接=是
I (270500) BemfaClient: 上报统计 已发=67  失败=0 丢弃=0 积压=0 连接=是
I (390690) BemfaClient: 上报统计 已发=73  失败=0 丢弃=0 积压=0 连接=是
```

- **成功率 = 73 / (73 + 0) = 100%**（口径：`已发` = "成功交给 esp-mqtt 并入 outbox"，**不是**"巴法云端已确认收到"——`Mqtt` 抽象没暴露 `MQTT_EVENT_PUBLISHED`，拿不到 ack 计数。见 §6 口径说明）
- **落地旁证（决定性）**：用户刷新巴法云控制台，确认三个主题的「**更新时间**」变成当前时间（改动前用"名称"当主题发了半小时，更新时间一动不动）
- **整段 400 s 零固件复位**（唯一的 `rst:0x1 (POWERON)` 是抓取脚本 `-Reset` 脉冲造成的，属测量起点）
- 周期：环境 30 s 一条、状态 60 s 一条
- 顺带一条**与本链路无关**的观察：t=371.8 s 有 `transport_base: tcp_read error, errno=Software caused connection abort`，那是**小智协议**那条连接（`api.tenclass.net`）的，bemfa 这条统计同时仍是 `连接=是`。记录在此以免以后误判成上报侧问题

### 4.3 开机回填与游标持久化（✅ 机制实测有效）

```
I (1900) BemfaClient: 开机回填 143 条未上报事件（sent_seq=0，本次 boot=2）
I (1900) BemfaClient: 开机回填 111 条未上报事件（sent_seq=1，本次 boot=15）
I (1870) BemfaClient: 开机回填  54 条未上报事件（sent_seq=6，本次 boot=18）
```

- `sent_seq` 落 NVS 生效（重启后从上次的序号继续，不是 0）
- `boot_id` 每次开机 +1（本轮跑到 `boot=18`），幂等键 = **device_id + boot_id + seq**（见 **BUG-037**）
- 回填源是 `events.log` 尾部 8 KB（`SnapshotStore::ReadEventsAfterSeq`），实测窗口 `8170 B 里 seq > 0 的有 143 条` —— 约 130~143 条的容量上限，写进 §7 局限

### 4.4 本轮定位/修复的上报侧缺陷（详见 docs/BUGS.md）

| 编号 | 一句话 |
|---|---|
| **BUG-036** | 设计文档 §7.1 的主题名带斜杠，在巴法云控制台根本建不出来（改用单级主题名） |
| **BUG-037** | 计划书 §9 的幂等键"设备 ID + 事件序号"重启后会误判重复（改为 + boot_id） |
| **BUG-038** | 首次连接几乎必然发生在"拿到 IP 之前"，只靠 esp-mqtt 自动重连要等 30~60 s（加 15 s 兜底重试） |
| **BUG-039** | **控制台每行有两行文字：粗体是"名称"，下面那行才是真正的 MQTT 主题值**；按名称发布不落地且**完全不报错** |

---

## 5. D6 验收项对照（计划书 §12）

| D6 验收项 | 结论 | 证据 / 说明 |
|---|---|---|
| 8 条命令词可识别并播报 | **✅（8/8 识别）**；播报 7/8 稳定，1 条曾间歇静默 | §3；`d6_task4_voice.log`（40 = 40）+ `d6_task4c_voice.log`（status 4/4） |
| 手机远程看到事件 | **✅** | `d6_task5c_uplink.log` + **用户确认控制台更新时间变化**（§4.2） |
| ≥500 条断网积压能力 | **⚠️ 仅主机单测** | `pending_queue_test` 覆盖满/丢最旧/回绕/截断；`BemfaClient` 缓冲 500×160 B 显式 PSRAM，**真机没有灌满 500 条**（未做断网实验） |
| 重启后补传 | **⚠️ 部分** | 机制已实测（§4.3 三次 `开机回填`，游标与 boot_id 都持久化）；但"断网期间造事件 → 断电重启 → 重连补传"这条**完整剧本未跑** |
| 三条链路并行 20 min | **❌ 未测** | 未做 20 min 并行负载实验。可用的一次连续运行只有 270 s（§4.2，零固件复位） |
| 断网时监测/预览/命令词/播报照常（计划书 §9） | **❌ 未测** | 未做断网窗口实验 |
| 手机端「有遗留」标记 → 「有没有人」播报 | **❌ 未验证** | `/leftover` 路由已实现（首页有两个链接），但 **PC 与板子不同网段**，HTTP 只能由手机验证；用户本轮未测 |
| 新上报统计 MCP 工具 `self.vehicle.net` | **⚠️ 已注册未调用** | 启动日志有 `MCP: Add tool: self.vehicle.net`；未逐个 `tools/call` |

---

## 6. D7 量化指标（**大部分未测**，如实列出）

| 指标（计划书 §1.1 / 设计文档 §10.3） | 目标 | 实测 | 说明 |
|---|---|---|---|
| MQTT 上报成功率（联网时） | ≥99% | **100%（73/73）** | 仅 400 s 窗口；口径是"交给 esp-mqtt"，不是端到端 ack（见 §4.2） |
| 离线命令词识别率 | ≥90%（安静环境） | **未按 8×10 协议测** | 只有临时记数（§3 表）。**且发现疑似误触发**：`车内湿度` 命中 14 次、`车内温度` 9 次，远多于用户实际说的遍数；命中概率普遍偏低（0.20~0.48，少数 0.76），与 `CONFIG_CUSTOM_WAKE_WORD_THRESHOLD=20` 偏低一致 |
| 全功能并行无复位 | 2 h | **未测 2 h** | 270 s 窗口零固件复位；但用户测试期间抓到 **BUG-006 的 `abort()` 复位循环 4 次**（`lvgl_port_touchpad_read` → `ESP_ERROR_CHECK(esp_lcd_touch_read_data)`），用户已决定不修 |
| 内部 RAM 余量 | ≥10 KB 且无分配失败 | **✅ 19,179 ~ 21,915 B**（改前 6,167） | 靠把 `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` 2048 → 512 换来的，见 **BUG-041**。**口径缺口**：旧版 6167 B 那个极小值出现在一次**小智对话**里，而本轮复测抓取在 30 s 时被别的进程抢走串口（`Access to the port 'COM13' is denied`），**没覆盖到对话窗口**，所以"对话路径下的新低水位"只有估算（≈25 KB），没有实测 |
| 预览帧率 | ≥10 fps | **未测** | 本轮未在负载下取 `VehicleUI: 预览实测 x.x fps` |
| 碰撞事件准确率 | 不做手持实测 | 主机单测覆盖 | `driving_monitor_test` 既有 |

> **口径说明（必须写清）**：`已发` 是"成功交给 esp-mqtt"（QoS1 已入 outbox），**不是**"巴法云端已确认收到"。
> 依据：`managed_components/78__esp-ml307/src/esp/esp_mqtt.cc` 没有把 `MQTT_EVENT_PUBLISHED` 暴露出来，拿不到 ack 计数。

---

## 7. 局限（不许含糊过去的部分）

1. **「设备状态」播报曾间歇静默（BUG-040）**：同一份逻辑、同一块板，旧固件 2/2 静默、新固件 4/4 正常。已排除片段/格式/解包/队列，**根因未定性**。逐段诊断日志已保留，下次复现能直接定位到是"没交出去"还是"交了没响"
2. ~~**内部 RAM 低水位 6167 B，低于 D7 目标 10 KB**~~ **已在收尾时解决**：`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` 2048 → 512（**BUG-041**），系统初始化后可用内部 RAM **19.3 → 38.3 KB**、低水位 **6167 → 19,179~21,915 B**。逐点差分证明我们自己的代码只花 ~750 B（80 KB 队列与 6 KB 栈都真在 PSRAM），7.8 KB 全是一个 esp-mqtt 客户端的定值开销
   - **遗留缺口**：旧版 6167 B 是在**一次小智对话**中掉的，本轮复测没覆盖对话窗口（抓取被抢端口中断），所以"对话路径下的新低水位"尚未实测
3. **跨重启补传上限约 130~143 条**：只读 `events.log` 尾部 8 KB（内部堆只有 ~20 KB，读不了整份 256 KB 日志）。断电期间超过这个量就会漏
4. **「有遗留」标记只存 RAM**：HTTP 任务栈在 PSRAM，写 NVS 会命中 `assert(esp_task_stack_is_sane_cache_disabled())`（BUG-024/026），所以重启后标记复位。这是**有意取舍**，不是遗漏
5. **BUG-006 仍然存在**：用户测试期间抓到 4 次 `abort()` 复位循环（触摸 I2C 一次 NACK → `ESP_ERROR_CHECK` → abort）。用户已决定不修，但**任何需要"长时间无人值守运行"的验收都会被它打断**，D7 的 2 h 测量必须先解决或至少接受这一点
6. **BUG-028 未修**：小智拍照上传在**有线局域网**路径上仍会卡死（手机热点正常）。本轮所有验收都在热点上做
7. **HTTP 三个路由的界面验收未做**：PC 与板子不同网段，`/`（含新增的「标记有遗留 / 清除标记」两个链接）、`/leftover`、`/latest.jpg`、`/events` 只能由手机浏览器验证
8. **命令词可能误触发**：见 §6 识别率一行的分析；建议下一轮把 `CONFIG_CUSTOM_WAKE_WORD_THRESHOLD` 从 20 调到 30 并按 8×10 协议重测（计划任务 1 步骤 13 的试法 B）

---

## 8. 本轮改动的文件（尚未提交）

**新增（`main/vehicle/`，纯逻辑 + 主机测试）**
`voice_intent.{h,cc}`、`voice_reply.{h,cc}`、`pending_queue.{h,cc}`；测试 `voice_intent_test.cc`、`voice_reply_test.cc`、`pending_queue_test.cc`、`report_json_test.cc`

**新增（板级）**
`voice_command.{h,cc}`、`bemfa_client.{h,cc}`、`boards/esp32s3/voice_commands.json`

**新增（构建期生成 / 脚本）**
`build/bemfa_secrets.h`（构建目录，gitignored，从 `.env` 生成）、`build/capture_serial.ps1`（带 `-Port/-Seconds/-Out/-Reset`、开端口重试）

**修改**
`main/CMakeLists.txt`（`--vehicle_commands` 透传、`bemfa_secrets.h` 生成 + `INCLUDE_DIRS`、`SOURCES` 三个新 `.cc`）、`scripts/build_default_assets.py`（`--vehicle_commands`）、`sdkconfig.defaults.esp32s3` + `sdkconfig`（切 `USE_CUSTOM_WAKE_WORD` + `SR_MN_CN_MULTINET7_QUANT`）、`main/audio/wake_words/custom_wake_word.{h,cc}`（命令词派发，不停 `running_`）、`main/audio/audio_service.{h,cc}`（`SetVoiceCommandCallback` + 接到 `CustomWakeWord`）、`main/vehicle/event_json.{h,cc}`（上报变体 + `AddReportFields` + `EnvToJson`/`StatusToJson`/`MotionStateId`）、`main/boards/esp32s3/config.h`（三个主题值 + broker）、`camera_capture.{h,cc}`（`SetCaptureDoneCallback`）、`vehicle_http.{h,cc}`（`/leftover` + 首页链接）、`vehicle_service.{h,cc}`（`WorkerTickable`/`SetCommandExecutor`、`kMaxSinks` 4→6 + 溢出告警）、`snapshot_store.{h,cc}`（`ReadEventsAfterSeq`）、`esp32s3_board.cc`（接线 + `self.vehicle.net`）、`docs/BUGS.md`（BUG-036~040）

---

## 9. 下一轮要做的（按优先级）

1. ~~**内部 RAM**：把低水位从 6167 B 拉回 ≥10 KB~~ **已完成（BUG-041）**：`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` 2048 → 512，+19 KB。**剩下的活**：补测"一次完整小智对话"期间的低水位（本轮抓取被抢端口，没覆盖到），把那个估算值变成实测值
2. **命令词阈值**：`CONFIG_CUSTOM_WAKE_WORD_THRESHOLD=30` 后按 8×10 协议重测识别率与误触发
3. **BUG-006**：如果 D7 要做 2 h 无人值守，触摸 NACK 必须不再 abort（最小改动是把 `lvgl_port_touchpad_read` 里的 `ESP_ERROR_CHECK` 降级为"打一条告警并跳过这一帧"——但那是 `managed_components` 里的上游代码）
4. **断网补传完整剧本**：关热点 → 造 ≥10 次事件 → 看 `积压` 增长 → 开热点 → 里 1 min 内归零并在控制台看到
5. **手机侧 HTTP 与遗留标记**：`http://<IP>:8080/` → 标记 → 说「有没有人」→ 播 `q_pending` → 清除 → 播 `q_no_alert`
6. **`self.vehicle.net` 工具**：逐个 `tools/call` 一遍
