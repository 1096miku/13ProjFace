#pragma once

// 行车状态判定：纯逻辑，无硬件依赖，可用 g++ 在主机上单元测试。
// 输入是 IMU 采样流，输出是事件流 + 车辆状态机。
//
// 判定口径（对应计划书 §4）：
//   1. 先做静止基线标定（默认 50 帧），此后所有阈值都判定在"相对基线的变化量"上
//   2. 纵向变化 → 急加速 / 急刹车；横向变化 → 急转弯
//   3. 合成变化量 → 颠簸（中） / 碰撞（大，并触发抓拍）
//   4. 三个轴的变化量都落在静止带内并持续 static_hold_ms → 停车；否则 → 行驶
//   5. 停车持续 lock_hold_ms → 自动进入锁车监测模式；锁车态下的震动 → 异常震动事件
//   6. 同类事件有冷却时间，避免一次动作被反复计数

#include "vehicle_types.h"

namespace vehicle {

class DrivingMonitor {
public:
    explicit DrivingMonitor(const MonitorConfig &config);

    // 喂入一帧 IMU 采样。标定期内不产生任何事件。
    void Feed(const ImuSample &sample);

    // 取出一条待处理事件，无事件时返回 false
    bool PopEvent(Event &out);

    MotionState state() const { return state_; }
    bool calibrated() const { return calibrated_; }
    float baseline_ax() const { return baseline_ax_; }
    float baseline_ay() const { return baseline_ay_; }
    float baseline_az() const { return baseline_az_; }

    // 手动锁车 / 解锁（屏幕按钮或语音"锁车"）；锁车请求在停车态下立即生效
    void RequestLock(bool locked);
    bool lock_requested() const { return lock_requested_; }

    // 统计与重置
    int32_t EventCount(EventType type) const;
    int32_t TotalEventCount() const;
    void ResetCounters();

private:
    void FinishCalibration();
    void Emit(EventType type, int64_t ts_ms, float value);
    bool CooldownElapsed(EventType type, int64_t ts_ms) const;
    int32_t CooldownFor(EventType type) const;
    void EnterState(MotionState next);

    MonitorConfig cfg_;
    MotionState state_ = MotionState::kUncalibrated;
    bool calibrated_ = false;
    bool lock_requested_ = false;

    // 基线标定累加
    int32_t calib_count_ = 0;
    double acc_ax_ = 0.0;
    double acc_ay_ = 0.0;
    double acc_az_ = 0.0;
    float baseline_ax_ = 0.0f;
    float baseline_ay_ = 0.0f;
    float baseline_az_ = 0.0f;

    // 停车/锁车计时
    int64_t static_since_ms_ = 0;   // 连续静止起始时刻；0 表示未在静止
    int64_t parked_since_ms_ = 0;   // 进入停车的时刻
    int64_t motion_since_ms_ = 0;   // 锁车态下连续运动的起始时刻

    int64_t last_emit_ms_[static_cast<int>(EventType::kCount)] = {0};
    int32_t counts_[static_cast<int>(EventType::kCount)] = {0};

    // 事件队列（环形）
    static constexpr int kQueueSize = 32;
    Event queue_[kQueueSize];
    int head_ = 0;
    int tail_ = 0;
};

}  // namespace vehicle
