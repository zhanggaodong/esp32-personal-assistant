#include "screen_screensaver.h"

#include <cstring>
#include <ctime>
#include <sstream>
#include <vector>

#include <esp_log.h>
#include <esp_heap_caps.h>

#include <lvgl.h>

#include "board.h"
#include "display/display.h"
#include "config/config_store.h"
#include "storage/littlefs_store.h"
#include "display/lvgl_display/jpg/jpeg_to_image.h"

#define TAG "ScreenScreensaver"

// 解析 "#RRGGBB" 字符串为 lv_color_t
static lv_color_t ParseColor(const std::string& s, uint8_t fallback_r, uint8_t fallback_g, uint8_t fallback_b) {
    uint8_t r = fallback_r, g = fallback_g, b = fallback_b;
    if (s.size() >= 7 && s[0] == '#') {
        auto hex2 = [](const char* p) -> int {
            int v = 0;
            for (int i = 0; i < 2; ++i) {
                char c = p[i];
                v <<= 4;
                if (c >= '0' && c <= '9') v |= (c - '0');
                else if (c >= 'a' && c <= 'f') v |= (c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') v |= (c - 'A' + 10);
            }
            return v;
        };
        r = (uint8_t)hex2(s.c_str() + 1);
        g = (uint8_t)hex2(s.c_str() + 3);
        b = (uint8_t)hex2(s.c_str() + 5);
    }
    return lv_color_make(r, g, b);
}

// 判断 "content" 字符串（逗号分隔）是否包含某项
static bool ContentHas(const std::string& content, const char* item) {
    if (content.empty() || item == nullptr) {
        return false;
    }
    std::istringstream ss(content);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        while (!tok.empty() && tok.front() == ' ') tok.erase(tok.begin());
        while (!tok.empty() && tok.back() == ' ') tok.pop_back();
        if (tok == item) {
            return true;
        }
    }
    return false;
}

// 从 LittleFS 读取并解码背景图到 bg_buf_/bg_dsc_。返回是否成功。
static bool TryDecodeBackground(const std::string& file, uint8_t** out_buf, size_t* out_len,
                                int* out_w, int* out_h, void** out_dsc) {
    std::vector<uint8_t> raw;
    if (file.empty() || !LittleFsStore::ReadFile(file.c_str(), raw) || raw.empty()) {
        return false;
    }
    uint8_t* pix = nullptr;
    size_t len = 0, w = 0, h = 0, stride = 0;
    if (jpeg_to_image(raw.data(), raw.size(), &pix, &len, &w, &h, &stride) != ESP_OK || pix == nullptr) {
        return false;
    }
    lv_img_dsc_t* dsc = new lv_img_dsc_t();
    bzero(dsc, sizeof(lv_img_dsc_t));
    dsc->data_size = len;
    dsc->data = pix;
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.w = (int)w;
    dsc->header.h = (int)h;
    dsc->header.stride = ((int)w) * 2;

    *out_buf = pix;
    if (out_len != nullptr) { *out_len = len; }
    *out_w = (int)w;
    *out_h = (int)h;
    if (out_dsc != nullptr) { *out_dsc = dsc; }
    return true;
}

ScreenScreensaver::ScreenScreensaver() {
    metadata_ = {"screensaver", "息屏", false};
}

ScreenScreensaver::~ScreenScreensaver() {
    onStop();
}

const AppMetadata& ScreenScreensaver::metadata() {
    return metadata_;
}

void ScreenScreensaver::LoadBackground() {
    if (bg_dsc_ != nullptr) {
        delete static_cast<lv_img_dsc_t*>(bg_dsc_);
        bg_dsc_ = nullptr;
    }
    if (bg_buf_ != nullptr) {
        heap_caps_free(bg_buf_);
        bg_buf_ = nullptr;
        bg_w_ = 0;
        bg_h_ = 0;
    }
}

// 每秒定时器回调：仅刷新时间/日期，避免全屏重建
static void OnClockTick(void* arg) {
    ScreenScreensaver* self = static_cast<ScreenScreensaver*>(arg);
    self->RefreshClock();
}

void ScreenScreensaver::onStart() {
    if (started_) {
        return;
    }
    started_ = true;
    if (clock_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = OnClockTick;
        args.arg = this;
        args.name = "screensaver_clock";
        esp_timer_create(&args, &clock_timer_);
    }
    ESP_LOGI(TAG, "Screensaver started");
}

void ScreenScreensaver::Rebuild() {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard lock(display);

    auto& store = ConfigStore::Instance();
    lv_obj_t* scr = lv_screen_active();

    // 1) 背景（纯黑打底，或解码背景图）
    if (bg_obj_ == nullptr) {
        bool has_bg = TryDecodeBackground(store.Get("screensaver.background"),
                                          &bg_buf_, nullptr, &bg_w_, &bg_h_, &bg_dsc_);
        if (has_bg) {
            lv_obj_t* img = lv_image_create(scr);
            lv_image_set_src(img, bg_dsc_);
            lv_image_set_scale(img, lv_image_scale_from_width(img, LV_HOR_RES));
            lv_obj_set_pos(img, 0, 0);
            bg_obj_ = img;
        } else {
            lv_obj_t* solid = lv_obj_create(scr);
            lv_obj_set_style_bg_opa(solid, LV_OPA_COVER, 0);
            bg_obj_ = solid;
        }
    }
    if (bg_obj_ != nullptr) {
        lv_obj_set_size(bg_obj_, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_pos(bg_obj_, 0, 0);
        lv_obj_set_style_border_width(bg_obj_, 0, 0);
        lv_obj_set_style_bg_color(bg_obj_, lv_color_black(), 0);
    }

    // 2) 文字公共样式
    std::string color = store.Get("screensaver.font_color");
    lv_color_t fg = ParseColor(color, 255, 255, 255);
    int font_size = store.GetInt("screensaver.font_size", 40);
    const lv_font_t* font = font_size >= 32 ? &lv_font_montserrat_32
                            : (font_size >= 24 ? &lv_font_montserrat_24 : &lv_font_montserrat_14);
    std::string align = store.Get("screensaver.align");
    lv_text_align_t ta = align == "left" ? LV_TEXT_ALIGN_LEFT
                         : align == "right" ? LV_TEXT_ALIGN_RIGHT
                         : LV_TEXT_ALIGN_CENTER;

    const lv_font_t* small_font = font_size >= 32 ? &lv_font_montserrat_24 : &lv_font_montserrat_14;

    std::string content = store.Get("screensaver.content");
    bool show_time = ContentHas(content, "time");
    bool show_date = ContentHas(content, "date");
    bool show_custom = ContentHas(content, "custom");

    // 3) 时间标签
    if (time_label_ == nullptr) {
        time_label_ = lv_label_create(scr);
    }
    lv_obj_set_style_text_color(time_label_, fg, 0);
    lv_obj_set_style_text_font(time_label_, font, 0);
    lv_obj_set_width(time_label_, LV_HOR_RES);
    lv_obj_set_style_text_align(time_label_, ta, 0);
    lv_obj_add_flag(time_label_, LV_OBJ_FLAG_HIDDEN);
    if (show_time) {
        lv_obj_clear_flag(time_label_, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(time_label_, "--:--");
        lv_obj_set_pos(time_label_, 0, 70);
    }

    // 4) 日期标签
    if (date_label_ == nullptr) {
        date_label_ = lv_label_create(scr);
    }
    lv_obj_set_style_text_color(date_label_, fg, 0);
    lv_obj_set_style_text_font(date_label_, small_font, 0);
    lv_obj_set_width(date_label_, LV_HOR_RES);
    lv_obj_set_style_text_align(date_label_, ta, 0);
    lv_obj_add_flag(date_label_, LV_OBJ_FLAG_HIDDEN);
    if (show_date) {
        lv_obj_clear_flag(date_label_, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(date_label_, "----.--.--");
        lv_obj_set_pos(date_label_, 0, 130);
    }

    // 5) 自定文本
    if (custom_label_ == nullptr) {
        custom_label_ = lv_label_create(scr);
    }
    lv_obj_set_style_text_color(custom_label_, fg, 0);
    lv_obj_set_style_text_font(custom_label_, small_font, 0);
    lv_obj_set_width(custom_label_, LV_HOR_RES);
    lv_obj_set_style_text_align(custom_label_, ta, 0);
    lv_obj_add_flag(custom_label_, LV_OBJ_FLAG_HIDDEN);
    if (show_custom) {
        lv_obj_clear_flag(custom_label_, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(custom_label_, store.Get("screensaver.custom_text").c_str());
        lv_obj_set_pos(custom_label_, 0, LV_VER_RES - 60);
    }

    ESP_LOGI(TAG, "Screensaver rebuilt (time=%d date=%d custom=%d)", show_time, show_date, show_custom);
}

void ScreenScreensaver::RefreshClock() {
    if (!visible_ || time_label_ == nullptr) {
        return;
    }
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard lock(display);

    auto& store = ConfigStore::Instance();
    bool show_time = ContentHas(store.Get("screensaver.content"), "time");
    bool show_date = ContentHas(store.Get("screensaver.content"), "date");
    bool is24h = store.GetBool("screensaver.clock_24h", true);

    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);

    char buf[32];
    if (show_time && time_label_ != nullptr) {
        if (is24h) {
            snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        } else {
            int h = tmv.tm_hour % 12;
            if (h == 0) { h = 12; }
            snprintf(buf, sizeof(buf), "%02d:%02d %s", h, tmv.tm_min, tmv.tm_hour < 12 ? "AM" : "PM");
        }
        lv_label_set_text(time_label_, buf);
    }
    if (show_date && date_label_ != nullptr) {
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
        lv_label_set_text(date_label_, buf);
    }
}

void ScreenScreensaver::onShow() {
    if (!started_) {
        onStart();
    }
    visible_ = true;
    Rebuild();
    RefreshClock();
    if (clock_timer_ != nullptr) {
        esp_timer_start_periodic(clock_timer_, 1000 * 1000);  // 每秒
    }
    ESP_LOGI(TAG, "Screensaver shown");
}

void ScreenScreensaver::onHide() {
    visible_ = false;
    if (clock_timer_ != nullptr) {
        esp_timer_stop(clock_timer_);
    }
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard lock(display);
    if (bg_obj_ != nullptr) lv_obj_add_flag(bg_obj_, LV_OBJ_FLAG_HIDDEN);
    if (time_label_ != nullptr) lv_obj_add_flag(time_label_, LV_OBJ_FLAG_HIDDEN);
    if (date_label_ != nullptr) lv_obj_add_flag(date_label_, LV_OBJ_FLAG_HIDDEN);
    if (custom_label_ != nullptr) lv_obj_add_flag(custom_label_, LV_OBJ_FLAG_HIDDEN);
}

void ScreenScreensaver::onStop() {
    visible_ = false;
    if (clock_timer_ != nullptr) {
        esp_timer_stop(clock_timer_);
        esp_timer_delete(clock_timer_);
        clock_timer_ = nullptr;
    }
    auto* display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        DisplayLockGuard lock(display);
        if (bg_obj_ != nullptr) { lv_obj_delete(bg_obj_); bg_obj_ = nullptr; }
        if (time_label_ != nullptr) { lv_obj_delete(time_label_); time_label_ = nullptr; }
        if (date_label_ != nullptr) { lv_obj_delete(date_label_); date_label_ = nullptr; }
        if (custom_label_ != nullptr) { lv_obj_delete(custom_label_); custom_label_ = nullptr; }
    }
    LoadBackground();
    ESP_LOGI(TAG, "Screensaver stopped");
}

void ScreenScreensaver::onConfigChanged(const char* key) {
    if (key == nullptr) {
        return;
    }
    const char* sa_keys[] = {"screensaver.background", "screensaver.content", "screensaver.custom_text",
                             "screensaver.font_color", "screensaver.font_size", "screensaver.align",
                             "screensaver.clock_24h"};
    for (const char* k : sa_keys) {
        if (strcmp(k, key) == 0) {
            Rebuild();
            return;
        }
    }
}