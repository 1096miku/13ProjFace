#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "vehicle_service.h"   // EventSink
#include "vehicle_types.h"

class SnapshotStore;

// 摄像头取帧 + JPEG 抓拍。
//
// ! 不复用 Esp32Camera::Capture()：它没有取帧接口（current_fb_ 是 private）、每帧在 PSRAM
// ! 新分配 153,600 B、会把帧推到聊天界面的预览槽并重置 5 s 隐藏计时，而且内部无锁
// ! （main/boards/common/esp32_camera.cc:59-130）。这里直接走 esp_camera_fb_get()/fb_return()，
// ! 并用一把锁把"预览"与"抓拍"隔开（设计文档 §8：预览与抓拍互斥）。
class CameraCapture : public EventSink {
public:
    explicit CameraCapture(SnapshotStore *store, int width = 320, int height = 240);
    ~CameraCapture();

    // 复制一帧到 dst（已做 RGB565 字节序交换，stride = width * 2）。
    // dst 容量不足 / 拿不到帧 / 分辨率不符都返回 false（调用方跳过这一帧即可）。
    bool CopyPreviewFrame(uint8_t *dst, size_t dst_capacity, int *width, int *height);

    // 抓拍一张 JPEG 到 out（内部释放编码缓冲）
    bool CaptureJpeg(std::string &out);

    // 打开/关闭取帧。关闭后 CopyPreviewFrame() 与 CaptureJpeg() **立刻返回 false、一次都不碰驱动**。
    //
    // ! 由 self.camera.set_enabled 调用，必须在**非 LVGL 任务**里调：本函数会用阻塞锁等待
    // ! 在飞的那一次取帧结束（相机没帧时 esp_camera_fb_get() 内部等 4000 ms），最坏阻塞 4 s。
    // ! 关摄像头后预览页卡死整个 app 的根因就是"相机已断电、预览还在 60 ms 一次去取帧"，
    // ! 见 docs/BUGS.md BUG-029。
    void SetEnabled(bool enabled);
    bool enabled() const { return enabled_; }

    // EventSink：碰撞 / 锁车期异常震动 / 进入锁车监测 / 手动抓拍 → 编码并落盘
    void OnCaptureRequest(vehicle::CaptureReason reason, int64_t ts_ms) override;

    // EventSink：相机不消费事件历史（落盘是 SnapshotStore 的事），但基类的 OnEvent 是纯虚的，
    // ! 不写这个空实现，CameraCapture 就是抽象类，`new CameraCapture(...)` 直接编译失败。
    void OnEvent(const vehicle::EventRecord &record) override {
        (void)record;
    }

    int ok_count() const { return ok_count_; }
    int fail_count() const { return fail_count_; }

    // 抓拍并落盘成功后调用（由 worker 任务在自己的栈上执行，**可以碰 flash**）。
    // > 用途：语音"重新抓拍"要等真正出图后再播"抓拍完成"（设计文档 §6.4）。
    void SetCaptureDoneCallback(std::function<void(vehicle::CaptureReason reason, bool saved)> callback) {
        capture_done_callback_ = std::move(callback);
    }

private:
    // 取一帧并做字节序交换到 swap_buf_；返回交换后的字节数（0 = 失败）
    size_t GrabSwapped();

    // 熔断：一次取帧耗时 ≥ kStallMs 视为"相机已停摆"，冷却期内一次都不碰驱动。
    // ! 没有它，相机停摆时 LVGL 任务会被 esp_camera_fb_get() 的 4 s 超时反复按死（BUG-029 的
    // ! "整个 app 卡死"那段机制）；冷却时长按 8 s 起倍增，上限 60 s。
    static constexpr int64_t kStallMs = 1000;
    static constexpr int64_t kCooldownBaseMs = 8000;
    static constexpr int64_t kCooldownMaxMs = 60000;

    SnapshotStore *store_ = nullptr;
    std::mutex mutex_;
    uint8_t *swap_buf_ = nullptr;
    size_t swap_capacity_ = 0;
    int width_ = 320;
    int height_ = 240;
    int ok_count_ = 0;
    int fail_count_ = 0;
    bool warned_size_ = false;

    std::atomic<bool> enabled_{true};
    std::function<void(vehicle::CaptureReason, bool)> capture_done_callback_;
    int64_t cooldown_until_ms_ = 0;   // > 当前时刻的毫秒数则跳过取帧
    int64_t cooldown_ms_ = 0;         // 上次熔断设定的冷却时长（倍增用）
};
