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
class SnapshotStore : public EventSink {
public:
    static constexpr const char *kRoot = "/snap";
    static constexpr size_t kMaxLogBytes = 256 * 1024;   // events.log 超过就清空重开

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

    // EventSink：事件落盘
    void OnEvent(const vehicle::EventRecord &record) override;

private:
    bool AppendEventLine(const std::string &line);

    vehicle::SnapshotRing ring_;
    bool mounted_ = false;
    std::mutex mutex_;             // worker 任务写、HTTP 任务读
    std::string memory_jpeg_;      // 内存模式的最近一张
};
