// ! 本项目（车载AI行车状态监测终端）不使用设备端视觉推理，整份自测已停用。
// ! 保留文件是为了留存"esp-dl 本地推理在本板输出与输入无关"的排查代码，
// ! 便于日后复查（结论与证据见 docs/ 下的调研报告）。如需删除请明确告知。
#if 0
#include "face_selftest.h"

#include <string.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "dl_detect_espdet_postprocessor.hpp"
#include "dl_image_define.hpp"
#include "dl_image_preprocessor.hpp"
#include "dl_model_base.hpp"
#include "dl_tensor_base.hpp"
#include "esp_camera.h"

#define TAG "FaceSelfTest"

#define PICO_MODEL_NAME "espdet_pico_224_224_face.espdl"

// > 按 Kconfig 选择模型的取数路径：RODATA（编进 app）或独立 flash 分区。
// > 这里必须与组件 human_face_detect.cpp 的写法一致，否则链接器会把模型数据 GC 掉
#if CONFIG_HUMAN_FACE_DETECT_MODEL_IN_FLASH_RODATA
extern const uint8_t human_face_detect_espdl[] asm("_binary_human_face_detect_espdl_start");
static const char *kModelPath = (const char *)human_face_detect_espdl;
static const fbs::model_location_type_t kModelLocation = fbs::MODEL_LOCATION_IN_FLASH_RODATA;
static const char *kModelPathName = "RODATA";
#else
static const char *kModelPath = "human_face_det";
static const fbs::model_location_type_t kModelLocation = fbs::MODEL_LOCATION_IN_FLASH_PARTITION;
static const char *kModelPathName = "PARTITION";
#endif

// > 只看输出张量的取值范围：若两个极端输入给出相同统计，说明模型对输入不敏感
static void DumpOutputStats(dl::Model *model, const char *label) {
    for (auto &kv : model->get_outputs()) {
        dl::TensorBase *t = kv.second;
        if (t == nullptr || t->get_dtype() != dl::DATA_TYPE_INT8) {
            continue;
        }
        double mn = 1e30;
        double mx = -1e30;
        double sum = 0;
        for (int i = 0; i < t->size; i++) {
            double v = (double)t->get_element<int8_t>(i);
            if (v < mn) mn = v;
            if (v > mx) mx = v;
            sum += v;
        }
        ESP_LOGW(TAG, "%s OUT[%s] min=%.1f max=%.1f mean=%.2f", label, kv.first.c_str(), mn, mx,
                 t->size ? sum / t->size : 0.0);
    }
}

static void DumpInputStats(dl::Model *model, const char *label) {
    dl::TensorBase *t = model->get_input();
    if (t == nullptr || t->get_dtype() != dl::DATA_TYPE_INT8) {
        return;
    }
    double mn = 1e30;
    double mx = -1e30;
    double sum = 0;
    for (int i = 0; i < t->size; i++) {
        double v = (double)t->get_element<int8_t>(i);
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        sum += v;
    }
    ESP_LOGI(TAG, "%s IN  min=%.1f max=%.1f mean=%.2f", label, mn, mx, t->size ? sum / t->size : 0.0);
}

static void FaceSelfTestTask(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(20000));
    ESP_LOGI(TAG, "=== face self test: is the model input-sensitive at all? ===");

    auto *model = new dl::Model(kModelPath, PICO_MODEL_NAME, kModelLocation);
    if (model == nullptr) {
        ESP_LOGE(TAG, "model load failed");
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGW(TAG, "model path = %s (rodata=%p)", kModelPathName, (void *)kModelPath);
    dl::TensorBase *in = model->get_input();
    if (in == nullptr || in->data == nullptr) {
        ESP_LOGE(TAG, "input tensor invalid");
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "input tensor: size=%d bytes=%d", in->size, in->get_bytes());

    // > 极端 1：全 -128（最暗）
    memset(in->data, (int)(int8_t)-128, (size_t)in->size);
    DumpInputStats(model, "ALL-DARK");
    model->run();
    DumpOutputStats(model, "ALL-DARK");

    // > 极端 2：全 +127（最亮）
    memset(in->data, 127, (size_t)in->size);
    DumpInputStats(model, "ALL-BRIGHT");
    model->run();
    DumpOutputStats(model, "ALL-BRIGHT");

    // > 极端 3：用相机真实帧做一次完整预处理，与上面两个极端对照
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb != nullptr) {
        dl::image::img_t img = {
            .data = fb->buf,
            .width = (uint16_t)fb->width,
            .height = (uint16_t)fb->height,
            .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE,
        };
        auto *pre = new dl::image::ImagePreprocessor(model, {0, 0, 0}, {255, 255, 255});
        pre->enable_letterbox({114, 114, 114});
        auto *post = new dl::detect::ESPDetPostProcessor(model, pre, 0.5f, 0.7f, 10,
                                                         {{8, 8, 4, 4}, {16, 16, 8, 8}, {32, 32, 16, 16}});
        pre->preprocess(img);
        DumpInputStats(model, "CAMERA");
        model->run();
        DumpOutputStats(model, "CAMERA");
        // > 顺便跑一次后处理，看能不能真的框住人脸（省一次烧录）
        post->clear_result();
        post->postprocess();
        auto &results = post->get_result(img.width, img.height);
        ESP_LOGW(TAG, ">>> CAMERA faces=%d", (int)results.size());
        int shown = 0;
        for (auto &r : results) {
            if (r.box.size() >= 4 && shown < 3) {
                ESP_LOGW(TAG, "    box=[%d,%d,%d,%d] score=%.2f", r.box[0], r.box[1], r.box[2], r.box[3], r.score);
                shown++;
            }
        }
        delete post;
        delete pre;
        esp_camera_fb_return(fb);
    }

    ESP_LOGI(TAG, "=== face self test done ===");
    vTaskDelete(nullptr);
}

void FaceSelfTestStart() {
    BaseType_t created = xTaskCreatePinnedToCore(FaceSelfTestTask, "face_selftest", 8192, nullptr, 5, nullptr, 1);
    ESP_LOGW(TAG, "self test task create: %s", created == pdPASS ? "ok" : "FAILED");
}
#endif  // ! 自测整体停用
