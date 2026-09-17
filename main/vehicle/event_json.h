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
