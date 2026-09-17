#pragma once

// 环境数据源抽象：温湿度 / 光照。
//
// 本板上没有温湿度与光照传感器（计划书 §2.2：I2C 地址已被 0x19/0x18/0x41/0x6A/0x38/0x21 占满），
// 所以先用模拟源把「界面显示 / 周期上报 / 语音查询」三条链路真实跑通；
// 采购到位后只新增一个 EnvironmentSensor 实现（SHT3x @0x44/0x45 + BH1750 @0x23），上层不改。
//
// 纯逻辑，不依赖 ESP-IDF，可用主机 g++ 单元测试。

#include <cstdint>

namespace vehicle {

struct EnvReading {
    int64_t ts_ms = 0;          // 采样时刻（开机以来的毫秒）
    float temp_c = 0.0f;        // 摄氏度
    float humidity_pct = 0.0f;  // 相对湿度 %
    int32_t lux = 0;            // 光照强度
    bool valid = false;         // false = 本帧不可用，界面应显示 "--"
    bool simulated = true;      // true = 数据来自模拟源（界面必须标"模拟"）
};

// 光照档位：播报不读数字、只播档位（设计文档 §6.4），具体数值上屏
enum class LightLevel : uint8_t {
    kDark = 0,  // 很暗  < 10 lux
    kDim,       // 偏暗  < 50
    kMedium,    // 适中  < 300
    kBright,    // 明亮  < 1000
    kStrong,    // 很强  >= 1000
};

LightLevel BucketLight(int32_t lux);
const char *ToString(LightLevel level);

class EnvironmentSensor {
public:
    virtual ~EnvironmentSensor() = default;

    // 读一帧。返回 false 表示本次读取失败（out 不可信，调用方应保持上一帧）。
    virtual bool Read(int64_t now_ms, EnvReading &out) = 0;
    // 数据源标识（"sim" / "sht3x"），用于界面标注与日志
    virtual const char *name() const = 0;
};

// 模拟源：围绕可配基线做有界缓变。
// ! 只依赖 now_ms，不做内部状态机——同一个 now_ms 必然得到同一个读数，
// ! 主机测试可以断言具体数值，真机上也不会因为读取频率变化而跳变。
class SimulatedSensor : public EnvironmentSensor {
public:
    struct Config {
        float temp_base_c = 26.0f;             // 基线温度（℃）
        float humidity_base_pct = 48.0f;       // 基线湿度（%）
        int32_t lux_base = 320;                // 基线光照（lux）
        float temp_amplitude_c = 1.5f;         // 温度缓变幅度（±）
        float humidity_amplitude_pct = 4.0f;   // 湿度缓变幅度（±）
        int32_t lux_amplitude = 180;           // 光照缓变幅度（±）
        int32_t temp_period_ms = 300000;       // 5 min 走一个正弦周期
        int32_t humidity_period_ms = 420000;   // 7 min
        int32_t lux_period_ms = 600000;        // 10 min
    };

    SimulatedSensor() = default;
    explicit SimulatedSensor(const Config &cfg) : cfg_(cfg) {}

    bool Read(int64_t now_ms, EnvReading &out) override;
    const char *name() const override { return "sim"; }

private:
    Config cfg_{};
};

}  // namespace vehicle
