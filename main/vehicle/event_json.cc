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
