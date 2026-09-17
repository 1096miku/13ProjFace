#pragma once

#include "i2c_device.h"
#include "vehicle_types.h"

// QMI8658A 六轴 IMU 驱动
//
// 硬件事实：I2C 7 位地址 0x6A（SDO/SA0 悬空）；INT1/INT2 未接 → 只能轮询。
// ! 不复用 I2cDevice::ReadReg/ReadRegs：它们内部走 ESP_ERROR_CHECK，
// ! IMU 不在线时会直接 abort 整机。这里自己读写并返回三态结果，让上层能降级。
class Qmi8658a : public I2cDevice {
public:
    static constexpr uint8_t kAddr = 0x6A;
    static constexpr uint8_t kWhoAmIExpected = 0x05;

    // 一次采样的结果
    enum class ReadResult {
        kOk,        // 读到一帧有效数据
        kNotReady,  // 数据尚未就绪（正常情况，不算错误）
        kError,     // I2C 通信失败
    };

    explicit Qmi8658a(i2c_master_bus_handle_t i2c_bus) : I2cDevice(i2c_bus, kAddr) {}

    // 复位 + 配置（±8 g / ±512 dps / 112.1 Hz / LPF 开 / 使能 A+G）
    // 返回 false 表示 IMU 不在线或配置失败，上层应降级而不是重启
    bool Init();

    ReadResult ReadSample(vehicle::ImuSample& out);

private:
    bool WriteChecked(uint8_t reg, uint8_t value);
    bool ReadChecked(uint8_t reg, uint8_t* buffer, size_t length);
};
