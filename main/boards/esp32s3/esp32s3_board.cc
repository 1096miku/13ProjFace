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
#include "camera_capture.h"
#include "snapshot_store.h"
#include "vehicle_http.h"
#include "vehicle_service.h"
#include "vehicle_ui.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/spi_common.h>
#include <esp_lcd_touch_ft5x06.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>

#include <stdexcept>
#include <vector>

#include "environment_sensor.h"
#include "event_json.h"

#define TAG "Esp32S3Board"

class Esp32S3Board : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    Pca9557* pca9557_ = nullptr;
    Button boot_button_;
    Display* display_ = nullptr;
    Esp32Camera* camera_ = nullptr;
    camera_config_t camera_config_ = {};   // InitializeCamera() 填好后留给"重新打开摄像头"用
    bool camera_enabled_ = true;
    SnapshotStore* snapshot_store_ = nullptr;
    CameraCapture* camera_capture_ = nullptr;
    VehicleService* vehicle_ = nullptr;
    VehicleUi* vehicle_ui_ = nullptr;
    VehicleHttp* http_ = nullptr;

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
        // ! 相机配置的取舍全部记在 docs/BUGS.md BUG-025（改之前先读那一条，别凭"官方推荐"想当然）：
        // !   fb_count = 2：上游 Esp32Camera::Capture() 会把一帧一直攥在 current_fb_ 里
        // !     （要等下一次 Capture() 才归还，见 main/boards/common/esp32_camera.cc:69-78），
        // !     只留 1 个缓冲时预览侧拿不到新帧；多一个 QVGA RGB565 缓冲约 150 KB PSRAM。
        // !   grab_mode 保持 CAMERA_GRAB_WHEN_EMPTY（= 板子原有取值，计划也只要求改 fb_count）：
        // !     实测把它改成 CAMERA_GRAB_LATEST 后预览掉到 9.8 fps，并在约 355 s 后出现
        // !     `cam_hal: Failed to get frame: timeout` 每 4.1 s 一条、永不恢复的停摆；
        // !     换回 WHEN_EMPTY 的这一版实测 13.6 fps 且抓拍/小智拍照都正常。
        // !   ! 另外：fb_count=3 + LATEST 那版在开机就报 `cam_hal: EV-EOF-OVF` 与
        // !   ! `FB-SIZE: 138240 != 153600`（驱动在 PSRAM DMA 关闭时每帧要软件搬 153,600 B，
        // !   ! 负载一高就搬不完），并让小智拍照的上传挂死。别往那个方向试。
        config.fb_count = 2;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

        camera_config_ = config;   // 留给 SetCameraEnabled(true) 重写传感器寄存器用
        camera_ = new Esp32Camera(camera_config_);
    }

    // 重新打开摄像头 = 唤醒传感器 + **重写一遍传感器寄存器**。
    //
    // ! 不能走 esp_camera_deinit() + esp_camera_init()：cam_dma_config() 要一次性拿到 30720 B
    // ! 连续 DMA 内部 RAM，而开机运行起来之后最大空块只剩 25600 B，实测直接失败：
    // !   `E cam_hal: cam_dma_config(524): DMA buffer 30720 Byte malloc failed,
    // !    the current largest free block:25600 Byte` → `Camera config failed with error 0xffffffff`
    // ! 所以"关闭"**不拆驱动**（DMA 缓冲与 cam_hal 原样保留，也就不需要重新分配），只给 GC0308
    // ! 断电；"打开"时唤醒 + 按 esp_camera_init() 后半段的口径把传感器寄存器重写回来。
    bool ReinitSensor() {
        pca9557_->SetOutputState(BOARD_PCA9557_CAMERA_PWDN_BIT, false);
        vTaskDelay(pdMS_TO_TICKS(100));

        sensor_t *s = esp_camera_sensor_get();
        if (s == nullptr) {
            ESP_LOGE(TAG, "传感器未初始化（开机时 esp_camera_init 就没成功），无法打开摄像头");
            return false;
        }
        if (s->reset(s) != 0) {
            ESP_LOGE(TAG, "传感器寄存器复位失败（SCCB 写不通）");
            return false;
        }
        // > 顺序与 esp_camera_init() 的后半段一致：帧尺寸 → 像素格式 → 状态 → 镜像
        s->set_framesize(s, static_cast<framesize_t>(camera_config_.frame_size));
        s->set_pixformat(s, static_cast<pixformat_t>(camera_config_.pixel_format));
        s->init_status(s);
        if (s->id.PID == GC0308_PID) {
            s->set_hmirror(s, 0);   // 与 Esp32Camera 构造函数同口径
        }
        ESP_LOGI(TAG, "传感器已重新初始化（PWDN 拉低 + 寄存器重写）");
        return true;
    }

    // 关闭摄像头：先让预览/抓拍停手（会等在飞的一次取帧结束），再给传感器断电。
    // ! 断电后 self.camera.take_photo 也会失败（拿不到帧）——这正是工具描述里承诺的行为。
    void SetCameraEnabled(bool enabled) {
        if (enabled == camera_enabled_) {
            return;
        }
        if (!enabled) {
            camera_capture_->SetEnabled(false);
            pca9557_->SetOutputState(BOARD_PCA9557_CAMERA_PWDN_BIT, true);
            camera_enabled_ = false;
            ESP_LOGI(TAG, "摄像头已关闭（预览黑屏、抓拍立刻失败、不再触碰驱动）");
            return;
        }
        if (!ReinitSensor()) {
            camera_enabled_ = false;
            // ! 抛异常而不是返回 false：返回 false 时模型会把"失败"念成"打开啦"（真机实测）
            throw std::runtime_error("摄像头重新初始化失败，仍处于关闭状态");
        }
        camera_capture_->SetEnabled(true);
        camera_enabled_ = true;
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
                // > 成功统一返回 true（成功与否指"这次操作有没有做到"，不是"摄像头现在开着吗"）：
                // > 关闭成功时返回 camera_enabled_==false 会被模型念成"没关成功"（真机实测）。
                SetCameraEnabled(enabled);   // 失败时抛异常，模型才能正确回答"没打开"
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

        // > 车辆数据暴露给小智：云端大模型据此回答"车现在什么状态 / 温度多少 / 有没有异常"。
        // > 这些 lambda 只捕获 this、在**调用时**才解引用 vehicle_ / vehicle_ui_；虽然本函数
        // > 在构造函数里比那两个成员更早执行，但工具只可能在开机完成之后被调用，所以是安全的。
        mcp_server.AddTool("self.vehicle.status",
            "读取车辆当前状态：行车状态、环境数据（温度/湿度/光照）、事件计数、最近一次事件。",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                const vehicle::VehicleStatus status = vehicle_->Status();
                const vehicle::EnvReading env = vehicle_->env();
                const std::vector<vehicle::EventRecord> history = vehicle_->CopyHistory();

                cJSON* root = cJSON_CreateObject();
                cJSON_AddStringToObject(root, "state", vehicle::ToString(status.state));
                cJSON_AddBoolToObject(root, "calibrated", status.calibrated);
                cJSON_AddNumberToObject(root, "events_total", status.events_total);
                cJSON_AddNumberToObject(root, "ax", status.sample.ax);
                cJSON_AddNumberToObject(root, "ay", status.sample.ay);
                cJSON_AddNumberToObject(root, "az", status.sample.az);

                cJSON* env_json = cJSON_AddObjectToObject(root, "env");
                cJSON_AddBoolToObject(env_json, "valid", env.valid);
                cJSON_AddBoolToObject(env_json, "simulated", env.simulated);
                cJSON_AddNumberToObject(env_json, "temp_c", env.temp_c);
                cJSON_AddNumberToObject(env_json, "humidity_pct", env.humidity_pct);
                cJSON_AddNumberToObject(env_json, "lux", env.lux);
                cJSON_AddStringToObject(env_json, "light_level", vehicle::ToString(vehicle::BucketLight(env.lux)));

                if (!history.empty()) {
                    const vehicle::EventRecord& last = history.back();
                    cJSON* last_json = cJSON_AddObjectToObject(root, "last_event");
                    // ! seq / ts_ms 是 int64，只能走 double 传（nano printf 不支持 64 位格式，见 BUG-001）；
                    // ! EventTypeId() 返回 std::string，必须 .c_str()（cJSON 只吃 const char*）。
                    cJSON_AddNumberToObject(last_json, "seq", static_cast<double>(last.seq));
                    cJSON_AddStringToObject(last_json, "type", vehicle::EventTypeId(last.event.type).c_str());
                    cJSON_AddNumberToObject(last_json, "ts_ms", static_cast<double>(last.event.ts_ms));
                    cJSON_AddNumberToObject(last_json, "value", last.event.value);
                }

                char* text = cJSON_PrintUnformatted(root);
                std::string result = text != nullptr ? text : "{}";
                cJSON_free(text);
                cJSON_Delete(root);
                // > 打一条调用日志：MCP 工具本身没有统一的调用日志，验收要靠串口。
                ESP_LOGI(TAG, "工具 self.vehicle.status → %s", result.c_str());
                return result;
            });

        mcp_server.AddTool("self.vehicle.set_page",
            "切换车载屏幕页面。page 取值：home（主页）、preview（实时画面）、events（事件记录）、"
            "settings（设置）、chat（返回小智聊天界面）。",
            PropertyList({
                Property("page", kPropertyTypeString)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                const std::string page = properties["page"].value<std::string>();
                if (page == "chat") {
                    vehicle_ui_->RequestChatScreen();
                } else if (page == "home") {
                    vehicle_ui_->RequestPage(VehicleUi::Page::kHome);
                } else if (page == "preview") {
                    vehicle_ui_->RequestPage(VehicleUi::Page::kPreview);
                } else if (page == "events") {
                    vehicle_ui_->RequestPage(VehicleUi::Page::kEvents);
                } else if (page == "settings") {
                    vehicle_ui_->RequestPage(VehicleUi::Page::kSettings);
                } else {
                    throw std::invalid_argument("page must be one of: home/preview/events/settings/chat");
                }
                ESP_LOGI(TAG, "工具 self.vehicle.set_page → %s", page.c_str());
                return page;
            });

        mcp_server.AddTool("self.vehicle.lock",
            "进入或退出锁车监测模式。locked=true 进入（停车态下立即生效），false 退出。",
            PropertyList({
                Property("locked", kPropertyTypeBoolean)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                const bool locked = properties["locked"].value<bool>();
                vehicle_->RequestLock(locked);
                ESP_LOGI(TAG, "工具 self.vehicle.lock → %s", locked ? "true" : "false");
                return locked;
            });

        mcp_server.AddTool("self.vehicle.capture",
            "立即抓拍一张照片并保存到最近抓拍。",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                vehicle_->RequestCapture();
                ESP_LOGI(TAG, "工具 self.vehicle.capture → 已请求抓拍");
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

        // > 抓拍与事件日志的存储层。挂载失败只告警（内存模式），不阻断开机。
        snapshot_store_ = new SnapshotStore(32);
        if (!snapshot_store_->Start()) {
            ESP_LOGW(TAG, "snapshots 分区不可用，抓拍只在内存里保留最近一张");
        }

        // > IMU 不在线时只告警降级，不阻断开机（屏幕、语音、摄像头照常）
        vehicle_ = new VehicleService(i2c_bus_);
        vehicle_->AddEventSink(snapshot_store_);
        // > 抓拍与预览共用相机，内部用一把锁互斥（设计文档 §8）。
        camera_capture_ = new CameraCapture(snapshot_store_);
        vehicle_->AddEventSink(camera_capture_);
        if (!vehicle_->Start()) {
            ESP_LOGW(TAG, "行车监测未启动（IMU 不在线），屏幕与语音功能不受影响");
        }

        // > 界面必须在 display->SetupUI() 之后才建；VehicleUi::Start() 只建定时器，
        // > 真正的界面等 lv_timer 第一次 tick 且 IsSetupUICalled() 为真时才创建。
        vehicle_ui_ = new VehicleUi(display_, vehicle_, camera_capture_);
        vehicle_ui_->Start();

        // > 局域网看图/看事件。**只构造对象、不起服务**：httpd_start() 会建 socket，
        // > 而 lwIP 的 tcpip 线程要到 WifiManager::Initialize() 里 esp_netif_init() 之后才存在
        // > （managed_components/78__esp-wifi-connect/wifi_manager.cc:74），那是在 StartNetwork()
        // > 里、构造函数之后。在构造函数里起会命中
        // > `assert failed: tcpip_send_msg_wait_sem tcpip.c:454 (Invalid mbox)` 无限重启
        // > （见 docs/BUGS.md BUG-026）。真正启动在下面的 StartNetwork() 重写里。
        http_ = new VehicleHttp(snapshot_store_);

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

    // > 网络起来之后再起 HTTP 服务（理由见构造函数里那段注释）。
    // > WifiBoard::StartNetwork() 内部先 esp_netif_init() 再异步连 WiFi，所以它返回之后
    // > socket API 就安全了——不需要等"连上"，绑 0.0.0.0:80 只要协议栈在就行。
    virtual void StartNetwork() override {
        WifiBoard::StartNetwork();
        if (http_ != nullptr && !http_->Start()) {
            ESP_LOGW(TAG, "局域网 HTTP 未启动，其它功能不受影响");
        }
    }
};

DECLARE_BOARD(Esp32S3Board);
