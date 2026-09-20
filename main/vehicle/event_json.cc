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

std::string EventToJson(const EventRecord &record, const std::string &device_id, int32_t boot_id) {
    char value_buf[16];
    snprintf(value_buf, sizeof(value_buf), "%.2f", static_cast<double>(record.event.value));

    std::string out;
    out.reserve(device_id.empty() ? 96 : 96 + device_id.size());
    out += "{\"seq\":";
    out += ToDecimal(record.seq);
    if (!device_id.empty()) {
        out += ",\"dev\":\"";
        out += device_id;
        out += "\",\"boot\":";
        out += ToDecimal(boot_id);
    }
    out += ",\"type\":\"";
    out += EventTypeId(record.event.type);
    out += "\",\"ts_ms\":";
    out += ToDecimal(record.event.ts_ms);
    out += ",\"value\":";
    out += value_buf;
    out += "}";
    return out;
}

int64_t ParseSeqFromJsonLine(const std::string &line) {
    const std::string key = "\"seq\":";
    const size_t pos = line.find(key);
    if (pos == std::string::npos) {
        return -1;
    }
    size_t i = pos + key.size();
    bool negative = false;
    if (i < line.size() && line[i] == '-') {
        negative = true;
        i++;
    }
    if (i >= line.size() || line[i] < '0' || line[i] > '9') {
        return -1;
    }
    int64_t value = 0;
    while (i < line.size() && line[i] >= '0' && line[i] <= '9') {
        value = value * 10 + (line[i] - '0');
        i++;
    }
    return negative ? -value : value;
}

std::string AddReportFields(const std::string &line, const std::string &device_id, int32_t boot_id) {
    if (device_id.empty() || line.find("\"dev\"") != std::string::npos || ParseSeqFromJsonLine(line) < 0) {
        return line;
    }
    const size_t comma = line.find(',', line.find("\"seq\":"));
    if (comma == std::string::npos) {
        return line;
    }
    std::string out;
    out.reserve(line.size() + device_id.size() + 24);
    out += line.substr(0, comma);
    out += ",\"dev\":\"";
    out += device_id;
    out += "\",\"boot\":";
    out += ToDecimal(boot_id);
    out += line.substr(comma);
    return out;
}

const char *MotionStateId(MotionState state) {
    switch (state) {
        case MotionState::kUncalibrated: return "uncalibrated";
        case MotionState::kParked: return "parked";
        case MotionState::kDriving: return "driving";
        case MotionState::kLockedMonitor: return "locked";
    }
    return "unknown";
}

std::string EnvToJson(const EnvReading &reading) {
    char number_buf[48];
    std::string out = "{\"ts_ms\":";
    out += ToDecimal(reading.ts_ms);
    snprintf(number_buf, sizeof(number_buf), ",\"temp\":%.1f,\"humid\":%.1f,\"lux\":%d",
             static_cast<double>(reading.temp_c), static_cast<double>(reading.humidity_pct),
             static_cast<int>(reading.lux));
    out += number_buf;
    out += ",\"src\":\"";
    out += reading.simulated ? "sim" : "sensor";
    out += "\"}";
    return out;
}

std::string StatusToJson(const VehicleStatus &status, bool online, int32_t boot_id) {
    std::string out = "{\"ts_ms\":";
    out += ToDecimal(status.sample.ts_ms);
    out += ",\"state\":\"";
    out += MotionStateId(status.state);
    out += "\",\"net\":\"";
    out += online ? "online" : "offline";
    out += "\",\"events\":";
    out += ToDecimal(status.events_total);
    out += ",\"boot\":";
    out += ToDecimal(boot_id);
    out += "}";
    return out;
}

}  // namespace vehicle
