#include "vehicle_http.h"

#include <string>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "board.h"
#include "cJSON.h"
#include "snapshot_store.h"

#define TAG "VehicleHttp"

namespace {

constexpr const char *kIndexHtml =
    "<!doctype html><html lang=zh><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>车载终端</title></head><body style='font-family:sans-serif;margin:16px'>"
    "<h2>车载终端</h2>"
    "<p><img src='/latest.jpg' style='max-width:100%;border:1px solid #ccc'></p>"
    "<p><a href='/latest.jpg'>原图</a> · <a href='/events'>事件 JSON</a> · <a href='/'>刷新</a></p>"
    "<h3>最近事件</h3><pre id=e style='white-space:pre-wrap'>加载中…</pre>"
    "<script>fetch('/events').then(r=>r.text()).then(t=>{"
    "document.getElementById('e').textContent=t||'（暂无事件）'})</script>"
    "</body></html>";

void SendText(httpd_req_t *req, const char *type, const std::string &body) {
    httpd_resp_set_type(req, type);
    httpd_resp_send(req, body.data(), static_cast<ssize_t>(body.size()));
}

// 访问地址最多打印尝试次数（首次 5 s 后，之后每次间隔 3 s）
constexpr int kUrlLogMaxAttempts = 6;

}  // namespace

VehicleHttp::VehicleHttp(SnapshotStore *store) : store_(store) {
}

VehicleHttp::~VehicleHttp() {
    if (url_timer_ != nullptr) {
        esp_timer_delete(url_timer_);
        url_timer_ = nullptr;
    }
    if (server_ != nullptr) {
        httpd_stop(server_);
        server_ = nullptr;
    }
}

bool VehicleHttp::Start(uint16_t port) {
    if (server_ != nullptr) {
        return true;
    }
    port_ = port;
    const size_t sram_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    // ! 控制端口也必须错开：`HTTPD_DEFAULT_CONFIG()` 的 `ctrl_port` 是 32768
    // ! （IDF v5.5.3 `esp_http_server.h` 的 `ESP_HTTPD_DEF_CTRL_PORT`），配网 AP 的 httpd
    // ! 用的是同一套默认值 —— 数据端口错开之后，第二处冲突就在这里，报
    // ! `E httpd: httpd_server_init: error in creating ctrl socket (112)`（112 = EADDRINUSE），
    // ! 同样紧跟上游的 `ESP_ERROR_CHECK` → abort。见 docs/BUGS.md BUG-035。
    config.ctrl_port = 32769;
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;
    // > 任务只做"把内存里的字节塞进 socket"，6 KB 足够；栈放 PSRAM 是因为内部 RAM 太紧
    // > （默认值本来就是 MALLOC_CAP_INTERNAL，见 IDF v5.5.3 esp_http_server.h:176）。
    // > 这个任务**不读 flash**（见头文件里的说明），所以放 PSRAM 是安全的。
    config.stack_size = 6144;
    config.task_caps = MALLOC_CAP_SPIRAM;

    if (httpd_start(&server_, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务启动失败（端口 %u 被占用？）", static_cast<unsigned>(port));
        server_ = nullptr;
        return false;
    }

    const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = HandleRoot, .user_ctx = this};
    const httpd_uri_t latest = {.uri = "/latest.jpg", .method = HTTP_GET, .handler = HandleLatest, .user_ctx = this};
    const httpd_uri_t events = {.uri = "/events", .method = HTTP_GET, .handler = HandleEvents, .user_ctx = this};
    httpd_register_uri_handler(server_, &root);
    httpd_register_uri_handler(server_, &latest);
    httpd_register_uri_handler(server_, &events);

    ESP_LOGI(TAG, "HTTP 服务已启动：/  /latest.jpg  /events（端口 %u，任务栈 6 KB 在 PSRAM，内部 RAM %u → %u B）",
             static_cast<unsigned>(port), static_cast<unsigned>(sram_before),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));

    // > 设备 IP 要等 WiFi 连上才知道，这里起一个一次性定时器在 5 s 后把地址打出来，
    // > 免得用户还得去翻 WiFi 日志。IP 从 Board::GetSystemInfoJson() 里取
    // > （main/boards/common/wifi_board.cc 把 "ip" 放进了这份 JSON）。
    const esp_timer_create_args_t args = {
        .callback = [](void *arg) { static_cast<VehicleHttp *>(arg)->LogAccessUrl(); },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "vehicle_http_url",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &url_timer_) != ESP_OK) {
        url_timer_ = nullptr;
    } else {
        esp_timer_start_once(url_timer_, 5 * 1000 * 1000);
    }
    return true;
}

void VehicleHttp::LogAccessUrl() {
    url_log_attempts_++;
    const std::string info = Board::GetInstance().GetSystemInfoJson();
    cJSON *root = cJSON_Parse(info.c_str());
    if (root == nullptr) {
        return;
    }
    // ! "ip" 不在顶层：Board::GetSystemInfoJson() 把 GetBoardJson() 挂在 "board" 下面
    // ! （main/boards/common/board.cc:173），真正的位置是 board.ip（见 docs/BUGS.md BUG-027）。
    const cJSON *board = cJSON_GetObjectItem(root, "board");
    const cJSON *ip = cJSON_IsObject(board) ? cJSON_GetObjectItem(board, "ip") : nullptr;
    if (cJSON_IsString(ip) && ip->valuestring != nullptr && ip->valuestring[0] != '\0') {
        ESP_LOGI(TAG, "手机浏览器打开：http://%s:%u/", ip->valuestring, static_cast<unsigned>(port_));
    } else if (url_log_attempts_ < kUrlLogMaxAttempts) {
        // > WiFi 还没连上（DHCP 拿 IP 可能十几秒），3 s 后再试；到上限就只留一条告警。
        esp_timer_start_once(url_timer_, 3 * 1000 * 1000);
    } else {
        ESP_LOGW(TAG, "还没拿到 IP；联网后用串口里 WiFi 打印的 IP 打开 http://<IP>:%u/",
                 static_cast<unsigned>(port_));
    }
    cJSON_Delete(root);
}

esp_err_t VehicleHttp::HandleRoot(httpd_req_t *req) {
    SendText(req, "text/html; charset=utf-8", kIndexHtml);
    // > 这里跑在 HTTP 任务里，顺手报一次栈余量：这条日志是"6 KB 够不够、PSRAM 栈有没有
    // > 被写穿"的唯一证据（uxTaskGetStackHighWaterMark(nullptr) = 当前任务）。
    ESP_LOGI(TAG, "首页已下发，HTTP 任务栈余量 %u B", static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    return ESP_OK;
}

esp_err_t VehicleHttp::HandleLatest(httpd_req_t *req) {
    auto *self = static_cast<VehicleHttp *>(req->user_ctx);
    // > 只读 PSRAM 缓存，不碰 SPIFFS（这个任务的栈在 PSRAM，读盘会 assert 复位）。
    const std::string jpeg = (self != nullptr && self->store_ != nullptr) ? self->store_->LatestJpeg() : std::string();
    if (jpeg.empty()) {
        httpd_resp_set_status(req, "404 Not Found");
        SendText(req, "text/plain; charset=utf-8", "还没有抓拍");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, jpeg.data(), static_cast<ssize_t>(jpeg.size()));
    return ESP_OK;
}

esp_err_t VehicleHttp::HandleEvents(httpd_req_t *req) {
    auto *self = static_cast<VehicleHttp *>(req->user_ctx);
    const std::string body = (self != nullptr && self->store_ != nullptr) ? self->store_->RecentEvents() : std::string();
    SendText(req, "text/plain; charset=utf-8", body);
    return ESP_OK;
}
