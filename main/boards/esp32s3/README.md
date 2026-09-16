# ESP32-S3 自定义板（车载终端）

ESP32-S3-WROOM-1-N16R8，16MB Flash / 8MB Octal PSRAM，480×320 横屏，带触摸和摄像头。

## 编译烧录

```bash
idf.py set-target esp32s3
idf.py menuconfig      # Xiaozhi Assistant -> Board Type -> Vehicle ESP32-S3 Terminal Board
idf.py build
idf.py flash monitor
```

## 引脚表

| 功能 | 引脚 / 地址 | 备注 |
|---|---|---|
| I2C | SDA=GPIO1, SCL=GPIO2 | PCA9557/ES8311 走 400kHz，ES7210/触摸按各自设备速率 |
| PCA9557 | 0x19 | IO0=LCD_CS(低有效) / IO1=PA_EN(高有效) / IO2=CAM_PWDN(高=休眠) |
| ES8311（播放） | 0x18 | |
| ES7210（录音） | 0x41 | 必须 256fs（MCLK=4.096MHz） |
| I2S | MCLK=38, BCLK=14, WS=13, DIN=12, DOUT=45 | 16kHz |
| LCD ST7789 | SPI2，SCK=41, MOSI=40, DC=39, CS=扩展器IO0, RST=板级复位, BL=42 | 480×320，BGR，big-endian，40MHz |
| 触摸 FT6336 | 0x38 @400kHz | RST 接板级复位、INT 未接 |
| 摄像头 GC0308 | SCCB 0x21，XCLK=5, PCLK=7, VSYNC=3, HREF=46, D0~D7=16/18/8/17/15/6/4/9 | PWDN 走扩展器 IO2；SCCB 复用 I2C |
| LED | GPIO10（低电平点亮） | ⚠ 与 CTP_INT、CN2-2 共用，代码按开漏输出 |
| BOOT 按键 | GPIO0 | 启动阶段按下进配网，其余情况切换对话 |

保留不可用：GPIO35~37（Octal PSRAM 占用）。

## MCP 工具

除内置工具外，本板额外提供：

| 工具 | 参数 | 说明 |
|---|---|---|
| `self.camera.set_enabled` | `enabled: bool` | 通过扩展器 IO2 唤醒/休眠摄像头 |
| `self.screen.set_enabled` | `enabled: bool` | 背光开/关 |
| `self.microphone.set_gain` | `gain_db: int` | 0~33 且必须是 3 的倍数（ES7210 步进 3dB） |
| `self.led.set_status` | `on: bool` | 板载指示灯 |

## 已知取舍

- **无设备端 AEC**：`AUDIO_INPUT_REFERENCE=false`，未开 `CONFIG_USE_DEVICE_AEC`，AI 说话时无法打断。
- **摄像头不做 LCD 实时预览**：仅通过 `self.camera.take_photo` 供 AI 视觉使用。
