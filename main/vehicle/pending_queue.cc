#include "pending_queue.h"

#include <cstring>

namespace vehicle {

PendingQueue::PendingQueue(char *buffer, int capacity) : buffer_(buffer), capacity_(capacity > 0 ? capacity : 0) {
}

bool PendingQueue::Push(const std::string &payload) {
    if (buffer_ == nullptr || capacity_ == 0) {
        return false;
    }
    bool accepted = true;
    if (size_ == capacity_) {
        // > 丢最旧：把读指针往前挪一格，覆盖那条
        head_ = (head_ + 1) % capacity_;
        size_--;
        dropped_++;
        accepted = false;
    }
    const size_t limit = kSlotBytes - 1;
    const size_t len = payload.size() < limit ? payload.size() : limit;
    const int tail = (head_ + size_) % capacity_;
    char *slot = Slot(tail);
    // > 槽位前两字节放长度（小端），后面跟内容：与 NUL 结尾相比，payload 里出现 '\0'
    // > （理论上不会有）也不会截断。
    const uint16_t stored = static_cast<uint16_t>(len);
    slot[0] = static_cast<char>(stored & 0xFF);
    slot[1] = static_cast<char>((stored >> 8) & 0xFF);
    memcpy(slot + 2, payload.data(), len);
    size_++;
    return accepted;
}

bool PendingQueue::Pop(std::string &out) {
    if (buffer_ == nullptr || size_ == 0) {
        return false;
    }
    const char *slot = Slot(head_);
    const uint16_t len = static_cast<uint16_t>(static_cast<unsigned char>(slot[0])) |
                         static_cast<uint16_t>(static_cast<unsigned char>(slot[1]) << 8);
    out.assign(slot + 2, len);
    head_ = (head_ + 1) % capacity_;
    size_--;
    return true;
}

}  // namespace vehicle
