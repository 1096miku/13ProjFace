#include "qmi8658a.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "imu_convert.h"

#define TAG "Qmi8658a"

namespace {

// 寄存器（见计划文档"前置事实"：两处独立来源已核对）
constexpr uint8_t kRegWhoAmI    = 0x00;
constexpr uint8_t kRegCtrl1     = 0x02;
constexpr uint8_t kRegCtrl2     = 0x03;
constexpr uint8_t kRegCtrl3     = 0x04;
constexpr uint8_t kRegCtrl5     = 0x06;
constexpr uint8_t kRegCtrl7     = 0x08;
constexpr uint8_t kRegStatusInt = 0x2D;
constexpr uint8_t kRegAxL       = 0x35;
constexpr uint8_t kRegReset     = 0x60;

constexpr uint8_t kSoftReset    = 0xB0;
constexpr uint8_t kCtrl1AddrAi  = 0x40;       // ADDR_AI=1（多字节读地址自增）；BE=0 → 小端
constexpr uint8_t kAccelFs8g    = 0x02 << 4;  // AFS=010 → ±8 g
constexpr uint8_t kGyroFs512dps = 0x05 << 4;  // GFS=101 → ±512 dps（本项目不用陀螺，只留档）
constexpr uint8_t kOdr112_1Hz   = 0x06;       // ODR=0110 → 112.1 Hz
constexpr uint8_t kCtrl5LpfOn   = 0x35;       // 加速度 + 陀螺 LPF 使能，模式 3.63%
constexpr uint8_t kEnableAccelGyro = 0x03;    // CTRL7: G_EN | A_EN
constexpr uint8_t kStatusDataAvail = 0x01;

// ! 真机实测（2026-09-16，快速轮询统计）：数据确实以 ~100 Hz 刷新，但 STATUSINT 的 Avail 位
// ! 只是每个采样周期里约 1.94 ms 的窄脉冲（Avail 占空比 19%，774 次/秒 ≈ 100 × 7.7），
// ! 两个脉冲之间有 ~8 ms 的间隙。所以"查一次没有就返回 kNotReady"会丢掉大部分采样机会；
// ! 只忙等 5 ms 又有约 30% 会整段落在间隙里（实测 miss 27%）。
// ! 这里按 1 ms 间隔重试、覆盖一个完整周期（~10 ms）：既不空转占满共享 I2C 总线，
// ! 又能保证拿到的永远是刚更新的那一帧。
constexpr int kAvailPollLimit = 12;

constexpr float kAccelFullScaleG  = 8.0f;
constexpr float kGyroFullScaleDps = 512.0f;

constexpr int kI2cTimeoutMs = 100;

}  // namespace

bool Qmi8658a::WriteChecked(uint8_t reg, uint8_t value) {
    const uint8_t buffer[2] = {reg, value};
    return i2c_master_transmit(i2c_device_, buffer, sizeof(buffer), kI2cTimeoutMs) == ESP_OK;
}

bool Qmi8658a::ReadChecked(uint8_t reg, uint8_t* buffer, size_t length) {
    return i2c_master_transmit_receive(i2c_device_, &reg, 1, buffer, length, kI2cTimeoutMs) == ESP_OK;
}

bool Qmi8658a::Init() {
    uint8_t who = 0;
    if (!ReadChecked(kRegWhoAmI, &who, 1)) {
        ESP_LOGE(TAG, "读 WHO_AM_I 失败：IMU 未响应（确认 0x%02X 在线、I2C 总线已建）", kAddr);
        return false;
    }
    if (who != kWhoAmIExpected) {
        ESP_LOGE(TAG, "WHO_AM_I=0x%02X，期望 0x%02X（可能不是 QMI8658A）", who, kWhoAmIExpected);
        return false;
    }

    // 软复位：写 0xB0 后必须等芯片内部完成，数据手册要求 ≥15 ms，这里给 100 ms 保险
    if (!WriteChecked(kRegReset, kSoftReset)) {
        ESP_LOGE(TAG, "软复位写入失败");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    if (!WriteChecked(kRegCtrl1, kCtrl1AddrAi) ||
        !WriteChecked(kRegCtrl2, kAccelFs8g | kOdr112_1Hz) ||
        !WriteChecked(kRegCtrl3, kGyroFs512dps | kOdr112_1Hz) ||
        !WriteChecked(kRegCtrl5, kCtrl5LpfOn) ||
        !WriteChecked(kRegCtrl7, kEnableAccelGyro)) {
        ESP_LOGE(TAG, "配置寄存器写入失败");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_LOGI(TAG, "初始化完成：WHO_AM_I=0x%02X，±%.0f g / ±%.0f dps / 112.1 Hz", who, kAccelFullScaleG,
             kGyroFullScaleDps);
    return true;
}

Qmi8658a::ReadResult Qmi8658a::ReadSample(vehicle::ImuSample& out) {
    uint8_t status = 0;
    bool available = false;
    for (int i = 0; i < kAvailPollLimit; i++) {
        if (!ReadChecked(kRegStatusInt, &status, 1)) {
            return ReadResult::kError;
        }
        if ((status & kStatusDataAvail) != 0) {
            available = true;
            break;
        }
        vTaskDelay(1);   // 1 tick ≈ 1 ms；不要忙等，避免占满共享 I2C 总线
    }
    if (!available) {
        return ReadResult::kNotReady;
    }

    uint8_t raw[12] = {};
    if (!ReadChecked(kRegAxL, raw, sizeof(raw))) {
        return ReadResult::kError;
    }

    out.ts_ms = esp_timer_get_time() / 1000;
    out.ax = vehicle::RawToAccelG(vehicle::AssembleInt16(raw[0], raw[1]), kAccelFullScaleG);
    out.ay = vehicle::RawToAccelG(vehicle::AssembleInt16(raw[2], raw[3]), kAccelFullScaleG);
    out.az = vehicle::RawToAccelG(vehicle::AssembleInt16(raw[4], raw[5]), kAccelFullScaleG);
    out.gx = vehicle::RawToGyroDps(vehicle::AssembleInt16(raw[6], raw[7]), kGyroFullScaleDps);
    out.gy = vehicle::RawToGyroDps(vehicle::AssembleInt16(raw[8], raw[9]), kGyroFullScaleDps);
    out.gz = vehicle::RawToGyroDps(vehicle::AssembleInt16(raw[10], raw[11]), kGyroFullScaleDps);
    return ReadResult::kOk;
}
