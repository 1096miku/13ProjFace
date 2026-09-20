#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "driving_monitor.h"
#include "environment_sensor.h"
#include "event_history.h"
#include "qmi8658a.h"
#include "vehicle_types.h"

// 事件消费者：worker_task 里调用，**可以慢**（写盘、发网络），绝不会阻塞采样。
// 落盘（SnapshotStore）与上报（Plan C 的 BemfaClient）各实现一个。
class EventSink {
public:
    virtual ~EventSink() = default;
    virtual void OnEvent(const vehicle::EventRecord &record) = 0;
    // 默认空实现：只有需要抓拍的消费者（相机）才关心
    virtual void OnCaptureRequest(vehicle::CaptureReason reason, int64_t ts_ms) {
        (void)reason;
        (void)ts_ms;
    }
};

// worker 任务每轮调用一次的执行器（Plan C 的语音命令用它把"执行"从音频任务挪到 worker 任务）。
class WorkerTickable {
public:
    virtual ~WorkerTickable() = default;
    virtual void ExecutePending() = 0;
};

// 车载业务服务：IMU 采样任务 + 行车判定 + 事件历史 + 慢活 worker。
//
// ! 分工（设计文档 D3 + 本次扩展）：
// !   imu_task ：读 IMU → Feed 判定 → 入历史 → 投事件队列。只做纯计算与内存操作，
// !              绝不写盘/抓拍/上报/碰 UI —— 它一卡，采样就丢，事件就漏。
// !   worker_task：排空事件队列 → 分发给 EventSink；1 Hz 读环境传感器；
// !              收到抓拍请求（碰撞 / 锁车期震动 / 进入锁车 / 手动）时发 OnCaptureRequest。
class VehicleService {
public:
    explicit VehicleService(i2c_master_bus_handle_t i2c_bus);

    // 注册事件消费者（最多 kMaxSinks 个）。必须在 Start() 之前调用。
    void AddEventSink(EventSink *sink);

    // 初始化 IMU 并启动 imu_task + worker_task；IMU 不在线时返回 false（调用方降级，不阻断开机）
    bool Start();

    // 只读快照（UI / MCP 工具用）。加锁整体拷走，不要试图拿引用。
    vehicle::VehicleStatus Status() const;

    // 最近环境读数（worker_task 每秒更新一次）
    vehicle::EnvReading env() const;

    // 事件历史的一份拷贝（屏幕事件页用；index 0 = 最旧）
    std::vector<vehicle::EventRecord> CopyHistory() const;

    // 阈值配置的一份拷贝（设置页只读展示用）
    vehicle::MonitorConfig config() const { return config_; }
    // 事件历史容量（事件页显示"共 N 条 / 容量 M"）
    int history_capacity() const { return history_.capacity(); }

    // 手动锁车 / 解锁（屏幕按钮、MCP 工具、Plan C 的语音"锁车"）。
    // ! 只是记一个请求，真正的状态切换发生在 imu_task 里，避免跨任务写 DrivingMonitor。
    void RequestLock(bool locked);

    // 手动抓拍（屏幕按钮 / 语音"重新抓拍"），由 worker_task 执行
    void RequestCapture();

    // 语音命令执行器（Plan C）：每轮 worker 循环调用一次，由它在**worker 任务**里执行
    // 需要访问 flash 或会阻塞的动作。可以是 nullptr（未接线时什么也不做）。
    void SetCommandExecutor(WorkerTickable *executor) { executor_ = executor; }

private:
    static void ImuTaskEntry(void *arg);
    static void WorkerTaskEntry(void *arg);
    void ImuTaskLoop();
    void WorkerTaskLoop();
    void PublishStatus(const vehicle::ImuSample &sample);
    void DispatchEvent(const vehicle::EventRecord &record);
    void LogEvent(const vehicle::EventRecord &record);

    static constexpr int kSamplePeriodMs = 20;      // 50 Hz
    // ! kTaskStackBytes 是"字节"，不是 FreeRTOS 的 ulStackDepth（那是 StackType_t 字数）；
    // ! 传给 xTaskCreateStaticPinnedToCore 时必须除以 sizeof(StackType_t)，见 Start()。
    static constexpr int kTaskStackBytes = 4096;
    // > worker 只要 8 KB 的一半：真机上每次抓拍都打一条 `worker 栈余量 6088 B`
    // > （build/acceptance_d4e.log），即最坏用掉约 2.1 KB。这里缩到 6144 B，
    // > 把省下的 2 KB 内部 RAM 还给系统 —— 那块内存的低水位只有 1003 B（同日志的
    // > `minimal sram`，出现在小智拍照的 JPEG 编码 + 上传瞬间）。
    static constexpr int kWorkerStackBytes = 6144;  // 写盘 + 环境读取，6 KB（栈必须在内部 RAM，见 BUG-024）
    static constexpr int kMaxErrorStreak = 50;      // 连续 50 次（≈1 s）I2C 失败则重新初始化
    static constexpr int kHistoryCapacity = 64;
    static constexpr int kEventQueueSize = 32;      // 与 DrivingMonitor 内部队列同量级
    static constexpr int kMaxSinks = 6;
    static constexpr int kEnvPeriodLoops = 5;       // worker 每 200 ms 一轮 → 5 轮 = 1 s
    static constexpr uint32_t kWorkerPeriodMs = 200;

    Qmi8658a imu_;
    vehicle::MonitorConfig config_;
    vehicle::DrivingMonitor monitor_;
    vehicle::EventHistory history_;
    vehicle::SimulatedSensor env_sensor_;   // ! 目前只有模拟源；接上真实传感器后换成 I2cEnvSensor

    mutable std::mutex status_mutex_;       // 保护 status_
    mutable std::mutex history_mutex_;      // 保护 history_ 与 env_
    vehicle::VehicleStatus status_{};
    vehicle::EnvReading env_{};

    // 跨任务请求：用 tri-state 原子量，-1 = 无请求，0 = 解锁，1 = 锁车
    std::atomic<int> lock_request_{-1};
    // 抓拍请求：-1 = 无请求；否则是 CaptureReason 的值（原因由请求方决定，worker 不猜）
    std::atomic<int> capture_request_{-1};

    EventSink *sinks_[kMaxSinks] = {};
    int sink_count_ = 0;
    WorkerTickable *executor_ = nullptr;

    QueueHandle_t event_queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    TaskHandle_t worker_task_ = nullptr;
    bool calibration_logged_ = false;
};
