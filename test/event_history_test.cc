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
