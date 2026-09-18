// 抓拍环形索引的主机单元测试
//
// 编译与运行：
//   g++ -std=c++17 -Wall -Wextra -I main/vehicle test/snapshot_ring_test.cc main/vehicle/snapshot_ring.cc -o build_host/snapshot_ring_test.exe
//   build_host/snapshot_ring_test.exe

#include <cstdio>
#include <string>

#include "snapshot_ring.h"

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
    printf("snapshot_ring\n");

    SnapshotRing ring(3);
    CHECK(ring.capacity() == 3, "容量为 3");
    CHECK(ring.written() == 0, "初始未写过");
    CHECK(ring.latest_slot() == -1, "初始没有最新槽位");
    CHECK(ring.LatestIndexContent().empty(), "未写过时 latest.idx 内容为空");

    CHECK(ring.NextSlot() == 0, "第 1 张写槽位 0");
    CHECK(ring.NextSlot() == 1, "第 2 张写槽位 1");
    CHECK(ring.NextSlot() == 2, "第 3 张写槽位 2");
    CHECK(ring.latest_slot() == 2, "最新槽位为 2");
    CHECK(ring.NextSlot() == 0, "第 4 张回绕到槽位 0");
    CHECK(ring.latest_slot() == 0, "回绕后最新槽位为 0");
    CHECK(ring.written() == 4, "写入计数累计");

    CHECK(ring.FileNameFor(0) == "snap_000.jpg", "槽位 0 → snap_000.jpg");
    CHECK(ring.FileNameFor(3).empty(), "越界槽位返回空串");
    CHECK(ring.FileNameFor(-1).empty(), "负槽位返回空串");

    // ! 3 位序号的补零要靠容量足够大的环来验：容量 3 的环里槽位 7 本来就是越界的。
    SnapshotRing full(32);
    CHECK(full.FileNameFor(7) == "snap_007.jpg", "槽位 7 → snap_007.jpg");
    CHECK(full.FileNameFor(31) == "snap_031.jpg", "槽位 31 → snap_031.jpg");

    CHECK(ring.LatestIndexContent() == "0\n", "latest.idx 内容是槽位号 + 换行");

    // 容量非法时按 1 处理（与 EventHistory 同风格：构造时一次性定死，之后不分配）
    SnapshotRing one(0);
    CHECK(one.capacity() == 1, "容量 0 被夹到 1");
    CHECK(one.NextSlot() == 0 && one.NextSlot() == 0, "容量 1 时永远写同一个槽位");

    if (g_failures == 0) {
        printf("all passed\n");
        return 0;
    }
    printf("%d failure(s)\n", g_failures);
    return 1;
}
