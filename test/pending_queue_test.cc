// 上报积压队列（定长槽位环形缓冲）的主机单元测试
//
// 编译与运行：
//   g++ -std=c++17 -Wall -Wextra -I main/vehicle test/pending_queue_test.cc main/vehicle/pending_queue.cc -o build_host/pending_queue_test.exe
//   build_host\pending_queue_test.exe

#include <cstdio>
#include <string>
#include <vector>

#include "pending_queue.h"

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

int main() {
    printf("pending_queue\n");

    std::vector<char> buffer(3 * PendingQueue::kSlotBytes, 0);
    PendingQueue queue(buffer.data(), 3);
    std::string out;

    CHECK(queue.capacity() == 3 && queue.size() == 0, "初始为空");
    CHECK(!queue.Pop(out), "空队列 Pop 返回 false");

    CHECK(queue.Push("a") && queue.Push("b") && queue.Push("c"), "压入 3 条");
    CHECK(queue.size() == 3 && queue.dropped() == 0, "满 3 条、无丢弃");

    // > 队列满时**丢最旧**（事件日志同理：宁可丢旧，不阻塞采样）
    CHECK(!queue.Push("d"), "满队列 Push 返回 false（已丢最旧）");
    CHECK(queue.size() == 3 && queue.dropped() == 1, "丢 1 条最旧");

    CHECK(queue.Pop(out) && out == "b", "先出 b（a 已被丢）");
    CHECK(queue.Pop(out) && out == "c", "再出 c");
    CHECK(queue.Pop(out) && out == "d", "最后出 d");
    CHECK(!queue.Pop(out), "排空后 Pop 返回 false");

    // 回绕：反复压弹不串槽
    for (int i = 0; i < 10; i++) {
        std::string payload = "e" + std::to_string(i);
        CHECK(queue.Push(payload), "回绕压入");
        CHECK(queue.Pop(out) && out == payload, "回绕弹出内容一致");
    }

    // 超长 payload：截断而不是越界写
    std::string huge(static_cast<size_t>(PendingQueue::kSlotBytes) * 3, 'x');
    CHECK(queue.Push(huge), "超长 payload 压入成功");
    CHECK(queue.Pop(out), "超长 payload 弹出成功");
    CHECK(out.size() == PendingQueue::kSlotBytes - 1, "超长 payload 被截断到 kSlotBytes-1");
    CHECK(out == huge.substr(0, PendingQueue::kSlotBytes - 1), "截断内容正确");

    printf("\n%s（失败 %d）\n", g_failures == 0 ? "全部通过" : "有失败", g_failures);
    return g_failures == 0 ? 0 : 1;
}
