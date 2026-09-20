#include "bemfa_client.h"

#include <cctype>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "bemfa_secrets.h"
#include "board.h"
#include "config.h"
#include "event_json.h"
#include "settings.h"
#include "snapshot_store.h"
#include "system_info.h"

#define TAG "BemfaClient"

namespace {

// > broker 地址/端口与主题名都在 config.h 里（BEMFA_BROKER_* / BEMFA_TOPIC_*），
// > 便于按现场情况换端点，不用动这个文件。
// > keepalive 与用户 STM32 工程那份跑通的实现一致（60 s）。
constexpr int kKeepAliveSeconds = 60;

// > ulStackDepth 的单位是 StackType_t 字数，必须除以 sizeof(StackType_t)（BUG-002）。
// > 与 vehicle_service.cc:24 的 CreateTask() 是同一套写法（那边 imu_task 也用 PSRAM 栈，已跑通）。
bool CreatePsramTask(TaskFunction_t entry, const char *name, int stack_bytes, void *arg, UBaseType_t priority,
                     BaseType_t core, TaskHandle_t *out) {
    StackType_t *stack = static_cast<StackType_t *>(heap_caps_malloc(stack_bytes, MALLOC_CAP_SPIRAM));
    StaticTask_t *tcb = static_cast<StaticTask_t *>(heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL));
    if (stack == nullptr || tcb == nullptr) {
        ESP_LOGE(TAG, "任务 %s 的栈/TCB 分配失败（PSRAM 或内部 RAM 余量不足？）", name);
        heap_caps_free(stack);
        heap_caps_free(tcb);
        return false;
    }
    *out = xTaskCreateStaticPinnedToCore(entry, name, static_cast<uint32_t>(stack_bytes) / sizeof(StackType_t), arg,
                                         priority, stack, tcb, core);
    return *out != nullptr;
}

}  // namespace

// > 内部 RAM 观测点（诊断用，刻意保留）：D7 的指标是"内部 RAM 余量 ≥10 KB 且无分配失败"，
// > 而加上本模块后低水位只有 6167 B。要判断这 4~5 KB 到底花在哪，就得在每一步打一次差值：
// > free = 当前余量，largest = 最大可分配块（碎片化指标），min = 开机以来低水位。
// ! 读的是 MALLOC_CAP_INTERNAL，与 SystemInfo 那行 "free sram" 同口径。
static void LogInternalRam(const char *stage) {
    ESP_LOGI(TAG, "[RAM] %-22s free=%6u largest=%6u min=%6u", stage,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));
}

BemfaClient::BemfaClient(VehicleService *vehicle, SnapshotStore *store) : vehicle_(vehicle), store_(store) {
    topic_event_ = BEMFA_TOPIC_EVENT;
    topic_env_ = BEMFA_TOPIC_ENV;
    topic_status_ = BEMFA_TOPIC_STATUS;
}

BemfaClient::~BemfaClient() {
    if (mqtt_) {
        mqtt_->Disconnect();
    }
    heap_caps_free(queue_buffer_);
    queue_buffer_ = nullptr;
}

std::string BemfaClient::DeviceId() {
    // > device_id 全程只此一处定义：MAC 去掉分隔符、转大写，12 位十六进制（设计文档 §7.1）
    const std::string mac = SystemInfo::GetMacAddress();
    std::string out;
    out.reserve(12);
    for (char c : mac) {
        if (c == ':' || c == '-' || c == ' ') {
            continue;
        }
        out.push_back(static_cast<char>(toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

bool BemfaClient::Start() {
    LogInternalRam("Start 入口");
    if (BEMFA_APP_ID[0] == '\0' || BEMFA_SECRET_KEY[0] == '\0') {
        ESP_LOGW(TAG, "没有巴法云凭据（构建期没读到 .env 里的 BEMFA_APP_ID / BEMFA_SECRET_KEY），MQTT 上报不启动");
        enabled_ = false;
        return false;
    }

    queue_buffer_ = static_cast<char *>(
        heap_caps_malloc(kQueueCapacity * vehicle::PendingQueue::kSlotBytes, MALLOC_CAP_SPIRAM));
    if (queue_buffer_ == nullptr) {
        ESP_LOGE(TAG, "积压队列缓冲分配失败（PSRAM %u B）",
                 static_cast<unsigned>(kQueueCapacity * vehicle::PendingQueue::kSlotBytes));
        return false;
    }
    queue_ = std::make_unique<vehicle::PendingQueue>(queue_buffer_, static_cast<int>(kQueueCapacity));
    // > 这一步必须**不掉**内部 RAM（缓冲显式 PSRAM）；掉了就说明分配跑到内部堆去了
    LogInternalRam("积压队列(PSRAM)后");

    device_id_ = DeviceId();

    // > boot_id：seq 每次重启从 1 重新开始（EventHistory 在 RAM 里），所以幂等键
    // > 必须带 boot —— 只用 device_id+seq 会把不同开机的同号事件判成同一条。
    // ! 这几行写 NVS，必须跑在内部 RAM 栈上：本函数由板级 StartNetwork() 调用（main 任务）。
    {
        Settings settings("vehicle", true);
        boot_id_ = settings.GetInt("boot_id", 0) + 1;
        settings.SetInt("boot_id", boot_id_);
        last_sent_seq_.store(settings.GetInt("sent_seq", 0));
    }

    // > 开机回填：上次运行期间没发出去的（seq > sent_seq）从 events.log 尾部捞回来。
    // > 只回填尾部 8 KB（约 130 条）—— 整个 events.log 可能 256 KB，而 std::string 走
    // > 默认分配器（内部堆 22 KB），一次性读全文件必然失败。
    if (store_ != nullptr) {
        std::string tail;
        if (store_->ReadEventsAfterSeq(last_sent_seq_.load(), tail)) {
            size_t begin = 0;
            int restored = 0;
            while (begin < tail.size()) {
                const size_t end = tail.find('\n', begin);
                const std::string line = tail.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
                begin = (end == std::string::npos) ? tail.size() : end + 1;
                if (!line.empty() && queue_->Push(line)) {
                    restored++;
                }
            }
            if (restored > 0) {
                ESP_LOGI(TAG, "开机回填 %d 条未上报事件（sent_seq=%d，本次 boot=%d）", restored,
                         static_cast<int>(last_sent_seq_.load()), static_cast<int>(boot_id_));
            }
        }
    }
    LogInternalRam("回填入队后");

    if (!CreatePsramTask(NetTaskEntry, "bemfa_net", kNetStackBytes, this, 2, 1, &net_task_)) {
        return false;
    }
    LogInternalRam("net_task 后(栈在PSRAM)");
    enabled_ = true;
    ESP_LOGI(TAG, "巴法云上报已启动：设备 %s，主题 %s / %s / %s，积压容量 %u 条（缓冲在 PSRAM）", device_id_.c_str(),
             topic_event_.c_str(), topic_env_.c_str(), topic_status_.c_str(),
             static_cast<unsigned>(kQueueCapacity));
    return true;
}

void BemfaClient::NetTaskEntry(void *arg) {
    static_cast<BemfaClient *>(arg)->NetLoop();
}

void BemfaClient::NetLoop() {
    // > Connect() 内部会等最多 MQTT_CONNECT_TIMEOUT_MS（10 s）的结果，所以放在这个任务里做，
    // > 不占 main 任务（Application::Start() 的时序）。重连由下面的循环自己兜底（见 TryConnect()）。
    mqtt_ = Board::GetInstance().GetNetwork()->CreateMqtt();
    if (mqtt_ == nullptr) {
        ESP_LOGE(TAG, "CreateMqtt() 返回空，上报任务退出");
        enabled_ = false;
        vTaskDelete(nullptr);
        return;
    }
    LogInternalRam("CreateMqtt 后");
    mqtt_->OnConnected([this]() {
        connected_.store(true);
        ESP_LOGI(TAG, "巴法云已连接（积压 %d 条待补传）", queue_ ? queue_->size() : 0);
    });
    mqtt_->OnDisconnected([this]() {
        connected_.store(false);
        ESP_LOGW(TAG, "巴法云断开，事件继续入本地队列（积压 %d 条）", queue_ ? queue_->size() : 0);
    });
    mqtt_->OnError([this](const std::string &error) { ESP_LOGW(TAG, "巴法云 MQTT 错误：%s", error.c_str()); });
    mqtt_->SetKeepAlive(kKeepAliveSeconds);

    int64_t last_env_ms = 0;
    int64_t last_status_ms = 0;
    int64_t last_log_ms = 0;
    int64_t last_connect_ms = -kConnectRetryMs;   // > 负数 = 立刻尝试第一次
    while (true) {
        const int64_t now_ms = esp_timer_get_time() / 1000;
        // > 未连接就自己兜底重连：见 TryConnect() 的注释（第一次连接常常发生在拿到 IP 之前）。
        if (!connected_.load() && (now_ms - last_connect_ms) >= kConnectRetryMs) {
            last_connect_ms = now_ms;
            TryConnect();
        }
        DrainQueue();
        if (connected_.load()) {
            if (now_ms - last_env_ms >= kEnvPeriodMs) {
                last_env_ms = now_ms;
                PublishEnv();
            }
            if (now_ms - last_status_ms >= kStatusPeriodMs) {
                last_status_ms = now_ms;
                PublishStatus();
            }
        }
        // > 每 30 s 打一条统计：D7 的"上报成功率"就是从这里数出来的。
        if (now_ms - last_log_ms >= 30000) {
            last_log_ms = now_ms;
            const Stats s = stats();
            ESP_LOGI(TAG, "上报统计 已发=%u 失败=%u 丢弃=%u 积压=%d 连接=%s", static_cast<unsigned>(s.published),
                     static_cast<unsigned>(s.failed), static_cast<unsigned>(s.dropped), static_cast<int>(s.queued),
                     s.connected ? "是" : "否");
        }
        vTaskDelay(pdMS_TO_TICKS(kLoopPeriodMs));
    }
}

// ! 只在**未连接**时调用：EspMqtt::Connect() 内部第一件事是 Disconnect() + 销毁旧客户端，
// ! 对已连上的连接再调一次会把好好的连接拆掉。
//
// > 为什么要自己兜底重试（实测数据，别照抄猜测）：
// > 板级 StartNetwork() 里 WifiBoard::StartNetwork() 是**异步**连 WiFi，所以本任务的
// > 前两次 Connect() 都发生在拿到 IP 之前，串口是
// >   `esp-tls: couldn't get hostname for :bemfa.com: getaddrinfo() returns 202`
// >   `esp_mqtt: MQTT error occurred: ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME`
// > （时刻 1.9 s 与 16.9 s，而 Got IP 在 18.6 s）。
// > esp-mqtt 的自动重连**确实会成功**（实测拿到 IP 后 30~60 s 内连上，随后
// > `上报统计 已发=148 失败=0 积压=0 连接=是`），但这段窗口里积压一直挂着、`连接=否`，
// > 且间隔按退避增长。这里补一个 15 s 的确定重试，把"拿到 IP 后多久连上"从
// > "看退避运气"变成"最迟 15 s"，长时间断网恢复时也不用等退避爬回去。
bool BemfaClient::TryConnect() {
    if (!mqtt_) {
        return false;
    }
    // ! 认证口径**照用户 STM32 工程那份跑通的实现**：
    // !   client_id = username = UID，password = secretKey；keepalive 60 s、clean session、无 will。
    // ! 我们 .env 里的 BEMFA_APP_ID 就是那个 UID 值（已逐值比对确认），所以 client_id 与 username 都用它。
    // ! 注意：同一 UID 同时只允许一个连接，别和 STM32 网关同时在线（会互相顶下线）。
    const std::string client_id = (BEMFA_UID[0] != '\0') ? std::string(BEMFA_UID) : std::string(BEMFA_APP_ID);
    LogInternalRam("Connect 前");
    const bool ok = mqtt_->Connect(BEMFA_BROKER_HOST, BEMFA_BROKER_PORT, client_id, client_id, BEMFA_SECRET_KEY);
    // > 这一行是本次排查的关键：esp-mqtt 客户端（任务栈 + outbox + 各缓冲）到底占多少内部 RAM
    if (ok) {
        LogInternalRam("Connect 成功(已连上)");
        return true;
    }
    LogInternalRam("Connect 失败后");
    ESP_LOGW(TAG, "连接巴法云失败（%s:%d），%d s 后重试", BEMFA_BROKER_HOST, BEMFA_BROKER_PORT,
             static_cast<int>(kConnectRetryMs / 1000));
    return false;
}

void BemfaClient::DrainQueue() {
    if (!connected_.load() || !queue_ || !mqtt_) {
        return;
    }
    // > 一次最多发 5 条：别把这个任务占太久（它还要发周期包、打统计）。
    int sent = 0;
    for (int i = 0; i < 5; i++) {
        std::string payload;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!queue_->Pop(payload)) {
                // > 刚把积压排空：此刻的内部 RAM 就是"回填突发"过后留下的水位（诊断用）
                if (sent > 0) {
                    LogInternalRam("积压排空后");
                }
                return;
            }
        }
        const int64_t seq = vehicle::ParseSeqFromJsonLine(payload);
        // > 补传时补上本次开机的 dev/boot：日志里本来只有 seq/type/ts_ms/value，
        // > 但幂等键要 dev+boot+seq，所以回填上来的行重新拼一次。
        const std::string outbound = vehicle::AddReportFields(payload, device_id_, boot_id_);
        if (!Publish(topic_event_, outbound, kEventQos)) {
            // > 刚好断线：放回队尾等下一次（**可能改变同批顺序**，但事件带 seq，接收端可自行排序；
            // > 比"弹出来直接丢掉"好）。
            std::lock_guard<std::mutex> lock(mutex_);
            queue_->Push(payload);
            return;
        }
        if (seq >= 0) {
            last_sent_seq_.store(seq);
        }
        sent++;
    }
}

bool BemfaClient::Publish(const std::string &topic, const std::string &payload, int qos) {
    if (!mqtt_ || !connected_.load()) {
        std::lock_guard<std::mutex> lock(mutex_);
        counters_.failed++;
        return false;
    }
    const bool ok = mqtt_->Publish(topic, payload, qos);
    std::lock_guard<std::mutex> lock(mutex_);
    if (ok) {
        counters_.published++;
    } else {
        counters_.failed++;
    }
    return ok;
}

void BemfaClient::PublishEnv() {
    if (vehicle_ == nullptr) {
        return;
    }
    const vehicle::EnvReading env = vehicle_->env();
    // > 读数无效时也发（手机上能看到"没数据"这件事），但 src 会标明模拟源
    Publish(topic_env_, vehicle::EnvToJson(env), kPeriodicQos);
}

void BemfaClient::PublishStatus() {
    if (vehicle_ == nullptr) {
        return;
    }
    const vehicle::VehicleStatus status = vehicle_->Status();
    Publish(topic_status_, vehicle::StatusToJson(status, true, boot_id_), kPeriodicQos);
}

void BemfaClient::OnEvent(const vehicle::EventRecord &record) {
    // ! 本函数运行在 worker 任务里（VehicleService::WorkerTaskLoop → DispatchEvent），
    // ! 那个任务的栈在**内部 RAM**，所以这里可以写 NVS。net_task 的栈在 PSRAM，写 NVS 会复位。
    if (!enabled_ || !queue_) {
        return;
    }
    const std::string payload = vehicle::EventToJson(record, device_id_, boot_id_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!queue_->Push(payload)) {
            counters_.dropped++;
        }
        counters_.queued = queue_->size();
    }
    // > 顺手把游标落盘：此刻的 last_sent_seq_ 是"上一条已交给 esp-mqtt 的序号"，
    // > 保守方向是"可能重发 1~2 条"，绝不会漏。事件频率低，NVS 写入次数可以忽略。
    PersistCursor();
}

void BemfaClient::PersistCursor() {
    // ! 只能在栈位于内部 RAM 的任务里调（写 NVS 会关 cache，见 BUG-024）
    Settings settings("vehicle", true);
    settings.SetInt("sent_seq", static_cast<int32_t>(last_sent_seq_.load()));
}

BemfaClient::Stats BemfaClient::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Stats out = counters_;
    out.connected = connected_.load();
    out.enabled = enabled_.load();
    out.queued = queue_ ? queue_->size() : 0;
    out.dropped = queue_ ? static_cast<uint32_t>(queue_->dropped()) : counters_.dropped;
    return out;
}

std::string BemfaClient::StatsJson() const {
    const Stats s = stats();
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"enabled\":%s,\"connected\":%s,\"published\":%u,\"failed\":%u,\"queued\":%d,\"dropped\":%u}",
             s.enabled ? "true" : "false", s.connected ? "true" : "false", static_cast<unsigned>(s.published),
             static_cast<unsigned>(s.failed), static_cast<int>(s.queued), static_cast<unsigned>(s.dropped));
    return std::string(buf);
}
