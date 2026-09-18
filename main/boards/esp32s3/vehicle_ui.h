#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include <lvgl.h>

#include "vehicle_service.h"

class Display;
class CameraCapture;

// 自定义车载页面：主页 / 实时画面 / 事件记录 / 设置。
//
// ! 四个必须记住的约束：
// !   1. 所有 LVGL 调用只能发生在 LVGL 任务里——要么是本类的 lv_timer 回调，
// !      要么是触摸事件回调（同样由 LVGL 任务派发）。跨任务请求（MCP 工具、Plan C 的语音）
// !      只写下面几个 atomic 成员，由定时器读取后执行。
// !   2. 四个页面各是一个独立 screen；进自定义页之前先记住当时的 lv_screen_active()（聊天界面），
// !      "返回"时切回去。
// !   3. 界面延迟到 display->IsSetupUICalled() 为真之后再建（板级构造函数跑在 SetupUI() 之前）。
// !   4. ! 字体只用"主题当前字体"，并且每秒重新贴一次——绝不要硬编码 &BUILTIN_TEXT_FONT：
// !      内置 font_puhui_basic_30_4 只有 206 个汉字（"车/速/事/件/记/置/画"都不在里面），
// !      运行时真正可用的 font_puhui_common_30_4 要等 assets.Apply() 才装进主题；
// !      而本工程没开 LV_USE_FONT_PLACEHOLDER，缺字是"直接消失"而不是方框，很难查。
class VehicleUi {
public:
    enum class Page : uint8_t { kHome = 0, kPreview, kEvents, kSettings, kCount };

    VehicleUi(Display* display, VehicleService* vehicle, CameraCapture* camera);
    ~VehicleUi();

    // 只建定时器；真正的界面在第一次 tick 且 IsSetupUICalled() 为真时创建
    void Start();

    // 切到某个自定义页（任意任务可调）；"chat" 表示切回聊天界面
    void RequestPage(Page page);
    void RequestChatScreen();

private:
    static void TimerEntry(lv_timer_t* timer);
    static void PreviewTimerEntry(lv_timer_t* timer);

    void Tick();
    void TickPreview();
    void ApplyPendingNavigation();
    void ApplyThemeAppearance();
    void BuildOnce();
    void BuildHome(lv_obj_t* root);
    void BuildPreview(lv_obj_t* root);
    void BuildEvents(lv_obj_t* root);
    void BuildSettings(lv_obj_t* root);
    // 每页底部的 5 键导航栏（主页/画面/事件/设置/返回）——没有它，除主页外的页面进不去
    void BuildNavBar(lv_obj_t* root);
    void RefreshHome();
    void RefreshEvents();
    void RefreshSettings();
    void RefreshAxes();
    // 预览页异常提示：blank = true 时把画面刷成黑色（"摄像头已关闭"用）
    void ShowPreviewNotice(const char* text, bool blank);

    bool OnOurPage() const;

    Display* display_ = nullptr;
    VehicleService* vehicle_ = nullptr;
    CameraCapture* camera_ = nullptr;

    lv_timer_t* timer_ = nullptr;
    lv_timer_t* preview_timer_ = nullptr;
    bool built_ = false;
    bool axes_visible_ = false;
    int events_page_ = 0;

    lv_obj_t* chat_screen_ = nullptr;
    lv_obj_t* pages_[static_cast<int>(Page::kCount)] = {};
    lv_obj_t* entry_button_ = nullptr;
    const lv_font_t* applied_font_ = nullptr;   // 已贴过的主题字体，避免每秒重复 set 样式

    lv_obj_t* home_temp_ = nullptr;
    lv_obj_t* home_humid_ = nullptr;
    lv_obj_t* home_lux_ = nullptr;
    lv_obj_t* home_state_ = nullptr;
    lv_obj_t* home_events_ = nullptr;
    lv_obj_t* home_axes_ = nullptr;
    lv_obj_t* home_net_ = nullptr;

    lv_obj_t* preview_canvas_ = nullptr;
    uint8_t* preview_buf_ = nullptr;
    size_t preview_capacity_ = 0;
    bool preview_active_ = false;
    int preview_frames_ = 0;
    int preview_misses_ = 0;
    int64_t preview_window_start_ms_ = 0;
    lv_obj_t* preview_status_ = nullptr;   // 预览页顶部状态文字（仅异常时显示）
    // > 提示按**时间**判定，不按次数：相机停摆时一次取帧要 4 s，按"连续 30 次"要等约 120 s 才上屏
    // > （docs/BUGS.md BUG-029）。
    int64_t preview_fail_since_ms_ = 0;   // 0 = 当前没有连续失败
    bool preview_blank_ = false;          // 画面已刷成黑屏（"摄像头已关闭"提示只刷一次）
    const char* preview_notice_ = nullptr;   // 已上屏的提示（同一个字符串字面量才不重复写）

    lv_obj_t* event_rows_[4] = {};   // 与 vehicle_ui.cc 的 kEventRows 保持一致（导航栏占了底部 40 px）
    lv_obj_t* events_page_label_ = nullptr;

    lv_obj_t* settings_lock_btn_label_ = nullptr;
    lv_obj_t* settings_axes_switch_ = nullptr;
    lv_obj_t* settings_axes_label_ = nullptr;
    lv_obj_t* settings_env_label_ = nullptr;

    std::atomic<int> pending_page_{-1};   // -1 = 无请求
    std::atomic<bool> pending_chat_{false};
};
