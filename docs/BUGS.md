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
- **补充（D5，2026-09-18）**：**这条约束对"读"同样成立，不只是写。** `esp_flash_read()` 也要先过 `rom_spiflash_api_funcs->start()`（IDF v5.5.3 `components/spi_flash/esp_flash_api.c:972`），而那就是 `spi1_start → cache_disable → spi_flash_disable_interrupts_caches_and_other_cpu()`（`spi_flash_os_func_app.c:112-134`）；SPIFFS 的 `fopen` 本身就已经在 `spiffs_phys_rd` 里读 flash 了。所以"栈在 PSRAM 的 HTTP 任务读 `/latest.jpg`"一样会复位。D5 的处置：`SnapshotStore` 维护最近一张 JPEG 与最近 50 行事件的 **PSRAM 缓存**（worker 写盘时顺手更新、开机时由 main 任务预热），HTTP 任务（栈在 PSRAM，`httpd_config_t::task_caps`）只读缓存、**一次都不碰 flash**。同时按本条的实测把 `kWorkerStackBytes` 从 8192 降到 **6144**（D5 实测余量仍有 `worker 栈余量 4080 B`），把 2 KB 内部 RAM 还给系统
- **! 教训**：**"把任务栈放 PSRAM 省内部 RAM"这条便利有硬约束**——任何可能碰 SPI flash 的代码（SPIFFS/FATFS/NVS/OTA/`esp_partition_*`）都**不能**跑在 PSRAM 栈上。给板级任务分工时先问一句"这个任务会不会碰 flash"，会的话栈必须放内部 RAM
- **补充（2026-09-20，内部 RAM 优化实验——"能不能腾出内存给 Plan C"）**：结论是**腾不出来**，四个方向只有"变差/崩溃/中性"三种结果。
  - **实测结构**（`heap_caps_print_heap_info(MALLOC_CAP_INTERNAL)`，空载，`build/ram_base_idle.log`）：内部 RAM 分三段，总 128 KB
    | 区域 | 长度 | 空载空闲 | 空载最大连续块 | 分配块数 |
    |---|---|---|---|---|
    | `0x3fcb851c`（= `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` 的 98304 B 保留池） | 98303 | 17315 | 14848 | 118 |
    | `0x600fe000`（RTC RAM，也算 INTERNAL） | 8152 | 780 | 512 | 99 |
    | `0x3fce9710`（通用内部堆） | 22308 | **76** | 68 | 262 |
    **通用那块 22 KB 在空载时就 99.7% 满**；"minimal sram"这个指标实际上就是它的余量，所以低水位是**结构性**的，不是某一次泄漏
  - **基线**（`build/ram_load.ps1`：切到实时画面页 + `///latest.jpg`/`/events` 每 ~1 s 打一轮 + 每 10 s 抓拍一次）：空载 `min 15159 B` / 重载 `min 9303 B`，最大连续块始终 `14848 B`
  - **四个旋钮的 A/B**：
    | 配置 | 重载 min | 最大连续块 | 结论 |
    |---|---|---|---|
    | 基线 | 9303 B | 14848 B | — |
    | `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` | **5963 B** | **5888 B** | ❌ **更差**：内部分配反而多了 4.5 KB（79688→90688 B），最大连续块腰斩 |
    | LVGL 画缓冲进 PSRAM（`buff_dma=1 + buff_spiram=1`） | — | — | ❌ **崩**：S3 的 SPIRAM 堆没有 `MALLOC_CAP_DMA`，`heap_caps_malloc(DMA\|SPIRAM)` 必然失败 → 没有 display → `taskLVGL` 空转被 WDT 抓（见 **BUG-032**） |
    | LVGL 画缓冲进 PSRAM（`buff_dma=0 + buff_spiram=1`） | — | — | ❌ 同样 `taskLVGL` 空转 → WDT |
    | `CONFIG_LWIP_TCP_SND_BUF_DEFAULT`/`WND_DEFAULT` 5760→2880 | 9299 B | 14848 B | ⚪ **中性**，堆结构逐字相同（上传路径除外，见下） |
  - **! 为什么"腾内存"这条路整体不成立**：本系统是**分配驱动**而不是池驱动——把某类分配赶到 PSRAM，其它分配会立刻长进腾出来的空间（V1 就是这样变差的）。**对 Plan C 的做法应该是"自己别占内部 RAM"**（MQTT 的收发缓冲、事件缓冲一律显式 `MALLOC_CAP_SPIRAM`），而不是指望先腾出 20 KB
  - **? 没测、但记下来备查的旋钮**：`CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM`（32 → 16，池子按需分配、未必真占）、`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`（2048 → 512）、`SPIRAM_MALLOC_RESERVE_INTERNAL`（98304，通用堆只剩 22 KB 就是它切的）；后两个都是"改引导策略"，按 V1 的教训收益存疑
  - **✅ 后续更正（2026-09-20 晚，Plan C D6）**：上面这颗没测的 `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`（2048 → 512）**补测后是大赢，而不是"中性"** —— 内部 RAM 可用量 **+19 KB**（系统初始化后 19,311 → 38,311 B），低水位 6167 → 19,179~21,915 B。第 82 行"分配驱动、把某类赶走会被别人填回来"的推断在这一颗上**没有成立**。详见 **BUG-041**（含逐点差分、回归检查清单、以及"对话路径低水位未复测"的缺口）
  - **! 顺带更正第 82 行的口径**：Plan C 实测"自己的缓冲一律显式 PSRAM"**确实做对了**（BemfaClient 整块只花 ~750 B 内部 RAM），但**光靠自己省是不够的**——上游一个 esp-mqtt 客户端就要 7.8 KB，所以还得动引导策略
  - **! 操作陷阱（会白测一轮）**：`Copy-Item` 还原 `sdkconfig` 时**文件时间也被还原成旧值**，`idf.py` 不会重新生成 `build/config/sdkconfig.h` → 你以为改了配置，其实编的还是上一版。改完 `sdkconfig` 必须 `(Get-Item sdkconfig).LastWriteTime = Get-Date`，并回读 `build/config/sdkconfig.h` 确认

### BUG-029 用 `self.camera.set_enabled(false)` 关掉摄像头后打开实时画面页 → **整个 app 卡死**（LVGL 任务被 `esp_camera_fb_get()` 每次按死 4 s）

- **现象**（用户 2026-09-18 晚实测，串口片段 uptime 546–610 s）：对板子说「关闭摄像头」→ 小智回「摄像头已经关闭啦」；**随后打开实时画面页 → 整个 app 卡死**：屏幕停在最后一帧、五个导航按钮全部无响应、喊「你好小智」也叫不醒（只能复位）。串口里三件事同时发生：
  1. `W (554760) cam_hal: Failed to get frame: timeout` —— 每约 **4.1 s** 一条，持续不断（554 / 559 / 563 / 567 / 571 / 576 / 580 / 584 / 588 / 593 / 597 / 601 / 605 / 610 s…）
  2. `E (580550) EspUdp: Send failed: ret=-1, errno=12`（**errno 12 = ENOMEM**）成片出现；同时 `W AFE: Ringbuffer of AFE(FEED) is full, Please use fetch() to read data…` 成片出现
  3. `I (580560) SystemInfo: free sram: 3275 minimal sram: 759` —— 内部 RAM 一度只剩 **3.3 KB**（低水位 **759 B**）
- **用户的预期（也是正确的设计）**：关掉摄像头应该是"**实时预览黑屏 + 抓拍失效**"，**不该卡死整个 app**
- **性质**：**真 bug**（不是设计如此）。属于本板自写代码（`camera_capture.cc` / `vehicle_ui.cc`）与既有 `self.camera.set_enabled` 工具之间的接线缺口
- **根因（UI 冻死这一段机制清楚）**：`CameraCapture::CopyPreviewFrame()`（`main/boards/esp32s3/camera_capture.cc:85-108`）跑在 **LVGL 任务**里——这个函数自己的注释（:90-92）就写着"**绝不能阻塞**……一旦相机停摆，这里会连界面一起卡死，触摸、翻页、按钮全部无响应"——它用 `std::try_lock` 只避开了**互斥锁**，紧接着却调用 `GrabSwapped()` → **`esp_camera_fb_get()`（:47）**；而相机给不出帧时这个调用会**内部等 4000 ms 再返回 NULL**（:50-52 的注释已写明是 `driver/esp_camera.c` 的 `FB_GET_TIMEOUT`）。预览定时器 60 ms 触发一次 → **每次触发都把 LVGL 任务按死约 4 s** → LVGL 任务几乎 100% 时间被阻塞 → 触摸事件、页面切换、按钮回调全部排不上队 → 屏幕按钮全无响应。
  雪上加霜的是：`vehicle_ui.cc:500` 那个"摄像头不可用"提示要**连续失败 30 次**才上屏，按 4 s/次算 **≈120 s**，所以用户看到的那段"卡死"里连提示都来不及出现
- **"唤不醒小智"（同一时段，但因果未定）**：音频上行整条链在报错——`EspUdp … errno=12 (ENOMEM)`、AFE 环形缓冲满、内部 RAM 掉到 3.3 KB。是"相机失败路径吃掉了内存"、还是"音频重试风暴 + 内存紧张互相加剧"，本轮**没有定论**，别凭一条日志下结论
- **另一处独立缺陷**：`self.camera.set_enabled` 只做了一件事——把 PCA9557 的 PWDN 位翻高/翻低（`main/boards/esp32s3/esp32s3_board.cc` 里那个回调），**既不通知 `CameraCapture`/`VehicleUi`，也没有重新初始化相机**。GC0308 掉电后寄存器状态丢失，再 `set_enabled(true)` 很可能回不到能出帧的状态（**待验证**）→ 这也是"关掉再开就卡死"的一部分
- **待办（记录时只记录、未改任何代码——用户 2026-09-18 当时要求"先不做修改，只记录待办事项"；当晚改口要求修复，见下方「修法」）**：
  1. **止血（最小改动、优先）**：给 `CameraCapture` 加一个"相机已关闭/不可用"状态，`CopyPreviewFrame()` / `CaptureJpeg()` 在该状态下**直接返回 false、完全不碰驱动**；`self.camera.set_enabled(false)` 时置位 → 预览页立刻黑屏、抓拍快速失败。**这是用户预期的那条路径**
  2. **别让 LVGL 任务碰"会阻塞 4 s"的调用**：二选一——① 把取帧挪到独立任务（worker 或新建 grab 任务），LVGL 只读最近一帧的副本；② 在预览路径加**熔断**（连续 N 次失败后冷却 X 秒再试），N 取 3~5，别用现在的 30
  3. **! 别和 BUG-025 打架**：预览消费帧本身就是"防驱动停摆"的手段（BUG-025 的修法之一），所以熔断/停帧**只能在"相机确实已被关掉或持续取不到帧"时启用**，相机正常时不能停
  4. `self.camera.set_enabled(true)` 是否需要重新 `esp_camera_init()`（或至少重配 GC0308 寄存器）——**需要真机验证**；若必须重 init，还要处理与预览/抓拍的互斥
  5. 把"摄像头不可用"的判据从"连续 30 次"改成**按时间**（例如连续失败 ≥3 s）或直接按"当前是否 enabled"，让提示及时出现
  6. 复现脚本化：`self.camera.set_enabled(false)` → 打开实时画面页 → 观察是否冻结；修完用它回归
- **证据**：用户 2026-09-18 晚提供的串口片段（uptime 546–610 s：`关闭摄像头` → `Failed to get frame: timeout` 群 → `EspUdp ENOMEM` 群 → `free sram 3275 / minimal sram 759`）；代码行号见上。**本轮本机没有留下日志文件**（当时串口在用户那边，未被我抓取）

- **修法（2026-09-18 深夜，用户改为"要修"，上面的"先不做修改"作废）**——三处改动，逐条对应上面的机制：
  1. **相机开关状态**：`CameraCapture` 新增 `SetEnabled(bool)` / `enabled()`（`camera_capture.{h,cc}`）。`CopyPreviewFrame()` 与 `CaptureJpeg()` 在关闭状态下**立刻返回 false，一次都不碰 `esp_camera_*`**。`SetEnabled()` 内部用**阻塞锁**等在飞的那一次取帧结束（相机没帧时最坏 4 s），所以只能从 MCP 任务调——`CopyPreviewFrame()` 用的是 `try_lock`，持锁期间它直接跳过，不会再进驱动
  2. **熔断（防"相机没关但驱动停摆"）**：`GrabSwapped()` 记录本次取帧耗时，**≥1 s 就认为停摆**，冷却期内一次都不碰驱动；冷却时长 8 s 起倍增、上限 60 s，出帧即清零。判据用**耗时**而不是"失败次数"：帧格式不符、缓冲暂时为空这些正常抖动返回都很快，只有停摆才会把 4000 ms 超时耗满
  3. **提示改成按时间**：`vehicle_ui.cc` 的预览页由"连续失败 30 次"（相机停摆时 ≈120 s 才上屏）改成**连续失败 ≥3 s** 就显示；相机关闭时**立刻把画面刷黑** + 显示「摄像头已关闭」，并在停用期间清掉 fps 统计窗（否则重开那一轮会打一条假读数 `预览实测 0.1 fps`）。提示文案用同一份字面量做"是否需要重写"的判据，避免 60 ms 一次重设 label
  4. **`self.camera.set_enabled` 真正停/起驱动**（`esp32s3_board.cc` 新增 `SetCameraEnabled()`）：关 = 先 `SetEnabled(false)` 再拉高 PWDN；开 = 拉低 PWDN + **重写传感器寄存器**（`s->reset()` → `set_framesize` → `set_pixformat` → `init_status` → GC0308 `set_hmirror(0)`，口径同 `esp_camera_init()` 后半段）。**不能重建驱动**，理由见 **BUG-031**
  5. **返回值语义**：`SetCameraEnabled()` 成功统一返回 `true`（"这次操作做到没有"，不是"摄像头现在开着吗"）。关闭成功时返回 `false` 会被模型念成"咦，摄像头没关成功欸"（真机实测）；重新初始化失败**抛异常**，模型才会如实回答"没打开"
- **验证（2026-09-18 深夜真机，日志 `build/acceptance_b029b.log`，用户操作 + 本机 COM10 抓取）**：
  ```
  I (87030) CameraCapture: 预览与抓拍已停用（不再触碰相机驱动）
  I (87030) Esp32S3Board: 摄像头已关闭（预览黑屏、抓拍立刻失败、不再触碰驱动）
  I (142400) Esp32S3Board: 工具 self.vehicle.set_page → preview      ← 关掉后进预览页，程序照常跑（修复前这里整个冻死）
  I (151430) Esp32S3Board: 传感器已重新初始化（PWDN 拉低 + 寄存器重写）
  I (151430) CameraCapture: 预览与抓拍已恢复
  I (156510) VehicleUi: 预览实测 10.0 fps（50 帧 / 5.0 s，丢帧 0）  ← 画面真的回来了
  ```
  关闭 → 进预览页 → 重新打开，**连续 3 轮全部通过**；关闭期间按「立即抓拍」打的是 `W CameraCapture: 抓拍失败（原因：手动）`（**立刻**失败，不是等 4 s），符合用户预期
- **! 教训**：**"非阻塞"不能只看锁**。原代码用 `try_lock` 躲开了互斥锁，却在拿到锁之后调了一个**内部会等 4 s** 的函数，等于白躲。给 LVGL 任务（或任何"卡一下全屏就死"的任务）写回调时，要连**被调用函数的内部等待时间**一起算进去

---

### BUG-035 进配网模式必定 abort 重启：我们自己的局域网 HTTP（80 端口）和配网 AP 的网页服务器抢同一个端口

- **现象**（用户 2026-09-20 现场）：换网络时"进不去配网，按 BOOT 会重启"。补上"长按 BOOT 进配网"（BUG-033）之后，配网**能进去**了（`Access Point started with SSID Xiaozhi-A261`），但紧接着就重启：
  ```
  I (13820) WifiManager: Starting config AP
  I (13860) WifiConfigurationAp: Access Point started with SSID Xiaozhi-A261
  E (13870) httpd: httpd_server_init: error in listen (112)          ← 112 = EADDRINUSE
  ESP_ERROR_CHECK failed: esp_err_t 0xffffffff (ESP_FAIL)
  file: "./managed_components/78__esp-wifi-connect/wifi_configuration_ap.cc" line 232
  func: void WifiConfigurationAp::StartWebServer()
  expression: httpd_start(&server_, &config)
  ```
- **根因**：**两个 httpd 实例抢同一组端口**。配网 AP 的网页服务器用 `HTTPD_DEFAULT_CONFIG()`（`wifi_configuration_ap.cc:226-232`），而 D5 加的局域网看图服务 `VehicleHttp` 也用它、并从 `StartNetwork()` 起就一直占着。**冲突有两处，改一处不够**：
  1. **数据端口** `server_port` = **80**（两边都是默认 80）→ `E httpd: httpd_server_init: error in listen (112)`
  2. 把数据端口错开之后，第二处冲突立刻显形：**控制端口** `ctrl_port` = **32768**（`esp_http_server.h` 的 `ESP_HTTPD_DEF_CTRL_PORT`，两边也都是默认值）→ `E httpd: httpd_server_init: error in creating ctrl socket (112)`
  两次都是 `EADDRINUSE` + 上游 `ESP_ERROR_CHECK` → abort 重启
- **为什么之前没发现**：D5 的验收只测了"局域网里用手机看图"，从没进过配网模式（那时也不需要，WiFi 已配好）。**两个功能各自都对，撞在一起才炸**。另外它还有个"看运气"的表象：`StartNetwork()` 里我们的服务先起，配网后进就必挂；若在它起来之前进配网（例如开机那一刻就按下 BOOT）反而是好的 —— 所以现场表现时好时坏
- **修法**：把局域网服务的**两个端口都错开**：`server_port = 8080`（`esp32s3_board.cc` 的 `kHttpPort`，访问地址打印带上端口）+ `ctrl_port = 32769`（`vehicle_http.cc` 的 `Start()`）。配网 AP 保留标准的 `192.168.4.1:80`，用户恢复网络的那条路一点不受影响。**没有**改成"进配网前停服务、出来再起"——`WifiBoard::EnterWifiConfigMode()` 不是虚函数，而 `WifiManager::StartConfigAp()` 里 `NotifyEvent(WifiEvent::ConfigModeEnter)` 是在 `config_ap_->Start()`（含 StartWebServer）**之后**才发的，事件回调赶不上；另外 `OnWifiConnectTimeout()` 也会自动进配网，那条路根本没人能拦
- **! 教训**：① **`ESP_ERROR_CHECK(httpd_start())` 这种写法会把"端口被占"升级成"整机重启"**，凡是自己起 `httpd` 的地方，先确认没有第二个 httpd；② **httpd 的"端口"是两个**（`server_port` + `ctrl_port`），只错开一个等于没改——本次就是这么被绊了第二下，**验证时必须把启动日志读到 `httpd_start` 之后**，别只看 AP 起来了就算过

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
  - **! 结论**：稳定性这一项**仍然没解决**，`docs/计划书.md` §1.1 的"连续运行 ≥2 小时无重启"目前**很难在加压条件下达成**；它是 D4–D7 的主要阻塞项。**（2026-09-18 晚 D5 补充：这句要加个限定——注意复位原因分类与测量纪律：安静环境下同一版固件已两次实测长时间无复位（一次 uptime 1181 s、一次 **4426 s = 73.8 分钟**）；见本文末尾"复现次数里有测量手段的贡献"那段。）**
- **待办（按性价比排序）**
  1. 复现时**先量 I2C 波形/时序**：确认 FT6336 是在什么时刻 NACK（上电瞬间？总线忙？供电跌落？）。没有示波器就先用逻辑分析仪抓 SDA/SCL
  2. 查 FT6336 供电与 FPC 接触（供电跌落会导致 NACK）；触摸与 IMU/扩展器/音频共用 GPIO1/2 的 400 kHz 总线，**触摸的 `scl_speed_hz` 是 400 kHz，而 `BOARD_I2C_FREQ_HZ` 定义的是 100 kHz**（`main/boards/esp32s3/config.h:12` 未被使用）——总线速率口径本身就是乱的，值得一并理清
  3. **不建议**改 `managed_components`（重新解析依赖会被覆盖，且治标不治本）；如果一定要在设备侧兜住，只能改 `main/boards/esp32s3/`（例如不给触摸走 `lvgl_port_add_touch`，自己 `lv_indev_create` + 自己的读回调，把触摸 NACK 降级成"丢这一帧"），那属于"绕开上游缺陷"，要先与我们自己的降级策略对齐
- **决策（2026-09-17 晚，用户）**：**暂不修**。理由：现场**没有可更换的 USB 线**（原假设的第一条措施无法执行），而触发频率已降到**数小时一次**（本次复现是 5.5 min 一次，属偏早的一次），不影响当前演示与开发；设备侧绕开方案（上面第 3 条）**保留待用**。做 D7 的"连续 2 小时无重启"指标前必须重新评估这一条——那项指标目前**不可能达成**。**（2026-09-18 晚 D5 补充：这句判得太重了，见本文末尾"复现次数里有测量手段的贡献"那段——安静环境、不加压测、不用会重连的抓取脚本时，同一版固件实测连续 **4426 s（73.8 分钟）**不复位；该指标是"要按纪律测"，不是"不可能"。）**
- **！再次复现（2026-09-18 下午 D5 带负载稳定性测试，证据 `build/acceptance_d5d_mcp_stability.log` 行 539–560）**：把 HTTP 任务 + PC 侧每 ~4 s 打三个路由的压测 + 自动抓拍一起压上去后，**uptime 145.92 s** 再次 abort，**签名与上文逐字一致**（`panel_io_i2c_rx_buffer(145)` → `FT5x06 ... I2C read error!` → `ESP_ERROR_CHECK failed 0x103 (ESP_ERR_INVALID_STATE)` → `esp_lvgl_port_touch.c:127` → `lvgl_port_touchpad_read` → `abort()` → `rst:0xc`）。整段日志里那条 I2C 失败仍只出现 **1 次**：
  ```
  I (141250) VehicleHttp: 首页已下发，HTTP 任务栈余量 4208 B
  E (145920) lcd_panel.io.i2c: panel_io_i2c_rx_buffer(145): i2c transaction failed
  E (145920) FT5x06: esp_lcd_touch_ft5x06_read_data(179): I2C read error!
  ESP_ERROR_CHECK failed: esp_err_t 0x103 (ESP_ERR_INVALID_STATE) at 0x420ca2ad
  file: "./managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port_touch.c" line 127
  func: lvgl_port_touchpad_read
  expression: esp_lcd_touch_read_data(touch_ctx->handle)
  abort() was called at PC 0x40385577 on core 1
  ```
  - 与新增的 HTTP/MCP 代码**无因果关系**：crash 栈在 LVGL 触摸读回调里，HTTP 任务在 PSRAM 栈上只做内存拷贝与 `send()`，两条路径唯一的交集是共用那条 400 kHz I2C 总线之外——没有交集
  - **后果（如实记录）**：D5 的"带负载连续 ≥10 min 无重启"这一验收项**在本轮这套测量条件下达不到**（原因与分类见下一条，别只看复位总数）。同一次会话里本条共复现 **5 次**，uptime 分别是 **145.92 s / 205.86 s / 88.97 s / 60.29 s / 49.00 s**；后来再算上 A/B 那几轮，D5 一共录到 **12 次**（含 10.48 s / 14.54 s 这种"越崩越早"的）。这与 2026-09-17 的决策一致，**本轮不修、不再提修复方案**；D4 曾取得过 588 s 的无重启窗口，说明它是概率事件而非固定周期
- **! 复现次数里有"测量手段"的贡献，别把复位总数全算到固件头上（2026-09-18 晚补充）**：把 D5 的日志按复位原因逐条拆开——`acceptance_d5d_mcp_stability.log` 40 分钟窗口内 **18 次复位，只有 8 次属于本条**：
  | 复位原因 | 次数 | 是谁造成的 |
  |---|---|---|
  | `rst:0xc (RTC_SW_CPU_RST)` **且带完整 panic** | **8** | **固件 abort（本条 BUG-006）**——行 579 / 1359 / 2175 / 2905 / 4073 / 6155 / 6629 / 7978 |
  | `rst:0x1 (POWERON)`（真掉电） | 5 | USB 链路掉线/供电中断（行 4798 / 5093 / 5386 / 5681 / 7010；前几行都有 `### port error`/`### attached`，且集中在用户挪动板子那 30 s 内） |
  | `rst:0x15 (USB_UART_CHIP_RESET)` | 5 | **外部造成**：2 次是我的 `app-flash`（行 14/25）+ **3 次是抓取脚本端口掉线后重连拉 RTS**（行 7208/8506/9040，三处紧邻 `### attached COM…`） |
  判据很简单：**只有带 panic 文本的那种才是固件 abort**；`rst:0x15` 与 `POWERON` 既没有 panic、复位原因也不同，一眼可分。
  另外本条的**触发率被测试条件明显放大**：测量窗口里同时跑着 HTTP 压测（数百次请求、最多 3 路并发）+ 实时画面页，正落在"整机负载高时更容易踩"上。**用户自己在安静环境跑同一版固件，实测连续 73.8 分钟（uptime 4426 s）未复位**（2026-09-18 晚；该 boot 里 `free sram` 17.9~18.4 KB、`minimal sram` **9115 B**，事件 #21~#26 与抓拍全部正常）——所以 D7 的"连续 2 小时"指标在安静环境下只差约 46 分钟，比 D5 数据看起来乐观得多。
  **以后做稳定性测量的三条纪律**：① 抓取脚本用"只开一次端口、不重连"的写法（见 BUG-005 / BUG-018）；② 测量窗口内**不烧录、不擦分区**；③ 统计时**按复位原因分类**，别只报总数
- **再次复现（2026-09-20，`build/acceptance_config_mode3.log`）**：在**配网模式里**又踩到一次，签名逐字一致（`panel_io_i2c_rx_buffer(145)` → `FT5x06 … I2C read error!` → `esp_lvgl_port_touch.c:127` → abort）。时间线：uptime **130.4 s** 长按 BOOT 进配网 → AP 起来、网页配网可用（日志里能看到它在扫 SSID 列表）→ uptime **165.1 s** 被本条打断。同一天的 `build/final_boot.log` 里还有**开机 35 秒内连踩两次**的实例（越崩越早的老形态）。**结论：配网这条路本身没问题（见 BUG-035），但配网期间如果踩到本条就会整机重启**——留档，不改变"不修"的决定
- **✅ 已降级为"打告警 + 跳过这一帧"（2026-09-20 晚，D7 开工前，用户批准）**：D7 要做"连续 2 小时无人值守"，本条随时会把测量打断，用户因此改主意同意修。改法是**最小降级**，不动上游任何语义：
  - 位置 `managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port_touch.c:126` 起：`ESP_ERROR_CHECK(esp_lcd_touch_read_data(...))` → 判返回值；失败时 `ESP_LOGW`（**第 1 次 + 之后每 100 次**，避免 I2C 死住时每帧刷爆串口）+ `data->state = LV_INDEV_STATE_RELEASED` + `return`
  - **为什么只动这一处**：`esp_lcd_touch_get_data()`（原第 130 行）**不会**因为 I2C 失败而 abort——它调用的 `esp_lcd_touch_ft5x06_get_xy()`（`managed_components/espressif__esp_lcd_touch_ft5x06/esp_lcd_touch_ft5x06.c:207`）只在临界区里读 `read_data()` 填好的缓存，自己不碰 I2C。**整条 abort 路径的唯一入口就是 `read_data()`**，所以第 130 行的 `ESP_ERROR_CHECK` 留着也是对的
  - **! 这条改动不在版本库里**：`managed_components/` 被 `.gitignore:3` 忽略（实测 `git ls-files managed_components` 输出 **0** 行）。**删掉或重装组件目录就会丢**，届时现象会原封不动回来。要长期保住得做"补丁文件 + 构建期应用"，本轮没做
  - **验证状态（如实）**：编译通过（构建日志里有 `lvgl9/esp_lvgl_port_touch.c.obj`、零 warning）、已烧录；但**"长时间运行不再 abort"尚未真机验证**——烧录后板子 USB 掉线（**BUG-043**），D7 测量中止。**下次验收判据**：串口出现"读取触摸数据失败(...)，跳过这一帧"而**不是** abort，且复位分类里固件 abort 计数为 0
  - **✅ 部分验证成功（2026-09-20 19:0x，真机，`build/d7_bug040.log`）**：300 s 窗口里抓到**两次**触摸 NACK——`261.19 s` 与 `264.85 s`，签名与历史**逐字一致**（`E lcd_panel.io.i2c: panel_io_i2c_rx_buffer(145): i2c transaction failed` → `E FT5x06: esp_lcd_touch_ft5x06_read_data(179): I2C read error!`），而**板子没有 abort、没有复位、日志一直继续到 289.58 s**。历史上同一签名**必 abort**（本文档已记 12 次）。⇒ **降级生效**
    - 这两次里**没有打出我的 W 告警**，原因已查清、不是没生效：告警本身是**限流过的**（原设计"第 1 次 + 之后每 100 次"），而 261/264 两次是本次开机里的**第 2、3 次**失败（第 1 次落在两次抓取之间那 22 s 的缝里）。已把限流放松为**前 10 次逐条打**，便于下次取证
    - 排查过程留档（避免重走）：确认补丁字符串确实在固件里（`build/xiaozhi.elf` 偏移 88554 处有 `跳过这一帧`）；枚举全仓 `esp_lcd_touch_read_data` 的调用方**只有两处**——本处与 `managed_components/espressif2022__esp_emote_gfx/src/core/gfx_touch.c:217`，而后者只有 `emote::EmoteDisplay` 路径会启动，本板 `CONFIG_USE_EMOTE_MESSAGE_STYLE` 未定义（`esp32s3_board.cc:114` 的 `#if` 为假）⇒ **gfx_touch 是死代码**，唯一在跑的调用方就是本处
  - **! 仍未验证**：长时间（≥2 h）不再 abort；以及"是不是**每次** NACK 都被兜住"（目前只证明这 2 次）

### BUG-033 BOOT 键进不了配网模式：这块板只有"启动阶段单击"这一条路，而 GPIO0 是启动 strap

- **现象**（用户 2026-09-20 实测）：想把板子从局域网 WiFi 换到手机热点，**进不去配网模式**；"按 BOOT 会导致重启"。同一版固件在别处一切正常
- **根因（两条叠加）**：
  1. 本板的 `InitializeButtons()` 只注册了 `OnClick`，且里面判 `app.GetDeviceState() == kDeviceStateStarting` 才进配网（`main/boards/esp32s3/esp32s3_board.cc`）——也就是说**必须在开机后到联网前的那几秒里"单击"**。开机跑起来之后单击只会 `ToggleChatState()` 切对话，**没有任何一条运行期进配网的路**
  2. **GPIO0 是 ESP32 的启动 strap**：上电时按住 BOOT = 进入 ROM 的 **UART 下载模式**，固件根本不跑（`CLAUDE.md` 里写的"启动阶段按下进配网"在实机上因此几乎踩不准，用户看到的就是"按 BOOT 重启/没反应"）
- **修法**：补 `boot_button_.OnLongPress(...)` → `EnterWifiConfigMode()`（`Button` 默认长按阈值 2000 ms）。这是上游其它板子的既有惯例（`main/boards/zhengchen-1.54tft-wifi/zhengchen-1.54tft-wifi.cc:91`、`doit-s3-aibox`、`yunliao-s3` 等 7 块板同款写法）。`EnterWifiConfigMode()` 在运行期（状态为 Idle）会走"`ResetProtocol()` → 等 1 s → `StopStation()` → `StartConfigAp()`"这条路，起 AP + 网页配网，**不重启芯片**
- **! 教训**：**"按键在启动阶段按"这种设计，遇上启动 strap 就等于没设计**。凡是靠某个按键进配网/进恢复模式的方案，先确认这个引脚在复位瞬间是不是 strap；是的话必须另给一条**运行期**的入口
- **验证口径**：长按 BOOT 2 s → 串口出现 `长按 BOOT → 进入配网模式` + `EnterWifiConfigMode called` + `Starting config AP`，屏幕提示热点名与网址

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

### BUG-028 小智拍照上传**间歇性**挂死/超时（上游 `self.camera.take_photo` 路径；已用 A/B 排除 D5 新增的 HTTP 服务）

- **一句话**：D5 期间复现的"小智拍照上传卡住/30 s 超时"，**与 D5 新加的局域网 HTTP 任务无关**（把 HTTP 整个关掉照样卡），也**不是 D5 引入的**（同一版固件里既有失败也有成功）。它是上游拍照上传路径的间歇性故障，**根因未定**。下面按时间顺序留证据，别重复走这三步
- **现象**：用户对板子说"看看相机拍到什么"（走上游 `self.camera.take_photo`），串口是
  ```
  I (81210) Application: << % self.camera.take_photo...
  I (81440) Esp32Camera: Captured frame: 320x240, format=153600
  I (81490) HttpClient: Established new connection to api.xiaozhi.me:80
  I (81490) Esp32Camera: JPEG encoding time: 49 ms
  ...
  E (141510) EspTcp: Send failed: ret=-1, errno=128
  E (141510) EspTcp: Not connected
  W (142260) httpd_txrx: httpd_sock_err: error in send : 11
  E (171510) HttpClient: Wait for HTTP headers receive timeout
  E (201510) HttpClient: Wait for HTTP headers receive timeout
  E (171510) Esp32Camera: Failed to upload photo, status code: -1
  E (201530) MCP: tools/call: Failed to upload photo
  ```
  **拍到了、编码也成功了（49 ms），但上传拿不到响应头，重试两次各 30 s 后失败。** 这就是 D4 验收记录「局限」第 8 条那个"根因未定位的上传挂死"——D5 把它从"出现过一次"变成了"**间歇性可复现**"
- **同期的内部 RAM（同一个 boot，`build/acceptance_d5d_mcp_stability.log`）**：
  | uptime | free sram | minimal sram |
  |---|---|---|
  | 6.5 s | 20119 | 18135 |
  | 34.3 s | 12071 | 5703 |
  | 70.3 s | 12827 | **1287** |
  | 211.3 s | 16907 | **395** |
  失败发生在 81~201 s 之间，**正好落在 "minimal sram 从 1287 B 继续掉到 395 B" 的那段窗口里**（`minimal` 是粘性值，只记"曾经到过"）。395 B **比 D4 记录的 1003 B 还低**，而 D4 那次没有 HTTP 任务
- **负载条件（第一次复现时）**：实时画面页在跑（13 fps）+ PC 端每 5 s 打 `/`、`/latest.jpg`、`/events`（`build/d5_http_load2.log`）+ 语音对话 + 3 个 `self.vehicle.*` 工具调用；同一 boot 里 httpd 服务了 42 次 `/`。**注意：后面已用 A/B 证明这些负载都不是必要条件**（见下），此表只用于说明"当时内存有多紧"
- **决定性 A/B（2026-09-18 下午，四轮对照）**：
  | 条件 | 结果 |
  |---|---|
  | **冷启动空载**：刚开机、不打开实时画面页、不先对话，直接"看看相机拍到什么" | ✅ **成功**（用户实测：小智念出了画面内容。**当时串口被用户的监视器占用，这一条没进 `build/` 日志，是用户目测结论**） |
  | 实时画面页开着（10~14 fps）+ 对话 | ❌ **挂住**（编码 47 ms 后有连接，随后无任何响应；用户实测一直卡在 `% self.camera.take_photo...`） |
  | 实时画面页 + PC 端 HTTP 压测 + 对话 | ❌ **两次 30 s 超时失败**（`Failed to upload photo, status code: -1`） |
  | **HTTP 服务整个关掉**（A/B 固件，`/` 端口 `curl` 全 `000`；`build/acceptance_d5f_ab_httpoff.log`） | ❌ **照样卡**（无预览也卡：一次卡 23 s 后被 BUG-006 abort、一次卡 55 s 后用户按 reset）；**但同一版固件里也有一次 ✅ 成功**（下面的三行对照） |
- **同一个 A/B 日志里的三行对照（`build/acceptance_d5f_ab_httpoff.log`）**——这条比上面任何推测都重要，它说明**故障是间歇的**：
  | 尝试 | uptime | 实时画面页 | 结果 |
  |---|---|---|---|
  | 1（行 89） | 160.8 s | 开着 | ❌ 挂住 23 s → 被 BUG-006 abort（184.26 s） |
  | 2（行 619） | 62.0 s | **没开** | ❌ 挂住 55 s → 用户按 reset |
  | 3（行 1577） | **18.8 s** | **没开** | ✅ **成功**：`Established new connection` → **950 ms 后** `Explain image size=320x240, compressed size=6475, remain stack size=4920` → 服务端返回 `{"success":true,...,"text":"画面是一张仰拍的自拍…"}` |
  即：**同一版固件、同样的空载条件，18.8 s 时成功、62 s 时挂住**。用户独立观察一致（"我自己按了 reset 键，有时却不会卡住，而是会回答内容"）
- **代码级排查（2026-09-18 晚，读上游实现，不改代码）**：把上传链路读了一遍，能排除掉几条、也能解释现象
  - 上传是 **chunked 的 multipart POST**：`Esp32Camera::Explain()` 设 `Transfer-Encoding: chunked`（`main/boards/common/esp32_camera.cc:246`），逐块 `http->Write(...)`（`:268/:276/:291/:306/:308`）——**这 5 处返回值全部没有检查**
  - 但**"发送被截断"这条路可以排除**：`HttpClient::Write()` 会把每块包成 `"<hex>\r\n<data>\r\n"` 交给 `EspTcp::Send()`（`managed_components/78__esp-ml307/src/http_client.cc:640-670`），而 `EspTcp::Send()` **内部有 while 循环把短写补全**（`.../src/esp/esp_tcp.cc:116-125`），失败时会打 `Send failed: ret=…, errno=…`——**失败那几次的窗口里没有任何 `Send failed`**，说明请求体是完整发出去的
  - 上传目标的 URL 是**服务端下发的**（MCP `initialize` 里的 `capabilities.vision.url` → `main/mcp_server.cc:337-347`），设备**不打印**它，所以目前无法从 PC 侧复现同一个请求（这是下一步要补的观测手段）
  - **errno 语义已核对**（`xtensa-esp-elf/…/picolibc/include/errno.h`）：`11 = EAGAIN`、`12 = ENOMEM`、`128 = ENOTCONN`
- **新的、很关键的一条设备侧证据**：用户 2026-09-18 下午的日志里有 `E (99750) EspUdp: Send failed: ret=-1, errno=12`（**ENOMEM**）——会话建立瞬间**网络栈分配不到缓冲**。这与"内部 RAM 低水位 395~987 B"是同一件事的两种表现，应当单独看：它伤的是**音频上行**（UDP），不一定直接造成上传挂死，但证明内存压力确实会打到网络路径
- **四个候选解释（按现有证据重排，都还没被证实）**：
  1. **服务端慢/排队（现在最像）**：设备把请求完整发出（无 `Send failed`），60 s 内没有响应头；同时刻设备一切正常（预览 10 fps、对话照常、MQTT 音频在走）；PC 直连同一端点 46 ms 返回；成功那次只用 **1.15 s**。`api.xiaozhi.me` 是公开的演示服务，视觉分析要等模型，超出客户端 2×30 s 的超时窗口完全可能
  2. **上传路径的时序竞态**（如相机帧与上传线程竞争）——间歇性符合，但解释不了"设备侧动作完全相同、时而成功时而 60 s 无响应"
  3. **内部 RAM/带宽不足**——有 `ENOMEM` 这种直接证据，但它出现在**音频 UDP** 方向；上传侧没有任何发送失败，所以至多是间接因素
  4. **崩溃重启后的状态**——弱线索（成功那次也跟在 `rst:0xc` 之后）
- **下一步（要证实解释 1 的最小代价动作）**：在 `main/mcp_server.cc:337-347`（**上游公共代码**）加一行 `ESP_LOGI` 把服务端下发的 `vision.url` 打出来 → 从 PC 用同一 URL + 同一 multipart 格式复现上传 → 若 PC 侧也复现 60 s 无响应，就是服务端问题；若 PC 侧秒回，则回到设备侧继续查
- **! 这条对本板的意义**：**"HTTP 栈放 PSRAM"只挡住了栈那一份，挡不住网络收发缓冲那一份。** 内部 RAM 的紧张源头是 WiFi/lwIP 收发缓冲 + 相机驱动每帧软件搬 153,600 B（`PSRAM DMA mode disabled`）。以后要在本板再加"常驻内部 RAM 或用网络"的模块（Plan C 的 MQTT、离线命令词），**必须先处理内部 RAM**，可选方向（都需要单独验证，不要凭"官方推荐"直接上）：`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`、`CONFIG_LWIP_TCP_WND_DEFAULT`/`SND_BUF_DEFAULT` 调小、LVGL 绘制缓冲 `width_ * 20` 调小、减少 SPIFFS `max_files`
- **已做完与仍未做的**：✅ 撤 HTTP 压测、✅ 冷启动空载、✅ PC 侧端点探测、✅ **HTTP 关掉的 A/B 固件**（本轮最有价值的一步）。**仍未做**：① 加"内部 RAM 低于阈值就打印 + 带当前任务名"的诊断（把解释 2 钉死或排除）；② 在失败窗口里抓一次网络侧证据（例如同时开 PC 端抓包看设备有没有把 POST 发出去）；③ 判明解释 1 的竞态位置需要读上游 `main/boards/common/esp32_camera.cc` 的取帧/上传实现
- **补充（2026-09-18 深夜，"卡 1 分钟"修掉之后的重测）**：
  - **同一个 boot 里一次失败、一次成功**（`build/acceptance_b029b.log`）：uptime 44 s 的 `self.camera.take_photo` 失败（窗口里**没有任何 `Send failed`**，等满 2×30 s 超时），uptime 194 s 的同一条命令 **1.45 s 成功**（`Explain image size=320x240, compressed size=7119`）。设备侧动作完全相同 → 与设备状态无关
  - 改掉 **BUG-030**（重复调用 `GetStatusCode()`）之后，用户连说近 **10 次**「小智，帮我看看相机抓拍到了什么」**全部成功**（含直接调 `self.camera.take_photo`），本机抓取窗口内未再复现
  - 结论维持"最像服务端"。**剩余风险**：再复现时第一条要做的仍是打好服务端下发的 `vision.url`、从 PC 侧复现同一 multipart POST（上面「下一步」那条），而不是继续在设备侧猜；"上传失败最多静默多久"已由 BUG-030 的修复从 60 s 降到 **30 s**（库默认值，未改）
- **补充（2026-09-20，用户定位到"跟网络路径有关"——目前最可信的定性）**：
  - **手机热点 → 可以；PC 用网线接局域网、板子连该局域网的 AP → 卡死**。用户在同一块板、同一版固件上只换网络就切换了成败，这是整条 BUG-028 里**最干净的一次单变量对照**
  - 失败时的设备侧签名（`build/acceptance_wrapup.log`，3 次全中）：`Established new connection to api.xiaozhi.me:80` → **正好 60.05 s 后** `E EspTcp: Send failed: ret=-1, errno=128`（ENOTCONN）→ 再 30 s `Wait for HTTP headers receive timeout` → `Failed to upload photo`。**整个窗口里设备一切正常**（预览 10~12 fps、抓拍落盘、事件照常），没有任何 WiFi/lwIP 报错
  - **"串口开着就失败"是巧合，别再走这条路**：本轮先测出"COM10 开着（读或不读）3~5 次全失败、关掉 4 次全成功"，但后来发现那两次对照正好**换了网络**（详见 BUG-034）。物理上也说不通：UART 没有硬件流控，ESP32 无法知道主机有没有在读
  - **! 结论重排**：解释 1 应该改写成"**服务端 / 中间网络路径**"——设备把请求完整交出（无 `Send failed` 的窗口里数据是发出去的），但对端不 ACK、60 s 后连接被判死。有线局域网里更可能是**中间设备（路由器/防火墙/透明代理）对这条 chunked POST 的处理**。**可做的下一次实验**：把 `Esp32Camera::Explain()` 的 `Transfer-Encoding: chunked` 改成一次性 `Content-Length`（把编码结果收进一个缓冲再 `SetContent()` 后 `Open()`），在**有线局域网**上复测——若这样能通，就是中间盒不认 chunked，属真修复

### BUG-030 `Esp32Camera::Explain()` 把 `GetStatusCode()` 调了两次 → 上传失败时白等 **60 s**（= 用户报的"说拍照会卡 1 分钟"）

- **现象**（用户 2026-09-18 报）：说「小智，帮我看看相机抓拍到了什么」之后**整整 1 分钟没有任何反应**，然后才回一句"照片传不上来"
- **位置**：`main/boards/common/esp32_camera.cc` 原 310-313 行（**上游公共代码**）
- **根因**：
  ```cpp
  if (http->GetStatusCode() != 200) {
      ESP_LOGE(TAG, "Failed to upload photo, status code: %d", http->GetStatusCode());  // ← 又调一次
  ```
  `HttpClient::GetStatusCode()` **每调用一次就会等满 `timeout_ms_`**（`managed_components/78__esp-ml307/include/http_client.h:101` 默认 **30000**）才返回失败；`headers_received_` 在失败后仍为 false，所以第二次调用**又等 30 s**。串口里就是两条相隔 30 s 的
  `E HttpClient: Wait for HTTP headers receive timeout`（实测 74580 / 104580 ms）——**30 + 30 = 60 s，与用户说的"1 分钟"逐字吻合**。注意第二条 `W ... Failed to upload photo, status code: -1` 的时间戳是**第一次**的超时时刻（74580），因为异常在第一个超时就抛了，日志前缀用的是调用点时间——看日志别被这一点带偏
- **修法**：只调一次并复用结果
  ```cpp
  const int status_code = http->GetStatusCode();
  if (status_code != 200) { ESP_LOGE(TAG, "Failed to upload photo, status code: %d", status_code); ... }
  ```
- **验证**：改后同样的失败路径只出现**一条** `Wait for HTTP headers receive timeout`，静默时间由 60 s 降到 30 s（30 s 是库默认值，未改）
- **! 同一模式还存在于这几处（本次没动，改的话一起评估）**：`main/boards/common/esp_video.cc:1028`、`main/boards/sensecap-watcher/sscma_camera.cc:733`、`main/ota.cc:287`、`main/assets.cc:441`——都是"判断里调一次、日志里再调一次"
- **! 教训**：**有副作用（会阻塞）的 getter 不能写进 `if` 条件里再在分支里重复调用**。这类写法平时看不出问题，只在失败路径上把代价翻倍——失败路径恰恰是用户唯一能感知的那条

### BUG-031 运行期无法 `esp_camera_deinit()` + `esp_camera_init()` 重开摄像头：`cam_dma_config` 要 30720 B 连续 DMA 内部 RAM，此时最大空块只剩 25600 B

- **现象**（2026-09-18 深夜，BUG-029 修复过程第一版实现）：用"删掉 `Esp32Camera` 再原地重建"的方式实现"打开摄像头"，真机上**必失败**：
  ```
  I (136820) camera: Detected GC0308 camera          ← 探针、SCCB 都正常
  I (137080) cam_hal: PSRAM DMA mode disabled
  I (137100) cam_hal: Allocating 153600 Byte frame buffer in PSRAM
  E (137110) cam_hal: cam_dma_config(524): DMA buffer 30720 Byte malloc failed, the current largest free block:25600 Byte
  E (137120) cam_hal: cam_config(599): cam_dma_config failed
  E (137120) camera: Camera config failed with error 0xffffffff
  ```
  同一版固件**开机时**同一个 `esp_camera_init()` 是成功的——差别只在"开机那一刻内部 RAM 还是干净的"
- **根因**：`cam_dma_config()` 里的 `heap_caps_malloc(cam_obj->dma_buffer_size /*30720*/, MALLOC_CAP_DMA)`（`managed_components/espressif__esp32-camera/driver/cam_hal.c:522`）要求**一次性拿到 30720 B 连续内部 RAM**（本板 `psram_mode = false`，DMA 只能落在内部 RAM）。跑起来之后 WiFi/lwIP/音频/LVGL 已经把内部 RAM 切碎，最大空块只剩 25600 B（`SystemInfo: minimal sram` 同期在 1.5 KB 上下）。`0xffffffff` = `ESP_FAIL`，来自 `cam_dma_config` 的 `return ESP_FAIL`
- **修法**：**关闭摄像头时不拆驱动**——DMA 缓冲、`cam_hal`、帧缓冲全部原样保留，只把 GC0308 的 PWDN 拉高断电；"打开"时拉低 PWDN + **重写传感器寄存器**（`sensor_t::reset()` → `set_framesize` → `set_pixformat` → `init_status`）。实现在 `main/boards/esp32s3/esp32s3_board.cc` 的 `SetCameraEnabled()` / `ReinitSensor()`
- **验证**：连续 3 轮"关 → 开"，每次都打 `传感器已重新初始化（PWDN 拉低 + 寄存器重写）`，预览恢复到 10 fps（`build/acceptance_b029b.log`）
- **! 这条对本板的意义**：**这块板上"可以随时 `esp_camera_deinit()` 再 `init()`"是不成立的**。以后任何"重启相机/切换相机配置"的需求（分辨率切换、双摄、Plan C 的按需抓拍省电）都要按"保留驱动、只重写传感器寄存器"的路子做；真要重建驱动，必须先把内部 RAM 腾到 30 KB 连续可用（见 §七 待办第 3 条）
- **? 与 BUG-025 的关系**：这条不改变 `fb_count`/`grab_mode` 的取值结论，别顺手去动那两个

### BUG-032 这块板的 LVGL 画缓冲**不能**放进 PSRAM：两种写法都会让 `taskLVGL` 空转被 WDT 抓

- **背景**：内部 RAM 优化实验里，`SpiLcdDisplay` 的画缓冲是内部 RAM 里最大的单笔常驻分配之一——`main/display/lcd_display.cc` 的 `buffer_size = width_ * 20`（480×20×2 = **19200 B**）、`buff_dma = 1` / `buff_spiram = 0`。想把它挪到 PSRAM 换出 19 KB
- **两种写法都失败**：
  1. `buff_dma = 1` + `buff_spiram = 1` → `esp_lvgl_port_disp.c:317-328` 会拼出 `MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM`。**S3 的 SPIRAM 堆没有 `MALLOC_CAP_DMA`**（IDF v5.5.3 `components/heap/port/esp32s3/memory_layout.c:63-65`：DRAM 有 DMA 无 SPIRAM，SPIRAM 有 SPIRAM 无 DMA），这个组合**谁都满足不了 → 分配返回 NULL** → `lvgl_port_add_disp()` 拿不到画缓冲 → LVGL 没有可用 display
  2. `buff_dma = 0` + `buff_spiram = 1` → 分配是成功了，但同样卡死
- **现象**（两次都是同一个签名，`build/ram_v3_boot.log` / `build/ram_v3b_boot.log`）：
  ```
  E (25504) task_wdt: Task watchdog got triggered. ... IDLE1 (CPU 1)
  E (25504) task_wdt: CPU 1: taskLVGL
  E (36384) Display: Failed to lock display
  ```
  从开机 14~25 s 起**每 10 s 一次、永不恢复**，直到复位；`xTaskGetState` 显示 LVGL 任务 100% 占着 CPU1
- **处置**：**回退**（`git diff main/display/lcd_display.cc` 为空）。这 19 KB 在 S3 上不划算
- **! 教训**：**`MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM` 在 S3 上不存在**。看到 `buff_dma=1 + buff_spiram=1` 这种"两个都要"的配置，先回读 `memory_layout.c` 的 caps 表，别指望分配器"尽力而为"——它是硬匹配，失败后只会静默少一块缓冲。另外：**LVGL 没有 display 时不是"什么都不画"，而是任务空转**，表现成 WDT 而不是黑屏，很容易误判成"性能不够"

---

## 六、计划与验收口径缺陷（照做会白干或验不了）

### BUG-034 把"换了网络"误判成"开着串口"：一次单变量没控住的对照，差点写进测量纪律

- **经过**（2026-09-20）：BUG-028 排查中先测出"**主机开着 COM10（读或不读都一样）→ 拍照上传 3~5 次全失败；关掉 → 4 次全成功**"，一度准备把"抓取时不要开串口"写成测量纪律。随后用户自己换了一次网络，发现真正的变量是**网络路径**：手机热点可以、PC 用网线接局域网 + 板子连该局域网的 AP 就卡死（BUG-028 的「2026-09-20 补充」）
- **为什么会被骗**：那两次对照之间**同时变了两个变量**——串口状态变了，网络也换了（用户换网是为了配合测试）。而"串口"这个变量在物理上根本不可能影响上传：**UART 没有硬件流控，ESP32 无法知道主机有没有在读**，抓取脚本唯一能碰到板子的是 `Open()` 时的 DTR/RTS 电平，而那两次前后 DTR/RTS 状态并没有稳定地跟着"成功/失败"走
- **! 教训（三条）**：
  1. **凡是"打开/关闭某个工具就复现"的对照，先问一句"这个工具在物理上怎么影响被测系统"**。说不通的相关性要么是巧合，要么还有一个没被识别的共同变量
  2. **一次只动一个变量**；用户"配合测试"时的顺手改动（换网、插拔、换线）就是最容易漏掉的那个共同变量，**动手前先问清"和上一次比，环境有什么变化"**
  3. 相关性再漂亮也别急着写进"纪律/规范"：先做一次**反向交替**（A/B/A/B）确认它可复现，再写

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

### BUG-026 计划让 HTTP 服务在**板级构造函数**里 `httpd_start()` → lwIP tcpip mbox 未初始化，断言复位无限重启

- **现象**：D5 烧入新固件后设备**无限重启**（每 ~1.5 s 一轮 `rst:0xc (RTC_SW_CPU_RST)`），每轮都停在同一个位置之后：
  ```
  I (1215) VehicleUi: 车载界面定时器已创建（1000 ms 刷新 / 60 ms 预览）
  assert failed: tcpip_send_msg_wait_sem /IDF/components/lwip/lwip/src/api/tcpip.c:454 (Invalid mbox)
  Backtrace: 0x403855b9:0x3fcb6400 ... 0x421213af:0x3fcb6590 0x421213fe:0x3fcb65b0 ...
  ```
  一次会话里 25 次（`build/acceptance_d5a_http.log`）
- **位置**：计划任务 10 步骤 3「在构造函数末尾（`GetBacklight()->RestoreBrightness();` 之前）插入 `http_ = new VehicleHttp(...); http_->Start();`」（计划文件 3095–3103 行）
- **根因**：`httpd_start()` → `httpd_server_init()` 要建 socket / `bind()` / `listen()`，进 lwIP 的 `tcpip_send_msg_wait_sem()`，而 **tcpip 线程是 `esp_netif_init()` 创建的**——它在 `WifiManager::Initialize()` 里（`managed_components/78__esp-wifi-connect/wifi_manager.cc:74`），而 `WifiManager::Initialize()` 由 `WifiBoard::StartNetwork()` 调用、那又是在 `main/application.cc:159` 才执行的，**晚于板级构造函数**。构造函数阶段 mbox 还是空 → 断言失败 → abort → 重启 → 同一行再崩，形成死循环
- **性质**：计划缺陷（**调用时机**错），不是接口写法错；代码本身编译干净、主机侧也看不出问题
- **修法**：构造函数里**只构造对象**（`http_ = new VehicleHttp(snapshot_store_);`），另重写 `Esp32S3Board::StartNetwork()`：先 `WifiBoard::StartNetwork()`（其内部已 `esp_netif_init()`），返回后再 `http_->Start()`。**不必等"连上 WiFi"**——协议栈初始化完就能绑 `0.0.0.0:80`
- **证据**：`build/acceptance_d5a_http.log`（复位循环）→ 修后 `build/acceptance_d5c_http.log`：`I (1700) VehicleHttp: HTTP 服务已启动：/  /latest.jpg  /events（端口 80，任务栈 6 KB 在 PSRAM，内部 RAM 69587 → 66295 B）`，烧录后新固件连续运行零复位；PC 端 `curl` 三个路由全部 200
- **! 教训**：**"在哪一行调用"和"代码写什么"一样会错**。任何走 socket / lwIP / `esp_netif` 的初始化都不能放在板级构造函数里；落地判据是"执行到这一行时 `esp_netif_init()` 跑过没有"。与 BUG-024（栈必须在内 RAM）同类：都是平台对**调用上下文**的隐式要求，编译器与主机测试都查不出来，只能真机跑

### BUG-027 计划给的 `LogAccessUrl()` 取错 JSON 路径：`ip` 在 `board.ip` 而不是顶层，串口永远打"还没拿到 IP"

- **现象**：修完 BUG-026 后 HTTP 三路由都正常，但串口只出
  `W (6730) VehicleHttp: 还没拿到 IP；联网后用串口里 WiFi 打印的 IP 打开 http://<IP>/`——而同一份日志里 `I (5310) WifiStation: Got IP: 192.168.137.168` 比它早 1.4 s
- **位置**：计划任务 10 步骤 2 的 `VehicleHttp::LogAccessUrl()`（计划文件 3044–3057 行）：`cJSON_GetObjectItem(root, "ip")`
- **根因**：`Board::GetSystemInfoJson()` 把板级 JSON **挂在 `board` 键下面**（`main/boards/common/board.cc:173`：`json += R"("board":)" + GetBoardJson();`），而 `"ip"` 是 `WifiBoard::GetBoardJson()` 里加的字段（`main/boards/common/wifi_board.cc:277`）→ 真实路径是 **`root.board.ip`**。计划那句注释（"wifi_board.cc:277 把 ip 放进了这份 JSON"）只核对了后半段，没核对嵌套层
- **修法**：先取 `board` 对象再取 `ip`（`cJSON_IsObject(board)` 判一次）；顺带把"5 s 只打一次"改成**最多 6 次尝试**（首次 5 s，之后每 3 s），WiFi 慢连时也能把地址打出来
- **证据**：`build/acceptance_d5c_http.log`：`I (6710) VehicleHttp: 手机浏览器打开：http://192.168.137.168/`
- **! 教训**：**跨文件的 JSON 字段要按"最外层键 → 内层键"核对生产端**，注释里的行号不算证据；这类错误不崩、只静默降级（这里表现为"功能全好、就是查不到地址"），最容易在验收里被漏过

### BUG-036 设计文档 §7.1 的 MQTT 主题名带斜杠，在巴法云上根本建不出来

- **现象/依据**：巴法云控制台"创建主题"只接受**字母或数字**组合（[平台操作教程](https://cloud.bemfa.com/docs/src/index_guild.html)），且**主题必须先在控制台创建好**才收得到消息。设计文档 §7.1 写的 `vehicle/{device_id}/event`、`/env`、`/status` 这类多级主题在控制台里建不出来
- **位置**：`docs/superpowers/specs/2026-09-16-vehicle-terminal-design.md` §7.1
- **修法**：改用**单级主题名**（`main/boards/esp32s3/config.h` 的 `BEMFA_TOPIC_EVENT/ENV/STATUS`：`vtevt01`/`vtenv01`/`vtsta01`），`device_id` 放进 payload 的 `dev` 字段。后三位刻意不用 `001`~`013`，避免被归类成"灯泡/插座"这类语音设备类型
- **附带**：设计文档 §6.1 说"切到 MN 后 `wn9` 仍留在模型包里"也是错的——`scripts/build_default_assets.py:853-856` 只在 ESP/AFE 唤醒词下打包 wakenet。实测 `idf.py build` 日志：`Note: Found wakenet models ['wn9_nihaoxiaozhi_tts'] but wake word type is not ESP/AFE, skipping` + `multinet models: mn7_cn, fst (will be packaged)`。结论无实际影响（`AfeAudioProcessor` 的 NS/VAD 本来就没打包），但文档这句是错的
- **证据**：`build/d6_task1_build.log`（assets 生成日志）；真机 `CustomWakeWord: Command: …` 9 行 + `Quantized MultiNet7: … name:mn7_cn`

### BUG-037 计划书 §9 的"幂等键 = 设备 ID + 事件序号"在重启后会误判重复

- **现象/依据**：`EventHistory` 的 `seq` 是 RAM 里的单调序号，**每次重启从 1 重新开始**（`main/vehicle/event_history.h`，`vehicle_service.cc` 的 `history_.Append`）。按计划书 §9 的键，第 1 次开机的 `seq=1` 与第 2 次开机的 `seq=1` 会被服务端判成同一条事件而被吞掉
- **位置**：`docs/计划书.md` §9；设计文档 §7.1/§5.3 同口径
- **修法**：幂等键改为 **device_id + boot_id + seq**；`boot_id` 每次开机在 NVS 里 +1（`main/boards/esp32s3/bemfa_client.cc` 的 `Start()`，`Settings("vehicle")` 的 `boot_id`），并在事件上报 JSON 里带 `"boot"` 字段
- **实测**：连续两次复位后串口分别打出 `本次 boot=2`、`本次 boot=4`（`build/d6_task5_mqtt.log`、`build/d6_task5_boot2.log`），键确实逐次变化
- **影响面**：只影响"补传去重"这一条口径；事件本身、落盘格式（`EventToJson(record)` 单参版）都没变，D5 的 `event_json_test.cc` 仍全绿

### BUG-038 巴法云首次连接几乎必然发生在"拿到 IP 之前"，只靠 esp-mqtt 自动重连要等 30~60 s

- **现象**：新固件第一次跑起来，串口是
  ```
  I (1900) BemfaClient: 巴法云上报已启动：设备 E83DC1FBA260 …积压容量 500 条（缓冲在 PSRAM）
  E (1900) esp-tls: couldn't get hostname for :bemfa.com: getaddrinfo() returns 202, addrinfo=0
  E (1920) mqtt_client: Error transport connect
  W (1940) BemfaClient: 首次连接巴法云失败（bemfa.com:9501）；esp-mqtt 会在后台自动重连
  I (18610) WifiStation: Got IP: 192.168.202.227
  I (30350) BemfaClient: 上报统计 已发=0 失败=0 丢弃=0 积压=143 连接=否
  ```
  即**第一次尝试时 WiFi 还没有 IP**，DNS 直接失败；此后一段时间 `连接=否`、积压 143 条不动
- **根因**：板级 `Esp32S3Board::StartNetwork()` 里 `WifiBoard::StartNetwork()` 是**异步**连 WiFi（`WifiBoard: Starting WiFi connection attempt` → 10 s 后才有 `Got IP`），函数返回时协议栈在、但网卡没连上。放在它后面的 `bemfa_->Start()` 于是"起得来、连不上"
- **性质**：设计文档 §3.1 把 `net_task` 挂在 `StartNetwork()` 之后的时序缺陷（不是接口写法错）；不崩，只是**静默不工作**——最容易被验收漏过
- **修法**：`BemfaClient::NetLoop()` 里加**自己的兜底重连**：未连接且距上次尝试 ≥ 15 s 就再调一次 `TryConnect()`。只在 `!connected_` 时调（`EspMqtt::Connect()` 第一件事是 `Disconnect()`，对已连上的连接再调一次会把好连接拆掉）
- **实测（改前/改后同一块板、同一网络）**：
  - 改前：拿到 IP 在 18.6 s，连上时间落在 **30~60 s 之间**（`build/d6_task5_mqtt_wait.log`：t=60.36 s 才有 `已发=145 连接=是`）
  - 改后：拿到 IP 在 6.19 s，**19.09 s 连上**（`I (19090) BemfaClient: 巴法云已连接（积压 143 条待补传）`，`build/d6_task5_boot2.log`），随后 t=30.24 s `已发=144 失败=0 丢弃=0 积压=0 连接=是`
- **遗留观察（未定性）**：`build/d6_task5_boot2.log` 里 `巴法云已连接` 打了**两次**（19.09 s / 20.10 s），中间**没有** `巴法云断开` 日志。两次 `OnConnected` 说明 `EspMqtt::connected_` 中途被复位过（`Disconnect()` 或 `MQTT_EVENT_DISCONNECTED` 都会复位，而后者带 `if (connected_)` 判断、可能因此不打回调）。不影响结果（0 失败、积压归零），但**如果 D7 的 2 h 测量里出现"连接=否/是"反复翻转，就是这里**，届时先查是不是"自己的重试 + esp-mqtt 自动重连"同时生效
- **! 教训**：**"异步连接"后面不要紧跟依赖网络的初始化**。判据不是"函数返回了"，而是"拿到 IP 了没有"。同类前科：BUG-026（构造函数里起 httpd，协议栈还没初始化）

### BUG-039 巴法云控制台每行有**两行文字**：粗体是"名称"，下面那行才是真正的 MQTT 主题值

- **现象**：`BemfaClient` 连上 `bemfa.com:9501`、`上报统计 已发=150 失败=0 丢弃=0 积压=0 连接=是` 全部正常，但**控制台里三个主题的"更新时间"/"消息"永远不动**（用 `vtevt01` / `vtenv01` / `vtsta01` 当主题名发了半小时）。换成端点 `mqttv2.bemfa.com:2023`（控制台"连接地址"列明示的那组）后**仍然不动**
- **根因**：控制台主题列表里**每行显示两行文字**——上面粗体那行是用户填的**名称**（`vtevt01`、`土壤湿度传感器`、`TBD_ctrl`…），**下面那行才是真正的 MQTT 主题值**（`wUV1aTSNK005`、`IQUxrCglW004`、`cSjvHK4fg012`…，末三位是设备类型码）。往"名称"发布，broker 照收不误（QoS1 也返回成功），但**不会落到那个主题上**，所以控制台毫无反应——一个完全不报错的静默失败
- **决定性旁证**：用户 STM32 工程（`18_Integrated/bsp/esp8266_mqtt.h`）里跑通的那几个主题 `xjbANQL7T004` / `muRn2H081004` / `IQUxrCglW004` / `8xpwLQNpG004` / `cSjvHK4fg012`，**正是控制台里"土壤湿度传感器""光敏传感器""TBD_ctrl"那几行下面的一串**；参考实现的 `mqtt_publish_data()` 就是把 `topic` 原样拷进 PUBLISH 报文的主题字段（无 `/set`、无 `/up` 后缀）
- **修法**：`main/boards/esp32s3/config.h` 的三个 `BEMFA_TOPIC_*` 改成**控制台下面那行的主题值**；注释里写清"名称 vs 主题值"
- **验证**：改后重烧，`已发=55 失败=0 积压=0 连接=是`，用户刷新控制台确认**更新时间变成当前时间**（`build/d6_task5c_uplink.log`）
- **! 教训**：**"发送成功"和"落地成功"是两件事**。MQTT 的 PUBLISH 对 broker 未知/不存在的主题通常照样返回成功，所以判据必须是**接收端有没有变化**（控制台更新时间/消息、订阅端收到），而不是发布端的计数。同类：BUG-038（连接上了但业务不通）
- **附带**：官方文档的 `bemfa.com:9501` 也不是不能连（实测能连上、能发），但本账号的主题是 V2 服务在管，**以控制台"连接地址"列为准**

### BUG-040 间歇性：命令词识别到、动作也执行了，但「设备状态」那一段播报没声音（未定性）

- **现象（用户报告 + 串口证据）**：D6 真机验收时，8 条命令词里**只有「设备状态」没有声音**，其余 7 条都正常；同一轮里「设备状态」说两次、两次都静默。而串口**两条链路都齐**：
  ```
  I (132010) CustomWakeWord: Voice command detected: action=status text=设备状态 prob=0.76
  I (132020) Esp32S3Board: VC_ACTION action=status intent=status
  I (132780) VoiceCommand: VC_DONE intent=status 环境=26.6℃/52%/496 lux 状态=行驶
  ```
  即"识别 → 派发 → 执行"全程正常。第二次（t=207070）同样三段齐全、同样没声音（`build/d6_task4_voice.log`）
- **已经排除的**（都不是原因）：
  1. **片段缺失**：`q_status_ok/q_online/q_events/unit_times` 四个 .ogg 都在 `main/assets/common/`，`main/assets/lang_config.h` 里 `OGG_Q_*` 常量齐全；`VoiceCommand::Play()` 里"片段没有对应音频常量"的告警**一条都没有**
  2. **容器格式**：5 个片段与能正常播的 `q_temp/tone_chime/unit_degree` 一样都是 `OggS + OpusHead + OpusTags`（逐字节头检查）
  3. **解包失败**：`main/audio/demuxer/ogg_demuxer.cc` **不用堆分配**（定长 `ctx_.packet_buf`），且版本/段数/溢出/容器异常都会 `ESP_LOGE/W`，日志里**没有**任何 OggDemuxer 告警
  4. **解码队列塞满**：`PushPacketToDecodeQueue(..., wait=true)` 会等，队列上限 `2400/60 = 40` 包
- **修后现状（不可复现）**：只加了几行逐段诊断日志（`播放 q_status_ok（4806 B，内部 RAM 余 19651 B）`）后重烧，「设备状态」**4/4 全部有声**（`build/d6_task4c_voice.log`，t=29690/38970/47690/66970），同一次运行里事件播报 `ev_driving/ev_hard_brake/ev_hard_accel/ev_parked` 也都有声。**代码逻辑没改**，所以判定为**间歇性、根因未定性**
- **保留的诊断手段**：`VoiceCommand::Play()` 里的逐段日志（片段名 + 字节数 + 内部 RAM 余量）**故意留着**。下次再遇到静默，一眼能分开"片段没交出去（0 字节/告警）"与"交出去了但没响（字节数正常）"
- **下一步（未做）**：怀疑方向是**音频输出电源状态竞争**——`AudioService::CheckAndUpdateAudioPowerState()`（`audio_service.cc:686-702`）每 1 s 判一次 `now - last_output_time_ > AUDIO_POWER_TIMEOUT_MS(15000)` 就 `EnableOutput(false)`，而 `PlaySound()` 只在**入口**判一次 `!output_enabled()` 再重开；两条路径都能开关功放（PCA9557 IO1），**没有互斥**。要验它得在 `EnableOutput` 两处加带时间戳的日志，制造"距上次出声 > 15 s 再下命令"的场景
- **怀疑方向已修正 + 埋点已完成（2026-09-20 晚，D7 开工前；用户批准继续追这条）**：
  - **修正 1：上面那条"定时器 vs PlaySound 竞争"其实很难成立**。`CheckAndUpdateAudioPowerState()` 关功放那一段是**双重条件**：
    ```cpp
    if (output_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->output_enabled()) {
        if (!(codec_->duplex() && codec_->input_enabled())) { codec_->EnableOutput(false); }  // ← 只有这里才真关
    }
    ```
    而本板 `BoxAudioCodec` 构造里 **`duplex_ = true`**（`main/audio/codecs/box_audio_codec.cc:12`），且待机时 MN 唤醒词一直在跑 → `AudioInputTask` 持续调 `ReadAudioData()` → `last_input_time_` 一直被刷新（`audio_service.cc:216`）→ **`input_enabled()` 恒为真 → 这个分支根本进不去**。也就是说"每 15 s 把功放关掉"在正常待机路径上**不成立**
  - **修正 2：新的首选怀疑 = PCA9557 输出锁存寄存器的"读—改—写"没有互斥**。`Pca9557::SetOutputState()`（`main/boards/esp32s3/esp32s3_boards.cc:3`）每次都是 `ReadReg(0x01)` → 改一位 → `WriteReg(0x01)`，而 **0x01 是三个功能共用的**：IO1=功放 PA_EN（音频）、IO2=摄像头 PWDN（抓拍/预览/关摄像头）、IO0=LCD_CS（显示初始化）。**没有任何锁**。两个调用者交错时会丢更新，典型序列：
    ```
    B(摄像头): 读 0x01 → 0x04
    A(音频):   读 0x01 → 0x04
    A(音频):   写 0x06  ← PA=1，功放开
    B(摄像头): 写 0x00  ← 用自己读到的旧值写回，PA 位被清掉
    ```
    结果：**`codec_->output_enabled()` 仍然是 true，但硬件功放已被关**，于是 `PlaySound()` 入口那句 `if (!output_enabled())` 不会重开、`AudioOutputTask` 也不会重开 → **PCM 照样送出去、`VC_DONE` 照样打印、就是没声音**，且**间歇性**。这与 BUG-040 的现象（识别/派发/执行三段齐全、只有声音没有、旧固件 2/2 静默而重烧后 4/4 正常）吻合，也比"定时器竞争"更能解释"软件标志与硬件状态不一致"
  - **已埋的观测点（4 处，全是为了分清"没交出去"与"交了没响 / 被谁关的"）**：
    1. `AudioService::PlaySound()` 入口：功放标志 + 距上次出声毫秒数
    2. `AudioService::AudioOutputTask()`：功放已关需重开时打距上次出声毫秒数
    3. `AudioService::CheckAndUpdateAudioPowerState()`：真关功放时打静默毫秒数（若从不出现 → 修正 1 得到证实）
    4. `Pca9557::SetOutputState()`：**谁改的（bit/level）、改成什么、写完回读什么**（若出现"回读值与写入值不符"或功放位被别的 bit 操作清掉 → 修正 2 得到证实）
  - **验证状态（如实）**：已编译（零 warning）、已烧录，**但一次复现都没跑到**——烧录后板子 USB 掉线（**BUG-043**），D7 测量中止。下次上电的第一件事就是重复"静默 > 15 s 再说「设备状态」"，看 4 条日志里哪条对上
  - **! 注意**：如果修正 2 成立，**修法是给 `SetOutputState()` 加互斥（或给三个 IO 各自的锁存影子值）**，而不是去动音频时序——那属于我们自己写的板级文件（`main/boards/esp32s3/`），改起来不碰上游
- **首次复现尝试（2026-09-20 19:0x，阈值已=30 的新固件，证据 `build/d7_bug040.log`）**：用户按协议做了 **10 次**"静默 ≥20 s 再说一次「设备状态」"，**用户报告其中有几次没声音**。串口侧 10 次**全部齐全**（`Voice command detected` → `VC_ACTION` → `播放 q_status_ok/q_online/q_events` → `VC_DONE`），并且：
  - **`静默 N ms 且输入未开，关闭功放` 一次都没出现** ⇒ **原"定时器 vs PlaySound 竞争"假设被否证**（与 `duplex=true` 的推理一致）
  - **10 次全是 `PlaySound 入口：功放标志=1`**，连间隔 **172 s** 的那次（`距上次出声=172051 ms`）标志也还是 1 ⇒ 软件侧功放**从未**被关过
  - **整个窗口零 `Pca9557: SET` 写入** ⇒ 也没人能悄悄清掉 PA 位（**修正 2 那条锁存竞争这次没被触发**，需要"摄像头 PWDN/功放同时动"的场景才能验）
  - **结论：静默发生在 `PlaySound` 的下游**（解包 → 解码 → `OutputData` → I2S/codec/功放）
  - **已补下一层埋点（已编译，未烧录）**：`AudioOutputTask` 里新增"一次播放的**首包 PCM 是否真的写出去了**"（静默 > 500 ms 后的第一包打一条 `OutputData 首包：静默 N ms 后开始写 PCM（N 样本）`）。下一次复现即可分开"**片段没走到 OutputData**（解包/解码）"与"**写出去了但没响**（I2S/codec/功放）"
  - **阈值=30 的旁证**：10 次识别概率分别是 0.66 / 0.46 / 0.90 / 0.47 / 0.70 / 0.33 / 0.31 / 0.56 / 0.30 / 0.91 —— **0.30 与 0.31 这种刚好压线的都被接受了**，说明阈值在起作用（D6 时=20，见过 0.20~0.48）
  - **! 未完成**：用户没记下具体哪几次没声音，所以还没能把"静默次数"与"日志里的某一次"对上。**下一轮必须让用户按序报序号**（第几次没声音），才能定位到具体那一条
- **第二次尝试（同日 19:0x，加了下游埋点的新固件，`build/d7_bug040b.log`）**：同一协议再做 10 次，**用户报告 10 次全都有声音**，且串口侧完全自洽：
  - 10 次 `VC_ACTION`（概率 0.86/0.84/0.56/0.54/0.56/0.45/0.43/0.69/0.37/0.51）
  - 10 次 **`OutputData 首包：静默 N ms 后开始写 PCM（960 样本）`** ⇒ **每一次都有真实 PCM 写进 codec**
  - 10 次**全无** `关闭功放`、全无 `Pca9557: SET` ⇒ 软件侧功放全程为开
  - 开机那次也端到端自洽：`PlaySound 入口：功放标志=0 距上次出声=10380 ms` → `Pca9557: SET bit=1 level=1 锁存 0x00→0x02（回读 0x02）` → `OutputData 首包：静默 10447 ms 后开始写 PCM`
  - **! 这一轮的对比价值**：上一轮（**没有**下游埋点的固件）用户报告"有几次没声音"，这一轮（**加了**日志的固件）10/10 有声。**存在"加日志改变了时序、把问题盖住"的可能（Heisenbug）**，不能据此说"修好了"——本轮**没有任何逻辑改动**，改的只是日志条数与限流阈值
  - **下次复现要抓的就是**：某一次**没有** `OutputData 首包`（= 片段没走到 codec，问题在解包/解码/队列），或者**有** `OutputData 首包` 但仍没声音（= 问题在 I2S/codec/功放/喇叭，属硬件侧）
- **! 教训**：**"没声音"必须先在代码里埋一条"我确实把这段交出去了"的证据**，否则会一直往"片段/格式/解码"上猜。这类"链路各段都打点、就是结果不对"的问题，第一刀是加观测点，不是改逻辑

### BUG-041 `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` 2048 → 512：内部 RAM 可用量 **+19 KB**（纠正 BUG-024 补充里"收益存疑"的判断）

- **背景**：BUG-024 的补充（2026-09-20 内部 RAM 优化实验）结论是"**腾不出来**"，并在末尾把 `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`（2048 → 512）列为"**没测、记下来备查**"的旋钮，附一句"按 V1 的教训收益存疑"。Plan C 加上巴法云 MQTT 客户端后内部 RAM 低水位掉到 **6167 B**（D5 基线 9267 B），所以回头把这颗没测的旋钮补上了
- **先量化"钱花在哪"**（`build/d6_ram_diag.log`，逐点差分，`MALLOC_CAP_INTERNAL` 口径）：
  ```
  [RAM] Start 入口           free= 64951 largest= 32768
  [RAM] 积压队列(PSRAM)后     free= 64703   ← 只花 248 B（80 KB 缓冲确实在 PSRAM）
  [RAM] net_task 后(栈在PSRAM) free= 64247   ← 只花 356 B（6 KB 栈确实在 PSRAM）
  [RAM] CreateMqtt 后         free= 64087   ← 只花 160 B
  [RAM] Connect 失败后        free= 56283   ← **一个 esp-mqtt 客户端独占 7768 B**
  [RAM] Connect 前(t≈17 s)    free= 19311   ← 系统自身初始化又用掉 ~37 KB
  ```
  即：**我们自己的代码合计只花 ~750 B 内部 RAM**（分层纪律是对的），7.8 KB 全是 esp-mqtt 客户端本身（`largest` 恰好掉 4096 → 那 4 KB 任务栈来自内部 RAM，`EspMqtt::Connect()` 里 `task.stack_size = 4096` 是上游定值）
- **改动**：`sdkconfig.defaults.esp32s3` + `sdkconfig` 的 `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` 2048 → 512（"小于此值一律进内部堆"的门槛降下来，让 513~2048 B 的分配倾向落 PSRAM）。改完必须 **touch `sdkconfig`** 再构建（BUG-024 补充里的操作陷阱）
- **实测 A/B（同一块板、同一天、同一网络，逐点同口径）**：

  | 观测点（内部 RAM free） | =2048（改前） | =512（改后） | 变化 |
  |---|---|---|---|
  | `Start 入口` | 64,951 | **75,659** | +10,708 |
  | 一个 esp-mqtt 客户端占用 | 7,768 | **6,200** | −1,568 |
  | **系统初始化后（连接前）** | **19,311** | **38,311** | **+19,000** |
  | t≈15 s `free sram` | 13,971 | **36,011** | +22,040 |
  | t≈25 s `free sram` | ~20,371 | **29,675**（min 21,915） | +9,300 |
  | 低水位（`minimal sram`） | **6,167** | **19,179 ~ 21,915** | **+13,000 ~ +15,700** |

- **回归检查（这是"全局引导策略"改动，必须逐项过）**：
  1. **LVGL 画缓冲**：`main/display/lcd_display.cc:154` 是 `buff_dma = 1`，`esp_lvgl_port_disp.c:323-324` 于是显式要 `MALLOC_CAP_DMA`（内部 RAM），**不经过本门槛** → BUG-032 的坑结构上避开
  2. **LVGL 端口任务栈**：`esp_lvgl_port.c:84-85` 显式 `MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT` → 不受影响
  3. **我们自己创建的任务**（`imu_task`/`worker_task`/`vehicle_http`/`bemfa_net`）栈与 80 KB 队列全部显式指定 caps → 不受影响
  4. **碰 flash 的路径**：第 3 条保证 worker 栈仍在内 RAM；真机实测 `SnapshotStore: 尾部窗口 8143 B 里 seq > 6 的有 54 条`（SPIFFS 读通）、`本次 boot=29`（NVS 写通）
  5. **真机 150 s + 25 s 两次运行**：零 `abort` / 零 `assert failed` / 零 `NO_MEM`；IMU、MN 模型、LVGL 界面、MQTT 上报（`已发=55 失败=0 积压=0 连接=是`）全部正常
  6. **用户肉眼确认**：屏幕（画面/颜色/触摸）、语音播报、小智对话 **三项均正常**
- **未复测**：旧版那个 6167 B 的**极小值出现在一次小智对话中**（`StateMachine: speaking -> listening`），而本轮的复测抓取在 30 s 时被别的进程抢走串口（`Access to the port 'COM13' is denied`）而中断，**没覆盖到对话窗口**。所以"对话路径下的新低水位"目前只有估算（≈38 KB − 旧版同路径约 12.7 KB 的瞬时回落 ≈ 25 KB），**没有实测**。下次 D7 测量要补这一条
- **! 教训（两条）**：
  1. **"没测、收益存疑"不等于"没用"**。BUG-024 补充里那颗没测的旋钮恰好是收益最大的那颗；写"存疑"可以，但**别让它顺手变成"别试了"**——本轮正是因为它顶着 6167 B 才回头去试
  2. **排查内存要逐点差分**，别猜"大概是 esp-mqtt 吧"。四个观测点把"我们自己的代码 ~750 B"和"上游客户端 7.8 KB"干净地分开，直接决定了该不该去动 `managed_components`（结论：不用动）

### BUG-042 复位脉冲会让**已打开**的串口抓取句柄失守：`Access to the port 'COM13' is denied`

- **现象（2026-09-20 晚，D7 抓开机日志）**：`capture_serial.ps1` 先打印 `attached COM13 at 17:27:14 for 90 s`、再 `reset pulsed`，紧接着在 `ReadLine()` 上抛
  ```
  fatal: Exception calling "ReadLine" with "0" argument(s): "Access to the port 'COM13' is denied."
  ```
  注意顺序：**端口是先打开成功的**，失守发生在 RTS 脉冲**之后**
- **定性（有旁证）**：**不是别的进程抢口**。紧接着用**新句柄**打开同一端口，成功并打出完整开机横幅（`ESP-ROM:esp32s3-20210327` / `Build:Mar 27 2021` / `rst:0x1 (POWERON)` / `octal_psram: …`），说明端口当时没被占——是**原来那个句柄在复位事件之后失效了**。至于失效的机制（USB 重新枚举导致句柄作废 vs. 驱动层失效），**没有再往下验**，不影响修法
- **影响面**：D6 交接里记的"本轮有 **3 次**测量因 `Access to the port 'COM13' is denied` 中断"，当时归因为"串口会被抢"；**至少有一部分要归到本条**——那是脚本自己脉冲复位把句柄搞掉的，不是外部程序抢的（另一部分仍可能是 BUG-043 的 USB 掉线）
- **修法（已改 `build/capture_serial.ps1`；该文件 gitignored）**：把**复位与抓取拆成两个句柄**——先建一个临时 `SerialPort` 只做 RTS 脉冲（`DtrEnable=$false` 保持 GPIO0 高，正常启动），`Close()` 后 `Start-Sleep -Seconds 2` 等 USB 稳定，**再**打开抓取句柄。原写法是"先开抓取句柄、再脉冲"（顺序反了）
- **验证状态（❌ 实测无效）**：2026-09-20 18:32:00 实测——脚本先打印 `reset pulsed (temporary handle closed)`、再 `attached COM13 at 18:32:00`，**紧接着仍然 `Access to the port 'COM13' is denied`**。即"拆成两个句柄"**没解决问题**
- **! 结论修正（同日 18:34，拿到 `build/d7_grab.log` 后）**：真正的机制不是"我们的脉冲废了句柄"，而是**板子自己在反复重启**——句柄失效的时刻与板子 `rst:0x1 (POWERON)` 重启的时刻一一吻合（见 **BUG-044**）。板子掉电 → USB 整设备重新枚举 → 已打开的句柄作废。抓取脚本里 `DtrEnable/RtsEnable` 本来就都是 `false`，**在供电不稳的前提下任何抓取都躲不过**
  - **仍然成立的部分**：`Access to the port is denied` **不等于**"别人占着"——改成两个句柄后端口能打开、能读出横幅，说明端口没被占
  - **下一步**：先解决 **BUG-044**（供电/线缆），再回头看本条的脚本改法有没有必要——现在这个"先复位后开端口"的改动**不构成修复**，只是把复位与抓取分开而已
- **! 教训**：看到 `Access to the port … is denied` **不要先怀疑"别人占着"**。先问两句：**这个句柄是什么时候打开的？中间发生过复位/USB 事件吗？** 本条的答案是"开着的时候被自己复位废掉的"

### BUG-043 烧录中途 `The chip stopped responding`（原样重试即成功）；随后 COM13 整个消失、COM10 长期打不开

- **现象（2026-09-20 晚，D7 烧录 + 抓取）**，四段连续发生：
  1. `idf.py -p COM13 flash` 写到 `Writing at 0x009e1c03... (28 %)` 时 esptool 抛 `StopIteration` → `A fatal error occurred: The chip stopped responding.`（`build/log/idf_py_stdout_output_37380`）。**此时 app 分区只写了一部分，板子在重烧成功前是坏状态**
  2. **同一条命令原样重试即成功**（`Hash of data verified.` → `Hard resetting via RTS pin...`），中间没有任何环境改动、没换波特率、没换端口
  3. 烧录成功后抓了一次开机日志（读到完整 `ESP-ROM` 横幅），随后 **COM13 从系统里消失**：`[SerialPort]::GetPortNames()` 只剩 `COM3,COM5,COM6,COM4,COM10`
  4. `COM10` **全程在列表里但打不开**：`Access to the port 'COM10' is denied`（与 D6 交接"COM10 本轮长期打不开"一致）→ COM10 是**僵尸口**，别拿它当备份通道
- **定性**：属**硬件/链路层**（USB 线缆/接触/供电），不是代码或工具链缺陷。三条旁证：① 同命令原样重试即成功；② 失败点在数据流中途（28%）而非固定偏移；③ D6 交接已记过同一现象（"板子 USB 会掉线，COM13+COM10 双双从系统消失，用户拔插后恢复"，`2026-09-20-d6-closed-d7-pending-handoff.md` 环境要点第 4 条）
- **恢复手段**：**只有拔插 USB**（COM13 消失后端口不再出现）。D7 本次中止于此
- **! 教训（两条）**：
  1. `The chip stopped responding` 的**第一动作是原样重试**，不是改波特率、换端口或怀疑代码。也别当成"没烧进去"——它不在 0% 失败，**看到 flash 中途失败就先重烧再做别的**，否则板子停在半写状态
  2. 长时间测量必须在**不动线、不挪板子、不插拔**的条件下做。测量途中一旦 COM13 消失，这个窗口直接作废，别拿它拼接数据

### BUG-044 **板子自己反复 `rst:0x1 (POWERON)` 重启**（COM13 上 ~17 s 一次、COM11 上 ~192 s 一次），USB 双口随之同步消失（D7 的硬阻塞）

- **现象（2026-09-20 18:33–18:34，`build/d7_grab.log`，46 s 内 3 次开机）**：
  - 三个句柄期各拿到一次完整开机横幅，**每次都是 `rst:0x1 (POWERON)`**，三次时间 18:33:12 / 18:33:33 / 18:33:58
  - **整段日志里没有任何 panic / `assert failed` / `Guru Meditation` / `rst:0xc`(RTC_SW_CPU_RST)**——不是固件崩溃，是**上电复位**
  - 句柄死亡时刻 = 板子重启时刻：`### handle died #1 @ 18:33:29` → 4 s 后新横幅；`#2 @ 18:33:51` → 7 s 后新横幅
  - **端口级旁证**：单独轮询 `GetPortNames()`（**完全不打开端口**）也能看到 **COM10 与 COM13 同步出现/消失**（约 10 s 在、约 4 s 不在）——两块口是同一个 USB 设备的两副面孔，设备整个消失 = 板子断电重新枚举
- **触发时机高度规律**：开机 → ~10 s 进 idle → 音频初始化 + **`Pca9557: SET bit=1 level=1`（开功放）→ 播开机提示音** → **5~7 s 后 POWERON 重启**。三次都落在"功放开 + 出声"之后
- **对比基线**：D6 同板同固件形态连续跑过 **400 s 零固件复位**（`build/d6_task5c_uplink.log`），D5 还跑过 73.8 分钟。所以**这是今天新出现的状态**，不是"一直如此"
- **可疑方向（未定性，按可能性排）**：
  1. **供电被音频/功放电流尖峰拉崩**——时机吻合（PA_EN 拉高 + 出声后几秒必重启）；D6 之前的 BUG-006 处置记录里就有"换线 / 主机直连 USB 口 / **独立 5V 供电**"这套措施，属同一类问题
  2. 用户这次拔插后**插到了供电能力更差的端口**（或经了 hub）
  3. EN 引脚被干扰/被串口桥的 RTS 拉动（本板 USB 侧有两个口，是桥+原生并存）
- **已排除**：固件崩溃（无 panic、复位原因不是 `rst:0xc`）、看门狗（不是 `rst:0x7/0x8`）、软件重启（不是 `rst:0x3`）
- **下一步（判据明确）**：**换线 / 换到主机直连的 USB 口 /（必要时）独立 5V 供电**，然后**先完全不开任何串口句柄**，只轮询 `GetPortNames()` 60 s
  - 若 COM10/COM13 **不再闪** → 供电/线缆确认是根因，接着抓 2 分钟看 `rst:0x1` 是否为 0
  - 若**仍然闪** → 与本机脚本无关，属板子硬件侧，需查供电电路/EN 网络
- **! 影响**：D7 的**任何**长时间测量（2 h 无复位、断网补传、20 min 三链路并行）在此问题解决前**都不能做**；`flash` 也会因为中途掉电而失败（BUG-043 的成因很可能就是它）
- **✅ 已恢复（2026-09-20 18:51，用户拔插并换到另一个 USB 口）**：**同一块板、同一版固件、只换了 USB 口**，口号从 `COM13` 变成 `COM11`（**交接文档提醒过的"口号可能变"，这次真发生了**）：
  - 单次打开连抓 **90 s**：`rst:0x` **0 条**、无 `fatal:`，抓到
    ```
    I (30320) BemfaClient: 上报统计 已发=53 失败=0 丢弃=0 积压=0 连接=是
    I (60320) BemfaClient: 上报统计 已发=55 失败=0 丢弃=0 积压=0 连接=是
    I (90320) BemfaClient: 上报统计 已发=56 失败=0 丢弃=0 积压=0 连接=是
    I (109580) SystemInfo: free sram: 37567 minimal sram: 18987
    ```
    即**已经跨过原来必死的 17.2 s（MQTT 重连那一下）**，且比原来多活了 6 倍时间
  - **结论**：根因在**供电/链路层（那个 USB 口）**，不在固件——这一条同时把"是不是新固件导致的"彻底排除（本轮的 3 处改动都还在跑）
  - **! 保留的疑问**：原来那个口是否**彻底**不可用没做对照（COM13 也曾有过 54.5 s 稳定窗口），所以严格说是"**换口后稳定**"，不是"**旧口必坏**"。下次若再出现 `rst:0x1` 连发，第一动作仍是换口/换线
  - **! 教训**：本条的三个观察（端口成对闪烁、句柄死亡=重启时刻、死前一行固定在 MQTT 连接）**都指向硬件，但最容易被误读成"新固件有 bug"**——因为时间上正好赶在改完三个决定之后。判据是**复位原因**（`rst:0x1 POWERON` vs `rst:0xc` 带 panic）与**有无 panic 文本**，不是"最近改了什么"（BUG-034 的同款教训）
- **! 再次复现（19:02，300 s 抓取之后）**：板子又进了"**USB 口在列表里但打不开**"的状态——`COM11` 存在，`Open()` 抛 `Incorrect function.`，`idf.py -p COM11 flash` 报 `A fatal error occurred: Could not open COM11, the port is busy or doesn't exist.`（`build/d7_flash4.log`）。时间线：18:53:20 抓取开始（板子 uptime ≈136 s，端口正常）→ 输出在 **uptime 289.58 s（18:55:54）无声停止**（**无 `fatal:`、无 `rst:0x`**）→ 18:58:20 抓取正常结束 → 19:02:55 端口已不可用。**恢复手段仍然只有拔插**
- **! 新观察：打开这个串口本身可能就会复位板子**——四次打开端口（18:33:12 / 18:33:33 / 18:33:58 / 18:38:32），**每一次都在同一秒内**紧跟一条 `ESP-ROM … rst:0x1 (POWERON)` 横幅。ROM 横幅只在芯片真的重启那一刻打印，且 CDC 缓冲在无主机连接时会被丢弃 ⇒ 最合理解释是 **open 时的 DTR/RTS 电平沿被 USB-Serial-JTAG 当作复位序列**。**未最终定性**（也可能是"板子本来就在重启循环、恰好被撞上"，但连续 4 次撞上的概率很低）
  - **影响与对策**：① 每次抓取开始都顺带复位一次板子（这正是"开机日志总能抓到"的原因，也是**不能**把它当"冷启动证据"的原因）；② 若某次复位把芯片带进 ROM 下载模式，就表现为"端口在、打不开、没输出"，只能拔插；③ **测量期间尽量少开关端口**（一次会话只开一次），更**不要用 `-Reset`**
- **! 第三次观察（19:07:15–19:10:17，`build/d7_bug040b.log`）：换到 COM11 只是把周期拉长，没根治**——该窗口里板子 19:07:04 开机，**19:10:17（uptime 192.25 s）句柄再次 `Access to the port 'COM11' is denied`**（板子又复位+USB 重新枚举），360 s 的窗口因此提前结束。⇒ COM13 上约 **17 s** 一次、COM11 上约 **192 s** 一次，**都不是能撑 2 h 的状态**
  - **对策（本轮采用）**：识别率这类"只关心命令词命中数"的测量改用 `build/d7_grab.ps1`（**句柄死了自动重开**，断点续记到一个日志里）；但**稳定性测量绝对不能用它**——重连本身会扰动（BUG-005/018），复位计数会失真
- **! 第四次观察（19:36，我自己的操作把端口搞坏了，如实记录）**：为验证"端口是不是被别的进程占着"，我**连续开关 COM11 十次**做压力测试——**前 2 次成功，第 3 次起全部 `Incorrect function.`**（8/10 失败）。同一时刻那 4 个残留的 ESP-IDF python 进程已被清掉、VS Code 的串口监视器已关，所以**不是别人占口，是我这串开关本身把它推坏的**
  - ⇒ **操作纪律：一个测量会话只开一次端口，绝不要用"反复开-关"去探测端口**；探测端口是否可用请用 `GetPortNames()`（只读，不碰驱动）
  - 同时说明：`Incorrect function.` **不是**"端口被占用"（占用报的是 `Access to the port is denied`），而是 **USB CDC 自身进了不可用状态**，**只能拔插恢复**
- **! 附带排掉的一条嫌疑**：机上曾有 **4 个今早 10:46 起就挂着的 ESP-IDF python 进程**（`idf5.5_py3.11_env` / `idf-python\3.11.2`）+ 18 个 VS Code 进程，一度怀疑是 IDE 的串口监视器长期占着 COM11/COM10（CLAUDE.md 记过同类前科）。**用户关掉监视器后那 4 个进程消失，但端口状态没有任何改善** ⇒ **不是它们**

### BUG-045 `ESP_LOGx` 里写 `%lld` 会原样打出 `ld`——毫秒数根本不会打出来（我自己加的诊断日志踩的）

- **现象**：BUG-040 那条新日志实测输出成
  ```
  I (10400) AudioService: PlaySound ?????????????????????=0 ???????????????=ld ms
  ```
  格式串是 `"PlaySound 入口：功放标志=%d 距上次出声=%lld ms"`。`%d`（功放标志=0）正常，**`%lld` 打出来就是字面量 `ld`，数值没了**（`build/d7_grab.log` 行 238；`?????` 是抓取脚本编码问题，与本条无关）
- **根因**：ESP-IDF 的日志格式化**不支持 `%lld`/`%llu`** 这类 64 位长度修饰（`ESP_LOGI` 只保证 `%d/%u/%x/%s/%c/%p/%.Nf` 这一组）。`duration_cast<...>.count()` 返回的是 64 位，`(long long)` 转过去正好踩上
- **修法**：**毫秒数一律 `%d` + `(int)`**（int32 毫秒够 24 天，本项目绝对够）。已改 `main/audio/audio_service.cc` 的 3 处（`PlaySound` 入口 / `AudioOutputTask` 重开 / `CheckAndUpdateAudioPowerState` 关闭）
- **! 教训**：**加诊断日志之后必须回读日志本身，确认它打对了数值**。这次差点拿着一堆 `ld` 去分析时序——"观测点"本身没被验证时，它比没有观测点更危险，因为它看起来像有数据

### BUG-046 `CONFIG_CUSTOM_WAKE_WORD_THRESHOLD` 20→30 的"防误触发"改动**反而砍掉一半召回**（连唤醒词一起），已回退

- **背景**：D6 交接（`2026-09-20-d6-closed-d7-pending-handoff.md` 开放问题 2）与验收记录 §6/§8 都建议把阈值 20 调到 30，理由是"**疑似误触发**"——判据是"`车内湿度` 命中 14 次、`车内温度` 9 次，远多于用户实际说的遍数"
- **! 那个前提是凭记忆估的、没有计数**。D7 用同一块板、按"8 条 × 各 10 次"协议重测（**阈值=30**、其余不变；证据 `build/d7_voice.log`，19:11:06–19:21:11，**窗口内零复位、零重连**）：

  | 命令 | 命中/10 | 命令 | 命中/10 |
  |---|---|---|---|
  | 车内温度 | 4 | 设备状态 | 4 |
  | 车内湿度 | 6 | 播放提示音 | 4 |
  | 光照多少 | 5 | 重新抓拍 | 4 |
  | 有没有人 | 3 | 锁车 | 7 |

  合计 **37/80 ≈ 46%**（D7 目标 ≥90%）；**唤醒词「你好小智」10 分钟内只命中 2 次**（用户独立反馈"喊它也没反应"，两边一致）
- **根因（有分布证据，不是猜）**：MN 报告的识别概率 **0.300 ~ 0.901，其中 12 个落在 0.30 ~ 0.34** —— **0.30 这个门槛正好切在概率分布的峰值上**。而 ESP-SR 的 `set_det_threshold` 是**全局**门槛（`main/audio/wake_words/custom_wake_word.cc:120`），唤醒词与 8 条命令**共用一个值**，所以抬阈值会**同时**废掉唤醒
- **顺手排除的另一个杠杆**：麦克风输入增益 `input_gain_` 已是 **30 dB**（ES7210 上限 33、步进 3，`main/audio/codecs/box_audio_codec.cc:17`），最多再 +3 dB，拉不动
- **处置**：**回退到 20**（`sdkconfig` + `sdkconfig.defaults.esp32s3`，已 touch），按同一协议**重测召回 + 补测一个静默窗口统计误触发**（这次两边都有实测数，不再靠估）
- **! 教训（三条）**：
  1. **"疑似"不能当调参依据**：把"命中次数 > 用户说的遍数"这类**凭记忆的估计**当成依据，方向可能整个反过来。要调参**先计数**
  2. **概率分布比命中总数有用**：只看"命中 37 次"只能知道不达标；看到**分布贴着门槛**才知道是门槛切太狠，而不是模型或麦克风不行
  3. **全局门槛要按最弱的那个用途定**：唤醒词与命令词共用阈值时，抬阈值 = 拿唤醒灵敏度换命令准确率。**改之前必须两边都测**
- **✅ A/B 结果（同日 19:39–19:51，**阈值=20**，证据 `build/d7_voice20.log`；窗口内**零复位、零重连**）**：
  - **召回**（按"每条约 10 次"、分母 80 粗算）：温度 6 / 湿度 8 / 光照 6 / 有没有人 6 / 设备状态 7 / 提示音 7 / 抓拍 5 / 锁车 13（**超分母，见 BUG-047**）= **58/80 ≈ 72.5%**（阈值=30 时是 **37/80 ≈ 46%**）
  - **误触发**：开头**完全静默 ~100 s**（19:39:50–19:41:30）期间 **0 次命令命中、0 次唤醒** —— 与"抬阈值防误触发"的前提**相反**，阈值 20 在这段里一次误触发都没有
  - ⇒ **20 明显优于 30（+26 个百分点召回，误触发仍为 0）**，回退是对的。但 **72.5% 仍未达 ≥90%**：这批命令词的实际命中概率 **0.20~0.64、多数落在 0.21~0.40**，20 与 30 切的是同一团分布 —— **阈值这条路已经走到头**，要再往上只能改命令词拼音写法（计划任务 1 步骤 13 的试法 B）或换更清晰的发音/拾音口径
  - **唤醒词**：这一轮用户没单独测「你好小智」，所以**阈值 20 下的唤醒灵敏度仍未测**（阈值 30 时 10 分钟只命中 2 次）

### BUG-047 用户报「锁车有时播报多次」；`lock` 命中 13 次 > 分母 10 —— **自触发与重复上报两个假设都被实测否证，未定性**

- **现象**：阈值=20 的 8×10 测量里，用户反馈"**锁车的时候有时会播报多次**"；串口侧 `lock` 命中 **13 次**（其余 7 条分别是 4~8 次，只有它超了分母 10）。日志里能看到成簇的 1~2 s 间隔：
  ```
  19:48:56  lock prob=0.23 → 播放 lock_entered（4897 B）→ 状态=停车
  19:48:57  lock prob=0.26 → 播放 lock_exited（5406 B）  ← 距上一次播报开始 1.38 s
  19:48:59  lock prob=0.24 → 播放 lock_entered         ← 距上一次 1.53 s
  ```
  每次命中都在"锁车监测"与"停车"之间**翻转一次状态**，并各播**一条不同**的片段
- **✅ 已定性：自触发成立（2026-09-20 深夜；用户直接观察 + 时间线双重证据）**：
  - **用户直接观察（决定性）**：**说一次「锁车」，设备连续播了好几次**；最近一次实验里出现「已进入锁车监测」紧跟「已解除锁车监测」两条
  - **片段时长实测**（从 OGG 末页 granule ÷ 48000 算出）：`lock_entered` = **1.25 s**、`lock_exited` = **1.35 s**（对照 `q_status_ok` = 1.20 s）
  - **时间线对齐**：`19:48:56.234` 开始播 `lock_entered`（1.25 s → 约 `19:48:57.48` 放完）→ **`19:48:57.614` 又命中 lock**（= 放完后 **约 0.13 s**）；`19:48:57.634` 开始播 `lock_exited`（1.35 s）→ **`19:48:59.164` 又命中**（= 放完后 **约 0.18 s**）。**这个 130~180 ms 的固定偏移，正是"MN 刚认完喇叭里念出的『锁车』两个字"的瞬间**
  - **机制**：播报片段**含命令词**（"已进入 / 已解除**锁车**监测"），而本板 `AUDIO_INPUT_REFERENCE=false`（**无 AEC**）⇒ 喇叭声被麦克风听回去 → MN 再报一次 → 状态翻转 → 播另一条 → 循环
  - **为什么只循环 2~4 次就停**：自触发的概率只有 **0.21~0.26**，**就压在阈值 0.20 上**，每次迭代约一半概率能继续 → 自然衰减
  - **为什么"阈值抬到 30"貌似有效**：0.21~0.26 全落在 0.30 以下被切掉（lock 命中 13 → 7），**代价是把真话（0.20~0.35）一起切掉**——自触发与真话的概率**完全重叠**，**阈值治不了这个**（见 BUG-046）
- **! 我上一轮的判断是错的，留档**：我拿"判决实验（只说一次 + 安静 15 s，19:52:06 那次）没复现"去**否证**自触发，并推断"用户实际说的遍数多于 10"。**错在用一次试验去否证一个间歇性现象**——自触发每次迭代只有约一半概率，一次没出不构成否证
  - **教训**：间歇性现象要么**重复 N 次**去测，要么**用已观测事件的时间线去对齐**（这次靠 130~180 ms 的固定偏移 + 片段时长定了性），**不能靠"我试了一次没出"来排除**
- **✅ 修法（已实现并烧录，2026-09-20 深夜）：播报期间加"命令词保护闸"**
  - `VoiceCommand::Play()` 按片段时长估算关闸（`字节数 / 4`，实测约 **4 B/ms**：4897 B/1.25 s、5406 B/1.35 s、4806 B/1.20 s），再叠 `kAnnounceGuardTailMs = 1500 ms` 余量（MN 检测窗 3 s，自触发实测落在播报开始后 1~4 s）
  - `VoiceCommand::OnVoiceAction()` 在闸内**丢弃**命令词，并打 `播报保护闸内丢弃命令词 action=...（闸内还剩 N ms）`——**这条日志本身就是自触发的实测计数**
  - 改动只在 `main/boards/esp32s3/voice_command.{h,cc}`（**我们自己的文件，不碰上游**）
  - **验证状态**：✅ 编译零 warning、已烧录；**⚠️ 首次验收未触发自触发**——用户按"说一次锁车 + 静默 8 s"重复 5 次，串口侧 **4 次命中 / 4 条播报 / `播报保护闸内丢弃` 0 次**（`build/d7_guard.log`，20:07:12–20:08:30），即**这段窗口里什么都没被拦，因为它根本没发生**。⇒ **"没重复"这次不能算修法生效的证据**
    - 旁证：这次 4 次命中的 prob 是 **0.30 / 0.33 / 0.43 / 0.49**，而上次发生自触发那批只有 **0.21~0.26** ⇒ **用户说得偏轻、概率压到 0.2x 时才容易引出自触发**（这也解释了为什么它时有时无）
    - **下次验收判据（要么看到拦、要么看到没被拦）**：跑一个**能引出自触发的长窗口**（上次是 10 分钟、连续多条命令的会话），窗口里**出现 `播报保护闸内丢弃` 且紧随其后没有第二次播报** = 修法生效；若**出现两条播报而没有任何丢弃日志** = 闸长不够（`kAnnounceGuardTailMs` 1500 → 调大）

---

## 七、待办索引（D5 结项后，未办事项一览）

> 这一节只做**索引**，细节都在上面各条里。开工前先扫一遍这里，挑一条动手。
> 状态时间点：**2026-09-20**（BUG-029/030 已修完；内部 RAM 实验、设置页核对、`set_page` 三个取值都已办完；BUG-028 定位到"跟网络路径有关"）。

| # | 待办 | 指向 | 前置/注意事项 |
|---|---|---|---|
| 1 | ~~修"关摄像头 → 打开实时预览 → 整个 app 卡死"~~ **已修完并真机验证（2026-09-18 深夜）** | **BUG-029** 的「修法 / 验证」两段；配套新增 **BUG-031**（运行期不能重建相机驱动） | 修完的回归口径：关 → 进预览页（不卡死、五个导航键可用、顶部显示「摄像头已关闭」）→ 重新打开（出图、≈10 fps），连续 3 轮 |
| 2 | **小智拍照上传间歇性挂死**（"卡 1 分钟"已修；2026-09-20 定位到**跟网络路径有关**） | **BUG-028**（含「2026-09-20 补充」）+ **BUG-030**（重复 `GetStatusCode()` → 白等 60 s，**已修**）+ **BUG-034**（别把它误判成"串口"） | 现状：**手机热点可以、PC 网线接局域网时卡死**（同一块板同一版固件，只换网络）。失败签名：连接后**正好 60 s** `Send failed errno=128` → 再 30 s 头超时。**下一刀**：把 `Explain()` 的 chunked 改成一次性 `Content-Length`，在**有线局域网**上复测（若能通 = 中间盒不认 chunked，属真修复） |
| 3 | ~~内部 RAM 优化实验（Plan C 前置）~~ **已做完 ✅（2026-09-20），结论：腾不出来** —— **! 但 2026-09-20 晚已被 BUG-041 部分推翻：`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` 2048→512 实测 +19 KB** | **BUG-024「补充（2026-09-20 内部 RAM 优化实验）」**（结构表 + 四个旋钮 A/B）+ BUG-032 + **BUG-041** | 实测：通用内部堆 22 KB **空载就 99.7% 满**，低水位是结构性的；V1（WiFi/lwIP 进 PSRAM）**更差**、V3（LVGL 画缓冲进 PSRAM）**两种写法都崩**、V5（TCP 缓冲减半）**中性**。**给 Plan C 的做法：自己的缓冲一律显式 PSRAM**（已照做，BemfaClient 只花 ~750 B）；**另外必须把 `SPIRAM_MALLOC_ALWAYSINTERNAL` 降到 512**（BUG-041：系统初始化后可用 19.3 → 38.3 KB）。另：改 `sdkconfig` 后必须 touch 它，否则编的还是旧配置 |
| 4 | **稳定性测量的三条纪律**（做 D7"连续 2 小时"指标前必读） | **BUG-006** 末尾 | ① 抓取脚本用"只开一次端口、不重连"的写法；② 测量窗口内不烧录/不擦分区；③ 统计**按复位原因分类**，别只报总数（D5 那次 40 分钟 18 次复位里只有 8 次是真固件 abort）。**已有基线：安静环境实测 73.8 分钟（4426 s）无复位**，D7 的 2 小时指标只差约 46 分钟，照这三条纪律测即可 |
| 5 | ~~设置页读数**目视核对**~~ **已核对通过 ✅（2026-09-20）** | 验收记录 **§10.1** | 阈值行（0.35/0.30/1.60/2.50 g、30 s/120 s）、"环境源：模拟"、事件容量 64、"当前：<状态>（标定中）"、三轴开关：用户逐项核对全部一致，且无文字重叠/越界（BUG-021 未复现） |
| 6 | ~~`self.vehicle.set_page` 的 `home`/`settings`/`chat`~~ **已补验通过 ✅（2026-09-20）** | 验收记录 **§10.1** | 串口取证：工具分别返回 `home` / `settings` / `chat`，屏幕切换正确 |
| 7 | `self.vehicle.status` 的 `last_event` 字段 | 同上 §8.3 | **已补验 ✅**（`events_total=54` 时返回了 `{"seq":54,"type":"parked",…}`），留在这张表里只是提示"其余工具字段别漏" |
| 8 | **Plan C 要用的巴法云连接凭据**（AppID / SecretKey）已收到并保存 | 仓库根的 **`.env`**（2026-09-18 用户提供；**已被 `.gitignore` 忽略，不进版本控制**——已用 `git check-ignore -v .env` 验证，`git status` 里也不会出现） | 新建 `bemfa_client.cc` 时从这里取：推荐在 CMake 里读入并生成一个 gitignored 的头文件，或填进同样被忽略的 `sdkconfig`；**不要把密钥写死进 `.cc`，也不要拷进 `docs/`、`main/` 等任何 tracked 文件**。密钥一旦编进固件就会出现在 `xiaozhi.bin` 里（可 dump），演示用途通常可接受 |

| 9 | **BUG-006 已降级为"告警 + 跳帧"，但"长时间不再 abort"尚未真机验证**；且改动落在 gitignored 的 `managed_components/` | **BUG-006** 末尾「✅ 已降级为…」段 | 下次验收判据：串口出现"读取触摸数据失败(…)，跳过这一帧"而**不是** abort、且复位分类里固件 abort 为 0。要长期保住得做"补丁文件 + 构建期应用"（**现在删掉组件目录就丢**） |
| 10 | **D7 量化测量（2026-09-20 晚进度）**：识别率**已测出 46%（阈值=30，37/80，含唤醒词 10 分钟仅 2 次）**；BUG-040 做了两轮 10 次试验；BUG-006 拿到部分验证（2 次 NACK 未 abort） | 验收记录 **§6 / §9** + **BUG-046** + `docs/handoff/2026-09-20-d7-fixes-flashed-handoff.md` | **还没做**：阈值=20 的召回复测 + 静默窗口误触发、长窗口 MQTT 成功率、断网补传剧本、手机侧 HTTP、`self.vehicle.net`、**2 h 无复位**。**头号阻塞仍是 BUG-044**（板子每 ~3 分钟 `rst:0x1` 复位一次，换口只是拉长周期） |
| 11 | **BUG-040**：两条假设（定时器竞争、PCA9557 锁存竞争）**均未成立**；静默已定位到 `PlaySound` **下游**，第 4 层埋点（`OutputData 首包`）已上机 | **BUG-040** 末尾两段（首次/第二次尝试） | 第二轮 10 次试验**全有声**且**每次都有 `OutputData 首包`**；第一轮**有几次没声音**但当时没有下游埋点。**存在"加日志改变时序把问题盖住"的可能（Heisenbug）**，别据此说"修好了" |
| 13 | **阈值已回退 20 并重测召回 + 补测误触发**（用户 2026-09-20 晚决定） | **BUG-046** + 验收记录 §6 | 改 `sdkconfig` 会**全量重编 ~10-12 min**；重测要按同协议（8×10）并把**静默窗口**的命中数也记下来（两边都有数才算闭环）——**已完成：46% → 72.5%，静默 100 s 误触发 0** |
| 14 | **BUG-047 自触发修法（播报保护闸）待验证** + 副作用待观察 | **BUG-047** 末尾「验证状态」与「下次验收判据」 | 需要一个**能引出自触发的长窗口**（上次是 10 分钟、连续多条命令、**说话偏轻、prob 压到 0.2x**）。判据：出现 `播报保护闸内丢弃` 且其后**没有**第二次播报 = 生效；出现两条播报却无丢弃日志 = 闸长不够（1500 → 调大）。**副作用**：闸内命令词一律丢弃（锁车约 2.8 s、状态播报约 6~8 s），播报期间说下一条会被丢掉 |
| 12 | **板子供电/链路不稳 → `rst:0x1 (POWERON)` 反复重启（D7 硬阻塞）** | **BUG-044**（含三段互相印证的证据链 + 可执行判据）+ BUG-043 | 第 1 步是用户动硬件（换线 / 主机直连 USB 口 / 独立 5V 供电）；之后**先不开端口只轮询 60 s** 看还闪不闪。**解决前不要烧录、不要做任何长测量** |

**另有两处"已知但不打算动"的**（别当成待办去修）：BUG-006（触摸 NACK → 整机 abort；用户 2026-09-17 决定不修、09-18 再次确认不必重提方案，**但 2026-09-20 晚为做 D7 的"2 小时无人值守"测量，用户已批准降级为"告警 + 跳帧"，见其条目末尾——改的是 gitignored 的 `managed_components/`**）；以及 `httpd` 在 3 路以上并发时打 `error in accept (23)`（socket 池上限，见验收记录 §8.4，服务与固件都不崩）。
