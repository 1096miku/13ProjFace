// IMU 换算纯逻辑的主机单元测试（不依赖 ESP-IDF）
//
// 编译与运行（Windows / MinGW）：
//   g++ -std=c++17 -Wall -Wextra -I main/vehicle test/imu_convert_test.cc -o build_host/imu_convert_test.exe
//   build_host/imu_convert_test.exe

#include <cmath>
#include <cstdio>

#include "imu_convert.h"

using namespace vehicle;

static int g_failures = 0;

#define CHECK(cond, what)                                     \
    do {                                                      \
        if (cond) {                                           \
            printf("  ok   %s\n", what);                      \
        } else {                                              \
            printf("  FAIL %s\n", what);                      \
            g_failures++;                                     \
        }                                                     \
    } while (0)

static bool Near(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) < eps;
}

int main() {
    printf("imu_convert\n");

    // 小端组装：低字节在前（QMI8658A 的 AX_L=0x35 在 AX_H=0x36 之前）
    CHECK(AssembleInt16(0x00, 0x01) == 256, "AssembleInt16(0x00,0x01) == 256");
    CHECK(AssembleInt16(0xFF, 0xFF) == -1, "AssembleInt16(0xFF,0xFF) == -1（负值补码）");
    CHECK(AssembleInt16(0x00, 0x80) == -32768, "AssembleInt16(0x00,0x80) == -32768（满量程负端）");
    CHECK(AssembleInt16(0xFF, 0x7F) == 32767, "AssembleInt16(0xFF,0x7F) == 32767（满量程正端）");

    // 换算：raw * full_scale / 32768
    CHECK(Near(RawToAccelG(32767, 8.0f), 7.99976f, 1e-4f), "±8g 满量程正端 ≈ +7.9998 g");
    CHECK(Near(RawToAccelG(-32768, 8.0f), -8.0f), "±8g 满量程负端 == -8 g");
    // 静止 1 g 对应的原始值（±8g 量程）：1 / 8 * 32768 = 4096
    CHECK(Near(RawToAccelG(4096, 8.0f), 1.0f), "±8g 下 raw=4096 → 1.000 g（静止基准）");
    CHECK(Near(RawToGyroDps(32767, 512.0f), 511.98f, 1e-2f), "±512dps 满量程正端 ≈ +512 dps");

    // 温度：整数部分取 TEMP_H（有符号），小数部分 TEMP_L / 256
    CHECK(Near(RawToTemperatureC(0x00, 0x1A), 26.0f), "TEMP_H=26, TEMP_L=0 → 26.00 ℃");
    CHECK(Near(RawToTemperatureC(0x80, 0x1A), 26.5f), "TEMP_L=128 → 26.50 ℃");
    CHECK(Near(RawToTemperatureC(0x00, 0xFF), -1.0f), "TEMP_H=0xFF（-1）→ -1.00 ℃");

    if (g_failures == 0) {
        printf("all passed\n");
        return 0;
    }
    printf("%d failure(s)\n", g_failures);
    return 1;
}
