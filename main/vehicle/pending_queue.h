#pragma once

// 定长槽位环形队列：断网积压的事件 payload 放这里。
//
// ! 为什么不用 std::deque<std::string>：500 条 × 约 80 B ≈ 40 KB，而 std::string/deque
// ! 走的是默认分配器（内部堆）。本板通用内部堆只有 22 KB 且空载就 99.7% 满（BUG-024 补充），
// ! 一压就 ENOMEM。所以缓冲由**调用方**提供——设备侧显式 heap_caps_malloc(MALLOC_CAP_SPIRAM)。
// 纯逻辑（不分配内存、不依赖 ESP-IDF），主机可测。

#include <cstddef>
#include <cstdint>
#include <string>

namespace vehicle {

class PendingQueue {
public:
    // > 一条 payload 的上限：事件 JSON 约 80 B，留一倍余量。
    static constexpr size_t kSlotBytes = 160;

    // buffer 至少要 capacity * kSlotBytes 字节，构造后由本对象独占
    PendingQueue(char *buffer, int capacity);

    // 压入一条。队列满时**丢最旧**并返回 false；payload 超长则截断。
    bool Push(const std::string &payload);
    // 弹出一条（FIFO）。空队列返回 false。
    bool Pop(std::string &out);

    int size() const { return size_; }
    int capacity() const { return capacity_; }
    int dropped() const { return dropped_; }

private:
    char *Slot(int index) const { return buffer_ + static_cast<size_t>(index) * kSlotBytes; }

    char *buffer_ = nullptr;
    int capacity_ = 0;
    int head_ = 0;   // 下一个读的位置
    int size_ = 0;
    int dropped_ = 0;
};

}  // namespace vehicle
