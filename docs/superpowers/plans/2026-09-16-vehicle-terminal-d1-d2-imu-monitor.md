# 车载终端 D1–D2：构建接线 + QMI8658A 驱动 + 判定内联 实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 把已有但没进构建的 `main/vehicle/` 判定逻辑接进固件，写出可用的 QMI8658A I2C 驱动，并在 `imu_task` 里完成"20 ms 采样 → 内联判定 → 事件落历史与日志"的闭环，达到计划书 D1/D2 的当日验收。

**架构：** 判定逻辑保持纯 C++（无 IDF 依赖，主机 g++ 可测）；设备侧只新增三层——`I2cDevice` 派生的 `Qmi8658a` 寄存器驱动、纯换算头 `imu_convert.h`、以及组装并驱动采样任务的 `VehicleService`。判定内联在 `imu_task`（设计文档 D3），事件只投递到内存历史 + 日志，绝不阻塞采样。

**技术栈：** ESP-IDF v5.5.3（`i2c_master` 新驱动）、C++17、FreeRTOS、主机侧 g++（MinGW `C:\mingw64\bin`）跑纯逻辑测试。

**前置事实（已核对，别再查一遍）：**

- IMU = QMI8658A，I2C 7 位地址 **0x6A**（SDO/SA0 悬空），**INT1/INT2 未接 → 只能轮询**（`11_PCA9577/docs/hw/BOARD_SCH_ANALYSIS.md`）。
- 寄存器口径（两处独立来源一致：[ph-qmi8658 register.rs](https://docs.rs/ph-qmi8658/latest/src/ph_qmi8658/register.rs.html)、[同类板已跑通的 QMI8658A.c](https://gitea.airlabs.art/Rdzleo/Baji_Rtc_Toy/src/commit/2458c4e8bc6fd7bbe61496608bba1c6f5c724dab/main/boards/movecall-moji-esp32s3/QMI8658A/QMI8658A.c)）：`WHO_AM_I=0x00`（期望 0x05）、`CTRL1=0x02`、`CTRL2=0x03`（AFS=bit6:4，AODR=bit3:0）、`CTRL3=0x04`、`CTRL5=0x06`、`CTRL7=0x08`（A_EN=0x01、G_EN=0x02）、`STATUSINT=0x2D`（bit0=Avail、bit1=Locked）、`TEMP_L=0x33`、加速度数据 `0x35–0x3A`、陀螺 `0x3B–0x40`、`RESET=0x60` 写 `0xB0` 复位。
- 换算因子：`raw * full_scale / 32768`（int16 补码）；加速度满量程码：0=±2g、1=±4g、2=±8g、3=±16g；陀螺：0=±16dps…7=±2048dps；ODR 码 0b0110 = 112.1 Hz、0b0011 = 896.8 Hz。
- I2C 基线：`main/boards/esp32s3/config.h` 里 `BOARD_I2C_PORT=I2C_NUM_0`、SDA=GPIO1、SCL=GPIO2；总线句柄由 `Esp32S3Board::InitializeI2c()` 创建（`esp32s3_board.cc:43`）。
- `I2cDevice`（`main/boards/common/i2c_device.h`）提供 `WriteReg/ReadReg/ReadRegs`，但**内部走 `ESP_ERROR_CHECK`**：IMU 不在线会直接 abort。本计划自己实现容错读写。
- `main/vehicle/driving_monitor.{h,cc}`、`vehicle_types.h` 已存在且主机测试通过，**本计划不改它们的判定逻辑**。

**命令备忘：**

- 主机测试（在 `13ProjFace` 根目录，PowerShell；需 `C:\mingw64\bin` 在 PATH）：
  ```
  g++ -std=c++17 -Wall -Wextra -I main/vehicle test/<name>.cc main/vehicle/<impl>.cc -o build_host/<name>.exe
  build_host/<name>.exe
  ```
- 固件构建（Git Bash / MSYS 里必须走 cmd）：
  ```
  cmd //c "set MSYSTEM=&& set IDF_TOOLS_PATH=D:\AAA_Game_XueXiBan\Espressif\tools&& set PATH=D:\AAA_Game_XueXiBan\Espressif\tools\idf-python\3.11.2;%PATH%&& call D:\AAA_Game_XueXiBan\Espressif\frameworks\esp-idf-v5.5.3\export.bat && cd /d D:\vscode\ESP32Project\13ProjFace && idf.py build"
  ```
- **提交纪律（本项目硬规则）**：不要自动 `git commit`。每个任务的"提交"步骤实际执行方式是：`git add <文件>` → 展示 `git status --short` 与 `git diff --cached --stat` → **等用户确认后**再 `git commit`。

---

## 文件结构

| 文件 | 动作 | 职责 |
|---|---|---|
| `main/CMakeLists.txt` | 修改（42、45 行附近） | 把 `vehicle` 加入 `INCLUDE_DIRS`，把两个 `.cc` 加入 `SOURCES` |
| `main/vehicle/imu_convert.h` | 创建 | 纯换算：int16 组装、raw→g/dps、温度。无 IDF 依赖 |
| `test/imu_convert_test.cc` | 创建 | `imu_convert.h` 的主机单元测试 |
| `main/vehicle/event_history.h/.cc` | 创建 | 定长环形事件历史 + 单调序号（纯逻辑） |
| `test/event_history_test.cc` | 创建 | 环形覆盖、序号单调、越界访问的主机测试 |
| `main/boards/esp32s3/qmi8658a.h/.cc` | 创建 | QMI8658A 驱动：容错寄存器读写、Init、ReadSample（三态结果） |
| `main/boards/esp32s3/vehicle_service.h/.cc` | 创建 | 组装 IMU + 配置 + DrivingMonitor + EventHistory，跑 `imu_task` |
| `main/boards/esp32s3/esp32s3_board.cc` | 修改（`InitializeTools` 前） | 构造 `VehicleService`，IMU 失败只告警不阻断开机 |

---

### 任务 1：把 `main/vehicle/` 接进固件构建

**文件：**
- 修改：`main/CMakeLists.txt:42`（`set(INCLUDE_DIRS ...)`）
- 修改：`main/CMakeLists.txt:45-63`（`list(APPEND SOURCES ...)`）

- [ ] **步骤 1：确认当前确实没进构建（记录基线）**

运行：
```powershell
Test-Path build\esp-idf\main\CMakeFiles\__idf_main.dir\vehicle\driving_monitor.cc.obj
```
预期：`False`（说明 `main/vehicle/` 现在一行都没编进固件）

- [ ] **步骤 2：改 `INCLUDE_DIRS`**

把 `main/CMakeLists.txt:42` 改为（只在末尾加 `"vehicle"`）：
```cmake
set(INCLUDE_DIRS "." "display" "display/lvgl_display" "display/lvgl_display/jpg" "audio" "audio/demuxer" "protocols" "vehicle")
```

- [ ] **步骤 3：把 vehicle 源文件加进 `SOURCES`**

在 `main/CMakeLists.txt` 的 `list(APPEND SOURCES "boards/common/board.cc" ...)` 之前插入一段：
```cmake
# 本项目自写的车载业务逻辑（纯 C++，不依赖 ESP-IDF，可用主机 g++ 单测）
list(APPEND SOURCES
    "vehicle/driving_monitor.cc"
    "vehicle/event_history.cc"
)
```

> `event_history.cc` 在任务 3 才创建。如果先做本任务，请把这一行留到任务 3 再加，否则编不过。

- [ ] **步骤 4：构建验证**

运行上面的"固件构建"命令。
预期：`Project build complete.`，且日志里出现 `Building CXX object esp-idf/main/CMakeFiles/__idf_main.dir/vehicle/driving_monitor.cc.obj`。

- [ ] **步骤 5：再次确认产物存在**

运行：
```powershell
Test-Path build\esp-idf\main\CMakeFiles\__idf_main.dir\vehicle\driving_monitor.cc.obj
```
预期：`True`

- [ ] **步骤 6：提交（见头部"提交纪律"）**

```bash
git add main/CMakeLists.txt
git commit -m "build: 把 main/vehicle 判定逻辑接进固件构建"
```

---

### 任务 2：IMU 换算纯逻辑 + 主机测试

**文件：**
- 创建：`main/vehicle/imu_convert.h`
- 测试：`test/imu_convert_test.cc`

- [ ] **步骤 1：编写失败的测试**

创建 `test/imu_convert_test.cc`：
```cpp
// IMU 换算纯逻辑的主机单元测试（不依赖 ESP-IDF）
//
// 编译与运行（Windows / MinGW）：
//   g++ -std=c++17 -Wall -Wextra -I main/vehicle test/imu_convert_test.cc -o build_host/imu_convert_test.exe
//   build_host/imu_convert_test.exe

#include <cmath>
#include <cstdio>

#include "imu_convert.h"

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
    return std::fabs(a - b) < eps;
}

int main() {
    printf("imu_convert\n");

    // 小端组装：低字节在前（QMI8658A 的 AX_L=0x35 在 AX_H=0x36 之前）
    CHECK(AssembleInt16(0x00, 0x01) == 256, "AssembleInt16(0x00,0x01) == 256");
    CHECK(AssembleInt16(0xFF, 0xFF) == -1, "AssembleInt16(0xFF,0xFF) == -1（负值补码）");
    CHECK(AssembleInt16(0x00, 0x80) == -32768, "AssembleInt16(0x00,0x80) == -32768（满量程负端）");
    CHECK(AssembleInt16(0xFF, 0x7F) == 32767, "AssembleInt16(0xFF,0x7F) == 32767（满量程正端）");

    // 换算：raw * full_scale / 32768
    CHECK(Near(RawToAccelG(32767, 8.0f), 7.99976f, 1e-4f), "±8g 满量程正端 ≈ +7.9998 g");
    CHECK(Near(RawToAccelG(-32768, 8.0f), -8.0f), "±8g 满量程负端 == -8 g");
    // 静止 1 g 对应的原始值（±8g 量程）：1 / 8 * 32768 = 4096
    CHECK(Near(RawToAccelG(4096, 8.0f), 1.0f), "±8g 下 raw=4096 → 1.000 g（静止基准）");
    CHECK(Near(RawToGyroDps(32767, 512.0f), 511.98f, 1e-2f), "±512dps 满量程正端 ≈ +512 dps");

    // 温度：整数部分取 TEMP_H（有符号），小数部分 TEMP_L / 256
    CHECK(Near(RawToTemperatureC(0x00, 0x1A), 26.0f), "TEMP_H=26, TEMP_L=0 → 26.00 ℃");
    CHECK(Near(RawToTemperatureC(0x80, 0x1A), 26.5f), "TEMP_L=128 → 26.50 ℃");
    CHECK(Near(RawToTemperatureC(0x00, 0xFF), -1.0f), "TEMP_H=0xFF（-1）→ -1.00 ℃");

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
g++ -std=c++17 -Wall -Wextra -I main/vehicle test/imu_convert_test.cc -o build_host/imu_convert_test.exe
```
预期：编译失败，报 `fatal error: imu_convert.h: No such file or directory`

- [ ] **步骤 3：编写最少实现代码**

创建 `main/vehicle/imu_convert.h`：
```cpp
#pragma once

// QMI8658A 原始值 → 物理量的换算。只依赖标准库，主机可单测；
// 设备侧由 qmi8658a.cc 调用，不在这里引入任何 ESP-IDF 头文件。

#include <cstdint>

namespace vehicle {

// 小端组装两个字节为一个有符号 16 位值（低字节在前）
inline int16_t AssembleInt16(uint8_t low, uint8_t high) {
    return static_cast<int16_t>((static_cast<uint16_t>(high) << 8) | low);
}

// 加速度换算：满量程 full_scale_g（±2/4/8/16 g）→ 单位 g
inline float RawToAccelG(int16_t raw, float full_scale_g) {
    return static_cast<float>(raw) * full_scale_g / 32768.0f;
}

// 陀螺换算：满量程 full_scale_dps（±16…2048 dps）→ 单位 dps
inline float RawToGyroDps(int16_t raw, float full_scale_dps) {
    return static_cast<float>(raw) * full_scale_dps / 32768.0f;
}

// 温度换算：TEMP_H 为有符号整数部分，TEMP_L/256 为小数部分
inline float RawToTemperatureC(uint8_t temp_l, uint8_t temp_h) {
    return static_cast<float>(static_cast<int8_t>(temp_h)) +
           static_cast<float>(temp_l) / 256.0f;
}

}  // namespace vehicle
```

- [ ] **步骤 4：运行测试验证通过**

运行：
```powershell
g++ -std=c++17 -Wall -Wextra -I main/vehicle test/imu_convert_test.cc -o build_host/imu_convert_test.exe
build_host\imu_convert_test.exe
```
预期：逐条 `ok`，最后 `all passed`，退出码 0

- [ ] **步骤 5：提交（见头部"提交纪律"）**

```bash
git add main/vehicle/imu_convert.h test/imu_convert_test.cc
git commit -m "feat: 增加 IMU 原始值换算纯逻辑与主机测试"
```

---

### 任务 3：事件历史环形缓冲 + 主机测试

**文件：**
- 创建：`main/vehicle/event_history.h`
- 创建：`main/vehicle/event_history.cc`
- 测试：`test/event_history_test.cc`
- 修改：`main/CMakeLists.txt`（任务 1 步骤 3 里预留的 `vehicle/event_history.cc`，此时补上）

- [ ] **步骤 1：编写失败的测试**

创建 `test/event_history_test.cc`：
```cpp
// 事件历史环形缓冲的主机单元测试
//
// 编译与运行：
//   g++ -std=c++17 -Wall -Wextra -I main/vehicle test/event_history_test.cc main/vehicle/event_history.cc -o build_host/event_history_test.exe
//   build_host/event_history_test.exe

#include <cstdio>

#include "event_history.h"

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

static Event MakeEvent(EventType type, int64_t ts, float value) {
    Event e;
    e.type = type;
    e.ts_ms = ts;
    e.value = value;
    return e;
}

int main() {
    printf("event_history\n");

    EventHistory h(3);

    CHECK(h.capacity() == 3, "容量为 3");
    CHECK(h.size() == 0, "初始为空");
    CHECK(h.last_seq() == 0, "初始序号为 0");

    int64_t s1 = h.Append(MakeEvent(EventType::kHardAccel, 1000, 0.4f));
    CHECK(s1 == 1, "第一条事件序号从 1 开始");
    CHECK(h.size() == 1, "追加一条后 size == 1");

    h.Append(MakeEvent(EventType::kHardBrake, 2000, -0.5f));
    h.Append(MakeEvent(EventType::kHardTurn, 3000, 0.35f));
    CHECK(h.size() == 3, "写满后 size == 容量");

    const EventRecord* oldest = h.At(0);
    CHECK(oldest != nullptr && oldest->event.type == EventType::kHardAccel, "At(0) 是最旧的一条");
    const EventRecord* newest = h.At(2);
    CHECK(newest != nullptr && newest->event.type == EventType::kHardTurn, "At(2) 是最新的一条");
    CHECK(newest != nullptr && newest->seq == 3, "序号随追加单调递增");

    // 覆盖最旧：追加第 4 条后，At(0) 应变成原来的第 2 条
    int64_t s4 = h.Append(MakeEvent(EventType::kBump, 4000, 1.8f));
    CHECK(s4 == 4, "第 4 条序号为 4");
    CHECK(h.size() == 3, "覆盖后 size 仍等于容量");
    CHECK(h.At(0) != nullptr && h.At(0)->event.type == EventType::kHardBrake, "覆盖后 At(0) 是第 2 条");
    CHECK(h.At(2) != nullptr && h.At(2)->event.type == EventType::kBump, "覆盖后 At(2) 是第 4 条");
    CHECK(h.last_seq() == 4, "last_seq 跟随最新一条");

    CHECK(h.At(-1) == nullptr, "负下标返回 nullptr");
    CHECK(h.At(3) == nullptr, "越界下标返回 nullptr");

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
g++ -std=c++17 -Wall -Wextra -I main/vehicle test/event_history_test.cc main/vehicle/event_history.cc -o build_host/event_history_test.exe
```
预期：编译失败，报 `fatal error: event_history.h: No such file or directory`

- [ ] **步骤 3：编写最少实现代码**

创建 `main/vehicle/event_history.h`：
```cpp
#pragma once

// 事件历史：定长环形缓冲 + 单调递增序号。
// 用途：屏幕事件页回看、巴法云上报的幂等键（device_id + seq）、断网补传游标。
// 纯逻辑，不依赖 ESP-IDF；构造时一次性分配，之后不再分配内存。

#include <cstdint>
#include <vector>

#include "vehicle_types.h"

namespace vehicle {

// 一条事件记录：事件本体 + 全局单调序号
struct EventRecord {
    Event event;
    int64_t seq = 0;  // 从 1 开始
};

class EventHistory {
public:
    explicit EventHistory(int capacity);

    // 追加一条事件，返回分配到的序号；缓冲满时覆盖最旧的一条
    int64_t Append(const Event& event);

    int capacity() const { return capacity_; }
    int size() const { return size_; }
    int64_t last_seq() const { return last_seq_; }

    // index 从 0 = 最旧 开始；越界返回 nullptr
    const EventRecord* At(int index) const;

private:
    int Capacity() const { return capacity_; }

    int capacity_ = 0;
    int size_ = 0;
    int head_ = 0;  // 下一个写入位置
    int64_t last_seq_ = 0;
    std::vector<EventRecord> buffer_;
};

}  // namespace vehicle
```

创建 `main/vehicle/event_history.cc`：
```cpp
#include "event_history.h"

namespace vehicle {

EventHistory::EventHistory(int capacity)
    : capacity_(capacity > 0 ? capacity : 1), buffer_(static_cast<size_t>(capacity > 0 ? capacity : 1)) {
}

int64_t EventHistory::Append(const Event& event) {
    EventRecord& slot = buffer_[static_cast<size_t>(head_)];
    slot.event = event;
    slot.seq = ++last_seq_;
    head_ = (head_ + 1) % capacity_;
    if (size_ < capacity_) {
        size_++;
    }
    return slot.seq;
}

const EventRecord* EventHistory::At(int index) const {
    if (index < 0 || index >= size_) {
        return nullptr;
    }
    // 最旧一条的位置：写满时是 head_，未写满时是 0
    const int oldest = (size_ < capacity_) ? 0 : head_;
    const int pos = (oldest + index) % capacity_;
    return &buffer_[static_cast<size_t>(pos)];
}

}  // namespace vehicle
```

- [ ] **步骤 4：运行测试验证通过**

运行：
```powershell
g++ -std=c++17 -Wall -Wextra -I main/vehicle test/event_history_test.cc main/vehicle/event_history.cc -o build_host/event_history_test.exe
build_host\event_history_test.exe
```
预期：逐条 `ok`，最后 `all passed`，退出码 0

- [ ] **步骤 5：把 `event_history.cc` 加进固件构建**

确认 `main/CMakeLists.txt` 的 `list(APPEND SOURCES ...)` 里已有（任务 1 步骤 3 曾要求延后）：
```cmake
list(APPEND SOURCES
    "vehicle/driving_monitor.cc"
    "vehicle/event_history.cc"
)
```
然后运行固件构建命令，预期出现 `.../vehicle/event_history.cc.obj`。

- [ ] **步骤 6：提交（见头部"提交纪律"）**

```bash
git add main/vehicle/event_history.h main/vehicle/event_history.cc test/event_history_test.cc main/CMakeLists.txt
git commit -m "feat: 增加事件历史环形缓冲与主机测试"
```

---

### 任务 4：QMI8658A I2C 驱动

**文件：**
- 创建：`main/boards/esp32s3/qmi8658a.h`
- 创建：`main/boards/esp32s3/qmi8658a.cc`

> 本任务没有主机测试（真机 I2C 无法在 PC 上跑）。可测的部分已经在任务 2 抽成纯函数；本任务的验收靠步骤 3 的真机串口输出。

- [ ] **步骤 1：编写驱动**

创建 `main/boards/esp32s3/qmi8658a.h`：
```cpp
#pragma once

#include "i2c_device.h"
#include "vehicle_types.h"

// QMI8658A 六轴 IMU 驱动
//
// 硬件事实：I2C 7 位地址 0x6A（SDO/SA0 悬空）；INT1/INT2 未接 → 只能轮询。
// ! 不复用 I2cDevice::ReadReg/ReadRegs：它们内部走 ESP_ERROR_CHECK，
// ! IMU 不在线时会直接 abort 整机。这里自己读写并返回三态结果，让上层能降级。
class Qmi8658a : public I2cDevice {
public:
    static constexpr uint8_t kAddr = 0x6A;
    static constexpr uint8_t kWhoAmIExpected = 0x05;

    // 一次采样的结果
    enum class ReadResult {
        kOk,        // 读到一帧有效数据
        kNotReady,  // 数据尚未就绪（正常情况，不算错误）
        kError,     // I2C 通信失败
    };

    explicit Qmi8658a(i2c_master_bus_handle_t i2c_bus) : I2cDevice(i2c_bus, kAddr) {}

    // 复位 + 配置（±8 g / ±512 dps / 112.1 Hz / LPF 开 / 使能 A+G）
    // 返回 false 表示 IMU 不在线或配置失败，上层应降级而不是重启
    bool Init();

    ReadResult ReadSample(vehicle::ImuSample& out);

private:
    bool WriteChecked(uint8_t reg, uint8_t value);
    bool ReadChecked(uint8_t reg, uint8_t* buffer, size_t length);
};
```

创建 `main/boards/esp32s3/qmi8658a.cc`：
```cpp
#include "qmi8658a.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "imu_convert.h"

#define TAG "Qmi8658a"

namespace {

// 寄存器（见计划文档"前置事实"：两处独立来源已核对）
constexpr uint8_t kRegWhoAmI    = 0x00;
constexpr uint8_t kRegCtrl1     = 0x02;
constexpr uint8_t kRegCtrl2     = 0x03;
constexpr uint8_t kRegCtrl3     = 0x04;
constexpr uint8_t kRegCtrl5     = 0x06;
constexpr uint8_t kRegCtrl7     = 0x08;
constexpr uint8_t kRegStatusInt = 0x2D;
constexpr uint8_t kRegAxL       = 0x35;
constexpr uint8_t kRegReset     = 0x60;

constexpr uint8_t kSoftReset    = 0xB0;
constexpr uint8_t kCtrl1AddrAi  = 0x40;       // ADDR_AI=1（多字节读地址自增）；BE=0 → 小端
constexpr uint8_t kAccelFs8g    = 0x02 << 4;  // AFS=010 → ±8 g
constexpr uint8_t kGyroFs512dps = 0x05 << 4;  // GFS=101 → ±512 dps（本项目不用陀螺，只留档）
constexpr uint8_t kOdr112_1Hz   = 0x06;       // ODR=0110 → 112.1 Hz
constexpr uint8_t kCtrl5LpfOn   = 0x35;       // 加速度 + 陀螺 LPF 使能，模式 3.63%
constexpr uint8_t kEnableAccelGyro = 0x03;    // CTRL7: G_EN | A_EN
constexpr uint8_t kStatusDataAvail = 0x01;

constexpr float kAccelFullScaleG  = 8.0f;
constexpr float kGyroFullScaleDps = 512.0f;

constexpr int kI2cTimeoutMs = 100;

}  // namespace

bool Qmi8658a::WriteChecked(uint8_t reg, uint8_t value) {
    const uint8_t buffer[2] = {reg, value};
    return i2c_master_transmit(i2c_device_, buffer, sizeof(buffer), kI2cTimeoutMs) == ESP_OK;
}

bool Qmi8658a::ReadChecked(uint8_t reg, uint8_t* buffer, size_t length) {
    return i2c_master_transmit_receive(i2c_device_, &reg, 1, buffer, length, kI2cTimeoutMs) == ESP_OK;
}

bool Qmi8658a::Init() {
    uint8_t who = 0;
    if (!ReadChecked(kRegWhoAmI, &who, 1)) {
        ESP_LOGE(TAG, "读 WHO_AM_I 失败：IMU 未响应（确认 0x%02X 在线、I2C 总线已建）", kAddr);
        return false;
    }
    if (who != kWhoAmIExpected) {
        ESP_LOGE(TAG, "WHO_AM_I=0x%02X，期望 0x%02X（可能不是 QMI8658A）", who, kWhoAmIExpected);
        return false;
    }

    // 软复位：写 0xB0 后必须等芯片内部完成，数据手册要求 ≥15 ms，这里给 100 ms 保险
    if (!WriteChecked(kRegReset, kSoftReset)) {
        ESP_LOGE(TAG, "软复位写入失败");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    if (!WriteChecked(kRegCtrl1, kCtrl1AddrAi) ||
        !WriteChecked(kRegCtrl2, kAccelFs8g | kOdr112_1Hz) ||
        !WriteChecked(kRegCtrl3, kGyroFs512dps | kOdr112_1Hz) ||
        !WriteChecked(kRegCtrl5, kCtrl5LpfOn) ||
        !WriteChecked(kRegCtrl7, kEnableAccelGyro)) {
        ESP_LOGE(TAG, "配置寄存器写入失败");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_LOGI(TAG, "初始化完成：WHO_AM_I=0x%02X，±%.0f g / ±%.0f dps / 112.1 Hz", who, kAccelFullScaleG,
             kGyroFullScaleDps);
    return true;
}

Qmi8658a::ReadResult Qmi8658a::ReadSample(vehicle::ImuSample& out) {
    uint8_t status = 0;
    if (!ReadChecked(kRegStatusInt, &status, 1)) {
        return ReadResult::kError;
    }
    if ((status & kStatusDataAvail) == 0) {
        return ReadResult::kNotReady;
    }

    uint8_t raw[12] = {};
    if (!ReadChecked(kRegAxL, raw, sizeof(raw))) {
        return ReadResult::kError;
    }

    out.ts_ms = esp_timer_get_time() / 1000;
    out.ax = vehicle::RawToAccelG(vehicle::AssembleInt16(raw[0], raw[1]), kAccelFullScaleG);
    out.ay = vehicle::RawToAccelG(vehicle::AssembleInt16(raw[2], raw[3]), kAccelFullScaleG);
    out.az = vehicle::RawToAccelG(vehicle::AssembleInt16(raw[4], raw[5]), kAccelFullScaleG);
    out.gx = vehicle::RawToGyroDps(vehicle::AssembleInt16(raw[6], raw[7]), kGyroFullScaleDps);
    out.gy = vehicle::RawToGyroDps(vehicle::AssembleInt16(raw[8], raw[9]), kGyroFullScaleDps);
    out.gz = vehicle::RawToGyroDps(vehicle::AssembleInt16(raw[10], raw[11]), kGyroFullScaleDps);
    return ReadResult::kOk;
}
```

- [ ] **步骤 2：临时接到启动流程做真机验证**

在 `main/boards/esp32s3/esp32s3_board.cc` 顶部加包含：
```cpp
#include "qmi8658a.h"
```
在 `class Esp32S3Board` 的 private 区加成员：
```cpp
    Qmi8658a* imu_ = nullptr;
```
在 `InitializeTools();` 之前插入调用：
```cpp
        InitializeImu();          // 临时：任务 5 会由 VehicleService 接管
```
并新增私有方法（放在 `InitializeTools()` 定义之前）：
```cpp
    void InitializeImu() {
        imu_ = new Qmi8658a(i2c_bus_);
        if (!imu_->Init()) {
            ESP_LOGE(TAG, "IMU 初始化失败，进入降级（不影响其他功能）");
            delete imu_;
            imu_ = nullptr;
            return;
        }
        // 临时自检：每秒打印一次三轴，静止时应 az ≈ 1 g
        xTaskCreate([](void* arg) {
            auto self = static_cast<Esp32S3Board*>(arg);
            vehicle::ImuSample sample;
            while (true) {
                if (self->imu_ != nullptr && self->imu_->ReadSample(sample) == Qmi8658a::ReadResult::kOk) {
                    ESP_LOGI(TAG, "IMU ax=%.3f ay=%.3f az=%.3f g | gx=%.1f gy=%.1f gz=%.1f dps",
                             sample.ax, sample.ay, sample.az, sample.gx, sample.gy, sample.gz);
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }, "imu_selftest", 4096, this, 4, nullptr);
    }
```
> `ESP_LOGE`/`vTaskDelay` 需要 `esp_log.h`、`freertos/FreeRTOS.h`、`freertos/task.h`——`esp32s3_board.cc` 已包含 `esp_log.h`，若编译报未定义再补 `#include <freertos/FreeRTOS.h>` 与 `#include <freertos/task.h>`。

- [ ] **步骤 3：真机验收（本任务的交付物）**

运行（把 `COM5` 换成实际串口）：
```bash
idf.py -p COM5 flash monitor
```
预期（四条，缺一不可）：
1. `I (xxx) Qmi8658a: 初始化完成：WHO_AM_I=0x05，±8 g / ±512 dps / 112.1 Hz`
2. 每秒一行 `IMU ax=... ay=... az=...`，板子平放静止时 `az` 在 **0.97–1.03 g** 之间、`ax/ay` 在 ±0.05 g 内
3. 把板子翻转 180°（屏幕朝下），`az` 变为 **−1 g 附近**
4. 连续 60 s 无 `imu_selftest` 中断、无 `E (xxx) Qmi8658a:` 报错

若步骤 3 的第 1 条打印的是 `WHO_AM_I=0xNN`（NN≠0x05）或读取失败：**不要改判定逻辑**，先按下面顺序排查——
- 用 `i2c_master_probe` 或临时扫描确认 0x6A 是否在线（同总线上 0x19/0x18/0x41/0x38 应都能找到）；
- 确认 `CTRL1` 写的是 `0x40` 而不是带 `BE=0x20` 的值（BE 会改变多字节读的字节序，导致数值离谱而不是读取失败）；
- 若仍失败，改用组件方案兜底：`idf.py add-dependency "waveshare/qmi8658^2.0.0"`，或照抄上面链接那份驱动的完整初始化序列（含 CTRL9 的 AHB 时钟门控握手）。

- [ ] **步骤 4：提交（见头部"提交纪律"）**

```bash
git add main/boards/esp32s3/qmi8658a.h main/boards/esp32s3/qmi8658a.cc main/boards/esp32s3/esp32s3_board.cc
git commit -m "feat: 增加 QMI8658A IMU 驱动并通过真机静止/翻转验收"
```

---

### 任务 5：`VehicleService`——采样 + 判定内联 + 事件落地

**文件：**
- 创建：`main/boards/esp32s3/vehicle_service.h`
- 创建：`main/boards/esp32s3/vehicle_service.cc`
- 修改：`main/boards/esp32s3/esp32s3_board.cc`（去掉任务 4 的临时自检，改由 `VehicleService` 接管）

- [ ] **步骤 1：编写 `vehicle_service.h`**

```cpp
#pragma once

#include <mutex>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "driving_monitor.h"
#include "event_history.h"
#include "qmi8658a.h"
#include "vehicle_types.h"

// 车载业务服务：IMU 采样任务 + 行车判定 + 事件历史。
//
// ! 判定内联在采样任务里（设计文档 D3）：Feed() 是纯计算、µs 级，
// ! 同一时间基准，省一个任务和一层样本队列。
// ! 采样任务里只做「读 → 判定 → 入历史 → 打日志」，绝不调用 MQTT/UI/播报。
class VehicleService {
public:
    explicit VehicleService(i2c_master_bus_handle_t i2c_bus);

    // 初始化 IMU 并启动 imu_task；IMU 不在线时返回 false（调用方降级，不阻断开机）
    bool Start();

    vehicle::MotionState state() const { return monitor_.state(); }
    bool calibrated() const { return monitor_.calibrated(); }

    // 取最近一帧采样（UI 显示三轴实时值用）
    vehicle::ImuSample latest_sample() const;

    // 只读访问事件历史（屏幕事件页 / 上报层用）
    const vehicle::EventHistory& history() const { return history_; }

private:
    static void ImuTaskEntry(void* arg);
    void ImuTaskLoop();
    void LogEvent(const vehicle::EventRecord& record);

    static constexpr int kSamplePeriodMs = 20;      // 50 Hz
    static constexpr int kTaskStackBytes = 4096;
    static constexpr int kMaxErrorStreak = 50;      // 连续 50 次（≈1 s）I2C 失败则重新初始化
    static constexpr int kHistoryCapacity = 64;

    Qmi8658a imu_;
    vehicle::MonitorConfig config_;
    vehicle::DrivingMonitor monitor_;
    vehicle::EventHistory history_;

    mutable std::mutex sample_mutex_;
    vehicle::ImuSample latest_{};

    TaskHandle_t task_ = nullptr;
    bool calibration_logged_ = false;
};
```

- [ ] **步骤 2：编写 `vehicle_service.cc`**

```cpp
#include "vehicle_service.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "VehicleService"

VehicleService::VehicleService(i2c_master_bus_handle_t i2c_bus)
    : imu_(i2c_bus), config_(), monitor_(config_), history_(kHistoryCapacity) {
}

bool VehicleService::Start() {
    if (task_ != nullptr) {
        return true;
    }
    if (!imu_.Init()) {
        ESP_LOGE(TAG, "IMU 初始化失败，行车监测不启动（其余功能照常）");
        return false;
    }

    // ! 栈从 PSRAM 出（与上游 custom_wake_word.cc 的唤醒词编码任务同法），
    // ! 内部 RAM 只留给控制块；本项目内部 RAM 本来就紧。
    StackType_t* stack = static_cast<StackType_t*>(heap_caps_malloc(kTaskStackBytes, MALLOC_CAP_SPIRAM));
    StaticTask_t* tcb = static_cast<StaticTask_t*>(heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL));
    if (stack == nullptr || tcb == nullptr) {
        ESP_LOGE(TAG, "采样任务栈分配失败（PSRAM 余量不足？）");
        heap_caps_free(stack);
        heap_caps_free(tcb);
        return false;
    }

    task_ = xTaskCreateStaticPinnedToCore(ImuTaskEntry, "imu_task", kTaskStackBytes, this, 5, stack, tcb, 0);
    ESP_LOGI(TAG, "imu_task 已启动：%d ms 周期（%d Hz）", kSamplePeriodMs, 1000 / kSamplePeriodMs);
    return task_ != nullptr;
}

void VehicleService::ImuTaskEntry(void* arg) {
    static_cast<VehicleService*>(arg)->ImuTaskLoop();
}

void VehicleService::ImuTaskLoop() {
    const TickType_t period = pdMS_TO_TICKS(kSamplePeriodMs);
    TickType_t last_wake = xTaskGetTickCount();
    int error_streak = 0;
    vehicle::ImuSample sample;

    while (true) {
        const Qmi8658a::ReadResult result = imu_.ReadSample(sample);
        if (result == Qmi8658a::ReadResult::kOk) {
            error_streak = 0;
            {
                std::lock_guard<std::mutex> lock(sample_mutex_);
                latest_ = sample;
            }

            monitor_.Feed(sample);

            if (!calibration_logged_ && monitor_.calibrated()) {
                calibration_logged_ = true;
                ESP_LOGI(TAG, "静止基线标定完成：ax=%.3f ay=%.3f az=%.3f g", monitor_.baseline_ax(),
                         monitor_.baseline_ay(), monitor_.baseline_az());
            }

            vehicle::Event event;
            while (monitor_.PopEvent(event)) {
                history_.Append(event);
                const vehicle::EventRecord* record = history_.At(history_.size() - 1);
                if (record != nullptr) {
                    LogEvent(*record);
                }
            }
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

void VehicleService::LogEvent(const vehicle::EventRecord& record) {
    ESP_LOGI(TAG, "事件 #%lld %s value=%.2f ts=%lld ms", static_cast<long long>(record.seq),
             vehicle::ToString(record.event.type), record.event.value,
             static_cast<long long>(record.event.ts_ms));
}

vehicle::ImuSample VehicleService::latest_sample() const {
    std::lock_guard<std::mutex> lock(sample_mutex_);
    return latest_;
}
```

- [ ] **步骤 3：自查事件循环的索引写法**

`EventHistory::At()` 的下标是"从最旧数起"，所以刚追加的那条是 `size() - 1`，不是 `size()`。追加后确认：
```powershell
Select-String -Path main\boards\esp32s3\vehicle_service.cc -Pattern "history_\.At\("
```
预期：只有一行，参数是 `history_.size() - 1`。

- [ ] **步骤 4：把 `VehicleService` 接进板级**

在 `main/boards/esp32s3/esp32s3_board.cc` 里：
1. 删除任务 4 添加的 `InitializeImu()` 方法、`imu_` 成员、`#include "qmi8658a.h"` 与 `InitializeImu();` 调用（避免两处都在读同一颗 IMU）。
2. 顶部改为 `#include "vehicle_service.h"`。
3. private 区加成员：
```cpp
    VehicleService* vehicle_ = nullptr;
```
4. 构造函数里，在 `InitializeTools();` 之后、`GetBacklight()->RestoreBrightness();` 之前插入：
```cpp
        vehicle_ = new VehicleService(i2c_bus_);
        if (!vehicle_->Start()) {
            ESP_LOGW(TAG, "行车监测未启动（IMU 不在线），屏幕与语音功能不受影响");
        }
```

- [ ] **步骤 5：构建验证**

运行"固件构建"命令。
预期：`Project build complete.`，且日志里出现
`Building CXX object esp-idf/main/CMakeFiles/__idf_main.dir/boards/esp32s3/vehicle_service.cc.obj`
板级文件零 warning（注意 `-Wunused-private-field`）。

- [ ] **步骤 6：真机验收（本任务的交付物）**

运行：
```bash
idf.py -p COM5 flash monitor
```
预期：
1. `VehicleService: imu_task 已启动：20 ms 周期（50 Hz）`
2. 上电静止约 1 s 后：`VehicleService: 静止基线标定完成：ax=±0.xx ay=±0.xx az≈1.00 g`
3. 手持板子快速向前推一下 → 打印 `事件 #1 kHardAccel value=0.4x`；向后拽 → `kHardBrake`；左右快速甩 → `kHardTurn`；敲桌面 → `kBump`
4. 静置 `static_hold_ms`（默认 30 s）→ 打印 `kParked`；再动一下 → `kMoving`
5. 连续运行 5 分钟无 `E (xxx)` 级别报错、无重启

- [ ] **步骤 7：提交（见头部"提交纪律"）**

```bash
git add main/boards/esp32s3/vehicle_service.h main/boards/esp32s3/vehicle_service.cc main/boards/esp32s3/esp32s3_board.cc
git commit -m "feat: imu_task 内联行车判定，事件落历史并输出日志"
```

---

### 任务 6：D1/D2 验收记录（计划书 §12 当日验收）

**文件：**
- 创建：`docs/验收记录/D1-D2-IMU与判定.md`

- [ ] **步骤 1：跑完计划书 D1/D2 的验收口径**

D1：串口打印三轴原始值与换算后的 g 值，静止时 `az ≈ 1 g`（任务 4 步骤 3 已验证）。
D2：四类事件各做 10 次，统计识别率 ≥90%；主机单测全绿。

主机单测一次跑全（PowerShell，逐条执行）：
```powershell
g++ -std=c++17 -Wall -Wextra -I main/vehicle test/driving_monitor_test.cc main/vehicle/driving_monitor.cc -o build_host/driving_monitor_test.exe
g++ -std=c++17 -Wall -Wextra -I main/vehicle test/imu_convert_test.cc -o build_host/imu_convert_test.exe
g++ -std=c++17 -Wall -Wextra -I main/vehicle test/event_history_test.cc main/vehicle/event_history.cc -o build_host/event_history_test.exe
build_host\driving_monitor_test.exe
build_host\imu_convert_test.exe
build_host\event_history_test.exe
```
预期：三个 exe 全部以退出码 0 结束并打印 `all passed`（`driving_monitor_test` 打印它自己的全绿输出）。

- [ ] **步骤 2：把实测数据写进记录文件**

创建 `docs/验收记录/D1-D2-IMU与判定.md`，至少包含：

```markdown
# D1–D2 验收记录（IMU 驱动 + 行车判定）

- 日期：____（填真机实测当天）
- 固件版本：`idf.py` 输出的 `App "xiaozhi" version: 2.2.4` + 提交号：____
- 硬件：自定义 ESP32-S3 开发板，IMU QMI8658A @0x6A

## 1. D1：IMU 驱动

| 项 | 期望 | 实测 |
|---|---|---|
| WHO_AM_I | 0x05 | ____ |
| 静止 az | 0.97–1.03 g | ____ |
| 静止 ax/ay | ±0.05 g | ____ |
| 翻转 180° 后 az | ≈ −1 g | ____ |
| 连续 60 s 读取错误 | 0 次 | ____ |

## 2. D2：四类事件识别（各 10 次）

| 事件 | 成功次数 | 识别率 | 备注 |
|---|---|---|---|
| 急加速 | __/10 | __% | |
| 急刹车 | __/10 | __% | |
| 急转弯 | __/10 | __% | |
| 颠簸 | __/10 | __% | |
| 碰撞（≥2.5 g） | 不做实测 | — | 手持无法稳定复现且有损坏风险，仅主机单测覆盖 |

## 3. 主机单元测试

| 测试 | 结果 |
|---|---|
| driving_monitor_test | PASS/FAIL |
| imu_convert_test | PASS/FAIL |
| event_history_test | PASS/FAIL |

## 4. 异常与处置

（记录现场遇到的偏差，例如阈值需要调整到多少、环境噪声影响等）
```

- [ ] **步骤 3：提交（见头部"提交纪律"）**

```bash
git add "docs/验收记录/D1-D2-IMU与判定.md"
git commit -m "docs: 记录 D1-D2 IMU 与行车判定验收数据"
```

---

## 自检

**1. 规格覆盖度**（对照 `docs/superpowers/specs/2026-09-16-vehicle-terminal-design.md`）

| 规格条目 | 覆盖任务 |
|---|---|
| §4 构建接线（INCLUDE_DIRS + vehicle 源文件） | 任务 1、任务 3 步骤 5 |
| §3.3 IMU 驱动（I2C 初始化、50 Hz 突发读、g/dps 换算） | 任务 2（换算）、任务 4（驱动） |
| §2 D3 判定内联在 `imu_task` | 任务 5 |
| §3.1 纪律：采样任务只读/判定/入队，不碰阻塞接口 | 任务 5 步骤 2 的注释与实现 |
| §3.1 栈从 PSRAM 出 | 任务 5 步骤 2（`heap_caps_malloc(MALLOC_CAP_SPIRAM)`） |
| §5.3 事件历史（供事件页与上报游标） | 任务 3 |
| §9 IMU 初始化失败降级、连续 1 s 失败重初始化 | 任务 4 步骤 3、任务 5 步骤 2 |
| §10.1 主机测试（`imu_convert`、`event_history`） | 任务 2、任务 3、任务 6 |
| §10.2 真机验收 1（I2C 在线、az≈1 g、翻转） | 任务 4 步骤 3 |
| §10.3 碰撞不做手持实测 | 任务 6 步骤 2 的记录表 |
| §5.1 `snapshots` 分区、§6 语音、§7 网络、§8 UI | **不在本计划**，属计划 B/C |

**2. 占位符扫描**：无"待定/TODO/后续实现/类似任务 N"类占位。每个代码步骤都给了可直接粘贴的完整代码；每个验证步骤都给了可直接执行的命令与预期输出。（初稿曾在任务 5 留过一处"故意写错、要求执行者改对"的代码，已改为正确实现，并保留一条索引写法的自查步骤。）

**3. 类型一致性核对**：

- `vehicle::EventHistory::Append(const Event&) -> int64_t`、`At(int) -> const EventRecord*`、`last_seq()`、`size()`、`capacity()`：任务 3 定义，任务 5 使用一致。
- `vehicle::ImuSample` 字段 `ts_ms/ax/ay/az/gx/gy/gz`：与 `main/vehicle/vehicle_types.h` 现有定义一致（已读文件核对）。
- `DrivingMonitor` 接口 `Feed(const ImuSample&)`、`PopEvent(Event&)`、`state()`、`calibrated()`、`baseline_ax/ay/az()`：与 `main/vehicle/driving_monitor.h` 现有定义一致（已读文件核对）。
- `Qmi8658a::ReadResult{kOk,kNotReady,kError}`：任务 4 定义，任务 5 使用一致。
- `Qmi8658a::kAddr/kWhoAmIExpected`：任务 4 定义并在 `Init()` 内使用一致。
- `VehicleService::Start()/state()/calibrated()/latest_sample()/history()`：任务 5 定义；任务 5 步骤 4 的板级调用只用 `Start()`，其余留给计划 B/C，未提前引用未定义符号。
