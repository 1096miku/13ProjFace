#include "esp32s3_boards.h"

#include <esp_log.h>

static const char *TAG = "Pca9557";

void Pca9557::SetOutputState(uint8_t bit, bool level) {
    // > 读改写输出锁存寄存器 0x01，避免影响其它已置位的 IO
    uint8_t before = ReadReg(0x01);
    uint8_t data = (before & ~(1 << bit)) | (level << bit);
    WriteReg(0x01, data);

    // ! BUG-040 诊断：0x01 是功放 PA_EN / 摄像头 PWDN / LCD_CS **共用的锁存寄存器**，
    // ! 而"读—改—写"整段没有互斥。两个调用者交错时，后写的那次会拿自己读到的旧值把
    // ! 对方刚置的位清掉（功放位被清 → 软件标志=1 但无声）。这里打出谁改的、改成什么、写完回读什么。
    ESP_LOGI(TAG, "SET bit=%u level=%d 锁存 0x%02X→0x%02X（回读 0x%02X）",
             (unsigned)bit, (int)level, before, data, ReadReg(0x01));
}
