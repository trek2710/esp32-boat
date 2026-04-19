// Minimal lv_conf.h for LVGL 8.3.
//
// LVGL's "extra" widgets (calendar, keyboard, msgbox, spinbox, chart, ...)
// are compiled by default and each one depends on one or more base widgets
// (btnmatrix, textarea, dropdown, ...). Disabling a base widget produces
// cryptic errors from the extras. Simpler to enable the whole widget set —
// the Flash cost on our 16 MB part is negligible.

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

// -------- Color depth & swap ------------------------------------------------
#define LV_COLOR_DEPTH   16
#define LV_COLOR_16_SWAP 1

// -------- Memory ------------------------------------------------------------
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE   (48U * 1024U)

// -------- Tick / HAL --------------------------------------------------------
#define LV_TICK_CUSTOM            0
#define LV_DISP_DEF_REFR_PERIOD   30

// -------- Debug / monitoring ------------------------------------------------
#define LV_USE_PERF_MONITOR    0
#define LV_USE_MEM_MONITOR     0
#define LV_USE_LOG             0
#define LV_USE_ASSERT_NULL     1
#define LV_USE_ASSERT_MALLOC   1
#define LV_USE_ASSERT_STYLE    0

// -------- Base widgets ------------------------------------------------------
#define LV_USE_LABEL      1
#define LV_USE_IMG        1
#define LV_USE_LINE       1
#define LV_USE_ARC        1
#define LV_USE_BAR        1
#define LV_USE_BTN        1
#define LV_USE_BTNMATRIX  1
#define LV_USE_CANVAS     1
#define LV_USE_CHECKBOX   1
#define LV_USE_DROPDOWN   1
#define LV_USE_LIST       1
#define LV_USE_ROLLER     1
#define LV_USE_SLIDER     1
#define LV_USE_SWITCH     1
#define LV_USE_TABLE      1
#define LV_USE_TEXTAREA   1

// -------- Extra widgets (pull in base widgets above as deps) ---------------
#define LV_USE_CALENDAR   1
#define LV_USE_CHART      1
#define LV_USE_COLORWHEEL 1
#define LV_USE_IMGBTN     1
#define LV_USE_KEYBOARD   1
#define LV_USE_LED        1
#define LV_USE_MENU       1
#define LV_USE_METER      1
#define LV_USE_MSGBOX     1
#define LV_USE_SPAN       1
#define LV_USE_SPINBOX    1
#define LV_USE_SPINNER    1
#define LV_USE_TABVIEW    1
#define LV_USE_TILEVIEW   1
#define LV_USE_WIN        1

// -------- Themes ------------------------------------------------------------
#define LV_USE_THEME_DEFAULT  1
#define LV_THEME_DEFAULT_DARK 1

// -------- Fonts -------------------------------------------------------------
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT       &lv_font_montserrat_20

#endif // LV_CONF_H
