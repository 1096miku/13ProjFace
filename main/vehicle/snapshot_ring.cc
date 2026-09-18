#include "snapshot_ring.h"

#include <cstdio>

namespace vehicle {

SnapshotRing::SnapshotRing(int capacity) : capacity_(capacity > 0 ? capacity : 1) {
}

int SnapshotRing::NextSlot() {
    const int slot = next_;
    next_ = (next_ + 1) % capacity_;
    latest_slot_ = slot;
    written_++;
    return slot;
}

std::string SnapshotRing::FileNameFor(int slot) const {
    if (slot < 0 || slot >= capacity_) {
        return std::string();
    }
    // ! 缓冲区要按 int 的最坏情况留：`%d` 最多 11 字节（含负号），"snap_" 5 + 序号 11 + ".jpg" 4 + '\0' = 21。
    // ! 留 16 会被 ESP-IDF 的 -Werror=format-truncation 直接拦成编译错误。
    char name[24];
    // > 文件名固定 3 位序号（snap_000.jpg … snap_031.jpg），spiffs 里按名排序即可按时间读。
    snprintf(name, sizeof(name), "snap_%03d.jpg", slot);
    return std::string(name);
}

std::string SnapshotRing::LatestIndexContent() const {
    if (latest_slot_ < 0) {
        return std::string();
    }
    char buf[16];   // 同上：`%d` + '\n' 最坏 13 字节
    snprintf(buf, sizeof(buf), "%d\n", latest_slot_);
    return std::string(buf);
}

}  // namespace vehicle
