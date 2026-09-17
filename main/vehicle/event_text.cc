#include "event_text.h"

#include <cstdio>

#include "event_json.h"   // 复用 ToDecimal（nano printf 不支持 64 位格式）

namespace vehicle {

std::string FormatUptime(int64_t ms) {
    const int64_t value = ms > 0 ? ms : 0;
    char buf[32];
    if (value < 60000) {
        // > 直接拼成 "12.3s"：先写 "12.3 s" 再删掉空格，避免两处格式串不一致。
        snprintf(buf, sizeof(buf), "%d.%d s", static_cast<int>(value / 1000),
                 static_cast<int>((value % 1000) / 100));
    } else {
        snprintf(buf, sizeof(buf), "%dm%02ds", static_cast<int>(value / 60000),
                 static_cast<int>((value % 60000) / 1000));
    }
    std::string out(buf);
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
