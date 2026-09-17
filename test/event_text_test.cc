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
