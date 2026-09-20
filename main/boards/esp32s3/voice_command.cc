#include "voice_command.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "cJSON.h"

#define TAG "VoiceCommand"

namespace {

// > 播报保护闸的余量（BUG-047）。片段本身只有 1.2~1.4 s，但 MN 的检测窗是 3 s，
// > 自触发的命中实测落在"播报开始后 1~4 s"，所以闸 = 片段时长 + 这段余量。
constexpr int64_t kAnnounceGuardTailMs = 1500;

// > 片段字节数 → 毫秒。实测同一套编码参数：4897 B/1.25 s、5406 B/1.35 s、4806 B/1.20 s，
// > 约 4 B/ms（≈32 kbps）。用途只是估闸长，不要求精确。
constexpr int64_t kOggBytesPerMs = 4;

// > ClipId → 播报片段。**必须逐个写全**：片段只有被代码引用时才会进 app，
// > 没被引用的常量会被 --gc-sections 丢掉（设计文档 §6.4 的实测结论，
// > 表现为"片段放进去了但播报没反应、app 体积也不涨"）。
std::string_view SoundFor(vehicle::ClipId id) {
    switch (id) {
        case vehicle::ClipId::kD0: return Lang::Sounds::OGG_D0;
        case vehicle::ClipId::kD1: return Lang::Sounds::OGG_D1;
        case vehicle::ClipId::kD2: return Lang::Sounds::OGG_D2;
        case vehicle::ClipId::kD3: return Lang::Sounds::OGG_D3;
        case vehicle::ClipId::kD4: return Lang::Sounds::OGG_D4;
        case vehicle::ClipId::kD5: return Lang::Sounds::OGG_D5;
        case vehicle::ClipId::kD6: return Lang::Sounds::OGG_D6;
        case vehicle::ClipId::kD7: return Lang::Sounds::OGG_D7;
        case vehicle::ClipId::kD8: return Lang::Sounds::OGG_D8;
        case vehicle::ClipId::kD9: return Lang::Sounds::OGG_D9;
        case vehicle::ClipId::kD10: return Lang::Sounds::OGG_D10;
        case vehicle::ClipId::kUnitDegree: return Lang::Sounds::OGG_UNIT_DEGREE;
        case vehicle::ClipId::kUnitTimes: return Lang::Sounds::OGG_UNIT_TIMES;
        case vehicle::ClipId::kQTemp: return Lang::Sounds::OGG_Q_TEMP;
        case vehicle::ClipId::kQHumid: return Lang::Sounds::OGG_Q_HUMID;
        case vehicle::ClipId::kQEvents: return Lang::Sounds::OGG_Q_EVENTS;
        case vehicle::ClipId::kQStatusOk: return Lang::Sounds::OGG_Q_STATUS_OK;
        case vehicle::ClipId::kQOnline: return Lang::Sounds::OGG_Q_ONLINE;
        case vehicle::ClipId::kQOffline: return Lang::Sounds::OGG_Q_OFFLINE;
        case vehicle::ClipId::kQNoAlert: return Lang::Sounds::OGG_Q_NO_ALERT;
        case vehicle::ClipId::kQPending: return Lang::Sounds::OGG_Q_PENDING;
        case vehicle::ClipId::kQLightDark: return Lang::Sounds::OGG_Q_LIGHT_DARK;
        case vehicle::ClipId::kQLightDim: return Lang::Sounds::OGG_Q_LIGHT_DIM;
        case vehicle::ClipId::kQLightMid: return Lang::Sounds::OGG_Q_LIGHT_MID;
        case vehicle::ClipId::kQLightBright: return Lang::Sounds::OGG_Q_LIGHT_BRIGHT;
        case vehicle::ClipId::kQLightStrong: return Lang::Sounds::OGG_Q_LIGHT_STRONG;
        case vehicle::ClipId::kChime: return Lang::Sounds::OGG_TONE_CHIME;
        case vehicle::ClipId::kSnapStart: return Lang::Sounds::OGG_SNAP_START;
        case vehicle::ClipId::kSnapDone: return Lang::Sounds::OGG_SNAP_DONE;
        case vehicle::ClipId::kLockEntered: return Lang::Sounds::OGG_LOCK_ENTERED;
        case vehicle::ClipId::kLockExited: return Lang::Sounds::OGG_LOCK_EXITED;
        case vehicle::ClipId::kEvHardAccel: return Lang::Sounds::OGG_EV_HARD_ACCEL;
        case vehicle::ClipId::kEvHardBrake: return Lang::Sounds::OGG_EV_HARD_BRAKE;
        case vehicle::ClipId::kEvHardTurn: return Lang::Sounds::OGG_EV_HARD_TURN;
        case vehicle::ClipId::kEvBump: return Lang::Sounds::OGG_EV_BUMP;
        case vehicle::ClipId::kEvCrash: return Lang::Sounds::OGG_EV_CRASH;
        case vehicle::ClipId::kEvParked: return Lang::Sounds::OGG_EV_PARKED;
        case vehicle::ClipId::kEvDriving: return Lang::Sounds::OGG_EV_DRIVING;
        case vehicle::ClipId::kEvMotionParked: return Lang::Sounds::OGG_EV_MOTION_PARKED;
        case vehicle::ClipId::kNone:
        case vehicle::ClipId::kCount:
            break;
    }
    return {};
}

}  // namespace

VoiceCommand::VoiceCommand(VehicleService *vehicle, int max_actions)
    : vehicle_(vehicle), max_actions_(max_actions < 8 ? max_actions : 8) {
}

VoiceCommand::~VoiceCommand() = default;

void VoiceCommand::OnVoiceAction(const std::string &action) {
    const vehicle::VoiceIntent intent = vehicle::ParseVoiceAction(action);
    if (intent == vehicle::VoiceIntent::kUnknown || intent == vehicle::VoiceIntent::kWake) {
        ESP_LOGW(TAG, "命令词 %s 无法处理（intent=%s）", action.c_str(), vehicle::ToString(intent));
        return;
    }
    // ! 播报保护闸（BUG-047）：闸内到达的一律丢弃并打日志——这条日志就是"自触发"的实测计数。
    // ! 只丢"播报期间/刚播完"的命令，正常用户不会在这么短的间隙里再下一条命令。
    // ! 为什么不靠抬阈值：自触发概率 0.21~0.26 与真话的 0.20~0.35 **完全重叠**，切不开。
    const int64_t now_ms = esp_timer_get_time() / 1000;
    const int64_t guard_until = announce_guard_until_ms_.load();
    if (now_ms < guard_until) {
        ESP_LOGW(TAG, "播报保护闸内丢弃命令词 action=%s（闸内还剩 %d ms）——判为自触发或抢话",
                 action.c_str(), static_cast<int>(guard_until - now_ms));
        return;
    }
    // ! 这里运行在**音频输入任务**里：只入队，不执行（NVS/播报都挪到 worker 任务，
    // ! 免得把麦克风采集按在那儿）。队列满就丢最新的并告警——待机态下不可能堆起来。
    const int head = head_.load();
    const int next = (head + 1) % max_actions_;
    if (next == tail_.load()) {
        ESP_LOGW(TAG, "命令队列已满，丢弃 action=%s", action.c_str());
        return;
    }
    pending_[head] = static_cast<int>(intent);
    head_.store(next);
}

void VoiceCommand::ExecutePending() {
    while (tail_.load() != head_.load()) {
        const int tail = tail_.load();
        const vehicle::VoiceIntent intent = static_cast<vehicle::VoiceIntent>(pending_[tail]);
        pending_[tail] = 0;
        tail_.store((tail + 1) % max_actions_);
        Execute(intent);
    }
}

void VoiceCommand::Execute(vehicle::VoiceIntent intent) {
    const vehicle::EnvReading env = vehicle_->env();
    const vehicle::VehicleStatus status = vehicle_->Status();

    switch (intent) {
        case vehicle::VoiceIntent::kTemp:
            Play(vehicle::TemperatureClips(env.temp_c, env.valid));
            break;
        case vehicle::VoiceIntent::kHumid:
            Play(vehicle::HumidityClips(env.humidity_pct, env.valid));
            break;
        case vehicle::VoiceIntent::kLight:
            Play({vehicle::LightClip(env.lux)});
            break;
        case vehicle::VoiceIntent::kOccupancy:
            Play({vehicle::OccupancyClip(leftover_pending_.load())});
            break;
        case vehicle::VoiceIntent::kStatus:
            Play(vehicle::StatusClips(NetworkOnline(), status.events_total));
            break;
        case vehicle::VoiceIntent::kChime:
            Play({vehicle::ClipId::kChime});
            break;
        case vehicle::VoiceIntent::kSnapshot:
            Play({vehicle::ClipId::kSnapStart});
            vehicle_->RequestCapture();
            break;
        case vehicle::VoiceIntent::kLock: {
            const bool locked = status.state == vehicle::MotionState::kLockedMonitor;
            Play({locked ? vehicle::ClipId::kLockExited : vehicle::ClipId::kLockEntered});
            vehicle_->RequestLock(!locked);
            break;
        }
        case vehicle::VoiceIntent::kWake:
        case vehicle::VoiceIntent::kUnknown:
        case vehicle::VoiceIntent::kCount:
            return;
    }
    // > 这一行是 D7 指标测量的**唯一证据来源**（build/count_vc.ps1 按这个前缀统计）
    ESP_LOGI(TAG, "VC_DONE intent=%s 环境=%.1f℃/%.0f%%/%d lux 状态=%s", vehicle::ToString(intent),
             static_cast<double>(env.temp_c), static_cast<double>(env.humidity_pct), static_cast<int>(env.lux),
             vehicle::ToString(status.state));
}

void VoiceCommand::Play(const std::vector<vehicle::ClipId> &clips) {
    auto &app = Application::GetInstance();
    int played = 0;
    int64_t total_ms = 0;
    for (vehicle::ClipId id : clips) {
        const std::string_view sound = SoundFor(id);
        if (sound.empty()) {
            ESP_LOGW(TAG, "片段 %s 没有对应音频常量，跳过", vehicle::ToString(id));
            continue;
        }
        // > 逐段打一条：D6 联调时"命令识别到了但没声音"只能靠这个区分
        // > "片段没交出去" 与 "交了但没响"。内部 RAM 余量一起打，因为解码队列的
        // > payload 走内部堆（BUG-024 补充：空载只剩 20 KB 上下）。
        ESP_LOGI(TAG, "播放 %s（%u B，内部 RAM 余 %u B）", vehicle::ToString(id),
                 static_cast<unsigned>(sound.size()),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
        // > Application::PlaySound → AudioService::PlaySound：内部 OggDemuxer 解包后推解码队列，
        // > 连续多次调用会**按顺序排队播放**（Application::ShowActivationCode 拼数字就是这么做的）。
        app.PlaySound(sound);
        played++;
        total_ms += static_cast<int64_t>(sound.size()) / kOggBytesPerMs;
    }
    if (played == 0) {
        ESP_LOGW(TAG, "这一段没有任何可播片段（序列为空或全是 kNone）");
        return;
    }
    // > 关闸：从"现在"起把命令词入口封住，长度 = 本段所有片段的估算时长 + 余量（BUG-047）
    const int64_t guard_ms = total_ms + kAnnounceGuardTailMs;
    announce_guard_until_ms_.store(esp_timer_get_time() / 1000 + guard_ms);
    ESP_LOGD(TAG, "播报保护闸关闭 %d ms（%d 段/估算 %d ms）", static_cast<int>(guard_ms), played,
             static_cast<int>(total_ms));
}

bool VoiceCommand::NetworkOnline() const {
    // > 判据用"是否拿到 IP"，不是"巴法云 MQTT 连上了"：没配凭据时 MQTT 永远连不上，
    // > 那时播"未联网"会让用户以为 WiFi 断了（VehicleHttp::LogAccessUrl 用的是同一处来源）。
    const std::string info = Board::GetInstance().GetSystemInfoJson();
    cJSON *root = cJSON_Parse(info.c_str());
    if (root == nullptr) {
        return false;
    }
    const cJSON *board = cJSON_GetObjectItem(root, "board");
    const cJSON *ip = cJSON_IsObject(board) ? cJSON_GetObjectItem(board, "ip") : nullptr;
    const bool online = cJSON_IsString(ip) && ip->valuestring != nullptr && ip->valuestring[0] != '\0';
    cJSON_Delete(root);
    return online;
}

void VoiceCommand::OnCaptureDone(vehicle::CaptureReason reason, bool saved) {
    // > 只有语音/按钮的手动抓拍才播报"抓拍完成"：锁车期自动抓拍会频繁打扰。
    if (!saved || reason != vehicle::CaptureReason::kManual) {
        return;
    }
    Play({vehicle::ClipId::kSnapDone});
}

void VoiceCommand::OnEvent(const vehicle::EventRecord &record) {
    // > 只在待机态播事件：小智正在说话/听的时候插播会把对话搅乱。
    if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
        return;
    }
    const vehicle::ClipId clip = vehicle::EventClip(record.event.type);
    if (clip != vehicle::ClipId::kNone) {
        Play({clip});
    }
}
