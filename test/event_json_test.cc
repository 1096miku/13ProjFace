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
