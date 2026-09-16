#pragma once

// QMI8658A 原始值 → 物理量的换算。只依赖标准库，主机可单测；
// 设备侧由 qmi8658a.cc 调用，不在这里引入任何 ESP-IDF 头文件。

#include <cstdint>

namespace vehicle {

// 小端组装两个字节为一个有符号 16 位值（低字节在前）
inline int16_t AssembleInt16(uint8_t low, uint8_t high) {
    return static_cast<int16_t>((static_cast<uint16_t>(high) << 8) | low);
}

// 加速度换算：满量程 full_scale_g（±2/4/8/16 g）→ 单位 g
inline float RawToAccelG(int16_t raw, float full_scale_g) {
    return static_cast<float>(raw) * full_scale_g / 32768.0f;
}

// 陀螺换算：满量程 full_scale_dps（±16…2048 dps）→ 单位 dps
inline float RawToGyroDps(int16_t raw, float full_scale_dps) {
    return static_cast<float>(raw) * full_scale_dps / 32768.0f;
}

// 温度换算：TEMP_H 为有符号整数部分，TEMP_L/256 为小数部分
inline float RawToTemperatureC(uint8_t temp_l, uint8_t temp_h) {
    return static_cast<float>(static_cast<int8_t>(temp_h)) +
           static_cast<float>(temp_l) / 256.0f;
}

}  // namespace vehicle
