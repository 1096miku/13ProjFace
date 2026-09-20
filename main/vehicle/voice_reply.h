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
