#include "environment_sensor.h"

#include <cmath>

namespace vehicle {

namespace {
constexpr double kTwoPi = 6.283185307179586;
}  // namespace

LightLevel BucketLight(int32_t lux) {
    if (lux < 10) {
        return LightLevel::kDark;
    }
    if (lux < 50) {
        return LightLevel::kDim;
    }
    if (lux < 300) {
        return LightLevel::kMedium;
    }
    if (lux < 1000) {
        return LightLevel::kBright;
    }
    return LightLevel::kStrong;
}

const char *ToString(LightLevel level) {
    switch (level) {
        case LightLevel::kDark: return "很暗";
        case LightLevel::kDim: return "偏暗";
        case LightLevel::kMedium: return "适中";
        case LightLevel::kBright: return "明亮";
        case LightLevel::kStrong: return "很强";
    }
    return "未知";
}

bool SimulatedSensor::Read(int64_t now_ms, EnvReading &out) {
    // > 整数取模 + sin 生成有界缓变：周期可配、结果可复现、无需内部状态。
    const auto wave = [now_ms](int32_t period_ms) -> double {
        if (period_ms <= 0) {
            return 0.0;
        }
        const int64_t phase = now_ms % period_ms;
        return std::sin(kTwoPi * static_cast<double>(phase) / static_cast<double>(period_ms));
    };

    out.ts_ms = now_ms;
    out.valid = true;
    out.simulated = true;
    out.temp_c = cfg_.temp_base_c + cfg_.temp_amplitude_c * static_cast<float>(wave(cfg_.temp_period_ms));
    out.humidity_pct =
        cfg_.humidity_base_pct + cfg_.humidity_amplitude_pct * static_cast<float>(wave(cfg_.humidity_period_ms));
    out.lux = cfg_.lux_base + static_cast<int32_t>(static_cast<double>(cfg_.lux_amplitude) * wave(cfg_.lux_period_ms));
    return true;
}

}  // namespace vehicle
