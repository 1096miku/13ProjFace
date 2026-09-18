# 踩坑与修复记录（BUGS）

本文件记录本项目遇到过的 bug、踩过的坑，以及**根因**和处置办法。

## 什么时候必须记

**任何一次把"现象"追到"根因"的排查，都要在这里留一条**，包括但不限于：

- 自己写的代码有 bug
- 上游 xiaozhi / ESP-IDF / 托管组件的 bug 或脆弱点
- **计划书、任务计划、交接文档里写错的代码或结论**（这类最危险：文档写着"可直接粘贴"，照抄就会崩）
- 硬件、接线、供电问题
- 工具链 / 构建 / 烧录环境问题

不必等修好才记：**没修好的也要记**，把已排除的可能性和下一步写清楚，否则下一个人会重走一遍。

## 怎么写

- 一条一个坑，用 `##` 标题，第一句说清现象
- **必须写根因和证据**（文件:行号、实测数值、串口片段）。只写"怎么改的"不算——换个场景就套不上了
- 已有条目的编号**不要改**（可能被提交信息或其他文档引用）；新条目追加到对应小节末尾
- 与 `docs/验收记录/` 的分工：验收记录写"这一版做到了什么"，本文件写"踩过什么坑、为什么"

---

## 一、会导致崩溃 / 死机

### BUG-001 `%lld` 打印 int64 直接崩在 printf 里（100% 必现）

- **现象**：启动约 2 s 后 `Guru Meditation Error: Core 0 panic'ed (LoadProhibited)`，PC 落在 `memchr`，`EXCVADDR: 0x00000000`。每次启动都一样，形成重启循环
- **位置**：`main/boards/esp32s3/vehicle_service.cc` 的 `LogEvent`（**问题代码来自计划书任务 5**）
- **根因**：本工程是 `CONFIG_NEWLIB_NANO_FORMAT=y`（`sdkconfig:3400`）。IDF 的 Kconfig 明确写着 nano 版格式化库 **"doesn't support 64-bit integer formats"**。`%lld` 只消费 4 字节，后面的可变参数**全部错位**，`%s` 对应的位置读到 `int64` 的高 32 位 = 0 → 传了 NULL 给 printf → 内部 `memchr(NULL, ...)`
- **判据**：崩溃现场 `A2: 0x00000000`（memchr 的字符串指针）；而 `vehicle::ToString()` 有 `default: return "未知"`，**不可能**返回 NULL，所以问题在格式化而不在它
- **修法**：日志里不用 `%lld`，改用 32 位整数与浮点格式（`%d` / `%.3f`）
- **! 注意**：上游 `main/boards/xingzhi-metal-1.54-wifi/cst816x.cc:74` 也用了 `%lld`（全部参数都是 `%lld`，所以只打印乱码不崩）。**新写日志时不要复制 `%lld`**

### BUG-002 `xTaskCreateStaticPinnedToCore` 的栈深度是"字数"不是"字节"

- **现象**：照计划字面写会让任务立刻踩内存
- **位置**：`main/boards/esp32s3/vehicle_service.cc` 的 `Start()`（**问题代码来自计划书任务 5**）
- **根因**：`usStackDepth` / `ulStackDepth` 的单位是 `StackType_t` 元素个数，S3 上 `sizeof(StackType_t) == 4`。计划里 `heap_caps_malloc(kTaskStackBytes)` 只给 4096 字节，却把 `4096` 直接当深度传进去 → 任务按 16 KB 使用这块缓冲区
- **依据**：`esp-idf-v5.5.3/components/freertos/FreeRTOS-Kernel/tasks.c:1044`
  ```c
  ( void ) memset( pxNewTCB->pxStack, ( int ) tskSTACK_FILL_BYTE, ( size_t ) ulStackDepth * sizeof( StackType_t ) );
  ```
- **修法**：`xTaskCreateStaticPinnedToCore(..., kTaskStackBytes / sizeof(StackType_t), ...)`
- **! 同源嫌疑（未修，别顺手改）**：上游 `main/audio/wake_words/custom_wake_word.cc` 用 `stack_size = 4096 * 7` 既做 `heap_caps_malloc` 的字节数、又直接当深度传。现象可能被掩盖（该任务未必真用到栈深处）。**没有真机验证前不要动它**

### BUG-024 worker 任务（栈在 PSRAM）里写 SPIFFS → `assert(esp_task_stack_is_sane_cache_disabled())` 复位

- **现象**（D4 首次真机，2026-09-17）：只要走"要落盘"的路径——点屏幕「抓拍」、「锁车监测」、晃板子产生事件——就**立刻重启**；同一版固件里"让小智抓拍"（不落盘）却完全正常。串口是
  ```
  assert failed: spi_flash_disable_interrupts_caches_and_other_cpu cache_utils.c:127 (esp_task_stack_is_sane_cache_disabled())
  Backtrace: ... SnapshotStore::SaveSnapshot ... CameraCapture::OnCaptureRequest ... VehicleService::WorkerTaskLoop
  Rebooting...
  ```
  一次会话里复现 **13 次**（`build/acceptance_d4_bug024_crash.log`，22:07 那一段每 10–40 s 一次，板子只要被碰就重启）
- **位置**：`main/boards/esp32s3/vehicle_service.cc` 的任务创建（`CreatePsramTask`：worker 的栈是 `heap_caps_malloc(kWorkerStackBytes, MALLOC_CAP_SPIRAM)`）+ `main/boards/esp32s3/snapshot_store.cc`（SPIFFS 读写）
- **根因**：`spi_flash_disable_interrupts_caches_and_other_cpu()` 的**第一行**就是 `assert(esp_task_stack_is_sane_cache_disabled())`，而该判据只要求**当前任务栈在内部 DRAM**（`esp_ptr_in_dram(sp)`）——IDF v5.5.3 `components/spi_flash/cache_utils.c:56-65` 与 `:126-127`。本工程开了 `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y`（`sdkconfig:3370`），worker 的栈在 PSRAM；SPIFFS 的 `fopen/fwrite/stat` 最终都要关 cache 去读写 flash，于是断言必失败。`xtensa-esp32s3-elf-addr2line` 解出的调用链把这条路钉死了：
  `WorkerTaskLoop → CameraCapture::OnCaptureRequest (camera_capture.cc:135) → SnapshotStore::SaveSnapshot (snapshot_store.cc:62 的 fopen) → vfs_spiffs_open → SPIFFS_open → spiffs_phys_rd → spiffs_api_read → esp_partition_read → assert`
- **修法**：**worker 的栈改到内部 RAM**（`MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`），`imu_task` 仍用 PSRAM（它只做纯计算与 I2C 读，不碰 flash）；原 `CreatePsramTask` 改名 `CreateTask` 并增加 `stack_caps` 参数
- **实测代价**：worker 栈仍是 8192 B，抓拍路径每次打印 `worker 栈余量 6088 B`（`uxTaskGetStackHighWaterMark`，IDF 明确返回**字节**）→ 最坏只用了约 2.1 KB；内部 RAM `free sram` 从约 30 KB 降到约 18 KB，`minimal sram` 出现 **1003 B** 的低水位（见验收记录的"局限"一节，D5 加 HTTP 任务前必须重新评估）
- **! 教训**：**"把任务栈放 PSRAM 省内部 RAM"这条便利有硬约束**——任何可能碰 SPI flash 的代码（SPIFFS/FATFS/NVS/OTA/`esp_partition_*`）都**不能**跑在 PSRAM 栈上。给板级任务分工时先问一句"这个任务会不会碰 flash"，会的话栈必须放内部 RAM

---

## 二、逻辑错误（不崩，但结果是错的）

### BUG-003 `STATUSINT.Avail` 是窄脉冲，按"查一次就跳过"会丢掉约 80% 采样

- **现象**：1 Hz 自检实际打印间隔是 2–3 s；按 100 ms 轮询统计 `ok` 只有 4.9/10
- **根因**：真机**最快轮询**实测（4045 次/秒）：`ok=774`（**Avail 占空比 19%**），但数据**真实刷新 100 次/秒**（与配置的 112.1 Hz ODR 相符）。即 Avail 是每个采样周期里约 **1.94 ms 的窄脉冲**，脉冲之间约 8 ms 间隙（774 ≈ 100 × 7.7 完全吻合）
- **影响**：20 ms 周期下"查一次没有就返回 `kNotReady`"会丢掉约 80% 的采样机会，实际采样率掉到 ~10 Hz，颠簸这类短瞬态很容易漏
- **修法**：`main/boards/esp32s3/qmi8658a.cc` 的 `ReadSample` 按 **1 ms 间隔重试、覆盖一个完整周期**（12 次）。**不要忙等**——I2C 总线与触摸/音频共享，空转会占满总线。修后实测 `ok=10/10 not_ready=0 err=0`
- **证据**：`build/serial4.log`（快速轮询统计）、`build/serial7.log`（修后）
- **? 方法教训**：只忙等 5 ms（20 次）时 miss 率仍有 27%，与"8 ms 间隙里塞得下 5 ms"的推算一致——**重试窗口必须覆盖一个完整采样周期**

### BUG-004 传感器上电暂态污染静止基线 → 静止误报 + 永远判不出"停车"

- **现象**：板子平放不动，每 3.01 s（正好等于事件冷却时间）稳定打印一对 `急加速 0.36 / 急转弯 0.35`；`kParked` 永远不出现
- **根因**：QMI8658A 使能后约 340 ms 内 `|a|` 会从 **2.09 g** 经 1.76 / 1.23 / 0.84 / 0.89 / 0.98 回落到 **1.00 g**（这是传感器内部建立过程；同批样本的陀螺始终 ≈0，说明**不是**板子在动）。基线取的是上电后头 50 帧，把这段暂态算了进去 → 基线合成量只剩 **0.75 g**、方向也歪
- **判据**：`静止基线标定完成：ax=.. ay=.. az=.. g（|a|=.. g）` —— 静止时 `|a|` 必须 ≈1.000 g。**这是最快的判据**，所以这行日志永久保留 `|a|`
- **扩大影响**：`is_static` 要求三个轴偏差都 < 0.06 g，基线一歪就永远判不出静止 → 状态机永远到不了"停车"
- **修法**：`main/vehicle/driving_monitor.cc` 的 `Feed` 标定分支**只累加 `|a|` 落在 1 g ± `calib_mag_band`（默认 0.10 g）内的样本**。条件驱动而非硬编码延时，顺带也排除"上电那一秒板子正被拿在手里"的情况
- **证据**：`build/monitor.log`（标定期 60 帧的 `|a|` 序列）；主机测试 `[标定期滤掉传感器上电暂态]`
- **? 遗留**：基线只在上电时标定一次。安装角度在运行中变化不会自动重标，是否需要"运行中重标定"待定

---

## 三、硬件与物理连接

### BUG-005 复位打断 I2C 事务会把总线钳死，软件复位清不掉，只能拔电（未根治）

- **现象**：某次复位后板子进入启动循环，761 次崩溃里 **760 次是同一个点**：
  ```
  E (235) i2c.master: I2C transaction timeout detected
  ESP_ERROR_CHECK failed: esp_err_t 0x103 (ESP_ERR_INVALID_STATE)
  file: "./main/boards/common/i2c_device.cc" line 24
  func: void I2cDevice::WriteReg(uint8_t, uint8_t)      <- Pca9557 构造时的第一次 I2C 写
  ```
- **触发条件**：固件正以 50 Hz 轮询 IMU 时，用主机 RTS 复位板子，复位正好落在一次 I2C 事务中间 → 从机被留在半字节状态钳住 SDA
- **为什么软件复位无效**：从机一直有电，MCU 复位不会释放它。`i2c_master_bus_reset()`（IDF 的 9 时钟恢复）**实测也解不开**，所以那条代码已从板级文件里撤掉（不留无效代码）
- **处置**：**拔掉 USB 断电 5 秒**才能恢复。抓取脚本要用"只开一次端口、不重连"的写法（`build/capture_once.ps1`），避免脚本自己在重连时拉 RTS 再补一刀
- **! 现场含义**：掉电、看门狗复位都可能踩到。上游 `I2cDevice::WriteReg` 走 `ESP_ERROR_CHECK`，一超时就 abort，没有降级余地

### BUG-006 触摸 I2C 失败 → 上游 `ESP_ERROR_CHECK` 直接 abort（**未解决：D3 验证期间已复现**）

- **现象**：运行一段时间后
  ```
  E lcd_panel.io.i2c: panel_io_i2c_rx_buffer(145): i2c transaction failed
  E FT5x06: esp_lcd_touch_ft5x06_read_data(179): I2C read error!
  ESP_ERROR_CHECK failed: esp_err_t 0x103 (ESP_ERR_INVALID_STATE)
    at managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port_touch.c:127
  ```
  然后整机重启；**重启后故障出现得更早**（第一次 8.6 s，下一次直接在 `InitializeTouch` 460 ms 失败），越崩越早，只能拔电
- **根因**：`ESP_ERROR_CHECK(esp_lcd_touch_read_data(...))` 是**上游托管组件里写死的**，触摸 I2C 一失败就 abort，整套设备重启
- **`ESP_ERR_INVALID_STATE` 的含义**（`i2c_master.c:726`）：事务没走到 `I2C_STATUS_DONE`，即**从机没应答(NACK)**。不是超时（超时会打 `I2C transaction timeout detected`），也不是抢锁（拿不到锁返回的是 `ESP_ERR_TIMEOUT`，见 `i2c_master.c:1008`）
- **已排除**：**与 `main/vehicle/` 和 `qmi8658a` 的代码无关**。触摸读取走 LVGL 任务、访问的是另一个器件（FT6336 @0x38），且 I2C 驱动有互斥锁
- **处置（2026-09-17 后段，用户反馈）**：按上面的硬件方向处理后，曾**能长时间稳定运行、未再复现 abort**。**具体做了哪几项（换线 / 主机直连 USB 口 / 独立 5V 供电）、连续运行多久，至今没有记录**——所以当时只记为"暂时缓解"。
- **！复现（2026-09-17 晚，D3 真机验证，证据 `build/acceptance_d3b.log` 行 692 附近）**：**"缓解"不成立**。运行到 **uptime 332.65 s（5.5 min）** 时再次 abort：
  ```
  I (332331) StateMachine: State: speaking -> listening
  E (332651) lcd_panel.io.i2c: panel_io_i2c_rx_buffer(145): i2c transaction failed
  E (332651) FT5x06: esp_lcd_touch_ft5x06_read_data(179): I2C read error!
  ESP_ERROR_CHECK failed: esp_err_t 0x103 (ESP_ERR_INVALID_STATE) at 0x420c2699
  file: "./managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port_touch.c" line 127
  func: lvgl_port_touchpad_read
  expression: esp_lcd_touch_read_data(touch_ctx->handle)
  abort() was called at PC 0x40385577 on core 1
  rst:0xc (RTC_SW_CPU_RST)
  ```
  - **整段日志里 `panel_io_i2c_rx_buffer ... failed` 只出现 1 次** → 一次 NACK 就够 abort 整机，没有任何重试/降级余地
  - **触发时机**：正在与小智语音交互（AI 刚回完话、状态机 `speaking → listening` 之后 0.3 s）。与最初记录的"多在 WiFi 连接 + 唤醒词模型加载 + 音频输入使能之后"一致：**整机负载高的时候更容易踩**
  - **本轮负载差异**：这次是带 D3 负载跑的（新 UI 四页 + `worker_task` + 1 Hz 环境采样），但这些都是纯内存/纯计算，**没有新增 I2C 流量**（环境源是模拟源、相机还没接线）→ 不能归因于新增代码
  - **! 结论**：稳定性这一项**仍然没解决**，`docs/计划书.md` §1.1 的"连续运行 ≥2 小时无重启"目前不可能达成；它是 D4–D7 的主要阻塞项
- **待办（按性价比排序）**
  1. 复现时**先量 I2C 波形/时序**：确认 FT6336 是在什么时刻 NACK（上电瞬间？总线忙？供电跌落？）。没有示波器就先用逻辑分析仪抓 SDA/SCL
  2. 查 FT6336 供电与 FPC 接触（供电跌落会导致 NACK）；触摸与 IMU/扩展器/音频共用 GPIO1/2 的 400 kHz 总线，**触摸的 `scl_speed_hz` 是 400 kHz，而 `BOARD_I2C_FREQ_HZ` 定义的是 100 kHz**（`main/boards/esp32s3/config.h:12` 未被使用）——总线速率口径本身就是乱的，值得一并理清
  3. **不建议**改 `managed_components`（重新解析依赖会被覆盖，且治标不治本）；如果一定要在设备侧兜住，只能改 `main/boards/esp32s3/`（例如不给触摸走 `lvgl_port_add_touch`，自己 `lv_indev_create` + 自己的读回调，把触摸 NACK 降级成"丢这一帧"），那属于"绕开上游缺陷"，要先与我们自己的降级策略对齐
- **决策（2026-09-17 晚，用户）**：**暂不修**。理由：现场**没有可更换的 USB 线**（原假设的第一条措施无法执行），而触发频率已降到**数小时一次**（本次复现是 5.5 min 一次，属偏早的一次），不影响当前演示与开发；设备侧绕开方案（上面第 3 条）**保留待用**。做 D7 的"连续 2 小时无重启"指标前必须重新评估这一条——那项指标目前**不可能达成**

### BUG-007 大幅度动作导致板子重启（复位原因是 RTS，不是掉电）

- **现象**：手持大幅度晃动板子会重启
- **判据**：复位原因**全部**是 `rst:0x15 (USB_UART_CHIP_RESET)`（主机侧 RTS 被拉动），既不是 panic，也不是 `rst:0x1 (POWERON)` / brownout
- **处置**：把 USB 线用胶带固定在桌面做**应力释放**、板子也压住。改完后连续 195 s 零重启
- **! 教训**：这类现象容易被误判成"固件 bug"。**先看 `rst:0x…` 的复位原因再下结论**

---

## 四、工具链与构建环境

### BUG-008 `idf.py` 在受限沙箱下报 `PermissionError: [WinError 5]`

- **现象**：`idf.py build` / `flash` 报 `PermissionError: [WinError 5] 拒绝访问`，堆栈落在 `asyncio` → `windows_utils.py` 的 `pipe()`
- **根因**：`idf.py` 用 Windows **命名管道**拉起 cmake / ninja / esptool，沙箱禁止创建命名管道。**不是代码问题**
- **处置**：给该命令放宽沙箱权限后重试；或改用 `cmake --build build`（绕开 `idf.py` 的 asyncio 路径）

### BUG-009 PowerShell 脚本被按 GBK 读，中文注释导致语法错误

- **现象**：`powershell -File xxx.ps1` 报 `Try 语句缺少自己的 Catch 或 Finally 块`，指向一个看起来完全正常的 `}`
- **根因**：文件是 **UTF-8 无 BOM**，而 Windows PowerShell 5.1 按 **ANSI(GBK)** 读 `.ps1`，中文被解成乱码后把引号/括号吃掉了
- **处置**：临时脚本**一律写纯 ASCII**（注释和提示都用英文）。写完后用
  `[System.Management.Automation.PSParser]::Tokenize()` 验语法，并检查文件最大字节 < 128

### BUG-010 Windows PowerShell 里没有 `pwsh`

- **现象**：`pwsh -File xxx.ps1` 报 `The term 'pwsh' is not recognized`
- **处置**：用 `powershell -ExecutionPolicy Bypass -File xxx.ps1`，或把脚本内容内联到当前会话；**不要假设有 PowerShell 7**

### BUG-011 `cmd //c` 在 PowerShell 下会假装成功

- **现象**：在 PowerShell 里用 `cmd //c "..."`，只打印 cmd 的 banner、退出码 0，**看起来像执行成功其实什么都没做**
- **根因**：`cmd //c` 是 Git Bash / MSYS 的写法；在 PowerShell 里 cmd 把 `//c` 当未知开关，起一个交互 shell 后立刻退出
- **处置**：PowerShell 用 `cmd /c "..."`；Git Bash / MSYS 才用 `cmd //c "..."`。构建输出重定向到日志文件再用 `Select-String` 读关键行（不要管道接 `findstr`，会吞输出）

### BUG-018 `idf_monitor` 反复报 `GetOverlappedResult failed (PermissionError(13, ...))`：板子到 PC 的 USB 链路整体掉线重枚举，不是固件掉线

- **现象**（2026-09-17，`idf_monitor.py -p COM12 -b 115200` 跑 177 s）：约每 9 s（177 s 内约 20 次）出现一次
  ```
  --- Error: GetOverlappedResult failed (PermissionError(13, '拒绝访问。', None, 5))
  --- Waiting for the device to reconnect......
  ```
  每次约 6 个点（6 × `RECONNECT_DELAY`=0.5 s ≈ **3 s**）后自己恢复，日志接着往下走
- **第一句话：这不是板子的问题，板子全程没重启**。判据见「证据 1」
- **位置（全在宿主侧，与固件无关）**
  - `site-packages/esp_idf_monitor/base/serial_reader.py:77` 的 `self.serial.read(...)` 抛异常 → `:80` 捕获 → `:84-85` 打印 → `:86` 关端口 → `:87-96` 每 `RECONNECT_DELAY`（`base/constants.py:62`，0.5 s）重开一次，**重开失败就打一个 `.`**。所以那串点是"重开次数"，不是"设备在重启"
  - 异常本体在 `site-packages/serial/serialwin32.py:288-295`：`GetOverlappedResult` 返回 FALSE 且 `GetLastError()==5`（`ERROR_ACCESS_DENIED`，映射成 `errno 13`）。pyserial 在 `:294` **只容忍 `ERROR_OPERATION_ABORTED`(995)**，其余一律上抛
- **WinError 5 的含义**：这个 COM 句柄底下的**设备对象已经不可用/被拒**。pyserial 无法区分"USB 掉线重枚举"与"驱动层把 pending 读作废"——两种情况报的都是 5。**光看这条日志分不出是哪种**，要靠"出错瞬间枚举端口"才分得开（见下）
- **重连不会复位板子（重要，别误判）**：`serial_reader.py:104-113` 的顺序是"先把 RTS/DTR 状态置为 assert（`base/constants.py:72-73` `LOW=True`）→ `open()` → 再一起 deassert"。经典自动复位电路**只在 DTR 与 RTS 处于特定组合时**才拉低 EN，两者同时 assert 不拉低。所以本次 20 次重连**一次都没复位**（与证据 1 的时钟连续性吻合）。这与 esptool / .NET `SerialPort.Open()` 会打出 `rst:0x15 (USB_UART_CHIP_RESET)`（BUG-007）不是一回事
- **证据 1（板子没重启）**：整段日志只有开头一个 `rst:0x1 (POWERON)`；`VehicleService` 的 `ts=` 与 `I (xxx)` 的偏移全程恒为 **170 ms**（33.161−32.991=0.170；177.431−177.261=0.170）→ 同一个 boot 连续跑了 177 s；事件号单调增到 #44
- **证据 2（真正的损失是丢日志）**：事件号不连续——贴出的日志里只出现 28 个（#2、#5–#11、#15–#22、#27–#31、#35–#40、#44），**另 16 个（#1、#3、#4、#12–#14、#23–#26、#32–#34、#41–#43）在设备侧发生过但从没到主机**；`SystemInfo`（10 s 一条）也缺了 6131 / 106131 / 116131 / 126131
  - **! 结论**：验收里凡是"数串口日志行数/事件条数"的指标都不可信。**D2 的识别率必须改到设备侧统计**才能测（见「规避」第三条）
- **端口身份（顺手查清，之前一直混）**
  - COM12 = `USB-SERIAL CH340K (VID_1A86 PID_7522, oem25.inf, wch.cn)` → 走 **UART0** 那路
  - COM10 = `USB 串行设备 (VID_303A PID_1001 MI_00, usbser.inf)` → ESP32-S3 **原生 USB-Serial-JTAG**（次控制台）。开机日志 `cpu_start: GPIO 44 and 43 are used as console UART I/O pins` 说的是主控制台在 UART0，两路都出完整日志
  - 所以"端口在 COM10/COM12 之间跳"是**两块接口都在线**，不是板子换了号；`build/acceptance*.log` 里的 `### attached COM10` 与 `serial14.log` 的 `COM12` 都能收到完整日志
  - 另有 COM7 / COM8 / COM9 / COM11 四个 `Disconnected` 的 CH340K 残留实例 = 这块桥以前插过别的 USB 口留下的 ghost
- **已排除**：与 `main/vehicle/`、`qmi8658a`、`VehicleService` 无关（它们跑在 MCU 里，碰不到主机的 COM 句柄）；不是 panic、不是 brownout、不是 RTS 复位、不是 `i2c`/触摸那两条（BUG-005/006）
- **!! 触发条件（2026-09-17 当晚已复现并定性）**：**是板子到 PC 的 USB 链路整体掉线并重枚举**——不是 pyserial 的软件假象，也不只是 CH340K 的问题
  - **复现**：`build/diag_com_port.py`（纯 pyserial，与 `idf_monitor` 同一条读路径，**全程没人碰板子**）两次各约 60 s，分别出现 6 次 / 7 次 `GetOverlappedResult failed`，间隔 **7.6 / 10.1 / 12.7 / 8.4 s**，与 `idf_monitor` 那次约 9 s 的节律一致；期间板子静止、**没有任何驾驶事件** → 与"大幅度动作/EMI"无关
  - **决定性证据（两路独立探测，互相印证）**
    - pyserial 侧：出错瞬间 `list_ports.comports()` 里 **COM10 与 COM12 一起消失**
      ```
      [17:17:16.416] FAILURE #13: GetOverlappedResult failed (PermissionError(13, ..., 5))
      [17:17:16.417]   port listed right after failure? False ; ports=['COM3','COM4','COM5','COM6']
      ```
    - 注册表侧（另一路，`[System.IO.Ports.SerialPort]::GetPortNames()` 读 `HKLM\HARDWARE\DEVICEMAP\SERIALCOMM`，与 SetupAPI 枚举无关）在**同样的秒级窗口**里也看不到 COM10 与 COM12，还抓到了重枚举的中间态：
      ```
      17:19:36.594  COM10=True  COM12=False   [COM3,COM5,COM6,COM4,COM10]
      17:19:37.234  COM10=True  COM12=False   [COM3,COM5,COM6,COM4,COM10,COM10]  <- 同一个 COM10 出现两次 = 新设备已到、旧条目未清
      17:19:46.686  COM10=False COM12=False   [COM3,COM5,COM6,COM4]
      ```
    - COM10 是 ESP32-S3 **片内** USB-Serial-JTAG、COM12 是**片外** CH340K，**两颗芯片、两路供电却同进同退** → 掉的是**整条 USB 链路 / 共同的那一级 hub 端口**，不是某一颗桥片坏
  - **恢复期的第二个错误码**：端口回来前后 `CreateFile` 会连报 `could not open port 'COM12': FileNotFoundError(2, '系统找不到指定的文件。', None, 2)`（注册表里已恢复、设备对象还没就绪），持续约 **3 s**——正好等于 `idf_monitor` 里那 6 个点
  - **ESP32 全程没复位**：同一次复现的原始数据里**一条 `rst:` 都没有**，`I (46110) → 66110 → 76110 → 86110 → 106110` 单调递增（缺掉的 56110 / 96110 正是掉线窗口）→ **USB 掉线期间 MCU 照常运行**
  - **! 控制变量**：掉线时**我一次都没碰过**的 COM10 也跟着掉，说明不是我的重连/RTS 动作引起的
  - **? 残余不确定**：从主机侧看不出物理层的具体动作是"VBUS 被切断"还是"hub 端口被禁用"，需要下面的一次对照实验
- **物理原因排序（都便宜，一次对照实验即可定性）**
  1. **两路 5V 回灌**：板子同时接 PC USB 与外部 5V（BUG-006 处置时试过"独立 5V 供电"），两路 5V 之间没有隔离 → 电流倒灌进主机 → hub 判过流 → 禁用端口 → 恢复 → 循环。**"周期约 10 s + 两路接口同时掉 + MCU 不受影响"三条全部吻合**
  2. USB 线 / 接口接触不良（两根线都被拉拽、插头松）
  3. 经过 hub / 延长线，或同口还挂着别的大功率设备；主机口供电不足
  4. Windows **USB 选择性暂停**（电源计划）在空闲时挂起端口、恢复时重新枚举。免费可试：设备管理器 → 各"USB 根集线器" → 电源管理 → 取消"允许计算机关闭此设备以节约电源"
- **对照实验（任做一条，1 分钟）**
  - **只留一路供电**：拔掉外部电源、仅用 PC USB 跑 2 分钟；或反过来用"只供数据不取电"的线 + 外部供电。掉线消失 → 就是回灌/过流
  - 换主机**后面板直插口**（不经 hub / 延长线）+ 一根短粗数据线
  - 开着设备管理器盯 COM10 与 COM12，确认是否每次"同时"消失
- **规避（不改固件）**
  1. 长采集**不要用 `idf_monitor`**：它会丢数据、还会自己重连。用"只开一次端口"的采集脚本
  2. **两路串口不能互补**：COM10 与 COM12 是同时掉的，别指望抓两路来互相补齐
  3. **验收计数改到设备侧**：让 `VehicleService` 周期打印累计分类计数，丢一段串口日志也能从后续累计值反推（本次 16/44 事件丢失就是这么发现的）
  4. 要再看这个掉线，直接跑 `build/diag_com_port.py`（打印错误码 + 出错瞬间的端口存在性 + 缺席时长）

---

## 五、板级移植约束（改 `main/boards/esp32s3/` 之前必读）

这六条原先写在 `CLAUDE.md` 的「六个必须保留的坑规避」。搬到这里的原因：它们和本文件同性质（排查结论 + 依据），放两份必然漂移；`CLAUDE.md` 只留一句引用。

**性质说明**：这是**约束**，不是事后排查流水。BUG-012 有明确的现象描述（花屏），BUG-015 的变换口径对齐原工程 `11_PCA9557` 的实测（T270）；其余四条是迁移期按原理图与数据手册定下的"必须这么写"——它们的"证据"就是下面引的代码本身。**改这几处代码前，先看引用行确认现状，不要凭记忆改。**

### BUG-012 PCA9557 上电顺序写反 → 上电瞬间屏幕随机花屏

- **现象**：屏幕随机花屏（每次上电不一定出现，越像"偶发"越容易误判成屏或排线问题）
- **位置**：`main/boards/esp32s3/esp32s3_boards.h:18-21`（`Pca9557` 构造）
- **根因**：PCA9557 的 `0x01` 是**输出锁存**、`0x03` 是**方向**。先写方向时，IO0（LCD_CS）在锁存值确定前就短暂输出，CS 抖一次
- **处置**：**先 `WriteReg(0x01, 0x05)`（CS=1 不选中 / PA=0 关 / PWDN=1 休眠），再 `WriteReg(0x03, 0xF8)`（IO7~IO3 输入、IO2~IO0 输出）**
- **! 注意**：`docs/小智AI移植.docx` 里的草稿顺序是**反的**，别照着改
- **相关**：`SetOutputState()`（`esp32s3_boards.cc:3-8`）走"读改写 `0x01`"，不要改成整字节写，否则会踩掉其它已置位的 IO

### BUG-013 背光硬编码 LEDC_TIMER_0 / CHANNEL_0，摄像头 XCLK 必须错开

- **现象**：未留存实测日志（迁移期就按机制避开了）。按机制推：两个使用者用同一 `timer_num` 调 `ledc_timer_config`，后一次会拿到 `ESP_ERR_INVALID_STATE`，而调用点都走 `ESP_ERROR_CHECK` → abort
- **位置**：`main/boards/common/backlight.cc:88`（`timer_num = LEDC_TIMER_0`）、`:99`（`channel = LEDC_CHANNEL_0`）、`:101`（`timer_sel = LEDC_TIMER_0`）——**上游公共代码，写死的**
- **处置**：本板摄像头 XCLK 用 `LEDC_TIMER_2` / `LEDC_CHANNEL_2`（`main/boards/esp32s3/esp32s3_board.cc:149-151`，那里已留 `// !` 注释）。**新增任何 LEDC 使用者都避开 TIMER_0 / CHANNEL_0**
- **! 不要改上游 `backlight.cc`**：它是 70+ 块板共用的公共代码

### BUG-014 LCD 片选不在 GPIO 上，必须在面板 init 之前用扩展器 IO0 拉低并整场保持

- **现象**：未留存实测日志（迁移期结论）
- **位置**：`main/boards/esp32s3/config.h:22`（`BOARD_PCA9557_LCD_CS_BIT 0`）、`esp32s3_board.cc:67`（`cs_gpio_num = DISPLAY_SPI_CS_PIN`，值为 NC）、`:76-77`（`esp_lcd_new_panel_st7789` **之前**调 `SetOutputState(IO0, false)`）
- **处置**：`cs_gpio_num` 必须是 `GPIO_NUM_NC`（原理图没把 CS 引到 GPIO），选中动作交给扩展器；SPI 总线上只有 LCD 一个从设备，所以拉低后**一直保持选中**，不需要每次传输前后翻转
- **! 顺序不能调**：先拉低 CS 再 `esp_lcd_new_panel_st7789` / `esp_lcd_panel_init`

### BUG-015 触摸坐标：`x_max/y_max` 填原始竖屏尺寸，flags 用固定一组

- **现象**：触摸点整体错位；若只改错某一项，可能出现"整体转 180°"
- **位置**：`main/boards/esp32s3/esp32s3_board.cc:105-121`
- **依据（跟 LVGL 的屏尺寸无关，别被 `DISPLAY_WIDTH/HEIGHT` 的宏名骗了）**：
  - `x_max = DISPLAY_HEIGHT`（**320**）、`y_max = DISPLAY_WIDTH`（**480**）——面板**原始**竖屏尺寸
  - 驱动软件层按 `mirror_x` → `mirror_y` → `swap_xy` **这个顺序**做变换
  - `swap_xy=1, mirror_x=0, mirror_y=1` 等价于原工程（`11_PCA9577`）的 `FT_ROT_270`：`x' = 480 − ty, y' = tx`
- **处置**：实测整体转 180° 时改 `mirror_x=1`；改这三项前先想清楚上面那个变换顺序，别只调一个

### BUG-016 摄像头 SCCB 复用板级 I2C 总线，不要另起一条

- **现象**：未留存实测日志（迁移期结论）——若给 SCCB 配独立引脚，摄像机会另起一条总线，与 PCA9557@0x19 / 触摸 / IMU 分家，多占引脚
- **位置**：`main/boards/esp32s3/esp32s3_board.cc:164-168`（`pin_sccb_sda/scl = -1`、`sccb_i2c_port = BOARD_I2C_PORT`）
- **依据**：`sdkconfig:2589` 是 `CONFIG_SCCB_HARDWARE_I2C_DRIVER_NEW=y` → 走 `managed_components/espressif__esp32-camera/driver/sccb-ng.c`；`sccb-ng.c:157-158` 在 `SCCB_Use_Port(i2c_num)` 时置 `sccb_owns_i2c_port = false`，`sccb-ng.c:187` 用 `i2c_master_get_bus_handle()` **取回已建好的总线句柄**（不 install 新总线）
- **处置**：`config.h:9-11` 的公共 I2C 是 `I2C_NUM_0` / SDA=GPIO1 / SCL=GPIO2，GC0308 的 SCCB 物理上就接在这一对上；填独立引脚会多出一条总线
- **? 注意**：因此 I2C 总线是**共享**的，任何高频轮询（IMU 50 Hz）都要考虑对触摸/音频的影响，见 BUG-003 的"不要忙等"

### BUG-017 LED(GPIO10) 与 CTP_INT 共用 → 必须开漏输出

- **现象**：未留存实测日志（迁移期结论）——推挽输出下我们与触摸芯片可能同时驱动同一根线，互相灌电流
- **位置**：`main/boards/esp32s3/esp32s3_board.cc:182-194`；引脚宏 `config.h:75`（`LAMP_GPIO = GPIO_NUM_10 // 绿灯，低电平点亮`）
- **根因**：GPIO10 同时接**绿灯、CTP_INT、CN2-2** 三方。若配成推挽输出，我们拉高时触摸芯片若同时拉低，两边互相灌电流
- **处置**：`GPIO_MODE_OUTPUT_OD` + `GPIO_PULLUP_ENABLE`；开漏下双方都只能拉低，不会互推；**低电平点灯依然成立**。上电默认 `gpio_set_level(LAMP_GPIO, 1)` 熄灭
- **? 推论（未实测，改这块时留意）**：开漏 + 上拉下的"输出高"是高阻，且 GPIO10 还被触摸芯片驱动，所以**不要**用 `gpio_get_level(GPIO10)` 反推 LED 状态

### BUG-025 相机共存：预览与"小智拍照"抢同一颗 GC0308，驱动会永久停摆

- **现象**（D4 真机，三个逐步收窄的观察，都由用户实测定位）：
  1. 进「画面」页后画面**卡在一张旧图上**不再更新，抓拍按钮也再没有新日志；
  2. 用户定位："**小智一唤醒（非待机态）画面就卡住，回到待命就恢复**"；
  3. 让预览长时间停在非待机态之后，驱动**永久停摆**：`W (xxx) cam_hal: Failed to get frame: timeout` 每 **4.11 s** 一条、到会话结束都不恢复，预览侧连续 30 次取帧失败后把"摄像头不可用"上屏（`build/acceptance_d4_bug025_stall.log`）
- **位置**：`main/boards/esp32s3/esp32s3_board.cc` 的 `InitializeCamera()`（`fb_count`/`grab_mode`）、`main/boards/esp32s3/vehicle_ui.cc` 的 `TickPreview()`、`main/boards/esp32s3/camera_capture.cc`；上游 `main/boards/common/esp32_camera.cc:59-78`、托管组件 `managed_components/espressif__esp32-camera/driver/`
- **根因（两条叠加）**：
  1. **上游 `Esp32Camera::Capture()` 会把一帧一直攥在 `current_fb_` 里**（要等下一次 `Capture()` 才 `esp_camera_fb_return`，`esp32_camera.cc:69-78`）。预览是同一颗相机的**第二个消费者**，靠 `esp_camera_fb_get()/fb_return()` 取帧；一旦预览停止消费（计划任务 9 要求的"非待机态暂停预览"），驱动就凑不出 `CAMERA_GRAB_WHEN_EMPTY` 所需的"全部缓冲都是空的"→ `cam_start_frame()` 找不到可用帧 → `CAM_STATE_IDLE`；而 `cam_task` 只在 VSYNC 事件里重试（`driver/cam_hal.c:280-289`、`423-424`），实测**再也回不来**：之后每次 `esp_camera_fb_get()` 都走满 `FB_GET_TIMEOUT = 4000 ms`（`driver/esp_camera.c`）返回 NULL
  2. `esp_camera_fb_get()` 最长阻塞 **4 s**，而预览跑在 **LVGL 任务**里；驱动一停摆，每次预览 tick 就把 LVGL 任务卡 4 s → 触摸、翻页、按钮全部无响应（用户"点了没反应"的观感来源）。原实现用的是阻塞 `lock_guard`
- **修法**（三处，全部有实测支撑；**相机参数最终保持计划原样**）：
  1. **去掉"非待机态暂停预览"**（刻意偏离计划任务 9，理由与代价见计划的「执行记录 → D4」）；
  2. `CopyPreviewFrame()` 改用 `mutex_.try_lock()`——拿不到锁就跳过这一帧，**LVGL 任务永不阻塞**；
  3. `GrabSwapped()` 取帧失败时打节流日志（每 60 次一条）——没有这条日志，现场只能看到"画面不动"，没法判断是相机没帧还是别的原因
- **! 走过的弯路（都按"官方推荐"试过，真机全否掉了，别再试）**：
  - 把 `grab_mode` 改成 `CAMERA_GRAB_LATEST`（依据是驱动头文件那句"queue 里始终是最新的 fb_count 帧"）：预览从 13.6 fps **掉到 9.8 fps**，并在约 355 s 出现上述永久停摆（`build/acceptance_d4_bug025_stall.log`）；
  - 再把 `fb_count` 提到 3 + `LATEST` + 预览永不停：**开机就**报 `cam_hal: EV-EOF-OVF` 与 `FB-SIZE: 138240 != 153600`，且小智拍照的上传挂死（`JPEG encoding time` 都没有，`build/acceptance_d4_bug025_v3hang.log`）。这两个现象与 `cam_hal: PSRAM DMA mode disabled` 有关——此时驱动每帧要在 `cam_task` 里**软件搬 153,600 B**，负载一高就搬不完、事件队列溢出
- **最终实测（本版，`build/acceptance_d4e.log`）**：预览 **13.1–15.0 fps**（长跑后段 10.0 fps），**对话期间持续实时**；抓拍 4 张全部落盘；小智拍照上传成功（`Esp32Camera: Explain image size=320x240, compressed size=9322`）；连续 **588 s** 无 `Failed to get frame`
- **? 仍未验证的两点（留给后面的人）**：
  1. `EV-VSYNC-OVF` 偶发（本版 588 s 里 2 次）后能自恢复，但没搞清触发条件；
  2. 打开 PSRAM DMA 模式（`esp_camera_set_psram_mode(true)` 必须**在 `esp_camera_init()` 之前**调，`cam_hal.c:577` 在 init 时取一次 `g_psram_dma_mode`）有可能一举消掉"软件搬 153 KB/帧"这个负载源，但驱动这个模式的风险未知，**没有验证过**

---

## 六、计划与验收口径缺陷（照做会白干或验不了）

### BUG-019 D3 的"事件页可翻页"验收项从一开始就无法执行：四个页面只有主页有入口

- **现象**：D3 真机验收时，按计划步骤"再点'车辆' → 点'上一页'/'下一页' 翻事件页"操作，**屏幕上根本没有"上一页/下一页"按钮**。其余各项（主页环境数据、行车状态、返回聊天界面、中文显示）全部正常
- **根因**：**不是按钮没画，是进不去那一页**。`VehicleUi::RequestPage()` 在 D3 阶段唯一的调用者是"车辆"入口按钮，而它写死了 `RequestPage(Page::kHome)`（`main/boards/esp32s3/vehicle_ui.cc` 的 `BuildOnce()`）；实时画面/事件/设置三页当时只能靠 MCP 工具 `self.vehicle.set_page` 切换，而那个工具属于**任务 11（D5）**，D3 还没实现 → 事件页连同它的翻页按钮都不可达
- **性质**：**计划缺陷**。计划（`docs/superpowers/plans/2026-09-17-vehicle-terminal-d3-d5-env-ui-snapshot.md` 任务 5 步骤 5）把"事件页能翻页"写进了 D3 的当日验收，却没为 D3 提供任何页面切换入口——验收项与当前交付物不匹配
- **修法**：每个自定义页底部加一条 5 键导航栏（**主页 / 画面 / 事件 / 设置 / 返回**），按钮直接调 `RequestPage()`；同时把各页的"返回"按钮去掉（导航栏第 5 键接管），事件页行数从 5 减到 4 给导航栏让位
- **! 教训**：**验收项必须能在"这一步的交付物"里走通**。凡是"某页可做 X"的验收，先回答"这一步里怎么到达那一页"；导航要和页面同一批交付，不要留到暴露接口的那一步

### BUG-020 锁车监测态回不到"行驶"：唤醒判据要求"连续 2 s 动态量 > 0.25 g"

- **现象**（用户真机实测，2026-09-17 晚）：状态机进入"锁车监测"后**再也回不到"行驶"**；期间急加速/急转弯/急刹车等**事件照常触发**、计数照常涨（所以看起来"判定没问题，只是状态卡住"）
- **位置**：`main/vehicle/driving_monitor.cc` 的 `case MotionState::kLockedMonitor`（改动前 208-227 行）
- **根因**：唤醒判据写成了 `mag >= parked_motion_threshold`（0.25 g）**连续** `wake_hold_ms`（默认 2 s）。而 `mag` 是**相对基线的动态合成量**（`driving_monitor.cc:143`），真机行驶时它是**断续**的——多数帧很小、只有起步/换挡偶尔过峰，只要中间有一帧低于 0.25 g，`motion_since_ms_` 就被清零（原 225 行），于是**永远凑不满连续 2 s**。同一份代码里"停车 → 行驶"用的是另一个（正确）判据 `!is_static`：**同一个物理量在两处用了两套判据**
- **为什么单测没抓到**：夹具 `Rig::FeedMotion(n, 0, 0, 1.3)` 让**每一帧**动态量恒为 0.3 g，正好满足"连续超阈值"，把断续的真机形态掩盖了（`test/driving_monitor_test.cc` 原 189 行）
- **修法**：唤醒改用与"停车 → 行驶"一致的 `!is_static`（三轴偏差都 < 0.06 g 才算静止）并持续 `wake_hold_ms`；异常震动告警（`mag >= 0.25 g`）拆成独立判断，不再兼任唤醒条件。顺带在唤醒时清 `parked_since_ms_`
- **验证**：新增用例 `[锁车态：轻微但持续的非静止也应判定为重新行驶]`（含"一直静止不能自己醒"的反向用例）——**修前 FAIL**（状态停在 `kLockedMonitor`、没有 `kMoving` 事件），修后全部通过
- **! 教训**：**同一个物理量不要在两处用两套判据**；测试夹具要像真机（断续、有噪声），"每帧都刚好过阈值"这种夹具会把 bug 藏起来

### BUG-021 设置页文字重叠与越界：标签不限宽，折行后压到下一个控件

- **现象**（用户真机截图 + 实测）：设置页文字互相重叠，部分文字画出屏幕右边界；其余三页正常
- **位置**：`main/boards/esp32s3/vehicle_ui.cc` 的 `BuildSettings()` 与 `MakeLabel()`
- **根因（两条叠加）**：
  1. `MakeLabel()` 没设宽度 → LVGL 按 `LV_SIZE_CONTENT` 排版，长文本**直接画到屏外**。30 号字一个汉字 30 px，480 px 的屏最多放 14~15 个汉字，而当时那句"环境数据源：模拟（本板无温湿度/光照传感器）"有 22 个汉字 ≈ 660 px
  2. 把三行文本塞进**一个** label 后，仍按"单行行距"给后面的控件排位置：该 label 实际高 3 × ~38 px、起点 56 → 占到约 170，而下一条却放在 140 → 必然压字
- **修法**：`MakeLabel()` 统一 `lv_obj_set_width(448)` + `LV_LABEL_LONG_WRAP`（任何文本都不会出屏）；设置页改成**一行一个标签、行距 36 px**，并把超长那句拆短（`环境源：模拟  事件容量 64`）
- **! 教训**：**限宽与换行模式要写在公共构造器里**，不要指望每个调用点自己把文本控制到能放下；多行文本的控件要按**实际行数**留高度

### BUG-022 计划里 `snapshot_ring` 的单元测试自相矛盾：容量 3 的环却断言槽位 7 / 31 能出文件名

- **现象**：按计划任务 2 步骤 1 原样建 `test/snapshot_ring_test.cc` 并运行，**2 项 FAIL**：`槽位 7 → snap_007.jpg`、`槽位 31 → snap_031.jpg`；同一次运行里 `槽位 3 → 空串`、`槽位 0 → snap_000.jpg` 都是 ok
- **位置**：计划 `docs/superpowers/plans/2026-09-17-vehicle-terminal-d3-d5-env-ui-snapshot.md` 任务 2 步骤 1（该文件的 458–462 行）
- **根因**：**测试夹具自己互相矛盾**。同一个 `SnapshotRing ring(3)` 上既断言 `ring.FileNameFor(3).empty()`（越界返回空串），又断言 `ring.FileNameFor(7)`/`ring.FileNameFor(31)` 返回文件名——后两条只有**容量 ≥ 32** 的环才可能成立。实现（`snapshot_ring.cc`：`slot < 0 || slot >= capacity_` 即空串）是自洽的，**错的是用例**
- **性质**：计划缺陷（夹具错误），实现无需改动
- **修法**：补零断言改用 `SnapshotRing full(32)`；容量 3 的环只留「槽位 0 有名字 / 槽位 3 与 -1 越界」三条
- **证据**：改前的 `build_host/snapshot_ring_test.exe` 输出 `2 failure(s)`；改后 `all passed`（19 项）
- **! 教训**：**夹具里的取值必须与被测对象的构造参数一致**——"越界"和"合法"两条断言不能共用同一个容量不足的实例。计划的测试跑出 FAIL 时，先分辨"实现错"还是"夹具错"，不要照着 FAIL 去改实现

### BUG-023 计划给 D4 的两段代码在 ESP-IDF 上编不过（而主机测试全绿）

- **现象**：`idf.py build` 失败，`build/last_build.log` 里两条 error：
  1. `main/vehicle/snapshot_ring.cc:24:44: error: '.jpg' directive output may be truncated writing 4 bytes into a region of size between 1 and 8 [-Werror=format-truncation=]`（同文件 33 行 `'%d'` 同理）
  2. `main/boards/esp32s3/esp32s3_board.cc:296:60: error: invalid new-expression of abstract class type 'CameraCapture'`
- **根因（两条独立）**：
  1. **缓冲区按"实际会写多少"留，而不是按"格式串最坏情况"留**：计划把 `char name[16]` 配给 `"snap_%03d.jpg"`（`%d` 最坏 11 字节 → 5+11+4+1 = 21），`char buf[8]` 配给 `"%d\n"`（最坏 13）。ESP-IDF 默认开 `-Werror=format-truncation`，GCC 按最坏情况判定 → 编译错误。**主机的 `g++ -Wall -Wextra` 不含这条告警，所以主机测试全绿**（同一份代码两种告警集）
  2. **`EventSink::OnEvent` 是纯虚 `= 0`**（`main/boards/esp32s3/vehicle_service.h:23`），而计划让 `CameraCapture : public EventSink` 只覆盖 `OnCaptureRequest` → 类仍是抽象的，`new CameraCapture(...)` 编不过
- **性质**：计划缺陷（计划给出的代码本身编不过），不是执行走样
- **修法**：`snapshot_ring.cc` 的 `name` 改 24、`buf` 改 16（并注明按最坏情况留）；`camera_capture.h` 补一个 `void OnEvent(const vehicle::EventRecord &) override {}` 空实现（相机不消费事件历史，落盘是 `SnapshotStore` 的事）——**没有**把 `EventSink::OnEvent` 改成非纯虚，保持"每个 sink 自己声明怎么处理事件"的约束
- **! 教训**：**主机测试通过 ≠ 设备能编过**——主机 `-Wall -Wextra` 与 ESP-IDF 的告警集不同（`format-truncation` 只在后者开）。任何新增的 `.cc` 都要真跑一次 `idf.py build`，别只看主机测试绿；`snprintf` 的缓冲区一律按格式串最坏情况留
