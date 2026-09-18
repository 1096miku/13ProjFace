#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "snapshot_ring.h"
#include "vehicle_service.h"   // EventSink
#include "vehicle_types.h"

// 抓拍与事件日志的落盘层：`snapshots` 分区挂在 /snap。
//
// ! 挂载失败时进"内存模式"：只保留最近一张抓拍在 PSRAM，事件日志停写。
// ! **任何情况下都不阻塞其它功能**（设计文档 §5.3 / §9）。
//
// ! 读写 flash 的接口（ReadLatestJpeg / ReadRecentEvents / 落盘）只能在**栈位于内部 RAM**
// ! 的任务里调用：SPIFFS 读写最终进 spi_flash，而 spi_flash 关 cache 前有
// ! `assert(esp_task_stack_is_sane_cache_disabled())`（IDF v5.5.3 components/spi_flash/
// ! cache_utils.c:125-127，读路径走 esp_flash_api.c:972 → spi1_start → cache_disable），
// ! 栈在 PSRAM 的任务一碰就复位（见 docs/BUGS.md BUG-024 / BUG-026）。
// ! 所以给"栈在 PSRAM"的 HTTP 任务专门留了 LatestJpeg() / RecentEvents() 两个**纯内存**接口。
class SnapshotStore : public EventSink {
public:
    static constexpr const char *kRoot = "/snap";
    static constexpr size_t kMaxLogBytes = 256 * 1024;   // events.log 超过就清空重开
    static constexpr int kCachedEventLines = 50;         // /events 一次最多展示 50 条

    explicit SnapshotStore(int capacity = 32);

    // 挂载分区；失败返回 false（调用方只告警，不阻断开机）
    bool Start();

    bool mounted() const { return mounted_; }
    int saved_count() const { return ring_.written(); }

    // 保存一张 JPEG：环形覆盖 + 更新 latest.idx
    bool SaveSnapshot(const uint8_t *jpeg, size_t len);
    // 读最近一张抓拍（供 /latest.jpg）
    bool ReadLatestJpeg(std::string &out);
    // 读最近 max_lines 行事件日志（供 /events）
    bool ReadRecentEvents(int max_lines, std::string &out);

    // PSRAM 缓存（每次返回一份拷贝）：只读内存，**不碰 flash**，给 HTTP 任务用
    std::string LatestJpeg();
    std::string RecentEvents();

    // EventSink：事件落盘
    void OnEvent(const vehicle::EventRecord &record) override;

private:
    bool AppendEventLine(const std::string &line);
    void CacheEventLine(const std::string &line);

    vehicle::SnapshotRing ring_;
    bool mounted_ = false;
    std::mutex mutex_;             // worker 任务写、HTTP 任务读
    // > 最近一张抓拍的字节：挂载时是落盘内容的副本，没挂载时就是唯一的一份。
    std::string cache_jpeg_;
    // > 最近 kCachedEventLines 行 JSON（含换行），与 events.log 尾部内容一致。
    std::string cache_events_;
};
