#pragma once

#include <cstdint>
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

    // EventSink：碰撞 / 锁车期异常震动 / 进入锁车监测 / 手动抓拍 → 编码并落盘
    void OnCaptureRequest(vehicle::CaptureReason reason, int64_t ts_ms) override;

    // EventSink：相机不消费事件历史（落盘是 SnapshotStore 的事），但基类的 OnEvent 是纯虚的，
    // ! 不写这个空实现，CameraCapture 就是抽象类，`new CameraCapture(...)` 直接编译失败。
    void OnEvent(const vehicle::EventRecord &record) override {
        (void)record;
    }

    int ok_count() const { return ok_count_; }
    int fail_count() const { return fail_count_; }

private:
    // 取一帧并做字节序交换到 swap_buf_；返回交换后的字节数（0 = 失败）
    size_t GrabSwapped();

    SnapshotStore *store_ = nullptr;
    std::mutex mutex_;
    uint8_t *swap_buf_ = nullptr;
    size_t swap_capacity_ = 0;
    int width_ = 320;
    int height_ = 240;
    int ok_count_ = 0;
    int fail_count_ = 0;
    bool warned_size_ = false;
};
