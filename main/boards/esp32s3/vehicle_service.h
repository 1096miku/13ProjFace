#pragma once

#include <mutex>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "driving_monitor.h"
#include "event_history.h"
#include "qmi8658a.h"
#include "vehicle_types.h"

// 车载业务服务：IMU 采样任务 + 行车判定 + 事件历史。
//
// ! 判定内联在采样任务里（设计文档 D3）：Feed() 是纯计算、µs 级，
// ! 同一时间基准，省一个任务和一层样本队列。
// ! 采样任务里只做「读 → 判定 → 入历史 → 打日志」，绝不调用 MQTT/UI/播报。
class VehicleService {
public:
    explicit VehicleService(i2c_master_bus_handle_t i2c_bus);

    // 初始化 IMU 并启动 imu_task；IMU 不在线时返回 false（调用方降级，不阻断开机）
    bool Start();

    vehicle::MotionState state() const { return monitor_.state(); }
    bool calibrated() const { return monitor_.calibrated(); }

    // 取最近一帧采样（UI 显示三轴实时值用）
    vehicle::ImuSample latest_sample() const;

    // 只读访问事件历史（屏幕事件页 / 上报层用）
    const vehicle::EventHistory& history() const { return history_; }

private:
    static void ImuTaskEntry(void* arg);
    void ImuTaskLoop();
    void LogEvent(const vehicle::EventRecord& record);

    static constexpr int kSamplePeriodMs = 20;      // 50 Hz
    // ! kTaskStackBytes 是"字节"，不是 FreeRTOS 的 ulStackDepth（那是 StackType_t 字数）；
    // ! 传给 xTaskCreateStaticPinnedToCore 时必须除以 sizeof(StackType_t)，见 Start()。
    static constexpr int kTaskStackBytes = 4096;
    static constexpr int kMaxErrorStreak = 50;      // 连续 50 次（≈1 s）I2C 失败则重新初始化
    static constexpr int kHistoryCapacity = 64;

    Qmi8658a imu_;
    vehicle::MonitorConfig config_;
    vehicle::DrivingMonitor monitor_;
    vehicle::EventHistory history_;

    mutable std::mutex sample_mutex_;
    vehicle::ImuSample latest_{};

    TaskHandle_t task_ = nullptr;
    bool calibration_logged_ = false;
};
