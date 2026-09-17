#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "vehicle_types.h"

// 摄像头取帧 + JPEG 抓拍。
//
// ! 不复用 Esp32Camera::Capture()：它没有取帧接口（current_fb_ 是 private）、每帧在 PSRAM
// ! 新分配 153,600 B、会把帧推到聊天界面的预览槽并重置 5 s 隐藏计时，而且内部无锁
// ! （main/boards/common/esp32_camera.cc:59-130）。这里直接走 esp_camera_fb_get()/fb_return()，
// ! 并用一把锁把"预览"与"抓拍"隔开（设计文档 §8：预览与抓拍互斥）。
class CameraCapture {
public:
    explicit CameraCapture(int width = 320, int height = 240);
    ~CameraCapture();

    // 复制一帧到 dst（已做 RGB565 字节序交换，stride = width * 2）。
    // dst 容量不足 / 拿不到帧 / 分辨率不符都返回 false（调用方跳过这一帧即可）。
    bool CopyPreviewFrame(uint8_t *dst, size_t dst_capacity, int *width, int *height);

    // 抓拍一张 JPEG 到 out（内部释放编码缓冲）
    bool CaptureJpeg(std::string &out);

    // todo 任务 8（D4）在这里把 SnapshotStore 接进来：抓拍成功即 SaveSnapshot 落盘。
    // todo 现在只编码不落盘——快照分区与 SnapshotStore 属于 D5 的任务 7，D3 先用不上。

private:
    // 取一帧并做字节序交换到 swap_buf_；返回交换后的字节数（0 = 失败）
    size_t GrabSwapped();

    std::mutex mutex_;
    uint8_t *swap_buf_ = nullptr;
    size_t swap_capacity_ = 0;
    int width_ = 320;
    int height_ = 240;
    int ok_count_ = 0;
    int fail_count_ = 0;
    bool warned_size_ = false;
};
