#pragma once

// 考勤判定规则引擎：纯逻辑，无硬件依赖，可主机单元测试。
// 判定优先顺序（对应计划书 §5.2）：
//   1. 课次是否有效、时间是否落在课次窗口内
//   2. 该人脸是否属于本课次应到名单
//   3. 首次识别时间与迟到阈值比较，定 正常/迟到
//   4. 课次结束结算未到者 = 缺勤
//   5. 重复识别只累计次数，不产生新结论
//   6. 请假由后端覆盖结论，签到事件不变

#include <unordered_map>
#include <vector>

#include "attendance_types.h"

namespace attendance {

struct Stats {
    int32_t present = 0;
    int32_t late = 0;
    int32_t absent = 0;
    int32_t on_leave = 0;
    int32_t total() const { return present + late + absent + on_leave; }
};

class RuleEngine {
public:
    // 开始一个课次。阈值低于下限会被夹到下限；课次参数非法则返回 false 并保持原课次
    bool BeginSession(const Session &session);

    // 设置应到名单（单设备单班场景下即人脸库中的学号集合）
    void SetRoster(const std::vector<uint32_t> &student_ids);

    // 处理一次签到
    Decision OnSignIn(uint32_t student_id, int64_t ts, bool time_trusted);

    // 课次结束时结算：未到者记为缺勤，返回本次新结算为缺勤的学号
    std::vector<uint32_t> Settle(int64_t now_ts);

    // 请假覆盖（后端下发）：只改结论，签到事件与首次识别时间不变
    bool ApplyLeaveOverride(uint32_t student_id);

    // 查询
    const Session &session() const { return session_; }
    bool session_active() const { return session_.valid(); }
    bool settled() const { return settled_; }
    Verdict VerdictOf(uint32_t student_id) const;
    const Record *FindRecord(uint32_t student_id) const;
    const std::unordered_map<uint32_t, Record> &records() const { return records_; }
    Stats SessionStats() const;
    // 某学生在本课次的签到次数（含首次）
    int32_t SignInCount(uint32_t student_id) const;

private:
    bool IsExpected(uint32_t student_id) const;
    static Verdict JudgeByTime(const Session &s, int64_t ts);

    Session session_;
    std::vector<uint32_t> roster_;
    std::unordered_map<uint32_t, Record> records_;
    bool settled_ = false;
};

}  // namespace attendance
