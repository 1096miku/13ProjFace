#pragma once

// 事件历史：定长环形缓冲 + 单调递增序号。
// 用途：屏幕事件页回看、巴法云上报的幂等键（device_id + seq）、断网补传游标。
// 纯逻辑，不依赖 ESP-IDF；构造时一次性分配，之后不再分配内存。

#include <cstdint>
#include <vector>

#include "vehicle_types.h"

namespace vehicle {

// 一条事件记录：事件本体 + 全局单调序号
struct EventRecord {
    Event event;
    int64_t seq = 0;  // 从 1 开始
};

class EventHistory {
public:
    explicit EventHistory(int capacity);

    // 追加一条事件，返回分配到的序号；缓冲满时覆盖最旧的一条
    int64_t Append(const Event& event);

    int capacity() const { return capacity_; }
    int size() const { return size_; }
    int64_t last_seq() const { return last_seq_; }

    // index 从 0 = 最旧 开始；越界返回 nullptr
    const EventRecord* At(int index) const;

private:
    int capacity_ = 0;
    int size_ = 0;
    int head_ = 0;  // 下一个写入位置
    int64_t last_seq_ = 0;
    std::vector<EventRecord> buffer_;
};

}  // namespace vehicle
