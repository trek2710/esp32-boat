// Minimal lv_conf.h for LVGL 8.3.
//
// Start from LVGL's lv_conf_template.h if you want to customise — this is
// deliberately a small subset: enough to compile our five instrument pages,
// nothing more. Override via build_flags in platformio.ini.

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

// -------- Color depth & swap ------------------------------------------------
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 1

// -------- Memory -----------------------------------------------------------
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE   (48U * 1024U)

// -------- Tick -------------------------------------------------------------
#define LV_TICK_CUSTOM 0

// -------- HAL --------------------------------------------------------------
#define LV_DISP_DEF_REFR_PERIOD 30

// -------- Features --------------------------------------------------------
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR  0
#define LV_USE_LOG          0
#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0

// -------- Widgets we actually use -----------------------------------------
#define LV_USE_LABEL  1
#define LV_USE_IMG    1
#define LV_USE_LINE   1
#define LV_USE_ARC    1
#define LV_USE_BAR    1
#define LV_USE_BTN    1
// btnmatrix and textarea are required by lv_extra.h (via lv_keyboard.h /
// lv_msgbox.h / lv_spinbox.h). Keeping them enabled is cheap and avoids the
// "lv_kb: lv_btnm is required" compile errors.
#define LV_USE_BTNMATRIX 1
#define LV_USE_TEXTAREA  1
#define LV_USE_CANVAS   0
#define LV_USE_CHECKBOX 0
#define LV_USE_DROPDOWN 0
#define LV_USE_LIST     1
#define LV_USE_SLIDER   0
#define LV_USE_SWITCH   0
#define LV_USE_TABLE    0

// -------- Themes ----------------------------------------------------------
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1

// -------- Fonts -----------------------------------------------------------
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT       &lv_font_montserrat_20

#endif // LV_CONF_H
