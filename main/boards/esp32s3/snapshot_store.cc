#include "snapshot_store.h"

#include <sys/stat.h>

#include <cstdio>

#include <esp_log.h>
#include <esp_spiffs.h>
#include <esp_timer.h>

#include "event_json.h"

#define TAG "SnapshotStore"

SnapshotStore::SnapshotStore(int capacity) : ring_(capacity) {
}

bool SnapshotStore::Start() {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = kRoot,
        .partition_label = "snapshots",
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    // > 首次挂载（分区是全 0xFF）会走 SPIFFS 格式化，整段 2.25 MB 要逐扇区擦除，
    // > 这一步是**开机时同步执行**的，所以把耗时打出来（只在第一次开机出现）。
    const int64_t begin_us = esp_timer_get_time();
    const esp_err_t err = esp_vfs_spiffs_register(&conf);
    const int64_t elapsed_ms = (esp_timer_get_time() - begin_us) / 1000;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "snapshots 分区挂载失败（%s，耗时 %d ms），进入内存模式：只保留最近一张抓拍",
                 esp_err_to_name(err), static_cast<int>(elapsed_ms));
        mounted_ = false;
        return false;
    }

    size_t total = 0;
    size_t used = 0;
    if (esp_spiffs_info("snapshots", &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "snapshots 已挂载到 %s：共 %u KB，已用 %u B（挂载耗时 %d ms）", kRoot,
                 static_cast<unsigned>(total / 1024), static_cast<unsigned>(used), static_cast<int>(elapsed_ms));
    }
    mounted_ = true;

    // > 预热 PSRAM 缓存：/latest.jpg 与 /events 不做任何读盘动作，重启后也要能立刻取到
    // > 上一次的内容。这里 safe 是因为 Start() 由板级构造函数调用（main 任务，栈在内部 RAM）。
    if (ReadLatestJpeg(cache_jpeg_)) {
        ESP_LOGI(TAG, "已把最近一张抓拍 %u B 读进内存缓存（供 /latest.jpg）", static_cast<unsigned>(cache_jpeg_.size()));
    }
    if (ReadRecentEvents(kCachedEventLines, cache_events_)) {
        ESP_LOGI(TAG, "已把最近事件 %u B 读进内存缓存（供 /events）", static_cast<unsigned>(cache_events_.size()));
    }
    return true;
}

bool SnapshotStore::SaveSnapshot(const uint8_t *jpeg, size_t len) {
    if (jpeg == nullptr || len == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!mounted_) {
        cache_jpeg_.assign(reinterpret_cast<const char *>(jpeg), len);
        ESP_LOGW(TAG, "内存模式：抓拍只保留在 PSRAM（%u B）", static_cast<unsigned>(len));
        return true;
    }

    const int slot = ring_.NextSlot();
    const std::string name = ring_.FileNameFor(slot);
    const std::string path = std::string(kRoot) + "/" + name;

    FILE *file = fopen(path.c_str(), "wb");
    if (file == nullptr) {
        ESP_LOGE(TAG, "打开 %s 失败，退回内存模式保存这一张", path.c_str());
        cache_jpeg_.assign(reinterpret_cast<const char *>(jpeg), len);
        return false;
    }
    const size_t written = fwrite(jpeg, 1, len, file);
    fclose(file);

    const std::string idx_path = std::string(kRoot) + "/latest.idx";
    FILE *idx = fopen(idx_path.c_str(), "w");
    if (idx != nullptr) {
        const std::string content = ring_.LatestIndexContent();
        fwrite(content.data(), 1, content.size(), idx);
        fclose(idx);
    }

    if (written != len) {
        ESP_LOGW(TAG, "抓拍写入不完整：%u / %u B", static_cast<unsigned>(written), static_cast<unsigned>(len));
    } else {
        ESP_LOGI(TAG, "抓拍已保存 %s（%u B，累计第 %d 张）", name.c_str(), static_cast<unsigned>(len), ring_.written());
    }
    // > 内存缓存始终跟着更新为相机刚给出的完整字节：/latest.jpg 只认它，不读盘。
    cache_jpeg_.assign(reinterpret_cast<const char *>(jpeg), len);
    return written == len;
}

std::string SnapshotStore::LatestJpeg() {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_jpeg_;
}

std::string SnapshotStore::RecentEvents() {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_events_;
}

bool SnapshotStore::ReadLatestJpeg(std::string &out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!mounted_) {
        if (cache_jpeg_.empty()) {
            return false;
        }
        out = cache_jpeg_;
        return true;
    }

    const std::string idx_path = std::string(kRoot) + "/latest.idx";
    FILE *idx = fopen(idx_path.c_str(), "r");
    if (idx == nullptr) {
        return false;
    }
    int slot = -1;
    const int matched = fscanf(idx, "%d", &slot);
    fclose(idx);
    if (matched != 1 || slot < 0) {
        return false;
    }
    const std::string name = ring_.FileNameFor(slot);
    if (name.empty()) {
        return false;
    }

    const std::string path = std::string(kRoot) + "/" + name;
    FILE *file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    out.clear();
    char buf[512];
    size_t read = 0;
    while ((read = fread(buf, 1, sizeof(buf), file)) > 0) {
        out.append(buf, read);
    }
    fclose(file);
    return !out.empty();
}

bool SnapshotStore::ReadRecentEvents(int max_lines, std::string &out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!mounted_) {
        return false;
    }
    const std::string path = std::string(kRoot) + "/events.log";
    FILE *file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    const long size = ftell(file);
    // > 从尾部往回读 8 KB 足够覆盖最近 50 条（一条约 60 B）；避免把整份日志读进内存。
    const long window = size > 8192 ? 8192 : size;
    if (fseek(file, size - window, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    std::string tail;
    tail.resize(static_cast<size_t>(window));
    const size_t got = fread(&tail[0], 1, static_cast<size_t>(window), file);
    fclose(file);
    tail.resize(got);

    // > 窗口开头可能截断半行：丢掉第一行（除非正好从文件头开始）。
    if (size > window) {
        const size_t newline = tail.find('\n');
        tail.erase(0, newline == std::string::npos ? tail.size() : newline + 1);
    }

    int lines = 0;
    for (size_t i = tail.size(); i > 0; i--) {
        if (tail[i - 1] == '\n') {
            lines++;
            if (lines > max_lines) {
                tail.erase(0, i);
                break;
            }
        }
    }
    out = tail;
    return true;
}

void SnapshotStore::OnEvent(const vehicle::EventRecord &record) {
    AppendEventLine(vehicle::EventToJson(record));
}

bool SnapshotStore::AppendEventLine(const std::string &line) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!mounted_) {
        return false;
    }
    const std::string path = std::string(kRoot) + "/events.log";

    struct stat st = {};
    if (stat(path.c_str(), &st) == 0 && static_cast<size_t>(st.st_size) > kMaxLogBytes) {
        // > 不考虑复杂轮转：/events 只要展示最近若干条，MQTT 补传游标由 Plan C 放 NVS。
        remove(path.c_str());
        ESP_LOGW(TAG, "events.log 超过 %u KB，已清空重开", static_cast<unsigned>(kMaxLogBytes / 1024));
    }

    FILE *file = fopen(path.c_str(), "a");
    if (file == nullptr) {
        ESP_LOGW(TAG, "events.log 打不开，事件未落盘");
        return false;
    }
    fwrite(line.data(), 1, line.size(), file);
    fwrite("\n", 1, 1, file);
    fclose(file);
    // > 写盘成功才更新内存缓存：/events 与 events.log 的内容由此保持一致。
    // > （没挂载时日志本来就不写，/events 也应当什么都没有，不要给出"以为记下来了"的假象。）
    CacheEventLine(line);
    return true;
}

void SnapshotStore::CacheEventLine(const std::string &line) {
    cache_events_ += line;
    cache_events_ += '\n';
    // > 只保留最后 kCachedEventLines 行：从尾部往回数到第 kCachedEventLines 个换行，
    // > 把它之前的整段丢掉。调用方已持锁。
    int lines = 0;
    for (size_t i = cache_events_.size(); i > 0; i--) {
        if (cache_events_[i - 1] == '\n' && ++lines > kCachedEventLines) {
            cache_events_.erase(0, i);
            break;
        }
    }
}
