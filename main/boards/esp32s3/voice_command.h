#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "vehicle_service.h"   // EventSink / WorkerTickable
#include "vehicle_types.h"
#include "voice_intent.h"
#include "voice_reply.h"

// 语音命令与播报（Plan C / D6）。
//
// ! 这个类**一次都不读 flash**：
// !   - OnVoiceAction() 由命令词回调调用，而回调运行在**音频输入任务**里（audio_service.cc
// !     的 AudioInputTask）；事件播报 OnEvent() 运行在 **worker 任务**里。
// !   - 手机端"有遗留"标记只存 RAM（atomic），不落 NVS —— 写 NVS 会关 cache，
// !     而标记是由 **HTTP 任务**（栈在 PSRAM）设置的，一写就命中
// !     `assert(esp_task_stack_is_sane_cache_disabled())`（BUG-024 / BUG-026）。
// !     代价：重启后标记复位，写进验收记录的局限一节。
class VoiceCommand : public EventSink, public WorkerTickable {
public:
    explicit VoiceCommand(VehicleService *vehicle, int max_actions = 8);
    ~VoiceCommand();

    // 命令词命中（任意任务可调，但只允许"存进队列"，执行在 worker 任务里做）
    void OnVoiceAction(const std::string &action);

    // 抓拍完成（由 CameraCapture 在 worker 任务里回调）
    void OnCaptureDone(vehicle::CaptureReason reason, bool saved);

    // 手机端标记（HTTP 任务调用；只写原子量）
    void SetLeftoverPending(bool pending) { leftover_pending_.store(pending); }
    bool leftover_pending() const { return leftover_pending_.load(); }

    // EventSink：行车事件 → 播报（只在待机态播，避免盖住小智说话）
    void OnEvent(const vehicle::EventRecord &record) override;

    // WorkerTickable：把排队的命令执行掉（由 VehicleService 的 worker 任务调用）
    void ExecutePending() override;

private:
    void Execute(vehicle::VoiceIntent intent);
    void Play(const std::vector<vehicle::ClipId> &clips);
    bool NetworkOnline() const;

    VehicleService *vehicle_ = nullptr;
    std::atomic<bool> leftover_pending_{false};
    int max_actions_ = 8;
    std::atomic<int> head_{0};
    std::atomic<int> tail_{0};
    int pending_[8] = {};   // 存 VoiceIntent 的值，0 = 空槽（kUnknown）
};
