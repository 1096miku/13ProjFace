#include "vehicle_service.h"

#include <cmath>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "VehicleService"

namespace {

// > ulStackDepth 的单位是 StackType_t 字数（S3 上 4 字节），必须除以 sizeof(StackType_t)，
// > 否则任务会按 4 倍大小使用这块缓冲（见 docs/BUGS.md BUG-002）。
// > stack_caps 决定栈从哪块内存出：
// >   - imu_task 只做纯计算与 I2C，栈放 PSRAM 省内部 RAM；
// >   - ! worker_task 会读写 SPIFFS（事件日志、抓拍落盘），**栈必须是内部 RAM**：
// >     SPIFFS 读写最终进 spi_flash，而 spi_flash_disable_interrupts_caches_and_other_cpu()
// >     里有 assert(esp_task_stack_is_sane_cache_disabled()) —— 要求当前 sp 在内部 DRAM
// >     （IDF v5.5.3 components/spi_flash/cache_utils.c:56-65、:126）。栈在 PSRAM 时
// >     关掉 cache 后连自己的栈都访问不到，直接 assert 复位（见 docs/BUGS.md BUG-024）。
bool CreateTask(TaskFunction_t entry, const char *name, int stack_bytes, void *arg, UBaseType_t priority,
                BaseType_t core, uint32_t stack_caps, TaskHandle_t *out) {
    StackType_t *stack = static_cast<StackType_t *>(heap_caps_malloc(stack_bytes, stack_caps));
    StaticTask_t *tcb = static_cast<StaticTask_t *>(heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL));
    if (stack == nullptr || tcb == nullptr) {
        ESP_LOGE(TAG, "任务 %s 的栈/TCB 分配失败（内部 RAM 或 PSRAM 余量不足？）", name);
        heap_caps_free(stack);
        heap_caps_free(tcb);
        return false;
    }
    *out = xTaskCreateStaticPinnedToCore(entry, name, static_cast<uint32_t>(stack_bytes) / sizeof(StackType_t), arg,
                                         priority, stack, tcb, core);
    return *out != nullptr;
}

}  // namespace

VehicleService::VehicleService(i2c_master_bus_handle_t i2c_bus)
    : imu_(i2c_bus), config_(), monitor_(config_), history_(kHistoryCapacity) {
}

void VehicleService::AddEventSink(EventSink *sink) {
    if (sink == nullptr || sink_count_ >= kMaxSinks) {
        return;
    }
    sinks_[sink_count_++] = sink;
}

bool VehicleService::Start() {
    if (task_ != nullptr) {
        return true;
    }
    if (!imu_.Init()) {
        ESP_LOGE(TAG, "IMU 初始化失败，行车监测不启动（其余功能照常）");
        return false;
    }

    event_queue_ = xQueueCreate(kEventQueueSize, sizeof(vehicle::EventRecord));
    if (event_queue_ == nullptr) {
        ESP_LOGE(TAG, "事件队列创建失败");
        return false;
    }

    // > worker 的栈必须在内部 RAM（它要读写 SPIFFS），见 CreateTask() 的注释。
    if (!CreateTask(WorkerTaskEntry, "vehicle_worker", kWorkerStackBytes, this, 3, 1,
                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, &worker_task_)) {
        return false;
    }
    // ! 采样任务优先级必须高于 worker（5 > 3）：worker 一卡（写盘/JPEG 编码），
    // ! 采样也不能被推迟，否则 50 Hz 采样会出现成片丢帧。
    if (!CreateTask(ImuTaskEntry, "imu_task", kTaskStackBytes, this, 5, 0, MALLOC_CAP_SPIRAM, &task_)) {
        return false;
    }

    ESP_LOGI(TAG, "imu_task 已启动：%d ms 周期（%d Hz）；worker 周期 %u ms", kSamplePeriodMs, 1000 / kSamplePeriodMs,
             static_cast<unsigned>(kWorkerPeriodMs));
    return true;
}

void VehicleService::ImuTaskEntry(void *arg) {
    static_cast<VehicleService *>(arg)->ImuTaskLoop();
}

void VehicleService::WorkerTaskEntry(void *arg) {
    static_cast<VehicleService *>(arg)->WorkerTaskLoop();
}

void VehicleService::ImuTaskLoop() {
    const TickType_t period = pdMS_TO_TICKS(kSamplePeriodMs);
    TickType_t last_wake = xTaskGetTickCount();
    int error_streak = 0;
    vehicle::ImuSample sample;
    vehicle::MotionState last_state = vehicle::MotionState::kUncalibrated;

    while (true) {
        // > 先吃掉外部请求再采样：锁车请求来自 UI/MCP，不能直接写 monitor_（跨任务写状态机会撕裂）。
        const int lock_req = lock_request_.exchange(-1);
        if (lock_req >= 0) {
            monitor_.RequestLock(lock_req == 1);
        }

        const Qmi8658a::ReadResult result = imu_.ReadSample(sample);
        if (result == Qmi8658a::ReadResult::kOk) {
            error_streak = 0;
            monitor_.Feed(sample);
            PublishStatus(sample);

            if (!calibration_logged_ && monitor_.calibrated()) {
                calibration_logged_ = true;
                const float bx = monitor_.baseline_ax();
                const float by = monitor_.baseline_ay();
                const float bz = monitor_.baseline_az();
                // > |a| 静止时应 ≈ 1.000 g；明显偏小说明标定那一秒里板子在动，基线不可信，
                // > 之后所有阈值判定都会失准（连"静止"都判不出来，永远到不了 kParked）。
                ESP_LOGI(TAG, "静止基线标定完成：ax=%.3f ay=%.3f az=%.3f g（|a|=%.3f g）", bx, by, bz,
                         sqrtf(bx * bx + by * by + bz * bz));
            }

            vehicle::Event event;
            while (monitor_.PopEvent(event)) {
                vehicle::EventRecord record;
                record.event = event;
                {
                    std::lock_guard<std::mutex> lock(history_mutex_);
                    record.seq = history_.Append(event);
                }
                LogEvent(record);
                // ! 队列满时丢这条并计数：采样任务不允许阻塞，而 worker 排空速度
                // ! （200 ms 一轮 × 32 条）远高于事件产生速率。
                if (xQueueSend(event_queue_, &record, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "事件队列已满，丢弃事件 #%d", static_cast<int>(record.seq));
                }
            }

            // 进入锁车监测那一刻请求抓拍（设计文档 §6：进入时抓拍 1 张）
            const vehicle::MotionState state = monitor_.state();
            if (state == vehicle::MotionState::kLockedMonitor && last_state != vehicle::MotionState::kLockedMonitor) {
                capture_request_.store(static_cast<int>(vehicle::CaptureReason::kLockEntered));
            }
            last_state = state;
        } else if (result == Qmi8658a::ReadResult::kError) {
            if (++error_streak >= kMaxErrorStreak) {
                ESP_LOGW(TAG, "连续 %d 次读取失败，重新初始化 IMU", error_streak);
                error_streak = 0;
                if (!imu_.Init()) {
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
            }
        }

        vTaskDelayUntil(&last_wake, period);
    }
}

void VehicleService::WorkerTaskLoop() {
    const TickType_t period = pdMS_TO_TICKS(kWorkerPeriodMs);
    TickType_t last_wake = xTaskGetTickCount();
    int loop = 0;
    vehicle::EventRecord record;

    while (true) {
        // 1) 排空事件队列（一轮最多 32 条，队列容量就是 32）
        while (xQueueReceive(event_queue_, &record, 0) == pdTRUE) {
            DispatchEvent(record);
        }

        // 2) 抓拍请求（碰撞 / 锁车期震动 / 进入锁车 / 手动）
        const int capture_reason = capture_request_.exchange(-1);
        if (capture_reason >= 0) {
            const auto reason = static_cast<vehicle::CaptureReason>(capture_reason);
            // > 只借 status_ 拿最新采样时间戳；抓拍原因由请求方决定，不在这里按状态猜。
            int64_t ts_ms = 0;
            {
                std::lock_guard<std::mutex> lock(status_mutex_);
                ts_ms = status_.sample.ts_ms;
            }
            for (int i = 0; i < sink_count_; i++) {
                sinks_[i]->OnCaptureRequest(reason, ts_ms);
            }
        }

        // 3) 1 Hz 读环境传感器
        if (++loop >= kEnvPeriodLoops) {
            loop = 0;
            vehicle::EnvReading reading;
            if (env_sensor_.Read(esp_timer_get_time() / 1000, reading)) {
                std::lock_guard<std::mutex> lock(history_mutex_);
                env_ = reading;
            }
        }

        vTaskDelayUntil(&last_wake, period);
    }
}

void VehicleService::PublishStatus(const vehicle::ImuSample &sample) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.state = monitor_.state();
    status_.calibrated = monitor_.calibrated();
    status_.sample = sample;
    status_.events_total = monitor_.TotalEventCount();
    status_.last_event_seq = history_.last_seq();
}

void VehicleService::DispatchEvent(const vehicle::EventRecord &record) {
    // > 只有碰撞与锁车期异常震动需要抓拍（设计文档 §4/§6）；其余事件仅落盘/上报。
    if (record.event.type == vehicle::EventType::kCrash) {
        capture_request_.store(static_cast<int>(vehicle::CaptureReason::kCrash));
    } else if (record.event.type == vehicle::EventType::kMotionWhileParked) {
        capture_request_.store(static_cast<int>(vehicle::CaptureReason::kMotionWhileParked));
    }
    for (int i = 0; i < sink_count_; i++) {
        sinks_[i]->OnEvent(record);
    }
}

void VehicleService::LogEvent(const vehicle::EventRecord &record) {
    // ! 不要用 %lld：本工程是 CONFIG_NEWLIB_NANO_FORMAT=y，nano 版 vfprintf 不支持 64 位整数格式，
    // ! %lld 只消费 4 字节，后面的可变参数全部错位——%s 会读到 NULL，
    // ! 在 printf 内部的 memchr 上踩空指针崩溃（LoadProhibited）。这里只用 32 位整数与浮点格式。
    ESP_LOGI(TAG, "事件 #%d %s value=%.2f ts=%.3f s", static_cast<int>(record.seq),
             vehicle::ToString(record.event.type), record.event.value,
             static_cast<double>(record.event.ts_ms) / 1000.0);
}

vehicle::VehicleStatus VehicleService::Status() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_;
}

vehicle::EnvReading VehicleService::env() const {
    std::lock_guard<std::mutex> lock(history_mutex_);
    return env_;
}

std::vector<vehicle::EventRecord> VehicleService::CopyHistory() const {
    std::lock_guard<std::mutex> lock(history_mutex_);
    std::vector<vehicle::EventRecord> out;
    out.reserve(static_cast<size_t>(history_.size()));
    for (int i = 0; i < history_.size(); i++) {
        const vehicle::EventRecord *record = history_.At(i);
        if (record != nullptr) {
            out.push_back(*record);
        }
    }
    return out;
}

void VehicleService::RequestLock(bool locked) {
    lock_request_.store(locked ? 1 : 0);
}

void VehicleService::RequestCapture() {
    capture_request_.store(static_cast<int>(vehicle::CaptureReason::kManual));
}
