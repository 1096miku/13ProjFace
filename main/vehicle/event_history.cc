#include "event_history.h"

namespace vehicle {

EventHistory::EventHistory(int capacity)
    : capacity_(capacity > 0 ? capacity : 1), buffer_(static_cast<size_t>(capacity > 0 ? capacity : 1)) {
}

int64_t EventHistory::Append(const Event& event) {
    EventRecord& slot = buffer_[static_cast<size_t>(head_)];
    slot.event = event;
    slot.seq = ++last_seq_;
    head_ = (head_ + 1) % capacity_;
    if (size_ < capacity_) {
        size_++;
    }
    return slot.seq;
}

const EventRecord* EventHistory::At(int index) const {
    if (index < 0 || index >= size_) {
        return nullptr;
    }
    // 最旧一条的位置：写满时是 head_，未写满时是 0
    const int oldest = (size_ < capacity_) ? 0 : head_;
    const int pos = (oldest + index) % capacity_;
    return &buffer_[static_cast<size_t>(pos)];
}

}  // namespace vehicle
