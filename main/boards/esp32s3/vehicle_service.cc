#include "vehicle_service.h"

#include <cmath>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "VehicleService"

VehicleService::VehicleService(i2c_master_bus_handle_t i2c_bus)
    : imu_(i2c_bus), config_(), monitor_(config_), history_(kHistoryCapacity) {
}

bool VehicleService::Start() {
    if (task_ != nullptr) {
        return true;
    }
    if (!imu_.Init()) {
        ESP_LOGE(TAG, "IMU 初始化失败，行车监测不启动（其余功能照常）");
        return false;
    }

    // ! 栈从 PSRAM 出（与上游 custom_wake_word.cc 的唤醒词编码任务同法），
    // ! 内部 RAM 只留给控制块；本项目内部 RAM 本来就紧。
    StackType_t* stack = static_cast<StackType_t*>(heap_caps_malloc(kTaskStackBytes, MALLOC_CAP_SPIRAM));
    StaticTask_t* tcb = static_cast<StaticTask_t*>(heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL));
    if (stack == nullptr || tcb == nullptr) {
        ESP_LOGE(TAG, "采样任务栈分配失败（PSRAM 余量不足？）");
        heap_caps_free(stack);
        heap_caps_free(tcb);
        return false;
    }

    // ! ulStackDepth 的单位是 StackType_t 字数（S3 上 4 字节），不是字节；
    // ! 直接把 kTaskStackBytes 传进去会让任务按 4 倍大小使用这块 PSRAM → 立刻踩内存。
    task_ = xTaskCreateStaticPinnedToCore(ImuTaskEntry, "imu_task", kTaskStackBytes / sizeof(StackType_t), this, 5,
                                          stack, tcb, 0);
    ESP_LOGI(TAG, "imu_task 已启动：%d ms 周期（%d Hz）", kSamplePeriodMs, 1000 / kSamplePeriodMs);
    return task_ != nullptr;
}

void VehicleService::ImuTaskEntry(void* arg) {
    static_cast<VehicleService*>(arg)->ImuTaskLoop();
}

void VehicleService::ImuTaskLoop() {
    const TickType_t period = pdMS_TO_TICKS(kSamplePeriodMs);
    TickType_t last_wake = xTaskGetTickCount();
    int error_streak = 0;
    vehicle::ImuSample sample;

    while (true) {
        const Qmi8658a::ReadResult result = imu_.ReadSample(sample);
        if (result == Qmi8658a::ReadResult::kOk) {
            error_streak = 0;
            {
                std::lock_guard<std::mutex> lock(sample_mutex_);
                latest_ = sample;
            }

            monitor_.Feed(sample);

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
                history_.Append(event);
                const vehicle::EventRecord* record = history_.At(history_.size() - 1);
                if (record != nullptr) {
                    LogEvent(*record);
                }
            }
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

void VehicleService::LogEvent(const vehicle::EventRecord& record) {
    // ! 不要用 %lld：本工程是 CONFIG_NEWLIB_NANO_FORMAT=y，nano 版 vfprintf 不支持 64 位整数格式，
    // ! %lld 只消费 4 字节，后面的可变参数全部错位——%s 会读到 NULL，
    // ! 在 printf 内部的 memchr 上踩空指针崩溃（LoadProhibited）。这里只用 32 位整数与浮点格式。
    ESP_LOGI(TAG, "事件 #%d %s value=%.2f ts=%.3f s", static_cast<int>(record.seq),
             vehicle::ToString(record.event.type), record.event.value,
             static_cast<double>(record.event.ts_ms) / 1000.0);
}

vehicle::ImuSample VehicleService::latest_sample() const {
    std::lock_guard<std::mutex> lock(sample_mutex_);
    return latest_;
}
