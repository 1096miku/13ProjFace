#include "voice_reply.h"

#include <cmath>

namespace vehicle {

namespace {

void AppendNumber(std::vector<ClipId> &out, int value) {
    const std::vector<ClipId> digits = NumberToClips(value);
    out.insert(out.end(), digits.begin(), digits.end());
}

}  // namespace

const char *ToString(ClipId id) {
    switch (id) {
        case ClipId::kNone: return "none";
        case ClipId::kD0: return "d0";
        case ClipId::kD1: return "d1";
        case ClipId::kD2: return "d2";
        case ClipId::kD3: return "d3";
        case ClipId::kD4: return "d4";
        case ClipId::kD5: return "d5";
        case ClipId::kD6: return "d6";
        case ClipId::kD7: return "d7";
        case ClipId::kD8: return "d8";
        case ClipId::kD9: return "d9";
        case ClipId::kD10: return "d10";
        case ClipId::kUnitDegree: return "unit_degree";
        case ClipId::kUnitTimes: return "unit_times";
        case ClipId::kQTemp: return "q_temp";
        case ClipId::kQHumid: return "q_humid";
        case ClipId::kQEvents: return "q_events";
        case ClipId::kQStatusOk: return "q_status_ok";
        case ClipId::kQOnline: return "q_online";
        case ClipId::kQOffline: return "q_offline";
        case ClipId::kQNoAlert: return "q_no_alert";
        case ClipId::kQPending: return "q_pending";
        case ClipId::kQLightDark: return "q_light_dark";
        case ClipId::kQLightDim: return "q_light_dim";
        case ClipId::kQLightMid: return "q_light_mid";
        case ClipId::kQLightBright: return "q_light_bright";
        case ClipId::kQLightStrong: return "q_light_strong";
        case ClipId::kChime: return "chime";
        case ClipId::kSnapStart: return "snap_start";
        case ClipId::kSnapDone: return "snap_done";
        case ClipId::kLockEntered: return "lock_entered";
        case ClipId::kLockExited: return "lock_exited";
        case ClipId::kEvHardAccel: return "ev_hard_accel";
        case ClipId::kEvHardBrake: return "ev_hard_brake";
        case ClipId::kEvHardTurn: return "ev_hard_turn";
        case ClipId::kEvBump: return "ev_bump";
        case ClipId::kEvCrash: return "ev_crash";
        case ClipId::kEvParked: return "ev_parked";
        case ClipId::kEvDriving: return "ev_driving";
        case ClipId::kEvMotionParked: return "ev_motion_parked";
        case ClipId::kCount: break;
    }
    return "none";
}

std::vector<ClipId> NumberToClips(int value) {
    // ! 没有"零下"片段（设计文档 §6.4）：负数只上屏、不播报
    if (value < 0) {
        return {};
    }
    if (value < 10) {
        return {static_cast<ClipId>(static_cast<int>(ClipId::kD0) + value)};
    }
    if (value < 20) {
        if (value == 10) {
            return {ClipId::kD10};
        }
        return {ClipId::kD10, static_cast<ClipId>(static_cast<int>(ClipId::kD0) + (value % 10))};
    }
    if (value < 100) {
        std::vector<ClipId> out;
        out.push_back(static_cast<ClipId>(static_cast<int>(ClipId::kD0) + (value / 10)));
        out.push_back(ClipId::kD10);
        if (value % 10 != 0) {
            out.push_back(static_cast<ClipId>(static_cast<int>(ClipId::kD0) + (value % 10)));
        }
        return out;
    }
    // > 逐位兜底（本项目用不到：温度/湿度/事件数都在 0–99，光照不读数字）
    std::vector<ClipId> out;
    int n = value;
    int digits[8] = {};
    int count = 0;
    while (n > 0 && count < 8) {
        digits[count++] = n % 10;
        n /= 10;
    }
    for (int i = count - 1; i >= 0; i--) {
        out.push_back(static_cast<ClipId>(static_cast<int>(ClipId::kD0) + digits[i]));
    }
    return out;
}

ClipId EventClip(EventType type) {
    switch (type) {
        case EventType::kHardAccel: return ClipId::kEvHardAccel;
        case EventType::kHardBrake: return ClipId::kEvHardBrake;
        case EventType::kHardTurn: return ClipId::kEvHardTurn;
        case EventType::kBump: return ClipId::kEvBump;
        case EventType::kCrash: return ClipId::kEvCrash;
        case EventType::kParked: return ClipId::kEvParked;
        case EventType::kMoving: return ClipId::kEvDriving;
        case EventType::kMotionWhileParked: return ClipId::kEvMotionParked;
        case EventType::kCount: break;
    }
    return ClipId::kNone;
}

ClipId OccupancyClip(bool pending) {
    return pending ? ClipId::kQPending : ClipId::kQNoAlert;
}

std::vector<ClipId> TemperatureClips(float temp_c, bool valid) {
    std::vector<ClipId> out{ClipId::kQTemp};
    if (!valid) {
        return out;
    }
    // > 播报用"最近整数"：26.4 → 二十六（没有"点四"这类片段，屏幕上是准确值）
    AppendNumber(out, static_cast<int>(lroundf(temp_c)));
    out.push_back(ClipId::kUnitDegree);
    return out;
}

std::vector<ClipId> HumidityClips(float humidity_pct, bool valid) {
    // > 「车内湿度百分之」是 q_humid 这一段的原文，所以后面直接接数字，不拼"百分之"
    std::vector<ClipId> out{ClipId::kQHumid};
    if (!valid) {
        return out;
    }
    AppendNumber(out, static_cast<int>(lroundf(humidity_pct)));
    return out;
}

ClipId LightClip(int32_t lux) {
    switch (BucketLight(lux)) {
        case LightLevel::kDark: return ClipId::kQLightDark;
        case LightLevel::kDim: return ClipId::kQLightDim;
        case LightLevel::kMedium: return ClipId::kQLightMid;
        case LightLevel::kBright: return ClipId::kQLightBright;
        case LightLevel::kStrong: return ClipId::kQLightStrong;
    }
    return ClipId::kQLightMid;
}

std::vector<ClipId> StatusClips(bool online, int32_t events_total) {
    std::vector<ClipId> out{ClipId::kQStatusOk, online ? ClipId::kQOnline : ClipId::kQOffline, ClipId::kQEvents};
    AppendNumber(out, events_total);
    out.push_back(ClipId::kUnitTimes);
    return out;
}

}  // namespace vehicle
