#pragma once

#include <lvgl.h>

#include "display/display.h"
#include "display/lvgl_display/lvgl_theme.h"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);

// All Framework screens use the active theme font. Assets::Apply() replaces
// that font with the complete Chinese CBIN font packaged by the build.
inline const lv_font_t* ResolveFrameworkTextFont(Display* display) {
    auto* theme = display == nullptr ? nullptr : dynamic_cast<LvglTheme*>(display->GetTheme());
    if (theme != nullptr) {
        auto font = theme->text_font();
        if (font != nullptr && font->font() != nullptr) {
            return font->font();
        }
    }
    return &BUILTIN_TEXT_FONT;
}
