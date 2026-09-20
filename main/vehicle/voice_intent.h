#pragma once

// 语音命令意图：action（来自 index.json / CustomWakeWord）→ 业务意图。
//
// 纯逻辑，不依赖 ESP-IDF，可用主机 g++ 单元测试（test/voice_intent_test.cc 会拿
// main/boards/esp32s3/voice_commands.json 与本表逐条对齐，防止两处漂移）。

#include <cstdint>
#include <string>

namespace vehicle {

enum class VoiceIntent : uint8_t {
    kUnknown = 0,
    kWake,        // "wake"：唤醒词本身，不是命令
    kTemp,        // "temp"      车内温度
    kHumid,       // "humid"     车内湿度
    kLight,       // "light"     光照多少
    kOccupancy,   // "occupancy" 有没有人
    kStatus,      // "status"    设备状态
    kChime,       // "chime"     播放提示音
    kSnapshot,    // "snapshot"  重新抓拍
    kLock,        // "lock"      锁车
    kCount,
};

const char *ToString(VoiceIntent intent);

// action → 意图；认不出的返回 kUnknown（调用方只告警，不做事）
VoiceIntent ParseVoiceAction(const std::string &action);

}  // namespace vehicle
