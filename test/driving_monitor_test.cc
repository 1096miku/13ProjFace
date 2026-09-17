// 行车状态判定的主机单元测试（不依赖 ESP-IDF，g++ 直接编译运行）
//
// 编译与运行（Windows / MinGW）：
//   g++ -std=c++17 -I main/vehicle test/driving_monitor_test.cc main/vehicle/driving_monitor.cc -o build_host/driving_monitor_test.exe
//   build_host/driving_monitor_test.exe

#include <cmath>
#include <cstdio>
#include <vector>

#include "driving_monitor.h"

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

// 测试台：固定 20 ms 采样周期，可注入任意三轴值
// ! 配置必须在构造 monitor 之前完成——monitor 持有的是配置的副本
static MonitorConfig MakeCfg(int32_t static_hold_ms, int32_t lock_hold_ms, int32_t wake_hold_ms) {
    MonitorConfig c;
    c.static_hold_ms = static_hold_ms;
    c.lock_hold_ms = lock_hold_ms;
    c.wake_hold_ms = wake_hold_ms;
    return c;
}

class Rig {
public:
    explicit Rig(int32_t static_hold_ms = 30000, int32_t lock_hold_ms = 120000, int32_t wake_hold_ms = 2000)
        : cfg_(MakeCfg(static_hold_ms, lock_hold_ms, wake_hold_ms)), monitor_(cfg_) {}

    void Feed(float ax, float ay, float az, int64_t dt_ms = 20) {
        t_ += dt_ms;
        ImuSample s;
        s.ts_ms = t_;
        s.ax = ax;
        s.ay = ay;
        s.az = az;
        monitor_.Feed(s);
    }

    void Calibrate(int samples = 50) {
        for (int i = 0; i < samples; i++) {
            Feed(0.0f, 0.0f, 1.0f);
        }
    }

    // 连续喂同一姿态若干帧（用于推进停车/锁车计时）
    void FeedStill(int frames) {
        for (int i = 0; i < frames; i++) {
            Feed(0.0f, 0.0f, 1.0f);
        }
    }

    void FeedMotion(int frames, float ax = 0.0f, float ay = 0.0f, float az = 1.3f) {
        for (int i = 0; i < frames; i++) {
            Feed(ax, ay, az);
        }
    }

    std::vector<Event> Drain() {
        std::vector<Event> out;
        Event e;
        while (monitor_.PopEvent(e)) {
            out.push_back(e);
        }
        return out;
    }

    MonitorConfig cfg_;
    DrivingMonitor monitor_;
    int64_t t_ = 1000;
};

static void TestDefaults() {
    printf("[默认阈值应与计划书一致]\n");
    MonitorConfig d;
    CHECK(d.accel_threshold == 0.35f, "急加速/急刹阈值 0.35 g");
    CHECK(d.turn_threshold == 0.30f, "急转弯阈值 0.30 g");
    CHECK(d.bump_threshold == 1.60f, "颠簸阈值 1.60 g");
    CHECK(d.crash_threshold == 2.50f, "碰撞阈值 2.50 g");
    CHECK(d.static_band == 0.06f, "静止带 0.06 g");
    CHECK(d.parked_motion_threshold == 0.25f, "锁车震动阈值 0.25 g");
    CHECK(d.static_hold_ms == 30000, "停车判定 30 s");
    CHECK(d.lock_hold_ms == 120000, "自动锁车 2 min");
}

static void TestCalibrationAndMotion() {
    printf("[标定与由静转动]\n");
    Rig rig;
    CHECK(rig.monitor_.state() == MotionState::kUncalibrated, "初始为标定中");
    rig.Calibrate();
    CHECK(rig.monitor_.calibrated(), "50 帧后完成标定");
    CHECK(rig.monitor_.state() == MotionState::kParked, "标定后为停车态");
    CHECK(rig.monitor_.EventCount(EventType::kMoving) == 0, "标定期不产生事件");

    rig.Feed(0.0f, 0.0f, 1.4f);  // 明显不是静止
    CHECK(rig.monitor_.state() == MotionState::kDriving, "非静止 → 行驶态");
    CHECK(rig.monitor_.EventCount(EventType::kMoving) == 1, "产生一条行驶事件");
}

static void TestDrivingEvents() {
    printf("[四类驾驶事件]\n");
    Rig rig;
    rig.Calibrate();
    rig.FeedMotion(20);  // 先进入行驶态

    // 急加速：+0.5 g
    int before = rig.monitor_.EventCount(EventType::kHardAccel);
    rig.Feed(0.5f, 0.0f, 1.0f);
    CHECK(rig.monitor_.EventCount(EventType::kHardAccel) == before + 1, "急加速被识别");
    // 冷却期内重复不应再次计数
    rig.Feed(0.5f, 0.0f, 1.0f, 100);
    CHECK(rig.monitor_.EventCount(EventType::kHardAccel) == before + 1, "冷却期内不重复计数");
    // 冷却结束后再次触发
    rig.Feed(0.5f, 0.0f, 1.0f, 4000);
    CHECK(rig.monitor_.EventCount(EventType::kHardAccel) == before + 2, "冷却结束后可再次触发");

    // 急刹车：-0.5 g
    before = rig.monitor_.EventCount(EventType::kHardBrake);
    rig.Feed(-0.5f, 0.0f, 1.0f, 4000);
    CHECK(rig.monitor_.EventCount(EventType::kHardBrake) == before + 1, "急刹车被识别");

    // 急转弯：+0.45 g 横向
    before = rig.monitor_.EventCount(EventType::kHardTurn);
    rig.Feed(0.0f, 0.45f, 1.0f, 4000);
    CHECK(rig.monitor_.EventCount(EventType::kHardTurn) == before + 1, "急转弯被识别");

    // 颠簸：合成变化量 2.0 g（< 碰撞阈值）
    before = rig.monitor_.EventCount(EventType::kBump);
    int crash_before = rig.monitor_.EventCount(EventType::kCrash);
    rig.Feed(0.0f, 0.0f, 3.0f, 4000);
    CHECK(rig.monitor_.EventCount(EventType::kBump) == before + 1, "颠簸被识别");
    CHECK(rig.monitor_.EventCount(EventType::kCrash) == crash_before, "颠簸不误判为碰撞");

    // 碰撞：合成变化量 3.0 g
    crash_before = rig.monitor_.EventCount(EventType::kCrash);
    rig.Feed(0.0f, 0.0f, 4.0f, 4000);
    CHECK(rig.monitor_.EventCount(EventType::kCrash) == crash_before + 1, "碰撞被识别");

    // 阈值边界：恰好等于阈值应触发（>= 判定）
    before = rig.monitor_.EventCount(EventType::kHardAccel);
    rig.Feed(0.35f, 0.0f, 1.0f, 4000);
    CHECK(rig.monitor_.EventCount(EventType::kHardAccel) == before + 1, "恰好等于阈值即触发");
    // 略低于阈值不应触发
    before = rig.monitor_.EventCount(EventType::kHardAccel);
    rig.Feed(0.34f, 0.0f, 1.0f, 4000);
    CHECK(rig.monitor_.EventCount(EventType::kHardAccel) == before, "低于阈值不触发");
}

static void TestParkAndLock() {
    printf("[停车判定与锁车监测（缩短时长便于快速推进）]\n");
    Rig rig(/*static_hold_ms=*/200, /*lock_hold_ms=*/400, /*wake_hold_ms=*/100);

    rig.Calibrate();
    rig.FeedMotion(20);  // 行驶
    CHECK(rig.monitor_.state() == MotionState::kDriving, "当前为行驶态");

    // 静止不足 static_hold：不应判定停车
    rig.FeedStill(5);
    CHECK(rig.monitor_.EventCount(EventType::kParked) == 0, "静止不足时长不判停车");

    // 补足静止时长
    rig.FeedStill(10);
    CHECK(rig.monitor_.EventCount(EventType::kParked) == 1, "静止达标 → 停车事件");
    CHECK(rig.monitor_.state() == MotionState::kParked, "状态切到停车");

    // 继续静止，自动进入锁车监测
    rig.FeedStill(30);
    CHECK(rig.monitor_.state() == MotionState::kLockedMonitor, "停车持续 → 自动进入锁车监测");

    // 锁车态异常震动
    int before = rig.monitor_.EventCount(EventType::kMotionWhileParked);
    rig.Feed(0.0f, 0.0f, 1.3f);  // 变化量 0.3 g > 0.25 g
    CHECK(rig.monitor_.EventCount(EventType::kMotionWhileParked) == before + 1, "锁车态异常震动被记录");
    CHECK(rig.monitor_.state() == MotionState::kLockedMonitor, "单次震动不改变锁车态");

    // 持续运动 → 判定重新行驶并解锁
    rig.FeedMotion(10, 0.0f, 0.0f, 1.3f);
    CHECK(rig.monitor_.state() == MotionState::kDriving, "持续运动 → 回到行驶态");
    CHECK(rig.monitor_.EventCount(EventType::kMoving) >= 2, "产生重新行驶事件");
    CHECK(!rig.monitor_.lock_requested(), "自动解锁");
}

// 真机的"行驶"是断续的动态量：偶尔有峰值，多数帧很小。
// ! 这条钉住一个真机 bug（用户实测：进了锁车监测就再也回不到行驶）：
// ! 原判据是"mag ≥ 0.25 g 连续 wake_hold_ms"，而正常起步/行驶的动态量根本达不到
// ! 持续 2 s 超过 0.25 g，于是车开走了状态机仍锁在"锁车监测"。
static void TestLockWakeOnGentleMotion() {
    printf("[锁车态：轻微但持续的非静止也应判定为重新行驶]\n");
    Rig rig(/*static_hold_ms=*/200, /*lock_hold_ms=*/400, /*wake_hold_ms=*/100);
    rig.Calibrate();
    rig.FeedMotion(20);
    rig.FeedStill(45);   // 20 帧判停车 + 20 帧判锁车，留出余量（别卡在边界上）
    CHECK(rig.monitor_.state() == MotionState::kLockedMonitor, "先在锁车态");
    rig.Drain();

    // az = 1.10 → 动态合成量 0.10 g（**低于** 0.25 g 的异常震动门限），
    // 但单轴偏差 0.10 g > 0.06 g 静止带 → 按"不再静止"就该醒。
    const int before = rig.monitor_.EventCount(EventType::kMoving);
    rig.FeedMotion(10, 0.0f, 0.0f, 1.10f);
    CHECK(rig.monitor_.EventCount(EventType::kMotionWhileParked) == 0, "低于 0.25 g 不报异常震动");
    CHECK(rig.monitor_.state() == MotionState::kDriving, "持续非静止 ≥ wake_hold → 回到行驶态");
    CHECK(rig.monitor_.EventCount(EventType::kMoving) == before + 1, "产生一条重新行驶事件");
    CHECK(!rig.monitor_.lock_requested(), "自动解锁");

    // 反向用例：一直静止绝不能自己醒过来
    Rig rig2(/*static_hold_ms=*/200, /*lock_hold_ms=*/400, /*wake_hold_ms=*/100);
    rig2.Calibrate();
    rig2.FeedMotion(20);
    rig2.FeedStill(45);
    CHECK(rig2.monitor_.state() == MotionState::kLockedMonitor, "第二组也进锁车态");
    rig2.FeedStill(500);
    CHECK(rig2.monitor_.state() == MotionState::kLockedMonitor, "一直静止则保持锁车态");
}

static void TestManualLock() {
    printf("[手动锁车]\n");
    Rig rig;
    rig.Calibrate();
    CHECK(rig.monitor_.state() == MotionState::kParked, "初始停车态");
    rig.monitor_.RequestLock(true);
    CHECK(rig.monitor_.state() == MotionState::kLockedMonitor, "停车态下手动锁车立即生效");
    CHECK(rig.monitor_.lock_requested(), "锁车请求已记录");
    rig.monitor_.RequestLock(false);
    CHECK(rig.monitor_.state() == MotionState::kParked, "解锁回到停车态");
    CHECK(!rig.monitor_.lock_requested(), "锁车请求已清除");
}

static void TestDefaultTimingEndToEnd() {
    printf("[默认阈值端到端：30 s 停车 / 2 min 自动锁车]\n");
    Rig rig;  // 全部默认值
    rig.Calibrate();
    rig.FeedMotion(20);
    CHECK(rig.monitor_.state() == MotionState::kDriving, "进入行驶态");

    // 30 s 停车判定：静态阈值未到时先确认不触发
    rig.FeedStill(1400);  // 28 s
    CHECK(rig.monitor_.EventCount(EventType::kParked) == 0, "28 s 时尚未判停车");
    rig.FeedStill(200);  // 再 4 s，累计 32 s
    CHECK(rig.monitor_.EventCount(EventType::kParked) == 1, "超过 30 s 判定停车");
    CHECK(rig.monitor_.state() == MotionState::kParked, "状态切到停车");

    // 2 min 自动锁车：先确认 100 s 时不锁
    rig.FeedStill(5000);  // 100 s
    CHECK(rig.monitor_.state() == MotionState::kParked, "停车 100 s 时尚未自动锁车");
    rig.FeedStill(2000);  // 再 40 s，累计 140 s
    CHECK(rig.monitor_.state() == MotionState::kLockedMonitor, "停车超过 2 min 自动进入锁车监测");
}

static void TestEventQueue() {
    printf("[事件队列]\n");
    Rig rig;
    rig.Calibrate();
    rig.FeedMotion(20);
    rig.Drain();

    rig.Feed(0.5f, 0.0f, 1.0f);
    auto events = rig.Drain();
    bool found = false;
    for (auto &e : events) {
        if (e.type == EventType::kHardAccel) {
            found = true;
            CHECK(e.value >= 0.5f - 0.001f && e.value <= 0.5f + 0.001f, "事件携带触发值");
            CHECK(e.ts_ms > 0, "事件携带时间戳");
        }
    }
    CHECK(found, "可取出急加速事件");

    // 队列容量：超量写入不应崩溃，且仍能取出事件
    for (int i = 0; i < 100; i++) {
        rig.Feed(0.5f, 0.0f, 1.0f, 5000);
    }
    int drained = 0;
    Event e;
    while (rig.monitor_.PopEvent(e)) {
        drained++;
    }
    CHECK(drained > 0, "溢出后队列仍可读出事件");

    rig.monitor_.ResetCounters();
    CHECK(rig.monitor_.TotalEventCount() == 0, "计数可重置");
}

static void TestCalibrationRejectsStartupTransient() {
    printf("[标定期滤掉传感器上电暂态]\n");
    Rig rig;
    // QMI8658A 使能后的实测暂态（合成量，单位 g）：先冲到 2.09 再回落到 1.00
    const float transient[] = {2.09f, 2.09f, 2.09f, 2.09f, 2.09f, 2.09f, 2.07f, 1.76f, 1.76f,
                               1.76f, 1.76f, 1.23f, 0.84f, 0.84f, 0.84f, 0.89f, 0.98f};
    for (float m : transient) {
        rig.Feed(0.0f, 0.0f, m);
    }
    CHECK(!rig.monitor_.calibrated(), "暂态帧不计入基线（凑不满 50 帧）");

    rig.Calibrate();
    CHECK(rig.monitor_.calibrated(), "暂态结束后仍能完成标定");

    // 若暂态混入基线，合成量会掉到 0.75 左右；正确应为 ≈ 1.00
    const float bx = rig.monitor_.baseline_ax();
    const float by = rig.monitor_.baseline_ay();
    const float bz = rig.monitor_.baseline_az();
    CHECK(std::fabs(std::sqrt(bx * bx + by * by + bz * bz) - 1.0f) < 0.01f, "基线合成量 ≈ 1.000 g");
    CHECK(std::fabs(bz - 1.0f) < 0.01f, "基线 az ≈ 1.000 g");

    // 基线正确后，静止帧不应产生任何事件
    const int before = rig.monitor_.TotalEventCount();
    rig.FeedStill(200);
    CHECK(rig.monitor_.TotalEventCount() == before, "静止时不再误报事件");
}

int main() {
    printf("=== 行车状态判定单元测试 ===\n");
    TestDefaults();
    TestCalibrationAndMotion();
    TestCalibrationRejectsStartupTransient();
    TestDrivingEvents();
    TestParkAndLock();
    TestLockWakeOnGentleMotion();
    TestManualLock();
    TestDefaultTimingEndToEnd();
    TestEventQueue();

    printf("\n=== 结果：%s（失败 %d 项）===\n", g_failures == 0 ? "全部通过" : "存在失败", g_failures);
    return g_failures == 0 ? 0 : 1;
}
