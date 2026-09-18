#include "vehicle_ui.h"

#include <cstdio>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "board.h"
#include "camera_capture.h"
#include "display.h"
#include "environment_sensor.h"
#include "event_text.h"
#include "lvgl_font.h"
#include "lvgl_theme.h"

#define TAG "VehicleUi"

// > 刻意不 include BUILTIN_TEXT_FONT、也不 LV_FONT_DECLARE 任何内置字体：
// > 内置 font_puhui_basic_30_4 只有 206 个汉字，用它写"车辆状态/事件记录"这类标签会缺字，
// > 而本工程没开 LV_USE_FONT_PLACEHOLDER —— 缺字不画方框、宽度为 0，字直接消失。
// > 字体统一由 ApplyThemeAppearance() 从主题取（assets.Apply() 之后主题里才是 common 字体）。

namespace {

constexpr int kRefreshMs = 1000;
constexpr int kPreviewIntervalMs = 60;    // ≈16 fps 目标：指标要求 ≥10 fps，留出丢帧余量
// > 导航栏占底部 40 px，所以每页内容必须排到 y < 284 为止；事件页行数也据此从 5 减到 4。
constexpr int kNavBarY = 284;
constexpr int kNavBarH = 34;
constexpr int kNavButtonW = 90;
constexpr int kNavButtonGap = 94;
constexpr int kEventRows = 4;
constexpr int kPreviewW = 320;
constexpr int kPreviewH = 240;
// > 取帧连续失败这么久就在预览页上屏"摄像头不可用"。按时间而不是按次数：相机停摆时
// > 一次 esp_camera_fb_get() 要耗满 4000 ms 超时，原来按"连续 30 次"要等约 120 s 才提示。
constexpr int64_t kPreviewFailHintMs = 3000;
// > 预览页提示文案。用同一份字面量做"是否需要重写"的判据，避免每 60 ms 重设一次 label。
constexpr const char* kNoticeNone = "";
constexpr const char* kNoticeCameraOff = "摄像头已关闭";
constexpr const char* kNoticeCameraBad = "摄像头不可用";
constexpr uint32_t kTextColor = 0xE8E8E8;
constexpr uint32_t kDimColor = 0x9AA0A6;
constexpr uint32_t kBgColor = 0x14161A;
// > 30 号字一个汉字 30 px，480 px 的屏最多放 14~15 个汉字。给每个标签限宽 + 自动换行，
// > 保证长文本不会画到屏外（真机验收时设置页出现过越界与重叠，见 docs/BUGS.md BUG-021）。
constexpr int kLabelMaxWidth = 448;

lv_obj_t* MakeLabel(lv_obj_t* parent, int x, int y, uint32_t color) {
    lv_obj_t* label = lv_label_create(parent);
    // > 不设字体：文字属性从父级（screen）继承，字体由 ApplyThemeAppearance() 统一贴。
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_width(label, kLabelMaxWidth);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(label, x, y);
    lv_label_set_text(label, "");
    return label;
}

lv_obj_t* MakeButton(lv_obj_t* parent, int x, int y, int w, int h, const char* text, lv_event_cb_t cb,
                     void* user_data) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_t* label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    return btn;
}

const char* StateText(vehicle::MotionState state) {
    return vehicle::ToString(state);
}

}  // namespace

VehicleUi::VehicleUi(Display* display, VehicleService* vehicle, CameraCapture* camera)
    : display_(display), vehicle_(vehicle), camera_(camera) {
}

VehicleUi::~VehicleUi() {
    if (preview_buf_ != nullptr) {
        heap_caps_free(preview_buf_);
        preview_buf_ = nullptr;
    }
}

bool VehicleUi::OnOurPage() const {
    lv_obj_t* active = lv_screen_active();
    for (int i = 0; i < static_cast<int>(Page::kCount); i++) {
        if (pages_[i] != nullptr && pages_[i] == active) {
            return true;
        }
    }
    return false;
}

void VehicleUi::Start() {
    if (timer_ != nullptr) {
        return;
    }
    timer_ = lv_timer_create(TimerEntry, kRefreshMs, this);
    preview_timer_ = lv_timer_create(PreviewTimerEntry, kPreviewIntervalMs, this);
    ESP_LOGI(TAG, "车载界面定时器已创建（%d ms 刷新 / %d ms 预览）", kRefreshMs, kPreviewIntervalMs);
}

void VehicleUi::TimerEntry(lv_timer_t* timer) {
    static_cast<VehicleUi*>(lv_timer_get_user_data(timer))->Tick();
}

void VehicleUi::PreviewTimerEntry(lv_timer_t* timer) {
    static_cast<VehicleUi*>(lv_timer_get_user_data(timer))->TickPreview();
}

void VehicleUi::RequestPage(Page page) {
    pending_page_.store(static_cast<int>(page));
}

void VehicleUi::RequestChatScreen() {
    pending_chat_.store(true);
}

void VehicleUi::Tick() {
    if (!built_) {
        // > 板级构造函数早于 Application::Initialize() 里的 display->SetupUI()，
        // > 所以界面必须等这个标志为真之后才能建。
        if (display_ == nullptr || !display_->IsSetupUICalled()) {
            return;
        }
        BuildOnce();
    }
    ApplyThemeAppearance();
    ApplyPendingNavigation();
    RefreshHome();
    RefreshEvents();
    RefreshSettings();
}

// > 上游会在运行中重新套用主题：main/assets.cc:335-340 的 RefreshDisplayTheme() 调
// > LcdDisplay::SetTheme()，而 SetTheme 改的是 **lv_screen_active()** 的字体与文字色
// > （lcd_display.cc:1149-1174）。所以这里每秒把我们自己页面的字体与底色重新贴一遍：
// > 字体能跟上 assets.Apply() 之后换上的 common 字体（内置 basic 只有 206 个汉字），
// > 底色也不会被主题覆盖。文字颜色不用管——每个 label 自己设了颜色，优先级高于继承。
void VehicleUi::ApplyThemeAppearance() {
    auto* theme = dynamic_cast<LvglTheme*>(display_->GetTheme());
    if (theme == nullptr) {
        return;
    }
    const std::shared_ptr<LvglFont> theme_font = theme->text_font();
    if (theme_font == nullptr) {
        return;
    }
    const lv_font_t* font = theme_font->font();
    if (font == nullptr) {
        return;
    }

    if (font != applied_font_) {
        for (int i = 0; i < static_cast<int>(Page::kCount); i++) {
            if (pages_[i] != nullptr) {
                // > 只贴在 screen 上，子控件继承；绝不逐个 label 设字体。
                lv_obj_set_style_text_font(pages_[i], font, 0);
            }
        }
        if (entry_button_ != nullptr) {
            lv_obj_t* label = lv_obj_get_child(entry_button_, 0);
            if (label != nullptr) {
                lv_obj_set_style_text_font(label, font, 0);
            }
        }
        applied_font_ = font;
        ESP_LOGI(TAG, "已套用主题字体（assets.Apply() 换上 common 字体后会在这一行跟上）");
    }

    for (int i = 0; i < static_cast<int>(Page::kCount); i++) {
        if (pages_[i] != nullptr) {
            lv_obj_set_style_bg_color(pages_[i], lv_color_hex(kBgColor), 0);
        }
    }
}

void VehicleUi::BuildOnce() {
    chat_screen_ = lv_screen_active();

    for (int i = 0; i < static_cast<int>(Page::kCount); i++) {
        pages_[i] = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(pages_[i], lv_color_hex(kBgColor), 0);
        lv_obj_clear_flag(pages_[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    BuildHome(pages_[static_cast<int>(Page::kHome)]);
    BuildPreview(pages_[static_cast<int>(Page::kPreview)]);
    BuildEvents(pages_[static_cast<int>(Page::kEvents)]);
    BuildSettings(pages_[static_cast<int>(Page::kSettings)]);

    // > 入口按钮挂在聊天界面（默认 screen）上：本工程从不使用 lv_layer_top()，
    // > 而且 top layer 不继承 screen 的字体（浮层文字得自己再设一次）。
    // > 上游的聊天容器在 SetupUI() 里已经建完，我们建得比它们晚，再用 lv_obj_move_foreground()
    // > 保一次序，避免被 container_ 盖住点不到。
    entry_button_ = MakeButton(chat_screen_, 6, 6, 110, 46, "车辆", [](lv_event_t* e) {
        auto* self = static_cast<VehicleUi*>(lv_event_get_user_data(e));
        if (self->OnOurPage()) {
            self->RequestChatScreen();
        } else {
            self->RequestPage(Page::kHome);
        }
    }, this);
    if (entry_button_ != nullptr && chat_screen_ != nullptr) {
        lv_obj_move_foreground(entry_button_);
    }

    built_ = true;
    ESP_LOGI(TAG, "车载界面已创建（4 页 + 聊天界面上的入口按钮）");
}

void VehicleUi::BuildHome(lv_obj_t* root) {
    lv_obj_t* title = MakeLabel(root, 130, 10, kTextColor);
    lv_label_set_text(title, "车辆状态");

    lv_obj_t* line1 = MakeLabel(root, 16, 70, kTextColor);
    lv_label_set_text(line1, "环境");
    home_temp_ = MakeLabel(root, 16, 110, kTextColor);
    home_humid_ = MakeLabel(root, 16, 150, kTextColor);
    home_lux_ = MakeLabel(root, 16, 190, kTextColor);

    lv_obj_t* line2 = MakeLabel(root, 250, 70, kTextColor);
    lv_label_set_text(line2, "行车");
    home_state_ = MakeLabel(root, 250, 110, kTextColor);
    home_events_ = MakeLabel(root, 250, 150, kTextColor);
    home_net_ = MakeLabel(root, 250, 190, kTextColor);
    home_axes_ = MakeLabel(root, 16, 236, kDimColor);

    BuildNavBar(root);
}

void VehicleUi::BuildPreview(lv_obj_t* root) {
    // > 顶部这行只在异常时出字（摄像头不可用），正常时是空 label。
    preview_status_ = MakeLabel(root, 12, 0, kDimColor);
    preview_capacity_ = static_cast<size_t>(kPreviewW) * kPreviewH * 2;
    preview_buf_ = static_cast<uint8_t*>(heap_caps_malloc(preview_capacity_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (preview_buf_ == nullptr) {
        ESP_LOGE(TAG, "预览缓冲分配失败（需要 %u B PSRAM），实时画面页不可用",
                 static_cast<unsigned>(preview_capacity_));
    } else {
        // > 先清零：第一帧到来之前画布显示的是这块内存的原始内容（未初始化就是花屏）。
        memset(preview_buf_, 0, preview_capacity_);
    }
    preview_canvas_ = lv_canvas_create(root);
    lv_obj_set_size(preview_canvas_, kPreviewW, kPreviewH);
    // > 显式放 y=26（画布 240 高 → 到 266），给底部 284 起的导航栏留位。
    lv_obj_set_pos(preview_canvas_, 8, 26);
    if (preview_buf_ != nullptr) {
        // > LV_COLOR_FORMAT_RGB565 的画布直接用我们的缓冲，之后每帧只改内容 + invalidate，
        // > 不每帧分配/释放图像对象（10 fps 下那种做法会把 PSRAM 打碎）。
        lv_canvas_set_buffer(preview_canvas_, preview_buf_, kPreviewW, kPreviewH, LV_COLOR_FORMAT_RGB565);
    }

    MakeButton(root, 340, 110, 130, 56, "抓拍", [](lv_event_t* e) {
        auto* self = static_cast<VehicleUi*>(lv_event_get_user_data(e));
        if (self->vehicle_ != nullptr) {
            self->vehicle_->RequestCapture();
        }
    }, this);

    BuildNavBar(root);
}

void VehicleUi::BuildEvents(lv_obj_t* root) {
    lv_obj_t* title = MakeLabel(root, 130, 10, kTextColor);
    lv_label_set_text(title, "事件记录");
    events_page_label_ = MakeLabel(root, 16, 52, kDimColor);

    // > 4 行 × 38 px：70 → 184，行高与 30 号字匹配，且给底部翻页键留位。
    for (int i = 0; i < kEventRows; i++) {
        event_rows_[i] = MakeLabel(root, 16, 92 + i * 38, kTextColor);
    }

    MakeButton(root, 130, 240, 100, 38, "上一页", [](lv_event_t* e) {
        auto* self = static_cast<VehicleUi*>(lv_event_get_user_data(e));
        if (self->events_page_ > 0) {
            self->events_page_--;
        }
    }, this);
    MakeButton(root, 250, 240, 100, 38, "下一页", [](lv_event_t* e) {
        auto* self = static_cast<VehicleUi*>(lv_event_get_user_data(e));
        self->events_page_++;   // RefreshEvents() 里会按实际条数夹住上限
    }, this);

    BuildNavBar(root);
}

void VehicleUi::BuildSettings(lv_obj_t* root) {
    lv_obj_t* title = MakeLabel(root, 16, 10, kTextColor);
    lv_label_set_text(title, "设置");
    // > 状态文字放标题右侧（底部 284 起被导航栏占了）。
    settings_lock_btn_label_ = MakeLabel(root, 250, 10, kDimColor);

    // ! 一行一个标签、行距 36 px、每行都短到不会折行。
    // ! 原来把三行文本塞进一个 label 再按单行行距排后面的控件，折行后必然压字（BUG-021）。
    const vehicle::MonitorConfig cfg = vehicle_->config();
    char buf[96];
    // ! 不要用 %lld：本工程是 nano printf（见 docs/BUGS.md BUG-001）。这里全是 32 位与浮点。

    snprintf(buf, sizeof(buf), "加速 %.2fg  转弯 %.2fg", static_cast<double>(cfg.accel_threshold),
             static_cast<double>(cfg.turn_threshold));
    lv_obj_t* row1 = MakeLabel(root, 16, 56, kDimColor);
    lv_label_set_text(row1, buf);

    snprintf(buf, sizeof(buf), "颠簸 %.2fg  碰撞 %.2fg", static_cast<double>(cfg.bump_threshold),
             static_cast<double>(cfg.crash_threshold));
    lv_obj_t* row2 = MakeLabel(root, 16, 92, kDimColor);
    lv_label_set_text(row2, buf);

    snprintf(buf, sizeof(buf), "停车 %d s  锁车 %d s", static_cast<int>(cfg.static_hold_ms / 1000),
             static_cast<int>(cfg.lock_hold_ms / 1000));
    lv_obj_t* row3 = MakeLabel(root, 16, 128, kDimColor);
    lv_label_set_text(row3, buf);

    // > 环境源必须标"模拟"（无硬件），事件容量顺带放这一行。
    snprintf(buf, sizeof(buf), "环境源：模拟  事件容量 %d", vehicle_->history_capacity());
    settings_env_label_ = MakeLabel(root, 16, 164, kDimColor);
    lv_label_set_text(settings_env_label_, buf);

    settings_axes_label_ = MakeLabel(root, 16, 200, kTextColor);
    lv_label_set_text(settings_axes_label_, "显示三轴实时值");
    settings_axes_switch_ = lv_switch_create(root);
    lv_obj_set_pos(settings_axes_switch_, 300, 190);
    lv_obj_add_event_cb(settings_axes_switch_, [](lv_event_t* e) {
        auto* self = static_cast<VehicleUi*>(lv_event_get_user_data(e));
        // > lv_event_get_target() 返回 void*，C++ 里必须显式转，别直接传给 lv_obj_has_state()。
        self->axes_visible_ = lv_obj_has_state(static_cast<lv_obj_t*>(lv_event_get_target(e)), LV_STATE_CHECKED);
    }, LV_EVENT_VALUE_CHANGED, this);

    MakeButton(root, 16, 236, 140, 42, "锁车监测", [](lv_event_t* e) {
        auto* self = static_cast<VehicleUi*>(lv_event_get_user_data(e));
        self->vehicle_->RequestLock(true);
    }, this);
    MakeButton(root, 166, 236, 140, 42, "解除锁车", [](lv_event_t* e) {
        auto* self = static_cast<VehicleUi*>(lv_event_get_user_data(e));
        self->vehicle_->RequestLock(false);
    }, this);
    MakeButton(root, 316, 236, 150, 42, "立即抓拍", [](lv_event_t* e) {
        static_cast<VehicleUi*>(lv_event_get_user_data(e))->vehicle_->RequestCapture();
    }, this);

    BuildNavBar(root);
}

// > 五个键直接调 RequestPage()/RequestChatScreen()，不依赖 MCP 工具、也不依赖语音。
// > ! 没有它，除主页外的三页根本进不去（真机验收时踩过，见 docs/BUGS.md BUG-019）。
void VehicleUi::BuildNavBar(lv_obj_t* root) {
    MakeButton(root, 6, kNavBarY, kNavButtonW, kNavBarH, "主页", [](lv_event_t* e) {
        static_cast<VehicleUi*>(lv_event_get_user_data(e))->RequestPage(Page::kHome);
    }, this);
    MakeButton(root, 6 + kNavButtonGap, kNavBarY, kNavButtonW, kNavBarH, "画面", [](lv_event_t* e) {
        static_cast<VehicleUi*>(lv_event_get_user_data(e))->RequestPage(Page::kPreview);
    }, this);
    MakeButton(root, 6 + kNavButtonGap * 2, kNavBarY, kNavButtonW, kNavBarH, "事件", [](lv_event_t* e) {
        static_cast<VehicleUi*>(lv_event_get_user_data(e))->RequestPage(Page::kEvents);
    }, this);
    MakeButton(root, 6 + kNavButtonGap * 3, kNavBarY, kNavButtonW, kNavBarH, "设置", [](lv_event_t* e) {
        static_cast<VehicleUi*>(lv_event_get_user_data(e))->RequestPage(Page::kSettings);
    }, this);
    MakeButton(root, 6 + kNavButtonGap * 4, kNavBarY, kNavButtonW, kNavBarH, "返回", [](lv_event_t* e) {
        static_cast<VehicleUi*>(lv_event_get_user_data(e))->RequestChatScreen();
    }, this);
}

void VehicleUi::ApplyPendingNavigation() {
    const int want = pending_page_.exchange(-1);
    const bool want_chat = pending_chat_.exchange(false);

    if (want >= 0 && want < static_cast<int>(Page::kCount)) {
        // > 只在"当前不在自定义页"时记聊天界面，否则页内互切会把聊天界面覆盖掉。
        if (!OnOurPage() && lv_screen_active() != nullptr) {
            chat_screen_ = lv_screen_active();
        }
        lv_screen_load(pages_[want]);
        preview_active_ = (want == static_cast<int>(Page::kPreview));
        if (preview_active_) {
            preview_frames_ = 0;
            preview_misses_ = 0;
            preview_fail_since_ms_ = 0;
            preview_blank_ = false;
            preview_notice_ = nullptr;
            preview_window_start_ms_ = esp_timer_get_time() / 1000;
        }
    } else if (want_chat && chat_screen_ != nullptr) {
        lv_screen_load(chat_screen_);
        preview_active_ = false;
    }
}

void VehicleUi::RefreshHome() {
    if (lv_screen_active() != pages_[static_cast<int>(Page::kHome)]) {
        return;   // 只有当前页在刷新，省 CPU
    }
    const vehicle::VehicleStatus status = vehicle_->Status();
    const vehicle::EnvReading env = vehicle_->env();

    char buf[96];
    if (env.valid) {
        snprintf(buf, sizeof(buf), "温度 %.1f C", static_cast<double>(env.temp_c));
        lv_label_set_text(home_temp_, buf);
        snprintf(buf, sizeof(buf), "湿度 %.0f %%", static_cast<double>(env.humidity_pct));
        lv_label_set_text(home_humid_, buf);
        snprintf(buf, sizeof(buf), "光照 %d lux（%s）", static_cast<int>(env.lux),
                 vehicle::ToString(vehicle::BucketLight(env.lux)));
        lv_label_set_text(home_lux_, buf);
    } else {
        lv_label_set_text(home_temp_, "温度 --");
        lv_label_set_text(home_humid_, "湿度 --");
        lv_label_set_text(home_lux_, "光照 --");
    }

    lv_label_set_text(home_state_, status.calibrated ? StateText(status.state) : "标定中");
    snprintf(buf, sizeof(buf), "事件 %d 次", static_cast<int>(status.events_total));
    lv_label_set_text(home_events_, buf);
    lv_label_set_text(home_net_, Board::GetInstance().GetNetworkStateIcon());
    RefreshAxes();
}

void VehicleUi::RefreshAxes() {
    if (home_axes_ == nullptr) {
        return;
    }
    if (!axes_visible_) {
        lv_label_set_text(home_axes_, "");
        return;
    }
    const vehicle::ImuSample s = vehicle_->Status().sample;
    char buf[96];
    snprintf(buf, sizeof(buf), "ax %.2f ay %.2f az %.2f g", static_cast<double>(s.ax), static_cast<double>(s.ay),
             static_cast<double>(s.az));
    lv_label_set_text(home_axes_, buf);
}

void VehicleUi::RefreshEvents() {
    if (event_rows_[0] == nullptr || lv_screen_active() != pages_[static_cast<int>(Page::kEvents)]) {
        return;
    }
    const std::vector<vehicle::EventRecord> all = vehicle_->CopyHistory();
    const int total = static_cast<int>(all.size());
    int pages = (total + kEventRows - 1) / kEventRows;
    if (pages < 1) {
        pages = 1;
    }
    if (events_page_ < 0) {
        events_page_ = 0;
    }
    if (events_page_ > pages - 1) {
        events_page_ = pages - 1;
    }

    for (int row = 0; row < kEventRows; row++) {
        // > 第 0 页显示最新的一批：从末尾往回数
        const int from_newest = events_page_ * kEventRows + row;
        if (from_newest >= total) {
            lv_label_set_text(event_rows_[row], "");
            continue;
        }
        const vehicle::EventRecord& record = all[static_cast<size_t>(total - 1 - from_newest)];
        lv_label_set_text(event_rows_[row], vehicle::FormatEventLine(record).c_str());
    }

    // > 页码文字要短：30 号字一个汉字 30 px，480 px 宽塞不下超过约 14 个汉字。
    // > 这里只报"页/总页 + 条数"，容量改在设置页说明，别把 label 撑出屏外。
    char buf[64];
    snprintf(buf, sizeof(buf), "第 %d/%d 页  共 %d 条", events_page_ + 1, pages, total);
    lv_label_set_text(events_page_label_, buf);
}

void VehicleUi::RefreshSettings() {
    if (settings_lock_btn_label_ == nullptr || lv_screen_active() != pages_[static_cast<int>(Page::kSettings)]) {
        return;
    }
    const vehicle::VehicleStatus status = vehicle_->Status();
    char buf[96];
    snprintf(buf, sizeof(buf), "当前：%s%s", StateText(status.state), status.calibrated ? "" : "（标定中）");
    lv_label_set_text(settings_lock_btn_label_, buf);
}

void VehicleUi::ShowPreviewNotice(const char* text, bool blank) {
    if (preview_status_ != nullptr && preview_notice_ != text) {
        preview_notice_ = text;
        lv_label_set_text(preview_status_, text);
    }
    if (!blank || preview_blank_ || preview_buf_ == nullptr || preview_canvas_ == nullptr) {
        return;
    }
    // > 画面刷黑一次就够：相机被关掉后不该再显示最后一帧（用户预期是"预览黑屏"）。
    memset(preview_buf_, 0, preview_capacity_);
    lv_obj_invalidate(preview_canvas_);
    preview_blank_ = true;
}

void VehicleUi::TickPreview() {
    if (!built_ || !preview_active_ || preview_canvas_ == nullptr || preview_buf_ == nullptr || camera_ == nullptr) {
        return;
    }
    if (lv_screen_active() != pages_[static_cast<int>(Page::kPreview)]) {
        return;
    }
    // ! 相机已被 self.camera.set_enabled(false) 关掉时，这里**一次都不能去取帧**：
    // ! GC0308 已断电，esp_camera_fb_get() 只会白等 4000 ms，而本函数跑在 LVGL 任务里，
    // ! 结果就是屏幕按钮全无响应、只能复位（docs/BUGS.md BUG-029 的实测现象）。
    if (!camera_->enabled()) {
        // > 顺手把 fps 统计窗清掉：相机停用期间没有帧可数，不清的话"重新打开"那一轮会打出一条
        // > `预览实测 0.1 fps` 的假读数（真机实测），看着像性能坏了。
        preview_frames_ = 0;
        preview_misses_ = 0;
        preview_window_start_ms_ = esp_timer_get_time() / 1000;
        ShowPreviewNotice(kNoticeCameraOff, true);
        return;
    }

    // ! 这里**刻意不做**"非待机态暂停预览"（计划任务 9 原本要求这么做），两条依据：
    // !   1. 真机上它表现为"小智一唤醒，画面就卡在最后一帧"（2026-09-17 用户实测反馈）；
    // !   2. 不再消费帧之后，上游 Esp32Camera::Capture() 又长期攥着一帧不放，驱动更容易凑不出
    // !      空缓冲而停在 IDLE（实测停摆后 `cam_hal: Failed to get frame: timeout` 每 4.1 s 一条、
    // !      不再恢复，见 docs/BUGS.md BUG-025）。
    // ! 代价：对话时多占一点 SPI/CPU 带宽（~13 fps 时 LCD flush 是大头）。
    // ? 若后续确认"对话时预览"会拖累小智拍照的上传（只出现过一次挂死，未证实相关），
    // ? 再改成"对话时降频预览"，而不是完全暂停。

    int w = 0;
    int h = 0;
    if (!camera_->CopyPreviewFrame(preview_buf_, preview_capacity_, &w, &h)) {
        preview_misses_++;
        // > 连续失败 ≥3 s 就上屏提示，避免"黑屏但不知道哪里坏了"。
        // > 相机初始化失败时 Esp32Camera 只打日志并让 streaming_on_ = false，
        // > 我们从 esp_camera_fb_get() 只会拿到 NULL，必须自己把这件事说出来。
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (preview_fail_since_ms_ == 0) {
            preview_fail_since_ms_ = now_ms;
        } else if (now_ms - preview_fail_since_ms_ >= kPreviewFailHintMs) {
            ShowPreviewNotice(kNoticeCameraBad, false);
        }
        return;
    }
    preview_fail_since_ms_ = 0;
    // > 出帧了就说明相机是好的：黑屏标记与提示一起清掉（画面随后会被这一帧覆盖）
    preview_blank_ = false;
    ShowPreviewNotice(kNoticeNone, false);
    if (w != kPreviewW || h != kPreviewH) {
        return;
    }
    lv_obj_invalidate(preview_canvas_);
    preview_frames_++;

    // > 设计文档 §10.2 要求预览 ≥10 fps，这里每 5 s 打一次实测值。
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - preview_window_start_ms_ >= 5000) {
        const double seconds = static_cast<double>(now_ms - preview_window_start_ms_) / 1000.0;
        ESP_LOGI(TAG, "预览实测 %.1f fps（%d 帧 / %.1f s，丢帧 %d）",
                 preview_frames_ / (seconds > 0 ? seconds : 1.0), preview_frames_, seconds, preview_misses_);
        preview_frames_ = 0;
        preview_misses_ = 0;
        preview_window_start_ms_ = now_ms;
    }
}
