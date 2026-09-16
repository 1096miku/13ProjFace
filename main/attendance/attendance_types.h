#pragma once

// 考勤领域类型：只依赖标准库，刻意不引入任何 ESP-IDF 头文件，
// 这样同一份实现既能跑在设备上，也能在 PC 上用 g++ 做单元测试。
// 术语定义见项目根目录 CONTEXT.md

#include <cstdint>
#include <string>
#include <vector>

namespace attendance {

// > 迟到阈值下限：设备时钟在断网时可能漂移，阈值必须留出余量吸收误差
inline constexpr int32_t kMinLateThresholdMinutes = 5;
inline constexpr int32_t kDefaultLateThresholdMinutes = 5;

// 考勤结论
enum class Verdict : uint8_t {
    kNone = 0,   // 尚未判定
    kPresent,    // 正常
    kLate,       // 迟到
    kAbsent,     // 缺勤
    kOnLeave,    // 请假（由后端覆盖）
};

const char *ToString(Verdict v);

// 一节课的考勤单元
struct Session {
    int32_t id = 0;
    int64_t start_ts = 0;  // UTC 秒
    int64_t end_ts = 0;    // UTC 秒
    int32_t late_threshold_min = kDefaultLateThresholdMinutes;

    bool valid() const { return id != 0 && end_ts > start_ts; }
    int32_t DurationMinutes() const { return static_cast<int32_t>((end_ts - start_ts) / 60); }
};

// 签到事件：某人某时刻被识别到这个原始事实，只增不改
struct SignEvent {
    uint32_t student_id = 0;
    int32_t session_id = 0;
    int64_t ts = 0;
    bool time_trusted = true;  // false = 设备时钟不可信，结论待后端重算
};

// 一次签到请求的判定结果
struct Decision {
    bool accepted = false;       // false = 本次签到未被接受（课次未开始/已结束/参数非法）
    bool duplicate = false;      // true = 本课次内重复签到，只累计次数
    Verdict verdict = Verdict::kNone;
    int32_t duplicate_count = 0; // 含首次在内的总签到次数
    int64_t first_seen_ts = 0;   // 首次被识别到的时间
    bool provisional = false;    // 结论待定（时间不可信，联网后由后端重算）
};

// 一个课次内的结论记录
struct Record {
    uint32_t student_id = 0;
    Verdict verdict = Verdict::kNone;
    int64_t first_seen_ts = 0;
    int32_t sign_in_count = 0;
    bool time_trusted = true;
};

}  // namespace attendance
