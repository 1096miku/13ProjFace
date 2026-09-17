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
