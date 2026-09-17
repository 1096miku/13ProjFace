#include "driving_monitor.h"

#include <cmath>

namespace vehicle {

const char *ToString(EventType t) {
    switch (t) {
    case EventType::kHardAccel: return "急加速";
    case EventType::kHardBrake: return "急刹车";
    case EventType::kHardTurn: return "急转弯";
    case EventType::kBump: return "颠簸";
    case EventType::kCrash: return "碰撞";
    case EventType::kParked: return "停车";
    case EventType::kMoving: return "行驶";
    case EventType::kMotionWhileParked: return "异常震动";
    default: return "未知";
    }
}

const char *ToString(MotionState s) {
    switch (s) {
    case MotionState::kUncalibrated: return "标定中";
    case MotionState::kParked: return "停车";
    case MotionState::kDriving: return "行驶";
    case MotionState::kLockedMonitor: return "锁车监测";
    default: return "未知";
    }
}

const char *ToString(CaptureReason reason) {
    switch (reason) {
    case CaptureReason::kCrash: return "碰撞";
    case CaptureReason::kMotionWhileParked: return "异常震动";
    case CaptureReason::kLockEntered: return "进入锁车监测";
    case CaptureReason::kManual: return "手动";
    default: return "未知";
    }
}

DrivingMonitor::DrivingMonitor(const MonitorConfig &config) : cfg_(config) {
    if (cfg_.baseline_samples < 1) {
        cfg_.baseline_samples = 1;
    }
}

void DrivingMonitor::FinishCalibration() {
    baseline_ax_ = static_cast<float>(acc_ax_ / calib_count_);
    baseline_ay_ = static_cast<float>(acc_ay_ / calib_count_);
    baseline_az_ = static_cast<float>(acc_az_ / calib_count_);
    calibrated_ = true;
    // 标定完成时默认处于停车态（设备刚通电、车未动）
    state_ = MotionState::kParked;
    parked_since_ms_ = 0;
}

void DrivingMonitor::Emit(EventType type, int64_t ts_ms, float value) {
    Event e;
    e.type = type;
    e.ts_ms = ts_ms;
    e.value = value;

    int next = (tail_ + 1) % kQueueSize;
    if (next == head_) {
        // 队列满：丢弃最旧的一条，保证新事件不丢
        head_ = (head_ + 1) % kQueueSize;
    }
    queue_[tail_] = e;
    tail_ = next;

    const int idx = static_cast<int>(type);
    last_emit_ms_[idx] = ts_ms;
    counts_[idx]++;
}

bool DrivingMonitor::PopEvent(Event &out) {
    if (head_ == tail_) {
        return false;
    }
    out = queue_[head_];
    head_ = (head_ + 1) % kQueueSize;
    return true;
}

int32_t DrivingMonitor::CooldownFor(EventType type) const {
    switch (type) {
    case EventType::kCrash: return cfg_.crash_cooldown_ms;
    case EventType::kMotionWhileParked: return cfg_.parked_motion_cooldown_ms;
    default: return cfg_.event_cooldown_ms;
    }
}

bool DrivingMonitor::CooldownElapsed(EventType type, int64_t ts_ms) const {
    const int idx = static_cast<int>(type);
    if (counts_[idx] == 0) {
        return true;
    }
    return (ts_ms - last_emit_ms_[idx]) >= CooldownFor(type);
}

void DrivingMonitor::EnterState(MotionState next) {
    state_ = next;
}

void DrivingMonitor::RequestLock(bool locked) {
    lock_requested_ = locked;
    if (locked && state_ == MotionState::kParked) {
        EnterState(MotionState::kLockedMonitor);
        motion_since_ms_ = 0;
    } else if (!locked && state_ == MotionState::kLockedMonitor) {
        // 解锁回到停车态，等待运动或再次锁车
        EnterState(MotionState::kParked);
        parked_since_ms_ = 0;
        static_since_ms_ = 0;
    }
}

void DrivingMonitor::Feed(const ImuSample &sample) {
    if (!calibrated_) {
        // ! QMI8658A 使能后有约 340 ms 的内部建立暂态：真机实测 |a| 会先冲到 2.09 g，
        // ! 再经 1.76 → 1.23 → 0.84 → 0.89 → 0.98 回落到 1.00 g。基线取的是上电后头 50 帧，
        // ! 若把暂态算进去，基线本身就是歪的（实测 |a| 只剩 0.75 g），后果是静止时误报
        // ! 急加速/急转弯、而且三个轴都超出静止带 → 永远判不出"停车"。
        // > 只累加 |a| 落在 1 g 附近的样本（条件驱动，不硬编码延时），顺带也排除掉
        // > "上电那一秒板子正被拿在手里"的情况。
        const float mag = std::sqrt(sample.ax * sample.ax + sample.ay * sample.ay + sample.az * sample.az);
        if (std::fabs(mag - 1.0f) > cfg_.calib_mag_band) {
            return;
        }
        acc_ax_ += sample.ax;
        acc_ay_ += sample.ay;
        acc_az_ += sample.az;
        calib_count_++;
        if (calib_count_ >= cfg_.baseline_samples) {
            FinishCalibration();
        }
        return;
    }

    const float dx = sample.ax - baseline_ax_;
    const float dy = sample.ay - baseline_ay_;
    const float dz = sample.az - baseline_az_;
    const float mag = std::sqrt(dx * dx + dy * dy + dz * dz);

    // ── 事件判定（与状态无关，碰撞/颠簸在任何状态下都要报）──
    if (mag >= cfg_.crash_threshold) {
        if (CooldownElapsed(EventType::kCrash, sample.ts_ms)) {
            Emit(EventType::kCrash, sample.ts_ms, mag);
        }
    } else if (mag >= cfg_.bump_threshold) {
        if (CooldownElapsed(EventType::kBump, sample.ts_ms)) {
            Emit(EventType::kBump, sample.ts_ms, mag);
        }
    } else {
        if (dx >= cfg_.accel_threshold && CooldownElapsed(EventType::kHardAccel, sample.ts_ms)) {
            Emit(EventType::kHardAccel, sample.ts_ms, dx);
        }
        if (dx <= -cfg_.accel_threshold && CooldownElapsed(EventType::kHardBrake, sample.ts_ms)) {
            Emit(EventType::kHardBrake, sample.ts_ms, dx);
        }
        if (std::fabs(dy) >= cfg_.turn_threshold && CooldownElapsed(EventType::kHardTurn, sample.ts_ms)) {
            Emit(EventType::kHardTurn, sample.ts_ms, dy);
        }
    }

    // ── 静止判定 ──
    const bool is_static =
        std::fabs(dx) < cfg_.static_band && std::fabs(dy) < cfg_.static_band && std::fabs(dz) < cfg_.static_band;
    if (is_static) {
        if (static_since_ms_ == 0) {
            static_since_ms_ = sample.ts_ms;
        }
    } else {
        static_since_ms_ = 0;
    }

    // ── 状态机 ──
    switch (state_) {
    case MotionState::kParked:
    case MotionState::kUncalibrated:
        if (!is_static) {
            // 由静转动：先报"行驶"，再按需重新计时
            if (state_ == MotionState::kParked) {
                Emit(EventType::kMoving, sample.ts_ms, mag);
            }
            EnterState(MotionState::kDriving);
            parked_since_ms_ = 0;
        } else if (state_ == MotionState::kParked && cfg_.lock_hold_ms > 0 && parked_since_ms_ != 0 &&
                   (sample.ts_ms - parked_since_ms_) >= cfg_.lock_hold_ms) {
            EnterState(MotionState::kLockedMonitor);
            motion_since_ms_ = 0;
        }
        break;

    case MotionState::kDriving:
        if (is_static && static_since_ms_ != 0 && (sample.ts_ms - static_since_ms_) >= cfg_.static_hold_ms) {
            Emit(EventType::kParked, sample.ts_ms, mag);
            EnterState(MotionState::kParked);
            parked_since_ms_ = static_since_ms_ + cfg_.static_hold_ms;
            // 若此前已有锁车请求，停车后立即进入锁车监测
            if (lock_requested_) {
                EnterState(MotionState::kLockedMonitor);
                motion_since_ms_ = 0;
            }
        }
        break;

    case MotionState::kLockedMonitor:
        // > 异常震动告警：动态合成量超过阈值就报（受冷却约束），与"要不要唤醒"无关。
        if (mag >= cfg_.parked_motion_threshold && CooldownElapsed(EventType::kMotionWhileParked, sample.ts_ms)) {
            Emit(EventType::kMotionWhileParked, sample.ts_ms, mag);
        }
        // ! 唤醒判据必须与"停车 → 行驶"一致，用"不再静止"，**不能**用 mag ≥ parked_motion_threshold：
        // ! 后者要求连续 wake_hold_ms（默认 2 s）的动态量都超过 0.25 g，而真机行驶的动态量是断续的
        // ! （多数帧很小，只有起步/换挡偶尔过峰），只要有一帧低于门限就把计时清零 → 车开走了
        // ! 状态机仍锁在"锁车监测"、回不到行驶。2026-09-17 真机实测到的问题，见 docs/BUGS.md BUG-020。
        if (!is_static) {
            if (motion_since_ms_ == 0) {
                motion_since_ms_ = sample.ts_ms;
            }
            // 持续不静止达到 wake_hold_ms：判定为重新行驶并自动解锁
            if (cfg_.wake_hold_ms > 0 && (sample.ts_ms - motion_since_ms_) >= cfg_.wake_hold_ms) {
                Emit(EventType::kMoving, sample.ts_ms, mag);
                EnterState(MotionState::kDriving);
                lock_requested_ = false;
                motion_since_ms_ = 0;
                static_since_ms_ = 0;
                parked_since_ms_ = 0;
            }
        } else {
            motion_since_ms_ = 0;
        }
        break;
    }

    // 停车计时：只要处于静止且不在行驶态，就推进 parked_since
    if (is_static && state_ == MotionState::kParked && parked_since_ms_ == 0) {
        parked_since_ms_ = static_since_ms_;
    }
}

int32_t DrivingMonitor::EventCount(EventType type) const {
    return counts_[static_cast<int>(type)];
}

int32_t DrivingMonitor::TotalEventCount() const {
    int32_t total = 0;
    for (int i = 0; i < static_cast<int>(EventType::kCount); i++) {
        total += counts_[i];
    }
    return total;
}

void DrivingMonitor::ResetCounters() {
    for (int i = 0; i < static_cast<int>(EventType::kCount); i++) {
        counts_[i] = 0;
        last_emit_ms_[i] = 0;
    }
}

}  // namespace vehicle
