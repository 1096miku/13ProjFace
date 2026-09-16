#pragma once

#include "i2c_device.h"

#include <driver/i2c_master.h>

/*
 * PCA9557 IO 扩展器
 *
 * IO0 = LCD_CS（低有效，本板 SPI 总线上只有一块从设备，常驻拉低选中）
 * IO1 = 功放使能 PA_EN（高有效）
 * IO2 = 摄像头 PWDN（高 = 休眠，低 = 工作）
 * IO3~IO7 = EXT-IO，保持输入
 */
class Pca9557 : public I2cDevice {
public:
    Pca9557(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        // ! 必须先写输出锁存寄存器 0x01、再切方向寄存器 0x03，
        // ! 否则上电瞬间 LCD_CS 会抖动一次，屏幕出现随机花屏。
        WriteReg(0x01, 0x05);  // CS=1(不选中) PA=0(关) PWDN=1(休眠)
        WriteReg(0x03, 0xF8);  // IO7~IO3 输入，IO2~IO0 输出
    }

    void SetOutputState(uint8_t bit, bool level);
};
