#pragma once

// 事件 → JSON：`/snap/events.log` 每行一条，也是 Plan C 巴法云 MQTT 的 payload（同构，别写两份）。
// 纯逻辑，不依赖 ESP-IDF。

#include <cstdint>
#include <string>

#include "event_history.h"
#include "environment_sensor.h"
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
// 无 device_id 时 = **落盘格式**（events.log / /events，与 D5 逐字节一致）；
// 给了 device_id 时 = **上报格式**：多出 "dev" 与 "boot"。
// 幂等键 = dev + boot + seq（seq 每次重启从 1 开始，只用 dev+seq 会把不同开机的同号事件判成同一条）。
std::string EventToJson(const EventRecord &record, const std::string &device_id = std::string(), int32_t boot_id = 0);

// 从一行事件 JSON 里取 "seq" 的值；取不到返回 -1。
// ! 只认 EventToJson() 写出的固定字段顺序，不做通用 JSON 解析（省一个 cJSON 依赖、可主机测）。
int64_t ParseSeqFromJsonLine(const std::string &line);

// 给一行**落盘格式**的事件 JSON 补上 dev/boot（开机回填补传时用）：
//   {"seq":12,"type":"bump",…} → {"seq":12,"dev":"A1B2…","boot":3,"type":"bump",…}
// ! 只认 EventToJson() 写出的固定格式；已经有 dev 或解析不出 seq 时原样返回。
std::string AddReportFields(const std::string &line, const std::string &device_id, int32_t boot_id);

// 运动状态 → 稳定 ASCII 标识（中文只给屏幕用；手机端/云端要能稳定解析）
const char *MotionStateId(MotionState state);

// 周期上报的 payload（设计文档 §7.1，字段顺序固定）
//   {"ts_ms":123456,"temp":26.4,"humid":48.0,"lux":320,"src":"sim"}
std::string EnvToJson(const EnvReading &reading);
//   {"ts_ms":0,"state":"parked","net":"online","events":3,"boot":7}
std::string StatusToJson(const VehicleStatus &status, bool online, int32_t boot_id);

}  // namespace vehicle
