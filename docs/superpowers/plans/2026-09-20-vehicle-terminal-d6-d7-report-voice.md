# 车载终端 D6–D7：巴法云 MQTT 上报 + 离线命令词与播报 + 联调验收 实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 把计划书 D6–D7 做完——巴法云 MQTT 事件/环境/状态上报与断网补传、离线命令词（MultiNet 8 条）与预录片段播报、D6 真机联调与 D7 量化指标测量 + 操作文档。

**架构：** 继续 Plan A/B 的分层：**纯逻辑进 `main/vehicle/`**（不得 include 任何 ESP-IDF 头，主机 g++ 可测），**设备侧接线进 `main/boards/esp32s3/`**。上报侧与命令词侧各加一个消费者/生产者，都挂在既有骨架上：  
- **上报**：`BemfaClient` 实现 `EventSink`（事件由既有 `worker_task` 投递）+ 自建 `net_task`（栈在 **PSRAM**、周期上报与补传）。网络客户端复用仓内 `Mqtt` 抽象（`managed_components/78__esp-ml307`，内部就是 esp-mqtt），不新增依赖、不自建 MQTT 栈。  
- **语音**：`CustomWakeWord`（上游 MN 路径）派发非 `wake` 的 `action` → `AudioService` 转发 → `VoiceCommand`（板级）查 `VoiceIntent` 表 → 读最新值/执行动作 → 按 `ClipId` 序列播放 `Lang::Sounds::OGG_*` 片段。命令词**只在待机态生效**（上游 MN 只在 `kDeviceStateIdle` 运行，本项目不改状态机）。  
- **硬纪律**：任何可能碰 flash（SPIFFS/NVS/`esp_partition_*`）的代码**不能跑在 PSRAM 栈上**（BUG-024/026）；自己申请的缓冲（上报队列 80 KB）一律显式 `MALLOC_CAP_SPIRAM`（BUG-024 的补充：内部通用堆只有 22 KB、空载就 99.7% 满）。

**技术栈：** ESP-IDF v5.5.3、C++17、FreeRTOS、ESP-SR MultiNet（`mn7_cn`）、esp-mqtt（经 `Mqtt` 抽象）、NVS（`Settings`）、LVGL 9.4、主机侧 g++（MinGW `C:\mingw64\bin`）跑纯逻辑测试。

**上游文档：** 设计文档 [`docs/superpowers/specs/2026-09-16-vehicle-terminal-design.md`](../specs/2026-09-16-vehicle-terminal-design.md)（§6 语音、§7 网络，口径以它为准，本计划对它的偏离见下）、计划书 [`docs/计划书.md`](../../计划书.md) §7/§9/§12、上一份计划 [`2026-09-17-vehicle-terminal-d3-d5-env-ui-snapshot.md`](2026-09-17-vehicle-terminal-d3-d5-env-ui-snapshot.md)、上一份交接 [`docs/handoff/2026-09-20-planB-closed-handoff.md`](../../handoff/2026-09-20-planB-closed-handoff.md)。

**可复用的参考实现（用户已有，本轮已核对过）**：`D:\AAA_Game_XueXiBan\KeilCode\STM32Project\18_Integrated\bsp\esp8266_mqtt.{c,h}` —— STM32F103 + ESP8266（AT 透传）上**手写 MQTT 3.1.1 报文**连巴法云的实现，**同一个账号并且跑通过**。

| 可以复用 | 不能复用 |
|---|---|
| 连接参数：`client_id = username = UID`、`password = secretKey`、keepalive 60 s、clean session、无 will（`esp8266_mqtt.c:336-382`） | AT 指令交互与手写报文打包（`esp8266_mqtt.c` 全文 712 行）——ESP32 侧用 IDF 自带 esp-mqtt（经仓内 `Mqtt` 抽象），重连与 keepalive 都是库的事 |
| 主题名风格：控制台生成的名字，后三位是设备类型码（它用 4 个 `…004` 上行 + 1 个 `…012` 下行） | 它的 payload 风格 `key=value`（`temp=25.6&humi=58.3`）——**我们要 JSON**（设计文档 §7.1）；但这条证明了**巴法云不校验 payload 格式** |
| 端点候选：`mqttv2.bemfa.com:2023`（它跑通的这一组） | 它的心跳调度（`mqtt_send_heart_async`）——esp-mqtt 内部会发 PINGREQ |
| 下行思路：订阅一个主题收控制指令（我们本轮**不做**，已记入任务 9 的待办） | —— |

（同源工程 `11_HALWIFI_Simple` 是它移植前的出处，见 `18_Integrated/docs/05-移植清单.md`。）

---

## 代码风格与纪律（每个任务都适用）

1. **不自动 `git commit`**：每个任务的"提交"步骤实际是 `git add <文件>` → 展示 `git status --short` 与 `git diff --cached --stat` → **等用户确认后**再 `git commit`。commit message 用简洁中文。
2. **注释用中文**；Better Comments 标签（`// !` 警告 / `// >` 要点 / `// ?` 待确认 / `// todo`）**只能写在 `//` 行注释里**，不要写进 `/* */`（写了不着色）。
3. `main/vehicle/` 下**不得 include 任何 ESP-IDF / FreeRTOS 头**（`<cstdint>`/`<string>`/`<vector>`/`<cmath>` 可以）。设备侧代码不得把判定逻辑复制一份到板级目录。
4. 板级源文件一律 `.cc`（CMake 只 glob `*.cc`）。**新增/删除任何 `main/vehicle/*.cc` 或 `main/boards/esp32s3/*.cc` 后必须 `idf.py reconfigure`**（glob 无 `CONFIGURE_DEPENDS`）。
5. **`main/vehicle/` 逻辑改完先跑主机测试再烧板**；跑 exe 需要 `C:\mingw64\bin` 在 PATH 上。
6. **日志里禁止 `%lld` / `%llu`**：本工程是 `CONFIG_NEWLIB_NANO_FORMAT=y`，nano 版 vfprintf 不支持 64 位整数格式，参数会错位并崩在 `memchr`（BUG-001）。64 位数字用 `%d` 转 `int`、`%.3f` 转 `double`，或直接用 `vehicle::ToDecimal()`。
7. 板级代码**零 warning**；未被使用的私有成员要删掉（`-Wunused-private-field`）。
8. **密钥纪律**：巴法云 AppID / SecretKey 只在仓库根 `.env`（已被 `.gitignore` 忽略）。**不得**写死进任何 `.cc`，**不得**拷进 `docs/`、`main/` 下的 tracked 文件。本计划的做法是构建期由 `main/CMakeLists.txt` 从 `.env` 生成 **构建目录里的** `bemfa_secrets.h`（`build/` 已被 gitignore）。
9. **每定位到一个根因，按 `docs/BUGS.md` 的格式追加一条**（含 `文件:行号` 与实测数值）。本计划已经预告了三条要记的（见任务 1/5/6 的"记录"步骤）。
10. **一次只动一个变量**：本轮最大的教训是"开着串口就失败"其实是"换了网络"（BUG-034）。动手前先问"和上一次比，环境有什么变化"。

**命令备忘：**

```
# 主机测试（PowerShell，先 $env:PATH="C:\mingw64\bin;$env:PATH"）
g++ -std=c++17 -Wall -Wextra -I main/vehicle test/<name>.cc main/vehicle/<impl>.cc -o build_host/<name>.exe
build_host\<name>.exe

# 固件构建（pwsh 里用 cmd /c，不是 cmd //c）
cmd /c "set MSYSTEM=&& set IDF_TOOLS_PATH=D:\AAA_Game_XueXiBan\Espressif\tools&& set PATH=D:\AAA_Game_XueXiBan\Espressif\tools\idf-python\3.11.2;%PATH%&& call D:\AAA_Game_XueXiBan\Espressif\frameworks\esp-idf-v5.5.3\export.bat && cd /d D:\vscode\ESP32Project\13ProjFace && idf.py build > build\last_build.log 2>&1"

# 烧录（会连 assets 分区一起写：MN 模型在 assets 里，第一次必须整片烧）
cmd /c "... && idf.py -p COM10 flash > build\last_flash.log 2>&1"

# 崩溃地址解析
xtensa-esp32s3-elf-addr2line -pfiaC -e build\xiaozhi.elf <addr>...
```

- `idf.py`（含烧录、串口抓取）**必须放宽沙箱权限到 `danger-full-access`**，否则内部 `asyncio` 建命名管道报 `PermissionError: [WinError 5]`（BUG-008）。用户会批准，但**也会拒绝**——被拒时不要重试同一条命令。
- **别在抓串口的同时烧录**：串口被占用会报 `Could not open COM10 / PermissionError(13)`。先 `job_kill` 掉抓取任务。
- 板子卡在 WDT 空转时烧不进去（`stub` 阶段 `Write timeout`）：让用户**拔插一次 USB** 再烧，通常一次就过。
- 串口号在 **COM10 / COM13** 之间漂；抓取脚本用"只开一次端口、不重连"的写法（`build/capture_once.ps1` / `build/capture_long.ps1`），避免拉 RTS 复位把 I2C 钳死（BUG-005）。
- **改完 `sdkconfig` 必须 touch 它**：`(Get-Item sdkconfig).LastWriteTime = Get-Date`，否则 `idf.py` 不重新生成 `build/config/sdkconfig.h`——"以为改了其实编的还是旧配置"（BUG-024 补充里的操作陷阱）。改完回读 `build/config/sdkconfig.h` 确认。

---

## 环境与操作要点（开工前必读）

| # | 要点 | 依据 |
|---|---|---|
| 1 | 板子 IP 会随网络变（局域网 Windows ICS `192.168.137.x` / 手机热点 `192.168.202.x`），别硬编码，从串口 `Got IP:` 取 | 交接文档第 5 条 |
| 2 | 局域网看图页是 **`http://<IP>:8080/`**（不是 80），串口会把带端口的完整地址打出来 | BUG-035 |
| 3 | **BAD-028 仍未修**：小智拍照上传在**有线局域网**路径上会卡死（手机热点正常）。本计划的验收凡是涉及"小智拍照"的，一律用**热点**或只验证"我们的抓拍/上报"，别把它算成本次的回归 | BUG-028 |
| 4 | **BUG-006（触摸 I2C 一次 NACK → 整机 abort）用户已决定不修**：遇到就按 BUG-006 判定、记一笔、继续干活 | 交接文档 |
| 5 | 内部 RAM 腾不出来（四个旋钮都试过）；本计划新增的缓冲**全部显式 PSRAM**，并先量一次基线 | BUG-024 补充 |

---

## 前置事实核验（写代码之前先确认这 5 条，都是"假设"或"用户动作"）

- [ ] **F1（用户动作）巴法云控制台已建好 3 个主题**：主题名只允许**字母/数字**（不能带斜杠、不能带下划线以外的符号），且**必须先在控制台创建**才收得到消息（依据 [巴法云 MQTT 文档](https://cloud.bemfa.com/docs/src/mqtt.html) 与 [平台操作教程](https://cloud.bemfa.com/docs/src/index_guild.html)）。本计划用的默认名：`vtevt01`（事件）/ `vtenv01`（环境）/ `vtsta01`（状态）。**让用户确认这三个主题已在控制台建好**，否则手机端看不到任何东西（服务端不会自动建主题）。
  - 后三位不是 `001`~`013`，不会被巴法云归类成"灯泡/插座"这类语音设备类型，符合我们的用途（只做数据查看）。
  - 若要改成别的名字：改 `main/boards/esp32s3/config.h` 的三个宏（任务 5），并同步在控制台建同名主题。
- [ ] **F2 `.env` 里的凭据与认证口径**（**已核对，事实如下，不用再猜**）：`.env` 里 `BEMFA_APP_ID`（28 字符、`beid_` 开头）与 `BEMFA_SECRET_KEY`（32 字符）各一行。已用程序逐值比对确认：**这两个值分别与用户 STM32 工程 `18_Integrated/bsp/esp8266_mqtt.h` 里那组跑通的 `MQTT_CLIENTID` / `MQTT_USARNAME` / `MQTT_PASSWD` 完全一致**——同一个巴法云账号，所以认证口径照那份实现来：
  - `client_id = username = BEMFA_APP_ID`，`password = BEMFA_SECRET_KEY`，keepalive **60 s**、clean session、无 will（`esp8266_mqtt.c:359-361`，conn flags `0xC2`）
  - **不要把值打印到日志、贴进聊天或写进任何 tracked 文件**；核对时只看长度/前缀，不看值
  - ! **同一个 UID 同时只允许一个连接**：用户的 STM32 网关如果正在用这套凭据连巴法云，两边会**互相顶下线**。D6 联调前先确认那个工程没在跑。
  - 端点有两组候选（见任务 5 步骤 8 的 `config.h` 注释）：官方文档 `bemfa.com:9501`，用户跑通的 `mqttv2.bemfa.com:2023`
- [ ] **F3 内部 RAM 基线**：串口空闲时 `SystemInfo: free sram: … minimal sram: …` 一行，记下数字（对照 BUG-024 补充的"空载 min 15159 B / 重载 min 9303 B"）。
- [ ] **F4 assets 分区余量够放 `mn7_cn`**：分区 6,029,312 B，当前 `build/generated_assets.bin` 2,851,767 B，`mn7_cn` 约 2.67 MB → 余量约 0.48 MB。构建后回读一次实际体积确认没超。
- [ ] **F5 板子当前可用**：串口看到 `Got IP:` 与 `VehicleHttp: 手机浏览器打开：http://<IP>:8080/`，`curl http://<IP>:8080/` 有 HTML。断了先让用户拔插 USB。

---

## 文件结构

| 文件 | 动作 | 职责 |
|---|---|---|
| `main/boards/esp32s3/voice_commands.json` | 创建 | 命令词表**单一来源**（9 条：1 唤醒 + 8 命令，`{action, command, text}`） |
| `scripts/build_default_assets.py` | 修改 | 新增 `--vehicle_commands`，把命令表并进 `index.json` 的 `multinet_model.commands`（替换上游那条只有唤醒词的数组） |
| `main/CMakeLists.txt` | 修改 | 本板分支传 `--vehicle_commands`；从 `.env` 生成构建目录里的 `bemfa_secrets.h` 并加进 `INCLUDE_DIRS`（见任务 5） |
| `main/Kconfig.projbuild` | 不改 | 主题名等常量放板级 `config.h`，不为单一板型加 Kconfig |
| `sdkconfig.defaults.esp32s3` | 修改 | 切到 `USE_CUSTOM_WAKE_WORD` + `SR_MN_CN_MULTINET7_QUANT`，写入唤醒词/阈值 |
| `main/vehicle/voice_intent.{h,cc}` | 创建 | `VoiceIntent` 枚举 + `action` → intent 查表（纯逻辑） |
| `main/vehicle/voice_reply.{h,cc}` | 创建 | `ClipId` 枚举 + 数字中文读数拼接 + 各命令的应答片段序列（纯逻辑） |
| `main/vehicle/pending_queue.{h,cc}` | 创建 | **定长槽位环形队列**（上报积压用；缓冲由调用方给，设备侧给 PSRAM） |
| `main/vehicle/event_json.{h,cc}` | 修改 | 加 `MotionStateId()`、`EnvToJson()`、`StatusToJson()`、`ParseSeqFromJsonLine()`、`EventToJson()` 的上报变体（带 `dev`/`boot`） |
| `test/voice_intent_test.cc` | 创建 | JSON 命令表 ↔ C++ 意图表一致性（防漂移）+ 9 条齐全 |
| `test/voice_reply_test.cc` | 创建 | 数字读数拼接边界（0/9/10/11/19/20/48/99/100/负数）、光照档位、状态序列顺序 |
| `test/pending_queue_test.cc` | 创建 | 环形队列满/空/覆盖语义 + 超长 payload 截断 |
| `test/report_json_test.cc` | 创建 | 上报 JSON 字段与顺序、`ParseSeqFromJsonLine` 边界 |
| `main/audio/wake_words/custom_wake_word.{h,cc}` | 修改 | 非 `wake` 的 `action` 派发出去，**不停止** `running_`（继续听下一条命令） |
| `main/audio/audio_service.{h,cc}` | 修改 | `SetVoiceCommandCallback()` + 在 `SetModelsList()` 里把它接到 `CustomWakeWord`（约 15 行） |
| `main/boards/esp32s3/voice_command.{h,cc}` | 创建 | 命令执行 + 播报（`ClipId` → `Lang::Sounds`）、事件播报、手机端"有遗留"标记（RAM） |
| `main/boards/esp32s3/bemfa_client.{h,cc}` | 创建 | 巴法云 MQTT：`net_task`、事件队列、断网补传、周期上报、上报统计 |
| `main/boards/esp32s3/snapshot_store.{h,cc}` | 修改 | 新增 `ReadEventsAfterSeq()`（开机回填未上报事件，只读日志尾部 8 KB） |
| `main/boards/esp32s3/camera_capture.{h,cc}` | 修改 | 新增 `SetCaptureDoneCallback()`（抓拍落盘后回调 → 播 `snap_done`） |
| `main/boards/esp32s3/vehicle_http.{h,cc}` | 修改 | 新增 `GET /leftover?value=0|1`（手机端标记"有遗留"），首页加两个链接 |
| `main/boards/esp32s3/config.h` | 修改 | 巴法云三个主题名 + broker 地址/端口（两组候选，见任务 5 步骤 8） |
| `main/boards/esp32s3/vehicle_service.h` | 修改 | `kMaxSinks` 4 → 6，并在溢出时 `ESP_LOGW`（现在是**静默忽略**，见任务 5 步骤 1） |
| `main/boards/esp32s3/esp32s3_board.cc` | 修改 | 接线 `VoiceCommand` / `BemfaClient`；注册语音命令回调；新增 MCP 工具 `self.vehicle.net` |
| `docs/操作文档.md` | 创建 | D7 交付物：通电、页面、命令词表、阈值、联网与上报、故障排查 |
| `docs/验收记录/D6-D7-MQTT与语音与联调.md` | 创建 | D6/D7 验收数据（任务 8/9/10 写） |
| `build/capture_long.ps1`、`build/count_vc.ps1` | 创建（gitignored） | 2 h 稳定性抓取、命令词命中计数（任务 9） |

**本计划相对设计文档/计划书的偏离（复核时请重点看这几条）：**

| # | 设计文档怎么写 | 本计划怎么做 | 理由 |
|---|---|---|---|
| 1 | §7.1 主题 `vehicle/{device_id}/event`、`/env`、`/status` | **单级主题名**（`vtevt01` / `vtenv01` / `vtsta01`，放 `config.h`），`device_id` 放在 payload 里 | 巴法云主题名只允许字母/数字，**带斜杠的主题在控制台建不出来**；而且必须先在控制台建好才收得到消息。这是设计文档写错了，要记 BUG |
| 2 | §7.1 payload 用 `"ts":1758000000`（unix 秒） | 用 `"ts_ms"`（开机以来的毫秒）+ `"boot"`（开机序号） | 设备没有 RTC（与 Plan B 同一条偏离：D5 已把 `ts` 改成 `ts_ms`）；unix 时间要等 NTP，本计划不做 |
| 3 | §7.1/§5.3 幂等键 = `device_id + seq` | 幂等键 = `device_id + boot_id + seq` | `EventHistory` 的 `seq` **每次重启从 1 重新开始**（它在 RAM 里），只用 `device_id+seq` 会把不同开机的同号事件判成同一条。"无重复补传"这条验收项按设计文档的口径根本成立不了 |
| 4 | §3.1 单列 `net_task`（6 KB 栈，PSRAM） | **照做**：自建 `net_task`，栈在 PSRAM，且**这个任务一次都不读 flash**（游标落 NVS 由 worker 任务代劳，见任务 6） | 与设计一致；"不读 flash"是 BUG-024/026 的硬约束 |
| 5 | §3.3 单列 `voice_command.{h,cc}`（放在板级） | 照做，但**纯逻辑部分**（intent 表、片段序列）拆到 `main/vehicle/voice_intent.*` 与 `voice_reply.*` | 板级代码没法主机单测；拆出来后"数字怎么读、状态播几段"可以秒级验证 |
| 6 | §6.3 "播报+上屏" | **只播报 + 串口日志**，不为语音应答新增 LVGL 浮层 | 主页的温度/湿度/光照/状态本来就每秒刷新，语音答案的内容屏上已有；新增浮层要动 `vehicle_ui.cc` 的 24 KB 界面代码与 lv_timer，收益不抵风险 |
| 7 | §6.4 光照"播报档位" | 照做（5 个档位片段），数值仍上屏 | — |
| 8 | 计划书 §7「有没有人」= 读"手机端是否标记有遗留" | 落地为 `GET /leftover?value=1` 由手机浏览器点击标记，**标记只存 RAM**（重启复位） | 设计要求的接口 Plan B 没做；但 HTTP 任务栈在 PSRAM、**不能写 NVS**（BUG-024/026），所以不做持久化。局限如实写进验收记录 |
| 9 | §6.1 "切到 MN 后 `wn9` 仍留在模型包里" | `wn9` **会被换掉**（模型包里只有 `mn7_cn`） | `scripts/build_default_assets.py:853-856` 只在 ESP/AFE 唤醒词下打包 wakenet。结论无实际影响（`AfeAudioProcessor` 的 NS/VAD 本来就没打包，`afe_config_init()` 传的 wakenet 是 NULL），但文档这句是错的，要记 BUG |
| 10 | §6.2 命令表在 `index.json` 里，改词表不用重训模型 | 照做（`--vehicle_commands`） | — |
| 11 | §6.4 播报覆盖"上电标定/上电就绪"（`calib_start` / `calib_done` / `ready` 三条片段） | **本轮不接**：开机标定完成时**不播报**，只保留串口日志 `静止基线标定完成` | 开播报要先在 `imu_task`（20 ms、优先级 5）判定状态跳变、再把播报挪到 worker，属于"上电体验"的锦上添花，不在 D6 验收项里（§12 D6 = "8 条命令词可识别并播报"）。三条片段留在 `main/assets/common/`，未被引用会被 `--gc-sections` 丢掉（app 体积不涨），要接时在 `VoiceCommand::Play()` 里直接用即可；已记入任务 9 步骤 3 的待办 |
| 12 | §6.4 `d100.ogg` 未使用 | 照做（转码脚本已跳过它） | — |

---

## 任务 1：命令表单一来源 + MultiNet 命令词生效

**交付物：** 9 条命令词被 MN 认识，说出「车内温度」这类命令时串口打印 `Custom wake word detected: … string=…` 与 `CustomWakeWord: Voice command …`（此时还没有业务动作）。**唤醒词换成 MN 拼音识别**是本任务最大的风险点，要在这一步就验掉。

**文件：**
- 创建：`main/boards/esp32s3/voice_commands.json`
- 创建：`main/vehicle/voice_intent.h`、`main/vehicle/voice_intent.cc`
- 创建：`test/voice_intent_test.cc`
- 修改：`scripts/build_default_assets.py:811-912`（argparse + `multinet_model_info`）
- 修改：`main/CMakeLists.txt:102-108`（本板分支）与 `:1019-1056`（`build_default_assets_bin()`）
- 修改：`main/CMakeLists.txt:46-51`（`SOURCES` 加 `vehicle/voice_intent.cc`）
- 修改：`sdkconfig.defaults.esp32s3`

- [ ] **步骤 1：写命令表 JSON**

创建 `main/boards/esp32s3/voice_commands.json`（**只允许 ASCII 拼音与中文 `text`**；拼音风格与上游默认值 `xiao tu dou` 一致：小写、空格分隔、不带声调）：

```json
[
  { "action": "wake",      "command": "ni hao xiao zhi",     "text": "你好小智" },
  { "action": "temp",      "command": "che nei wen du",      "text": "车内温度" },
  { "action": "humid",     "command": "che nei shi du",      "text": "车内湿度" },
  { "action": "light",     "command": "guang zhao duo shao", "text": "光照多少" },
  { "action": "occupancy", "command": "you mei you ren",     "text": "有没有人" },
  { "action": "status",    "command": "she bei zhuang tai",  "text": "设备状态" },
  { "action": "chime",     "command": "bo fang ti shi yin",  "text": "播放提示音" },
  { "action": "snapshot",  "command": "chong xin zhua pai",  "text": "重新抓拍" },
  { "action": "lock",      "command": "suo che",             "text": "锁车" }
]
```

> 唤醒词必须叫 `wake`：`custom_wake_word.cc:179` 靠 `action == "wake"` 区分"唤醒"与"命令词"，上游逻辑不改。

- [ ] **步骤 2：给构建脚本加 `--vehicle_commands`**

修改 `scripts/build_default_assets.py`。先在 `main()` 的 argparse 里加参数（紧跟 `--extra_files`，`:819` 之后）：

```python
    parser.add_argument('--vehicle_commands', help='Path to vehicle command table JSON (replaces the single wake command)')
```

再在 `multinet_model_info` 组装完之后（`:908` 的 `]` 与 `}` 之后、`:909` 的 `print` 之前）插入：

```python
        # > 本项目：命令表是 JSON 单一来源（9 条），构建期并进 index.json；
        # > 运行期 CustomWakeWord 从 index.json 读回并逐条 esp_mn_commands_add()，
        # > 所以改词表**不需要**重新训练模型（设计文档 §6.2）。
        if args.vehicle_commands:
            with io.open(args.vehicle_commands, "r", encoding="utf-8") as f:
                commands = json.load(f)
            actions = [c.get("action") for c in commands]
            if "wake" not in actions:
                print("Error: vehicle command table must contain an entry with action=wake")
                sys.exit(1)
            if len(actions) != len(set(actions)):
                print(f"Error: duplicated action in vehicle command table: {actions}")
                sys.exit(1)
            multinet_model_info["commands"] = commands
            print(f"  vehicle commands: {len(commands)} 条（{', '.join(actions)}）")
```

- [ ] **步骤 3：CMake 接线**

修改 `main/CMakeLists.txt` 的本板分支（`:102`），加一行：

```cmake
elseif(CONFIG_BOARD_TYPE_VEHICLE_ESP32S3)	
    # > 唯一板型标识：与上游任何板型都不重名，供服务端与 OTA 识别本自定义固件
    set(BOARD_NAME "VEHICLE-ESP32S3-TERM")
    set(BOARD_TYPE "esp32s3")	    #板型代码的文件夹
    set(BUILTIN_TEXT_FONT font_puhui_basic_30_4)	#lvgl显示字体大小
    set(BUILTIN_ICON_FONT font_awesome_30_4)		#lvgl显示字体大小
    set(DEFAULT_EMOJI_COLLECTION twemoji_64)
    # > 命令词表（9 条）：与运行期共用同一份 JSON，避免两处漂移
    set(VEHICLE_COMMANDS_JSON ${CMAKE_CURRENT_SOURCE_DIR}/boards/esp32s3/voice_commands.json)
```

在 `build_default_assets_bin()` 里加参数与依赖（`:1044` 之后加参数、`:1051-1053` 的 `DEPENDS` 里加文件）：

```cmake
    list(APPEND BUILD_ARGS "--esp_sr_model_path" "${ESP_SR_MODEL_PATH}")
    list(APPEND BUILD_ARGS "--xiaozhi_fonts_path" "${XIAOZHI_FONTS_PATH}")

    # > 本板的命令词表：改了它必须重新生成 assets.bin（否则 MN 只认唤醒词）
    if(VEHICLE_COMMANDS_JSON AND EXISTS ${VEHICLE_COMMANDS_JSON})
        list(APPEND BUILD_ARGS "--vehicle_commands" "${VEHICLE_COMMANDS_JSON}")
    endif()

    # Create custom command to build assets
    add_custom_command(
        OUTPUT ${GENERATED_ASSETS_BIN}
        COMMAND python ${PROJECT_DIR}/scripts/build_default_assets.py ${BUILD_ARGS}
        DEPENDS
            ${SDKCONFIG}
            ${PROJECT_DIR}/scripts/build_default_assets.py
            ${VEHICLE_COMMANDS_JSON}
        COMMENT "Building default assets.bin based on configuration"
        VERBATIM
    )
```

并在 `main/CMakeLists.txt:46-51` 的 `SOURCES` 列表里加上 `"vehicle/voice_intent.cc"`：

```cmake
    "vehicle/driving_monitor.cc"
    "vehicle/event_history.cc"
    "vehicle/environment_sensor.cc"
    "vehicle/event_json.cc"
    "vehicle/event_text.cc"
    "vehicle/snapshot_ring.cc"
    "vehicle/voice_intent.cc"
```

- [ ] **步骤 4：写失败的测试（意图表）**

创建 `test/voice_intent_test.cc`：

```cpp
// 命令表 ↔ C++ 意图表一致性测试（防两处漂移）
//
// 编译与运行：
//   g++ -std=c++17 -Wall -Wextra -I main/vehicle test/voice_intent_test.cc main/vehicle/voice_intent.cc -o build_host/voice_intent_test.exe
//   build_host\voice_intent_test.exe
//
// ! 测试在**仓库根目录**下运行（命令备忘里的写法），所以用相对路径读 JSON。

#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "voice_intent.h"

using namespace vehicle;

static int g_failures = 0;

#define CHECK(cond, what)                                     \
    do {                                                      \
        if (cond) {                                           \
            printf("  ok   %s\n", what);                      \
        } else {                                              \
            printf("  FAIL %s\n", what);                      \
            g_failures++;                                     \
        }                                                     \
    } while (0)

// > 只认我们自己写的这份 JSON 的固定写法（`"action": "xxx"`），不做通用 JSON 解析：
// > 主机测试不引入 cJSON，少一个跨平台依赖。JSON 格式变了这里会直接读不到而报错。
static std::vector<std::string> ExtractActions(const std::string &text) {
    std::vector<std::string> actions;
    const std::string key = "\"action\"";
    size_t pos = 0;
    while ((pos = text.find(key, pos)) != std::string::npos) {
        const size_t colon = text.find(':', pos + key.size());
        if (colon == std::string::npos) break;
        const size_t open = text.find('"', colon);
        if (open == std::string::npos) break;
        const size_t close = text.find('"', open + 1);
        if (close == std::string::npos) break;
        actions.push_back(text.substr(open + 1, close - open - 1));
        pos = close + 1;
    }
    return actions;
}

int main() {
    printf("voice_intent\n");

    std::ifstream file("main/boards/esp32s3/voice_commands.json");
    if (!file) {
        printf("  FAIL 打不开 main/boards/esp32s3/voice_commands.json（必须在仓库根目录运行）\n");
        return 1;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::vector<std::string> actions = ExtractActions(buffer.str());

    CHECK(actions.size() == 9, "命令表 9 条（1 唤醒 + 8 命令）");
    CHECK(actions.empty() || actions[0] == "wake", "第一条必须是 wake");

    std::set<VoiceIntent> seen;
    for (const std::string &action : actions) {
        const VoiceIntent intent = ParseVoiceAction(action);
        if (intent == VoiceIntent::kUnknown) {
            printf("  FAIL JSON action \"%s\" 没有 C++ 映射\n", action.c_str());
            g_failures++;
            continue;
        }
        if (!seen.insert(intent).second) {
            printf("  FAIL JSON action \"%s\" 与前面某条映射到了同一个 intent\n", action.c_str());
            g_failures++;
        }
    }

    // > 反向：C++ 表里的每个 intent（除 kUnknown/kCount）都必须能在 JSON 里找到 action，
    // > 否则就是"代码里有、模型永远认不出"的死意图。
    for (int i = 1; i < static_cast<int>(VoiceIntent::kCount); i++) {
        const VoiceIntent intent = static_cast<VoiceIntent>(i);
        bool found = false;
        for (const std::string &action : actions) {
            if (ParseVoiceAction(action) == intent) { found = true; break; }
        }
        if (!found) {
            printf("  FAIL intent %s 在 JSON 里没有对应 action\n", ToString(intent));
            g_failures++;
        }
    }

    CHECK(ParseVoiceAction("") == VoiceIntent::kUnknown, "空 action → kUnknown");
    CHECK(ParseVoiceAction("temp") == VoiceIntent::kTemp, "temp → kTemp");
    CHECK(ParseVoiceAction("lock") == VoiceIntent::kLock, "lock → kLock");
    CHECK(std::string(ToString(VoiceIntent::kOccupancy)) == "occupancy", "ToString(kOccupancy)");

    printf("\n%s（失败 %d）\n", g_failures == 0 ? "全部通过" : "有失败", g_failures);
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **步骤 5：运行测试确认失败**

```powershell
$env:PATH="C:\mingw64\bin;$env:PATH"
g++ -std=c++17 -Wall -Wextra -I main/vehicle test/voice_intent_test.cc main/vehicle/voice_intent.cc -o build_host/voice_intent_test.exe
```
预期：编译失败，报 `voice_intent.h: No such file or directory`。

- [ ] **步骤 6：写最少实现**

创建 `main/vehicle/voice_intent.h`：

```cpp
#pragma once

// 语音命令意图：action（来自 index.json / CustomWakeWord）→ 业务意图。
//
// 纯逻辑，不依赖 ESP-IDF，可用主机 g++ 单元测试（test/voice_intent_test.cc 会拿
// main/boards/esp32s3/voice_commands.json 与本表逐条对齐，防止两处漂移）。

#include <cstdint>
#include <string>

namespace vehicle {

enum class VoiceIntent : uint8_t {
    kUnknown = 0,
    kWake,        // "wake"：唤醒词本身，不是命令
    kTemp,        // "temp"      车内温度
    kHumid,       // "humid"     车内湿度
    kLight,       // "light"     光照多少
    kOccupancy,   // "occupancy" 有没有人
    kStatus,      // "status"    设备状态
    kChime,       // "chime"     播放提示音
    kSnapshot,    // "snapshot"  重新抓拍
    kLock,        // "lock"      锁车
    kCount,
};

const char *ToString(VoiceIntent intent);

// action → 意图；认不出的返回 kUnknown（调用方只告警，不做事）
VoiceIntent ParseVoiceAction(const std::string &action);

}  // namespace vehicle
```

创建 `main/vehicle/voice_intent.cc`：

```cpp
#include "voice_intent.h"

namespace vehicle {

namespace {

struct ActionEntry {
    const char *action;
    VoiceIntent intent;
};

// > 表的顺序与 voice_commands.json 一致，方便两条一起看。
constexpr ActionEntry kActions[] = {
    {"wake", VoiceIntent::kWake},           {"temp", VoiceIntent::kTemp},
    {"humid", VoiceIntent::kHumid},         {"light", VoiceIntent::kLight},
    {"occupancy", VoiceIntent::kOccupancy}, {"status", VoiceIntent::kStatus},
    {"chime", VoiceIntent::kChime},         {"snapshot", VoiceIntent::kSnapshot},
    {"lock", VoiceIntent::kLock},
};

}  // namespace

const char *ToString(VoiceIntent intent) {
    switch (intent) {
        case VoiceIntent::kWake: return "wake";
        case VoiceIntent::kTemp: return "temp";
        case VoiceIntent::kHumid: return "humid";
        case VoiceIntent::kLight: return "light";
        case VoiceIntent::kOccupancy: return "occupancy";
        case VoiceIntent::kStatus: return "status";
        case VoiceIntent::kChime: return "chime";
        case VoiceIntent::kSnapshot: return "snapshot";
        case VoiceIntent::kLock: return "lock";
        case VoiceIntent::kUnknown:
        case VoiceIntent::kCount:
            break;
    }
    return "unknown";
}

VoiceIntent ParseVoiceAction(const std::string &action) {
    for (const ActionEntry &entry : kActions) {
        if (action == entry.action) {
            return entry.intent;
        }
    }
    return VoiceIntent::kUnknown;
}

}  // namespace vehicle
```

- [ ] **步骤 7：运行测试确认通过**

```powershell
g++ -std=c++17 -Wall -Wextra -I main/vehicle test/voice_intent_test.cc main/vehicle/voice_intent.cc -o build_host/voice_intent_test.exe
build_host\voice_intent_test.exe
```
预期：全部 `ok`，末行 `全部通过（失败 0）`，退出码 0。

- [ ] **步骤 8：切 Kconfig 到 MultiNet**

修改 `sdkconfig.defaults.esp32s3`：把 `CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y` 这一段扩成下面这样（`CONFIG_SR_WN_*` 那行**保留**，理由写在注释里）：

```
# ---- 语音：离线命令词走 MultiNet（Plan C / D6）----
# > 与 CONFIG_USE_AFE_WAKE_WORD 是同一个 choice，切过去后唤醒词由 MN 拼音识别承担。
CONFIG_USE_CUSTOM_WAKE_WORD=y
CONFIG_CUSTOM_WAKE_WORD="ni hao xiao zhi"
CONFIG_CUSTOM_WAKE_WORD_DISPLAY="你好小智"
CONFIG_CUSTOM_WAKE_WORD_THRESHOLD=20
# > 打包进 assets/srmodels.bin（约 2.67 MB，assets 余量约 0.48 MB，够）
CONFIG_SR_MN_CN_MULTINET7_QUANT=y
# ! 这行在 USE_CUSTOM_WAKE_WORD 下**不生效**：build_default_assets.py:853-856 只在
# ! ESP/AFE 唤醒词下打包 wakenet，切到 MN 后模型包里只有 mn7_cn（设计文档 §6.1 那句
# ! "wn9 仍留在模型包里"是错的，见 docs/BUGS.md）。留着是为了回退时不用再改回来。
CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y
```

- [ ] **步骤 9：改完 `sdkconfig` 必须 touch 并回读确认**

```powershell
(Get-Item sdkconfig).LastWriteTime = Get-Date
cmd /c "... && idf.py reconfigure"        # 让 sdkconfig 由 defaults 重新生成
Select-String -Path build/config/sdkconfig.h -Pattern "USE_CUSTOM_WAKE_WORD|CUSTOM_WAKE_WORD|MULTINET7_QUANT"
```
预期：`build/config/sdkconfig.h` 里能看到
`#define CONFIG_USE_CUSTOM_WAKE_WORD 1`、`#define CONFIG_CUSTOM_WAKE_WORD "ni hao xiao zhi"`、
`#define CONFIG_SR_MN_CN_MULTINET7_QUANT 1`，且**没有** `CONFIG_USE_AFE_WAKE_WORD`。
（若 `menuconfig` 曾经手改过 `sdkconfig`，defaults 不会覆盖已有值——必要时删掉 `sdkconfig` 重新生成，注意它是 gitignored 的本地文件。）

- [ ] **步骤 10：重新配置 + 构建，验证命令表进了 assets**

```powershell
cmd /c "... && idf.py reconfigure && idf.py build > build\d6_task1_build.log 2>&1"
Select-String -Path build\d6_task1_build.log -Pattern "vehicle commands|multinet models|Generated default assets|Project build complete"
```
预期日志里同时出现：
- `vehicle commands: 9 条（wake, temp, humid, light, occupancy, status, chime, snapshot, lock）`
- `multinet models: mn7_cn (will be packaged)`
- `Note: Found wakenet models ... but wake word type is not ESP/AFE, skipping`（预期内的提示）
- `Project build complete`

- [ ] **步骤 11：整片烧录并验证"9 条命令词被 MN 认识"**

```powershell
cmd /c "... && idf.py -p COM10 flash > build\d6_task1_flash.log 2>&1"
# 抓串口（先确保没有别的抓取任务占着端口）
pwsh -File build\capture_once.ps1 -Seconds 60 -Out build\d6_task1_boot.log
Select-String -Path build\d6_task1_boot.log -Pattern "CustomWakeWord: Command:|Found multinet|srmodel|VehicleHttp: 手机浏览器打开"
```

预期：启动时打印 **9 行** `CustomWakeWord: Command: <pinyin>, Text: <中文>, Action: <action>`，且 `VehicleHttp` 的 8080 地址照常出现。

- [ ] **步骤 12（用户操作）：唤醒词与命令词真机验证**

让用户对板子依次说：
1. 「你好小智」→ 串口出现 `Custom wake word detected: command_id=1, …`，小智应答（对话链路不受影响）
2. 「车内温度」「车内湿度」「光照多少」「有没有人」「设备状态」「播放提示音」「重新抓拍」「锁车」各说 1 次
3. 说「你好小智」→ 等它回完 → 再说「车内温度」（**验证待机态之外不响应**：对话进行中 MN 不跑，命令词应当没反应）

抓取日志，核对：
```powershell
Select-String -Path build\d6_task1_cmd.log -Pattern "Custom wake word detected|Voice command"
```
预期：唤醒 1 次 + 8 条命令各命中一次；对话中说的那条**不**出现。

- [ ] **步骤 13：唤醒率不够时的兜底实验（只在步骤 12 里唤醒失败/误唤醒严重时做）**

MN 拼音识别弱于 wn9，若识别差，按下面的顺序试**一种**拼写（每次只改 `voice_commands.json` 里 `wake` 那一行的 `command` **和** `sdkconfig.defaults.esp32s3` 的 `CONFIG_CUSTOM_WAKE_WORD`，两处必须一致）：

| 试法 | `command` 取值 |
|---|---|
| A（默认） | `ni hao xiao zhi` |
| B | `ni hao xiao zhi` + `CONFIG_CUSTOM_WAKE_WORD_THRESHOLD=30`（阈值上调减少误触发） |
| C | `ni3 hao3 xiao3 zhi4`（带声调写法） |

每试一种：`idf.py build` → 烧录 → 说 10 次「你好小智」，统计命中次数，取最好的一种写回。**若三种都不可接受**（< 7/10），按设计文档 §11 的回退：改回 `CONFIG_USE_AFE_WAKE_WORD=y`（删掉 `USE_CUSTOM_WAKE_WORD`、`SR_MN_CN_MULTINET7_QUANT` 两行），**并如实记录"本次交付无离线命令词"**，任务 3/4 的语音部分改为待办——这条要写进验收记录的局限一节，不许含糊过去。

- [ ] **步骤 14：记录 BUG（本步骤必做）**

按 `docs/BUGS.md` 格式在**第六节（计划与验收口径缺陷）**末尾追加一条（编号接 `BUG-035` 之后，写成 `BUG-036`）：

```
### BUG-036 设计文档 §7.1 的 MQTT 主题名带斜杠，在巴法云上根本建不出来
- 现象/依据：巴法云控制台"创建主题"只接受字母或数字组合（[平台操作教程](https://cloud.bemfa.com/docs/src/index_guild.html)），
  且主题必须先在控制台创建才收得到消息；文档里 `vehicle/{device_id}/event` 这类多级主题无法创建
- 修法：改用单级主题名（boards/esp32s3/config.h 的 BEMFA_TOPIC_*），device_id 放 payload
- 另：设计文档 §6.1 说"切到 MN 后 wn9 仍留在模型包里"是错的——
  scripts/build_default_assets.py:853-856 只在 ESP/AFE 唤醒词下打包 wakenet
- 出处：Plan C 计划任务 1/5
```

- [ ] **步骤 15：展示并提交（等用户确认）**

```bash
git add main/boards/esp32s3/voice_commands.json main/vehicle/voice_intent.h main/vehicle/voice_intent.cc \
        test/voice_intent_test.cc scripts/build_default_assets.py main/CMakeLists.txt sdkconfig.defaults.esp32s3 \
        docs/BUGS.md
git status --short
git diff --cached --stat
```
建议 commit message：`feat: 命令表单一来源与 MultiNet 命令词生效（D6 任务 1）`

---

## 任务 2：应答片段组装（纯逻辑）

**交付物：** `voice_reply.{h,cc}` + 主机测试。决定"每个命令播哪几段、什么顺序"，但还不接音频（任务 4 接）。

**文件：**
- 创建：`main/vehicle/voice_reply.h`、`main/vehicle/voice_reply.cc`
- 创建：`test/voice_reply_test.cc`
- 修改：`main/CMakeLists.txt`（`SOURCES` 加 `vehicle/voice_reply.cc`）

- [ ] **步骤 1：写失败的测试**

创建 `test/voice_reply_test.cc`：

```cpp
// 语音应答片段组装的主机单元测试
//
// 编译与运行：
//   g++ -std=c++17 -Wall -Wextra -I main/vehicle test/voice_reply_test.cc main/vehicle/voice_reply.cc -o build_host/voice_reply_test.exe
//   build_host\voice_reply_test.exe

#include <cstdio>
#include <string>
#include <vector>

#include "voice_reply.h"

using namespace vehicle;

static int g_failures = 0;

#define CHECK(cond, what)                                     \
    do {                                                      \
        if (cond) {                                           \
            printf("  ok   %s\n", what);                      \
        } else {                                              \
            printf("  FAIL %s\n", what);                      \
            g_failures++;                                     \
        }                                                     \
    } while (0)

static std::string Join(const std::vector<ClipId> &clips) {
    std::string out;
    for (ClipId id : clips) {
        if (!out.empty()) out += " ";
        out += ToString(id);
    }
    return out;
}

int main() {
    printf("voice_reply\n");

    // 数字读数：0–9 单段；10–19 = d10 + 个位；20–99 = 十位 + d10 + 个位
    CHECK(Join(NumberToClips(0)) == "d0", "0 → d0");
    CHECK(Join(NumberToClips(7)) == "d7", "7 → d7");
    CHECK(Join(NumberToClips(10)) == "d10", "10 → d10（不拼 0）");
    CHECK(Join(NumberToClips(11)) == "d10 d1", "11 → d10 d1");
    CHECK(Join(NumberToClips(19)) == "d10 d9", "19 → d10 d9");
    CHECK(Join(NumberToClips(20)) == "d2 d10", "20 → d2 d10");
    CHECK(Join(NumberToClips(26)) == "d2 d10 d6", "26 → 二十六");
    CHECK(Join(NumberToClips(48)) == "d4 d10 d8", "48 → 四十八");
    CHECK(Join(NumberToClips(90)) == "d9 d10", "90 → d9 d10");
    CHECK(Join(NumberToClips(99)) == "d9 d10 d9", "99 → d9 d10 d9");
    // > 本项目用不到 >99（温度/湿度/事件数都在 0–99），但别让越界变成越界访问
    CHECK(Join(NumberToClips(100)) == "d1 d0 d0", "100 → 逐位兜底 d1 d0 d0");
    CHECK(Join(NumberToClips(123)) == "d1 d2 d3", "123 → 逐位兜底");
    // ! 没有"零下"片段：负温度只上屏、不播报（设计文档 §6.4）
    CHECK(NumberToClips(-3).empty(), "负数 → 空（只上屏）");

    // 温度/湿度：前缀 + 数字 + 单位
    CHECK(Join(TemperatureClips(26.4f, true)) == "q_temp d2 d10 d6 unit_degree", "26.4 ℃ → 车内温度 二十六 度");
    CHECK(Join(TemperatureClips(48.6f, true)) == "q_temp d4 d10 d9 unit_degree", "48.6 ℃ 四舍五入到 49");
    CHECK(Join(HumidityClips(48.0f, true)) == "q_humid d4 d10 d8", "湿度 48%（片段自带"百分之"）");
    CHECK(Join(TemperatureClips(26.0f, false)) == "q_temp", "读数无效 → 只播前缀");
    CHECK(Join(HumidityClips(48.0f, false)) == "q_humid", "读数无效 → 只播前缀");

    // 光照：播档位，不播数字
    CHECK(ToString(LightClip(5)) == "q_light_dark", "5 lux → 很暗");
    CHECK(ToString(LightClip(30)) == "q_light_dim", "30 lux → 偏暗");
    CHECK(ToString(LightClip(200)) == "q_light_mid", "200 lux → 适中");
    CHECK(ToString(LightClip(600)) == "q_light_bright", "600 lux → 明亮");
    CHECK(ToString(LightClip(2000)) == "q_light_strong", "2000 lux → 很强");

    // 设备状态：正常 + 联网/未联网 + 事件数 + 次
    CHECK(Join(StatusClips(true, 3)) == "q_status_ok q_online q_events d3 unit_times", "在线 3 次事件");
    CHECK(Join(StatusClips(false, 0)) == "q_status_ok q_offline q_events d0 unit_times", "未联网 0 次事件");

    // 有没有人：只读"手机端有遗留标记"这一个本地状态位
    CHECK(ToString(OccupancyClip(false)) == "q_no_alert", "无标记 → 未检测到异常");
    CHECK(ToString(OccupancyClip(true)) == "q_pending", "有标记 → 有遗留提醒待确认");

    // 事件播报：每类事件一段
    CHECK(ToString(EventClip(EventType::kHardBrake)) == "ev_hard_brake", "急刹车");
    CHECK(ToString(EventClip(EventType::kHardAccel)) == "ev_hard_accel", "急加速");
    CHECK(ToString(EventClip(EventType::kHardTurn)) == "ev_hard_turn", "急转弯");
    CHECK(ToString(EventClip(EventType::kBump)) == "ev_bump", "颠簸");
    CHECK(ToString(EventClip(EventType::kCrash)) == "ev_crash", "碰撞");
    CHECK(ToString(EventClip(EventType::kParked)) == "ev_parked", "已停车");
    CHECK(ToString(EventClip(EventType::kMoving)) == "ev_driving", "行驶中");
    CHECK(ToString(EventClip(EventType::kMotionWhileParked)) == "ev_motion_parked", "锁车期异常震动");
    CHECK(ToString(EventClip(EventType::kCount)) == "none", "未知事件 → none（不播）");

    printf("\n%s（失败 %d）\n", g_failures == 0 ? "全部通过" : "有失败", g_failures);
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **步骤 2：运行测试确认失败**

```powershell
g++ -std=c++17 -Wall -Wextra -I main/vehicle test/voice_reply_test.cc main/vehicle/voice_reply.cc -o build_host/voice_reply_test.exe
```
预期：编译失败，`voice_reply.h: No such file or directory`。

- [ ] **步骤 3：写实现**

创建 `main/vehicle/voice_reply.h`：

```cpp
#pragma once

// 语音应答的**片段序列**组装：只决定"播哪几段、什么顺序"，不管怎么发声。
// 设备侧 boards/esp32s3/voice_command.cc 负责 ClipId → Lang::Sounds::OGG_* 的映射。
//
// 拆出来的理由：片段顺序（二十六 = d2 d10 d6、状态 = 正常+联网+事件数+次）是纯逻辑，
// 放主机测试里秒级可验；播报本身只能真机听。纯逻辑，不依赖 ESP-IDF。

#include <cstdint>
#include <vector>

#include "environment_sensor.h"
#include "vehicle_types.h"

namespace vehicle {

// 片段 ID：与 main/assets/common/*.ogg 一一对应（文件名小写 → 枚举名大写）
enum class ClipId : uint8_t {
    kNone = 0,
    // 数字 0–9 与「十」
    kD0, kD1, kD2, kD3, kD4, kD5, kD6, kD7, kD8, kD9, kD10,
    // 单位
    kUnitDegree, kUnitTimes,
    // 查询应答前缀
    kQTemp, kQHumid, kQEvents, kQStatusOk, kQOnline, kQOffline, kQNoAlert, kQPending,
    // 光照档位
    kQLightDark, kQLightDim, kQLightMid, kQLightBright, kQLightStrong,
    // 动作提示
    kChime, kSnapStart, kSnapDone, kLockEntered, kLockExited,
    // 行车事件
    kEvHardAccel, kEvHardBrake, kEvHardTurn, kEvBump, kEvCrash, kEvParked, kEvDriving, kEvMotionParked,
    kCount,
};

// 枚举名（ASCII 小写下划线），用于串口日志与测试断言
const char *ToString(ClipId id);

// 0–99 按中文读数拼接：0–9 → d0…d9；10–19 → d10 + 个位（10 只发 d10）；
// 20–99 → 十位数字 + d10 + 个位。>99 逐位兜底。**负数返回空**（没有"零下"片段）。
std::vector<ClipId> NumberToClips(int value);

// 事件 → 一段播报（没有对应片段的类型返回 kNone）
ClipId EventClip(EventType type);

// 「有没有人」：pending = 手机端标记"有遗留"（本机 RAM 状态位，见 voice_command.cc）
ClipId OccupancyClip(bool pending);

// 查询应答的完整序列（valid=false 时只播前缀，表示这一帧读数不可用）
std::vector<ClipId> TemperatureClips(float temp_c, bool valid);
std::vector<ClipId> HumidityClips(float humidity_pct, bool valid);
ClipId LightClip(int32_t lux);
std::vector<ClipId> StatusClips(bool online, int32_t events_total);

}  // namespace vehicle
```

创建 `main/vehicle/voice_reply.cc`：

```cpp
#include "voice_reply.h"

#include <cmath>

namespace vehicle {

namespace {

void AppendNumber(std::vector<ClipId> &out, int value) {
    const std::vector<ClipId> digits = NumberToClips(value);
    out.insert(out.end(), digits.begin(), digits.end());
}

}  // namespace

const char *ToString(ClipId id) {
    switch (id) {
        case ClipId::kNone: return "none";
        case ClipId::kD0: return "d0";
        case ClipId::kD1: return "d1";
        case ClipId::kD2: return "d2";
        case ClipId::kD3: return "d3";
        case ClipId::kD4: return "d4";
        case ClipId::kD5: return "d5";
        case ClipId::kD6: return "d6";
        case ClipId::kD7: return "d7";
        case ClipId::kD8: return "d8";
        case ClipId::kD9: return "d9";
        case ClipId::kD10: return "d10";
        case ClipId::kUnitDegree: return "unit_degree";
        case ClipId::kUnitTimes: return "unit_times";
        case ClipId::kQTemp: return "q_temp";
        case ClipId::kQHumid: return "q_humid";
        case ClipId::kQEvents: return "q_events";
        case ClipId::kQStatusOk: return "q_status_ok";
        case ClipId::kQOnline: return "q_online";
        case ClipId::kQOffline: return "q_offline";
        case ClipId::kQNoAlert: return "q_no_alert";
        case ClipId::kQPending: return "q_pending";
        case ClipId::kQLightDark: return "q_light_dark";
        case ClipId::kQLightDim: return "q_light_dim";
        case ClipId::kQLightMid: return "q_light_mid";
        case ClipId::kQLightBright: return "q_light_bright";
        case ClipId::kQLightStrong: return "q_light_strong";
        case ClipId::kChime: return "chime";
        case ClipId::kSnapStart: return "snap_start";
        case ClipId::kSnapDone: return "snap_done";
        case ClipId::kLockEntered: return "lock_entered";
        case ClipId::kLockExited: return "lock_exited";
        case ClipId::kEvHardAccel: return "ev_hard_accel";
        case ClipId::kEvHardBrake: return "ev_hard_brake";
        case ClipId::kEvHardTurn: return "ev_hard_turn";
        case ClipId::kEvBump: return "ev_bump";
        case ClipId::kEvCrash: return "ev_crash";
        case ClipId::kEvParked: return "ev_parked";
        case ClipId::kEvDriving: return "ev_driving";
        case ClipId::kEvMotionParked: return "ev_motion_parked";
        case ClipId::kCount: break;
    }
    return "none";
}

std::vector<ClipId> NumberToClips(int value) {
    // ! 没有"零下"片段（设计文档 §6.4）：负数只上屏、不播报
    if (value < 0) {
        return {};
    }
    if (value < 10) {
        return {static_cast<ClipId>(static_cast<int>(ClipId::kD0) + value)};
    }
    if (value < 20) {
        if (value == 10) {
            return {ClipId::kD10};
        }
        return {ClipId::kD10, static_cast<ClipId>(static_cast<int>(ClipId::kD0) + (value % 10))};
    }
    if (value < 100) {
        std::vector<ClipId> out;
        out.push_back(static_cast<ClipId>(static_cast<int>(ClipId::kD0) + (value / 10)));
        out.push_back(ClipId::kD10);
        if (value % 10 != 0) {
            out.push_back(static_cast<ClipId>(static_cast<int>(ClipId::kD0) + (value % 10)));
        }
        return out;
    }
    // > 逐位兜底（本项目用不到：温度/湿度/事件数都在 0–99，光照不读数字）
    std::vector<ClipId> out;
    int n = value;
    int digits[8] = {};
    int count = 0;
    while (n > 0 && count < 8) {
        digits[count++] = n % 10;
        n /= 10;
    }
    for (int i = count - 1; i >= 0; i--) {
        out.push_back(static_cast<ClipId>(static_cast<int>(ClipId::kD0) + digits[i]));
    }
    return out;
}

ClipId EventClip(EventType type) {
    switch (type) {
        case EventType::kHardAccel: return ClipId::kEvHardAccel;
        case EventType::kHardBrake: return ClipId::kEvHardBrake;
        case EventType::kHardTurn: return ClipId::kEvHardTurn;
        case EventType::kBump: return ClipId::kEvBump;
        case EventType::kCrash: return ClipId::kEvCrash;
        case EventType::kParked: return ClipId::kEvParked;
        case EventType::kMoving: return ClipId::kEvDriving;
        case EventType::kMotionWhileParked: return ClipId::kEvMotionParked;
        case EventType::kCount: break;
    }
    return ClipId::kNone;
}

ClipId OccupancyClip(bool pending) {
    return pending ? ClipId::kQPending : ClipId::kQNoAlert;
}

std::vector<ClipId> TemperatureClips(float temp_c, bool valid) {
    std::vector<ClipId> out{ClipId::kQTemp};
    if (!valid) {
        return out;
    }
    // > 播报用"最近整数"：26.4 → 二十六（没有"点四"这类片段，屏幕上是准确值）
    AppendNumber(out, static_cast<int>(lroundf(temp_c)));
    out.push_back(ClipId::kUnitDegree);
    return out;
}

std::vector<ClipId> HumidityClips(float humidity_pct, bool valid) {
    // > 「车内湿度百分之」是 q_humid 这一段的原文，所以后面直接接数字，不拼"百分之"
    std::vector<ClipId> out{ClipId::kQHumid};
    if (!valid) {
        return out;
    }
    AppendNumber(out, static_cast<int>(lroundf(humidity_pct)));
    return out;
}

ClipId LightClip(int32_t lux) {
    switch (BucketLight(lux)) {
        case LightLevel::kDark: return ClipId::kQLightDark;
        case LightLevel::kDim: return ClipId::kQLightDim;
        case LightLevel::kMedium: return ClipId::kQLightMid;
        case LightLevel::kBright: return ClipId::kQLightBright;
        case LightLevel::kStrong: return ClipId::kQLightStrong;
    }
    return ClipId::kQLightMid;
}

std::vector<ClipId> StatusClips(bool online, int32_t events_total) {
    std::vector<ClipId> out{ClipId::kQStatusOk, online ? ClipId::kQOnline : ClipId::kQOffline, ClipId::kQEvents};
    AppendNumber(out, events_total);
    out.push_back(ClipId::kUnitTimes);
    return out;
}

}  // namespace vehicle
```

并在 `main/CMakeLists.txt` 的 `SOURCES` 里加 `"vehicle/voice_reply.cc"`。

- [ ] **步骤 4：运行测试确认通过**

```powershell
g++ -std=c++17 -Wall -Wextra -I main/vehicle test/voice_reply_test.cc main/vehicle/voice_reply.cc main/vehicle/environment_sensor.cc -o build_host/voice_reply_test.exe
build_host\voice_reply_test.exe
```
预期：全部 `ok`。若报 `BucketLight` 未定义，说明漏了 `main/vehicle/environment_sensor.cc`（`BucketLight` 在那里实现）。

- [ ] **步骤 5：展示并提交（等用户确认）**

```bash
git add main/vehicle/voice_reply.h main/vehicle/voice_reply.cc test/voice_reply_test.cc main/CMakeLists.txt
git status --short
git diff --cached --stat
```
建议 commit message：`feat: 语音应答片段组装（数字读数/档位/状态序列）+ 主机测试`

---

## 任务 3：命令词派发链路（MN → AudioService → 板级）

**交付物：** 说出的命令词能带着 `action` 一路走到板级代码，串口打印 `Esp32S3Board: VC_ACTION action=temp intent=temp`。这一版**只打日志、不播报、不执行动作**（那是任务 4）。

**文件：**
- 修改：`main/audio/wake_words/custom_wake_word.h`、`.cc:173-193`
- 修改：`main/audio/audio_service.h:105-135`、`.cc:700-726`
- 修改：`main/boards/esp32s3/esp32s3_board.cc:498-533`

- [ ] **步骤 1：给 `CustomWakeWord` 加命令词回调并派发**

`main/audio/wake_words/custom_wake_word.h`：在 `OnWakeWordDetected` 之后加声明：

```cpp
    void OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback);
    // > Plan C：命令词（非 wake 的 action）回调。命中后**不停** running_，继续监听下一条。
    void OnVoiceCommand(std::function<void(const std::string& action)> callback) {
        voice_command_callback_ = std::move(callback);
    }
```

并在私有成员区（`wake_word_detected_callback_` 之后）加：

```cpp
    std::function<void(const std::string& wake_word)> wake_word_detected_callback_;
    std::function<void(const std::string& action)> voice_command_callback_;
```

`main/audio/wake_words/custom_wake_word.cc` 的 `Feed()`：把 `:178-187` 那段 `if (command.action == "wake") { … }` 补一个 `else` 分支：

```cpp
                auto& command = commands_[mn_result->command_id[i] - 1];
                if (command.action == "wake") {
                    last_detected_wake_word_ = command.text;
                    running_ = false;
                    input_buffer_.clear();

                    if (wake_word_detected_callback_) {
                        wake_word_detected_callback_(last_detected_wake_word_);
                    }
                } else {
                    // > 命令词命中：**不要**动 running_（待机态继续听下一条命令），只把 action 派发出去。
                    // > 走到这里说明 MN 正在跑，而上游只在 kDeviceStateIdle 使能它
                    // > （main/application.cc:903/921 用 IsAfeWakeWord() 判断，CustomWakeWord 不是 AFE），
                    // > 所以"命令词只在待机态生效"是天然成立的边界（设计文档 D2）。
                    ESP_LOGI(TAG, "Voice command detected: action=%s text=%s prob=%.2f", command.action.c_str(),
                             command.text.c_str(), mn_result->prob[i]);
                    if (voice_command_callback_) {
                        voice_command_callback_(command.action);
                    }
                }
```

- [ ] **步骤 2：`AudioService` 转发**

`main/audio/audio_service.h`：在 `SetCallbacks` 之后加公有方法：

```cpp
    void SetCallbacks(AudioServiceCallbacks& callbacks);
    // > Plan C：命令词（非唤醒 action）回调。未设置时行为与上游一致（命令词被忽略）。
    void SetVoiceCommandCallback(std::function<void(const std::string& action)> callback);
```

在私有成员区（`AudioServiceCallbacks callbacks_;` 之后）加：

```cpp
    AudioServiceCallbacks callbacks_;
    std::function<void(const std::string& action)> voice_command_callback_;
```

`main/audio/audio_service.cc`：在 `SetCallbacks()`（`:629-631`）之后加实现：

```cpp
void AudioService::SetVoiceCommandCallback(std::function<void(const std::string& action)> callback) {
    voice_command_callback_ = std::move(callback);
}
```

并在 `SetModelsList()` 的唤醒回调注册之后（`:719-725` 那段 `if (wake_word_) { … }` 里）加：

```cpp
    if (wake_word_) {
        wake_word_->OnWakeWordDetected([this](const std::string& wake_word) {
            if (callbacks_.on_wake_word_detected) {
                callbacks_.on_wake_word_detected(wake_word);
            }
        });
        // > 只有 CustomWakeWord 认识 action：AfeWakeWord / EspWakeWord 没有这个能力。
        // > dynamic_cast 在本工程可用（IsAfeWakeWord() 已经在用）。
        if (auto *custom = dynamic_cast<CustomWakeWord *>(wake_word_.get())) {
            custom->OnVoiceCommand([this](const std::string& action) {
                if (voice_command_callback_) {
                    voice_command_callback_(action);
                }
            });
        }
    }
```

> `custom_wake_word.h` 已经在 `audio_service.cc` 里被 include 了吗？没有则加上 `#include "wake_words/custom_wake_word.h"`（与 `AfeWakeWord` 的 include 放一起）。

- [ ] **步骤 3：板级接线（这一版只打日志）**

`main/boards/esp32s3/esp32s3_board.cc` 的 `StartNetwork()`（`:527-532`）里，HTTP 启动之后加：

```cpp
    virtual void StartNetwork() override {
        WifiBoard::StartNetwork();
        if (http_ != nullptr && !http_->Start(kHttpPort)) {
            ESP_LOGW(TAG, "局域网 HTTP 未启动，其它功能不受影响");
        }
        // > 命令词回调：现在只打日志（任务 4 接上播报与动作执行）。
        // > 放在 StartNetwork() 里而不是构造函数：AudioService 的 wake_word_ 要到
        // > Application::Start() → SetModelsList() 之后才存在，构造函数里设回调会被丢掉。
        Application::GetInstance().GetAudioService().SetVoiceCommandCallback([](const std::string &action) {
            ESP_LOGI("Esp32S3Board", "VC_ACTION action=%s intent=%s", action.c_str(),
                     vehicle::ToString(vehicle::ParseVoiceAction(action)));
        });
    }
```

并在文件顶部加 `#include "voice_intent.h"`（板级目录外头文件，`INCLUDE_DIRS` 已含 `vehicle`）。

- [ ] **步骤 4：构建并确认编过（零 warning）**

```powershell
cmd /c "... && idf.py build > build\d6_task3_build.log 2>&1"
Select-String -Path build\d6_task3_build.log -Pattern "warning|error|Project build complete" | Select-Object -First 20
```
预期：`Project build complete`；若出现 `warning:` 且指向本任务的四个文件，必须修掉再继续。

- [ ] **步骤 5：烧录并真机验证派发链路**

```powershell
cmd /c "... && idf.py -p COM10 flash > build\d6_task3_flash.log 2>&1"
pwsh -File build\capture_once.ps1 -Seconds 90 -Out build\d6_task3_cmd.log
```

对板子依次说 8 条命令词，然后：

```powershell
Select-String -Path build\d6_task3_cmd.log -Pattern "Voice command detected|VC_ACTION"
```
预期：每条命令各出现一对日志，例如
```
I (…) CustomWakeWord: Voice command detected: action=temp text=车内温度 prob=0.93
I (…) Esp32S3Board: VC_ACTION action=temp intent=temp
```
并且**唤醒词仍然正常**（`Custom wake word detected: command_id=1` → 小智应答）。

- [ ] **步骤 6：展示并提交（等用户确认）**

```bash
git add main/audio/wake_words/custom_wake_word.h main/audio/wake_words/custom_wake_word.cc \
        main/audio/audio_service.h main/audio/audio_service.cc main/boards/esp32s3/esp32s3_board.cc
git status --short
git diff --cached --stat
```
建议 commit message：`feat: 命令词 action 从 CustomWakeWord 派发到板级（D6 任务 3）`

---

## 任务 4：命令执行与播报（VoiceCommand）+ 手机端"有遗留"标记

**交付物：** 8 条命令词全部有实际动作与播报；行车事件自动播报；抓拍完成播 `snap_done`；浏览器可标记"有遗留"，「有没有人」据此播报。

**文件：**
- 创建：`main/boards/esp32s3/voice_command.h`、`main/boards/esp32s3/voice_command.cc`
- 修改：`main/boards/esp32s3/camera_capture.h`、`.cc:181-201`（抓拍完成回调）
- 修改：`main/boards/esp32s3/vehicle_http.h`、`.cc:18-28,54-88,135-163`（`/leftover` 路由 + 首页链接 + 构造参数）
- 修改：`main/boards/esp32s3/esp32s3_board.cc`（接线 + `kMaxSinks` 由任务 5 处理）

- [ ] **步骤 1：`CameraCapture` 加"抓拍完成"回调**

`camera_capture.h`：加 include 与成员/接口：

```cpp
#include <functional>
```

```cpp
    // 抓拍并落盘成功后调用（由 worker 任务在自己的栈上执行，**可以碰 flash**）。
    // > 用途：语音"重新抓拍"要等真正出图后再播"抓拍完成"（设计文档 §6.4）。
    void SetCaptureDoneCallback(std::function<void(vehicle::CaptureReason reason, bool saved)> callback) {
        capture_done_callback_ = std::move(callback);
    }
```

私有成员区加：

```cpp
    std::function<void(vehicle::CaptureReason, bool)> capture_done_callback_;
```

`camera_capture.cc` 的 `OnCaptureRequest()` 末尾（`:200` 的 `ESP_LOGI` 之后）加：

```cpp
    if (capture_done_callback_) {
        capture_done_callback_(reason, saved);
    }
```

- [ ] **步骤 2：写 `VoiceCommand`**

创建 `main/boards/esp32s3/voice_command.h`：

```cpp
#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "vehicle_service.h"   // EventSink
#include "vehicle_types.h"
#include "voice_intent.h"
#include "voice_reply.h"

class VehicleService;

// 语音命令与播报（Plan C / D6）。
//
// ! 这个类**一次都不读 flash**：
// !   - OnVoiceAction() 由命令词回调调用，而回调运行在**音频输入任务**里（audio_service.cc
// !     的 AudioInputTask）；事件播报 OnEvent() 运行在 **worker 任务**里。
// !   - 手机端"有遗留"标记只存 RAM（atomic），不落 NVS —— 写 NVS 会关 cache，
// !     而标记是由 **HTTP 任务**（栈在 PSRAM）设置的，一写就命中
// !     `assert(esp_task_stack_is_sane_cache_disabled())`（BUG-024 / BUG-026）。
// !     代价：重启后标记复位，写进验收记录的局限一节。
class VoiceCommand : public EventSink {
public:
    VoiceCommand(VehicleService *vehicle, int max_actions = 4);
    ~VoiceCommand();

    // 命令词命中（任意任务可调，但只允许"存进队列"，执行在 worker 任务里做）
    void OnVoiceAction(const std::string &action);

    // 抓拍完成（由 CameraCapture 在 worker 任务里回调）
    void OnCaptureDone(vehicle::CaptureReason reason, bool saved);

    // 手机端标记（HTTP 任务调用；只写原子量）
    void SetLeftoverPending(bool pending) { leftover_pending_.store(pending); }
    bool leftover_pending() const { return leftover_pending_.load(); }

    // EventSink：行车事件 → 播报（只在待机态播，避免盖住小智说话）
    void OnEvent(const vehicle::EventRecord &record) override;

    // 把排队的命令执行掉；由 VehicleService 的 worker 任务调用（设计文档 §3.1 的分工）
    void ExecutePending();

private:
    void Execute(vehicle::VoiceIntent intent);
    void Play(const std::vector<vehicle::ClipId> &clips);
    bool NetworkOnline() const;

    VehicleService *vehicle_ = nullptr;
    std::atomic<bool> leftover_pending_{false};
    int max_actions_ = 4;
    std::atomic<int> head_{0};
    std::atomic<int> tail_{0};
    int pending_[8] = {};   // 存 VoiceIntent 的值，0 = 空槽（kUnknown）
};
```

- [ ] **步骤 3：写 `VoiceCommand` 实现**

创建 `main/boards/esp32s3/voice_command.cc`：

```cpp
#include "voice_command.h"

#include <cstring>

#include <esp_log.h>

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "cJSON.h"

#define TAG "VoiceCommand"

namespace {

// > ClipId → 播报片段。**必须逐个写全**：片段只有被代码引用时才会进 app，
// > 没被引用的常量会被 --gc-sections 丢掉（设计文档 §6.4 的实测结论，
// > 表现为"片段放进去了但播报没反应、app 体积也不涨"）。
std::string_view SoundFor(vehicle::ClipId id) {
    switch (id) {
        case vehicle::ClipId::kD0: return Lang::Sounds::OGG_D0;
        case vehicle::ClipId::kD1: return Lang::Sounds::OGG_D1;
        case vehicle::ClipId::kD2: return Lang::Sounds::OGG_D2;
        case vehicle::ClipId::kD3: return Lang::Sounds::OGG_D3;
        case vehicle::ClipId::kD4: return Lang::Sounds::OGG_D4;
        case vehicle::ClipId::kD5: return Lang::Sounds::OGG_D5;
        case vehicle::ClipId::kD6: return Lang::Sounds::OGG_D6;
        case vehicle::ClipId::kD7: return Lang::Sounds::OGG_D7;
        case vehicle::ClipId::kD8: return Lang::Sounds::OGG_D8;
        case vehicle::ClipId::kD9: return Lang::Sounds::OGG_D9;
        case vehicle::ClipId::kD10: return Lang::Sounds::OGG_D10;
        case vehicle::ClipId::kUnitDegree: return Lang::Sounds::OGG_UNIT_DEGREE;
        case vehicle::ClipId::kUnitTimes: return Lang::Sounds::OGG_UNIT_TIMES;
        case vehicle::ClipId::kQTemp: return Lang::Sounds::OGG_Q_TEMP;
        case vehicle::ClipId::kQHumid: return Lang::Sounds::OGG_Q_HUMID;
        case vehicle::ClipId::kQEvents: return Lang::Sounds::OGG_Q_EVENTS;
        case vehicle::ClipId::kQStatusOk: return Lang::Sounds::OGG_Q_STATUS_OK;
        case vehicle::ClipId::kQOnline: return Lang::Sounds::OGG_Q_ONLINE;
        case vehicle::ClipId::kQOffline: return Lang::Sounds::OGG_Q_OFFLINE;
        case vehicle::ClipId::kQNoAlert: return Lang::Sounds::OGG_Q_NO_ALERT;
        case vehicle::ClipId::kQPending: return Lang::Sounds::OGG_Q_PENDING;
        case vehicle::ClipId::kQLightDark: return Lang::Sounds::OGG_Q_LIGHT_DARK;
        case vehicle::ClipId::kQLightDim: return Lang::Sounds::OGG_Q_LIGHT_DIM;
        case vehicle::ClipId::kQLightMid: return Lang::Sounds::OGG_Q_LIGHT_MID;
        case vehicle::ClipId::kQLightBright: return Lang::Sounds::OGG_Q_LIGHT_BRIGHT;
        case vehicle::ClipId::kQLightStrong: return Lang::Sounds::OGG_Q_LIGHT_STRONG;
        case vehicle::ClipId::kChime: return Lang::Sounds::OGG_TONE_CHIME;
        case vehicle::ClipId::kSnapStart: return Lang::Sounds::OGG_SNAP_START;
        case vehicle::ClipId::kSnapDone: return Lang::Sounds::OGG_SNAP_DONE;
        case vehicle::ClipId::kLockEntered: return Lang::Sounds::OGG_LOCK_ENTERED;
        case vehicle::ClipId::kLockExited: return Lang::Sounds::OGG_LOCK_EXITED;
        case vehicle::ClipId::kEvHardAccel: return Lang::Sounds::OGG_EV_HARD_ACCEL;
        case vehicle::ClipId::kEvHardBrake: return Lang::Sounds::OGG_EV_HARD_BRAKE;
        case vehicle::ClipId::kEvHardTurn: return Lang::Sounds::OGG_EV_HARD_TURN;
        case vehicle::ClipId::kEvBump: return Lang::Sounds::OGG_EV_BUMP;
        case vehicle::ClipId::kEvCrash: return Lang::Sounds::OGG_EV_CRASH;
        case vehicle::ClipId::kEvParked: return Lang::Sounds::OGG_EV_PARKED;
        case vehicle::ClipId::kEvDriving: return Lang::Sounds::OGG_EV_DRIVING;
        case vehicle::ClipId::kEvMotionParked: return Lang::Sounds::OGG_EV_MOTION_PARKED;
        case vehicle::ClipId::kNone:
        case vehicle::ClipId::kCount:
            break;
    }
    return {};
}

}  // namespace

VoiceCommand::VoiceCommand(VehicleService *vehicle, int max_actions)
    : vehicle_(vehicle), max_actions_(max_actions < 8 ? max_actions : 8) {
}

VoiceCommand::~VoiceCommand() = default;

void VoiceCommand::OnVoiceAction(const std::string &action) {
    const vehicle::VoiceIntent intent = vehicle::ParseVoiceAction(action);
    if (intent == vehicle::VoiceIntent::kUnknown || intent == vehicle::VoiceIntent::kWake) {
        ESP_LOGW(TAG, "命令词 %s 无法处理（intent=%s）", action.c_str(), vehicle::ToString(intent));
        return;
    }
    // ! 这里运行在**音频输入任务**里：只入队，不执行（NVS/播报都挪到 worker 任务，
    // ! 免得把麦克风采集按在那儿）。队列满就丢最新的并告警——待机态下不可能堆起来。
    const int head = head_.load();
    const int next = (head + 1) % max_actions_;
    if (next == tail_.load()) {
        ESP_LOGW(TAG, "命令队列已满，丢弃 action=%s", action.c_str());
        return;
    }
    pending_[head] = static_cast<int>(intent);
    head_.store(next);
}

void VoiceCommand::ExecutePending() {
    while (tail_.load() != head_.load()) {
        const int tail = tail_.load();
        const vehicle::VoiceIntent intent = static_cast<vehicle::VoiceIntent>(pending_[tail]);
        pending_[tail] = 0;
        tail_.store((tail + 1) % max_actions_);
        Execute(intent);
    }
}

void VoiceCommand::Execute(vehicle::VoiceIntent intent) {
    const vehicle::EnvReading env = vehicle_->env();
    const vehicle::VehicleStatus status = vehicle_->Status();

    switch (intent) {
        case vehicle::VoiceIntent::kTemp:
            Play(vehicle::TemperatureClips(env.temp_c, env.valid));
            break;
        case vehicle::VoiceIntent::kHumid:
            Play(vehicle::HumidityClips(env.humidity_pct, env.valid));
            break;
        case vehicle::VoiceIntent::kLight:
            Play({vehicle::LightClip(env.lux)});
            break;
        case vehicle::VoiceIntent::kOccupancy:
            Play({vehicle::OccupancyClip(leftover_pending_.load())});
            break;
        case vehicle::VoiceIntent::kStatus:
            Play(vehicle::StatusClips(NetworkOnline(), status.events_total));
            break;
        case vehicle::VoiceIntent::kChime:
            Play({vehicle::ClipId::kChime});
            break;
        case vehicle::VoiceIntent::kSnapshot:
            Play({vehicle::ClipId::kSnapStart});
            vehicle_->RequestCapture();
            break;
        case vehicle::VoiceIntent::kLock: {
            const bool locked = status.state == vehicle::MotionState::kLockedMonitor;
            vehicle_->RequestLock(!locked);
            Play({locked ? vehicle::ClipId::kLockExited : vehicle::ClipId::kLockEntered});
            break;
        }
        case vehicle::VoiceIntent::kWake:
        case vehicle::VoiceIntent::kUnknown:
        case vehicle::VoiceIntent::kCount:
            return;
    }
    // > 这一行是 D7 指标测量的**唯一证据来源**（build/count_vc.ps1 按这个前缀统计）
    ESP_LOGI(TAG, "VC_DONE intent=%s 环境=%.1f℃/%.0f%%/%d lux 状态=%s", vehicle::ToString(intent),
             static_cast<double>(env.temp_c), static_cast<double>(env.humidity_pct), static_cast<int>(env.lux),
             vehicle::ToString(status.state));
}

void VoiceCommand::Play(const std::vector<vehicle::ClipId> &clips) {
    auto &app = Application::GetInstance();
    for (vehicle::ClipId id : clips) {
        const std::string_view sound = SoundFor(id);
        if (sound.empty()) {
            ESP_LOGW(TAG, "片段 %s 没有对应音频常量，跳过", vehicle::ToString(id));
            continue;
        }
        // > Application::PlaySound → AudioService::PlaySound：内部 OggDemuxer 解包后推解码队列，
        // > 连续多次调用会**按顺序排队播放**（Application::ShowActivationCode 拼数字就是这么做的）。
        app.PlaySound(sound);
    }
}

bool VoiceCommand::NetworkOnline() const {
    // > 判据用"是否拿到 IP"，不是"巴法云 MQTT 连上了"：没配凭据时 MQTT 永远连不上，
    // > 那时播"未联网"会让用户以为 WiFi 断了（VehicleHttp::LogAccessUrl 用的是同一处来源）。
    const std::string info = Board::GetInstance().GetSystemInfoJson();
    cJSON *root = cJSON_Parse(info.c_str());
    if (root == nullptr) {
        return false;
    }
    const cJSON *board = cJSON_GetObjectItem(root, "board");
    const cJSON *ip = cJSON_IsObject(board) ? cJSON_GetObjectItem(board, "ip") : nullptr;
    const bool online = cJSON_IsString(ip) && ip->valuestring != nullptr && ip->valuestring[0] != '\0';
    cJSON_Delete(root);
    return online;
}

void VoiceCommand::OnCaptureDone(vehicle::CaptureReason reason, bool saved) {
    // > 只有语音/按钮的手动抓拍才播报"抓拍完成"：锁车期自动抓拍会频繁打扰。
    if (!saved || reason != vehicle::CaptureReason::kManual) {
        return;
    }
    Play({vehicle::ClipId::kSnapDone});
}

void VoiceCommand::OnEvent(const vehicle::EventRecord &record) {
    // > 只在待机态播事件：小智正在说话/听的时候插播会把对话搅乱。
    if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
        return;
    }
    const vehicle::ClipId clip = vehicle::EventClip(record.event.type);
    if (clip != vehicle::ClipId::kNone) {
        Play({clip});
    }
}
```

- [ ] **步骤 4：`VehicleService` 增加"排空命令队列"的钩子**

`vehicle_service.h`：加公有方法与成员：

```cpp
    // 手动抓拍（屏幕按钮 / 语音"重新抓拍"），由 worker_task 执行
    void RequestCapture();

    // 语音命令执行器（Plan C）：每轮 worker 循环调用一次，由它在**worker 任务**里执行
    // 需要访问 flash 或会阻塞的动作。可以是 nullptr（未接线时什么也不做）。
    void SetCommandExecutor(WorkerTickable *executor) { executor_ = executor; }
```

并把接口定义放在 `EventSink` 之后：

```cpp
// worker 任务每轮调用一次的执行器（Plan C 的语音命令用它把"执行"从音频任务挪到 worker 任务）。
class WorkerTickable {
public:
    virtual ~WorkerTickable() = default;
    virtual void ExecutePending() = 0;
};
```

私有成员加：

```cpp
    WorkerTickable *executor_ = nullptr;
```

`vehicle_service.cc` 的 `WorkerTaskLoop()` 里，在"3) 1 Hz 读环境传感器"之前插一步：

```cpp
        // 2.5) 语音命令执行（Plan C）：执行放在 worker 任务里，因为音频输入任务不能
        //      做任何可能阻塞的事（它一停，麦克风采集就丢帧）。
        if (executor_ != nullptr) {
            executor_->ExecutePending();
        }
```

- [ ] **步骤 5：`VoiceCommand` 同时实现 `WorkerTickable`**

`voice_command.h`：类声明改为

```cpp
class VoiceCommand : public EventSink, public WorkerTickable {
```

（`ExecutePending()` 已经是 `WorkerTickable` 要求的签名，加 `override`：）

```cpp
    void ExecutePending() override;
```

- [ ] **步骤 6：HTTP 加 `/leftover` 路由**

`vehicle_http.h`：构造函数加一个可空参数与私有成员/处理器：

```cpp
class VoiceCommand;

    explicit VehicleHttp(SnapshotStore *store, VoiceCommand *voice = nullptr);
```

```cpp
    static esp_err_t HandleLeftover(httpd_req_t *req);
```

```cpp
    SnapshotStore *store_ = nullptr;
    VoiceCommand *voice_ = nullptr;
```

`vehicle_http.cc`：构造函数改为 `VehicleHttp::VehicleHttp(SnapshotStore *store, VoiceCommand *voice) : store_(store), voice_(voice) {}`，
include `voice_command.h`，在 `Start()` 里注册路由（`:88` 之后）：

```cpp
    const httpd_uri_t leftover = {.uri = "/leftover", .method = HTTP_GET, .handler = HandleLeftover, .user_ctx = this};
    httpd_register_uri_handler(server_, &leftover);
```

首页 HTML（`:18-28` 的 `kIndexHtml`）在 `<h3>最近事件</h3>` 之前插一行：

```cpp
    "<p>遗留确认：<a href='/leftover?value=1'>标记有遗留</a> · <a href='/leftover?value=0'>清除标记</a></p>"
```

处理器实现（加在 `HandleEvents` 之后）：

```cpp
// GET /leftover?value=1 → 标记"有遗留待确认"；value=0 → 清除。
// ! 本函数的任务栈在 PSRAM（httpd_config_t::task_caps），**不能写 NVS**（会关 cache 触发
// ! assert，见 BUG-024/026），所以标记只存在 VoiceCommand 的原子量里 —— 重启复位。
esp_err_t VehicleHttp::HandleLeftover(httpd_req_t *req) {
    auto *self = static_cast<VehicleHttp *>(req->user_ctx);
    bool pending = true;   // > 不带参数就等于"标记有遗留"
    char query[32] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char value[4] = {};
        if (httpd_query_key_value(query, "value", value, sizeof(value)) == ESP_OK) {
            pending = (value[0] != '0');
        }
    }
    if (self != nullptr && self->voice_ != nullptr) {
        self->voice_->SetLeftoverPending(pending);
    }
    ESP_LOGI(TAG, "手机端标记：有遗留待确认=%s", pending ? "是" : "否");
    SendText(req, "text/plain; charset=utf-8", pending ? "已标记：有遗留待确认（重启后复位）" : "已清除标记");
    return ESP_OK;
}
```

- [ ] **步骤 7：板级接线**

`esp32s3_board.cc`：
1. 顶部加 `#include "voice_command.h"`；
2. 成员加 `VoiceCommand* voice_command_ = nullptr;`；
3. 构造函数里，在 `camera_capture_` 建好之后、`vehicle_ui_` 之前插入：

```cpp
        // > 语音：命令执行 + 播报（Plan C）。它同时是 EventSink（行车事件播报）与
        // > WorkerTickable（在 worker 任务里执行命令，避开音频任务）。
        voice_command_ = new VoiceCommand(vehicle_);
        vehicle_->AddEventSink(voice_command_);
        vehicle_->SetCommandExecutor(voice_command_);
        camera_capture_->SetCaptureDoneCallback([this](vehicle::CaptureReason reason, bool saved) {
            voice_command_->OnCaptureDone(reason, saved);
        });
```

4. `http_ = new VehicleHttp(snapshot_store_, voice_command_);`
5. `StartNetwork()` 里的命令词回调（任务 3 加的那段匿名 lambda）换成转发：

```cpp
        Application::GetInstance().GetAudioService().SetVoiceCommandCallback([this](const std::string &action) {
            ESP_LOGI(TAG, "VC_ACTION action=%s intent=%s", action.c_str(), vehicle::ToString(vehicle::ParseVoiceAction(action)));
            voice_command_->OnVoiceAction(action);
        });
```

- [ ] **步骤 8：构建（先 reconfigure，新增了 .cc）**

```powershell
cmd /c "... && idf.py reconfigure && idf.py build > build\d6_task4_build.log 2>&1"
Select-String -Path build\d6_task4_build.log -Pattern "voice_command.cc.obj|warning:|error:|Project build complete"
```
预期：日志里有 `voice_command.cc.obj`（板级文件真被编进去了），零 `warning:`，`Project build complete`。

**同时核对片段真的进了 app**（设计文档 §6.4 的坑）：
```powershell
(Get-Item build\xiaozhi.bin).Length
```
预期：比任务 3 那一版**明显变大**（42 个片段合计 120,679 B，实际增长 ≈ 120 KB ± 去重）。若体积**一点都不涨**，说明 `SoundFor()` 里的常量没被引用到（回头检查 switch 是否漏了分支或阈值判断）。

- [ ] **步骤 9：烧录并真机验证（用户操作）**

```powershell
cmd /c "... && idf.py -p COM10 flash > build\d6_task4_flash.log 2>&1"
pwsh -File build\capture_once.ps1 -Seconds 180 -Out build\d6_task4_voice.log
```

请用户做这几件事，每做一件记一次：

| # | 用户说/做 | 期望（串口 + 耳朵） |
|---|---|---|
| 1 | 「你好小智」→「车内温度」 | 播「车内温度 二十六 度」，串口 `VC_DONE intent=temp` |
| 2 | 「车内湿度」 | 播「车内湿度百分之 四十八」 |
| 3 | 「光照多少」 | 播档位（如「光照适中」） |
| 4 | 「设备状态」 | 播「设备运行正常 已联网 今日事件 N 次」 |
| 5 | 「播放提示音」 | 响一声提示音（`tone_chime`） |
| 6 | 「重新抓拍」 | 播「正在抓拍」→ 约 1 s 后「抓拍完成」，串口 `抓拍完成：原因=手动` |
| 7 | 「锁车」 | 播「已进入锁车监测」；再说「锁车」→ 播「已解除锁车监测」 |
| 8 | 手机浏览器打开 `http://<IP>:8080/` 点「标记有遗留」，然后说「有没有人」 | 播「有遗留提醒待确认」；点「清除标记」再说 → 播「未检测到异常」 |
| 9 | 手持板子模拟一次急刹车 | 屏幕弹事件 + 播「急刹车」 |
| 10 | 边让小智说话（唤醒后问个问题）边说「车内温度」 | **不应**插播（非待机态） |

```powershell
Select-String -Path build\d6_task4_voice.log -Pattern "VC_ACTION|VC_DONE|抓拍完成|手机端标记"
```

- [ ] **步骤 10：展示并提交（等用户确认）**

```bash
git add main/boards/esp32s3/voice_command.h main/boards/esp32s3/voice_command.cc \
        main/boards/esp32s3/camera_capture.h main/boards/esp32s3/camera_capture.cc \
        main/boards/esp32s3/vehicle_http.h main/boards/esp32s3/vehicle_http.cc \
        main/boards/esp32s3/vehicle_service.h main/boards/esp32s3/vehicle_service.cc \
        main/boards/esp32s3/esp32s3_board.cc
git status --short
git diff --cached --stat
```
建议 commit message：`feat: 语音命令执行与片段播报、事件播报、手机端遗留标记（D6 任务 4）`

---

## 任务 5：巴法云 MQTT 连接与事件上报

**交付物：** 事件实时发到巴法云，`手机小程序能看到`；上报统计（已发/失败/积压/连接）可查。**断网补传在任务 6**。

**文件：**
- 修改：`main/boards/esp32s3/config.h`（主题名宏）
- 修改：`main/CMakeLists.txt`（从 `.env` 生成 `bemfa_secrets.h`）
- 创建：`main/vehicle/pending_queue.h`、`main/vehicle/pending_queue.cc`、`test/pending_queue_test.cc`
- 修改：`main/vehicle/event_json.{h,cc}`（上报变体）
- 创建：`test/report_json_test.cc`
- 创建：`main/boards/esp32s3/bemfa_client.h`、`bemfa_client.cc`
- 修改：`main/boards/esp32s3/vehicle_service.h`（`kMaxSinks`）、`esp32s3_board.cc`（接线 + MCP 工具）

- [ ] **步骤 1：把 `kMaxSinks` 提到 6 并让溢出有声音**

`vehicle_service.h` 的 `kMaxSinks = 4` 改成 6，并加注释：

```cpp
    // > Plan C 之后消费者是 4 个：SnapshotStore（落盘）、CameraCapture（抓拍）、
    // > VoiceCommand（播报）、BemfaClient（上报）。留 2 个余量。
    // ! AddEventSink() 满员时是**静默丢弃**的（返回值 void），加消费者时务必回看这行。
    static constexpr int kMaxSinks = 6;
```

`vehicle_service.cc` 的 `AddEventSink()` 加告警：

```cpp
void VehicleService::AddEventSink(EventSink *sink) {
    if (sink == nullptr || sink_count_ >= kMaxSinks) {
        // ! 不加这条日志的话，"第 7 个消费者"会被静默丢掉（原来就是静默 return）
        ESP_LOGW(TAG, "事件消费者注册失败（已满 %d 个）", kMaxSinks);
        return;
    }
    sinks_[sink_count_++] = sink;
}
```

- [ ] **步骤 2：写失败的测试（环形队列）**

创建 `test/pending_queue_test.cc`：

```cpp
// 上报积压队列（定长槽位环形缓冲）的主机单元测试
//
// 编译与运行：
//   g++ -std=c++17 -Wall -Wextra -I main/vehicle test/pending_queue_test.cc main/vehicle/pending_queue.cc -o build_host/pending_queue_test.exe
//   build_host\pending_queue_test.exe

#include <cstdio>
#include <string>
#include <vector>

#include "pending_queue.h"

using namespace vehicle;

static int g_failures = 0;

#define CHECK(cond, what)                                     \
    do {                                                      \
        if (cond) {                                           \
            printf("  ok   %s\n", what);                      \
        } else {                                              \
            printf("  FAIL %s\n", what);                      \
            g_failures++;                                     \
        }                                                     \
    } while (0)

int main() {
    printf("pending_queue\n");

    std::vector<char> buffer(3 * PendingQueue::kSlotBytes, 0);
    PendingQueue queue(buffer.data(), 3);
    std::string out;

    CHECK(queue.capacity() == 3 && queue.size() == 0, "初始为空");
    CHECK(!queue.Pop(out), "空队列 Pop 返回 false");

    CHECK(queue.Push("a") && queue.Push("b") && queue.Push("c"), "压入 3 条");
    CHECK(queue.size() == 3 && queue.dropped() == 0, "满 3 条、无丢弃");

    // > 队列满时**丢最旧**（事件日志同理：宁可丢旧，不阻塞采样）
    CHECK(!queue.Push("d"), "满队列 Push 返回 false（已丢最旧）");
    CHECK(queue.size() == 3 && queue.dropped() == 1, "丢 1 条最旧");

    CHECK(queue.Pop(out) && out == "b", "先出 b（a 已被丢）");
    CHECK(queue.Pop(out) && out == "c", "再出 c");
    CHECK(queue.Pop(out) && out == "d", "最后出 d");
    CHECK(!queue.Pop(out), "排空后 Pop 返回 false");

    // 回绕：反复压弹不串槽
    for (int i = 0; i < 10; i++) {
        std::string payload = "e" + std::to_string(i);
        CHECK(queue.Push(payload), "回绕压入");
        CHECK(queue.Pop(out) && out == payload, "回绕弹出内容一致");
    }

    // 超长 payload：截断而不是越界写
    std::string huge(static_cast<size_t>(PendingQueue::kSlotBytes) * 3, 'x');
    CHECK(queue.Push(huge), "超长 payload 压入成功");
    CHECK(queue.Pop(out), "超长 payload 弹出成功");
    CHECK(out.size() == PendingQueue::kSlotBytes - 1, "超长 payload 被截断到 kSlotBytes-1");
    CHECK(out == huge.substr(0, PendingQueue::kSlotBytes - 1), "截断内容正确");

    printf("\n%s（失败 %d）\n", g_failures == 0 ? "全部通过" : "有失败", g_failures);
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **步骤 3：运行确认失败，然后写实现**

```powershell
g++ -std=c++17 -Wall -Wextra -I main/vehicle test/pending_queue_test.cc main/vehicle/pending_queue.cc -o build_host/pending_queue_test.exe
```
预期：`pending_queue.h: No such file or directory`。

创建 `main/vehicle/pending_queue.h`：

```cpp
#pragma once

// 定长槽位环形队列：断网积压的事件 payload 放这里。
//
// ! 为什么不用 std::deque<std::string>：500 条 × 约 80 B ≈ 40 KB，而 std::string/deque
// ! 走的是默认分配器（内部堆）。本板通用内部堆只有 22 KB 且空载就 99.7% 满（BUG-024 补充），
// ! 一压就 ENOMEM。所以缓冲由**调用方**提供——设备侧显式 heap_caps_malloc(MALLOC_CAP_SPIRAM)。
// 纯逻辑（不分配内存、不依赖 ESP-IDF），主机可测。

#include <cstddef>
#include <cstdint>
#include <string>

namespace vehicle {

class PendingQueue {
public:
    // > 一条 payload 的上限：事件 JSON 约 80 B，留一倍余量。
    static constexpr size_t kSlotBytes = 160;

    // buffer 至少要 capacity * kSlotBytes 字节，构造后由本对象独占
    PendingQueue(char *buffer, int capacity);

    // 压入一条。队列满时**丢最旧**并返回 false；payload 超长则截断。
    bool Push(const std::string &payload);
    // 弹出一条（FIFO）。空队列返回 false。
    bool Pop(std::string &out);

    int size() const { return size_; }
    int capacity() const { return capacity_; }
    int dropped() const { return dropped_; }

private:
    char *Slot(int index) const { return buffer_ + static_cast<size_t>(index) * kSlotBytes; }

    char *buffer_ = nullptr;
    int capacity_ = 0;
    int head_ = 0;   // 下一个读的位置
    int size_ = 0;
    int dropped_ = 0;
};

}  // namespace vehicle
```

创建 `main/vehicle/pending_queue.cc`：

```cpp
#include "pending_queue.h"

#include <cstring>

namespace vehicle {

PendingQueue::PendingQueue(char *buffer, int capacity) : buffer_(buffer), capacity_(capacity > 0 ? capacity : 0) {
}

bool PendingQueue::Push(const std::string &payload) {
    if (buffer_ == nullptr || capacity_ == 0) {
        return false;
    }
    bool accepted = true;
    if (size_ == capacity_) {
        // > 丢最旧：把读指针往前挪一格，覆盖那条
        head_ = (head_ + 1) % capacity_;
        size_--;
        dropped_++;
        accepted = false;
    }
    const size_t limit = kSlotBytes - 1;
    const size_t len = payload.size() < limit ? payload.size() : limit;
    const int tail = (head_ + size_) % capacity_;
    char *slot = Slot(tail);
    // > 槽位前两字节放长度（小端），后面跟内容：与 NUL 结尾相比，payload 里出现 '\0'
    // > （理论上不会有）也不会截断。
    const uint16_t stored = static_cast<uint16_t>(len);
    slot[0] = static_cast<char>(stored & 0xFF);
    slot[1] = static_cast<char>((stored >> 8) & 0xFF);
    memcpy(slot + 2, payload.data(), len);
    size_++;
    return accepted;
}

bool PendingQueue::Pop(std::string &out) {
    if (buffer_ == nullptr || size_ == 0) {
        return false;
    }
    const char *slot = Slot(head_);
    const uint16_t len = static_cast<uint16_t>(static_cast<unsigned char>(slot[0])) |
                         static_cast<uint16_t>(static_cast<unsigned char>(slot[1]) << 8);
    out.assign(slot + 2, len);
    head_ = (head_ + 1) % capacity_;
    size_--;
    return true;
}

}  // namespace vehicle
```

- [ ] **步骤 4：运行测试确认通过**

```powershell
g++ -std=c++17 -Wall -Wextra -I main/vehicle test/pending_queue_test.cc main/vehicle/pending_queue.cc -o build_host/pending_queue_test.exe
build_host\pending_queue_test.exe
```
预期：全部 `ok`。

- [ ] **步骤 5：事件 JSON 的上报变体（先写测试）**

创建 `test/report_json_test.cc`：

```cpp
// 上报 JSON（事件/环境/状态）与 seq 解析的主机单元测试
//
// 编译与运行：
//   g++ -std=c++17 -Wall -Wextra -I main/vehicle test/report_json_test.cc main/vehicle/event_json.cc -o build_host/report_json_test.exe
//   build_host\report_json_test.exe

#include <cstdio>
#include <string>

#include "event_json.h"

using namespace vehicle;

static int g_failures = 0;

#define CHECK(cond, what)                                     \
    do {                                                      \
        if (cond) {                                           \
            printf("  ok   %s\n", what);                      \
        } else {                                              \
            printf("  FAIL %s\n", what);                      \
            g_failures++;                                     \
        }                                                     \
    } while (0)

int main() {
    printf("report_json\n");

    EventRecord record;
    record.seq = 12;
    record.event.type = EventType::kHardBrake;
    record.event.ts_ms = 1758000000123LL;
    record.event.value = -0.52f;

    // > 落盘格式**必须与 D5 逐字节一致**：/events 与 events.log 的既有测试都是按它写的
    CHECK(EventToJson(record) ==
              "{\"seq\":12,\"type\":\"hard_brake\",\"ts_ms\":1758000000123,\"value\":-0.52}",
          "不带参 = 落盘格式（与 D5 一致）");

    // > 上报格式多 dev 与 boot：幂等键 = dev + boot + seq
    // > （seq 每次重启从 1 开始，只用 dev+seq 会把不同开机的事件判成同一条）
    CHECK(EventToJson(record, "A1B2C3D4E5F6", 3) ==
              "{\"seq\":12,\"dev\":\"A1B2C3D4E5F6\",\"boot\":3,\"type\":\"hard_brake\","
              "\"ts_ms\":1758000000123,\"value\":-0.52}",
          "带参 = 上报格式");

    // 从落盘行里取 seq（补传游标用）
    CHECK(ParseSeqFromJsonLine("{\"seq\":12,\"type\":\"hard_brake\",\"ts_ms\":1,\"value\":-0.52}") == 12,
          "解析 seq=12");
    CHECK(ParseSeqFromJsonLine("{\"seq\":1,\"type\":\"bump\",\"ts_ms\":9,\"value\":1.90}") == 1, "解析 seq=1");
    CHECK(ParseSeqFromJsonLine("not json") == -1, "非 JSON → -1");
    CHECK(ParseSeqFromJsonLine("") == -1, "空行 → -1");
    CHECK(ParseSeqFromJsonLine("{\"type\":\"bump\"}") == -1, "没有 seq → -1");
    CHECK(ParseSeqFromJsonLine("{\"seq\":,\"type\":\"bump\"}") == -1, "seq 后面不是数字 → -1");

    // 运动状态用 ASCII 标识（中文只给屏幕用；手机端/云端要稳定标识）
    CHECK(std::string(MotionStateId(MotionState::kParked)) == "parked", "parked");
    CHECK(std::string(MotionStateId(MotionState::kDriving)) == "driving", "driving");
    CHECK(std::string(MotionStateId(MotionState::kLockedMonitor)) == "locked", "locked");
    CHECK(std::string(MotionStateId(MotionState::kUncalibrated)) == "uncalibrated", "uncalibrated");

    EnvReading env;
    env.ts_ms = 123456;
    env.temp_c = 26.4f;
    env.humidity_pct = 48.0f;
    env.lux = 320;
    env.valid = true;
    env.simulated = true;
    CHECK(EnvToJson(env) == "{\"ts_ms\":123456,\"temp\":26.4,\"humid\":48.0,\"lux\":320,\"src\":\"sim\"}",
          "环境上报 JSON");

    VehicleStatus status;
    status.state = MotionState::kParked;
    status.events_total = 3;
    CHECK(StatusToJson(status, true, 7) == "{\"ts_ms\":0,\"state\":\"parked\",\"net\":\"online\",\"events\":3,\"boot\":7}",
          "状态上报 JSON（在线）");
    CHECK(StatusToJson(status, false, 7) == "{\"ts_ms\":0,\"state\":\"parked\",\"net\":\"offline\",\"events\":3,\"boot\":7}",
          "状态上报 JSON（离线）");

    printf("\n%s（失败 %d）\n", g_failures == 0 ? "全部通过" : "有失败", g_failures);
    return g_failures == 0 ? 0 : 1;
}
```

> `StatusToJson()` 里的 `ts_ms` 取 `EnvReading`/`VehicleStatus` 的采样时间戳；`VehicleStatus.sample.ts_ms` 是最近一帧 IMU 时间。上面的断言假定 `sample.ts_ms == 0`（未初始化），实现必须照这个口径写。

- [ ] **步骤 6：运行确认失败，然后补实现**

```powershell
g++ -std=c++17 -Wall -Wextra -I main/vehicle test/report_json_test.cc main/vehicle/event_json.cc -o build_host/report_json_test.exe
```
预期：编译失败（`MotionStateId` 等未声明）。

`main/vehicle/event_json.h` 加：

```cpp
// 运动状态 → 稳定 ASCII 标识（中文只给屏幕用；手机端/云端要能稳定解析）
const char *MotionStateId(MotionState state);

// 一行事件 JSON。两种口径：
//   EventToJson(record)                        → **落盘格式**（events.log / /events，与 D5 逐字节一致）
//   EventToJson(record, "A1B2…", boot)         → **上报格式**：多出 "dev" 与 "boot"。
//                                               幂等键 = dev + boot + seq（见 docs/BUGS.md 里
//                                               "seq 每次重启从 1 开始"的说明）
std::string EventToJson(const EventRecord &record, const std::string &device_id = std::string(), int32_t boot_id = 0);

// 从一行事件 JSON 里取 "seq" 的值；取不到返回 -1。
// ! 只认 EventToJson() 写出的固定字段顺序，不做通用 JSON 解析（省一个 cJSON 依赖、可主机测）。
int64_t ParseSeqFromJsonLine(const std::string &line);

// 周期上报的 payload（设计文档 §7.1，字段顺序固定）
//   {"ts_ms":123456,"temp":26.4,"humid":48.0,"lux":320,"src":"sim"}
std::string EnvToJson(const EnvReading &reading);
//   {"ts_ms":0,"state":"parked","net":"online","events":3,"boot":7}
std::string StatusToJson(const VehicleStatus &status, bool online, int32_t boot_id);
```

（记得 `#include "environment_sensor.h"`。）

`main/vehicle/event_json.cc` 加实现：

```cpp
const char *MotionStateId(MotionState state) {
    switch (state) {
        case MotionState::kUncalibrated: return "uncalibrated";
        case MotionState::kParked: return "parked";
        case MotionState::kDriving: return "driving";
        case MotionState::kLockedMonitor: return "locked";
    }
    return "unknown";
}

std::string EventToJson(const EventRecord &record, const std::string &device_id, int32_t boot_id) {
    char value_buf[16];
    snprintf(value_buf, sizeof(value_buf), "%.2f", static_cast<double>(record.event.value));

    std::string out;
    out.reserve(device_id.empty() ? 96 : 96 + device_id.size());
    out += "{\"seq\":";
    out += ToDecimal(record.seq);
    if (!device_id.empty()) {
        out += ",\"dev\":\"";
        out += device_id;
        out += "\",\"boot\":";
        out += ToDecimal(boot_id);
    }
    out += ",\"type\":\"";
    out += EventTypeId(record.event.type);
    out += "\",\"ts_ms\":";
    out += ToDecimal(record.event.ts_ms);
    out += ",\"value\":";
    out += value_buf;
    out += "}";
    return out;
}

int64_t ParseSeqFromJsonLine(const std::string &line) {
    const std::string key = "\"seq\":";
    const size_t pos = line.find(key);
    if (pos == std::string::npos) {
        return -1;
    }
    size_t i = pos + key.size();
    bool negative = false;
    if (i < line.size() && line[i] == '-') {
        negative = true;
        i++;
    }
    if (i >= line.size() || line[i] < '0' || line[i] > '9') {
        return -1;
    }
    int64_t value = 0;
    while (i < line.size() && line[i] >= '0' && line[i] <= '9') {
        value = value * 10 + (line[i] - '0');
        i++;
    }
    return negative ? -value : value;
}

std::string EnvToJson(const EnvReading &reading) {
    char number_buf[48];
    std::string out = "{\"ts_ms\":";
    out += ToDecimal(reading.ts_ms);
    snprintf(number_buf, sizeof(number_buf), ",\"temp\":%.1f,\"humid\":%.1f,\"lux\":%d",
             static_cast<double>(reading.temp_c), static_cast<double>(reading.humidity_pct),
             static_cast<int>(reading.lux));
    out += number_buf;
    out += ",\"src\":\"";
    out += reading.simulated ? "sim" : "sensor";
    out += "\"}";
    return out;
}

std::string StatusToJson(const VehicleStatus &status, bool online, int32_t boot_id) {
    std::string out = "{\"ts_ms\":";
    out += ToDecimal(status.sample.ts_ms);
    out += ",\"state\":\"";
    out += MotionStateId(status.state);
    out += "\",\"net\":\"";
    out += online ? "online" : "offline";
    out += "\",\"events\":";
    out += ToDecimal(status.events_total);
    out += ",\"boot\":";
    out += ToDecimal(boot_id);
    out += "}";
    return out;
}
```

> 环境 JSON 的数字格式与测试断言（`temp\":26.4`、`humid\":48.0`）要对齐：`%.1f` 打印 48.0 → `48.0`，与断言一致。

- [ ] **步骤 7：运行测试确认通过**

```powershell
g++ -std=c++17 -Wall -Wextra -I main/vehicle test/report_json_test.cc main/vehicle/event_json.cc -o build_host/report_json_test.exe
build_host\report_json_test.exe
g++ -std=c++17 -Wall -Wextra -I main/vehicle test/event_json_test.cc main/vehicle/event_json.cc -o build_host/event_json_test.exe
build_host\event_json_test.exe
```
预期：两个测试都全绿（**第二个是 D5 的旧测试，用来证明落盘格式没被改坏**）。

- [ ] **步骤 8：主题名宏**

`main/boards/esp32s3/config.h` 末尾加：

```c
/* ---- 巴法云 MQTT（Plan C / D6）----
 * ! 主题名**只允许字母/数字**，且必须先在巴法云控制台创建好才收得到消息
 * ! （https://cloud.bemfa.com/docs/src/index_guild.html）。改成别的名字时，
 * ! 控制台里也要建同名主题，并且同步改这里。
 * > 后三位不是 001~013，不会被归类成"插座/灯泡"这类语音设备类型。
 * > 用户 STM32 工程里的真实主题名就是这种风格：4 个 …004（传感器，上行）+ 1 个 …012（电视，下行控制），
 * > 见 18_Integrated/bsp/esp8266_mqtt.h:16-20。
 */
#define BEMFA_TOPIC_EVENT  "vtevt01"
#define BEMFA_TOPIC_ENV    "vtenv01"
#define BEMFA_TOPIC_STATUS "vtsta01"

/* 服务器地址与端口。
 * > 官方文档（https://cloud.bemfa.com/docs/src/mqtt.html）给的是 bemfa.com:9501（明文）/ 9503（TLS）；
 * ! 用户 STM32 工程**跑通过**的是 mqttv2.bemfa.com:2023（同一账号，见 esp8266_mqtt.h:11-12）。
 * > 默认用官方文档那一组；连不上时**只改这两行**换另一组（一次只动一个变量的教训见 BUG-034）。
 */
#define BEMFA_BROKER_HOST  "bemfa.com"
#define BEMFA_BROKER_PORT  9501
```

- [ ] **步骤 9：构建期从 `.env` 生成凭据头文件**

`main/CMakeLists.txt`：在 `idf_component_register(...)`（`:910`）**之前**插入：

```cmake
# ---- 巴法云凭据（Plan C / D6）----
# ! 密钥只在仓库根的 .env（已被 .gitignore 忽略）。这里读进**构建目录**里的头文件，
# ! 绝不写进 main/ 下的任何 tracked 文件，也不写进 .cc。
# ! 代价：密钥会出现在固件镜像里（可 dump）——演示用途可接受，已在 .env 里写明。
# > BEMFA_UID 是**可选覆盖项**：本项目 .env 里的 BEMFA_APP_ID 本身就是 UID
# > （28 字符、beid_ 前缀，已与用户 STM32 工程跑通的那组逐值比对），所以正常情况下
# > .env 里**不需要** BEMFA_UID；留着它只为将来换账号时能显式指定 client_id。
if(CONFIG_BOARD_TYPE_VEHICLE_ESP32S3)
    set(BEMFA_ENV_FILE "${PROJECT_DIR}/.env")
    set(BEMFA_SECRETS_HEADER "${CMAKE_BINARY_DIR}/bemfa_secrets.h")
    set(BEMFA_APP_ID "")
    set(BEMFA_SECRET_KEY "")
    set(BEMFA_UID "")
    if(EXISTS ${BEMFA_ENV_FILE})
        file(STRINGS "${BEMFA_ENV_FILE}" BEMFA_ENV_LINES REGEX "^BEMFA_")
        foreach(bemfa_line ${BEMFA_ENV_LINES})
            if(bemfa_line MATCHES "^BEMFA_APP_ID=(.+)$")
                set(BEMFA_APP_ID "${CMAKE_MATCH_1}")
            elseif(bemfa_line MATCHES "^BEMFA_SECRET_KEY=(.+)$")
                set(BEMFA_SECRET_KEY "${CMAKE_MATCH_1}")
            elseif(bemfa_line MATCHES "^BEMFA_UID=(.+)$")
                set(BEMFA_UID "${CMAKE_MATCH_1}")
            endif()
        endforeach()
    else()
        message(WARNING "找不到 ${BEMFA_ENV_FILE}：巴法云 MQTT 上报会被跳过（其它功能照常）")
    endif()
    file(WRITE ${BEMFA_SECRETS_HEADER}
"// 本文件由 main/CMakeLists.txt 从仓库根 .env 生成（位于构建目录，已被 gitignore）
// ! 不要提交、不要拷进任何 tracked 文件。
#pragma once
#define BEMFA_APP_ID \"${BEMFA_APP_ID}\"
#define BEMFA_SECRET_KEY \"${BEMFA_SECRET_KEY}\"
#define BEMFA_UID \"${BEMFA_UID}\"
")
    list(APPEND INCLUDE_DIRS "${CMAKE_BINARY_DIR}")
    # > 只打印 AppID 与"是否拿到 SecretKey"，不打印密钥本身
    message(STATUS "巴法云凭据：AppID='${BEMFA_APP_ID}' SecretKey=${BEMFA_SECRET_KEY} UID=${BEMFA_UID} → ${BEMFA_SECRETS_HEADER}")
endif()
```

> `INCLUDE_DIRS` 的追加必须在 `idf_component_register(INCLUDE_DIRS ${INCLUDE_DIRS} ...)` 之前生效——所以这段放在 `:910` 之前，且板级代码用 `#include "bemfa_secrets.h"` 就能取到（引号包含会在 `-I${CMAKE_BINARY_DIR}` 里找）。

- [ ] **步骤 10：写 `BemfaClient`（连接 + 事件上报）**

创建 `main/boards/esp32s3/bemfa_client.h`：

```cpp
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "mqtt.h"
#include "pending_queue.h"
#include "vehicle_service.h"   // EventSink
#include "vehicle_types.h"

class SnapshotStore;
class VehicleService;

// 巴法云 MQTT 上报（Plan C / D6）。
//
// ! 三条硬约束（每条都有 BUGS 里的依据）：
// !   1. 主题名必须先在巴法云控制台建好，且只允许字母/数字 —— 设计文档 §7.1 的
// !      `vehicle/{device_id}/event` 在控制台建不出来（BUG-036）。
// !   2. net_task 的栈放 **PSRAM**（省内部 RAM），所以这个任务**一次都不读 flash**：
// !      游标落 NVS 由 worker 任务在 OnEvent() 里代劳（BUG-024/026）。
// !   3. 积压队列的 80 KB 缓冲显式 `MALLOC_CAP_SPIRAM`：内部通用堆只有 22 KB
// !      且空载就 99.7% 满（BUG-024 补充），std::deque<std::string> 一压就 ENOMEM。
class BemfaClient : public EventSink {
public:
    BemfaClient(VehicleService *vehicle, SnapshotStore *store);
    ~BemfaClient();

    // 建 net_task、起 MQTT 客户端；凭据缺失时返回 false（只告警，不阻断开机）。
    // ! 必须在网络初始化之后调用（板级 StartNetwork），且调用方栈要在**内部 RAM**：
    // ! 这里会读 events.log 做开机回填。
    bool Start();

    // EventSink：只入队 + 顺手持久化游标（本函数运行在 worker 任务里，栈在内部 RAM）
    void OnEvent(const vehicle::EventRecord &record) override;

    struct Stats {
        uint32_t published = 0;   // publish 返回成功（= 已交给 esp-mqtt，不等于已被对端收到）
        uint32_t failed = 0;      // 未连接或 publish 失败
        uint32_t dropped = 0;     // 队列满丢最旧
        int32_t queued = 0;
        bool connected = false;
        bool enabled = false;     // 凭据缺失时为 false
    };
    Stats stats() const;
    std::string StatsJson() const;   // 给 MCP 工具 self.vehicle.net

    bool enabled() const { return enabled_; }
    bool connected() const { return connected_.load(); }

private:
    static void NetTaskEntry(void *arg);
    void NetLoop();
    void DrainQueue();
    bool Publish(const std::string &topic, const std::string &payload, int qos);
    void PublishEnv();
    void PublishStatus();
    void PersistCursor();   // ! 只能在栈位于内部 RAM 的任务里调（写 NVS）
    static std::string DeviceId();

    static constexpr int kNetStackBytes = 6144;
    static constexpr size_t kQueueCapacity = 500;
    static constexpr uint32_t kLoopPeriodMs = 200;
    static constexpr int64_t kEnvPeriodMs = 30000;
    static constexpr int64_t kStatusPeriodMs = 60000;
    static constexpr int kEventQos = 1;   // ! 巴法云不支持 QoS2（会被强制下线）
    static constexpr int kPeriodicQos = 0;

    VehicleService *vehicle_ = nullptr;
    SnapshotStore *store_ = nullptr;

    std::unique_ptr<Mqtt> mqtt_;
    std::unique_ptr<vehicle::PendingQueue> queue_;
    char *queue_buffer_ = nullptr;
    mutable std::mutex mutex_;          // 保护 counter_ 与 cursor_
    Stats counters_{};

    std::string device_id_;
    std::string topic_event_;
    std::string topic_env_;
    std::string topic_status_;

    std::atomic<bool> connected_{false};
    std::atomic<bool> enabled_{false};
    std::atomic<int64_t> last_sent_seq_{0};   // 已成功交给 esp-mqtt 的最大事件序号（RAM）
    int32_t boot_id_ = 0;
    TaskHandle_t net_task_ = nullptr;
};
```

创建 `main/boards/esp32s3/bemfa_client.cc`：

```cpp
#include "bemfa_client.h"

#include <cctype>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "bemfa_secrets.h"
#include "board.h"
#include "config.h"
#include "event_json.h"
#include "settings.h"
#include "snapshot_store.h"
#include "system_info.h"

#define TAG "BemfaClient"

namespace {

// > broker 地址/端口与主题名都在 config.h 里（BEMFA_BROKER_* / BEMFA_TOPIC_*），
// > 便于按现场情况换端点，不用动这个文件。
// > keepalive 与用户 STM32 工程那份跑通的实现一致（bsp/esp8266_mqtt.c:361 的 60 s）。
constexpr int kKeepAliveSeconds = 60;

// > ulStackDepth 的单位是 StackType_t 字数，必须除以 sizeof(StackType_t)（BUG-002）。
bool CreatePsramTask(TaskFunction_t entry, const char *name, int stack_bytes, void *arg, UBaseType_t priority,
                     BaseType_t core, TaskHandle_t *out) {
    StackType_t *stack = static_cast<StackType_t *>(heap_caps_malloc(stack_bytes, MALLOC_CAP_SPIRAM));
    StaticTask_t *tcb = static_cast<StaticTask_t *>(heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL));
    if (stack == nullptr || tcb == nullptr) {
        ESP_LOGE(TAG, "任务 %s 的栈/TCB 分配失败（PSRAM 或内部 RAM 余量不足？）", name);
        heap_caps_free(stack);
        heap_caps_free(tcb);
        return false;
    }
    *out = xTaskCreateStaticPinnedToCore(entry, name, static_cast<uint32_t>(stack_bytes) / sizeof(StackType_t), arg,
                                         priority, stack, tcb, core);
    return *out != nullptr;
}

}  // namespace

BemfaClient::BemfaClient(VehicleService *vehicle, SnapshotStore *store) : vehicle_(vehicle), store_(store) {
    topic_event_ = BEMFA_TOPIC_EVENT;
    topic_env_ = BEMFA_TOPIC_ENV;
    topic_status_ = BEMFA_TOPIC_STATUS;
}

BemfaClient::~BemfaClient() {
    if (mqtt_) {
        mqtt_->Disconnect();
    }
    heap_caps_free(queue_buffer_);
    queue_buffer_ = nullptr;
}

std::string BemfaClient::DeviceId() {
    // > device_id 全程只此一处定义：MAC 去掉分隔符、转大写，12 位十六进制（设计文档 §7.1）
    const std::string mac = SystemInfo::GetMacAddress();
    std::string out;
    out.reserve(12);
    for (char c : mac) {
        if (c == ':' || c == '-' || c == ' ') {
            continue;
        }
        out.push_back(static_cast<char>(toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

bool BemfaClient::Start() {
    if (BEMFA_APP_ID[0] == '\0' || BEMFA_SECRET_KEY[0] == '\0') {
        ESP_LOGW(TAG, "没有巴法云凭据（构建期没读到 .env 里的 BEMFA_APP_ID / BEMFA_SECRET_KEY），MQTT 上报不启动");
        enabled_ = false;
        return false;
    }

    queue_buffer_ = static_cast<char *>(
        heap_caps_malloc(kQueueCapacity * vehicle::PendingQueue::kSlotBytes, MALLOC_CAP_SPIRAM));
    if (queue_buffer_ == nullptr) {
        ESP_LOGE(TAG, "积压队列缓冲分配失败（PSRAM %u B）",
                 static_cast<unsigned>(kQueueCapacity * vehicle::PendingQueue::kSlotBytes));
        return false;
    }
    queue_ = std::make_unique<vehicle::PendingQueue>(queue_buffer_, static_cast<int>(kQueueCapacity));

    device_id_ = DeviceId();

    // > boot_id：seq 每次重启从 1 重新开始（EventHistory 在 RAM 里），所以幂等键
    // > 必须带 boot —— 只用 device_id+seq 会把不同开机的同号事件判成同一条。
    // ! 这几行写 NVS，必须跑在内部 RAM 栈上：本函数由板级 StartNetwork() 调用（main 任务）。
    {
        Settings settings("vehicle", true);
        boot_id_ = settings.GetInt("boot_id", 0) + 1;
        settings.SetInt("boot_id", boot_id_);
        last_sent_seq_.store(settings.GetInt("sent_seq", 0));
    }

    // > 开机回填：上次运行期间没发出去的（seq > sent_seq）从 events.log 尾部捞回来。
    // > 只回填尾部 8 KB（约 130 条）—— 整个 events.log 可能 256 KB，而 std::string 走
    // > 默认分配器（内部堆 22 KB），一次性读全文件必然失败。
    if (store_ != nullptr) {
        std::string tail;
        if (store_->ReadEventsAfterSeq(last_sent_seq_.load(), tail)) {
            size_t begin = 0;
            int restored = 0;
            while (begin < tail.size()) {
                const size_t end = tail.find('\n', begin);
                const std::string line = tail.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
                begin = (end == std::string::npos) ? tail.size() : end + 1;
                if (!line.empty() && queue_->Push(line)) {
                    restored++;
                }
            }
            if (restored > 0) {
                ESP_LOGI(TAG, "开机回填 %d 条未上报事件（sent_seq=%d，本次 boot=%d）", restored,
                         static_cast<int>(last_sent_seq_.load()), static_cast<int>(boot_id_));
            }
        }
    }

    if (!CreatePsramTask(NetTaskEntry, "bemfa_net", kNetStackBytes, this, 2, 1, &net_task_)) {
        return false;
    }
    enabled_ = true;
    ESP_LOGI(TAG, "巴法云上报已启动：设备 %s，主题 %s / %s / %s，积压容量 %u 条（缓冲在 PSRAM）", device_id_.c_str(),
             topic_event_.c_str(), topic_env_.c_str(), topic_status_.c_str(),
             static_cast<unsigned>(kQueueCapacity));
    return true;
}

void BemfaClient::NetTaskEntry(void *arg) {
    static_cast<BemfaClient *>(arg)->NetLoop();
}

void BemfaClient::NetLoop() {
    // > Connect() 内部会等最多 MQTT_CONNECT_TIMEOUT_MS 的连接结果，所以放在这个任务里做，
    // > 不占 main 任务（Application::Start() 的时序）。失败也没关系：esp-mqtt 打开自动重连，
    // > 之后会在后台一直重试，IsConnected() 由事件回调更新。
    mqtt_ = Board::GetInstance().GetNetwork()->CreateMqtt();
    if (mqtt_ == nullptr) {
        ESP_LOGE(TAG, "CreateMqtt() 返回空，上报任务退出");
        vTaskDelete(nullptr);
        return;
    }
    mqtt_->OnConnected([this]() {
        connected_.store(true);
        ESP_LOGI(TAG, "巴法云已连接（积压 %d 条待补传）", queue_ ? queue_->size() : 0);
    });
    mqtt_->OnDisconnected([this]() {
        connected_.store(false);
        ESP_LOGW(TAG, "巴法云断开，事件继续入本地队列（积压 %d 条）", queue_ ? queue_->size() : 0);
    });
    mqtt_->OnError([this](const std::string &error) { ESP_LOGW(TAG, "巴法云 MQTT 错误：%s", error.c_str()); });
    mqtt_->SetKeepAlive(kKeepAliveSeconds);

    // ! 认证口径**照用户 STM32 工程那份跑通的实现**（18_Integrated/bsp/esp8266_mqtt.h:13-15 与 .c:359-382）：
    // !   client_id = username = UID，password = secretKey；keepalive 60 s、clean session、无 will。
    // ! 我们 .env 里的 BEMFA_APP_ID 就是那个 UID 值（已逐值比对确认），所以 client_id 与 username 都用它。
    // ! 巴法云文档的"方式一（私钥当 client_id）/方式二（username=appID, password=secretKey）"，
    // ! 在这份跑通的配置里是同一组值；照跑通的那一份写最省事。
    // ! 注意：同一 UID 同时只允许一个连接，别和 STM32 网关同时在线（会互相顶下线）。
    const std::string client_id = (BEMFA_UID[0] != '\0') ? std::string(BEMFA_UID) : std::string(BEMFA_APP_ID);
    if (!mqtt_->Connect(BEMFA_BROKER_HOST, BEMFA_BROKER_PORT, client_id, client_id, BEMFA_SECRET_KEY)) {
        ESP_LOGW(TAG, "首次连接巴法云失败（%s:%d，client_id=%s）；esp-mqtt 会在后台自动重连",
                 BEMFA_BROKER_HOST, BEMFA_BROKER_PORT, client_id.c_str());
    }

    int64_t last_env_ms = 0;
    int64_t last_status_ms = 0;
    int64_t last_log_ms = 0;
    while (true) {
        const int64_t now_ms = esp_timer_get_time() / 1000;
        DrainQueue();
        if (connected_.load()) {
            if (now_ms - last_env_ms >= kEnvPeriodMs) {
                last_env_ms = now_ms;
                PublishEnv();
            }
            if (now_ms - last_status_ms >= kStatusPeriodMs) {
                last_status_ms = now_ms;
                PublishStatus();
            }
        }
        // > 每 30 s 打一条统计：D7 的"上报成功率"就是从这里数出来的。
        if (now_ms - last_log_ms >= 30000) {
            last_log_ms = now_ms;
            const Stats s = stats();
            ESP_LOGI(TAG, "上报统计 已发=%u 失败=%u 丢弃=%u 积压=%d 连接=%s", static_cast<unsigned>(s.published),
                     static_cast<unsigned>(s.failed), static_cast<unsigned>(s.dropped), static_cast<int>(s.queued),
                     s.connected ? "是" : "否");
        }
        vTaskDelay(pdMS_TO_TICKS(kLoopPeriodMs));
    }
}

void BemfaClient::DrainQueue() {
    if (!connected_.load() || !queue_ || !mqtt_) {
        return;
    }
    // > 一次最多发 5 条：别把这个任务占太久（它还要发周期包、打统计）。
    for (int i = 0; i < 5; i++) {
        std::string payload;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!queue_->Pop(payload)) {
                return;
            }
        }
        const int64_t seq = vehicle::ParseSeqFromJsonLine(payload);
        // > 补传时补上本次开机的 dev/boot：JSON 里本来只有 seq/type/ts_ms/value，
        // > 但幂等键要 dev+boot+seq，所以回填上来的行重新拼一次。
        std::string outbound = payload;
        const std::string outbound = vehicle::AddReportFields(payload, device_id_, boot_id_);
        if (!Publish(topic_event_, outbound, kEventQos)) {
            // > 刚好断线：放回队尾等下一次（**可能改变同批顺序**，但事件带 seq，接收端可自行排序；
            // > 比"弹出来直接丢掉"好）。
            std::lock_guard<std::mutex> lock(mutex_);
            queue_->Push(payload);
            return;
        }
        if (seq >= 0) {
            last_sent_seq_.store(seq);
        }
    }
}

bool BemfaClient::Publish(const std::string &topic, const std::string &payload, int qos) {
    if (!mqtt_ || !connected_.load()) {
        std::lock_guard<std::mutex> lock(mutex_);
        counters_.failed++;
        return false;
    }
    const bool ok = mqtt_->Publish(topic, payload, qos);
    std::lock_guard<std::mutex> lock(mutex_);
    if (ok) {
        counters_.published++;
    } else {
        counters_.failed++;
    }
    return ok;
}

void BemfaClient::PublishEnv() {
    if (vehicle_ == nullptr) {
        return;
    }
    const vehicle::EnvReading env = vehicle_->env();
    // > 读数无效时也发（手机上能看到"没数据"这件事），但 src 会标明模拟源
    Publish(topic_env_, vehicle::EnvToJson(env), kPeriodicQos);
}

void BemfaClient::PublishStatus() {
    if (vehicle_ == nullptr) {
        return;
    }
    const vehicle::VehicleStatus status = vehicle_->Status();
    Publish(topic_status_, vehicle::StatusToJson(status, true, boot_id_), kPeriodicQos);
}

void BemfaClient::OnEvent(const vehicle::EventRecord &record) {
    // ! 本函数运行在 worker 任务里（VehicleService::WorkerTaskLoop → DispatchEvent），
    // ! 那个任务的栈在**内部 RAM**，所以这里可以写 NVS。net_task 的栈在 PSRAM，写 NVS 会复位。
    if (!enabled_ || !queue_) {
        return;
    }
    const std::string payload = vehicle::EventToJson(record, device_id_, boot_id_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!queue_->Push(payload)) {
            counters_.dropped++;
        }
        // > 顺手把游标落盘：此刻的 last_sent_seq_ 是"上一条已确认交给 esp-mqtt 的序号"，
        // > 保守方向是"可能重发 1~2 条"，绝不会漏。事件频率低，NVS 写入次数可以忽略。
        counters_.queued = queue_->size();
    }
    PersistCursor();
}

void BemfaClient::PersistCursor() {
    // ! 只能在栈位于内部 RAM 的任务里调（写 NVS 会关 cache，见 BUG-024）
    Settings settings("vehicle", true);
    settings.SetInt("sent_seq", static_cast<int32_t>(last_sent_seq_.load()));
}

BemfaClient::Stats BemfaClient::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Stats out = counters_;
    out.connected = connected_.load();
    out.enabled = enabled_.load();
    out.queued = queue_ ? queue_->size() : 0;
    out.dropped = queue_ ? static_cast<uint32_t>(queue_->dropped()) : counters_.dropped;
    return out;
}

std::string BemfaClient::StatsJson() const {
    const Stats s = stats();
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"enabled\":%s,\"connected\":%s,\"published\":%u,\"failed\":%u,\"queued\":%d,\"dropped\":%u}",
             s.enabled ? "true" : "false", s.connected ? "true" : "false", static_cast<unsigned>(s.published),
             static_cast<unsigned>(s.failed), static_cast<int>(s.queued), static_cast<unsigned>(s.dropped));
    return std::string(buf);
}
```

并在 `main/vehicle/event_json.{h,cc}` 补一个开机回填用的纯逻辑函数（**同时补测试断言**，见下面步骤 11）：

```cpp
// 给一行**落盘格式**的事件 JSON 补上 dev/boot（开机回填补传时用）：
//   {"seq":12,"type":"bump",…} → {"seq":12,"dev":"A1B2…","boot":3,"type":"bump",…}
// ! 只认 EventToJson() 写出的固定格式；已经有 dev 或解析不出 seq 时原样返回。
std::string AddReportFields(const std::string &line, const std::string &device_id, int32_t boot_id);
```

```cpp
std::string AddReportFields(const std::string &line, const std::string &device_id, int32_t boot_id) {
    if (device_id.empty() || line.find("\"dev\"") != std::string::npos || ParseSeqFromJsonLine(line) < 0) {
        return line;
    }
    const size_t comma = line.find(',', line.find("\"seq\":"));
    if (comma == std::string::npos) {
        return line;
    }
    std::string out;
    out.reserve(line.size() + device_id.size() + 24);
    out += line.substr(0, comma);
    out += ",\"dev\":\"";
    out += device_id;
    out += "\",\"boot\":";
    out += ToDecimal(boot_id);
    out += line.substr(comma);
    return out;
}
```

- [ ] **步骤 11：给回填函数补测试断言**

在 `test/report_json_test.cc` 的 `main()` 末尾（`printf("\n%s…")` 之前）插入：

```cpp
    // 开机回填：给落盘行补 dev/boot
    const std::string log_line = "{\"seq\":12,\"type\":\"hard_brake\",\"ts_ms\":1758000000123,\"value\":-0.52}";
    CHECK(AddReportFields(log_line, "A1B2C3D4E5F6", 3) ==
              "{\"seq\":12,\"dev\":\"A1B2C3D4E5F6\",\"boot\":3,\"type\":\"hard_brake\","
              "\"ts_ms\":1758000000123,\"value\":-0.52}",
          "补 dev/boot 后与 EventToJson(record, dev, boot) 同构");
    CHECK(AddReportFields(AddReportFields(log_line, "A1B2C3D4E5F6", 3), "A1B2C3D4E5F6", 3) ==
              AddReportFields(log_line, "A1B2C3D4E5F6", 3),
          "重复调用是幂等的（不会补两次）");
    CHECK(AddReportFields("not json", "A1B2C3D4E5F6", 3) == "not json", "非 JSON 原样返回");
    CHECK(AddReportFields(log_line, "", 3) == log_line, "device_id 为空时原样返回");
```

重新编译运行：

```powershell
g++ -std=c++17 -Wall -Wextra -I main/vehicle test/report_json_test.cc main/vehicle/event_json.cc -o build_host/report_json_test.exe
build_host\report_json_test.exe
```
预期：全部 `ok`。

- [ ] **步骤 12：板级接线（BemfaClient + MCP 工具）**

`esp32s3_board.cc`：
1. 顶部加 `#include "bemfa_client.h"`；
2. 成员加 `BemfaClient* bemfa_ = nullptr;`；
3. 构造函数里，`voice_command_` 之后插入：

```cpp
        // > 巴法云上报：**只构造对象**，与 HTTP 同理——Start() 里要读 flash（开机回填 events.log），
        // > 而 lwIP 与 socket 也要等网络初始化，所以真正的启动在 StartNetwork()。
        bemfa_ = new BemfaClient(vehicle_, snapshot_store_);
        vehicle_->AddEventSink(bemfa_);
```

4. `StartNetwork()` 里，HTTP 之后加：

```cpp
        if (bemfa_ != nullptr && !bemfa_->Start()) {
            ESP_LOGW(TAG, "巴法云 MQTT 上报未启动（凭据缺失或内存不足），其它功能不受影响");
        }
```

5. `InitializeTools()` 里加第 5 个 `self.vehicle.*` 工具（放在 `self.vehicle.capture` 之后）：

```cpp
        mcp_server.AddTool("self.vehicle.net",
            "读取巴法云上报统计：是否启用、是否已连接、已发条数、失败条数、积压条数、丢弃条数。",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                const std::string result = bemfa_ != nullptr ? bemfa_->StatsJson() : std::string("{\"enabled\":false}");
                ESP_LOGI(TAG, "工具 self.vehicle.net → %s", result.c_str());
                return result;
            });
```

- [ ] **步骤 13：构建并确认凭据真的被读到**

```powershell
cmd /c "... && idf.py reconfigure && idf.py build > build\d6_task5_build.log 2>&1"
Select-String -Path build\d6_task5_build.log -Pattern "巴法云凭据|bemfa_client.cc.obj|warning:|error:|Project build complete"
```
预期：日志里有 `巴法云凭据：AppID='…' SecretKey=… UID=…`（**SecretKey 的值会原样出现在构建日志里**——日志文件在 `build/`（gitignored），但仍不要把这一段贴进任何文档或提交信息），有 `bemfa_client.cc.obj`，零 warning，`Project build complete`。

- [ ] **步骤 14：烧录 + 真机验证"手机能看到事件"（用户操作）**

```powershell
cmd /c "... && idf.py -p COM10 flash > build\d6_task5_flash.log 2>&1"
pwsh -File build\capture_once.ps1 -Seconds 120 -Out build\d6_task5_mqtt.log
Select-String -Path build\d6_task5_mqtt.log -Pattern "BemfaClient"
```
预期串口：
```
I (…) BemfaClient: 巴法云上报已启动：设备 A261… 主题 vtevt01 / vtenv01 / vtsta01，积压容量 500 条（缓冲在 PSRAM）
I (…) BemfaClient: 巴法云已连接（积压 0 条待补传）
I (…) BemfaClient: 上报统计 已发=2 失败=0 丢弃=0 积压=0 连接=是
```

然后请用户：手持板子晃出一次急刹车 → 手机巴法云小程序打开 `vtevt01` 主题 → **能看到那条事件 JSON**；`vtenv01` 每 30 s 一条、`vtsta01` 每 60 s 一条。

**连不上时的排查顺序（照着做，别乱试）**：
1. 串口是不是 `BemfaClient: 巴法云 MQTT 错误：…`？先确认板子拿到了 IP（`Got IP:`）。
2. 主题名是否与控制台**逐字符一致**（含大小写）；主题没建的话数据发出去了也看不到。
3. **连不上 / 认证失败**：`.env` 的两个值已与 STM32 工程逐值比对过、是跑通的那一组，所以**不要先怀疑凭据**；先**只换端点这一个变量**——把 `main/boards/esp32s3/config.h` 的 `BEMFA_BROKER_HOST/PORT` 从 `bemfa.com:9501` 改成用户 STM32 工程跑通的那组 `mqttv2.bemfa.com:2023`，重新 `idf.py build && idf.py flash`。
4. 换成 2023 也不通 → 确认**同一 UID 没有第二个客户端在线**（同一个 UID 两个连接会互相顶下线；如果 STM32 网关正跑着，让用户先停掉它），并确认控制台账号没被禁用。
5. 仍然失败 → 记录确切现象与串口原文，按 `docs/BUGS.md` 追加一条（**没修好也要记**）。

- [ ] **步骤 15：展示并提交（等用户确认）**

```bash
git add main/boards/esp32s3/config.h main/boards/esp32s3/bemfa_client.h main/boards/esp32s3/bemfa_client.cc \
        main/boards/esp32s3/esp32s3_board.cc main/boards/esp32s3/vehicle_service.h main/boards/esp32s3/vehicle_service.cc \
        main/vehicle/pending_queue.h main/vehicle/pending_queue.cc main/vehicle/event_json.h main/vehicle/event_json.cc \
        test/pending_queue_test.cc test/report_json_test.cc main/CMakeLists.txt
git status --short
git diff --cached --stat
```
提交前**自查一遍**：`git diff --cached | Select-String -Pattern "BEMFA_|SecretKey|AppID"` 应当**只**出现在读取 `.env` 的 CMake 代码与宏名里，**不得**出现真实密钥值。

建议 commit message：`feat: 巴法云 MQTT 事件上报与积压队列（D6 任务 5）`

---

## 任务 6：断网补传与开机回填

**交付物：** 断网期间事件进本地队列（≥500 条容量）、恢复后按序补传；重启后把上次没发出去的从 `events.log` 捞回来重发。

> **执行顺序提醒**：本任务步骤 1 的 `SnapshotStore::ReadEventsAfterSeq()` 是**任务 5 的硬依赖**（`BemfaClient::Start()` 调它）。严格按顺序执行时，请在任务 5 步骤 13（构建）**之前**先完成本任务步骤 1-2。

**文件：**
- 修改：`main/boards/esp32s3/snapshot_store.h`、`.cc:149-194`

- [ ] **步骤 1：实现 `ReadEventsAfterSeq()`**

`snapshot_store.h` 加公有方法（放在 `ReadRecentEvents` 之后）：

```cpp
    // 读 events.log **尾部 8 KB** 里 seq > after_seq 的行（按原顺序），供开机补传。
    // ! 只窗口读尾部：整个 events.log 可能 256 KB，而 std::string 走默认分配器（内部堆只有
    // ! 22 KB，见 BUG-024 补充），一次性读全文件必然分配失败。所以"跨重启补传"的上限
    // ! 大约是最近 130 条事件（8 KB / 约 60 B 一行）——够覆盖一次断电/重启，写进验收局限一节。
    // ! 只能在**栈位于内部 RAM** 的任务里调（读 flash，见 BUG-024 / BUG-026）。
    bool ReadEventsAfterSeq(int64_t after_seq, std::string &out);
```

`snapshot_store.cc` 加实现（复用 `ReadRecentEvents()` 的尾部窗口逻辑，再加一层 seq 过滤）：

```cpp
bool SnapshotStore::ReadEventsAfterSeq(int64_t after_seq, std::string &out) {
    std::string tail;
    // > 先拿尾部窗口（max_lines 给一个大值 = 不按行数裁剪，只保留 8 KB 窗口）
    if (!ReadRecentEvents(100000, tail)) {
        return false;
    }
    out.clear();
    size_t begin = 0;
    int kept = 0;
    while (begin < tail.size()) {
        const size_t end = tail.find('\n', begin);
        const std::string line =
            tail.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        begin = (end == std::string::npos) ? tail.size() : end + 1;
        if (line.empty()) {
            continue;
        }
        const int64_t seq = vehicle::ParseSeqFromJsonLine(line);
        if (seq > after_seq) {
            out += line;
            out += '\n';
            kept++;
        }
    }
    ESP_LOGI(TAG, "尾部窗口 %u B 里 seq > %d 的有 %d 条", static_cast<unsigned>(tail.size()),
             static_cast<int>(after_seq), kept);
    return !out.empty();
}
```

> `ReadRecentEvents()` 内部用的是 `std::string tail; tail.resize(window)`（最大 8 KB）——这个量级在内部堆里已经跑通过（D5 的开机预热就在用同一条路径）。

- [ ] **步骤 2：构建**

```powershell
cmd /c "... && idf.py build > build\d6_task6_build.log 2>&1"
Select-String -Path build\d6_task6_build.log -Pattern "snapshot_store.cc.obj|warning:|error:|Project build complete"
```
预期：零 warning，`Project build complete`。

- [ ] **步骤 3：断网补传真机验证（用户操作）**

准备：手机热点开着（板子连它），串口抓取 **10 分钟**（`build/capture_once.ps1 -Seconds 600`）。

1. 让用户**关掉手机热点**（板子掉线，串口会打 `BemfaClient: 巴法云断开…`）
2. 断网期间手持板子做出 **≥10 次**事件（急加速/急刹车/急转弯/晃板子），确认串口仍在记事件（`VehicleService: 事件 #…`）
3. 等 1~2 分钟，确认串口统计里的 `积压=` 在增长
4. 让用户**重新打开热点**，板子重连
5. 观察串口：`巴法云已连接（积压 N 条待补传）` → 随后的 `上报统计 已发=… 积压=0`

```powershell
Select-String -Path build\d6_task6_net.log -Pattern "BemfaClient|事件 #"
```
预期：重连后积压条数**在 1 分钟内归零**，且手机小程序里能看到断网期间的那些事件（**顺序按 seq 递增**，允许极少数重发）。

- [ ] **步骤 4：重启补传验证（用户操作）**

1. 让用户在**断网状态**（热点关着）下做出 3 次事件，然后**直接断电重启**（拔插 USB）
2. 等板子起来、WiFi 连上（此期间热点仍关着）
3. 打开热点，观察串口
```powershell
Select-String -Path build\d6_task6_restart.log -Pattern "开机回填|BemfaClient: 巴法云已连接|上报统计"
```
预期：出现 `BemfaClient: 开机回填 3 条未上报事件（sent_seq=…，本次 boot=…）`；重连后积压归零；手机端能看到这 3 条。

> 若回填条数**多于**断网期间的实际条数（例如 5 条而不是 3 条），说明游标落后于实际发送进度（`PersistCursor()` 只在**新事件**到来时写 NVS 的副作用）——这时**不要**改代码，先在验收记录里如实记下"可能重复 1~2 条"，因为重复比丢失安全，且幂等键已经带了 `boot`。

- [ ] **步骤 5：记录 BUG（本步骤必做）**

在 `docs/BUGS.md` **第六节**追加：

```
### BUG-037 计划书 §9 的"幂等键 = 设备 ID + 事件序号"在重启后会误判重复
- 现象/依据：EventHistory 的 seq 是 RAM 里的单调序号，**每次重启从 1 重新开始**
  （main/vehicle/event_history.h:16、vehicle_service.cc:128 history_.Append）。
  按计划书 §9 的键，第 1 次开机的 seq=1 与第 2 次开机的 seq=1 会被判成同一条
- 修法：幂等键改为 device_id + boot_id + seq；boot_id 每次开机在 NVS 里 +1
  （bemfa_client.cc 的 Start()），并在事件上报 JSON 里带 "boot"
- 影响面：只影响"补传去重"这一条口径；事件本身、落盘格式都没变
- 出处：Plan C 计划任务 5/6
```

- [ ] **步骤 6：展示并提交（等用户确认）**

```bash
git add main/boards/esp32s3/snapshot_store.h main/boards/esp32s3/snapshot_store.cc docs/BUGS.md
git status --short
git diff --cached --stat
```
建议 commit message：`feat: 断网队列与开机补传（D6 任务 6）`

---

## 任务 7：D6 收尾联调（三条链路并行）

**交付物：** 上报 / 语音 / 预览与抓拍**同时**跑 20 分钟不出问题；D6 的验收记录成稿。

**文件：**
- 创建：`docs/验收记录/D6-D7-MQTT与语音与联调.md`（D6 一节）

- [ ] **步骤 1：写验收记录骨架**

创建 `docs/验收记录/D6-D7-MQTT与语音与联调.md`，先写标题、日期、HEAD、环境（网络、串口号、固件版本）与三张空表（D6 验收项 / D7 指标 / 局限）。格式照 `docs/验收记录/D3-D5-环境与界面与抓拍.md`。

- [ ] **步骤 2：并行负载 20 分钟（用户操作 + 本机抓取）**

```powershell
pwsh -File build\capture_once.ps1 -Seconds 1200 -Out build\d6_integration.log
```
让用户在这 20 分钟里做：

| 事项 | 频率 |
|---|---|
| 停在实时画面页（持续预览） | 全程 |
| 手机浏览器每 5 s 刷新 `http://<IP>:8080/` | 全程 |
| 说「车内温度」「设备状态」各一次 | 每 2 分钟 |
| 制造一次行车事件 | 每 3 分钟 |
| 说「重新抓拍」 | 每 5 分钟 |
| 手机小程序保持打开 | 全程 |

- [ ] **步骤 3：核对结果（本机）**

```powershell
Select-String -Path build\d6_integration.log -Pattern "预览实测|VC_DONE|上报统计|抓拍完成|abort|Guru Meditation|assert failed|rst:0x" |
    Select-Object -Last 40
```
记录三件事：
1. **有没有复位**：数 `rst:0x` 的次数，并**按复位原因分类**（`rst:0x1`=上电、`rst:0x3`=软件、`rst:0x15`=USB 串口复位…）。按 BUG-006 末尾的三条纪律，**只报总数不算测量**。
2. **内部 RAM**：`SystemInfo: free sram: … minimal sram: …` 的最低值（与 F3 的基线比）。
3. **预览 fps**：`VehicleUi: 预览实测 x.x fps` 最后一次的读数（基线 11.8~15.0 fps）。

- [ ] **步骤 4：填 D6 验收表**

在验收记录里逐项写结论（**只写实测到的，没测的写"未测"**）：

| D6 验收项（计划书 §12） | 结论 | 证据 |
|---|---|---|
| 手机远程看到事件 | ✅/❌ | `build/d6_task5_mqtt.log` + 小程序截图（用户提供） |
| 8 条命令词可识别并播报 | ✅/❌ | `build/d6_task4_voice.log` 的 `VC_DONE` |
| ≥500 条断网积压能力 | ✅/❌ | `PendingQueue` 单测 + 容量日志（**真机没灌满 500 条要写明**） |
| 重启后补传 | ✅/❌ | `build/d6_task6_restart.log` 的 `开机回填 N 条` |
| 三条链路并行 20 min | ✅/⚠️ | `build/d6_integration.log` + 复位分类 |
| 断网时监测/预览/命令词/播报照常（计划书 §9） | ✅/❌ | 任务 6 步骤 3 的断网窗口（`build/d6_task6_net.log`）里 `VC_DONE`/`预览实测` 仍在打 |

- [ ] **步骤 5：展示并提交（等用户确认）**

```bash
git add "docs/验收记录/D6-D7-MQTT与语音与联调.md"
git status --short
git diff --cached --stat
```
建议 commit message：`docs: 记录 D6 上报与语音联调验收数据`

---

## 任务 8：D7 量化指标测量

**交付物：** 计划书 §1.1 的指标表填上实测值（命令词识别率、MQTT 上报成功率、2 h 无复位、内存余量），以及与设计文档 §10.3 修正口径的对照。

**文件：**
- 创建：`build/capture_long.ps1`、`build/count_vc.ps1`（都在 `build/`，gitignored）
- 修改：`docs/验收记录/D6-D7-MQTT与语音与联调.md`（D7 指标表）

- [ ] **步骤 1：写两个测量脚本**

创建 `build/capture_long.ps1`（**只开一次端口、不重连**；不拉 DTR/RTS，避免把板子复位或钳死 I2C，BUG-005）：

```powershell
param(
    [int]$Seconds = 7200,
    [string]$Port = "COM10",
    [string]$Out = "build\long.log"
)
# > 长时间抓取：只开一次串口，中途不重连（重连会拉 RTS → 复位 → 测量作废，见 BUG-005）
$sp = New-Object System.IO.Ports.SerialPort $Port, 115200, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One
$sp.DtrEnable = $false
$sp.RtsEnable = $false
$sp.ReadTimeout = 500
$sp.Open()
$sw = New-Object System.IO.StreamWriter($Out, $false, [System.Text.Encoding]::UTF8)
$deadline = (Get-Date).AddSeconds($Seconds)
Write-Host "抓取 $Seconds s → $Out（$Port）"
while ((Get-Date) -lt $deadline) {
    try {
        $line = $sp.ReadLine()
        $sw.WriteLine($line)
        # > 内存/统计这类关键行立刻落盘：中途被打断也能拿到数据
        if ($line -match "free sram|上报统计|VC_") { $sw.Flush() }
    } catch [System.TimeoutException] { }
}
$sp.Close()
$sw.Close()
Write-Host "抓取结束：$Out"
```

创建 `build/count_vc.ps1`：

```powershell
param([string]$Log = "build\long.log")
# > 命令词命中计数：VC_ACTION 由 esp32s3_board.cc 的语音回调打印（每条命令一次）
$hits = Get-Content $Log |
    Select-String -Pattern "VC_ACTION action=(\w+)" |
    ForEach-Object { $_.Matches[0].Groups[1].Value }
if (-not $hits) { Write-Host "没有命中记录"; exit }
Write-Host "总命中 $($hits.Count) 次"
$hits | Group-Object | Sort-Object Name | ForEach-Object { "{0,-10} {1}" -f $_.Name, $_.Count }
```

- [ ] **步骤 2（用户操作）：命令词识别率测量（8 条 × 10 次）**

```powershell
pwsh -File build\capture_long.ps1 -Seconds 900 -Out build\d7_voice.log
```
让用户按**固定顺序、固定语速**说：`车内温度 / 车内湿度 / 光照多少 / 有没有人 / 设备状态 / 播放提示音 / 重新抓拍 / 锁车`，每条**说 10 次**，每次之间间隔 3 s（说错了不补，如实计入分母）。

```powershell
pwsh -File build\count_vc.ps1 -Log build\d7_voice.log
```
预期：每条的命中次数为 0~10（分母固定 10）。识别率 = 该条命中次数 / 10，写入验收记录。
**指标口径（计划书 §1.1 是 ≥90% 且"安静车内环境"）**：本测量在室内桌面环境做，如实写明环境；若某条低于 90%，先按任务 1 步骤 13 的办法试换拼音写法，**换了就重新测这一条并注明是哪一版固件**。

- [ ] **步骤 3（用户操作）：MQTT 上报成功率测量**

```powershell
pwsh -File build\capture_long.ps1 -Seconds 1800 -Out build\d7_mqtt.log
```
让用户制造 **≥20 次**事件（间隔 ≥10 s，避开 3 s 冷却）。然后取**最后一次**累计统计：

```powershell
Get-Content build\d7_mqtt.log | Select-String -Pattern "上报统计" | Select-Object -Last 1
```
记录 `已发 / 失败 / 丢弃 / 积压`，成功率 = `已发 / (已发 + 失败)`。

**口径必须写清**（设计文档 §10.3 的精神）：`已发` 是"成功交给 esp-mqtt"（QoS1 已入 outbox），**不是**"巴法云端已确认收到"——`Mqtt` 抽象（`78__esp-ml307/src/esp/esp_mqtt.cc:86-99`）没有把 `MQTT_EVENT_PUBLISHED` 暴露出来，拿不到 ack 计数。手机端看到的条数与设备端 `已发` 的差值是旁证，一起写进记录。

- [ ] **步骤 4（用户操作）：2 小时连续运行 + 复位分类**

> 前置：先照 BUG-006 末尾的三条纪律——不烧录、不擦分区、不换网络。

```powershell
pwsh -File build\capture_long.ps1 -Seconds 7200 -Out build\d7_2h.log
```
2 小时后：

```powershell
# 复位原因分类（别只报总数）
Get-Content build\d7_2h.log | Select-String -Pattern "rst:0x[0-9a-f]+" |
    ForEach-Object { $_.Matches[0].Value } | Group-Object | Format-Table Count, Name
# 内部 RAM 低水位
Get-Content build\d7_2h.log | Select-String -Pattern "minimal sram: (\d+)" |
    ForEach-Object { [int]$_.Matches[0].Groups[1].Value } | Measure-Object -Minimum
# 命令词与上报是否一直活着（首尾各取一条）
Get-Content build\d7_2h.log | Select-String -Pattern "上报统计" | Select-Object -First 1 -Last 1
```
判定：**真固件复位**（`assert failed` / `Guru Meditation` / `rst:0x3` 且带 panic）次数应为 0；`rst:0x15`（USB 串口复位）与上电复位不算固件问题，但**要分开列出来**。已有基线：安静环境 73.8 分钟（4426 s）无复位。

- [ ] **步骤 5：填 D7 指标表**

在 `docs/验收记录/D6-D7-MQTT与语音与联调.md` 里填：

| 指标（计划书 §1.1 / 设计文档 §10.3） | 目标 | 实测 | 证据 |
|---|---|---|---|
| MQTT 上报成功率（联网时） | ≥99% | 已发/(已发+失败) | `build/d7_mqtt.log` |
| 离线命令词识别率 | ≥90%（安静环境） | 逐条列出 8 条 | `build/d7_voice.log` + `count_vc.ps1` |
| 全功能并行无复位 | 2 h | 复位次数（**按原因分类**） | `build/d7_2h.log` |
| 内部 RAM 余量 | ≥10 KB 且无分配失败 | `minimal sram` 最低值 | `build/d7_2h.log` |
| 预览帧率 | ≥10 fps | 最后一次 `预览实测` | `build/d7_2h.log` |
| 碰撞事件准确率 | 不做手持实测 | 主机单测覆盖（`test/driving_monitor_test.cc`） | 已有 |

- [ ] **步骤 6：展示并提交（等用户确认）**

```bash
git add "docs/验收记录/D6-D7-MQTT与语音与联调.md"
git status --short
git diff --cached --stat
```
建议 commit message：`docs: 记录 D7 量化指标实测值`

---

## 任务 9：操作文档、演示脚本核对与收尾

**交付物：** `docs/操作文档.md`（交付物 4）、演示视频脚本按实际口径校正（交付物 5）、`docs/BUGS.md` §七 待办索引更新、交接文档。

**文件：**
- 创建：`docs/操作文档.md`
- 修改：`docs/计划书.md` §13.1（演示脚本按实际能力校正）
- 修改：`docs/BUGS.md` §七（关掉已办项、加 Plan C 的新待办）
- 创建：`docs/handoff/<日期>-planC-d6-d7-completed-handoff.md`

- [ ] **步骤 1：写 `docs/操作文档.md`**

必须包含以下小节（每节都要能照着操作，不要写"视情况而定"）：

1. **通电与开机**：USB 供电（数据口）、开机约 15 s 完成基线标定（屏幕提示"（标定中）"→ 正常）、首次开机 SPIFFS 格式化约 2 s。
2. **四个页面**：主页 / 实时画面 / 事件记录 / 设置，各自的导航键位置与作用；返回聊天界面按最右键。
3. **语音命令词表**：9 条（含唤醒词）的**中文说法 + 拼音（写进固件的实际值）**，以及两条边界：① 只在待机态生效；② 识别不到时用 BOOT 键切换对话作为兜底。
4. **阈值与现场调整**：`main/vehicle/vehicle_types.h` 的 `MonitorConfig` 每个字段的含义与默认值（0.35/0.30/1.60/2.50 g、30 s/120 s），改完要 `idf.py build` 重烧。
5. **联网与远程查看**：局域网看图 `http://<IP>:8080/`（IP 从串口 `Got IP:` 或 `VehicleHttp: 手机浏览器打开：` 取）；巴法云三个主题名与手机小程序的查看路径；`/leftover` 标记的用法与"重启复位"这一限制。
6. **故障排查表**：把本轮真实踩到的坑列成"现象 → 原因 → 处置"，至少覆盖：进不去配网（长按 BOOT 2 s）／一进配网就重启（端口冲突，必须用 8080）／烧不进去（拔插 USB）／手机看不到事件（主题没在控制台建、网络路径不对、**同一 UID 被第二个客户端顶下线**）／命令词不响应（不在待机态、唤醒词阈值）／摄像头关闭后卡死（已修，附回归口径）／巴法云连不上（换端点 `mqttv2.bemfa.com:2023`）。
7. **已知限制**：BUG-006（触摸 NACK → abort，不修）、BUG-028（有线局域网下小智拍照上传卡死，用热点规避）、`/leftover` 标记不持久化、跨重启补传上限约 130 条、上报"成功率"口径。

- [ ] **步骤 2：校正演示视频脚本**

对着 `docs/计划书.md` §13.1 的 8 步逐条核对**实际能不能做**，把做不到或口径变了的改掉，例如：

```markdown
### 13.1 演示视频脚本（3–5 分钟，2026-09-20 按实测口径校正）

6. 手机打开巴法云小程序 → 订阅 `vtevt01` / `vtenv01` / `vtsta01` 三个主题 → 远程看到事件与环境数据
7. 语音："你好小智" → "车内温度" → 播报；"有没有人" → 播报待确认状态
   （注：命令词只在待机态生效，说完「你好小智」要等它回完再发命令）
8. 对小智AI 提问一个车载相关问题 → 云端问答
9. 遗留确认演示：手机浏览器打开 `http://<IP>:8080/` → 点「标记有遗留」→ 再说「有没有人」→ 播「有遗留提醒待确认」
```

- [ ] **步骤 3：更新 `docs/BUGS.md` §七 待办索引**

- 把已办项划掉（本轮新办完的：多消费者静默丢弃、设计文档主题名/wn9 口径错误 → 指向 BUG-036/037）
- 新增 Plan C 遗留待办并注明前置，例如：
  1. **BUG-028 残余**（有线局域网拍照上传卡死）——挡着"局域网看图 + 小智拍照"同时演示
  2. `/leftover` 标记不持久化（要持久化就得让 HTTP 任务把写请求交给内部栈任务）
  3. 跨重启补传上限 8 KB（要扩大就得改成"按 seq 定位文件偏移"而不是尾部窗口）
  4. `Mqtt` 抽象没有 `MQTT_EVENT_PUBLISHED` 回调 → 上报成功率只能是"已交给 esp-mqtt"口径
  5. 内部 RAM 仍然结构性吃紧（BUG-024 补充），任何新常驻缓冲都要显式 PSRAM
  6. 上电播报没接：`calib_start` / `calib_done` / `ready` 三条片段已编进 assets 但代码没引用（会被 `--gc-sections` 丢掉、app 体积不涨），要接就在 `VoiceCommand::Play()` 里加——见本计划"偏离"表第 11 条
  7. **下行控制（手机 → 设备）没做**：巴法云支持"订阅一个主题收指令"，用户 STM32 网关就是这么用的（`MQTT_TOPIC_CTRL = cSjvHK4fg012`，载荷 `<地址>,<控制字>`，见 `18_Integrated/docs/03-通信协议.md:74-99`）。本轮范围外，但**实现路径已验证可行**：`Mqtt::Subscribe()` + `OnMessage()`（`78__esp-ml307/include/mqtt.h:15-21`），配合一个 `DOWNLINK` 主题即可（例如手机发 `lock=1` → `vehicle_->RequestLock(true)`）

- [ ] **步骤 4：写交接文档**

按 `docs/handoff/` 的既有格式与命名写 `<YYYY-MM-DD>-planC-d6-d7-completed-handoff.md`，内容至少包含：一句话现状、本轮提交清单、交付物与证据（只给引用）、下一步待办（指向 §七）、环境与操作要点（本轮新踩到的）、敏感信息（`.env` 不在版本控制、密钥在镜像里可 dump）、恢复上下文的最短路径。**写完不自动 commit**，按 Git 规则展示摘要。

- [ ] **步骤 5：整体验证一轮（照 `verification-before-completion`）**

```powershell
# 主机测试全跑一遍（新增 4 个 + 既有 7 个）
$env:PATH="C:\mingw64\bin;$env:PATH"
$tests = @(
  @{n="voice_intent_test"; s="test/voice_intent_test.cc main/vehicle/voice_intent.cc"},
  @{n="voice_reply_test";  s="test/voice_reply_test.cc main/vehicle/voice_reply.cc main/vehicle/environment_sensor.cc"},
  @{n="pending_queue_test";s="test/pending_queue_test.cc main/vehicle/pending_queue.cc"},
  @{n="report_json_test";  s="test/report_json_test.cc main/vehicle/event_json.cc"},
  @{n="event_json_test";   s="test/event_json_test.cc main/vehicle/event_json.cc"},
  @{n="driving_monitor_test"; s="test/driving_monitor_test.cc main/vehicle/driving_monitor.cc"},
  @{n="event_history_test";   s="test/event_history_test.cc main/vehicle/event_history.cc"},
  @{n="event_text_test";   s="test/event_text_test.cc main/vehicle/event_text.cc"},
  @{n="snapshot_ring_test";s="test/snapshot_ring_test.cc main/vehicle/snapshot_ring.cc"},
  @{n="environment_sim_test"; s="test/environment_sim_test.cc main/vehicle/environment_sensor.cc"},
  @{n="imu_convert_test";  s="test/imu_convert_test.cc"}
)
foreach ($t in $tests) {
  g++ -std=c++17 -Wall -Wextra -I main/vehicle $t.s.Split(' ') -o "build_host/$($t.n).exe"
  if ($LASTEXITCODE -ne 0) { Write-Host "编译失败：$($t.n)" -ForegroundColor Red; continue }
  & "build_host/$($t.n).exe" | Select-Object -Last 1
}
```
预期：11 个测试全部打印"全部通过"，没有编译失败。

```powershell
cmd /c "... && idf.py build > build\final_build.log 2>&1"
Select-String -Path build\final_build.log -Pattern "warning:|error:|Project build complete|vehicle commands|multinet models"
(Get-Item build\xiaozhi.bin).Length
```
预期：零 warning、`Project build complete`、`vehicle commands: 9 条`、`multinet models: mn7_cn`；记下 app 体积（对照设计文档 §5.2 的余量账）。

- [ ] **步骤 6：展示并提交（等用户确认）**

```bash
git add docs/操作文档.md docs/计划书.md docs/BUGS.md docs/handoff/
git status --short
git diff --cached --stat
```
建议 commit message：`docs: 操作文档、演示脚本校正与 D6-D7 收尾交接`

---

## 计划自检（作者已做，执行者可复核）

**1. 规格覆盖度**（`docs/计划书.md` §7/§9/§12 的 D6–D7 要求 → 任务）：

| 规格要求 | 任务 |
|---|---|
| 离线命令词 8 条（MultiNet 接入） | 任务 1（生效）、3（派发）、4（执行） |
| 语音播报（预录片段 + 数字拼接） | 任务 2（纯逻辑）、4（播放） |
| 巴法云 MQTT 事件上报 | 任务 5 |
| 环境数据周期上报（30 s）/ 状态（60 s） | 任务 5（`PublishEnv` / `PublishStatus`） |
| 断网补传、幂等键、NVS 游标 `sent_seq` | 任务 5（队列/游标）、6（回填与验证） |
| 局域网 HTTP（D5 已完成，本轮不变） | 不涉及；只加 `/leftover`（任务 4） |
| 「有没有人」口径（读遗留状态位） | 任务 4 |
| 联调、量化指标、演示视频、操作文档 | 任务 7/8/9 |
| 每条根因记 `docs/BUGS.md` | 任务 1（BUG-036）、6（BUG-037）、5 步骤 14 的排查要求 |

**2. 占位符扫描**：本计划没有"待定/TODO/后续实现"式步骤；每个产出代码的步骤都给了完整代码；每个验证步骤都给了命令与**预期输出**。唯一保留的 `// todo` 是上游代码里既有的那处（`camera_capture.h`），与本计划无关。

**3. 类型与命名一致性**（跨任务核对过）：

| 名字 | 定义处 | 使用处 |
|---|---|---|
| `vehicle::VoiceIntent` / `ParseVoiceAction` / `ToString(VoiceIntent)` | 任务 1 | 任务 3、4 |
| `vehicle::ClipId` / `NumberToClips` / `EventClip` / `OccupancyClip` / `TemperatureClips` / `HumidityClips` / `LightClip` / `StatusClips` / `ToString(ClipId)` | 任务 2 | 任务 4 |
| `vehicle::PendingQueue{kSlotBytes,Push,Pop,size,capacity,dropped}` | 任务 5 | 任务 5（`BemfaClient`）、测试 |
| `vehicle::EventToJson(record, device_id, boot_id)` / `ParseSeqFromJsonLine` / `AddReportFields` / `EnvToJson` / `StatusToJson` / `MotionStateId` | 任务 5 | 任务 5、6 |
| `SnapshotStore::ReadEventsAfterSeq` | 任务 6 步骤 1 | 任务 5（`BemfaClient::Start()`，**先做 6-1 再构建 5**） |
| `WorkerTickable::ExecutePending()` | 任务 4 步骤 4 | `VoiceCommand`（任务 4 步骤 5）、`VehicleService::WorkerTaskLoop` |
| `CameraCapture::SetCaptureDoneCallback` | 任务 4 步骤 1 | `VoiceCommand::OnCaptureDone`（任务 4） |
| `VoiceCommand::SetLeftoverPending` | 任务 4 步骤 2 | `VehicleHttp::HandleLeftover`（任务 4 步骤 6） |
| `BemfaClient::{Start,OnEvent,stats,StatsJson,connected}` | 任务 5 | `Esp32S3Board`（任务 5 步骤 12） |
| `BEMFA_TOPIC_*` / `BEMFA_BROKER_HOST` / `BEMFA_BROKER_PORT` / `BEMFA_APP_ID` / `BEMFA_SECRET_KEY` / `BEMFA_UID` | 任务 5 步骤 8/9 | `bemfa_client.cc` |

**4. 已知的执行顺序约束**（执行时别踩）：
- 任务 6 步骤 1（`ReadEventsAfterSeq` 实现）**早于**任务 5 步骤 13（构建）；
- 任务 5 步骤 1（`kMaxSinks` 提到 6）**早于**任务 4 步骤 7（注册第 4 个消费者），否则 `VoiceCommand` 会被静默丢掉；
- `sdkconfig` 改完必须 touch（任务 1 步骤 9）。

---

## 风险与回退

| 风险 | 触发信号 | 回退动作 |
|---|---|---|
| MN 唤醒率不可接受 | 任务 1 步骤 12/13：10 次唤醒 < 7 次，或误唤醒频繁 | 改回 `CONFIG_USE_AFE_WAKE_WORD=y`（删 `USE_CUSTOM_WAKE_WORD` 与 `SR_MN_CN_MULTINET7_QUANT`），**如实记录"无离线命令词"**；任务 3/4 的语音部分转待办 |
| 巴法云连不上（端点/认证） | 任务 5 步骤 14 的排查第 3、4 条 | **只换端点**：改成 `mqttv2.bemfa.com:2023`（STM32 工程跑通的那组，改 `config.h` 两行）；确认没有第二个客户端占用同一 UID；仍不通则记录并保留"局域网看图"作为远程查看手段 |
| 内部 RAM 撑不住（MQTT 客户端 4 KB 内部栈） | 任务 7 步骤 3 的 `minimal sram` 明显低于基线，或 `EspUdp … errno=12` 成片出现（BUG-029 的签名） | 先量（`heap_caps_print_heap_info`）；必要时把 `Mqtt` 换成直接用 esp-mqtt 并把 `task.stack_size` 降到 3072、`buffer.size` 降到 1024（要给 `main/CMakeLists.txt` 加 `mqtt` 到 `PRIV_REQUIRES`）；**不要**去动上游 managed component |
| 命令词误触发（车内对话命中） | 任务 8 步骤 2 出现大量"没说话也命中" | 上调 `CONFIG_CUSTOM_WAKE_WORD_THRESHOLD`（20 → 30）；抓拍/锁车这类**有副作用**的动作已有冷却与状态判断 |
| 上报把 worker 拖住 | 事件落盘延迟、`worker 栈余量` 变小 | `BemfaClient::OnEvent()` **只入队**（本计划已如此设计）；若仍变慢，把 `PersistCursor()` 改成"每 N 条或每 30 s 写一次" |
| BUG-006 打断测量 | 2 h 测量中途 abort | 按 BUG-006 判定、记一笔、**重新开始测量窗口**（用户已决定不修） |

---

## 完成后的状态

做完这 9 个任务，计划书的 **D6 / D7** 到齐：事件/环境/状态上报巴法云（手机远程可见）、断网积压与开机补传、8 条离线命令词与预录片段播报、手机端遗留标记、量化指标实测值、操作文档与演示脚本。

**届时仍未做（如果本轮止步于任务 7 或 8）**：D7 的 2 小时指标、操作文档、演示视频录制（视频必须由用户录）、交接文档——这些都在任务 8/9 里，别漏。

**复核这一版计划时，请重点看这五个决定：**

1. **MQTT 主题改成单级名**（任务 5 步骤 8）——设计文档 §7.1 的 `vehicle/{device_id}/event` 在巴法云控制台**建不出来**（只允许字母/数字），而且必须先在控制台建好才收得到消息。这是本计划对设计文档最大的一处偏离，也是**唯一需要用户先去控制台做动作**的前置（F1）。
2. **幂等键带 `boot`**（任务 5 步骤 10、BUG-037）——`seq` 每次重启从 1 开始，按计划书 §9 的 `device_id + seq` 会把不同开机的同号事件判成同一条，"无重复补传"这条验收项根本成立不了。
3. **游标写 NVS 由 worker 任务代劳**（任务 5 步骤 10 的 `PersistCursor()`）——`net_task` 的栈在 PSRAM（省内部 RAM），而写 NVS 会关 cache，PSRAM 栈上一写就命中 `assert(esp_task_stack_is_sane_cache_disabled())`（BUG-024/026）。这是本轮最容易写错的一处。
4. **命令词执行放在 worker 任务**（任务 4 步骤 4/5 的 `WorkerTickable`）——命令词回调运行在音频输入任务里，任何阻塞都会丢麦克风帧；而播报与判状态又必须有人做，所以用"音频任务只入队、worker 任务执行"的分工。
5. **`/leftover` 标记只存 RAM**（任务 4 步骤 6）——HTTP 任务栈在 PSRAM，不能写 NVS；重启复位这一限制必须写进操作文档与验收记录，不许含糊。

---

## 执行交接

**计划已完成并保存到 `docs/superpowers/plans/2026-09-20-vehicle-terminal-d6-d7-report-voice.md`。两种执行方式：**

**1. 子代理驱动（推荐）** —— 每个任务调度一个新的子代理，任务间进行审查，快速迭代
- **必需子技能：** 使用 superpowers:subagent-driven-development
- 每个任务一个新子代理 + 两阶段审查

**2. 内联执行** —— 在当前会话中使用 executing-plans 执行任务，批量执行并设有检查点
- **必需子技能：** 使用 superpowers:executing-plans
- 批量执行并设有检查点供审查

**选哪种方式？**

**无论哪种方式，开工前请先做这三件事：**
1. 让用户确认 **F1（巴法云控制台三个主题已建）** —— 没做这一步，任务 5/6 的验收全部看不到结果；
2. 跑一遍 F3/F5（内存基线与板子可用性），把数字记在验收记录里；
3. 打招呼：本计划的每一步 `idf.py`/烧录/抓串口都需要 `danger-full-access`，且**抓串口与烧录不能同时进行**。