// 环境数据源（模拟）的主机单元测试
//
// 编译与运行：
//   g++ -std=c++17 -Wall -Wextra -I main/vehicle test/environment_sim_test.cc main/vehicle/environment_sensor.cc -o build_host/environment_sim_test.exe
//   build_host/environment_sim_test.exe

#include <cstdio>

#include "environment_sensor.h"

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
    const float d = a - b;
    return (d < 0 ? -d : d) <= eps;
}

int main() {
    printf("environment_sim\n");

    // 光照分档边界
    CHECK(BucketLight(0) == LightLevel::kDark, "0 lux → 很暗");
    CHECK(BucketLight(9) == LightLevel::kDark, "9 lux → 很暗");
    CHECK(BucketLight(10) == LightLevel::kDim, "10 lux → 偏暗");
    CHECK(BucketLight(49) == LightLevel::kDim, "49 lux → 偏暗");
    CHECK(BucketLight(50) == LightLevel::kMedium, "50 lux → 适中");
    CHECK(BucketLight(299) == LightLevel::kMedium, "299 lux → 适中");
    CHECK(BucketLight(300) == LightLevel::kBright, "300 lux → 明亮");
    CHECK(BucketLight(999) == LightLevel::kBright, "999 lux → 明亮");
    CHECK(BucketLight(1000) == LightLevel::kStrong, "1000 lux → 很强");
    CHECK(BucketLight(100000) == LightLevel::kStrong, "100000 lux → 很强");
    CHECK(ToString(LightLevel::kDark) != nullptr, "ToString 不返回空指针");

    SimulatedSensor sensor;
    CHECK(sensor.name() != nullptr, "模拟源有名字");

    // t=0 时正弦值为 0 → 读数等于基线
    EnvReading r;
    CHECK(sensor.Read(0, r), "读取成功");
    CHECK(r.valid && r.simulated, "标记为有效且来自模拟源");
    CHECK(r.ts_ms == 0, "ts_ms 原样回填");
    CHECK(Near(r.temp_c, 26.0f), "t=0 温度等于基线 26.0");
    CHECK(Near(r.humidity_pct, 48.0f), "t=0 湿度等于基线 48.0");
    CHECK(r.lux == 320, "t=0 光照等于基线 320");

    // 同一时刻必须可复现（真机 UI 每秒读一次，不能自己跳）
    EnvReading a;
    EnvReading b;
    sensor.Read(123456, a);
    sensor.Read(123456, b);
    CHECK(Near(a.temp_c, b.temp_c) && Near(a.humidity_pct, b.humidity_pct) && a.lux == b.lux,
          "同一 now_ms 两次读数一致");

    // 扫一遍一个完整周期，读数必须始终落在基线 ± 幅度内
    bool in_range = true;
    bool changed = false;
    EnvReading prev;
    sensor.Read(0, prev);
    for (int64_t t = 0; t <= 600000; t += 1000) {
        EnvReading cur;
        if (!sensor.Read(t, cur)) {
            in_range = false;
            break;
        }
        if (cur.temp_c < 26.0f - 1.5f || cur.temp_c > 26.0f + 1.5f) in_range = false;
        if (cur.humidity_pct < 48.0f - 4.0f || cur.humidity_pct > 48.0f + 4.0f) in_range = false;
        if (cur.lux < 320 - 180 || cur.lux > 320 + 180) in_range = false;
        if (cur.temp_c != prev.temp_c || cur.lux != prev.lux) changed = true;
        prev = cur;
    }
    CHECK(in_range, "一个周期内所有读数都在基线 ± 幅度内");
    CHECK(changed, "读数确实在缓变（不是常量）");

    // 自定义配置生效
    SimulatedSensor::Config cfg;
    cfg.temp_base_c = 30.0f;
    cfg.humidity_base_pct = 60.0f;
    cfg.lux_base = 100;
    cfg.temp_amplitude_c = 0.0f;
    cfg.humidity_amplitude_pct = 0.0f;
    cfg.lux_amplitude = 0;
    SimulatedSensor fixed(cfg);
    EnvReading f;
    fixed.Read(987654, f);
    CHECK(Near(f.temp_c, 30.0f) && Near(f.humidity_pct, 60.0f) && f.lux == 100,
          "幅度为 0 时读数为常量基线");

    if (g_failures == 0) {
        printf("all passed\n");
        return 0;
    }
    printf("%d failure(s)\n", g_failures);
    return 1;
}
