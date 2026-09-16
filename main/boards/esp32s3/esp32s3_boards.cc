#include "esp32s3_boards.h"

void Pca9557::SetOutputState(uint8_t bit, bool level) {
    // > 读改写输出锁存寄存器 0x01，避免影响其它已置位的 IO
    uint8_t data = ReadReg(0x01);
    data = (data & ~(1 << bit)) | (level << bit);
    WriteReg(0x01, data);
}
