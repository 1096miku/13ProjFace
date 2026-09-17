#pragma once

// 车载终端领域类型：只依赖标准库，不引入任何 ESP-IDF 头文件，
// 这样同一份判定逻辑既能跑在设备上，也能在 PC 上用 g++ 做单元测试。

#include <cstdint>

namespace vehicle {

// 驾驶事件类型
enum class EventType : uint8_t {
    kHardAccel = 0,         // 急加速
    kHardBrake,             // 急刹车
    kHardTurn,              // 急转弯
    kBump,                  // 颠簸
    kCrash,                 // 碰撞
    kParked,                // 进入停车
    kMoving,                // 由停转行
    kMotionWhileParked,     // 锁车/停车期间的异常震动（遗留或防盗告警源）
    kCount,
};

const char *ToString(EventType t);

// IMU 一帧采样
struct ImuSample {
    int64_t ts_ms = 0;
    float ax = 0.0f;  // 纵向（车头方向），单位 g
    float ay = 0.0f;  // 横向，单位 g
    float az = 0.0f;  // 垂直（重力方向），单位 g
    float gx = 0.0f;  // 角速度，单位 dps（保留，不参与当前判定）
    float gy = 0.0f;
    float gz = 0.0f;
};

struct Event {
    EventType type = EventType::kCount;
    int64_t ts_ms = 0;
    float value = 0.0f;  // 触发时的加速度值（g），便于记录与展示
};

// 判定阈值与时长：全部可配，便于按实际安装方向与路况现场调整
struct MonitorConfig {
    float accel_threshold = 0.35f;         // 急加速/急刹车触发阈值（g，相对基线）
    float turn_threshold = 0.30f;          // 急转弯触发阈值（g，相对基线）
    float bump_threshold = 1.60f;          // 颠簸触发阈值（g，合成变化量）
    float crash_threshold = 2.50f;         // 碰撞触发阈值（g，合成变化量）
    float static_band = 0.06f;             // 判定"静止"的容差（g）
    float parked_motion_threshold = 0.25f; // 停车/锁车期间的异常震动阈值（g）
    int32_t baseline_samples = 50;         // 静止基线标定所需样本数
    float calib_mag_band = 0.10f;          // 标定时只接受 |a| 落在 1 g ± 该值内的样本（滤掉上电暂态）
    int32_t static_hold_ms = 30000;        // 静止持续多久判定为停车（演示 30 s）
    int32_t lock_hold_ms = 120000;         // 停车后多久自动进入锁车监测（演示 2 min）
    int32_t wake_hold_ms = 2000;           // 锁车态下持续运动多久判定为重新行驶
    int32_t event_cooldown_ms = 3000;      // 同类事件的冷却时间
    int32_t crash_cooldown_ms = 10000;
    int32_t parked_motion_cooldown_ms = 10000;
};

// 车辆状态机
enum class MotionState : uint8_t {
    kUncalibrated = 0,  // 尚未完成静止基线标定
    kParked,            // 停车
    kDriving,           // 行驶
    kLockedMonitor,     // 锁车监测模式
};

const char *ToString(MotionState s);

// 抓拍原因（给 worker 决定"要不要抓、怎么记日志"用；不是判定事件，所以不塞进 EventType）
enum class CaptureReason : uint8_t {
    kCrash = 0,          // 判定到碰撞
    kMotionWhileParked,  // 停车/锁车期间的异常震动
    kLockEntered,        // 刚进入锁车监测模式
    kManual,             // 屏幕按钮 / 语音"重新抓拍"
};

const char *ToString(CaptureReason reason);

// 对外只读快照：UI / 上报层只看这一个结构，不要直接碰 DrivingMonitor。
// 由 imu_task 每帧更新一次，读取方加锁整体拷走，保证"状态 + 采样值 + 计数"互相一致
// （分开读会出现"状态已经是停车、采样值还是上一帧行驶"这类撕裂）。
struct VehicleStatus {
    MotionState state = MotionState::kUncalibrated;
    bool calibrated = false;
    ImuSample sample{};   // 最近一帧有效采样（ts_ms 是它自己的时间戳）
    int32_t events_total = 0;
    int64_t last_event_seq = 0;
};

}  // namespace vehicle
