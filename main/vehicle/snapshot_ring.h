#pragma once

// 抓拍环形存储的**索引**逻辑：只回答"下一张写哪个槽位、最新一张是谁、文件名是什么"，
// 不碰文件系统（挂载与读写是设备侧的 SnapshotStore，见 main/boards/esp32s3/snapshot_store.cc）。
// 纯逻辑，不依赖 ESP-IDF。

#include <cstdint>
#include <string>

namespace vehicle {

class SnapshotRing {
public:
    explicit SnapshotRing(int capacity);

    // 取下一个写入槽位（capacity() 个槽位循环使用），并把"最新槽位"推进到它
    int NextSlot();

    // 最近一次 NextSlot 得到的槽位；从未写入过返回 -1
    int latest_slot() const { return latest_slot_; }
    int capacity() const { return capacity_; }
    int written() const { return written_; }

    // 槽位 → 文件名（"snap_007.jpg"）；越界返回空串
    std::string FileNameFor(int slot) const;

    // latest.idx 的内容（"7\n"）；从未写入过返回空串（调用方据此跳过写文件）
    std::string LatestIndexContent() const;

private:
    int capacity_ = 1;
    int next_ = 0;
    int latest_slot_ = -1;
    int written_ = 0;
};

}  // namespace vehicle
