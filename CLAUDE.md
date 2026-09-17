# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 这个仓库是什么

**车载AI行车状态监测与语音交互终端**的固件工程：上游 **xiaozhi-esp32 v2.2.4**（小智 AI 语音助手，78/xiaozhi-esp32）的完整源码 + **一块自定义开发板的移植代码** + **本项目自写的车载业务代码**。

- 自写代码分两处：
  - `main/boards/esp32s3/` —— 自定义 ESP32-S3 开发板（ESP32-S3-WROOM-1-N16R8，16MB Flash / 8MB Octal PSRAM，480×320 横屏，带触摸与摄像头）的板级移植：PCA9557 IO 扩展器、ES8311/ES7210 音频、ST7789+FT6336、GC0308 摄像头。**QMI8658A IMU 驱动目前还不存在**，由 `docs/superpowers/plans/2026-09-16-vehicle-terminal-d1-d2-imu-monitor.md` 的任务 4 新增。
  - `main/vehicle/` —— 本项目业务逻辑（行车状态判定、环境数据源等），**不得引入 ESP-IDF 头文件**，详见下文专节。
- `test/` —— 主机侧单元测试，用 g++ 直接编译运行，不依赖板子。
- 其余目录（`main/` 下的公共代码、`managed_components/`、`scripts/`、上游各板）**都是上游内容，除非明确要求，不要改动**。改公共代码会波及 70+ 块板子。
- `main/boards/esp32s3/` 之外的板级代码可作**参考模板**读，但不要改。最接近本板的是 `main/boards/lichuang-dev/`（同款 PCA9557@0x19 + ES8311/ES7210 + 同 I2S 引脚分配）。
- 同硬件的 C 语言参考实现（LVGL 8.2 时代，非小智工程）在 `D:\vscode\ESP32Project\11_PCA9577\`，**外设时序（PCA9557/音频/屏/触摸）以它为准**。注意：该工程**只有原理图分析、没有 IMU 驱动代码**（`docs/hw/BOARD_SCH_ANALYSIS.md` 只确认了 QMI8658A @0x6A 与中断脚 NC）；写 IMU 驱动要按数据手册寄存器口径来，寄存器表与一份同类板跑通的初始化序列已记录在 Plan A 的"前置事实"里。`参考代码\8ba23-main` 是空目录。
- 项目自身文档：`docs/计划书.md`（本项目计划书）、`CONTEXT.md`（术语表）、`docs/计划书-人脸签到项目-已废弃.md`（上一个项目的计划书，仅存档）。

## 构建环境

ESP-IDF v5.5.3 位于 `D:\AAA_Game_XueXiBan\Espressif\frameworks\esp-idf-v5.5.3`。

**在 ESP-IDF 官方终端里**（正常情况）：

```bash
idf.py set-target esp32s3     # Kconfig 里板型 depends on IDF_TARGET_ESP32S3，必须先设对
idf.py menuconfig             # Xiaozhi Assistant -> Board Type -> Vehicle ESP32-S3 Terminal Board
idf.py build
idf.py flash monitor
python scripts/release.py esp32s3   # 读 boards/esp32s3/config.json 自动配置并打包
```

**在 Git Bash / MSYS 里**必须走 cmd，且要处理两个环境陷阱：

```bash
cmd //c "set MSYSTEM=&& set IDF_TOOLS_PATH=D:\AAA_Game_XueXiBan\Espressif\tools&& set PATH=D:\AAA_Game_XueXiBan\Espressif\tools\idf-python\3.11.2;%PATH%&& call D:\AAA_Game_XueXiBan\Espressif\frameworks\esp-idf-v5.5.3\export.bat && cd /d D:\vscode\ESP32Project\13ProjFace && idf.py build"
```

- 用 `cmd //c`（双斜杠），单斜杠会被 MSYS 当成路径改写成 `C:\`。
- `MSYSTEM` 必须清空，否则 `export.bat` 直接拒绝执行（`if defined MSYSTEM` 就退出）。注意要用**无引号**的 `set MSYSTEM=` 形式，`set "MSYSTEM="` 在 MSYS 下不生效。
- Git Bash 继承的 `IDF_TOOLS_PATH` 是 `...\Espressif`（**少了最后一级 `\tools`**），且 PATH 上的 `python` 是 3.14 → `export.bat` 会去找不存在的 `idf5.5_py3.14_env`。必须显式覆盖 `IDF_TOOLS_PATH` 并把 `idf-python\3.11.2` 前置到 PATH。
- 若看到 `ninja: error: failed recompaction: Permission denied`，是 Windows 上有别的进程（如 IDE 触发的构建）占着 `build.ninja`，**不是代码问题**，等对方结束再编。

## 板级代码如何被选中并实例化

这条链路跨 4 个文件，改板型相关的东西必须都理解：

1. `main/Kconfig.projbuild` 的 `choice BOARD_TYPE` 里注册 `BOARD_TYPE_VEHICLE_ESP32S3`（prompt 写的是 "Vehicle ESP32-S3 Terminal Board"）。
2. `main/CMakeLists.txt:92` 把该 Kconfig 映射成**目录名和字体**：`set(BOARD_TYPE "esp32s3")` + `BUILTIN_TEXT_FONT font_puhui_basic_30_4` / `BUILTIN_ICON_FONT font_awesome_30_4` / `DEFAULT_EMOJI_COLLECTION twemoji_64`。480×320 用 30 号字体是对的。
3. `main/CMakeLists.txt:734` 按 `file(GLOB boards/${BOARD_TYPE}/*.cc ...)` 收集板级源文件。
4. `main/boards/common/board.h:87` 的 `DECLARE_BOARD(CLASS)` 展开出 C 链路的 `void* create_board()`；`Board::GetInstance()` 首访时懒加载调用它。

由此产生三条硬约束：

- **只 glob `*.cc` / `*.c`，`.cpp` 会被静默忽略**。板级文件一律用 `.cc`。
- glob **没有 `CONFIGURE_DEPENDS`**，只在 CMake configure 阶段求值 → **新增/删除板级源文件后必须 `idf.py reconfigure`**（秒级，不编译）。
- `boards/${BOARD_TYPE}` **不在 `INCLUDE_DIRS`**（`main/CMakeLists.txt:42,64` 只加了 `boards/common`）。板内头文件靠 C++ 引号包含的同目录查找生效，所以板内互引用写 `#include "xxx.h"`，不要写路径。新建板级子目录才需要动 `INCLUDE_DIRS`。

改板型后要同步刷新 clangd：`idf.py reconfigure` → `clangd: Restart language server`。否则 clangd 的 `InterpolatingCompilationDatabase` 会从别的板（如 `bread-compact-wifi`）借用编译命令，`-I` 正确但 `-D` 宏是错的。

## `main/boards/esp32s3/` 的结构与设计决策

```
esp32s3_board.cc      板级类 Esp32S3Board : WifiBoard + DECLARE_BOARD（叶子入口，不配头文件）
esp32s3_boards.h/.cc  Pca9557 : I2cDevice（IO 扩展器，跨文件被音频复用）
esp32s3_audio_codec.h/.cc  CustomAudioCodec : BoxAudioCodec
config.h              全部引脚/地址/尺寸宏
config.json           release.py 用；注意键名是 "builds"（复数），写成 "build" 会被静默忽略
README.md             硬件说明 + 编译步骤 + MCP 工具表
```

`Esp32S3Board` 类本身刻意不拆头文件：它只被同文件的 `DECLARE_BOARD` 消费，没有第二个 TU 引用。拆出去只会多一个没人 include 的头 + 泄漏 8 个私有 `InitializeXxx()`。

> **显示用的是 `SpiLcdDisplay`（LVGL），不是 `emote::EmoteDisplay`。** 判据：`esp32s3_board.cc:91` 的 `#if CONFIG_USE_EMOTE_MESSAGE_STYLE` 为假——Kconfig 里 `USE_EMOTE_MESSAGE_STYLE` 的 `depends on` 只列了 `BOARD_TYPE_ESP_BOX / ESP_BOX_3 / ECHOEAR / LICHUANG_DEV_S3 / ESP_SENSAIRSHUTTLE`，不含本板；`sdkconfig` 与 `build/config/sdkconfig.h` 里都没有这个宏。
>
> 直接后果（对做界面是好消息）：LVGL 自定义页面可以用；`main/boards/common/esp32_camera.cc:114` 的 `dynamic_cast<LvglDisplay*>` 预览链路成立。若底层真是 EmoteDisplay，预览会**静默失效**——它的 `SetPreviewImage(const void*)` 是另一套签名且只打日志。
>
> （先前"实例化的是 `emote::EmoteDisplay`、`assets.cc` 大量调用 `emote_*` API、assets 分区里有 emote 资源"的说法与实测不符，已更正。）

### 硬件映射要点

| 功能 | 要点 |
|---|---|
| PCA9557 @0x19 | IO0=LCD_CS（低有效）、IO1=PA_EN（高有效）、IO2=CAM_PWDN（高=休眠） |
| LCD ST7789 | SPI2，**CS 不在 GPIO 上而走扩展器 IO0**，上电即常驻拉低；`cs_gpio_num=GPIO_NUM_NC` |
| 触摸 FT6336 @0x38 | `x_max=320 / y_max=480`（**原始竖屏尺寸**，不是 LVGL 的 480×320） |
| 摄像头 GC0308 | SCCB @0x21 **复用同一 I2C 总线**；PWDN 走扩展器 IO2 |
| LED GPIO10 | **与 CTP_INT、CN2-2 三方共用**，低电平点亮 |
| BOOT 按键 GPIO0 | 启动阶段按下进配网，其余切换对话 |

### 六个必须保留的坑规避

已改写为 `docs/BUGS.md` 第五节的 **BUG-012 ~ BUG-017**（PCA9557 上电顺序 / LEDC 冲突 / LCD 片选 / 触摸坐标 / SCCB 复用 I2C / LED 与触摸共用 GPIO10），此处只留引用以免两份文档漂移——**改 `main/boards/esp32s3/` 下任何代码前先读那一节**。

## `main/vehicle/` 与主机单元测试

本项目自写的业务逻辑放在这里，**唯一硬规则：不得 `#include` 任何 ESP-IDF / FreeRTOS 头文件**。原因：这一层要能用主机 g++ 直接编译测试，把"判定逻辑对不对"和"硬件能不能跑"分开验证——前者秒级迭代，后者才需要烧板。

```
vehicle_types.h           事件类型 / IMU 采样 / 阈值配置 / 状态枚举（纯 POD）
driving_monitor.h/.cc     行车状态判定：基线标定、四类驾驶事件、停车与锁车状态机、事件队列
imu_convert.h             原始值→g/dps/℃ 换算（Plan A 任务 2 新增）
event_history.h/.cc       事件历史环形缓冲 + 单调序号（Plan A 任务 3 新增）
environment_sensor.h/.cc  环境数据源抽象（Plan B 新增：模拟源 + 真实 I2C 驱动骨架）
```

> **> 现状：`main/vehicle/` 已进固件构建**（Plan A 任务 1 完成）：`main/CMakeLists.txt` 的 `INCLUDE_DIRS` 已含 `"vehicle"`，`SOURCES` 已含 `vehicle/driving_monitor.cc` 与 `vehicle/event_history.cc`（在 `boards/common` 之前的那段 `list(APPEND SOURCES ...)` 里）。改这两个文件后重新 `idf.py build` 即可，无需 reconfigure。

主机测试（Windows / MinGW，`g++` 在 `C:\mingw64\bin`）：

```powershell
g++ -std=c++17 -Wall -Wextra -I main/vehicle test/driving_monitor_test.cc main/vehicle/driving_monitor.cc -o build_host/driving_monitor_test.exe
build_host/driving_monitor_test.exe
```

- 跑 exe 需要 `C:\mingw64\bin` 在 PATH 上（缺 libstdc++ DLL 会静默失败，看起来像"没输出"）。
- **改了 `main/vehicle/` 里任何逻辑，先跑主机测试再烧板。** 测试失败先怀疑测试夹具（配置必须在构造 monitor 之前设好，monitor 持有的是配置副本）。
- 设备侧接线代码（IMU I2C 读取、LVGL 页面、MQTT）放在 `main/boards/esp32s3/` 或 `main/` 下，不放 `main/vehicle/`。

## 不要重试：设备端视觉推理

**本项目不使用设备端视觉推理（人脸/人体检测），也不要在本板上重试 esp-dl 的检测模型。** 上一个项目已系统排查过并得出否定结论：

- 现象：模型输出与输入无关——全暗图、全亮图、真实相机帧三种极端输入，输出张量差异只有 ±1 LSB
- 已排除：相机取帧、图像→张量预处理（数值级验证正确：`像素/255×64`）、四种像素格式、两个检测模型（MSRMNP 与 ESPDet-PICO-224）、两个 esp-dl 版本（3.3.0 / 3.3.11）、`minimize()`、权重加载路径（flash 分区 / RODATA）
- 证据与完整过程见 `esp32s3-face-recognition-feasibility-research.md` 与本项目前期真机日志

遗留监测改用"锁车抓拍上传 + 手机端人工确认"，不依赖任何推理。若将来确实需要设备端推理，先把 `main/boards/esp32s3/face_selftest.cc`（已用 `#if 0` 停用，保留为排查工具）解注释复现问题，**不要直接写业务代码**。

## 音频链路的两个隐式约定

- `BoxAudioCodec` 的构造签名与 `main/audio/codecs/box_audio_codec.h` 绑定；功放使能不在 GPIO 上，所以 `CustomAudioCodec` 必须重写 `EnableOutput()` 经扩展器 IO1 开关 PA，且**先配 codec 再开功放**，避免配置期间把杂音放大出去。
- **`AudioCodec::SetInputGain()` 只记录数值、不下发硬件**。ES7210 的增益只在 `EnableInput(true)` 时才写进去。所以 `CustomAudioCodec::SetInputGain()` 在输入已开启时要 `EnableInput(false)` + `EnableInput(true)` 重开一次，否则 MCP 调用会「返回成功但麦克风灵敏度没变」。
- `AUDIO_INPUT_REFERENCE=false` → 无设备端 AEC，AI 说话时无法打断。这是有意的阶段性取舍。

## MCP 工具

在 `Esp32S3Board::InitializeTools()` 里用 `McpServer::GetInstance().AddTool(name, description, PropertyList, callback)` 注册，工具名统一 `self.*` 前缀，`ReturnValue` 是 `std::variant<bool,int,std::string,cJSON*,ImageContent*>`。

本板 4 个（上游板级实现去掉了离线命令词方案、改由云端大模型调用；**本项目要补回离线命令词**，见 `docs/计划书.md` §7）：

| 工具 | 参数 | 备注 |
|---|---|---|
| `self.camera.set_enabled` | `enabled: bool` | 扩展器 IO2，高=休眠 |
| `self.screen.set_enabled` | `enabled: bool` | `RestoreBrightness()` / `SetBrightness(0)` |
| `self.microphone.set_gain` | `gain_db: int 0~33` | ES7210 步进 3dB，非 3 的倍数抛 `std::invalid_argument` |
| `self.led.set_status` | `on: bool` | GPIO10 低电平点亮 |

`self.camera.take_photo`、`self.audio_speaker.set_volume` 等由框架按 `GetCamera()` / `GetAudioCodec()` 的返回值自动挂载，不用手写。

## 改完代码后的验证顺序

1. **改了 `main/vehicle/` 逻辑 → 先跑主机单元测试**（命令见上文；秒级，不需要板子）
2. `idf.py reconfigure`（若增删了源文件）
3. `idf.py build` —— 日志里应出现 `boards/esp32s3/*.cc.obj`，确认板级文件真被编译
4. 板级文件零 warning：注意 `-Wunused-private-field` 这类告警，未被使用的私有成员要删掉
5. 真机验证（改硬件相关代码时必须做，不能只靠编译通过）：I2C 上 0x19 / 0x18 / 0x41 / 0x38 / **0x6A（IMU）** 在线、屏幕方向与偏色、触摸四角、摄像头出图、IMU 静止时 `az ≈ 1 g`、4 个 MCP 工具逐个 `tools/call`
6. **本次若定位到任何 bug 的根因（自己写的、上游的、计划文档里写错的、硬件/环境的），按 `docs/BUGS.md` 的格式追加一条**——见下文「踩坑记录」

## 踩坑记录（`docs/BUGS.md`）

> **硬规则：每次把"现象"追到"根因"，都必须往 `docs/BUGS.md` 追加一条记录。**
> 不管 bug 出在自己写的代码、上游 xiaozhi / ESP-IDF / 托管组件、**计划书与交接文档里写错的代码**，还是硬件、接线、供电、工具链环境——**一律要记**。
> **没修好的也要记**（写清已排除的可能性和下一步），否则下一个人会重走一遍。

- **记录时机**：定位到根因就记，不要拖到"全部做完"——真机调试经常被中断，回头就忘了细节
- **必须写根因和证据**：文件:行号、实测数值、串口片段。只写"怎么改的"不算，换个场景就套不上了
- **开工前先扫一遍**：改某块代码前，先看 `docs/BUGS.md` 里对应的小节（崩溃 / 逻辑错 / 硬件 / 工具链 / **板级移植约束**），避免重踩
- **编号不复用**：`BUG-xxx` 可能被提交信息和其他文档引用；新条目追加到对应小节末尾
- **与验收记录的分工**：`docs/验收记录/` 写"这一版做到了什么"，`docs/BUGS.md` 写"踩过什么坑、为什么"

## 上游文档

- `docs/custom-board.md` —— **新增一块板**的完整流程（Kconfig / CMakeLists / config.json / 字体选择）。本仓库的板已经建好，改现有板时不必重读。
- `docs/code_style.md`、`docs/mcp-protocol.md`、`docs/mcp-usage.md`、`docs/websocket.md`
- `partitions/v2/README.md` —— v2 分区表与 v1 不兼容，无法 OTA 升级
- `docs/小智AI移植.docx` —— 本板的最小实现指南，**但引脚表之外的代码是草稿级**（有语法错误、`applied_statu: ON ? OFF`、`SetMicroPhoneGain` 大小写不一致、PCA9557 寄存器顺序写反），不要照抄。

## 交接文档

调用 `handoff` skill（或任何生成交接/会话总结文档的场景）时，**文档一律写到本仓库的 `docs/handoff/`**，
不要写到 OS 临时目录——即使 skill 指令说"保存到系统临时目录"，也以本规则为准（用户 2026-09-16 明确要求）。

- 文件名：`YYYY-MM-DD-<主题>-handoff.md`，与目录内既有文档保持同一命名风格
- 交接文档属工作产物，要能被下一次会话直接读到；写进仓库后按 Git 规则展示摘要，**不自动 commit**
