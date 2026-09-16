#include "esp32s3_audio_codec.h"

void CustomAudioCodec::EnableOutput(bool enable) {
    // > 先把 codec 配置好，再开功放，避免配置期间把杂音放大出去
    BoxAudioCodec::EnableOutput(enable);
    pca9557_->SetOutputState(BOARD_PCA9557_PA_EN_BIT, enable);
}

void CustomAudioCodec::SetInputGain(float gain_db) {
    AudioCodec::SetInputGain(gain_db);

    // ! 增益仅在输入通道重新打开时才会写进 ES7210。
    // ! 输入正在工作时重开一次，代价是毫秒级采集中断（MCP 调用场景可接受）。
    if (input_enabled()) {
        EnableInput(false);
        EnableInput(true);
    }
}
