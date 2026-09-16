#pragma once

#include <driver/gpio.h>
#include <driver/i2c_types.h>
#include <driver/spi_master.h>

/*================ 公共I2C总线 ================*/

#define BOARD_I2C_PORT                  I2C_NUM_0
#define BOARD_I2C_SDA_GPIO              GPIO_NUM_1
#define BOARD_I2C_SCL_GPIO              GPIO_NUM_2
#define BOARD_I2C_FREQ_HZ               100000

/*================ I2C设备地址 ================*/

#define BOARD_PCA9557_ADDR              0x19
#define AUDIO_CODEC_ES8311_ADDR         (0x18<<1)
#define AUDIO_CODEC_ES7210_ADDR         (0x41<<1)

/*================ PCA9557引脚 ================*/

#define BOARD_PCA9557_LCD_CS_BIT        0
#define BOARD_PCA9557_PA_EN_BIT         1
#define BOARD_PCA9557_CAMERA_PWDN_BIT   2

/*================ 音频 ================*/

#define AUDIO_INPUT_SAMPLE_RATE         16000
#define AUDIO_OUTPUT_SAMPLE_RATE        16000

#define AUDIO_I2S_GPIO_MCLK             GPIO_NUM_38
#define AUDIO_I2S_GPIO_BCLK             GPIO_NUM_14
#define AUDIO_I2S_GPIO_WS               GPIO_NUM_13
#define AUDIO_I2S_GPIO_DIN              GPIO_NUM_12
#define AUDIO_I2S_GPIO_DOUT             GPIO_NUM_45

/* PA由PCA9557 IO1控制，不是ESP32 GPIO */
#define AUDIO_CODEC_PA_PIN              GPIO_NUM_NC

/*
 * false：暂不提供扬声器回采参考通道。
 * 基础对话正常后再调试AEC。
 */
#define AUDIO_INPUT_REFERENCE           false

 /*================ LCD ================*/

#define DISPLAY_SPI_HOST                SPI2_HOST
#define DISPLAY_SPI_SCK_PIN             GPIO_NUM_41
#define DISPLAY_SPI_MOSI_PIN            GPIO_NUM_40

/* CS由PCA9557 IO0控制 */
#define DISPLAY_SPI_CS_PIN              GPIO_NUM_NC

#define DISPLAY_DC_PIN                  GPIO_NUM_39
#define DISPLAY_RESET_PIN               GPIO_NUM_NC
#define DISPLAY_BACKLIGHT_PIN           GPIO_NUM_42

#define DISPLAY_WIDTH                   480
#define DISPLAY_HEIGHT                  320
#define DISPLAY_OFFSET_X                0
#define DISPLAY_OFFSET_Y                0

#define DISPLAY_SWAP_XY                 true
#define DISPLAY_MIRROR_X                true
#define DISPLAY_MIRROR_Y                true
#define DISPLAY_INVERT_COLOR            true
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

/*================ 按键 ================*/


 /* 按键 / LED */
#define BOOT_BUTTON_GPIO        GPIO_NUM_0   // 由 GPIO_NUM_NC 改为 0（原理图 SW2）
#define LAMP_GPIO               GPIO_NUM_10  // 绿灯，低电平点亮

/* 触摸 FT6336 */
#define TOUCH_I2C_ADDR          0x38
/* CTP_RST 接板级复位、CTP_INT 未接 */

/* 摄像头 GC0308 */
#define CAMERA_PIN_PWDN         GPIO_NUM_NC  // 实际由 PCA9557 IO2 控制
#define CAMERA_PIN_RESET        GPIO_NUM_NC
#define CAMERA_PIN_XCLK         GPIO_NUM_5
#define CAMERA_PIN_PCLK         GPIO_NUM_7
#define CAMERA_PIN_VSYNC        GPIO_NUM_3
#define CAMERA_PIN_HREF         GPIO_NUM_46
#define CAMERA_PIN_D0           16
#define CAMERA_PIN_D1           18
#define CAMERA_PIN_D2           8
#define CAMERA_PIN_D3           17
#define CAMERA_PIN_D4           15
#define CAMERA_PIN_D5           6
#define CAMERA_PIN_D6           4
#define CAMERA_PIN_D7           9
#define CAMERA_SCCB_ADDR        0x21
#define XCLK_FREQ_HZ            20000000