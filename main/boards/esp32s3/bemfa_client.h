#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "mqtt.h"
#include "pending_queue.h"
#include "vehicle_service.h"   // EventSink
#include "vehicle_types.h"

class SnapshotStore;
class VehicleService;

// 巴法云 MQTT 上报（Plan C / D6）。
//
// ! 三条硬约束（每条都有 BUGS 里的依据）：
// !   1. 主题名必须先在巴法云控制台建好，且只允许字母/数字 —— 设计文档 §7.1 的
// !      `vehicle/{device_id}/event` 在控制台建不出来（BUG-036）。
// !   2. net_task 的栈放 **PSRAM**（省内部 RAM），所以这个任务**一次都不读 flash**：
// !      游标落 NVS 由 worker 任务在 OnEvent() 里代劳（BUG-024/026）。
// !   3. 积压队列的 80 KB 缓冲显式 `MALLOC_CAP_SPIRAM`：内部通用堆只有 22 KB
// !      且空载就 99.7% 满（BUG-024 补充），std::deque<std::string> 一压就 ENOMEM。
class BemfaClient : public EventSink {
public:
    BemfaClient(VehicleService *vehicle, SnapshotStore *store);
    ~BemfaClient();

    // 建 net_task、起 MQTT 客户端；凭据缺失时返回 false（只告警，不阻断开机）。
    // ! 必须在网络初始化之后调用（板级 StartNetwork），且调用方栈要在**内部 RAM**：
    // ! 这里会读 events.log 做开机回填、还会写 NVS 记 boot_id。
    bool Start();

    // EventSink：只入队 + 顺手持久化游标（本函数运行在 worker 任务里，栈在内部 RAM）
    void OnEvent(const vehicle::EventRecord &record) override;

    struct Stats {
        uint32_t published = 0;   // publish 返回成功（= 已交给 esp-mqtt，不等于已被对端收到）
        uint32_t failed = 0;      // 未连接或 publish 失败
        uint32_t dropped = 0;     // 队列满丢最旧
        int32_t queued = 0;
        bool connected = false;
        bool enabled = false;     // 凭据缺失时为 false
    };
    Stats stats() const;
    std::string StatsJson() const;   // 给 MCP 工具 self.vehicle.net

    bool enabled() const { return enabled_; }
    bool connected() const { return connected_.load(); }

private:
    static void NetTaskEntry(void *arg);
    void NetLoop();
    bool TryConnect();
    void DrainQueue();
    bool Publish(const std::string &topic, const std::string &payload, int qos);
    void PublishEnv();
    void PublishStatus();
    void PersistCursor();   // ! 只能在栈位于内部 RAM 的任务里调（写 NVS）
    static std::string DeviceId();

    static constexpr int kNetStackBytes = 6144;
    static constexpr size_t kQueueCapacity = 500;
    static constexpr uint32_t kLoopPeriodMs = 200;
    static constexpr int64_t kEnvPeriodMs = 30000;
    static constexpr int64_t kStatusPeriodMs = 60000;
    // > 未连接时自己的兜底重试间隔。见 bemfa_client.cc 里 TryConnect() 的注释：
    // > 板级 StartNetwork() 是"异步连 WiFi"，第一次 Connect() 往往发生在拿到 IP 之前
    // > （DNS 直接失败），而只靠 esp-mqtt 的自动重连实测不够快（实测 30 s 内没再试）。
    static constexpr int64_t kConnectRetryMs = 15000;
    static constexpr int kEventQos = 1;   // ! 巴法云不支持 QoS2（会被强制下线）
    static constexpr int kPeriodicQos = 0;

    VehicleService *vehicle_ = nullptr;
    SnapshotStore *store_ = nullptr;

    std::unique_ptr<Mqtt> mqtt_;
    std::unique_ptr<vehicle::PendingQueue> queue_;
    char *queue_buffer_ = nullptr;
    mutable std::mutex mutex_;          // 保护 counters_ 与队列操作
    Stats counters_{};

    std::string device_id_;
    std::string topic_event_;
    std::string topic_env_;
    std::string topic_status_;

    std::atomic<bool> connected_{false};
    std::atomic<bool> enabled_{false};
    std::atomic<int64_t> last_sent_seq_{0};   // 已成功交给 esp-mqtt 的最大事件序号（RAM）
    int32_t boot_id_ = 0;
    TaskHandle_t net_task_ = nullptr;
};
