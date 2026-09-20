#pragma once

#include <cstdint>

#include <esp_http_server.h>
#include <esp_timer.h>

class SnapshotStore;
class VoiceCommand;

// 局域网 HTTP：手机浏览器看图看事件（设计文档 §7.2）。
//   /            一页静态 HTML（内嵌，无外部依赖）：最近事件 + 一张图
//   /latest.jpg  最近一张抓拍（Cache-Control: no-store）
//   /events      最近 50 条事件，每行一条 JSON（text/plain）
//   /leftover    标记/清除"有遗留待确认"（Plan C：语音"有没有人"读这个 RAM 状态位）
//
// ! HTTP 任务的栈刻意放在 **PSRAM**（httpd_config_t::task_caps），而 SPIFFS 的读写
// ! 要求当前任务栈在内部 DRAM（BUG-024 / BUG-026），所以这个类**一次都不读盘**：
// ! /latest.jpg 与 /events 读的是 SnapshotStore 里由 worker 维护的 PSRAM 缓存。
// ! 为什么不用"6 KB 内部栈 + 直接读盘"（本计划任务 10 的原始写法）：实测内部 RAM
// ! 低水位只有 1003 B，且正好出现在小智拍照（JPEG 编码 + 上传）那一刻，再常驻 4~6 KB
// ! 内部 RAM 会与那条路径正面相撞（依据 build/acceptance_d4e.log）。
class VehicleHttp {
public:
    explicit VehicleHttp(SnapshotStore *store, VoiceCommand *voice = nullptr);
    ~VehicleHttp();

    // 起服务（默认 80 端口）。失败返回 false，只告警不阻断开机。
    bool Start(uint16_t port = 80);
    bool started() const { return server_ != nullptr; }

private:
    static esp_err_t HandleRoot(httpd_req_t *req);
    static esp_err_t HandleLatest(httpd_req_t *req);
    static esp_err_t HandleEvents(httpd_req_t *req);
    static esp_err_t HandleLeftover(httpd_req_t *req);
    void LogAccessUrl();

    SnapshotStore *store_ = nullptr;
    VoiceCommand *voice_ = nullptr;
    httpd_handle_t server_ = nullptr;
    uint16_t port_ = 0;                        // 实际监听端口（打印访问地址时要用）
    esp_timer_handle_t url_timer_ = nullptr;   // 开机后打印一次访问地址（拿不到 IP 就再试几次）
    int url_log_attempts_ = 0;
};
