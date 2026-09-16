#pragma once

#include "codecs/box_audio_codec.h"
#include "config.h"
#include "esp32s3_boards.h"

/*
 * ES8311（播放）+ ES7210（录音）双芯片编解码器。
 *
 * 与其他板唯一的差别：功放使能 PA_EN 不在 ESP32 的 GPIO 上，
 * 而是挂在 PCA9557 扩展器的 IO1，所以必须重写 EnableOutput。
 */
class CustomAudioCodec : public BoxAudioCodec {
public:
    CustomAudioCodec(i2c_master_bus_handle_t i2c_bus, Pca9557* pca9557)
        : BoxAudioCodec(i2c_bus,
                        AUDIO_INPUT_SAMPLE_RATE,
                        AUDIO_OUTPUT_SAMPLE_RATE,
                        AUDIO_I2S_GPIO_MCLK,
                        AUDIO_I2S_GPIO_BCLK,
                        AUDIO_I2S_GPIO_WS,
                        AUDIO_I2S_GPIO_DOUT,
                        AUDIO_I2S_GPIO_DIN,
                        AUDIO_CODEC_PA_PIN,        // GPIO_NUM_NC，功放不走 GPIO
                        AUDIO_CODEC_ES8311_ADDR,
                        AUDIO_CODEC_ES7210_ADDR,
                        AUDIO_INPUT_REFERENCE),
          pca9557_(pca9557) {
    }

    void EnableOutput(bool enable) override;

    // ! 基类 AudioCodec::SetInputGain 只记录数值、不下发硬件：
    // ! ES7210 的增益只在 EnableInput(true) 时写入。这里补一次重开，
    // ! 否则 MCP 调完会「返回成功但麦克风灵敏度没变」。
    void SetInputGain(float gain_db) override;

private:
    Pca9557* pca9557_;
};
