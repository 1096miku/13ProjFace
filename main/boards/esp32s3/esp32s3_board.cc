#include "esp32s3_boards.h"
#include "esp32s3_audio_codec.h"

#include "wifi_board.h"
#include "display/lcd_display.h"
#include "display/emote_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "esp32_camera.h"
#include "mcp_server.h"
#include "vehicle_service.h"
#include "vehicle_ui.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/spi_common.h>
#include <esp_lcd_touch_ft5x06.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>

#define TAG "Esp32S3Board"

class Esp32S3Board : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    Pca9557* pca9557_ = nullptr;
    Button boot_button_;
    Display* display_ = nullptr;
    Esp32Camera* camera_ = nullptr;
    VehicleService* vehicle_ = nullptr;
    VehicleUi* vehicle_ui_ = nullptr;

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)BOARD_I2C_PORT,
            .sda_io_num = BOARD_I2C_SDA_GPIO,
            .scl_io_num = BOARD_I2C_SCL_GPIO,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));

        // > 构造时即关功放、休眠摄像头，避免上电爆音和摄像头抢总线
        pca9557_ = new Pca9557(i2c_bus_, BOARD_PCA9557_ADDR);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_SPI_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SPI_SCK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_SPI_CS_PIN;   // NC：片选由 PCA9557 IO0 常驻拉低
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(DISPLAY_SPI_HOST, &io_config, &panel_io));

        // > 先选中片选再初始化面板：SPI 总线上只有 LCD 一个从设备，之后一直保持选中
        pca9557_->SetOutputState(BOARD_PCA9557_LCD_CS_BIT, false);

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RESET_PIN;   // NC：复位挂板级网络，不可控
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
        panel_config.data_endian = LCD_RGB_DATA_ENDIAN_BIG;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);

#if CONFIG_USE_EMOTE_MESSAGE_STYLE
        display_ = new emote::EmoteDisplay(panel, panel_io, DISPLAY_WIDTH, DISPLAY_HEIGHT);
#else
        display_ = new SpiLcdDisplay(panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
#endif
    }

    void InitializeTouch() {
        esp_lcd_touch_handle_t tp = nullptr;

        // > 触摸面板原始方向是 320x480 竖屏，x_max/y_max 必须填原始尺寸；
        // > 变换由驱动软件层按 mirror_x → mirror_y → swap_xy 顺序做，
        // > 这组参数等价于原工程的 FT_ROT_270（x' = 480-ty, y' = tx）。
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_HEIGHT,   // 320
            .y_max = DISPLAY_WIDTH,    // 480
            .rst_gpio_num = GPIO_NUM_NC,   // 接板级复位，不可控
            .int_gpio_num = GPIO_NUM_NC,   // INT 未接
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = 1,
                .mirror_x = 0,
                .mirror_y = 1,
            },
        };

        esp_lcd_panel_io_handle_t tp_io_handle = nullptr;
        esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
        tp_io_config.scl_speed_hz = 400000;

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle));
        ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &tp));
        assert(tp);

        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(),
            .handle = tp,
        };
        if (touch_cfg.disp) {
            lvgl_port_add_touch(&touch_cfg);
        } else {
            ESP_LOGE(TAG, "Touch display is not initialized");
        }
    }

    void InitializeCamera() {
        // > PWDN 走扩展器 IO2：拉低唤醒
        pca9557_->SetOutputState(BOARD_PCA9557_CAMERA_PWDN_BIT, false);
        vTaskDelay(pdMS_TO_TICKS(20));

        camera_config_t config = {};
        // ! 背光占用 LEDC_TIMER_0 / LEDC_CHANNEL_0，摄像头必须错开，否则两者互相踩
        config.ledc_channel = LEDC_CHANNEL_2;
        config.ledc_timer = LEDC_TIMER_2;
        config.pin_d0 = CAMERA_PIN_D0;
        config.pin_d1 = CAMERA_PIN_D1;
        config.pin_d2 = CAMERA_PIN_D2;
        config.pin_d3 = CAMERA_PIN_D3;
        config.pin_d4 = CAMERA_PIN_D4;
        config.pin_d5 = CAMERA_PIN_D5;
        config.pin_d6 = CAMERA_PIN_D6;
        config.pin_d7 = CAMERA_PIN_D7;
        config.pin_xclk = CAMERA_PIN_XCLK;
        config.pin_pclk = CAMERA_PIN_PCLK;
        config.pin_vsync = CAMERA_PIN_VSYNC;
        config.pin_href = CAMERA_PIN_HREF;
        // > SCCB 复用已建好的 I2C 总线：两个引脚填 -1，
        // > 驱动改用 SCCB_Use_Port(sccb_i2c_port) 按端口号取回总线句柄
        config.pin_sccb_sda = -1;
        config.pin_sccb_scl = -1;
        config.sccb_i2c_port = BOARD_I2C_PORT;
        config.pin_pwdn = CAMERA_PIN_PWDN;     // NC，实际由扩展器控制
        config.pin_reset = CAMERA_PIN_RESET;
        config.xclk_freq_hz = XCLK_FREQ_HZ;
        config.pixel_format = PIXFORMAT_RGB565;
        config.frame_size = FRAMESIZE_QVGA;
        config.jpeg_quality = 12;
        config.fb_count = 1;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

        camera_ = new Esp32Camera(config);
    }

    void InitializeLed() {
        // ! LAMP_GPIO 同时接 CTP_INT、绿灯和 CN2-2：用开漏输出，
        // ! 我们和触摸芯片都只能拉低，不会互相灌电流；低电平仍然点灯。
        gpio_config_t config = {
            .pin_bit_mask = 1ULL << LAMP_GPIO,
            .mode = GPIO_MODE_OUTPUT_OD,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&config));
        gpio_set_level(LAMP_GPIO, 1);   // 低电平点亮，上电默认熄灭
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            // 启动阶段（尚未联网）按 BOOT 直接进配网，其余情况切换对话状态
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();

        mcp_server.AddTool("self.camera.set_enabled",
            "打开或关闭摄像头。关闭后 self.camera.take_photo 将无法拍照。",
            PropertyList({
                Property("enabled", kPropertyTypeBoolean)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                bool enabled = properties["enabled"].value<bool>();
                // PWDN 高电平休眠
                pca9557_->SetOutputState(BOARD_PCA9557_CAMERA_PWDN_BIT, !enabled);
                return true;
            });

        mcp_server.AddTool("self.screen.set_enabled",
            "打开或关闭屏幕（背光）。关闭后屏幕变黑，但设备仍在工作。",
            PropertyList({
                Property("enabled", kPropertyTypeBoolean)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                bool enabled = properties["enabled"].value<bool>();
                auto backlight = GetBacklight();
                if (enabled) {
                    backlight->RestoreBrightness();
                } else {
                    backlight->SetBrightness(0);
                }
                return true;
            });

        mcp_server.AddTool("self.microphone.set_gain",
            "设置麦克风输入增益。单位为 dB，取值范围 0~33，且必须是 3 的倍数"
            "（ES7210 的增益调节步进为 3dB）。",
            PropertyList({
                Property("gain_db", kPropertyTypeInteger, 0, 33)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int gain_db = properties["gain_db"].value<int>();
                if (gain_db % 3 != 0) {
                    throw std::invalid_argument("gain_db must be a multiple of 3");
                }
                GetAudioCodec()->SetInputGain((float)gain_db);
                return gain_db;
            });

        mcp_server.AddTool("self.led.set_status",
            "打开或关闭板载指示灯。",
            PropertyList({
                Property("on", kPropertyTypeBoolean)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                bool on = properties["on"].value<bool>();
                gpio_set_level(LAMP_GPIO, on ? 0 : 1);   // 低电平点亮
                return true;
            });
    }

public:
    Esp32S3Board() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeDisplay();
        InitializeTouch();
        InitializeCamera();
        InitializeLed();
        InitializeButtons();
        InitializeTools();

        // > IMU 不在线时只告警降级，不阻断开机（屏幕、语音、摄像头照常）
        vehicle_ = new VehicleService(i2c_bus_);
        if (!vehicle_->Start()) {
            ESP_LOGW(TAG, "行车监测未启动（IMU 不在线），屏幕与语音功能不受影响");
        }

        // > 界面必须在 display->SetupUI() 之后才建；VehicleUi::Start() 只建定时器，
        // > 真正的界面等 lv_timer 第一次 tick 且 IsSetupUICalled() 为真时才创建。
        // > 相机（实时画面页的帧源）在 D4 接入，这里先传 nullptr。
        vehicle_ui_ = new VehicleUi(display_, vehicle_, nullptr);
        vehicle_ui_->Start();

        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static CustomAudioCodec audio_codec(i2c_bus_, pca9557_);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }
};

DECLARE_BOARD(Esp32S3Board);
