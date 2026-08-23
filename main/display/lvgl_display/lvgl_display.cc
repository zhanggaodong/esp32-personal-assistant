#include <esp_log.h>
#include <esp_err.h>
#include <string>
#include <cstdlib>
#include <cstring>
#include <font_awesome.h>
#include <cbin_font.h>

#include "lvgl_display.h"
#include "board.h"
#include "application.h"
#include "audio_codec.h"
#include "settings.h"
#include "assets/lang_config.h"
#include "jpg/image_to_jpeg.h"

#define TAG "Display"

extern const uint8_t font_custom_80_4_bin_start[] asm("_binary_font_custom_80_4_bin_start");

LV_FONT_DECLARE(font_custom_80_4);

LvglDisplay::LvglDisplay() {
    // Notification timer
    esp_timer_create_args_t notification_timer_args = {
        .callback = [](void *arg) {
            LvglDisplay *display = static_cast<LvglDisplay*>(arg);
            DisplayLockGuard lock(display);
            lv_obj_add_flag(display->notification_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(display->status_label_, LV_OBJ_FLAG_HIDDEN);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "notification_timer",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&notification_timer_args, &notification_timer_));

    // Create a power management lock
    auto ret = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "display_update", &pm_lock_);
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG, "Power management not supported");
    } else {
        ESP_ERROR_CHECK(ret);
    }
}

LvglDisplay::~LvglDisplay() {
    if (notification_timer_ != nullptr) {
        esp_timer_stop(notification_timer_);
        esp_timer_delete(notification_timer_);
    }

    if (network_label_ != nullptr) {
        lv_obj_del(network_label_);
    }
    if (notification_label_ != nullptr) {
        lv_obj_del(notification_label_);
    }
    if (status_label_ != nullptr) {
        lv_obj_del(status_label_);
    }
    if (mute_label_ != nullptr) {
        lv_obj_del(mute_label_);
    }
    
    // Countdown UI cleanup
    
    // Countdown UI cleanup
    if (countdown_time_font_ != nullptr) {
        if (countdown_time_font_ != (lv_font_t*)LV_FONT_DEFAULT) {
            cbin_font_delete(countdown_time_font_);
        }
        countdown_time_font_ = nullptr;
    }
    if (countdown_container_ != nullptr) {
        lv_obj_del(countdown_container_);
        countdown_container_ = nullptr;
    }
    
    if (battery_label_ != nullptr) {
        lv_obj_del(battery_label_);
    }
    if( low_battery_popup_ != nullptr ) {
        lv_obj_del(low_battery_popup_);
    }
    if (pm_lock_ != nullptr) {
        esp_pm_lock_delete(pm_lock_);
    }
}

void LvglDisplay::SetStatus(const char* status) {
    DisplayLockGuard lock(this);
    if (status_label_ == nullptr) {
        return;
    }
    lv_label_set_text(status_label_, status);
    lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    last_status_update_time_ = std::chrono::system_clock::now();
}

void LvglDisplay::ShowNotification(const std::string &notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void LvglDisplay::ShowNotification(const char* notification, int duration_ms) {
    DisplayLockGuard lock(this);
    if (notification_label_ == nullptr) {
        return;
    }
    lv_label_set_text(notification_label_, notification);
    lv_obj_remove_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);

    esp_timer_stop(notification_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(notification_timer_, duration_ms * 1000));
}

void LvglDisplay::UpdateStatusBar(bool update_all) {
    auto& app = Application::GetInstance();
    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();

    // Update mute icon
    {
        DisplayLockGuard lock(this);
        if (mute_label_ == nullptr) {
            return;
        }

        // Update icon if mute state changes
        if (codec->output_volume() == 0 && !muted_) {
            muted_ = true;
            lv_label_set_text(mute_label_, FONT_AWESOME_VOLUME_XMARK);
        } else if (codec->output_volume() > 0 && muted_) {
            muted_ = false;
            lv_label_set_text(mute_label_, "");
        }
    }

    // Update time
    if (app.GetDeviceState() == kDeviceStateIdle) {
        if (last_status_update_time_ + std::chrono::seconds(10) < std::chrono::system_clock::now()) {
            // Set status to clock "HH:MM"
            time_t now = time(NULL);
            struct tm* tm = localtime(&now);
            // Check if the we have already set the time
            if (tm->tm_year >= 2025 - 1900) {
                char time_str[16];
                strftime(time_str, sizeof(time_str), "%H:%M", tm);
                SetStatus(time_str);
            } else {
                ESP_LOGW(TAG, "System time is not set, tm_year: %d", tm->tm_year);
            }
        }
    }

    esp_pm_lock_acquire(pm_lock_);
    // Update battery icon
    int battery_level;
    bool charging, discharging;
    const char* icon = nullptr;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        if (charging) {
            icon = FONT_AWESOME_BATTERY_BOLT;
        } else {
            const char* levels[] = {
                FONT_AWESOME_BATTERY_EMPTY, // 0-19%
                FONT_AWESOME_BATTERY_QUARTER,    // 20-39%
                FONT_AWESOME_BATTERY_HALF,    // 40-59%
                FONT_AWESOME_BATTERY_THREE_QUARTERS,    // 60-79%
                FONT_AWESOME_BATTERY_FULL, // 80-99%
                FONT_AWESOME_BATTERY_FULL, // 100%
            };
            icon = levels[battery_level / 20];
        }
        DisplayLockGuard lock(this);
        if (battery_label_ != nullptr && battery_icon_ != icon) {
            battery_icon_ = icon;
            lv_label_set_text(battery_label_, battery_icon_);
        }

        if (low_battery_popup_ != nullptr) {
            if (strcmp(icon, FONT_AWESOME_BATTERY_EMPTY) == 0 && discharging) {
                if (lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN)) { // Show if low battery popup is hidden
                    lv_obj_remove_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                    app.PlaySound(Lang::Sounds::OGG_LOW_BATTERY);
                }
            } else {
                // Hide the low battery popup when the battery is not empty
                if (!lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN)) { // Hide if low battery popup is shown
                    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
    }

    // Update network icon every 10 seconds
    static int seconds_counter = 0;
    if (update_all || seconds_counter++ % 10 == 0) {
        // Don't read 4G network status during firmware upgrade to avoid occupying UART resources
        auto device_state = Application::GetInstance().GetDeviceState();
        static const std::vector<DeviceState> allowed_states = {
            kDeviceStateIdle,
            kDeviceStateStarting,
            kDeviceStateWifiConfiguring,
            kDeviceStateListening,
            kDeviceStateActivating,
        };
        if (std::find(allowed_states.begin(), allowed_states.end(), device_state) != allowed_states.end()) {
            icon = board.GetNetworkStateIcon();
            if (network_label_ != nullptr && icon != nullptr && network_icon_ != icon) {
                DisplayLockGuard lock(this);
                network_icon_ = icon;
                lv_label_set_text(network_label_, network_icon_);
            }
        }
    }

    esp_pm_lock_release(pm_lock_);
}

void LvglDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
}

void LvglDisplay::SetPowerSaveMode(bool on) {
    if (on) {
        SetChatMessage("system", "");
        SetEmotion("sleepy");
    } else {
        SetChatMessage("system", "");
        SetEmotion("neutral");
    }
}

bool LvglDisplay::SnapshotToJpeg(std::string& jpeg_data, int quality) {
#if CONFIG_LV_USE_SNAPSHOT
    DisplayLockGuard lock(this);

    lv_obj_t* screen = lv_screen_active();
    lv_draw_buf_t* draw_buffer = lv_snapshot_take(screen, LV_COLOR_FORMAT_RGB565);
    if (draw_buffer == nullptr) {
        ESP_LOGE(TAG, "Failed to take snapshot, draw_buffer is nullptr");
        return false;
    }

    // swap bytes
    uint16_t* data = (uint16_t*)draw_buffer->data;
    size_t pixel_count = draw_buffer->data_size / 2;
    for (size_t i = 0; i < pixel_count; i++) {
        data[i] = __builtin_bswap16(data[i]);
    }

    // Clear output string and use callback version to avoid pre-allocating large memory blocks
    jpeg_data.clear();

    // Use callback-based JPEG encoder to further save memory
    bool ret = image_to_jpeg_cb((uint8_t*)draw_buffer->data, draw_buffer->data_size, draw_buffer->header.w, draw_buffer->header.h, V4L2_PIX_FMT_RGB565, quality,
        [](void *arg, size_t index, const void *data, size_t len) -> size_t {
        std::string* output = static_cast<std::string*>(arg);
        if (data && len > 0) {
            output->append(static_cast<const char*>(data), len);
        }
        return len;
    }, &jpeg_data);
    if (!ret) {
        ESP_LOGE(TAG, "Failed to convert image to JPEG");
    }

    lv_draw_buf_destroy(draw_buffer);
    return ret;
#else
    ESP_LOGE(TAG, "LV_USE_SNAPSHOT is not enabled");
    return false;
#endif
}

void LvglDisplay::ShowCountdown(int duration_sec) {
    DisplayLockGuard lock(this);

    // Create container if not exists
    if (!countdown_container_) {
        // Full screen transparent container
        countdown_container_ = lv_obj_create(lv_screen_active());
        lv_obj_set_size(countdown_container_, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_style_bg_color(countdown_container_, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(countdown_container_, LV_OPA_90, 0); // 90% opacity overlay
        lv_obj_set_style_border_width(countdown_container_, 0, 0);
        lv_obj_set_scrollbar_mode(countdown_container_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_center(countdown_container_);

        // Load custom font if needed
        if (!countdown_time_font_) {
            // Try to load from binary first (as per user request reference)
            // But user said "main\display\lvgl_display\font_custom_80_4.bin"
            // And lcd_display.cc does: extern const uint8_t font_custom_80_4_bin_start[] asm("_binary_font_custom_80_4_bin_start");
            
            // We'll use cbin_font_create similar to LcdDisplay (via LvglFont)
            // cbin_font_create returns lv_font_t* directly.
            countdown_time_font_ = cbin_font_create((uint8_t*)font_custom_80_4_bin_start);
            if (!countdown_time_font_) {
                // Fallback to internal large font if fails (should not happen if compiled in)
                ESP_LOGE(TAG, "Failed to load custom 60 font, fallback");
                countdown_time_font_ = (lv_font_t*)LV_FONT_DEFAULT; 
            }
        }

        // Create Arc (Ring)
        countdown_arc_ = lv_arc_create(countdown_container_);
        lv_obj_set_size(countdown_arc_, LV_HOR_RES * 0.8, LV_HOR_RES * 0.8);
        lv_arc_set_rotation(countdown_arc_, 270);
        lv_arc_set_bg_angles(countdown_arc_, 0, 360);
        lv_arc_set_range(countdown_arc_, 0, duration_sec);
        lv_arc_set_value(countdown_arc_, duration_sec);
        lv_obj_center(countdown_arc_);
        
        // Remove knob
        lv_obj_remove_style(countdown_arc_, NULL, LV_PART_KNOB);
        lv_obj_clear_flag(countdown_arc_, LV_OBJ_FLAG_CLICKABLE);

        // Styling
        lv_obj_set_style_arc_color(countdown_arc_, lv_palette_main(LV_PALETTE_GREY), LV_PART_MAIN);
        lv_obj_set_style_arc_width(countdown_arc_, 15, LV_PART_MAIN);
        lv_obj_set_style_arc_color(countdown_arc_, lv_palette_main(LV_PALETTE_BLUE), LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(countdown_arc_, 15, LV_PART_INDICATOR);

        // Create Label
        countdown_label_ = lv_label_create(countdown_container_);
        lv_obj_set_style_text_font(countdown_label_, countdown_time_font_, 0);
        lv_obj_set_style_text_color(countdown_label_, lv_color_white(), 0);
        lv_label_set_text_fmt(countdown_label_, "%d", duration_sec); // Just showing seconds or duration? User said "countdown remaining time"
        lv_obj_center(countdown_label_);
    } else {
        lv_obj_clear_flag(countdown_container_, LV_OBJ_FLAG_HIDDEN);
        lv_arc_set_range(countdown_arc_, 0, duration_sec);
        lv_arc_set_value(countdown_arc_, duration_sec);
    }
    
    // Set initial text
    int minutes = duration_sec / 60;
    int seconds = duration_sec % 60;
    if (minutes > 0) {
       lv_label_set_text_fmt(countdown_label_, "%02d:%02d", minutes, seconds);
    } else {
       lv_label_set_text_fmt(countdown_label_, "%d", seconds);
    }
}

void LvglDisplay::UpdateCountdown(int remaining_sec) {
    DisplayLockGuard lock(this);
    if (!countdown_container_ || !countdown_arc_ || !countdown_label_) return;

    lv_arc_set_value(countdown_arc_, remaining_sec);

    int minutes = remaining_sec / 60;
    int seconds = remaining_sec % 60;
    if (minutes > 0) {
       lv_label_set_text_fmt(countdown_label_, "%02d:%02d", minutes, seconds);
    } else {
       lv_label_set_text_fmt(countdown_label_, "%d", seconds);
    }
}

void LvglDisplay::HideCountdown() {
    DisplayLockGuard lock(this);
    if (countdown_container_) {
        // Just hide it, or delete it? Let's delete to save RAM if it's intermittent
        // Or keep it simple and hide. The prompt implies it's a transient state.
        // Let's hide it for now, can reuse.
        lv_obj_add_flag(countdown_container_, LV_OBJ_FLAG_HIDDEN);
    }
}
