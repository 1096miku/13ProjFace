// 语音应答片段组装的主机单元测试
//
// 编译与运行：
//   g++ -std=c++17 -Wall -Wextra -I main/vehicle test/voice_reply_test.cc main/vehicle/voice_reply.cc -o build_host/voice_reply_test.exe
//   build_host\voice_reply_test.exe

#include <cstdio>
#include <string>
#include <vector>

#include "voice_reply.h"

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

// ! ToString() 返回 const char*，直接和字符串字面量比是指针比较（-Waddress 会警告，
// ! 且恒为假）。一律先转 std::string 再比。
static std::string S(const char *text) {
    return std::string(text);
}

static std::string Join(const std::vector<ClipId> &clips) {
    std::string out;
    for (ClipId id : clips) {
        if (!out.empty()) out += " ";
        out += ToString(id);
    }
    return out;
}

int main() {
    printf("voice_reply\n");

    // 数字读数：0–9 单段；10–19 = d10 + 个位；20–99 = 十位 + d10 + 个位
    CHECK(Join(NumberToClips(0)) == "d0", "0 → d0");
    CHECK(Join(NumberToClips(7)) == "d7", "7 → d7");
    CHECK(Join(NumberToClips(10)) == "d10", "10 → d10（不拼 0）");
    CHECK(Join(NumberToClips(11)) == "d10 d1", "11 → d10 d1");
    CHECK(Join(NumberToClips(19)) == "d10 d9", "19 → d10 d9");
    CHECK(Join(NumberToClips(20)) == "d2 d10", "20 → d2 d10");
    CHECK(Join(NumberToClips(26)) == "d2 d10 d6", "26 → 二十六");
    CHECK(Join(NumberToClips(48)) == "d4 d10 d8", "48 → 四十八");
    CHECK(Join(NumberToClips(90)) == "d9 d10", "90 → d9 d10");
    CHECK(Join(NumberToClips(99)) == "d9 d10 d9", "99 → d9 d10 d9");
    // > 本项目用不到 >99（温度/湿度/事件数都在 0–99），但别让越界变成越界访问
    CHECK(Join(NumberToClips(100)) == "d1 d0 d0", "100 → 逐位兜底 d1 d0 d0");
    CHECK(Join(NumberToClips(123)) == "d1 d2 d3", "123 → 逐位兜底");
    // ! 没有"零下"片段：负温度只上屏、不播报（设计文档 §6.4）
    CHECK(NumberToClips(-3).empty(), "负数 → 空（只上屏）");

    // 温度/湿度：前缀 + 数字 + 单位
    CHECK(Join(TemperatureClips(26.4f, true)) == "q_temp d2 d10 d6 unit_degree", "26.4 ℃ → 车内温度 二十六 度");
    CHECK(Join(TemperatureClips(48.6f, true)) == "q_temp d4 d10 d9 unit_degree", "48.6 ℃ 四舍五入到 49");
    CHECK(Join(HumidityClips(48.0f, true)) == "q_humid d4 d10 d8", "湿度 48%（片段自带“百分之”）");
    CHECK(Join(TemperatureClips(26.0f, false)) == "q_temp", "读数无效 → 只播前缀");
    CHECK(Join(HumidityClips(48.0f, false)) == "q_humid", "读数无效 → 只播前缀");

    // 光照：播档位，不播数字
    CHECK(S(ToString(LightClip(5))) == "q_light_dark", "5 lux → 很暗");
    CHECK(S(ToString(LightClip(30))) == "q_light_dim", "30 lux → 偏暗");
    CHECK(S(ToString(LightClip(200))) == "q_light_mid", "200 lux → 适中");
    CHECK(S(ToString(LightClip(600))) == "q_light_bright", "600 lux → 明亮");
    CHECK(S(ToString(LightClip(2000))) == "q_light_strong", "2000 lux → 很强");

    // 设备状态：正常 + 联网/未联网 + 事件数 + 次
    CHECK(Join(StatusClips(true, 3)) == "q_status_ok q_online q_events d3 unit_times", "在线 3 次事件");
    CHECK(Join(StatusClips(false, 0)) == "q_status_ok q_offline q_events d0 unit_times", "未联网 0 次事件");

    // 有没有人：只读"手机端有遗留标记"这一个本地状态位
    CHECK(S(ToString(OccupancyClip(false))) == "q_no_alert", "无标记 → 未检测到异常");
    CHECK(S(ToString(OccupancyClip(true))) == "q_pending", "有标记 → 有遗留提醒待确认");

    // 事件播报：每类事件一段
    CHECK(S(ToString(EventClip(EventType::kHardBrake))) == "ev_hard_brake", "急刹车");
    CHECK(S(ToString(EventClip(EventType::kHardAccel))) == "ev_hard_accel", "急加速");
    CHECK(S(ToString(EventClip(EventType::kHardTurn))) == "ev_hard_turn", "急转弯");
    CHECK(S(ToString(EventClip(EventType::kBump))) == "ev_bump", "颠簸");
    CHECK(S(ToString(EventClip(EventType::kCrash))) == "ev_crash", "碰撞");
    CHECK(S(ToString(EventClip(EventType::kParked))) == "ev_parked", "已停车");
    CHECK(S(ToString(EventClip(EventType::kMoving))) == "ev_driving", "行驶中");
    CHECK(S(ToString(EventClip(EventType::kMotionWhileParked))) == "ev_motion_parked", "锁车期异常震动");
    CHECK(S(ToString(EventClip(EventType::kCount))) == "none", "未知事件 → none（不播）");

    printf("\n%s（失败 %d）\n", g_failures == 0 ? "全部通过" : "有失败", g_failures);
    return g_failures == 0 ? 0 : 1;
}
