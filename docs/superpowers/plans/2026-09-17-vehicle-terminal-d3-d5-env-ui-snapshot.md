# 车载终端 D3–D5：环境抽象 + LVGL 四页 + 抓拍/HTTP 实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 把计划书 D3–D5 做完——环境数据抽象层（模拟源）、屏幕四个自定义页面、摄像头抓拍与预览、锁车监测联动抓拍、局域网 HTTP 看图、事件落盘与环形覆盖。

**架构：** 延续 Plan A 的分层：**纯逻辑进 `main/vehicle/`**（不得 include 任何 ESP-IDF 头，主机 g++ 可测），**设备侧接线进 `main/boards/esp32s3/`**。`imu_task` 只做「采样 → 判定 → 入历史 → 投队列」，新增一个低优先级 `worker_task` 负责写盘、抓拍、环境采样这类会阻塞的活；UI 通过一个受锁保护的 `VehicleStatus` 快照单向拉取数据，绝不直接读 `DrivingMonitor`。

**技术栈：** ESP-IDF v5.5.3、C++17、FreeRTOS、LVGL 9.4（`SpiLcdDisplay` 底座）、esp_lcd/esp32-camera、spiffs + `esp_http_server`、主机侧 g++（MinGW `C:\mingw64\bin`）跑纯逻辑测试。

**上游文档：** 设计文档 `docs/superpowers/specs/2026-09-16-vehicle-terminal-design.md`（口径以它为准）、计划书 `docs/计划书.md`、上一份 Plan A 计划 `docs/superpowers/plans/2026-09-16-vehicle-terminal-d1-d2-imu-monitor.md`。

---

## 代码风格与纪律（每个任务都适用）

1. **不自动 `git commit`**：每个任务的"提交"步骤实际是 `git add <文件>` → 展示 `git status --short` 与 `git diff --cached --stat` → **等用户确认后**再 `git commit`。commit message 用简洁中文。
2. **注释用中文**；Better Comments 标签（`// !` 警告 / `// >` 要点 / `// ?` 待确认 / `// todo`）**只能写在 `//` 行注释里**，不要写进 `/* */`（写了不着色）。
3. `main/vehicle/` 下**不得 include 任何 ESP-IDF / FreeRTOS 头**（`<cstdint>`/`<string>`/`<vector>`/`<cmath>` 可以）。设备侧代码不得把判定逻辑复制一份到板级目录。
4. 板级源文件一律 `.cc`（CMake 只 glob `*.cc`）。**新增/删除任何 `.cc` 或 `main/assets/common/*.ogg` 后必须 `idf.py reconfigure`**（glob 无 `CONFIGURE_DEPENDS`）。
5. **`main/vehicle/` 逻辑改完先跑主机测试再烧板**；跑 exe 需要 `C:\mingw64\bin` 在 PATH 上。
6. **日志里禁止 `%lld` / `%llu`**：本工程是 `CONFIG_NEWLIB_NANO_FORMAT=y`，nano 版 vfprintf 不支持 64 位整数格式，参数会错位并崩在 `memchr`（见 `docs/BUGS.md` BUG-001）。64 位数字要用十进制手工拼（本计划的任务 3 提供了 `vehicle::ToDecimal`）。
7. 板级代码**零 warning**；未被使用的私有成员要删掉（`-Wunused-private-field`）。
8. 每定位到一个根因，按 `docs/BUGS.md` 的格式追加一条（含 `文件:行号` 与实测数值）。

**命令备忘：**

```
# 主机测试（PowerShell，先 $env:PATH="C:\mingw64\bin;$env:PATH"）
g++ -std=c++17 -Wall -Wextra -I main/vehicle test/<name>.cc main/vehicle/<impl>.cc -o build_host/<name>.exe
build_host\<name>.exe

# 固件构建（pwsh 里用 cmd /c，不是 cmd //c）
cmd /c "set MSYSTEM=&& set IDF_TOOLS_PATH=D:\AAA_Game_XueXiBan\Espressif\tools&& set PATH=D:\AAA_Game_XueXiBan\Espressif\tools\idf-python\3.11.2;%PATH%&& call D:\AAA_Game_XueXiBan\Espressif\frameworks\esp-idf-v5.5.3\export.bat && cd /d D:\vscode\ESP32Project\13ProjFace && idf.py build > build\last_build.log 2>&1"
```

- `idf.py` 必须放宽沙箱权限（`danger-full-access`），否则报 `PermissionError: [WinError 5]`（BUG-008）。用户会批准，但**也会拒绝**——被拒时不要重试同一条命令。
- 串口在 COM10 / COM12 之间跳；抓取用 `build/capture_once.ps1`（只开一次端口、不重连，避免拉 RTS 复位把 I2C 钳死，见 BUG-005）。
- 崩溃地址解析：`xtensa-esp32s3-elf-addr2line -pfiaC -e build\xiaozhi.elf <addr>...`。

---

## 文件结构

| 文件 | 动作 | 职责 |
|---|---|---|
| `main/vehicle/vehicle_types.h` | 修改 | 新增 `VehicleStatus`（给 UI/上报的只读快照）与 `CaptureReason`（抓拍原因） |
| `main/vehicle/environment_sensor.{h,cc}` | 创建 | `EnvironmentSensor` 接口 + `SimulatedSensor` + 光照分档（纯逻辑） |
| `main/vehicle/snapshot_ring.{h,cc}` | 创建 | 抓拍环形的**索引**逻辑：下一个槽位、文件名、latest 指针（纯逻辑） |
| `main/vehicle/event_json.{h,cc}` | 创建 | 事件 → 一行 JSON（纯逻辑）；含 64 位十进制拼接 `ToDecimal` |
| `main/vehicle/event_text.{h,cc}` | 创建 | 事件 → 屏上一行文本（纯逻辑，事件页与 Plan C 播报共用） |
| `test/environment_sim_test.cc` | 创建 | 模拟源范围/缓变/可复现 + 光照分档边界 |
| `test/snapshot_ring_test.cc` | 创建 | 环形推进/回绕/文件名/latest 内容 |
| `test/event_json_test.cc` | 创建 | JSON 字段、`ToDecimal` 边界（含 INT64_MIN）、payload 长度 |
| `test/event_text_test.cc` | 创建 | 事件行文本与时间格式（`12.3s` / `3m14s`） |
| `main/boards/esp32s3/vehicle_service.{h,cc}` | 修改 | 状态快照、锁车请求、事件队列、`worker_task`（写盘/抓拍/环境采样）、`EventSink` 分发 |
| `main/boards/esp32s3/vehicle_ui.{h,cc}` | 创建 | 独立 screen + 四页（主页/实时画面/事件/设置）+ 主题字体跟随 |
| `main/boards/esp32s3/camera_capture.{h,cc}` | 创建 | 取帧（RGB565 字节序交换）、JPEG 抓拍、预览/抓拍互斥、抓拍落盘 |
| `main/boards/esp32s3/snapshot_store.{h,cc}` | 创建 | `snapshots` 分区挂载、JPEG 环形落盘、`events.log` 追加、退化内存模式 |
| `main/boards/esp32s3/vehicle_http.{h,cc}` | 创建 | `esp_http_server` 三路由 `/`、`/latest.jpg`、`/events` |
| `main/boards/esp32s3/esp32s3_board.cc` | 修改 | 构造并接线以上模块；`fb_count = 2`；新增 4 个 MCP 工具 |
| `main/CMakeLists.txt` | 修改 | 新增 `vehicle/*.cc` 到 `SOURCES`；`PRIV_REQUIRES` 加 `spiffs`、`esp_http_server` |
| `partitions/v2/16m.csv` | 修改 | **删掉两个人脸模型分区**，用同一段尾部 2.25 MB 建 `snapshots`（见任务 7） |
| `docs/验收记录/D3-D5-环境与界面与抓拍.md` | 创建 | D3–D5 验收记录（任务 12 写） |

**与本计划的既定偏离（复核时请重点看这几条，都是刻意取舍）：**

| # | 设计文档怎么写 | 本计划怎么做 | 理由 |
|---|---|---|---|
| 1 | §3.3 单列 `event_bus.{h,cc}` 事件队列模块 | **不新建**：复用已有 `EventHistory`（环形 + 单调序号）+ `xQueue`（设备侧）+ NVS 游标 | `EventHistory` 已提供环形与序号；再包一层纯逻辑只是重复。（`docs/BUGS.md` 的"编号不复用"同理，少一个模块少一处漂移） |
| 2 | §3.1 单列 `env_task`（1 s） | 折进 `worker_task`（每 5 次循环读一次 = 1 s） | 模拟源读一帧就是一次函数调用，单开任务只是多一份 3 KB 栈与一类时序 bug |
| 3 | §3.1 单列 `camera_task` | 抓拍由 `worker_task` 同步调用；预览由 LVGL 定时器驱动 | 抓拍与预览本来就要互斥（同一把锁），两个任务反而更难保证 |
| 4 | §5.3 事件 JSON 用 `"ts"`（unix 秒） | 用 `"ts_ms"`（开机以来的毫秒） | 设备没有 RTC，`Event.ts_ms` 是单调毫秒；unix 时间要在 Plan C 联网后再加，现在写 `ts` 是假数据 |
| 5 | §6.1 Kconfig `CONFIG_VEHICLE_ENV_SIMULATED` | **先不加** | 目前只有模拟源一个实现，加开关等于给唯一选项加配置项；真实驱动（SHT3x+BH1750）到位时再加开关与第二个实现 |
| 6 | §5.1 "现有分区偏移一律不动，追加 `snapshots` 到 `0xDC0000`" | **删掉 `human_face_det` + `human_face_feat` 两个分区**，把同一段 `0xDC0000+0x240000` 给 `snapshots` | 设计文档这条写于分区表还是旧版的时候；实测表已铺满 16 MB，`0xDC0000` 被那两个分区占着（详见任务 7 开头的依据）。删的两个分区从未烧写过，其余偏移一个不动 |
| 7 | §8 主页显示"今日事件计数" | 显示"**本次**事件数"（开机以来） | 设备无 RTC，`Event.ts_ms` 是开机以来的毫秒；写"今日"是假数据。墙上时间要等 Plan C 联网对时 |
| 8 | §9 "锁车抓拍跳过并记事件" | 抓拍失败只 `ESP_LOGW`，**不往事件历史里塞条目** | `EventType` 里没有"抓拍失败"这一项，硬塞会污染事件页与事件计数。降级事实写进验收记录的局限一节 |

---

## 执行记录

### D3（2026-09-17 晚）——已完成任务 1、3、6、4、5 + `camera_capture` 前置

执行时发现两处**计划自身的排序缺陷**，按下述方式处理，文档保持与代码一致：

1. **任务 3（`event_json`）提前到 D3 执行**：任务 6 的 `FormatEventLine()` 要用任务 3 的 `ToDecimal()`（nano printf 不支持 64 位格式，必须自己拼十进制），而本文档把任务 3 的交付划在 D5。**结论：任务 3 是任务 6 的硬前置**，两者应一起做；`main/CMakeLists.txt` 里 `vehicle/event_json.cc` 与 `vehicle/event_text.cc` 同时加入。
2. **`camera_capture.{h,cc}` 拆成两步**：`vehicle_ui.cc` 编译需要 `CameraCapture` 的完整类型（`CopyPreviewFrame()`），而本文档把这两个文件放在任务 8（D4）、且让它依赖任务 7 的 `SnapshotStore`（D5）。D3 版本先只做**取帧 + RGB565 字节序交换 + JPEG 编码**，不含 `SnapshotStore`；`OnCaptureRequest()` 的落盘钩子在 D4 补，`camera_capture.h` 里留了 `// todo` 标记。
3. **D3 不接线相机**：`esp32s3_board.cc` 里传 `nullptr`，实时画面页此时是黑屏（缓冲已清零），D4 再把 `CameraCapture` 传进去。
4. **任务 5 步骤 4 的两个访问器（`config()` / `history_capacity()`）与任务 11 的 `RefreshSettings()` 一并写入**：避免第二次改动同一文件。

---

### D4（2026-09-17 晚 ~ 2026-09-18 上午）——已完成任务 **2**、7、8、9

**交付物**：`snapshots` 分区（0xdc0000, 2304K）+ `SnapshotStore`（SPIFFS 挂载、抓拍环形落盘、`events.log` 追加）+ `CameraCapture` 落盘钩子 + 实时画面页接线与实测帧率。验收数据见 `docs/验收记录/D3-D5-环境与界面与抓拍.md` 的 D3/D4 两节。

执行时发现的问题与处置（**文档与代码保持一致**）：

1. **任务 2（`snapshot_ring`）必须提前到 D4**：D3 那轮没有做它（D3 执行记录里列的交付物只有任务 1/3/4/5/6），而任务 7 的 `SnapshotStore` 直接 `#include "snapshot_ring.h"`。所以 D4 先补任务 2（纯逻辑 + 主机测试 19 项全绿）+ `main/CMakeLists.txt` 的 `vehicle/snapshot_ring.cc`。
2. **任务 9 的"非待机态暂停预览"被删掉**（本计划最需要复核的一处偏离）。原计划理由（"小智拍照/说话时也在用相机，重叠会互相偷帧"）在真机上被证伪：
   - 现象：小智一唤醒（非待机态）画面就卡在最后一帧，用户直接反馈为 bug；
   - 机制：预览是同一颗 GC0308 的**第二个消费者**，停止消费帧之后，上游 `Esp32Camera::Capture()` 又长期攥着 `current_fb_` 不放，驱动凑不出 `CAMERA_GRAB_WHEN_EMPTY` 要求的"全部缓冲都空"→ 进 IDLE → 实测**永久停摆**（`cam_hal: Failed to get frame: timeout` 每 4.11 s 一条、不再恢复）；
   - 代价：对话时多占一点 SPI/CPU 带宽（实测 fps 从 14.8 掉到 11.8 后自行恢复）。
   - 完整证据见 `docs/BUGS.md` BUG-025。
3. **相机参数保持本计划原样（`fb_count = 2`、`grab_mode` 不动）**：执行中一度按驱动头文件注释改成 `CAMERA_GRAB_LATEST`（预览掉到 9.8 fps + 约 355 s 永久停摆），又试过 `fb_count=3 + LATEST`（开机即 `EV-EOF-OVF` / `FB-SIZE: 138240 != 153600`，小智拍照上传挂死）。**两个方向都被真机否掉，退回原配置**：13.1–15.0 fps 且抓拍/小智拍照都正常。三版固件的日志都留在 `build/` 里。
4. **`CameraCapture` 需要补一个 `OnEvent` 空实现**（`EventSink::OnEvent` 是纯虚；计划的类声明里没写，`new CameraCapture(...)` 直接编不过）——见 BUG-023。
5. **worker 任务的栈从 PSRAM 改到内部 RAM**：worker 要写 SPIFFS，而 `spi_flash_disable_interrupts_caches_and_other_cpu()` 的断言要求当前任务栈在内部 DRAM（BUG-024：真机表现为"只要落盘就复位"，复现 13 次）。`imu_task` 仍用 PSRAM。**这动了本文档「架构」一节"两个任务的栈都从 PSRAM 出"的前提**，同时内部 RAM 会少约 8 KB。
6. **新增两条诊断日志（计划里没有，但验收需要）**：`CameraCapture` 取帧失败的节流日志（每 60 次一条）与抓拍完成时的 `worker 栈余量`（`uxTaskGetStackHighWaterMark`）。没有前者无法区分"相机没帧"和"别的原因"，没有后者无法判断 8 KB 栈该不该缩（实测最坏只用约 2.1 KB）。
7. **任务 7 的首次挂载没有触发格式化**：把 `snapshots` 分区整段擦掉再启动，SPIFFS 仍直接挂载成功（92 ms），`format_if_mount_failed` 这条路径在本板没被走到——验收记录里如实写了，别当成"已验证格式化"。

---

### 任务 1：环境数据抽象层（模拟源）

**文件：**
- 创建：`main/vehicle/environment_sensor.h`
- 创建：`main/vehicle/environment_sensor.cc`
- 测试：`test/environment_sim_test.cc`
- 修改：`main/CMakeLists.txt:45-48`（把 `.cc` 加进 `SOURCES`）

- [ ] **步骤 1：编写失败的测试**

创建 `test/environment_sim_test.cc`：

```cpp
// 环境数据源（模拟）的主机单元测试
//
// 编译与运行：
//   g++ -std=c++17 -Wall -Wextra -I main/vehicle test/environment_sim_test.cc main/vehicle/environment_sensor.cc -o build_host/environment_sim_test.exe
//   build_host/environment_sim_test.exe

#include <cstdio>

#include "environment_sensor.h"

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

static bool Near(float a, float b, float eps = 1e-4f) {
    const float d = a - b;
    return (d < 0 ? -d : d) <= eps;
}

int main() {
    printf("environment_sim\n");

    // 光照分档边界
    CHECK(BucketLight(0) == LightLevel::kDark, "0 lux → 很暗");
    CHECK(BucketLight(9) == LightLevel::kDark, "9 lux → 很暗");
    CHECK(BucketLight(10) == LightLevel::kDim, "10 lux → 偏暗");
    CHECK(BucketLight(49) == LightLevel::kDim, "49 lux → 偏暗");
    CHECK(BucketLight(50) == LightLevel::kMedium, "50 lux → 适中");
    CHECK(BucketLight(299) == LightLevel::kMedium, "299 lux → 适中");
    CHECK(BucketLight(300) == LightLevel::kBright, "300 lux → 明亮");
    CHECK(BucketLight(999) == LightLevel::kBright, "999 lux → 明亮");
    CHECK(BucketLight(1000) == LightLevel::kStrong, "1000 lux → 很强");
    CHECK(BucketLight(100000) == LightLevel::kStrong, "100000 lux → 很强");
    CHECK(ToString(LightLevel::kDark) != nullptr, "ToString 不返回空指针");

    SimulatedSensor sensor;
    CHECK(sensor.name() != nullptr, "模拟源有名字");

    // t=0 时正弦值为 0 → 读数等于基线
    EnvReading r;
    CHECK(sensor.Read(0, r), "读取成功");
    CHECK(r.valid && r.simulated, "标记为有效且来自模拟源");
    CHECK(r.ts_ms == 0, "ts_ms 原样回填");
    CHECK(Near(r.temp_c, 26.0f), "t=0 温度等于基线 26.0");
    CHECK(Near(r.humidity_pct, 48.0f), "t=0 湿度等于基线 48.0");
    CHECK(r.lux == 320, "t=0 光照等于基线 320");

    // 同一时刻必须可复现（真机 UI 每秒读一次，不能自己跳）
    EnvReading a;
    EnvReading b;
    sensor.Read(123456, a);
    sensor.Read(123456, b);
    CHECK(Near(a.temp_c, b.temp_c) && Near(a.humidity_pct, b.humidity_pct) && a.lux == b.lux,
          "同一 now_ms 两次读数一致");

    // 扫一遍一个完整周期，读数必须始终落在基线 ± 幅度内
    bool in_range = true;
    bool changed = false;
    EnvReading prev;
    sensor.Read(0, prev);
    for (int64_t t = 0; t <= 600000; t += 1000) {
        EnvReading cur;
        if (!sensor.Read(t, cur)) {
            in_range = false;
            break;
        }
        if (cur.temp_c < 26.0f - 1.5f || cur.temp_c > 26.0f + 1.5f) in_range = false;
        if (cur.humidity_pct < 48.0f - 4.0f || cur.humidity_pct > 48.0f + 4.0f) in_range = false;
        if (cur.lux < 320 - 180 || cur.lux > 320 + 180) in_range = false;
        if (cur.temp_c != prev.temp_c || cur.lux != prev.lux) changed = true;
        prev = cur;
    }
    CHECK(in_range, "一个周期内所有读数都在基线 ± 幅度内");
    CHECK(changed, "读数确实在缓变（不是常量）");

    // 自定义配置生效
    SimulatedSensor::Config cfg;
    cfg.temp_base_c = 30.0f;
    cfg.humidity_base_pct = 60.0f;
    cfg.lux_base = 100;
    cfg.temp_amplitude_c = 0.0f;
    cfg.humidity_amplitude_pct = 0.0f;
    cfg.lux_amplitude = 0;
    SimulatedSensor fixed(cfg);
    EnvReading f;
    fixed.Read(987654, f);
    CHECK(Near(f.temp_c, 30.0f) && Near(f.humidity_pct, 60.0f) && f.lux == 100,
          "幅度为 0 时读数为常量基线");

    if (g_failures == 0) {
        printf("all passed\n");
        return 0;
    }
    printf("%d failure(s)\n", g_failures);
    return 1;
}
```

- [ ] **步骤 2：运行测试验证失败**

运行：
```powershell
$env:PATH="C:\mingw64\bin;$env:PATH"
g++ -std=c++17 -Wall -Wextra -I main/vehicle test/environment_sim_test.cc main/vehicle/environment_sensor.cc -o build_host/environment_sim_test.exe
```
预期：FAIL，`fatal error: environment_sensor.h: No such file or directory`

- [ ] **步骤 3：编写最少实现代码**

创建 `main/vehicle/environment_sensor.h`：

```cpp
#pragma once

// 环境数据源抽象：温湿度 / 光照。
//
// 本板上没有温湿度与光照传感器（计划书 §2.2：I2C 地址已被 0x19/0x18/0x41/0x6A/0x38/0x21 占满），
// 所以先用模拟源把「界面显示 / 周期上报 / 语音查询」三条链路真实跑通；
// 采购到位后只新增一个 EnvironmentSensor 实现（SHT3x @0x44/0x45 + BH1750 @0x23），上层不改。
//
// 纯逻辑，不依赖 ESP-IDF，可用主机 g++ 单元测试。

#include <cstdint>

namespace vehicle {

struct EnvReading {
    int64_t ts_ms = 0;          // 采样时刻（开机以来的毫秒）
    float temp_c = 0.0f;        // 摄氏度
    float humidity_pct = 0.0f;  // 相对湿度 %
    int32_t lux = 0;            // 光照强度
    bool valid = false;         // false = 本帧不可用，界面应显示 "--"
    bool simulated = true;      // true = 数据来自模拟源（界面必须标"模拟"）
};

// 光照档位：播报不读数字、只播档位（设计文档 §6.4），具体数值上屏
enum class LightLevel : uint8_t {
    kDark = 0,  // 很暗  < 10 lux
    kDim,       // 偏暗  < 50
    kMedium,    // 适中  < 300
    kBright,    // 明亮  < 1000
    kStrong,    // 很强  >= 1000
};

LightLevel BucketLight(int32_t lux);
const char *ToString(LightLevel level);

class EnvironmentSensor {
public:
    virtual ~EnvironmentSensor() = default;

    // 读一帧。返回 false 表示本次读取失败（out 不可信，调用方应保持上一帧）。
    virtual bool Read(int64_t now_ms, EnvReading &out) = 0;
    // 数据源标识（"sim" / "sht3x"），用于界面标注与日志
    virtual const char *name() const = 0;
};

// 模拟源：围绕可配基线做有界缓变。
// ! 只依赖 now_ms，不做内部状态机——同一个 now_ms 必然得到同一个读数，
// ! 主机测试可以断言具体数值，真机上也不会因为读取频率变化而跳变。
class SimulatedSensor : public EnvironmentSensor {
public:
    struct Config {
        float temp_base_c = 26.0f;             // 基线温度（℃）
        float humidity_base_pct = 48.0f;       // 基线湿度（%）
        int32_t lux_base = 320;                // 基线光照（lux）
        float temp_amplitude_c = 1.5f;         // 温度缓变幅度（±）
        float humidity_amplitude_pct = 4.0f;   // 湿度缓变幅度（±）
        int32_t lux_amplitude = 180;           // 光照缓变幅度（±）
        int32_t temp_period_ms = 300000;       // 5 min 走一个正弦周期
        int32_t humidity_period_ms = 420000;   // 7 min
        int32_t lux_period_ms = 600000;        // 10 min
    };

    SimulatedSensor() = default;
    explicit SimulatedSensor(const Config &cfg) : cfg_(cfg) {}

    bool Read(int64_t now_ms, EnvReading &out) override;
    const char *name() const override { return "sim"; }

private:
    Config cfg_{};
};

}  // namespace vehicle
```

创建 `main/vehicle/environment_sensor.cc`：

```cpp
#include "environment_sensor.h"

#include <cmath>

namespace vehicle {

namespace {
constexpr double kTwoPi = 6.283185307179586;
}  // namespace

LightLevel BucketLight(int32_t lux) {
    if (lux < 10) {
        return LightLevel::kDark;
    }
    if (lux < 50) {
        return LightLevel::kDim;
    }
    if (lux < 300) {
        return LightLevel::kMedium;
    }
    if (lux < 1000) {
        return LightLevel::kBright;
    }
    return LightLevel::kStrong;
}

const char *ToString(LightLevel level) {
    switch (level) {
        case LightLevel::kDark: return "很暗";
        case LightLevel::kDim: return "偏暗";
        case LightLevel::kMedium: return "适中";
        case LightLevel::kBright: return "明亮";
        case LightLevel::kStrong: return "很强";
    }
    return "未知";
}

bool SimulatedSensor::Read(int64_t now_ms, EnvReading &out) {
    // > 整数取模 + sin 生成有界缓变：周期可配、结果可复现、无需内部状态。
    const auto wave = [now_ms](int32_t period_ms) -> double {
        if (period_ms <= 0) {
            return 0.0;
        }
        const int64_t phase = now_ms % period_ms;
        return std::sin(kTwoPi * static_cast<double>(phase) / static_cast<double>(period_ms));
    };

    out.ts_ms = now_ms;
    out.valid = true;
    out.simulated = true;
    out.temp_c = cfg_.temp_base_c + cfg_.temp_amplitude_c * static_cast<float>(wave(cfg_.temp_period_ms));
    out.humidity_pct =
        cfg_.humidity_base_pct + cfg_.humidity_amplitude_pct * static_cast<float>(wave(cfg_.humidity_period_ms));
    out.lux = cfg_.lux_base + static_cast<int32_t>(static_cast<double>(cfg_.lux_amplitude) * wave(cfg_.lux_period_ms));
    return true;
}

}  // namespace vehicle
```

- [ ] **步骤 4：运行测试验证通过**

运行：
```powershell
$env:PATH="C:\mingw64\bin;$env:PATH"
g++ -std=c++17 -Wall -Wextra -I main/vehicle test/environment_sim_test.cc main/vehicle/environment_sensor.cc -o build_host/environment_sim_test.exe
build_host\environment_sim_test.exe
```
预期：`all passed`，且编译**零 warning**（`-Wall -Wextra`）。

- [ ] **步骤 5：把新 `.cc` 接进固件构建**

修改 `main/CMakeLists.txt:45-48`，改成：

```cmake
# 本项目自写的车载业务逻辑（纯 C++，不依赖 ESP-IDF，可用主机 g++ 单测）
list(APPEND SOURCES
    "vehicle/driving_monitor.cc"
    "vehicle/event_history.cc"
    "vehicle/environment_sensor.cc"
)
```

运行 `idf.py reconfigure`，预期：无报错、秒级返回。

- [ ] **步骤 6：展示并提交（等用户确认）**

```bash
git add main/vehicle/environment_sensor.h main/vehicle/environment_sensor.cc test/environment_sim_test.cc main/CMakeLists.txt
git status --short
git diff --cached --stat
```
建议 commit message：`feat: 新增环境数据源抽象与模拟源`

---

### 任务 2：抓拍环形的索引逻辑

**文件：**
- 创建：`main/vehicle/snapshot_ring.h`
- 创建：`main/vehicle/snapshot_ring.cc`
- 测试：`test/snapshot_ring_test.cc`
- 修改：`main/CMakeLists.txt:45-49`

- [ ] **步骤 1：编写失败的测试**

创建 `test/snapshot_ring_test.cc`：

```cpp
// 抓拍环形索引的主机单元测试
//
// 编译与运行：
//   g++ -std=c++17 -Wall -Wextra -I main/vehicle test/snapshot_ring_test.cc main/vehicle/snapshot_ring.cc -o build_host/snapshot_ring_test.exe
//   build_host/snapshot_ring_test.exe

#include <cstdio>
#include <string>

#include "snapshot_ring.h"

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
    printf("snapshot_ring\n");

    SnapshotRing ring(3);
    CHECK(ring.capacity() == 3, "容量为 3");
    CHECK(ring.written() == 0, "初始未写过");
    CHECK(ring.latest_slot() == -1, "初始没有最新槽位");
    CHECK(ring.LatestIndexContent().empty(), "未写过时 latest.idx 内容为空");

    CHECK(ring.NextSlot() == 0, "第 1 张写槽位 0");
    CHECK(ring.NextSlot() == 1, "第 2 张写槽位 1");
    CHECK(ring.NextSlot() == 2, "第 3 张写槽位 2");
    CHECK(ring.latest_slot() == 2, "最新槽位为 2");
    CHECK(ring.NextSlot() == 0, "第 4 张回绕到槽位 0");
    CHECK(ring.latest_slot() == 0, "回绕后最新槽位为 0");
    CHECK(ring.written() == 4, "写入计数累计");

    CHECK(ring.FileNameFor(0) == "snap_000.jpg", "槽位 0 → snap_000.jpg");
    CHECK(ring.FileNameFor(7) == "snap_007.jpg", "槽位 7 → snap_007.jpg");
    CHECK(ring.FileNameFor(31) == "snap_031.jpg", "槽位 31 → snap_031.jpg");
    CHECK(ring.FileNameFor(3).empty(), "越界槽位返回空串");
    CHECK(ring.FileNameFor(-1).empty(), "负槽位返回空串");

    CHECK(ring.LatestIndexContent() == "0\n", "latest.idx 内容是槽位号 + 换行");

    // 容量非法时按 1 处理（与 EventHistory 同风格：构造时一次性定死，之后不分配）
    SnapshotRing one(0);
    CHECK(one.capacity() == 1, "容量 0 被夹到 1");
    CHECK(one.NextSlot() == 0 && one.NextSlot() == 0, "容量 1 时永远写同一个槽位");

    if (g_failures == 0) {
        printf("all passed\n");
        return 0;
    }
    printf("%d failure(s)\n", g_failures);
    return 1;
}
```

- [ ] **步骤 2：运行测试验证失败**

运行：
```powershell
$env:PATH="C:\mingw64\bin;$env:PATH"
g++ -std=c++17 -Wall -Wextra -I main/vehicle test/snapshot_ring_test.cc main/vehicle/snapshot_ring.cc -o build_host/snapshot_ring_test.exe
```
预期：FAIL，`fatal error: snapshot_ring.h: No such file or directory`

- [ ] **步骤 3：编写最少实现代码**

创建 `main/vehicle/snapshot_ring.h`：

```cpp
#pragma once

// 抓拍环形存储的**索引**逻辑：只回答"下一张写哪个槽位、最新一张是谁、文件名是什么"，
// 不碰文件系统（挂载与读写是设备侧的 SnapshotStore，见 main/boards/esp32s3/snapshot_store.cc）。
// 纯逻辑，不依赖 ESP-IDF。

#include <cstdint>
#include <string>

namespace vehicle {

class SnapshotRing {
public:
    explicit SnapshotRing(int capacity);

    // 取下一个写入槽位（Capacity() 个槽位循环使用），并把"最新槽位"推进到它
    int NextSlot();

    // 最近一次 NextSlot 得到的槽位；从未写入过返回 -1
    int latest_slot() const { return latest_slot_; }
    int capacity() const { return capacity_; }
    int written() const { return written_; }

    // 槽位 → 文件名（"snap_007.jpg"）；越界返回空串
    std::string FileNameFor(int slot) const;

    // latest.idx 的内容（"7\n"）；从未写入过返回空串（调用方据此跳过写文件）
    std::string LatestIndexContent() const;

private:
    int capacity_ = 1;
    int next_ = 0;
    int latest_slot_ = -1;
    int written_ = 0;
};

}  // namespace vehicle
```

创建 `main/vehicle/snapshot_ring.cc`：

```cpp
#include "snapshot_ring.h"

#include <cstdio>

namespace vehicle {

SnapshotRing::SnapshotRing(int capacity) : capacity_(capacity > 0 ? capacity : 1) {
}

int SnapshotRing::NextSlot() {
    const int slot = next_;
    next_ = (next_ + 1) % capacity_;
    latest_slot_ = slot;
    written_++;
    return slot;
}

std::string SnapshotRing::FileNameFor(int slot) const {
    if (slot < 0 || slot >= capacity_) {
        return std::string();
    }
    char name[16];
    // > 文件名固定 3 位序号（snap_000.jpg … snap_031.jpg），spiffs 里按名排序即可按时间读。
    snprintf(name, sizeof(name), "snap_%03d.jpg", slot);
    return std::string(name);
}

std::string SnapshotRing::LatestIndexContent() const {
    if (latest_slot_ < 0) {
        return std::string();
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%d\n", latest_slot_);
    return std::string(buf);
}

}  // namespace vehicle
```

- [ ] **步骤 4：运行测试验证通过**

运行：
```powershell
$env:PATH="C:\mingw64\bin;$env:PATH"
g++ -std=c++17 -Wall -Wextra -I main/vehicle test/snapshot_ring_test.cc main/vehicle/snapshot_ring.cc -o build_host/snapshot_ring_test.exe
build_host\snapshot_ring_test.exe
```
预期：`all passed`，零 warning。

- [ ] **步骤 5：接进固件构建**

把 `main/CMakeLists.txt` 的 `list(APPEND SOURCES ...)` 改成：

```cmake
# 本项目自写的车载业务逻辑（纯 C++，不依赖 ESP-IDF，可用主机 g++ 单测）
list(APPEND SOURCES
    "vehicle/driving_monitor.cc"
    "vehicle/event_history.cc"
    "vehicle/environment_sensor.cc"
    "vehicle/snapshot_ring.cc"
)
```

运行 `idf.py reconfigure`。

- [ ] **步骤 6：展示并提交（等用户确认）**

```bash
git add main/vehicle/snapshot_ring.h main/vehicle/snapshot_ring.cc test/snapshot_ring_test.cc main/CMakeLists.txt
git status --short
git diff --cached --stat
```
建议 commit message：`feat: 新增抓拍环形索引逻辑与主机测试`

---

### 任务 3：事件 JSON 序列化（含 64 位十进制拼接）

**文件：**
- 创建：`main/vehicle/event_json.h`
- 创建：`main/vehicle/event_json.cc`
- 测试：`test/event_json_test.cc`
- 修改：`main/CMakeLists.txt:45-50`

- [ ] **步骤 1：编写失败的测试**

创建 `test/event_json_test.cc`：

```cpp
// 事件 JSON 序列化的主机单元测试
//
// 编译与运行：
//   g++ -std=c++17 -Wall -Wextra -I main/vehicle test/event_json_test.cc main/vehicle/event_json.cc -o build_host/event_json_test.exe
//   build_host/event_json_test.exe

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

static EventRecord Make(int64_t seq, EventType type, int64_t ts_ms, float value) {
    EventRecord r;
    r.seq = seq;
    r.event.type = type;
    r.event.ts_ms = ts_ms;
    r.event.value = value;
    return r;
}

int main() {
    printf("event_json\n");

    // 十进制拼接：必须自己实现，不能用 %lld / std::to_string
    // （本工程 CONFIG_NEWLIB_NANO_FORMAT=y，nano vfprintf 不支持 64 位格式；见 BUG-001）
    CHECK(ToDecimal(0) == "0", "0 → \"0\"");
    CHECK(ToDecimal(7) == "7", "7 → \"7\"");
    CHECK(ToDecimal(-1) == "-1", "-1 → \"-1\"");
    CHECK(ToDecimal(1758000000LL) == "1758000000", "10 位正数");
    CHECK(ToDecimal(-9223372036854775807LL - 1) == "-9223372036854775808",
          "INT64_MIN 不溢出（取负会溢出，实现必须按无符号处理）");
    CHECK(ToDecimal(9223372036854775807LL) == "9223372036854775807", "INT64_MAX");

    // JSON 字段与顺序固定，便于人读与 diff
    const std::string js = EventToJson(Make(12, EventType::kHardBrake, 1758000000123LL, -0.52f));
    CHECK(js == "{\"seq\":12,\"type\":\"hard_brake\",\"ts_ms\":1758000000123,\"value\":-0.52}",
          "急刹车事件 JSON 正确");
    printf("       payload = %s\n", js.c_str());

    CHECK(EventTypeId(EventType::kHardAccel) == std::string("hard_accel"), "急加速 id");
    CHECK(EventTypeId(EventType::kHardBrake) == std::string("hard_brake"), "急刹车 id");
    CHECK(EventTypeId(EventType::kHardTurn) == std::string("hard_turn"), "急转弯 id");
    CHECK(EventTypeId(EventType::kBump) == std::string("bump"), "颠簸 id");
    CHECK(EventTypeId(EventType::kCrash) == std::string("crash"), "碰撞 id");
    CHECK(EventTypeId(EventType::kParked) == std::string("parked"), "停车 id");
    CHECK(EventTypeId(EventType::kMoving) == std::string("driving"), "行驶 id");
    CHECK(EventTypeId(EventType::kMotionWhileParked) == std::string("motion_while_parked"), "异常震动 id");
    CHECK(EventTypeId(EventType::kCount) == std::string("unknown"), "非法枚举有兜底值");

    // 一行一条，不含换行（写文件时由调用方补 \n）
    CHECK(js.find('\n') == std::string::npos, "payload 里没有换行");

    // 免费版 MQTT 单条 payload 要控制在 256 B 内（设计文档 §7.1）
    const std::string longest = EventToJson(Make(9223372036854775807LL, EventType::kMotionWhileParked,
                                                 9223372036854775807LL, -19.99f));
    CHECK(longest.size() < 256, "最长 payload 仍小于 256 B");
    printf("       longest = %d B\n", static_cast<int>(longest.size()));

    if (g_failures == 0) {
        printf("all passed\n");
        return 0;
    }
    printf("%d failure(s)\n", g_failures);
    return 1;
}
```

- [ ] **步骤 2：运行测试验证失败**

运行：
```powershell
$env:PATH="C:\mingw64\bin;$env:PATH"
g++ -std=c++17 -Wall -Wextra -I main/vehicle test/event_json_test.cc main/vehicle/event_json.cc -o build_host/event_json_test.exe
```
预期：FAIL，`fatal error: event_json.h: No such file or directory`

- [ ] **步骤 3：编写最少实现代码**

创建 `main/vehicle/event_json.h`：

```cpp
#pragma once

// 事件 → JSON：`/snap/events.log` 每行一条，也是 Plan C 巴法云 MQTT 的 payload（同构，别写两份）。
// 纯逻辑，不依赖 ESP-IDF。

#include <cstdint>
#include <string>

#include "event_history.h"
#include "vehicle_types.h"

namespace vehicle {

// 事件类型 → 稳定 ASCII 标识（英文小写下划线，供手机端/云端解析，不要用中文）
std::string EventTypeId(EventType type);

// int64 → 十进制字符串。
// ! 必须自己拼：本工程 CONFIG_NEWLIB_NANO_FORMAT=y，nano 版 vfprintf 不支持 64 位整数格式，
// ! %lld 只消费 4 字节会让后续参数全部错位；std::to_string 内部同样走 vfprintf，也不能用。
std::string ToDecimal(int64_t value);

// 一行 JSON（不含换行），字段顺序固定：
//   {"seq":12,"type":"hard_brake","ts_ms":1758000000123,"value":-0.52}
// 注意 ts_ms 是"开机以来的毫秒"（设备无 RTC）。unix 时间由 Plan C 的上报层补。
std::string EventToJson(const EventRecord &record);

}  // namespace vehicle
```

创建 `main/vehicle/event_json.cc`：

```cpp
#include "event_json.h"

#include <cstdio>

namespace vehicle {

std::string EventTypeId(EventType type) {
    switch (type) {
        case EventType::kHardAccel: return "hard_accel";
        case EventType::kHardBrake: return "hard_brake";
        case EventType::kHardTurn: return "hard_turn";
        case EventType::kBump: return "bump";
        case EventType::kCrash: return "crash";
        case EventType::kParked: return "parked";
        case EventType::kMoving: return "driving";
        case EventType::kMotionWhileParked: return "motion_while_parked";
        case EventType::kCount: break;
    }
    return "unknown";
}

std::string ToDecimal(int64_t value) {
    if (value == 0) {
        return "0";
    }
    // > 先取无符号幅值：对 INT64_MIN 直接取负会溢出（-INT64_MIN 仍是 INT64_MIN），
    // > 用 uint64_t 承接才安全。
    const bool negative = value < 0;
    uint64_t magnitude = negative ? (~static_cast<uint64_t>(value) + 1ULL) : static_cast<uint64_t>(value);

    char digits[24];
    int n = 0;
    while (magnitude > 0 && n < static_cast<int>(sizeof(digits))) {
        digits[n++] = static_cast<char>('0' + static_cast<int>(magnitude % 10ULL));
        magnitude /= 10ULL;
    }

    std::string out;
    out.reserve(static_cast<size_t>(n) + 1);
    if (negative) {
        out.push_back('-');
    }
    while (n > 0) {
        out.push_back(digits[--n]);
    }
    return out;
}

std::string EventToJson(const EventRecord &record) {
    char value_buf[16];
    snprintf(value_buf, sizeof(value_buf), "%.2f", static_cast<double>(record.event.value));

    std::string out;
    out.reserve(96);
    out += "{\"seq\":";
    out += ToDecimal(record.seq);
    out += ",\"type\":\"";
    out += EventTypeId(record.event.type);
    out += "\",\"ts_ms\":";
    out += ToDecimal(record.event.ts_ms);
    out += ",\"value\":";
    out += value_buf;
    out += "}";
    return out;
}

}  // namespace vehicle
```

- [ ] **步骤 4：运行测试验证通过**

运行：
```powershell
$env:PATH="C:\mingw64\bin;$env:PATH"
g++ -std=c++17 -Wall -Wextra -I main/vehicle test/event_json_test.cc main/vehicle/event_json.cc -o build_host/event_json_test.exe
build_host\event_json_test.exe
```
预期：`all passed`（含 `INT64_MIN` 用例），零 warning。

- [ ] **步骤 5：接进固件构建**

`main/CMakeLists.txt` 的 `list(APPEND SOURCES ...)` 追加 `"vehicle/event_json.cc"`，运行 `idf.py reconfigure`。

- [ ] **步骤 6：展示并提交（等用户确认）**

```bash
git add main/vehicle/event_json.h main/vehicle/event_json.cc test/event_json_test.cc main/CMakeLists.txt
git status --short
git diff --cached --stat
```
建议 commit message：`feat: 新增事件 JSON 序列化与 64 位十进制拼接`

---

### 任务 4：状态快照、锁车请求、事件队列与 worker 任务

**为什么要做这一层：** 上游 `VehicleService` 现在只有 `imu_task` 一个任务，`monitor_` 与 `history_` 都只在它里面被读写。UI（LVGL 任务）和后续的写盘/上报都要读这些数据，直接读就是数据竞争；而写盘与抓拍会阻塞，绝不能在 `imu_task` 里做。所以加一层：**受锁保护的只读快照 + 事件队列 + 一个低优先级 worker**。

**文件：**
- 修改：`main/vehicle/vehicle_types.h`（新增 `VehicleStatus`、`CaptureReason`）
- 修改：`main/boards/esp32s3/vehicle_service.h`
- 修改：`main/boards/esp32s3/vehicle_service.cc`

- [ ] **步骤 1：在 `vehicle_types.h` 里加两个纯 POD 类型**

在 `main/vehicle/vehicle_types.h` 的 `MotionState` 定义**之后**、`ToString(MotionState)` 声明之前插入：

```cpp
// 抓拍原因（给 worker 决定"要不要抓、怎么命名/记日志"用；不是判定事件，所以不塞进 EventType）
enum class CaptureReason : uint8_t {
    kCrash = 0,          // 判定到碰撞
    kMotionWhileParked,  // 停车/锁车期间的异常震动
    kLockEntered,        // 刚进入锁车监测模式
    kManual,             // 屏幕按钮 / 语音"重新抓拍"
};

const char *ToString(CaptureReason reason);
```

在 `vehicle_types.h` 末尾（`ToString(MotionState)` 声明之后、`}  // namespace vehicle` 之前）插入：

```cpp
// 对外只读快照：UI / 上报层只看这一个结构，不要直接碰 DrivingMonitor。
// 由 imu_task 每帧更新一次，读取方加锁整体拷走，保证"状态 + 采样值 + 计数"互相一致
// （分开读会出现"状态已经是停车、采样值还是上一帧行驶"这类撕裂）。
struct VehicleStatus {
    MotionState state = MotionState::kUncalibrated;
    bool calibrated = false;
    ImuSample sample{};   // 最近一帧有效采样（ts_ms 是它自己的时间戳）
    int32_t events_total = 0;
    int64_t last_event_seq = 0;
};
```

`ToString(CaptureReason)` 的实现放在已有 `ToString(MotionState)` 所在的那个 `.cc` 里（用 `grep -n "ToString(MotionState)" main/vehicle/*.cc` 找到，Plan A 把它放在 `driving_monitor.cc`）：

```cpp
const char *ToString(CaptureReason reason) {
    switch (reason) {
        case CaptureReason::kCrash: return "碰撞";
        case CaptureReason::kMotionWhileParked: return "异常震动";
        case CaptureReason::kLockEntered: return "进入锁车监测";
        case CaptureReason::kManual: return "手动";
    }
    return "未知";
}
```

- [ ] **步骤 2：改 `vehicle_service.h`**

把 `main/boards/esp32s3/vehicle_service.h` 整体替换为：

```cpp
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "driving_monitor.h"
#include "environment_sensor.h"
#include "event_history.h"
#include "qmi8658a.h"
#include "vehicle_types.h"

// 事件消费者：worker_task 里调用，**可以慢**（写盘、发网络），绝不会阻塞采样。
// 落盘（SnapshotStore）与上报（Plan C 的 BemfaClient）各实现一个。
class EventSink {
public:
    virtual ~EventSink() = default;
    virtual void OnEvent(const vehicle::EventRecord &record) = 0;
    // 默认空实现：只有需要抓拍的消费者（相机）才关心
    virtual void OnCaptureRequest(vehicle::CaptureReason reason, int64_t ts_ms) {
        (void)reason;
        (void)ts_ms;
    }
};

// 车载业务服务：IMU 采样任务 + 行车判定 + 事件历史 + 慢活 worker。
//
// ! 分工（设计文档 D3 + 本次扩展）：
// !   imu_task ：读 IMU → Feed 判定 → 入历史 → 投事件队列。只做纯计算与内存操作，
// !              绝不写盘/抓拍/上报/碰 UI —— 它一卡，采样就丢，事件就漏。
// !   worker_task：排空事件队列 → 分发给 EventSink；1 Hz 读环境传感器；
// !              收到抓拍请求（碰撞 / 锁车期震动 / 进入锁车 / 手动）时发 OnCaptureRequest。
class VehicleService {
public:
    explicit VehicleService(i2c_master_bus_handle_t i2c_bus);

    // 注册事件消费者（最多 4 个）。必须在 Start() 之前调用。
    void AddEventSink(EventSink *sink);

    // 初始化 IMU 并启动 imu_task + worker_task；IMU 不在线时返回 false（调用方降级，不阻断开机）
    bool Start();

    // 只读快照（UI / MCP 工具用）。加锁整体拷走，不要试图拿引用。
    vehicle::VehicleStatus Status() const;

    // 最近环境读数（worker_task 每秒更新一次）
    vehicle::EnvReading env() const;

    // 事件历史的一份拷贝（屏幕事件页用；index 0 = 最旧）
    std::vector<vehicle::EventRecord> CopyHistory() const;

    // 手动锁车 / 解锁（屏幕按钮、MCP 工具、Plan C 的语音"锁车"）。
    // ! 只是记一个请求，真正的状态切换发生在 imu_task 里，避免跨任务写 DrivingMonitor。
    void RequestLock(bool locked);

    // 手动抓拍（屏幕按钮 / 语音"重新抓拍"），由 worker_task 执行
    void RequestCapture();

private:
    static void ImuTaskEntry(void *arg);
    static void WorkerTaskEntry(void *arg);
    void ImuTaskLoop();
    void WorkerTaskLoop();
    void PublishStatus(const vehicle::ImuSample &sample);
    void DispatchEvent(const vehicle::EventRecord &record);
    void LogEvent(const vehicle::EventRecord &record);

    static constexpr int kSamplePeriodMs = 20;      // 50 Hz
    // ! kTaskStackBytes 是"字节"，不是 FreeRTOS 的 ulStackDepth（那是 StackType_t 字数）；
    // ! 传给 xTaskCreateStaticPinnedToCore 时必须除以 sizeof(StackType_t)，见 Start()。
    static constexpr int kTaskStackBytes = 4096;
    static constexpr int kWorkerStackBytes = 8192;  // 写盘 + 环境读取，8 KB
    static constexpr int kMaxErrorStreak = 50;      // 连续 50 次（≈1 s）I2C 失败则重新初始化
    static constexpr int kHistoryCapacity = 64;
    static constexpr int kEventQueueSize = 32;      // 与 DrivingMonitor 内部队列同量级
    static constexpr int kMaxSinks = 4;
    static constexpr int kEnvPeriodLoops = 5;       // worker 每 200 ms 一轮 → 5 轮 = 1 s
    static constexpr uint32_t kWorkerPeriodMs = 200;

    Qmi8658a imu_;
    vehicle::MonitorConfig config_;
    vehicle::DrivingMonitor monitor_;
    vehicle::EventHistory history_;
    vehicle::SimulatedSensor env_sensor_;   // ! 目前只有模拟源；接上真实传感器后换成 I2cEnvSensor

    mutable std::mutex status_mutex_;       // 保护 status_
    mutable std::mutex history_mutex_;      // 保护 history_ 与 env_
    vehicle::VehicleStatus status_{};
    vehicle::EnvReading env_{};

    // 跨任务请求：用 tri-state 原子量，-1 = 无请求，0 = 解锁，1 = 锁车
    std::atomic<int> lock_request_{-1};
    // 抓拍请求：-1 = 无请求；否则是 CaptureReason 的值（原因由请求方决定，worker 不猜）
    std::atomic<int> capture_request_{-1};

    EventSink *sinks_[kMaxSinks] = {};
    int sink_count_ = 0;

    QueueHandle_t event_queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    TaskHandle_t worker_task_ = nullptr;
    bool calibration_logged_ = false;
};
```

- [ ] **步骤 3：改 `vehicle_service.cc`**

把 `main/boards/esp32s3/vehicle_service.cc` 整体替换为：

```cpp
#include "vehicle_service.h"

#include <cmath>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "VehicleService"

namespace {

// > 与 VehicleService::Start() 里同一套从 PSRAM 出栈的写法：内部 RAM 只留给控制块。
// > ulStackDepth 的单位是 StackType_t 字数（S3 上 4 字节），必须除以 sizeof(StackType_t)。
bool CreatePsramTask(TaskFunction_t entry, const char *name, int stack_bytes, void *arg, UBaseType_t priority,
                     BaseType_t core, TaskHandle_t *out) {
    StackType_t *stack = static_cast<StackType_t *>(heap_caps_malloc(stack_bytes, MALLOC_CAP_SPIRAM));
    StaticTask_t *tcb = static_cast<StaticTask_t *>(heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL));
    if (stack == nullptr || tcb == nullptr) {
        ESP_LOGE(TAG, "任务 %s 的栈/TCB 分配失败（PSRAM 余量不足？）", name);
        heap_caps_free(stack);
        heap_caps_free(tcb);
        return false;
    }
    *out = xTaskCreateStaticPinnedToCore(entry, name, static_cast<uint32_t>(stack_bytes) / sizeof(StackType_t), arg,
                                         priority, stack, tcb, core);
    return *out != nullptr;
}

}  // namespace

VehicleService::VehicleService(i2c_master_bus_handle_t i2c_bus)
    : imu_(i2c_bus), config_(), monitor_(config_), history_(kHistoryCapacity) {
}

void VehicleService::AddEventSink(EventSink *sink) {
    if (sink == nullptr || sink_count_ >= kMaxSinks) {
        return;
    }
    sinks_[sink_count_++] = sink;
}

bool VehicleService::Start() {
    if (task_ != nullptr) {
        return true;
    }
    if (!imu_.Init()) {
        ESP_LOGE(TAG, "IMU 初始化失败，行车监测不启动（其余功能照常）");
        return false;
    }

    event_queue_ = xQueueCreate(kEventQueueSize, sizeof(vehicle::EventRecord));
    if (event_queue_ == nullptr) {
        ESP_LOGE(TAG, "事件队列创建失败");
        return false;
    }

    if (!CreatePsramTask(WorkerTaskEntry, "vehicle_worker", kWorkerStackBytes, this, 3, 1, &worker_task_)) {
        return false;
    }
    // ! 采样任务优先级必须高于 worker（5 > 3）：worker 一卡（写盘/JPEG 编码），
    // ! 采样也不能被推迟，否则 50 Hz 采样会出现成片丢帧。
    if (!CreatePsramTask(ImuTaskEntry, "imu_task", kTaskStackBytes, this, 5, 0, &task_)) {
        return false;
    }

    ESP_LOGI(TAG, "imu_task 已启动：%d ms 周期（%d Hz）；worker 周期 %u ms", kSamplePeriodMs, 1000 / kSamplePeriodMs,
             static_cast<unsigned>(kWorkerPeriodMs));
    return true;
}

void VehicleService::ImuTaskEntry(void *arg) {
    static_cast<VehicleService *>(arg)->ImuTaskLoop();
}

void VehicleService::WorkerTaskEntry(void *arg) {
    static_cast<VehicleService *>(arg)->WorkerTaskLoop();
}

void VehicleService::ImuTaskLoop() {
    const TickType_t period = pdMS_TO_TICKS(kSamplePeriodMs);
    TickType_t last_wake = xTaskGetTickCount();
    int error_streak = 0;
    vehicle::ImuSample sample;
    vehicle::MotionState last_state = vehicle::MotionState::kUncalibrated;

    while (true) {
        // > 先吃掉外部请求再采样：锁车请求来自 UI/MCP，不能直接写 monitor_（跨任务写状态机会撕裂）。
        const int lock_req = lock_request_.exchange(-1);
        if (lock_req >= 0) {
            monitor_.RequestLock(lock_req == 1);
        }

        const Qmi8658a::ReadResult result = imu_.ReadSample(sample);
        if (result == Qmi8658a::ReadResult::kOk) {
            error_streak = 0;
            monitor_.Feed(sample);
            PublishStatus(sample);

            if (!calibration_logged_ && monitor_.calibrated()) {
                calibration_logged_ = true;
                const float bx = monitor_.baseline_ax();
                const float by = monitor_.baseline_ay();
                const float bz = monitor_.baseline_az();
                // > |a| 静止时应 ≈ 1.000 g；明显偏小说明标定那一秒里板子在动，基线不可信，
                // > 之后所有阈值判定都会失准（连"静止"都判不出来，永远到不了 kParked）。
                ESP_LOGI(TAG, "静止基线标定完成：ax=%.3f ay=%.3f az=%.3f g（|a|=%.3f g）", bx, by, bz,
                         sqrtf(bx * bx + by * by + bz * bz));
            }

            vehicle::Event event;
            while (monitor_.PopEvent(event)) {
                vehicle::EventRecord record;
                record.event = event;
                {
                    std::lock_guard<std::mutex> lock(history_mutex_);
                    record.seq = history_.Append(event);
                }
                LogEvent(record);
                // ! 队列满就丢最旧的还是丢这条？这里选择"丢这条并计数"：
                // ! 采样任务不允许阻塞，worker 排空速度（200 ms 一次 × 32 条）远高于事件产生速率。
                if (xQueueSend(event_queue_, &record, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "事件队列已满，丢弃事件 #%d", static_cast<int>(record.seq));
                }
            }

            // 进入锁车监测那一刻请求抓拍（设计文档 §6：进入时抓拍 1 张）
            const vehicle::MotionState state = monitor_.state();
            if (state == vehicle::MotionState::kLockedMonitor && last_state != vehicle::MotionState::kLockedMonitor) {
                capture_request_.store(static_cast<int>(vehicle::CaptureReason::kLockEntered));
            }
            last_state = state;
        } else if (result == Qmi8658a::ReadResult::kError) {
            if (++error_streak >= kMaxErrorStreak) {
                ESP_LOGW(TAG, "连续 %d 次读取失败，重新初始化 IMU", error_streak);
                error_streak = 0;
                if (!imu_.Init()) {
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
            }
        }

        vTaskDelayUntil(&last_wake, period);
    }
}

void VehicleService::WorkerTaskLoop() {
    const TickType_t period = pdMS_TO_TICKS(kWorkerPeriodMs);
    TickType_t last_wake = xTaskGetTickCount();
    int loop = 0;
    vehicle::EventRecord record;

    while (true) {
        // 1) 排空事件队列（一轮最多 32 条，队列容量就是 32）
        while (xQueueReceive(event_queue_, &record, 0) == pdTRUE) {
            DispatchEvent(record);
        }

        // 2) 抓拍请求（碰撞 / 锁车期震动 / 进入锁车 / 手动）
        const int capture_reason = capture_request_.exchange(-1);
        if (capture_reason >= 0) {
            const auto reason = static_cast<vehicle::CaptureReason>(capture_reason);
            // > 只借 status_ 拿最新采样时间戳；抓拍原因由请求方决定，不在这里按状态猜。
            int64_t ts_ms = 0;
            {
                std::lock_guard<std::mutex> lock(status_mutex_);
                ts_ms = status_.sample.ts_ms;
            }
            for (int i = 0; i < sink_count_; i++) {
                sinks_[i]->OnCaptureRequest(reason, ts_ms);
            }
        }

        // 3) 1 Hz 读环境传感器
        if (++loop >= kEnvPeriodLoops) {
            loop = 0;
            vehicle::EnvReading reading;
            if (env_sensor_.Read(esp_timer_get_time() / 1000, reading)) {
                std::lock_guard<std::mutex> lock(history_mutex_);
                env_ = reading;
            }
        }

        vTaskDelayUntil(&last_wake, period);
    }
}

void VehicleService::PublishStatus(const vehicle::ImuSample &sample) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.state = monitor_.state();
    status_.calibrated = monitor_.calibrated();
    status_.sample = sample;
    status_.events_total = monitor_.TotalEventCount();
    status_.last_event_seq = history_.last_seq();
}

void VehicleService::DispatchEvent(const vehicle::EventRecord &record) {
    // > 只有碰撞与锁车期异常震动需要抓拍（设计文档 §4/§6）；其余事件仅落盘/上报。
    if (record.event.type == vehicle::EventType::kCrash) {
        capture_request_.store(static_cast<int>(vehicle::CaptureReason::kCrash));
    } else if (record.event.type == vehicle::EventType::kMotionWhileParked) {
        capture_request_.store(static_cast<int>(vehicle::CaptureReason::kMotionWhileParked));
    }
    for (int i = 0; i < sink_count_; i++) {
        sinks_[i]->OnEvent(record);
    }
}

void VehicleService::LogEvent(const vehicle::EventRecord &record) {
    // ! 不要用 %lld：本工程是 CONFIG_NEWLIB_NANO_FORMAT=y，nano 版 vfprintf 不支持 64 位整数格式，
    // ! %lld 只消费 4 字节，后面的可变参数全部错位——%s 会读到 NULL，
    // ! 在 printf 内部的 memchr 上踩空指针崩溃（LoadProhibited）。这里只用 32 位整数与浮点格式。
    ESP_LOGI(TAG, "事件 #%d %s value=%.2f ts=%.3f s", static_cast<int>(record.seq),
             vehicle::ToString(record.event.type), record.event.value,
             static_cast<double>(record.event.ts_ms) / 1000.0);
}

vehicle::VehicleStatus VehicleService::Status() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_;
}

vehicle::EnvReading VehicleService::env() const {
    std::lock_guard<std::mutex> lock(history_mutex_);
    return env_;
}

std::vector<vehicle::EventRecord> VehicleService::CopyHistory() const {
    std::lock_guard<std::mutex> lock(history_mutex_);
    std::vector<vehicle::EventRecord> out;
    out.reserve(static_cast<size_t>(history_.size()));
    for (int i = 0; i < history_.size(); i++) {
        const vehicle::EventRecord *record = history_.At(i);
        if (record != nullptr) {
            out.push_back(*record);
        }
    }
    return out;
}

void VehicleService::RequestLock(bool locked) {
    lock_request_.store(locked ? 1 : 0);
}

void VehicleService::RequestCapture() {
    capture_request_.store(static_cast<int>(vehicle::CaptureReason::kManual));
}
```

- [ ] **步骤 4：删掉被取代的旧接口**

删掉原来的 `latest_sample()` 与 `sample_mutex_`、`latest_` 成员：它们被 `Status()` 取代，且**目前没有任何调用方**（`grep -rn "latest_sample" main/` 只有 `vehicle_service.h/.cc` 自己），留着就是死代码。

改完后 `grep -n "latest_sample\|sample_mutex_" main/boards/esp32s3/vehicle_service.*` 应无输出。

- [ ] **步骤 5：构建验证**

运行（注意 `idf.py` 需要放宽沙箱权限）：
```
idf.py build > build\last_build.log 2>&1
```
预期：`Project build complete.`；`Select-String -Path build\last_build.log -Pattern "vehicle_service|warning|error"` 里**没有** `vehicle_service` 相关的 warning/error。

- [ ] **步骤 6：真机验证（这一步只验证"没破坏已有功能"）**

烧录并抓串口（用 `build/capture_once.ps1`，只开一次端口）：

预期看到（与 Plan A 相同，外加一条新日志）：
```
I (xxx) VehicleService: 静止基线标定完成：ax=... az=... g（|a|≈1.000 g）
I (xxx) VehicleService: imu_task 已启动：20 ms 周期（50 Hz）；worker 周期 200 ms
I (xxx) VehicleService: 事件 #1 急加速 value=0.38 ts=... s
```
静止放置 30 s 以上，确认**没有**崩溃、没有 `事件队列已满`、`kParked` 仍会出现。

- [ ] **步骤 7：展示并提交（等用户确认）**

```bash
git add main/vehicle/vehicle_types.h main/boards/esp32s3/vehicle_service.h main/boards/esp32s3/vehicle_service.cc
git status --short
git diff --cached --stat
```
建议 commit message：`refactor: VehicleService 增加状态快照、事件队列与 worker 任务`

---

### 任务 5：`VehicleUi` 骨架 + 主页（D3 验收项之一）

> **前置依赖（**按文档顺序执行时注意**）：** 本任务的 `vehicle_ui.cc` 直接调用 `vehicle::FormatEventLine()`，那是**任务 6** 的产物。所以执行顺序是 **先任务 6（`event_text`），再本任务**；或者按文档顺序做到本任务时，先跳到任务 6 把 `main/vehicle/event_text.{h,cc}` + `test/event_text_test.cc` 建好，再回来做本任务。两个任务之间没有别的耦合，换来换去不会互相污染。

**四个已核实的事实，决定了整个任务怎么写（别再猜，也别"顺手改回去"）：**

1. **中文字体是个陷阱，这是本任务最容易做错的地方。**
   - 内置的 `font_puhui_basic_30_4` **只有 206 个汉字**（取字来源见 `managed_components/78__xiaozhi-fonts/generate_fonts.ipynb:246-256`，cmap 在 `font_puhui_basic_30_4.c:28019-28049`）。**"车 / 速 / 事 / 件 / 记 / 置 / 画 / 实 / 返 / 回"等本页要用字大多不在里面。**
   - 运行时真正生效的是 assets 分区里的 `font_puhui_common_30_4`（≈18408 字符，2,499,568 B）。它由 `main/assets.cc:242-256` 用 `LvglCBinFont` 替换主题字体，**触发点在 `main/application.cc:393` 的 `assets.Apply()`**——晚于 `display->SetupUI()`（`:67`），而且**还要等 assets 分区有效**（`application.cc:351-354`，无效时静默降级）。
   - `CONFIG_LV_USE_FONT_PLACEHOLDER` **未开**（`sdkconfig:3045`），也没有任何 `lv_font_set_fallback`（全仓 0 处）→ 缺字在 LVGL 9.4 里 `box_w = adv_w = 0`（`managed_components/lvgl__lvgl/src/font/lv_font.c:135-141`）：**既不画方框也不占宽度，字直接消失**，页面看起来像"少字"而不是报错。
   - **因此规则是：`vehicle_ui` 里一个字体都不许硬编码，全部用"主题当前字体"，并且每秒重新贴一次**，这样 `assets.Apply()` 换了 common 字体后 1 秒内自动跟上。做法：把字体设在 **screen 对象**上，让子控件继承（LVGL 的文字属性从父级继承），**不要**给每个 label 单独 `lv_obj_set_style_text_font(..., &BUILTIN_TEXT_FONT, 0)`。
2. 聊天界面建在**默认 screen**（`lv_screen_active()`，`main/display/lcd_display.cc:818`），所有聊天控件都挂在它上面。我们自建 screen 用 `lv_screen_load()` 切换——切走后聊天界面的对象**不销毁、API 也不崩**，只是不可见（含 `preview_image_`，它在 `lcd_display.cc:850` 建在默认 screen 上）。
3. `Display::IsSetupUICalled()`（`main/display/display.h:49`）就是"聊天界面已建好"的现成标志（`SetupUI()` 里置位，`display.h:43-45`）。板级构造函数跑在 `Application::Initialize()` 的 `Board::GetInstance()`（`main/application.cc:62`）里，**早于** `display->SetupUI()`（`:67`）——所以 UI 必须**延迟到 `IsSetupUICalled()` 为真之后**再建。
4. **入口按钮不要放 `lv_layer_top()`**：本工程 0 处使用该 API（`lv_layer_top` 全仓 grep 无命中），而且 top layer 不吃 screen 的字体/样式继承（浮层文字得自己再设字体）。改用**默认 screen 上的子对象 + `lv_obj_move_foreground()`**，这样它和聊天控件同屏共存、也能继承主题字体。

> **? 两处无先例的 API，先在真机确认再往下做：** `lv_screen_load()` 与"自建 screen"在本仓库都是 0 先例（`lv_screen_load|lv_scr_act` 全仓 grep 无命中）。若真机上切页出现白屏、花屏或触摸失效，**回退方案**是上游已经验证过的共存套路（`main/boards/jiuchuan-s3/jiuchuan_dev_board.cc:46-59`：继承 `SpiLcdDisplay` → override `SetupUI()` 先调父类 → `DisplayLockGuard lock(this)` 再改）——即不建新 screen，把四页做成默认 screen 上的容器，切页改为显示/隐藏容器并把 `top_bar_/status_bar_/bottom_bar_/emoji_box_` 一起隐藏。回退前先把现象记进 `docs/BUGS.md`。

**文件：**
- 创建：`main/boards/esp32s3/vehicle_ui.h`
- 创建：`main/boards/esp32s3/vehicle_ui.cc`
- 修改：`main/boards/esp32s3/esp32s3_board.cc`（构造 `VehicleUi` 并 `Start()`）

- [ ] **步骤 1：写 `vehicle_ui.h`**

```cpp
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include <lvgl.h>

#include "vehicle_service.h"

class Display;
class CameraCapture;

// 自定义车载页面：主页 / 实时画面 / 事件记录 / 设置。
//
// ! 四个必须记住的约束：
// !   1. 所有 LVGL 调用只能发生在 LVGL 任务里——要么是本类的 lv_timer 回调，
// !      要么是触摸事件回调（同样由 LVGL 任务派发）。跨任务请求（MCP 工具、Plan C 的语音）
// !      只写下面几个 atomic 成员，由定时器读取后执行。
// !   2. 四个页面各是一个独立 screen；进自定义页之前先记住当时的 lv_screen_active()（聊天界面），
// !      "返回"时切回去。
// !   3. 界面延迟到 display->IsSetupUICalled() 为真之后再建（板级构造函数跑在 SetupUI() 之前）。
// !   4. ! 字体只用"主题当前字体"，并且每秒重新贴一次——绝不要硬编码 &BUILTIN_TEXT_FONT：
// !      内置 font_puhui_basic_30_4 只有 206 个汉字（"车/速/事/件/记/置/画"都不在里面），
// !      运行时真正可用的 font_puhui_common_30_4 要等 assets.Apply() 才装进主题；
// !      而本工程没开 LV_USE_FONT_PLACEHOLDER，缺字是"直接消失"而不是方框，很难查。
class VehicleUi {
public:
    enum class Page : uint8_t { kHome = 0, kPreview, kEvents, kSettings, kCount };

    VehicleUi(Display* display, VehicleService* vehicle, CameraCapture* camera);
    ~VehicleUi();

    // 只建定时器；真正的界面在第一次 tick 且 IsSetupUICalled() 为真时创建
    void Start();

    // 切到某个自定义页（任意任务可调）；"chat" 表示切回聊天界面
    void RequestPage(Page page);
    void RequestChatScreen();

private:
    static void TimerEntry(lv_timer_t* timer);
    static void PreviewTimerEntry(lv_timer_t* timer);

    void Tick();
    void TickPreview();
    void ApplyPendingNavigation();
    void ApplyThemeAppearance();
    void BuildOnce();
    void BuildHome(lv_obj_t* root);
    void BuildPreview(lv_obj_t* root);
    void BuildEvents(lv_obj_t* root);
    void BuildSettings(lv_obj_t* root);
    void RefreshHome();
    void RefreshEvents();
    void RefreshSettings();
    void RefreshAxes();

    bool OnOurPage() const;

    Display* display_ = nullptr;
    VehicleService* vehicle_ = nullptr;
    CameraCapture* camera_ = nullptr;

    lv_timer_t* timer_ = nullptr;
    lv_timer_t* preview_timer_ = nullptr;
    bool built_ = false;
    bool axes_visible_ = false;
    int events_page_ = 0;

    lv_obj_t* chat_screen_ = nullptr;
    lv_obj_t* pages_[static_cast<int>(Page::kCount)] = {};
    lv_obj_t* entry_button_ = nullptr;
    const lv_font_t* applied_font_ = nullptr;   // 已贴过的主题字体，避免每秒重复 set 样式

    lv_obj_t* home_temp_ = nullptr;
    lv_obj_t* home_humid_ = nullptr;
    lv_obj_t* home_lux_ = nullptr;
    lv_obj_t* home_state_ = nullptr;
    lv_obj_t* home_events_ = nullptr;
    lv_obj_t* home_axes_ = nullptr;
    lv_obj_t* home_net_ = nullptr;

    lv_obj_t* preview_canvas_ = nullptr;
    uint8_t* preview_buf_ = nullptr;
    size_t preview_capacity_ = 0;
    bool preview_active_ = false;
    int preview_frames_ = 0;
    int preview_misses_ = 0;
    int64_t preview_window_start_ms_ = 0;

    lv_obj_t* event_rows_[5] = {};
    lv_obj_t* events_page_label_ = nullptr;

    lv_obj_t* settings_lock_btn_label_ = nullptr;
    lv_obj_t* settings_axes_switch_ = nullptr;
    lv_obj_t* settings_axes_label_ = nullptr;
    lv_obj_t* settings_env_label_ = nullptr;

    std::atomic<int> pending_page_{-1};   // -1 = 无请求
    std::atomic<bool> pending_chat_{false};
};
```

- [ ] **步骤 2：写 `vehicle_ui.cc`**

```cpp
#include "vehicle_ui.h"

#include <cstdio>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "board.h"
#include "camera_capture.h"
#include "display.h"
#include "environment_sensor.h"
#include "event_text.h"
#include "lvgl_font.h"
#include "lvgl_theme.h"

#define TAG "VehicleUi"

// > 刻意不 include BUILTIN_TEXT_FONT、也不 LV_FONT_DECLARE 任何内置字体：
// > 内置 font_puhui_basic_30_4 只有 206 个汉字，用它写"车辆状态/事件记录"这类标签会缺字，
// > 而本工程没开 LV_USE_FONT_PLACEHOLDER —— 缺字不画方框、宽度为 0，字直接消失。
// > 字体统一由 ApplyThemeFont() 从主题取（assets.Apply() 之后主题里才是 common 字体）。

namespace {

constexpr int kRefreshMs = 1000;
constexpr int kPreviewIntervalMs = 60;    // ≈16 fps 目标：指标要求 ≥10 fps，留出丢帧余量
constexpr int kEventRows = 5;
constexpr int kPreviewW = 320;
constexpr int kPreviewH = 240;
constexpr uint32_t kTextColor = 0xE8E8E8;
constexpr uint32_t kDimColor = 0x9AA0A6;
constexpr uint32_t kBgColor = 0x14161A;

lv_obj_t* MakeLabel(lv_obj_t* parent, int x, int y, uint32_t color) {
    lv_obj_t* label = lv_label_create(parent);
    // > 不设字体：文字属性从父级（screen）继承，字体由 ApplyThemeFont() 统一贴。
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_pos(label, x, y);
    lv_label_set_text(label, "");
    return label;
}

lv_obj_t* MakeButton(lv_obj_t* parent, int x, int y, int w, int h, const char* text, lv_event_cb_t cb,
                     void* user_data) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_t* label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    return btn;
}

const char* StateText(vehicle::MotionState state) {
    return vehicle::ToString(state);
}

}  // namespace

VehicleUi::VehicleUi(Display* display, VehicleService* vehicle, CameraCapture* camera)
    : display_(display), vehicle_(vehicle), camera_(camera) {
}

VehicleUi::~VehicleUi() {
    if (preview_buf_ != nullptr) {
        heap_caps_free(preview_buf_);
        preview_buf_ = nullptr;
    }
}

bool VehicleUi::OnOurPage() const {
    lv_obj_t* active = lv_screen_active();
    for (int i = 0; i < static_cast<int>(Page::kCount); i++) {
        if (pages_[i] != nullptr && pages_[i] == active) {
            return true;
        }
    }
    return false;
}

void VehicleUi::Start() {
    if (timer_ != nullptr) {
        return;
    }
    timer_ = lv_timer_create(TimerEntry, kRefreshMs, this);
    preview_timer_ = lv_timer_create(PreviewTimerEntry, kPreviewIntervalMs, this);
    ESP_LOGI(TAG, "车载界面定时器已创建（%d ms 刷新 / %d ms 预览）", kRefreshMs, kPreviewIntervalMs);
}

void VehicleUi::TimerEntry(lv_timer_t* timer) {
    static_cast<VehicleUi*>(lv_timer_get_user_data(timer))->Tick();
}

void VehicleUi::PreviewTimerEntry(lv_timer_t* timer) {
    static_cast<VehicleUi*>(lv_timer_get_user_data(timer))->TickPreview();
}

void VehicleUi::RequestPage(Page page) {
    pending_page_.store(static_cast<int>(page));
}

void VehicleUi::RequestChatScreen() {
    pending_chat_.store(true);
}

void VehicleUi::Tick() {
    if (!built_) {
        // > 板级构造函数早于 Application::Initialize() 里的 display->SetupUI()，
        // > 所以界面必须等这个标志为真之后才能建。
        if (display_ == nullptr || !display_->IsSetupUICalled()) {
            return;
        }
        BuildOnce();
    }
    ApplyThemeAppearance();
    ApplyPendingNavigation();
    RefreshHome();
    RefreshEvents();
    RefreshSettings();
}

// > 上游会在运行中重新套用主题：main/assets.cc:335-340 的 RefreshDisplayTheme() 调
// > LcdDisplay::SetTheme()，而 SetTheme 改的是 **lv_screen_active()** 的字体与文字色
// > （lcd_display.cc:1149-1174）。所以这里每秒把我们自己页面的字体与底色重新贴一遍：
// > 字体能跟上 assets.Apply() 之后换上的 common 字体（内置 basic 只有 206 个汉字），
// > 底色也不会被主题覆盖。文字颜色不用管——每个 label 自己设了颜色，优先级高于继承。
void VehicleUi::ApplyThemeAppearance() {
    auto* theme = dynamic_cast<LvglTheme*>(display_->GetTheme());
    if (theme == nullptr) {
        return;
    }
    const std::shared_ptr<LvglFont> theme_font = theme->text_font();
    if (theme_font == nullptr) {
        return;
    }
    const lv_font_t* font = theme_font->font();
    if (font == nullptr) {
        return;
    }

    if (font != applied_font_) {
        for (int i = 0; i < static_cast<int>(Page::kCount); i++) {
            if (pages_[i] != nullptr) {
                // > 只贴在 screen 上，子控件继承；绝不逐个 label 设字体。
                lv_obj_set_style_text_font(pages_[i], font, 0);
            }
        }
        if (entry_button_ != nullptr) {
            lv_obj_t* label = lv_obj_get_child(entry_button_, 0);
            if (label != nullptr) {
                lv_obj_set_style_text_font(label, font, 0);
            }
        }
        applied_font_ = font;
        ESP_LOGI(TAG, "已套用主题字体（assets.Apply() 换上 common 字体后会在这一行跟上）");
    }

    for (int i = 0; i < static_cast<int>(Page::kCount); i++) {
        if (pages_[i] != nullptr) {
            lv_obj_set_style_bg_color(pages_[i], lv_color_hex(kBgColor), 0);
        }
    }
}

void VehicleUi::BuildOnce() {
    chat_screen_ = lv_screen_active();

    for (int i = 0; i < static_cast<int>(Page::kCount); i++) {
        pages_[i] = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(pages_[i], lv_color_hex(kBgColor), 0);
        lv_obj_clear_flag(pages_[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    BuildHome(pages_[static_cast<int>(Page::kHome)]);
    BuildPreview(pages_[static_cast<int>(Page::kPreview)]);
    BuildEvents(pages_[static_cast<int>(Page::kEvents)]);
    BuildSettings(pages_[static_cast<int>(Page::kSettings)]);

    // > 入口按钮挂在聊天界面（默认 screen）上：本工程从不使用 lv_layer_top()，
    // > 而且 top layer 不继承 screen 的字体（浮层文字得自己再设一次）。
    // > 上游的聊天容器在 SetupUI() 里已经建完，我们建得比它们晚，再用 lv_obj_move_foreground()
    // > 保一次序，避免被 container_ 盖住点不到。
    entry_button_ = MakeButton(chat_screen_, 6, 6, 110, 46, "车辆", [](lv_event_t* e) {
        auto* self = static_cast<VehicleUi*>(lv_event_get_user_data(e));
        if (self->OnOurPage()) {
            self->RequestChatScreen();
        } else {
            self->RequestPage(Page::kHome);
        }
    }, this);
    if (entry_button_ != nullptr && chat_screen_ != nullptr) {
        lv_obj_move_foreground(entry_button_);
    }

    built_ = true;
    ESP_LOGI(TAG, "车载界面已创建（4 页 + 顶层入口按钮）");
}

void VehicleUi::BuildHome(lv_obj_t* root) {
    lv_obj_t* title = MakeLabel(root, 130, 10, kTextColor);
    lv_label_set_text(title, "车辆状态");

    lv_obj_t* line1 = MakeLabel(root, 16, 70, kTextColor);
    lv_label_set_text(line1, "环境");
    home_temp_ = MakeLabel(root, 16, 110, kTextColor);
    home_humid_ = MakeLabel(root, 16, 150, kTextColor);
    home_lux_ = MakeLabel(root, 16, 190, kTextColor);

    lv_obj_t* line2 = MakeLabel(root, 250, 70, kTextColor);
    lv_label_set_text(line2, "行车");
    home_state_ = MakeLabel(root, 250, 110, kTextColor);
    home_events_ = MakeLabel(root, 250, 150, kTextColor);
    home_net_ = MakeLabel(root, 250, 190, kTextColor);
    home_axes_ = MakeLabel(root, 16, 250, kDimColor);

    MakeButton(root, 340, 250, 120, 50, "返回", [](lv_event_t* e) {
        static_cast<VehicleUi*>(lv_event_get_user_data(e))->RequestChatScreen();
    }, this);
}

void VehicleUi::BuildPreview(lv_obj_t* root) {
    preview_capacity_ = static_cast<size_t>(kPreviewW) * kPreviewH * 2;
    preview_buf_ = static_cast<uint8_t*>(heap_caps_malloc(preview_capacity_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (preview_buf_ == nullptr) {
        ESP_LOGE(TAG, "预览缓冲分配失败（需要 %u B PSRAM），实时画面页不可用",
                 static_cast<unsigned>(preview_capacity_));
    }
    preview_canvas_ = lv_canvas_create(root);
    lv_obj_set_size(preview_canvas_, kPreviewW, kPreviewH);
    lv_obj_align(preview_canvas_, LV_ALIGN_LEFT_MID, 8, 0);
    if (preview_buf_ != nullptr) {
        // > LV_COLOR_FORMAT_RGB565 的画布直接用我们的缓冲，之后每帧只改内容 + invalidate，
        // > 不每帧分配/释放图像对象（10 fps 下那种做法会把 PSRAM 打碎）。
        lv_canvas_set_buffer(preview_canvas_, preview_buf_, kPreviewW, kPreviewH, LV_COLOR_FORMAT_RGB565);
    }

    MakeButton(root, 340, 40, 120, 56, "抓拍", [](lv_event_t* e) {
        auto* self = static_cast<VehicleUi*>(lv_event_get_user_data(e));
        if (self->vehicle_ != nullptr) {
            self->vehicle_->RequestCapture();
        }
    }, this);
    MakeButton(root, 340, 220, 120, 56, "返回", [](lv_event_t* e) {
        static_cast<VehicleUi*>(lv_event_get_user_data(e))->RequestChatScreen();
    }, this);
}

void VehicleUi::BuildEvents(lv_obj_t* root) {
    lv_obj_t* title = MakeLabel(root, 130, 10, kTextColor);
    lv_label_set_text(title, "事件记录");

    for (int i = 0; i < kEventRows; i++) {
        event_rows_[i] = MakeLabel(root, 16, 70 + i * 44, kTextColor);
    }

    MakeButton(root, 130, 258, 90, 50, "上一页", [](lv_event_t* e) {
        auto* self = static_cast<VehicleUi*>(lv_event_get_user_data(e));
        if (self->events_page_ > 0) {
            self->events_page_--;
        }
    }, this);
    MakeButton(root, 240, 258, 90, 50, "下一页", [](lv_event_t* e) {
        auto* self = static_cast<VehicleUi*>(lv_event_get_user_data(e));
        self->events_page_++;   // RefreshEvents() 里会按实际条数夹住上限
    }, this);
    MakeButton(root, 350, 258, 110, 50, "返回", [](lv_event_t* e) {
        static_cast<VehicleUi*>(lv_event_get_user_data(e))->RequestChatScreen();
    }, this);

    events_page_label_ = MakeLabel(root, 16, 20, kDimColor);
}

void VehicleUi::BuildSettings(lv_obj_t* root) {
    lv_obj_t* title = MakeLabel(root, 130, 10, kTextColor);
    lv_label_set_text(title, "设置");

    const vehicle::MonitorConfig cfg = vehicle_->config();
    char buf[192];
    // ! 不要用 %lld：本工程是 nano printf（见 docs/BUGS.md BUG-001）。这里全是 32 位与浮点。
    snprintf(buf, sizeof(buf), "阈值：加速 %.2fg 转弯 %.2fg 颠簸 %.2fg\n碰撞 %.2fg 停车 %d s 锁车 %d s",
             static_cast<double>(cfg.accel_threshold), static_cast<double>(cfg.turn_threshold),
             static_cast<double>(cfg.bump_threshold), static_cast<double>(cfg.crash_threshold),
             static_cast<int>(cfg.static_hold_ms / 1000), static_cast<int>(cfg.lock_hold_ms / 1000));
    lv_obj_t* thresholds = MakeLabel(root, 16, 64, kDimColor);
    lv_label_set_text(thresholds, buf);

    settings_env_label_ = MakeLabel(root, 16, 132, kDimColor);
    lv_label_set_text(settings_env_label_, "环境数据源：模拟（本板无温湿度/光照传感器）");

    settings_axes_label_ = MakeLabel(root, 16, 172, kTextColor);
    lv_label_set_text(settings_axes_label_, "显示三轴实时值");
    settings_axes_switch_ = lv_switch_create(root);
    lv_obj_set_pos(settings_axes_switch_, 260, 162);
    lv_obj_add_event_cb(settings_axes_switch_, [](lv_event_t* e) {
        auto* self = static_cast<VehicleUi*>(lv_event_get_user_data(e));
        // > lv_event_get_target() 返回 void*，C++ 里必须显式转，别直接传给 lv_obj_has_state()。
        self->axes_visible_ = lv_obj_has_state(static_cast<lv_obj_t*>(lv_event_get_target(e)), LV_STATE_CHECKED);
    }, LV_EVENT_VALUE_CHANGED, this);

    lv_obj_t* lock_btn = MakeButton(root, 16, 220, 150, 56, "锁车监测", [](lv_event_t* e) {
        auto* self = static_cast<VehicleUi*>(lv_event_get_user_data(e));
        self->vehicle_->RequestLock(true);
    }, this);
    (void)lock_btn;
    MakeButton(root, 176, 220, 150, 56, "解除锁车", [](lv_event_t* e) {
        auto* self = static_cast<VehicleUi*>(lv_event_get_user_data(e));
        self->vehicle_->RequestLock(false);
    }, this);
    MakeButton(root, 336, 220, 130, 56, "立即抓拍", [](lv_event_t* e) {
        static_cast<VehicleUi*>(lv_event_get_user_data(e))->vehicle_->RequestCapture();
    }, this);

    settings_lock_btn_label_ = MakeLabel(root, 16, 288, kDimColor);
}

void VehicleUi::ApplyPendingNavigation() {
    const int want = pending_page_.exchange(-1);
    const bool want_chat = pending_chat_.exchange(false);

    if (want >= 0 && want < static_cast<int>(Page::kCount)) {
        // > 只在"当前不在自定义页"时记聊天界面，否则页内互切会把聊天界面覆盖掉。
        if (!OnOurPage() && lv_screen_active() != nullptr) {
            chat_screen_ = lv_screen_active();
        }
        lv_screen_load(pages_[want]);
        preview_active_ = (want == static_cast<int>(Page::kPreview));
        if (preview_active_) {
            preview_frames_ = 0;
            preview_misses_ = 0;
            preview_window_start_ms_ = esp_timer_get_time() / 1000;
        }
    } else if (want_chat && chat_screen_ != nullptr) {
        lv_screen_load(chat_screen_);
        preview_active_ = false;
    }
}

void VehicleUi::RefreshHome() {
    if (lv_screen_active() != pages_[static_cast<int>(Page::kHome)]) {
        return;   // 只有当前页在刷新，省 CPU
    }
    const vehicle::VehicleStatus status = vehicle_->Status();
    const vehicle::EnvReading env = vehicle_->env();

    char buf[96];
    if (env.valid) {
        snprintf(buf, sizeof(buf), "温度 %.1f C", static_cast<double>(env.temp_c));
        lv_label_set_text(home_temp_, buf);
        snprintf(buf, sizeof(buf), "湿度 %.0f %%", static_cast<double>(env.humidity_pct));
        lv_label_set_text(home_humid_, buf);
        snprintf(buf, sizeof(buf), "光照 %d lux（%s）", static_cast<int>(env.lux),
                 vehicle::ToString(vehicle::BucketLight(env.lux)));
        lv_label_set_text(home_lux_, buf);
    } else {
        lv_label_set_text(home_temp_, "温度 --");
        lv_label_set_text(home_humid_, "湿度 --");
        lv_label_set_text(home_lux_, "光照 --");
    }

    lv_label_set_text(home_state_, status.calibrated ? StateText(status.state) : "标定中");
    snprintf(buf, sizeof(buf), "事件 %d 次", static_cast<int>(status.events_total));
    lv_label_set_text(home_events_, buf);
    lv_label_set_text(home_net_, Board::GetInstance().GetNetworkStateIcon());
    RefreshAxes();
}

void VehicleUi::RefreshAxes() {
    if (home_axes_ == nullptr) {
        return;
    }
    if (!axes_visible_) {
        lv_label_set_text(home_axes_, "");
        return;
    }
    const vehicle::ImuSample s = vehicle_->Status().sample;
    char buf[96];
    snprintf(buf, sizeof(buf), "ax %.2f ay %.2f az %.2f g", static_cast<double>(s.ax), static_cast<double>(s.ay),
             static_cast<double>(s.az));
    lv_label_set_text(home_axes_, buf);
}

void VehicleUi::RefreshEvents() {
    if (event_rows_[0] == nullptr || lv_screen_active() != pages_[static_cast<int>(Page::kEvents)]) {
        return;
    }
    const std::vector<vehicle::EventRecord> all = vehicle_->CopyHistory();
    const int total = static_cast<int>(all.size());
    int pages = (total + kEventRows - 1) / kEventRows;
    if (pages < 1) {
        pages = 1;
    }
    if (events_page_ < 0) {
        events_page_ = 0;
    }
    if (events_page_ > pages - 1) {
        events_page_ = pages - 1;
    }

    for (int row = 0; row < kEventRows; row++) {
        // > 第 0 页显示最新的一批：从末尾往回数
        const int from_newest = events_page_ * kEventRows + row;
        if (from_newest >= total) {
            lv_label_set_text(event_rows_[row], "");
            continue;
        }
        const vehicle::EventRecord& record = all[static_cast<size_t>(total - 1 - from_newest)];
        lv_label_set_text(event_rows_[row], vehicle::FormatEventLine(record).c_str());
    }

    char buf[48];
    snprintf(buf, sizeof(buf), "%d / %d（共 %d 条，容量 %d）", events_page_ + 1, pages, total,
             vehicle_->history_capacity());
    lv_label_set_text(events_page_label_, buf);
}

void VehicleUi::RefreshSettings() {
    if (settings_lock_btn_label_ == nullptr || lv_screen_active() != pages_[static_cast<int>(Page::kSettings)]) {
        return;
    }
    const vehicle::VehicleStatus status = vehicle_->Status();
    char buf[64];
    snprintf(buf, sizeof(buf), "当前：%s", StateText(status.state));
    lv_label_set_text(settings_lock_btn_label_, buf);
    (void)settings_axes_label_;
}

void VehicleUi::TickPreview() {
    if (!built_ || !preview_active_ || preview_canvas_ == nullptr || preview_buf_ == nullptr || camera_ == nullptr) {
        return;
    }
    if (lv_screen_active() != pages_[static_cast<int>(Page::kPreview)]) {
        return;
    }

    int w = 0;
    int h = 0;
    if (!camera_->CopyPreviewFrame(preview_buf_, preview_capacity_, &w, &h)) {
        preview_misses_++;
        return;
    }
    if (w != kPreviewW || h != kPreviewH) {
        return;
    }
    lv_obj_invalidate(preview_canvas_);
    preview_frames_++;

    // > 设计文档 §10.2 要求预览 ≥10 fps，这里每 5 s 打一次实测值。
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - preview_window_start_ms_ >= 5000) {
        const double seconds = static_cast<double>(now_ms - preview_window_start_ms_) / 1000.0;
        ESP_LOGI(TAG, "预览实测 %.1f fps（%d 帧 / %.1f s，丢帧 %d）",
                 preview_frames_ / (seconds > 0 ? seconds : 1.0), preview_frames_, seconds, preview_misses_);
        preview_frames_ = 0;
        preview_misses_ = 0;
        preview_window_start_ms_ = now_ms;
    }
}
```

- [ ] **步骤 3：接线到板级**

在 `main/boards/esp32s3/esp32s3_board.cc`：
1. 头部 include 增加：

```cpp
#include "vehicle_ui.h"
```
2. 私有成员区（第 30 行 `VehicleService* vehicle_ = nullptr;` 之后）增加：

```cpp
    VehicleUi* vehicle_ui_ = nullptr;
```
3. 构造函数里，在 `vehicle_->Start()` 那段**之后**（`:281` 之后）追加：

```cpp
        // > 界面必须在 display->SetupUI() 之后才建；VehicleUi::Start() 只建定时器，
        // > 真正的界面等 lv_timer 第一次 tick 且 IsSetupUICalled() 为真时才创建。
        vehicle_ui_ = new VehicleUi(display_, vehicle_, nullptr);   // 任务 8 会把 camera_ 传进来
        vehicle_ui_->Start();
```

- [ ] **步骤 4：加 `VehicleService` 的两个访问器**

`vehicle_ui.cc` 用到了 `vehicle_->config()` 与 `vehicle_->history_capacity()`，任务 4 的头里还没有。在 `vehicle_service.h` public 段（`Status()` 之后）追加：

```cpp
    // 阈值配置的一份拷贝（设置页只读展示用）
    vehicle::MonitorConfig config() const { return config_; }
    // 事件历史的容量（设置/事件页显示"已用/容量"）
    int history_capacity() const { return history_.capacity(); }
```

同时在 `main/vehicle/environment_sensor.h` 的 `BucketLight` 已经可用（任务 1 已实现），`vehicle_ui.cc` 已 include 它。

- [ ] **步骤 5：构建 + 真机验证（D3 验收）**

```
idf.py reconfigure      # 新增了板级 .cc
idf.py build > build\last_build.log 2>&1
```
预期：`Project build complete.`，且板级文件零 warning。

烧录后抓串口，预期：
```
I (xxx) VehicleUi: 车载界面定时器已创建（1000 ms 刷新 / 100 ms 预览）
I (xxx) VehicleUi: 车载界面已创建（4 页 + 顶层入口按钮）
```
真机动作与判据（**D3 的当日验收**）：
1. 屏幕上出现左上角"车辆"按钮 → 点它 → 进入**主页**；
2. 主页显示：温度/湿度/光照（模拟值，数字每秒在动）、行车状态（"停车"/"行驶"/"标定中"）、事件数；
3. 点"返回" → 回到 xiaozhi 聊天界面，聊天界面功能正常（能对话、表情/字幕照常）；
4. 再进"车辆"→ 事件页**能翻页**（`上一页`/`下一页` 有效，页码显示 `1 / N`）；
5. **中文必须逐字核对**（这是本任务最容易翻车的地方）：
   - 判据不是"方框"，而是**整字消失**——本工程没开 `CONFIG_LV_USE_FONT_PLACEHOLDER`（`sdkconfig:3045`），缺字在 LVGL 9.4 里 `box_w = adv_w = 0`（`lv_font.c:135-141`），既不画方框也不占宽度。
   - 先看串口有没有 `VehicleUi: 已套用主题字体（...）`。没有 → `GetTheme()` 或 `text_font()` 取不到，先查这里。
   - 有这行但字仍缺 → 说明主题字体还是 `font_puhui_basic_30_4`（只有 206 个汉字）。查 assets 是否真的 Apply 了：串口应有 `Assets: The partition size is 5888 KB` 一类日志，且 `assets` 分区有效（`main/application.cc:351-354` 在分区无效时会静默降级）。
   - 若确认 common 字体已生效、仍有个别字不显示，**把文案换成同义的常用字**（例如"锁车监测"→"锁车"）。**不要**去改 `main/CMakeLists.txt:102` 的 `BUILTIN_TEXT_FONT`——那会波及所有用 30 号字体的板，而且换的也是同一套 basic 系列。
   - 任何一项实测不符预期，都要按 `docs/BUGS.md` 的格式记一条（含串口片段与具体缺失的汉字）。

- [ ] **步骤 6：展示并提交（等用户确认）**

```bash
git add main/boards/esp32s3/vehicle_ui.h main/boards/esp32s3/vehicle_ui.cc main/boards/esp32s3/esp32s3_board.cc main/boards/esp32s3/vehicle_service.h
git status --short
git diff --cached --stat
```
建议 commit message：`feat: 新增车载 LVGL 页面（主页/事件页/设置页骨架）与顶层入口`

---

### 任务 6：事件行文本格式化（纯逻辑，给事件页用）

> **本任务应在任务 5 之前完成**（任务 5 的界面代码直接用这里的 `FormatEventLine()`）。它是独立的小任务，先做掉可以省一次来回。

**为什么单独做：** 事件页是纯 UI、没法主机测试，但它显示的**文本内容**可以——而且 Plan C 的语音播报也要用同一份格式化。把"事件 → 一行文字"抽成纯函数，UI 只负责把字符串贴到 label 上。

**文件：**
- 创建：`main/vehicle/event_text.h`
- 创建：`main/vehicle/event_text.cc`
- 测试：`test/event_text_test.cc`
- 修改：`main/CMakeLists.txt:45-51`

- [ ] **步骤 1：编写失败的测试**

创建 `test/event_text_test.cc`：

```cpp
// 事件行文本格式化的主机单元测试
//
// 编译与运行：
//   g++ -std=c++17 -Wall -Wextra -I main/vehicle test/event_text_test.cc main/vehicle/event_text.cc main/vehicle/event_json.cc main/vehicle/driving_monitor.cc -o build_host/event_text_test.exe
//   build_host/event_text_test.exe
// （要一起编 event_json.cc 是为了复用 ToDecimal；要编 driving_monitor.cc 是因为
//   vehicle::ToString(EventType) 的实现放在那里。）

#include <cstdio>
#include <string>

#include "event_text.h"

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

static EventRecord Make(int64_t seq, EventType type, int64_t ts_ms, float value) {
    EventRecord r;
    r.seq = seq;
    r.event.type = type;
    r.event.ts_ms = ts_ms;
    r.event.value = value;
    return r;
}

int main() {
    printf("event_text\n");

    CHECK(FormatUptime(0) == "0.0s", "0 ms → 0.0s");
    CHECK(FormatUptime(12300) == "12.3s", "12300 ms → 12.3s");
    CHECK(FormatUptime(59900) == "59.9s", "59900 ms → 59.9s");
    CHECK(FormatUptime(60000) == "1m00s", "60000 ms → 1m00s");
    CHECK(FormatUptime(123000) == "2m03s", "123000 ms → 2m03s");
    CHECK(FormatUptime(-5) == "0.0s", "负数当 0 处理");

    const std::string line = FormatEventLine(Make(7, EventType::kHardAccel, 12300, 0.38f));
    printf("       line = %s\n", line.c_str());
    CHECK(line == "#7 急加速 0.38g 12.3s", "急加速事件行");
    CHECK(line.find('\n') == std::string::npos, "行里没有换行（label 不需要）");

    CHECK(FormatEventLine(Make(12, EventType::kHardBrake, 194900, -0.52f)) == "#12 急刹车 -0.52g 3m14s",
          "急刹车事件行（负值保留符号）");
    CHECK(FormatEventLine(Make(1, EventType::kMotionWhileParked, 1000, 0.31f)) == "#1 异常震动 0.31g 1.0s",
          "锁车期异常震动");
    CHECK(FormatEventLine(Make(3, EventType::kParked, 40000, 0.0f)) == "#3 停车 0.00g 40.0s", "停车事件");

    if (g_failures == 0) {
        printf("all passed\n");
        return 0;
    }
    printf("%d failure(s)\n", g_failures);
    return 1;
}
```

- [ ] **步骤 2：运行测试验证失败**

运行：
```powershell
$env:PATH="C:\mingw64\bin;$env:PATH"
g++ -std=c++17 -Wall -Wextra -I main/vehicle test/event_text_test.cc main/vehicle/event_text.cc main/vehicle/event_json.cc main/vehicle/driving_monitor.cc -o build_host/event_text_test.exe
```
预期：FAIL，`fatal error: event_text.h: No such file or directory`

- [ ] **步骤 3：编写最少实现代码**

创建 `main/vehicle/event_text.h`：

```cpp
#pragma once

// 事件 → 屏上一行 / 播报用的文本。纯逻辑，不依赖 ESP-IDF。
// 屏幕事件页与 Plan C 的语音播报共用同一份格式化，避免两处漂移。

#include <cstdint>
#include <string>

#include "event_history.h"

namespace vehicle {

// 开机以来的毫秒 → "12.3s"（<1 min）或 "3m14s"（>=1 min）。负数当 0。
std::string FormatUptime(int64_t ms);

// 一行事件文本，例："#7 急加速 0.38g 12.3s"
std::string FormatEventLine(const EventRecord &record);

}  // namespace vehicle
```

创建 `main/vehicle/event_text.cc`：

```cpp
#include "event_text.h"

#include <cstdio>

#include "event_json.h"   // 复用 ToDecimal（nano printf 不支持 64 位格式）

namespace vehicle {

std::string FormatUptime(int64_t ms) {
    int64_t value = ms > 0 ? ms : 0;
    char buf[32];
    if (value < 60000) {
        snprintf(buf, sizeof(buf), "%d.%d s", static_cast<int>(value / 1000),
                 static_cast<int>((value % 1000) / 100));
    } else {
        snprintf(buf, sizeof(buf), "%dm%02ds", static_cast<int>(value / 60000),
                 static_cast<int>((value % 60000) / 1000));
    }
    std::string out(buf);
    // > 去掉 "12.3 s" 中间那个空格，让行更紧凑（"12.3s"）
    const size_t space = out.find(" s");
    if (space != std::string::npos) {
        out.erase(space, 1);
    }
    return out;
}

std::string FormatEventLine(const EventRecord &record) {
    char value_buf[16];
    snprintf(value_buf, sizeof(value_buf), "%.2f", static_cast<double>(record.event.value));

    std::string out;
    out.reserve(64);
    out += "#";
    out += ToDecimal(record.seq);
    out += " ";
    out += ToString(record.event.type);
    out += " ";
    out += value_buf;
    out += "g ";
    out += FormatUptime(record.event.ts_ms);
    return out;
}

}  // namespace vehicle
```

- [ ] **步骤 4：运行测试验证通过**

运行：
```powershell
$env:PATH="C:\mingw64\bin;$env:PATH"
g++ -std=c++17 -Wall -Wextra -I main/vehicle test/event_text_test.cc main/vehicle/event_text.cc main/vehicle/event_json.cc main/vehicle/driving_monitor.cc -o build_host/event_text_test.exe
build_host\event_text_test.exe
```
预期：`all passed`，零 warning。

- [ ] **步骤 5：接进固件构建**

`main/CMakeLists.txt` 的 `list(APPEND SOURCES ...)` 追加 `"vehicle/event_text.cc"`，运行 `idf.py reconfigure`。

- [ ] **步骤 6：展示并提交（等用户确认）**

```bash
git add main/vehicle/event_text.h main/vehicle/event_text.cc test/event_text_test.cc main/CMakeLists.txt
git status --short
git diff --cached --stat
```
建议 commit message：`feat: 新增事件行文本格式化与主机测试`

---

### 任务 7：`snapshots` 分区 + `SnapshotStore`（D5 的存储底座）

> **! 这一步必须先纠正设计文档的一个错误。** `docs/superpowers/specs/2026-09-16-vehicle-terminal-design.md:117-123` 写"现有分区偏移一律不动，追加 `snapshots, data, spiffs, 0xDC0000, 0x240000`"。实测 `partitions/v2/16m.csv` 的表**已经铺满 16 MB、没有空洞**：`assets` 结束于 `0xDC0000`，紧接着是上一项目遗留的 `human_face_det`(0xDC0000, 0x40000) 与 `human_face_feat`(0xE00000, 0x200000)，合计正好 `0x240000` = 2.25 MB。所以正确做法是**删掉这两个没人用的模型分区、把同一段地址让给 `snapshots`**——其余分区偏移一个都不动。
>
> 依据：`build/partition-table.bin`（3072 B）解码出的 label 序列就是 `nvs|otadata|phy_init|ota_0|ota_1|assets|human_face_det|human_face_feat`；`build/flash_args` 里**没有**这两个模型分区的烧写条目（从未烧过）；全工程只有 `main/boards/esp32s3/face_selftest.cc:35` 引用 label `"human_face_det"`，而该文件整份被 `#if 0` 停用（`face_selftest.cc:4`）；`build/ldgen_libraries` 里没有任何 espdl 库。

**文件：**
- 修改：`partitions/v2/16m.csv`
- 创建：`main/boards/esp32s3/snapshot_store.h`
- 创建：`main/boards/esp32s3/snapshot_store.cc`
- 修改：`main/boards/esp32s3/esp32s3_board.cc`（构造并注册 EventSink）

- [ ] **步骤 1：改分区表**

把 `partitions/v2/16m.csv` 的第 10–13 行（两个模型分区及其注释）替换为：

```
# > 本项目用闪存尾部 2.25 MB 放抓拍与事件日志。
# > 这段地址原本是上一项目的人脸模型分区（human_face_det / human_face_feat），
# > 那两个分区从未被烧写过（见 build/flash_args），且唯一引用它们的 face_selftest.cc 已整份 #if 0 停用。
# > 其余分区偏移一个都没动。
snapshots,       data, spiffs,  0xDC0000, 0x240000,
```

改完运行（**这一步会重刷分区表，需要用户批准**）：

```
cmd /c "... && idf.py build"          # 重新生成 build/partition_table/partition-table.bin
python -m esptool --chip esp32s3 -p COM10 -b 460800 write_flash 0x8000 build\partition_table\partition-table.bin
```

预期：`Hash of data verified.`；重启后串口能看到 `snapshots` 分区被挂载（下一步的日志）。

- [ ] **步骤 2：写 `snapshot_store.h`**

```cpp
#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "snapshot_ring.h"
#include "vehicle_service.h"   // EventSink
#include "vehicle_types.h"

// 抓拍与事件日志的落盘层：`snapshots` 分区挂在 /snap。
//
// ! 挂载失败时进"内存模式"：只保留最近一张抓拍在 PSRAM，/latest.jpg 仍可访问，
// ! 事件日志停写。**任何情况下都不阻塞其它功能**（设计文档 §5.3 / §9）。
class SnapshotStore : public EventSink {
public:
    static constexpr const char *kRoot = "/snap";
    static constexpr size_t kMaxLogBytes = 256 * 1024;   // events.log 超过就清空重开

    explicit SnapshotStore(int capacity = 32);

    // 挂载分区；失败返回 false（调用方只告警，不阻断开机）
    bool Start();

    bool mounted() const { return mounted_; }
    int saved_count() const { return ring_.written(); }

    // 保存一张 JPEG：环形覆盖 + 更新 latest.idx
    bool SaveSnapshot(const uint8_t *jpeg, size_t len);
    // 读最近一张抓拍（供 /latest.jpg）
    bool ReadLatestJpeg(std::string &out);
    // 读最近 max_lines 行事件日志（供 /events）
    bool ReadRecentEvents(int max_lines, std::string &out);

    // EventSink：事件落盘
    void OnEvent(const vehicle::EventRecord &record) override;

private:
    bool AppendEventLine(const std::string &line);

    vehicle::SnapshotRing ring_;
    bool mounted_ = false;
    std::mutex mutex_;             // worker 任务写、HTTP 任务读
    std::string memory_jpeg_;      // 内存模式的最近一张
};
```

- [ ] **步骤 3：写 `snapshot_store.cc`**

```cpp
#include "snapshot_store.h"

#include <sys/stat.h>

#include <cstdio>

#include <esp_log.h>
#include <esp_spiffs.h>

#include "event_json.h"

#define TAG "SnapshotStore"

SnapshotStore::SnapshotStore(int capacity) : ring_(capacity) {
}

bool SnapshotStore::Start() {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = kRoot,
        .partition_label = "snapshots",
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    const esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "snapshots 分区挂载失败（%s），进入内存模式：只保留最近一张抓拍", esp_err_to_name(err));
        mounted_ = false;
        return false;
    }

    size_t total = 0;
    size_t used = 0;
    if (esp_spiffs_info("snapshots", &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "snapshots 已挂载到 %s：共 %u KB，已用 %u B", kRoot, static_cast<unsigned>(total / 1024),
                 static_cast<unsigned>(used));
    }
    mounted_ = true;
    return true;
}

bool SnapshotStore::SaveSnapshot(const uint8_t *jpeg, size_t len) {
    if (jpeg == nullptr || len == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!mounted_) {
        memory_jpeg_.assign(reinterpret_cast<const char *>(jpeg), len);
        ESP_LOGW(TAG, "内存模式：抓拍只保留在 PSRAM（%u B）", static_cast<unsigned>(len));
        return true;
    }

    const int slot = ring_.NextSlot();
    const std::string name = ring_.FileNameFor(slot);
    const std::string path = std::string(kRoot) + "/" + name;

    FILE *file = fopen(path.c_str(), "wb");
    if (file == nullptr) {
        ESP_LOGE(TAG, "打开 %s 失败，退回内存模式保存这一张", path.c_str());
        memory_jpeg_.assign(reinterpret_cast<const char *>(jpeg), len);
        return false;
    }
    const size_t written = fwrite(jpeg, 1, len, file);
    fclose(file);

    const std::string idx_path = std::string(kRoot) + "/latest.idx";
    FILE *idx = fopen(idx_path.c_str(), "w");
    if (idx != nullptr) {
        const std::string content = ring_.LatestIndexContent();
        fwrite(content.data(), 1, content.size(), idx);
        fclose(idx);
    }

    if (written != len) {
        ESP_LOGW(TAG, "抓拍写入不完整：%u / %u B", static_cast<unsigned>(written), static_cast<unsigned>(len));
    } else {
        ESP_LOGI(TAG, "抓拍已保存 %s（%u B，累计第 %d 张）", name.c_str(), static_cast<unsigned>(len), ring_.written());
    }
    return written == len;
}

bool SnapshotStore::ReadLatestJpeg(std::string &out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!mounted_) {
        if (memory_jpeg_.empty()) {
            return false;
        }
        out = memory_jpeg_;
        return true;
    }

    const std::string idx_path = std::string(kRoot) + "/latest.idx";
    FILE *idx = fopen(idx_path.c_str(), "r");
    if (idx == nullptr) {
        return false;
    }
    int slot = -1;
    const int matched = fscanf(idx, "%d", &slot);
    fclose(idx);
    if (matched != 1 || slot < 0) {
        return false;
    }
    const std::string name = ring_.FileNameFor(slot);
    if (name.empty()) {
        return false;
    }

    const std::string path = std::string(kRoot) + "/" + name;
    FILE *file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    out.clear();
    char buf[512];
    size_t read = 0;
    while ((read = fread(buf, 1, sizeof(buf), file)) > 0) {
        out.append(buf, read);
    }
    fclose(file);
    return !out.empty();
}

bool SnapshotStore::ReadRecentEvents(int max_lines, std::string &out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!mounted_) {
        return false;
    }
    const std::string path = std::string(kRoot) + "/events.log";
    FILE *file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    const long size = ftell(file);
    // > 从尾部往回读 8 KB 足够覆盖最近 50 条（一条约 60 B）；避免把整份日志读进内存。
    const long window = size > 8192 ? 8192 : size;
    if (fseek(file, size - window, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    std::string tail;
    tail.resize(static_cast<size_t>(window));
    const size_t got = fread(&tail[0], 1, static_cast<size_t>(window), file);
    fclose(file);
    tail.resize(got);

    // > 窗口开头可能截断半行：丢掉第一行（除非正好从文件头开始）。
    if (size > window) {
        const size_t newline = tail.find('\n');
        tail.erase(0, newline == std::string::npos ? tail.size() : newline + 1);
    }

    int lines = 0;
    for (size_t i = tail.size(); i > 0; i--) {
        if (tail[i - 1] == '\n') {
            lines++;
            if (lines > max_lines) {
                tail.erase(0, i);
                break;
            }
        }
    }
    out = tail;
    return true;
}

void SnapshotStore::OnEvent(const vehicle::EventRecord &record) {
    AppendEventLine(vehicle::EventToJson(record));
}

bool SnapshotStore::AppendEventLine(const std::string &line) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!mounted_) {
        return false;
    }
    const std::string path = std::string(kRoot) + "/events.log";

    struct stat st = {};
    if (stat(path.c_str(), &st) == 0 && static_cast<size_t>(st.st_size) > kMaxLogBytes) {
        // > 不考虑复杂轮转：/events 只要展示最近若干条，MQTT 补传游标由 Plan C 放 NVS。
        remove(path.c_str());
        ESP_LOGW(TAG, "events.log 超过 %u KB，已清空重开", static_cast<unsigned>(kMaxLogBytes / 1024));
    }

    FILE *file = fopen(path.c_str(), "a");
    if (file == nullptr) {
        ESP_LOGW(TAG, "events.log 打不开，事件未落盘");
        return false;
    }
    fwrite(line.data(), 1, line.size(), file);
    fwrite("\n", 1, 1, file);
    fclose(file);
    return true;
}
```

- [ ] **步骤 4：接线到板级**

`main/boards/esp32s3/esp32s3_board.cc`：
1. include 增加 `#include "snapshot_store.h"`。
2. 成员区增加 `SnapshotStore* snapshot_store_ = nullptr;`。
3. 构造函数里，在 `vehicle_ = new VehicleService(i2c_bus_);` **之前**插入：

```cpp
        // > 抓拍与事件日志的存储层。挂载失败只告警（内存模式），不阻断开机。
        snapshot_store_ = new SnapshotStore(32);
        if (!snapshot_store_->Start()) {
            ESP_LOGW(TAG, "snapshots 分区不可用，抓拍只在内存里保留最近一张");
        }
```
4. 在 `vehicle_ = new VehicleService(i2c_bus_);` **之后**、`vehicle_->Start()` **之前**插入：

```cpp
        vehicle_->AddEventSink(snapshot_store_);
```

- [ ] **步骤 5：构建 + 真机验证**

```
idf.py reconfigure
idf.py build > build\last_build.log 2>&1
```

烧录（**分区表已改，务必按任务 7 步骤 1 刷 0x8000**）后抓串口，预期：
```
I (xxx) SnapshotStore: snapshots 已挂载到 /snap：共 2304 KB，已用 0 B
I (xxx) VehicleService: imu_task 已启动：20 ms 周期（50 Hz）；worker 周期 200 ms
```
真机动作：静置 30 s 以上让状态机进停车、再等到进"锁车监测"或手动晃一下板子触发事件 → 预期日志出现事件行。串口里**不应**出现 `events.log 打不开` 或 `内存模式`。

- [ ] **步骤 6：展示并提交（等用户确认）**

```bash
git add partitions/v2/16m.csv main/boards/esp32s3/snapshot_store.h main/boards/esp32s3/snapshot_store.cc main/boards/esp32s3/esp32s3_board.cc
git status --short
git diff --cached --stat
```
建议 commit message：`feat: 用尾部 2.25MB 建 snapshots 分区并实现抓拍/事件落盘`

---

### 任务 8：摄像头取帧与 JPEG 抓拍（D4 的抓拍部分）

> **为什么不用 `Esp32Camera::Capture()`：** 它没有取帧接口（`current_fb_` 是 private）、每帧在 PSRAM 新分配 153,600 B、无条件把帧推到**默认 screen** 的聊天预览槽并重置 5 秒隐藏计时，而且内部无锁（`main/boards/common/esp32_camera.cc:59-130`）。实时预览与"抓拍落盘"都拿不到干净的一帧。所以这里自己用全局 `esp_camera_fb_get()/fb_return()`，并复用与上游一致的 RGB565 字节序交换（`esp32_camera.cc:99-108` 的 `__builtin_bswap16`）。
>
> **随之而来的并发风险与对策：** `esp_camera_fb_get/return` 不是线程安全的，而 `self.camera.take_photo`（`mcp_server.cc:100-121`）会在 MCP 线程调 `Capture()`。本板的相机配置是 `fb_count = 1`（`esp32s3_board.cc:175`）——**只有一个帧缓冲，两边抢必然出问题**。所以：
> 1. 把 `fb_count` 改成 **2**（多一个 150 KB 的 PSRAM 帧缓冲，8 MB PSRAM 完全够）；
> 2. 我们自己的取帧走一把 `std::mutex`，并且**持帧时间只有一次 memcpy**；
> 3. 预览在**非待机态自动暂停**（任务 9 实现），从源头避开与小智拍照/说话的重叠窗口。

**文件：**
- 创建：`main/boards/esp32s3/camera_capture.h`
- 创建：`main/boards/esp32s3/camera_capture.cc`
- 修改：`main/boards/esp32s3/esp32s3_board.cc`（`fb_count = 2`；构造 `CameraCapture` 并注册 EventSink）

- [ ] **步骤 1：写 `camera_capture.h`**

```cpp
#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "vehicle_service.h"   // EventSink
#include "vehicle_types.h"

class SnapshotStore;

// 摄像头取帧 + JPEG 抓拍。
//
// ! 不复用 Esp32Camera::Capture()：它没有取帧接口、每帧新分配 153,600 B PSRAM、
// ! 会把帧推到聊天界面的预览槽并重置 5 s 隐藏计时，而且内部无锁。
// ! 这里直接走 esp_camera_fb_get()/fb_return()，并用一把锁把"预览"与"抓拍"隔开
// ! （设计文档 §8：预览与抓拍互斥）。
class CameraCapture : public EventSink {
public:
    explicit CameraCapture(SnapshotStore *store, int width = 320, int height = 240);
    ~CameraCapture();

    // 复制一帧到 dst（已做 RGB565 字节序交换，stride = width * 2）。
    // dst 容量不足 / 拿不到帧 / 分辨率不符都返回 false（调用方跳过这一帧即可）。
    bool CopyPreviewFrame(uint8_t *dst, size_t dst_capacity, int *width, int *height);

    // 抓拍一张 JPEG 到 out（内部 free 编码缓冲）
    bool CaptureJpeg(std::string &out);

    // EventSink：碰撞 / 锁车期异常震动 / 进入锁车监测 / 手动抓拍
    void OnCaptureRequest(vehicle::CaptureReason reason, int64_t ts_ms) override;

    int ok_count() const { return ok_count_; }
    int fail_count() const { return fail_count_; }

private:
    // 取一帧并做字节序交换到 swap_buf_；返回交换后的字节数（0 = 失败）
    size_t GrabSwapped();

    SnapshotStore *store_ = nullptr;
    std::mutex mutex_;
    uint8_t *swap_buf_ = nullptr;
    size_t swap_capacity_ = 0;
    int width_ = 320;
    int height_ = 240;
    int ok_count_ = 0;
    int fail_count_ = 0;
    bool warned_size_ = false;
};
```

- [ ] **步骤 2：写 `camera_capture.cc`**

```cpp
#include "camera_capture.h"

#include <cstdlib>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>

#include "esp_camera.h"
#include "image_to_jpeg.h"
#include "snapshot_store.h"

#define TAG "CameraCapture"

namespace {
constexpr uint8_t kJpegQuality = 80;   // 与 Esp32Camera::Explain() 一致（esp32_camera.cc:209）
}  // namespace

CameraCapture::CameraCapture(SnapshotStore *store, int width, int height)
    : store_(store), width_(width), height_(height) {
}

CameraCapture::~CameraCapture() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (swap_buf_ != nullptr) {
        heap_caps_free(swap_buf_);
        swap_buf_ = nullptr;
    }
}

size_t CameraCapture::GrabSwapped() {
    const size_t need = static_cast<size_t>(width_) * static_cast<size_t>(height_) * 2;
    if (swap_buf_ == nullptr || swap_capacity_ < need) {
        if (swap_buf_ != nullptr) {
            heap_caps_free(swap_buf_);
            swap_buf_ = nullptr;
            swap_capacity_ = 0;
        }
        swap_buf_ = static_cast<uint8_t *>(heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (swap_buf_ == nullptr) {
            ESP_LOGE(TAG, "交换缓冲分配失败（需要 %u B PSRAM）", static_cast<unsigned>(need));
            return 0;
        }
        swap_capacity_ = need;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == nullptr) {
        fail_count_++;
        return 0;
    }

    size_t copied = 0;
    if (fb->format == PIXFORMAT_RGB565 && fb->width == width_ && fb->height == height_) {
        // > 与上游同口径：RGB565 需要 16 位字节序交换（esp32_camera.cc:102-105 的 bswap16），
        // > 显示侧（lcd_display.cc:157 swap_bytes=1）与 JPEG 编码侧（esp32_camera.cc:204-209）
        // > 用的都是交换后的数据。
        const uint16_t *src = reinterpret_cast<const uint16_t *>(fb->buf);
        uint16_t *dst = reinterpret_cast<uint16_t *>(swap_buf_);
        for (size_t i = 0; i < static_cast<size_t>(width_) * static_cast<size_t>(height_); i++) {
            dst[i] = __builtin_bswap16(src[i]);
        }
        copied = need;
        ok_count_++;
    } else if (!warned_size_) {
        warned_size_ = true;
        ESP_LOGW(TAG, "相机帧格式不符：format=%d %dx%d（期望 RGB565 %dx%d），预览与抓拍会一直跳过", fb->format,
                 fb->width, fb->height, width_, height_);
        fail_count_++;
    } else {
        fail_count_++;
    }

    esp_camera_fb_return(fb);
    return copied;
}

bool CameraCapture::CopyPreviewFrame(uint8_t *dst, size_t dst_capacity, int *width, int *height) {
    const size_t need = static_cast<size_t>(width_) * static_cast<size_t>(height_) * 2;
    if (dst == nullptr || dst_capacity < need) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (GrabSwapped() == 0) {
        return false;
    }
    memcpy(dst, swap_buf_, need);
    if (width != nullptr) {
        *width = width_;
    }
    if (height != nullptr) {
        *height = height_;
    }
    return true;
}

bool CameraCapture::CaptureJpeg(std::string &out) {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t swapped = GrabSwapped();
    if (swapped == 0) {
        return false;
    }

    uint8_t *jpeg = nullptr;
    size_t jpeg_len = 0;
    if (!image_to_jpeg(swap_buf_, swapped, static_cast<uint16_t>(width_), static_cast<uint16_t>(height_),
                       V4L2_PIX_FMT_RGB565, kJpegQuality, &jpeg, &jpeg_len)) {
        ESP_LOGW(TAG, "JPEG 编码失败");
        return false;
    }
    if (jpeg == nullptr || jpeg_len == 0) {
        ESP_LOGW(TAG, "JPEG 编码返回空数据");
        return false;
    }

    out.assign(reinterpret_cast<const char *>(jpeg), jpeg_len);
    // > image_to_jpeg() 的输出缓冲由调用方释放，实现内部自己也是 free(outbuf)
    // > （main/display/lvgl_display/jpg/image_to_jpeg.cpp:423）。
    free(jpeg);
    return true;
}

void CameraCapture::OnCaptureRequest(vehicle::CaptureReason reason, int64_t ts_ms) {
    if (store_ == nullptr) {
        return;
    }
    std::string jpeg;
    if (!CaptureJpeg(jpeg)) {
        ESP_LOGW(TAG, "抓拍失败（原因：%s）", vehicle::ToString(reason));
        return;
    }
    const bool saved = store_->SaveSnapshot(reinterpret_cast<const uint8_t *>(jpeg.data()), jpeg.size());
    // ! 不要用 %lld：本工程是 nano printf（docs/BUGS.md BUG-001）。ts_ms 用 double 打。
    ESP_LOGI(TAG, "抓拍完成：原因=%s %u KB ts=%.3f s 落盘=%s", vehicle::ToString(reason),
             static_cast<unsigned>(jpeg.size() / 1024), static_cast<double>(ts_ms) / 1000.0, saved ? "是" : "否");
}
```

- [ ] **步骤 3：把相机帧缓冲加到 2 个，并接线**

`main/boards/esp32s3/esp32s3_board.cc`：
1. `InitializeCamera()` 里把 `config.fb_count = 1;`（第 175 行）改成：

```cpp
        // ! 设 2 而不是 1：实时预览（vehicle_ui 的 16 fps 取帧）与小智的 self.camera.take_photo
        // ! 会同时用相机，只有一个帧缓冲时两边必然互相偷帧。多一个 QVGA RGB565 帧缓冲
        // ! 约 150 KB PSRAM，8 MB PSRAM 完全够。
        config.fb_count = 2;
```
2. include 增加 `#include "camera_capture.h"`。
3. 成员区增加 `CameraCapture* camera_capture_ = nullptr;`。
4. 构造函数里，在 `vehicle_->AddEventSink(snapshot_store_);` **之后**、`vehicle_->Start()` **之前**插入：

```cpp
        // > 抓拍与预览共用相机，内部用一把锁互斥（设计文档 §8）。
        camera_capture_ = new CameraCapture(snapshot_store_);
        vehicle_->AddEventSink(camera_capture_);
```

- [ ] **步骤 4：手动抓拍联调（D4 抓拍验收）**

构建、烧录、抓串口。动作与判据：
1. 点屏幕左上角"车辆" → 进实时画面页（此时预览还没接上，画面是空的，正常）；
2. 点"抓拍" → 串口预期：
```
I (xxx) CameraCapture: 抓拍完成：原因=手动 20 KB ts=12.345 s 落盘=是
I (xxx) SnapshotStore: 抓拍已保存 snap_000.jpg（20123 B，累计第 1 张）
```
3. 连点 3 次 → `snap_001.jpg` / `snap_002.jpg` 依次出现，**没有** `抓拍失败`、没有 `交换缓冲分配失败`。
4. 静置让状态机进"锁车监测"（或手动点设置页"锁车监测"按钮）→ 再晃一下板子 → 预期 `原因=异常震动` 或 `原因=进入锁车监测` 的抓拍日志。
5. 让小智拍一张照（"你好小智，看看这是什么"）→ 确认 `self.camera.take_photo` **仍然能用**（说明 `fb_count = 2` 与我们的取帧没把相机搞坏）。

- [ ] **步骤 5：展示并提交（等用户确认）**

```bash
git add main/boards/esp32s3/camera_capture.h main/boards/esp32s3/camera_capture.cc main/boards/esp32s3/esp32s3_board.cc
git status --short
git diff --cached --stat
```
建议 commit message：`feat: 新增摄像头取帧与 JPEG 抓拍并接入事件联动`

---

### 任务 9：实时画面页联调（D4 验收项）

页面骨架与预览定时器在任务 5 已经写好（`BuildPreview()` + `TickPreview()`），这一步只做三件事：把 `CameraCapture` 交给 UI、加"非待机态暂停预览"的规则、实测帧率。

**文件：**
- 修改：`main/boards/esp32s3/vehicle_ui.h`、`main/boards/esp32s3/vehicle_ui.cc`
- 修改：`main/boards/esp32s3/esp32s3_board.cc`（把 `camera_capture_` 传给 `VehicleUi`）

- [ ] **步骤 1：加"对话时暂停预览"规则**

`vehicle_ui.h` 私有成员里加一行：

```cpp
    int preview_paused_ = 0;   // 因非待机态而跳过预览的次数
```

`vehicle_ui.cc` 头部 include 增加：

```cpp
#include "application.h"
#include "device_state.h"
```

把 `TickPreview()` 的开头改成：

```cpp
    if (!built_ || !preview_active_ || preview_canvas_ == nullptr || preview_buf_ == nullptr || camera_ == nullptr) {
        return;
    }
    if (lv_screen_active() != pages_[static_cast<int>(Page::kPreview)]) {
        return;
    }
    // > 只在待机态预览：小智拍照（self.camera.take_photo → Esp32Camera::Capture()）与说话时
    // > 也在用摄像头和整机带宽，重叠会互相偷帧（本板 fb_count 只有 2）；顺带也避免对话时
    // > 预览把 SPI 带宽占满。
    if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
        preview_paused_++;
        return;
    }
```

fps 日志带上暂停计数：

```cpp
        ESP_LOGI(TAG, "预览实测 %.1f fps（%d 帧 / %.1f s，丢帧 %d，对话中暂停 %d 次）",
                 preview_frames_ / (seconds > 0 ? seconds : 1.0), preview_frames_, seconds, preview_misses_,
                 preview_paused_);
```

`ApplyPendingNavigation()` 里切换页面时清计数的那两行，改成三个一起清：

```cpp
        if (preview_active_) {
            preview_frames_ = 0;
            preview_misses_ = 0;
            preview_paused_ = 0;
            preview_window_start_ms_ = esp_timer_get_time() / 1000;
        }
```

- [ ] **步骤 1b：补上"摄像头不可用"的降级提示（设计文档 §9 要求）**

`vehicle_ui.h` 私有成员再加两个：

```cpp
    lv_obj_t* preview_status_ = nullptr;   // 预览页左上角的状态文字（仅异常时显示）
    int preview_fail_streak_ = 0;          // 连续取帧失败次数
```

`BuildPreview()` 里补一行（放在建 canvas 之前）：

```cpp
    preview_status_ = MakeLabel(root, 12, 6, kDimColor);
```

`TickPreview()` 里，取帧失败分支改成：

```cpp
    int w = 0;
    int h = 0;
    if (!camera_->CopyPreviewFrame(preview_buf_, preview_capacity_, &w, &h)) {
        preview_misses_++;
        // > 连续失败 ≈2 s（60 ms × 30）就上屏提示，避免"黑屏但不知道哪里坏了"。
        // > 摄像头初始化失败时 Esp32Camera 只打日志并让 streaming_on_ = false，
        // > 我们从 esp_camera_fb_get() 只会拿到 NULL，必须自己把这件事说出来。
        if (++preview_fail_streak_ == 30 && preview_status_ != nullptr) {
            lv_label_set_text(preview_status_, "摄像头不可用");
            ESP_LOGW(TAG, "连续 30 次取帧失败，预览页显示“摄像头不可用”");
        }
        return;
    }
    if (preview_fail_streak_ > 0) {
        preview_fail_streak_ = 0;
        if (preview_status_ != nullptr) {
            lv_label_set_text(preview_status_, "");
        }
    }
```

- [ ] **步骤 2：把相机传给 UI**

`esp32s3_board.cc` 里把任务 5 写的那一行：

```cpp
        vehicle_ui_ = new VehicleUi(display_, vehicle_, nullptr);   // 任务 8 会把 camera_ 传进来
```

改成：

```cpp
        vehicle_ui_ = new VehicleUi(display_, vehicle_, camera_capture_);
```

**注意顺序**：`camera_capture_` 必须在 `vehicle_ui_` 之前构造（任务 8 已经把它放在前面），否则传进去的是空指针。

- [ ] **步骤 3：构建 + 真机测帧率（D4 验收）**

构建、烧录，进实时画面页，观察：
1. 画面出现（QVGA 320×240，**不做放大**——设计文档 §8 就是为了省 SPI 带宽），能看出画面在动；
2. 串口每 5 s 一行：

```
I (xxx) VehicleUi: 预览实测 15.3 fps（77 帧 / 5.0 s，丢帧 0，对话中暂停 0 次）
```

3. **判据：实测 ≥10 fps**（`docs/计划书.md` §1.1 的指标）。若达不到：
   - 先看"丢帧"计数是否在涨 → 涨说明取帧跟不上，把 `kPreviewIntervalMs` 从 60 调到 100，并把实测值如实写进验收记录；
   - 丢帧为 0 但 fps 仍低 → 说明 LVGL 重绘（canvas invalidate → SPI flush）是瓶颈，同样降周期或缩小预览区；
   - **把最终采用的周期与实测 fps 都写进验收记录**，不要只报一个"达标"。
4. 唤醒小智说句话，确认预览暂停（"对话中暂停"计数在涨）、回到待机后预览自动恢复。

- [ ] **步骤 4：展示并提交（等用户确认）**

```bash
git add main/boards/esp32s3/vehicle_ui.h main/boards/esp32s3/vehicle_ui.cc main/boards/esp32s3/esp32s3_board.cc
git status --short
git diff --cached --stat
```
建议 commit message：`feat: 实时画面页接入摄像头预览并实测帧率`

---

### 任务 10：局域网 HTTP 三路由（D5 验收项）

**文件：**
- 创建：`main/boards/esp32s3/vehicle_http.h`
- 创建：`main/boards/esp32s3/vehicle_http.cc`
- 修改：`main/boards/esp32s3/esp32s3_board.cc`
- 修改：`main/CMakeLists.txt:910-928`（把 `esp_http_server`、`spiffs` 显式写进 `PRIV_REQUIRES`）

- [ ] **步骤 1：写 `vehicle_http.h`**

```cpp
#pragma once

#include <cstdint>

#include "esp_http_server.h"

class SnapshotStore;

// 局域网 HTTP：手机浏览器看图看事件（设计文档 §7.2）。
//   /            一页静态 HTML（内嵌，无外部依赖）：最近事件 + 一张图
//   /latest.jpg  最近一张抓拍（Cache-Control: no-store）
//   /events      最近 50 条事件，每行一条 JSON（text/plain）
class VehicleHttp {
public:
    explicit VehicleHttp(SnapshotStore *store);
    ~VehicleHttp();

    // 起服务（默认 80 端口）。失败返回 false，只告警不阻断开机。
    bool Start(uint16_t port = 80);
    bool started() const { return server_ != nullptr; }

private:
    static esp_err_t HandleRoot(httpd_req_t *req);
    static esp_err_t HandleLatest(httpd_req_t *req);
    static esp_err_t HandleEvents(httpd_req_t *req);
    static void LogAccessUrl();

    SnapshotStore *store_ = nullptr;
    httpd_handle_t server_ = nullptr;
};
```

- [ ] **步骤 2：写 `vehicle_http.cc`**

```cpp
#include "vehicle_http.h"

#include <string>

#include <esp_log.h>
#include <esp_timer.h>

#include "board.h"
#include "cJSON.h"
#include "snapshot_store.h"

#define TAG "VehicleHttp"

namespace {

constexpr const char *kIndexHtml =
    "<!doctype html><html lang=zh><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>车载终端</title></head><body style='font-family:sans-serif;margin:16px'>"
    "<h2>车载终端</h2>"
    "<p><img src='/latest.jpg' style='max-width:100%;border:1px solid #ccc'></p>"
    "<p><a href='/latest.jpg'>原图</a> · <a href='/events'>事件 JSON</a> · <a href='/'>刷新</a></p>"
    "<h3>最近事件</h3><pre id=e style='white-space:pre-wrap'>加载中…</pre>"
    "<script>fetch('/events').then(r=>r.text()).then(t=>{"
    "document.getElementById('e').textContent=t||'（暂无事件）'})</script>"
    "</body></html>";

void SendText(httpd_req_t *req, const char *type, const std::string &body) {
    httpd_resp_set_type(req, type);
    httpd_resp_send(req, body.data(), static_cast<ssize_t>(body.size()));
}

}  // namespace

VehicleHttp::VehicleHttp(SnapshotStore *store) : store_(store) {
}

VehicleHttp::~VehicleHttp() {
    if (server_ != nullptr) {
        httpd_stop(server_);
        server_ = nullptr;
    }
}

bool VehicleHttp::Start(uint16_t port) {
    if (server_ != nullptr) {
        return true;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;
    // > 这个任务里要读文件（抓拍 JPEG 约 20 KB）并拼 HTML，默认 4096 不够宽裕。
    config.stack_size = 6144;

    if (httpd_start(&server_, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务启动失败（端口 %u 被占用？）", static_cast<unsigned>(port));
        server_ = nullptr;
        return false;
    }

    const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = HandleRoot, .user_ctx = this};
    const httpd_uri_t latest = {.uri = "/latest.jpg", .method = HTTP_GET, .handler = HandleLatest, .user_ctx = this};
    const httpd_uri_t events = {.uri = "/events", .method = HTTP_GET, .handler = HandleEvents, .user_ctx = this};
    httpd_register_uri_handler(server_, &root);
    httpd_register_uri_handler(server_, &latest);
    httpd_register_uri_handler(server_, &events);

    ESP_LOGI(TAG, "HTTP 服务已启动：/  /latest.jpg  /events（端口 %u）", static_cast<unsigned>(port));

    // > 设备 IP 要等 WiFi 连上才知道，这里起一个一次性定时器在 5 s 后把地址打出来，
    // > 免得用户还得去翻 WiFi 日志。IP 从 Board::GetSystemInfoJson() 里取
    // > （main/boards/common/wifi_board.cc:277 把 "ip" 放进了这份 JSON）。
    esp_timer_handle_t timer = nullptr;
    const esp_timer_create_args_t args = {
        .callback = [](void *) { LogAccessUrl(); },
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "vehicle_http_url",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_once(timer, 5 * 1000 * 1000);
    }
    return true;
}

void VehicleHttp::LogAccessUrl() {
    const std::string info = Board::GetInstance().GetSystemInfoJson();
    cJSON *root = cJSON_Parse(info.c_str());
    if (root == nullptr) {
        return;
    }
    const cJSON *ip = cJSON_GetObjectItem(root, "ip");
    if (cJSON_IsString(ip) && ip->valuestring != nullptr && ip->valuestring[0] != '\0') {
        ESP_LOGI(TAG, "手机浏览器打开：http://%s/", ip->valuestring);
    } else {
        ESP_LOGW(TAG, "还没拿到 IP；联网后用串口里 WiFi 打印的 IP 打开 http://<IP>/");
    }
    cJSON_Delete(root);
}

esp_err_t VehicleHttp::HandleRoot(httpd_req_t *req) {
    SendText(req, "text/html; charset=utf-8", kIndexHtml);
    return ESP_OK;
}

esp_err_t VehicleHttp::HandleLatest(httpd_req_t *req) {
    auto *self = static_cast<VehicleHttp *>(req->user_ctx);
    std::string jpeg;
    if (self == nullptr || self->store_ == nullptr || !self->store_->ReadLatestJpeg(jpeg)) {
        httpd_resp_set_status(req, "404 Not Found");
        SendText(req, "text/plain; charset=utf-8", "还没有抓拍");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, jpeg.data(), static_cast<ssize_t>(jpeg.size()));
    return ESP_OK;
}

esp_err_t VehicleHttp::HandleEvents(httpd_req_t *req) {
    auto *self = static_cast<VehicleHttp *>(req->user_ctx);
    std::string body;
    if (self == nullptr || self->store_ == nullptr || !self->store_->ReadRecentEvents(50, body)) {
        SendText(req, "text/plain; charset=utf-8", "");
        return ESP_OK;
    }
    SendText(req, "text/plain; charset=utf-8", body);
    return ESP_OK;
}
```

- [ ] **步骤 3：接线到板级**

`esp32s3_board.cc`：
1. include 增加 `#include "vehicle_http.h"`。
2. 成员区增加 `VehicleHttp* http_ = nullptr;`。
3. 构造函数末尾（`GetBacklight()->RestoreBrightness();` 之前）插入：

```cpp
        // > 局域网看图/看事件。起不来（端口占用等）只告警，其它功能照常。
        http_ = new VehicleHttp(snapshot_store_);
        if (!http_->Start()) {
            ESP_LOGW(TAG, "局域网 HTTP 未启动，其它功能不受影响");
        }
```

- [ ] **步骤 4：把依赖显式写进 CMake**

`main/CMakeLists.txt:910-928` 的 `PRIV_REQUIRES` 末尾追加两行：

```cmake
                        spiffs
                        esp_http_server
```

> 事实：这两个组件在 IDF v5.5.3 里**没有开关、永远编译**，而且实测它们的 include 目录与静态库已经通过传递依赖进了 main 的编译/链接闭包（`build/ldgen_libraries` 里有 `libspiffs.a`、`libesp_http_server.a`）。**所以不写也能编过**——写上是明确依赖，避免上游哪天调整传递依赖时静默断掉。

- [ ] **步骤 5：构建 + 手机验收（D5 验收项）**

```
idf.py reconfigure
idf.py build > build\last_build.log 2>&1
```

烧录后抓串口，预期：

```
I (xxx) VehicleHttp: HTTP 服务已启动：/  /latest.jpg  /events（端口 80）
I (xxx) VehicleHttp: 手机浏览器打开：http://192.168.x.x/
```

验收动作：
1. 手机连同一个 WiFi → 浏览器打开 `http://<IP>/` → 看到一页 HTML，含最近事件列表与一张图；
2. 打开 `http://<IP>/latest.jpg` → 显示最近一张抓拍原图；
3. 点"抓拍"后再刷新 `/latest.jpg` → 图变了（`Cache-Control: no-store` 生效）；
4. 打开 `http://<IP>/events` → 每行一条 JSON，字段为 `seq/type/ts_ms/value`；
5. 还没有任何抓拍时 `/latest.jpg` 返回 **404 + "还没有抓拍"**（不是 500，也不要挂死）。

- [ ] **步骤 6：展示并提交（等用户确认）**

```bash
git add main/boards/esp32s3/vehicle_http.h main/boards/esp32s3/vehicle_http.cc main/boards/esp32s3/esp32s3_board.cc main/CMakeLists.txt
git status --short
git diff --cached --stat
```
建议 commit message：`feat: 新增局域网 HTTP 看图与事件接口`

---

### 任务 11：设置页读数接线 + 四个 MCP 工具

设置页骨架在任务 5 已建好；这一步把读数补齐，并把车辆数据通过 MCP 暴露给小智——这样"车现在什么状态/温度多少"这类问句能由云端大模型回答（这也是 D6/D7 演示脚本里要用到的一条）。

**文件：**
- 修改：`main/boards/esp32s3/esp32s3_board.cc`（`InitializeTools()` 增加 4 个工具）
- 修改：`main/boards/esp32s3/vehicle_ui.cc`（设置页读数补"标定中"标记）

- [ ] **步骤 1：设置页读数补齐**

把 `vehicle_ui.cc` 的 `RefreshSettings()` 整体替换为：

```cpp
void VehicleUi::RefreshSettings() {
    if (settings_lock_btn_label_ == nullptr || lv_screen_active() != pages_[static_cast<int>(Page::kSettings)]) {
        return;
    }
    const vehicle::VehicleStatus status = vehicle_->Status();
    char buf[96];
    snprintf(buf, sizeof(buf), "当前：%s%s", StateText(status.state), status.calibrated ? "" : "（标定中）");
    lv_label_set_text(settings_lock_btn_label_, buf);
}
```

（顺带删掉原来那行占位用的 `(void)settings_axes_label_;`——`settings_axes_label_` 在 `BuildSettings()` 里已经被真正使用，那行是多余的。）

- [ ] **步骤 2：在 `InitializeTools()` 末尾追加四个工具**

在 `main/boards/esp32s3/esp32s3_board.cc` 的 `InitializeTools()`（第 208-264 行）里，`self.led.set_status` 那个 `AddTool(...)` 之后追加：

```cpp
        // > 车辆数据暴露给小智：云端大模型据此回答"车现在什么状态/温度多少/有没有异常"。
        // > 这些回调都在 Application 主任务里跑；vehicle_ / vehicle_ui_ 在构造期就建好了，
        // > 而 MCP 工具只可能在开机完成之后被调用，所以这里直接解引用是安全的。
        mcp_server.AddTool("self.vehicle.status",
            "读取车辆当前状态：行车状态、环境数据（温度/湿度/光照）、事件计数、最近一次事件。",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                const vehicle::VehicleStatus status = vehicle_->Status();
                const vehicle::EnvReading env = vehicle_->env();
                const std::vector<vehicle::EventRecord> history = vehicle_->CopyHistory();

                cJSON* root = cJSON_CreateObject();
                cJSON_AddStringToObject(root, "state", vehicle::ToString(status.state));
                cJSON_AddBoolToObject(root, "calibrated", status.calibrated);
                cJSON_AddNumberToObject(root, "events_total", status.events_total);
                cJSON_AddNumberToObject(root, "ax", status.sample.ax);
                cJSON_AddNumberToObject(root, "ay", status.sample.ay);
                cJSON_AddNumberToObject(root, "az", status.sample.az);

                cJSON* env_json = cJSON_AddObjectToObject(root, "env");
                cJSON_AddBoolToObject(env_json, "valid", env.valid);
                cJSON_AddBoolToObject(env_json, "simulated", env.simulated);
                cJSON_AddNumberToObject(env_json, "temp_c", env.temp_c);
                cJSON_AddNumberToObject(env_json, "humidity_pct", env.humidity_pct);
                cJSON_AddNumberToObject(env_json, "lux", env.lux);
                cJSON_AddStringToObject(env_json, "light_level", vehicle::ToString(vehicle::BucketLight(env.lux)));

                if (!history.empty()) {
                    const vehicle::EventRecord& last = history.back();
                    cJSON* last_json = cJSON_AddObjectToObject(root, "last_event");
                    // ! seq / ts_ms 是 int64，走 double 传（nano printf 不支持 64 位格式，见 BUG-001）
                    cJSON_AddNumberToObject(last_json, "seq", static_cast<double>(last.seq));
                    cJSON_AddStringToObject(last_json, "type", vehicle::EventTypeId(last.event.type));
                    cJSON_AddNumberToObject(last_json, "ts_ms", static_cast<double>(last.event.ts_ms));
                    cJSON_AddNumberToObject(last_json, "value", last.event.value);
                }

                char* text = cJSON_PrintUnformatted(root);
                std::string result = text != nullptr ? text : "{}";
                cJSON_free(text);
                cJSON_Delete(root);
                return result;
            });

        mcp_server.AddTool("self.vehicle.set_page",
            "切换车载屏幕页面。page 取值：home（主页）、preview（实时画面）、events（事件记录）、"
            "settings（设置）、chat（返回小智聊天界面）。",
            PropertyList({
                Property("page", kPropertyTypeString)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                const std::string page = properties["page"].value<std::string>();
                if (page == "chat") {
                    vehicle_ui_->RequestChatScreen();
                } else if (page == "home") {
                    vehicle_ui_->RequestPage(VehicleUi::Page::kHome);
                } else if (page == "preview") {
                    vehicle_ui_->RequestPage(VehicleUi::Page::kPreview);
                } else if (page == "events") {
                    vehicle_ui_->RequestPage(VehicleUi::Page::kEvents);
                } else if (page == "settings") {
                    vehicle_ui_->RequestPage(VehicleUi::Page::kSettings);
                } else {
                    throw std::invalid_argument("page must be one of: home/preview/events/settings/chat");
                }
                return page;
            });

        mcp_server.AddTool("self.vehicle.lock",
            "进入或退出锁车监测模式。locked=true 进入（停车态下立即生效），false 退出。",
            PropertyList({
                Property("locked", kPropertyTypeBoolean)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                const bool locked = properties["locked"].value<bool>();
                vehicle_->RequestLock(locked);
                return locked;
            });

        mcp_server.AddTool("self.vehicle.capture",
            "立即抓拍一张照片并保存到最近抓拍。",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                vehicle_->RequestCapture();
                return true;
            });
```

需要的头文件：`cJSON` 已由 `mcp_server.h` 间接带入（`main/mcp_server.h:14`），保险起见在 include 区显式补齐：

```cpp
#include <vector>

#include "environment_sensor.h"
#include "event_json.h"
```

（`vehicle_ui.h` 已由任务 5 引入；`vehicle_service.h` 原本就有。）

- [ ] **步骤 3：真机验收（设置页 + MCP）**

1. 进设置页：阈值一行与"环境数据源：模拟（本板无温湿度/光照传感器）"都能看清；三轴开关打开后主页出现 `ax .. ay .. az .. g` 且数值随晃动变化；
2. 点"锁车监测" → 状态应变成"锁车监测"（当前是停车态时）；点"解除锁车" → 回到"停车"；
3. 用 MCP 客户端（或小智语音"车现在什么状态"）逐个验证 4 个新工具：
   - `self.vehicle.status` → 返回 JSON，含 `state`/`env`/`events_total`/`last_event`；
   - `self.vehicle.set_page` 传 `preview` → 屏幕切到实时画面页；传 `chat` → 回聊天界面；
   - `self.vehicle.lock` 传 `true`/`false` → 状态跟着变；
   - `self.vehicle.capture` → 串口出现一条 `原因=手动` 的抓拍日志；
   - 传非法 `page`（如 `"foo"`）→ 应返回错误而**不是**崩溃（`std::invalid_argument` 由框架转成工具错误）。
4. 阈值只读展示与实际阈值一致（对照 `main/vehicle/vehicle_types.h:43-58` 的默认值）。

- [ ] **步骤 4：展示并提交（等用户确认）**

```bash
git add main/boards/esp32s3/esp32s3_board.cc main/boards/esp32s3/vehicle_ui.cc
git status --short
git diff --cached --stat
```
建议 commit message：`feat: 设置页读数接线并新增四个车辆 MCP 工具`

---

### 任务 12：D3–D5 真机验收、稳定性复测与验收记录

**文件：**
- 创建：`docs/验收记录/D3-D5-环境与界面与抓拍.md`

- [ ] **步骤 1：跑一遍全部主机测试（回归）**

```powershell
$env:PATH="C:\mingw64\bin;$env:PATH"
g++ -std=c++17 -Wall -Wextra -I main/vehicle test/driving_monitor_test.cc main/vehicle/driving_monitor.cc -o build_host/driving_monitor_test.exe
g++ -std=c++17 -Wall -Wextra -I main/vehicle test/imu_convert_test.cc -o build_host/imu_convert_test.exe
g++ -std=c++17 -Wall -Wextra -I main/vehicle test/event_history_test.cc main/vehicle/event_history.cc -o build_host/event_history_test.exe
g++ -std=c++17 -Wall -Wextra -I main/vehicle test/environment_sim_test.cc main/vehicle/environment_sensor.cc -o build_host/environment_sim_test.exe
g++ -std=c++17 -Wall -Wextra -I main/vehicle test/snapshot_ring_test.cc main/vehicle/snapshot_ring.cc -o build_host/snapshot_ring_test.exe
g++ -std=c++17 -Wall -Wextra -I main/vehicle test/event_json_test.cc main/vehicle/event_json.cc -o build_host/event_json_test.exe
g++ -std=c++17 -Wall -Wextra -I main/vehicle test/event_text_test.cc main/vehicle/event_text.cc main/vehicle/event_json.cc main/vehicle/driving_monitor.cc -o build_host/event_text_test.exe
build_host\driving_monitor_test.exe; build_host\imu_convert_test.exe; build_host\event_history_test.exe; build_host\environment_sim_test.exe; build_host\snapshot_ring_test.exe; build_host\event_json_test.exe; build_host\event_text_test.exe
```

预期：**七套全 `all passed`**。有任何一套挂掉先修，不要带病烧板。

- [ ] **步骤 2：带负载稳定性复测（本轮最该盯的一项）**

BUG-006（触摸 I2C 失败 → 上游 `ESP_ERROR_CHECK` abort 整机）目前只是**硬件侧缓解、证据待补**（`docs/BUGS.md` 的 BUG-006），而本轮新增的负载不小：摄像头预览 + 环境任务 + HTTP 服务 + 事件写盘。所以必须做一次带负载的连续运行：

1. 上电后进实时画面页，保持预览开着；
2. 用 `build/capture_once.ps1` 抓串口（**只开一次端口、不重连**——脚本自己重连会拉 RTS 补一刀复位，见 BUG-005）；
3. 同时用手机反复刷新 `/` 与 `/latest.jpg`，期间点几次抓拍、晃几次板子触发事件；
4. **连续 ≥10 分钟**，记录：
   - 有没有 `ESP_ERROR_CHECK failed` / `abort()` / 重启（判据：串口里没有新的一行 `rst:0x…`）；
   - 预览 fps 的最小值；
   - `SystemInfo` 周期打印里的 Free 内部 RAM 与 Free PSRAM；
5. 若出现 abort：把完整串口片段与 `rst:` 行记进 `docs/BUGS.md` 的 BUG-006（**这一条就是要补的证据**），并按 `systematic-debugging` 往下查；**不要**直接改 `managed_components`。

- [ ] **步骤 3：写验收记录**

创建 `docs/验收记录/D3-D5-环境与界面与抓拍.md`，按 `docs/验收记录/D1-D2-IMU与判定.md` 的既有格式写，必须包含：

1. 头部：日期、仓库/分支/HEAD、固件版本与 ELF SHA256、硬件、串口、证据文件（写清是 `build/` 下哪个日志）；
2. D3 验收：主页显示项逐条打勾（环境三项 / 行车状态 / 事件数 / 联网图标）、事件页翻页实测、**中文逐字显示情况**；
3. D4 验收：预览**实测 fps**（写明采用的定时器周期）、抓拍张数与文件大小、与 `self.camera.take_photo` 共存是否正常；
4. D5 验收：`snapshots` 分区挂载与容量、锁车进入自动抓拍、异常震动抓拍、手机浏览器打开 `/` 与 `/latest.jpg` 的结果、`/events` 内容样例；
5. **主机测试**：七套全绿；
6. **稳定性**：带负载连续运行的时长、是否有 abort、内存实测值；
7. **局限（如实写）**：
   - 环境数据是**模拟源**，不是真实传感器（计划书 §11 已列为已知限制）；
   - 无 RTC → 事件时间只有"开机以来的毫秒"，页面与 JSON 里都不写"今日"；
   - 抓拍分辨率 QVGA 320×240；
   - BUG-006 的证据补齐情况；
8. 结论表（一项一行、✅/⚠️ 明确，⚠️ 的要写清缺什么）。

- [ ] **步骤 4：展示并提交（等用户确认）**

```bash
git add "docs/验收记录/D3-D5-环境与界面与抓拍.md"
git status --short
git diff --cached --stat
```
建议 commit message：`docs: 记录 D3-D5 环境与界面与抓拍验收数据`

---

## 完成后的状态

做完这 12 个任务，计划书的 **D3 / D4 / D5** 到齐：环境数据（模拟源）+ 主页/事件页/设置页/实时画面四个页面 + 摄像头抓拍与预览 + 锁车联动抓拍 + 局域网 HTTP 看图看事件 + 事件落盘（抓拍环形 32 张、事件日志 256 KB 上限）。

**仍然不在本计划范围内**（留给 Plan C，即 D6–D7）：

- **离线命令词**（MultiNet 接入）：`voice_commands.json`、`custom_wake_word.cc` 的 action 派发、`audio_service.cc` 的转发、`sdkconfig.defaults.esp32s3` 的 `CONFIG_USE_CUSTOM_WAKE_WORD` / `CONFIG_SR_MN_CN_MULTINET7_QUANT`（这两个默认值**目前还没写进任何配置文件**，是 Plan C 的第一件事）
- **语音播报**：42 条片段已在 `main/assets/common/`，但代码里还没有任何地方引用它们——**未被引用的片段常量会被 `--gc-sections` 丢掉，app 体积不涨**（设计文档 §6.4 的实测结论）
- **巴法云 MQTT 上报与断网补传**：`bemfa_client.cc`、事件序号幂等键、NVS 游标 `sent_seq`（Plan B 只把事件写进 `/snap/events.log`，不负责发出去）
- **联调、量化指标表、演示视频、操作文档**

**复核这一版计划时，请重点看这五个决定：**

1. **分区表要删两个模型分区**（任务 7）——设计文档写错了，`0xDC0000` 已被占用；删的是从未烧写过的人脸模型分区，偏移一个不动，但**分区表要重刷**。
2. **字体只用主题字体、每秒重贴**（任务 5）——硬编码内置字体会大面积缺字且不报错。
3. **不用 `lv_layer_top()`、`lv_screen_load()` 无本仓先例**（任务 5）——入口按钮改挂聊天界面；页面切换保留回退方案。
4. **不用 `Esp32Camera::Capture()` 做预览/抓拍**（任务 8）——它没有取帧接口且每帧新分配 150 KB；改为自己 `esp_camera_fb_get()` + `fb_count = 2`。
5. **`event_bus` / `env_task` / `camera_task` 三个设计文档里单列的模块被合并**（见开头"与本计划的既定偏离"）——理由是它们各自的纯逻辑已存在或只是薄包装。
