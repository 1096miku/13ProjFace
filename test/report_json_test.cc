// 上报 JSON（事件/环境/状态）与 seq 解析的主机单元测试
//
// 编译与运行：
//   g++ -std=c++17 -Wall -Wextra -I main/vehicle test/report_json_test.cc main/vehicle/event_json.cc -o build_host/report_json_test.exe
//   build_host\report_json_test.exe

#include <cstdio>
#include <string>

#include "event_json.h"

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

int main() {
    printf("report_json\n");

    EventRecord record;
    record.seq = 12;
    record.event.type = EventType::kHardBrake;
    record.event.ts_ms = 1758000000123LL;
    record.event.value = -0.52f;

    // > 落盘格式**必须与 D5 逐字节一致**：/events 与 events.log 的既有测试都是按它写的
    CHECK(EventToJson(record) ==
              "{\"seq\":12,\"type\":\"hard_brake\",\"ts_ms\":1758000000123,\"value\":-0.52}",
          "不带参 = 落盘格式（与 D5 一致）");

    // > 上报格式多 dev 与 boot：幂等键 = dev + boot + seq
    // > （seq 每次重启从 1 开始，只用 dev+seq 会把不同开机的事件判成同一条）
    CHECK(EventToJson(record, "A1B2C3D4E5F6", 3) ==
              "{\"seq\":12,\"dev\":\"A1B2C3D4E5F6\",\"boot\":3,\"type\":\"hard_brake\","
              "\"ts_ms\":1758000000123,\"value\":-0.52}",
          "带参 = 上报格式");

    // 从落盘行里取 seq（补传游标用）
    CHECK(ParseSeqFromJsonLine("{\"seq\":12,\"type\":\"hard_brake\",\"ts_ms\":1,\"value\":-0.52}") == 12,
          "解析 seq=12");
    CHECK(ParseSeqFromJsonLine("{\"seq\":1,\"type\":\"bump\",\"ts_ms\":9,\"value\":1.90}") == 1, "解析 seq=1");
    CHECK(ParseSeqFromJsonLine("not json") == -1, "非 JSON → -1");
    CHECK(ParseSeqFromJsonLine("") == -1, "空行 → -1");
    CHECK(ParseSeqFromJsonLine("{\"type\":\"bump\"}") == -1, "没有 seq → -1");
    CHECK(ParseSeqFromJsonLine("{\"seq\":,\"type\":\"bump\"}") == -1, "seq 后面不是数字 → -1");

    // 运动状态用 ASCII 标识（中文只给屏幕用；手机端/云端要稳定标识）
    CHECK(std::string(MotionStateId(MotionState::kParked)) == "parked", "parked");
    CHECK(std::string(MotionStateId(MotionState::kDriving)) == "driving", "driving");
    CHECK(std::string(MotionStateId(MotionState::kLockedMonitor)) == "locked", "locked");
    CHECK(std::string(MotionStateId(MotionState::kUncalibrated)) == "uncalibrated", "uncalibrated");

    EnvReading env;
    env.ts_ms = 123456;
    env.temp_c = 26.4f;
    env.humidity_pct = 48.0f;
    env.lux = 320;
    env.valid = true;
    env.simulated = true;
    CHECK(EnvToJson(env) == "{\"ts_ms\":123456,\"temp\":26.4,\"humid\":48.0,\"lux\":320,\"src\":\"sim\"}",
          "环境上报 JSON");

    VehicleStatus status;
    status.state = MotionState::kParked;
    status.events_total = 3;
    CHECK(StatusToJson(status, true, 7) == "{\"ts_ms\":0,\"state\":\"parked\",\"net\":\"online\",\"events\":3,\"boot\":7}",
          "状态上报 JSON（在线）");
    CHECK(StatusToJson(status, false, 7) == "{\"ts_ms\":0,\"state\":\"parked\",\"net\":\"offline\",\"events\":3,\"boot\":7}",
          "状态上报 JSON（离线）");

    // 开机回填：给落盘行补 dev/boot
    const std::string log_line = "{\"seq\":12,\"type\":\"hard_brake\",\"ts_ms\":1758000000123,\"value\":-0.52}";
    CHECK(AddReportFields(log_line, "A1B2C3D4E5F6", 3) ==
              "{\"seq\":12,\"dev\":\"A1B2C3D4E5F6\",\"boot\":3,\"type\":\"hard_brake\","
              "\"ts_ms\":1758000000123,\"value\":-0.52}",
          "补 dev/boot 后与 EventToJson(record, dev, boot) 同构");
    CHECK(AddReportFields(AddReportFields(log_line, "A1B2C3D4E5F6", 3), "A1B2C3D4E5F6", 3) ==
              AddReportFields(log_line, "A1B2C3D4E5F6", 3),
          "重复调用是幂等的（不会补两次）");
    CHECK(AddReportFields("not json", "A1B2C3D4E5F6", 3) == "not json", "非 JSON 原样返回");
    CHECK(AddReportFields(log_line, "", 3) == log_line, "device_id 为空时原样返回");

    printf("\n%s（失败 %d）\n", g_failures == 0 ? "全部通过" : "有失败", g_failures);
    return g_failures == 0 ? 0 : 1;
}
