# Framework Font Loading Fix Implementation Plan

**Goal:** Load the complete Chinese font packaged by the GitHub Action and use it consistently across Framework screens.

**Architecture:** Reuse the existing `Assets::Apply()` path used by the original XiaoZhi application. Framework startup will apply the asset font after the board display is initialized; screen modules will resolve their text font from `LvglThemeManager`, with the compiled font as fallback.

**Tech Stack:** ESP-IDF, LVGL, xiaozhi-fonts CBIN assets, C++.

---

### Task 1: Apply packaged assets during Framework startup

Modify `main/framework/framework_main.cc` to call `Assets::GetInstance().Apply()` after board startup and before screen construction. Log a warning and continue with the built-in fallback if the assets partition is absent or invalid.

### Task 2: Use one resolved text font in Framework screens

Add a small font resolver under `main/framework/ui/` that returns the active light-theme text font and falls back to `BUILTIN_TEXT_FONT`. Update Home, Screensaver, Settings, and AI Chat labels to use it.

### Task 3: Verify the generated build inputs

Confirm the Action build still selects `font_puhui_basic_20_4`, packages `font_puhui_common_20_4.bin`, and that the source tree contains no Unicode-escaped Chinese text. Run static checks available locally; final acceptance requires the GitHub Action artifact and device flash.
