// 命令表 ↔ C++ 意图表一致性测试（防两处漂移）
//
// 编译与运行：
//   g++ -std=c++17 -Wall -Wextra -I main/vehicle test/voice_intent_test.cc main/vehicle/voice_intent.cc -o build_host/voice_intent_test.exe
//   build_host\voice_intent_test.exe
//
// ! 测试在**仓库根目录**下运行（命令备忘里的写法），所以用相对路径读 JSON。

#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "voice_intent.h"

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

// > 只认我们自己写的这份 JSON 的固定写法（`"action": "xxx"`），不做通用 JSON 解析：
// > 主机测试不引入 cJSON，少一个跨平台依赖。JSON 格式变了这里会直接读不到而报错。
static std::vector<std::string> ExtractActions(const std::string &text) {
    std::vector<std::string> actions;
    const std::string key = "\"action\"";
    size_t pos = 0;
    while ((pos = text.find(key, pos)) != std::string::npos) {
        const size_t colon = text.find(':', pos + key.size());
        if (colon == std::string::npos) break;
        const size_t open = text.find('"', colon);
        if (open == std::string::npos) break;
        const size_t close = text.find('"', open + 1);
        if (close == std::string::npos) break;
        actions.push_back(text.substr(open + 1, close - open - 1));
        pos = close + 1;
    }
    return actions;
}

int main() {
    printf("voice_intent\n");

    std::ifstream file("main/boards/esp32s3/voice_commands.json");
    if (!file) {
        printf("  FAIL 打不开 main/boards/esp32s3/voice_commands.json（必须在仓库根目录运行）\n");
        return 1;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::vector<std::string> actions = ExtractActions(buffer.str());

    CHECK(actions.size() == 9, "命令表 9 条（1 唤醒 + 8 命令）");
    CHECK(actions.empty() || actions[0] == "wake", "第一条必须是 wake");

    std::set<VoiceIntent> seen;
    for (const std::string &action : actions) {
        const VoiceIntent intent = ParseVoiceAction(action);
        if (intent == VoiceIntent::kUnknown) {
            printf("  FAIL JSON action \"%s\" 没有 C++ 映射\n", action.c_str());
            g_failures++;
            continue;
        }
        if (!seen.insert(intent).second) {
            printf("  FAIL JSON action \"%s\" 与前面某条映射到了同一个 intent\n", action.c_str());
            g_failures++;
        }
    }

    // > 反向：C++ 表里的每个 intent（除 kUnknown/kCount）都必须能在 JSON 里找到 action，
    // > 否则就是"代码里有、模型永远认不出"的死意图。
    for (int i = 1; i < static_cast<int>(VoiceIntent::kCount); i++) {
        const VoiceIntent intent = static_cast<VoiceIntent>(i);
        bool found = false;
        for (const std::string &action : actions) {
            if (ParseVoiceAction(action) == intent) { found = true; break; }
        }
        if (!found) {
            printf("  FAIL intent %s 在 JSON 里没有对应 action\n", ToString(intent));
            g_failures++;
        }
    }

    CHECK(ParseVoiceAction("") == VoiceIntent::kUnknown, "空 action → kUnknown");
    CHECK(ParseVoiceAction("temp") == VoiceIntent::kTemp, "temp → kTemp");
    CHECK(ParseVoiceAction("lock") == VoiceIntent::kLock, "lock → kLock");
    CHECK(std::string(ToString(VoiceIntent::kOccupancy)) == "occupancy", "ToString(kOccupancy)");

    printf("\n%s（失败 %d）\n", g_failures == 0 ? "全部通过" : "有失败", g_failures);
    return g_failures == 0 ? 0 : 1;
}
