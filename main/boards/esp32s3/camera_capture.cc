#include "camera_capture.h"

#include <cstdlib>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>

#include "esp_camera.h"
#include "image_to_jpeg.h"
#include "snapshot_store.h"

#define TAG "CameraCapture"

namespace {
constexpr uint8_t kJpegQuality = 80;   // 与 Esp32Camera::Explain() 一致（esp32_camera.cc:209）
}  // namespace

CameraCapture::CameraCapture(SnapshotStore *store, int width, int height)
    : store_(store), width_(width), height_(height) {
}

CameraCapture::~CameraCapture() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (swap_buf_ != nullptr) {
        heap_caps_free(swap_buf_);
        swap_buf_ = nullptr;
    }
}

size_t CameraCapture::GrabSwapped() {
    const size_t need = static_cast<size_t>(width_) * static_cast<size_t>(height_) * 2;
    if (swap_buf_ == nullptr || swap_capacity_ < need) {
        if (swap_buf_ != nullptr) {
            heap_caps_free(swap_buf_);
            swap_buf_ = nullptr;
            swap_capacity_ = 0;
        }
        swap_buf_ = static_cast<uint8_t *>(heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (swap_buf_ == nullptr) {
            ESP_LOGE(TAG, "交换缓冲分配失败（需要 %u B PSRAM）", static_cast<unsigned>(need));
            return 0;
        }
        swap_capacity_ = need;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == nullptr) {
        fail_count_++;
        // > esp_camera_fb_get() 内部等 4000 ms（driver/esp_camera.c 的 FB_GET_TIMEOUT）后返回 NULL：
        // > 相机没帧可给时它会静默失败，主循环里什么也看不出来（BUG-025 就是靠这条日志定位的）。
        // > 每 60 次打一条（预览 60 ms 一帧 ≈ 3.6 s），既不刷屏又能看出在持续失败。
        if (fail_count_ % 60 == 1) {
            ESP_LOGW(TAG, "取帧失败累计 %d 次：相机没有可用帧缓冲（预览与抓拍都会跳过）", fail_count_);
        }
        return 0;
    }

    size_t copied = 0;
    if (fb->format == PIXFORMAT_RGB565 && fb->width == width_ && fb->height == height_) {
        // > 与上游同口径：RGB565 需要 16 位字节序交换（esp32_camera.cc:102-105 的 bswap16），
        // > 显示侧（lcd_display.cc:157 swap_bytes=1）与 JPEG 编码侧（esp32_camera.cc:204-209）
        // > 用的都是交换后的数据。
        const uint16_t *src = reinterpret_cast<const uint16_t *>(fb->buf);
        uint16_t *dst = reinterpret_cast<uint16_t *>(swap_buf_);
        const size_t pixels = static_cast<size_t>(width_) * static_cast<size_t>(height_);
        for (size_t i = 0; i < pixels; i++) {
            dst[i] = __builtin_bswap16(src[i]);
        }
        copied = need;
        ok_count_++;
    } else {
        if (!warned_size_) {
            warned_size_ = true;
            ESP_LOGW(TAG, "相机帧格式不符：format=%d %dx%d（期望 RGB565 %dx%d），预览与抓拍会一直跳过",
                     fb->format, fb->width, fb->height, width_, height_);
        }
        fail_count_++;
    }

    esp_camera_fb_return(fb);
    return copied;
}

bool CameraCapture::CopyPreviewFrame(uint8_t *dst, size_t dst_capacity, int *width, int *height) {
    const size_t need = static_cast<size_t>(width_) * static_cast<size_t>(height_) * 2;
    if (dst == nullptr || dst_capacity < need) {
        return false;
    }
    // ! 本函数跑在 LVGL 任务里，**绝不能阻塞**：拿不到锁（worker 正在抓拍）就跳过这一帧。
    // ! 用阻塞锁的话，一旦相机停摆（见 docs/BUGS.md BUG-025），这里会连界面一起卡死——
    // ! 触摸、翻页、按钮全部无响应，而画面停在最后一帧上。
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return false;
    }
    if (GrabSwapped() == 0) {
        return false;
    }
    memcpy(dst, swap_buf_, need);
    if (width != nullptr) {
        *width = width_;
    }
    if (height != nullptr) {
        *height = height_;
    }
    return true;
}

bool CameraCapture::CaptureJpeg(std::string &out) {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t swapped = GrabSwapped();
    if (swapped == 0) {
        return false;
    }

    uint8_t *jpeg = nullptr;
    size_t jpeg_len = 0;
    if (!image_to_jpeg(swap_buf_, swapped, static_cast<uint16_t>(width_), static_cast<uint16_t>(height_),
                       V4L2_PIX_FMT_RGB565, kJpegQuality, &jpeg, &jpeg_len)) {
        ESP_LOGW(TAG, "JPEG 编码失败");
        return false;
    }
    if (jpeg == nullptr || jpeg_len == 0) {
        ESP_LOGW(TAG, "JPEG 编码返回空数据");
        return false;
    }

    out.assign(reinterpret_cast<const char *>(jpeg), jpeg_len);
    // > image_to_jpeg() 的输出缓冲由调用方释放，实现内部自己也是 free(outbuf)
    // > （main/display/lvgl_display/jpg/image_to_jpeg.cpp:423）。
    free(jpeg);
    return true;
}

void CameraCapture::OnCaptureRequest(vehicle::CaptureReason reason, int64_t ts_ms) {
    if (store_ == nullptr) {
        return;
    }
    std::string jpeg;
    if (!CaptureJpeg(jpeg)) {
        // ! 抓拍失败只告警、不往事件历史里塞条目：EventType 里没有"抓拍失败"，
        // ! 硬塞会污染事件页与事件计数（计划里刻意偏离设计文档 §9 的那一条）。
        ESP_LOGW(TAG, "抓拍失败（原因：%s）", vehicle::ToString(reason));
        return;
    }
    const bool saved = store_->SaveSnapshot(reinterpret_cast<const uint8_t *>(jpeg.data()), jpeg.size());
    // > worker 的栈在**内部 RAM**（它要写 SPIFFS，见 vehicle_service.cc 的 CreateTask()），
    // > 而内部 RAM 现在很紧：在这里打一条栈余量，用来判断 kWorkerStackBytes=8192 是否偏大。
    // > uxTaskGetStackHighWaterMark() 在 ESP-IDF 里返回**字节**（FreeRTOS-Kernel/include/freertos/task.h 注释）。
    const unsigned stack_free = static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr));
    // ! 不要用 %lld：本工程是 nano printf（docs/BUGS.md BUG-001）。ts_ms 用 double 打。
    ESP_LOGI(TAG, "抓拍完成：原因=%s %u KB ts=%.3f s 落盘=%s（worker 栈余量 %u B）", vehicle::ToString(reason),
             static_cast<unsigned>(jpeg.size() / 1024), static_cast<double>(ts_ms) / 1000.0, saved ? "是" : "否",
             stack_free);
}
