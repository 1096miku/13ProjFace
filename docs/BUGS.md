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

### BUG-006 触摸 I2C 失败 → 上游 `ESP_ERROR_CHECK` 直接 abort（已缓解：硬件侧，证据待补）

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
- **处置（2026-09-17 后段，用户反馈）**：按上面的硬件方向处理后，**已能长时间稳定运行，未再复现 abort**。**具体做了哪几项（换线 / 主机直连 USB 口 / 独立 5V 供电）、连续运行多久、是否仍有零星 `i2c transaction failed` 日志，尚未记录**——证据补齐之前这条只算**暂时缓解**，不是根治。
  - **根因未变**：上游 `managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port_touch.c:127` 的 `ESP_ERROR_CHECK` 还在，代码里没有降级余地；触摸再次 NACK 仍会 abort 整机
  - **若再次复现**：仍按原待办往下查 FT6336 供电与 FPC 接触。**不建议**改 `managed_components`（重新解析依赖会被覆盖，且治标不治本）
  - **! 别忘了**：证据补上后要把本条的"暂时缓解"改成"已解决"，并写明持续时间与复现条件

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
