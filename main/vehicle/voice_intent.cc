#include "voice_intent.h"

namespace vehicle {

namespace {

struct ActionEntry {
    const char *action;
    VoiceIntent intent;
};

// > 表的顺序与 voice_commands.json 一致，方便两条一起看。
constexpr ActionEntry kActions[] = {
    {"wake", VoiceIntent::kWake},           {"temp", VoiceIntent::kTemp},
    {"humid", VoiceIntent::kHumid},         {"light", VoiceIntent::kLight},
    {"occupancy", VoiceIntent::kOccupancy}, {"status", VoiceIntent::kStatus},
    {"chime", VoiceIntent::kChime},         {"snapshot", VoiceIntent::kSnapshot},
    {"lock", VoiceIntent::kLock},
};

}  // namespace

const char *ToString(VoiceIntent intent) {
    switch (intent) {
        case VoiceIntent::kWake: return "wake";
        case VoiceIntent::kTemp: return "temp";
        case VoiceIntent::kHumid: return "humid";
        case VoiceIntent::kLight: return "light";
        case VoiceIntent::kOccupancy: return "occupancy";
        case VoiceIntent::kStatus: return "status";
        case VoiceIntent::kChime: return "chime";
        case VoiceIntent::kSnapshot: return "snapshot";
        case VoiceIntent::kLock: return "lock";
        case VoiceIntent::kUnknown:
        case VoiceIntent::kCount:
            break;
    }
    return "unknown";
}

VoiceIntent ParseVoiceAction(const std::string &action) {
    for (const ActionEntry &entry : kActions) {
        if (action == entry.action) {
            return entry.intent;
        }
    }
    return VoiceIntent::kUnknown;
}

}  // namespace vehicle
