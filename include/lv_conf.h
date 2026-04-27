// lv_conf.h for LVGL 8.3.x on ESP32-S3 / Waveshare 2.1" round.
//
// Based on LVGL's lv_conf_template.h, trimmed and adjusted for this project.
// Every option LVGL's lv_conf_internal.h looks for is explicitly defined
// here, even when the value is the default — an explicit, complete config
// is much easier to reason about than "minimal plus unknown defaults", and
// we already got burned once by a too-terse file (lv_color_t was vanishing
// at compile time because the header chain hit an #elif it couldn't match).
//
// If upgrading LVGL, regenerate this from the new release's lv_conf_template.h
// rather than patching in place.

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

// =============================================================================
// COLOR SETTINGS
// =============================================================================

#define LV_COLOR_DEPTH         16       // 16-bit RGB565 — what the ST7701 RGB-parallel panel expects.
// Round 49: set to 0. The =1 here was a holdover from when this project
// thought it was driving an ST77916 QSPI panel (rounds 4-12). On the
// RGB-parallel ST7701 (round 13+) the IDF driver memcpy's the LVGL
// framebuffer to PSRAM and the RGB peripheral DMAs raw RGB565 to the
// data pins in native CPU byte order. With SWAP=1, LVGL packs each
// pixel as [G_high(3), R(5), B(5), G_low(3)] — which the IDF driver
// (correctly) reads back as standard RGB565 [R(5), G(6), B(5)]. The
// bit fields don't match → every non-palindromic colour is scrambled:
//   0xCC0000 (red)   → 0x00C8 → R=0  G=6  B=8   (dark teal, invisible on black)
//   0x006400 (green) → 0x2003 → R=4  G=0  B=3   (dark red — what the user saw at the green-sector spot)
//   0xFFFF00 (yellow)→ 0xE0FF → R=28 G=7  B=31  (magenta — the "pink TWA needle")
//   0x1A2740 (navy)  → 0x2819 → R=5  G=0  B=25  (blue-purple — close enough to navy that it looked OK)
// White/black are palindromes and rendered fine, which masked the bug
// for many rounds. With SWAP=0 the layout becomes the standard
// [R(5), G(6), B(5)] that the IDF driver expects.
#define LV_COLOR_16_SWAP       0
#define LV_COLOR_SCREEN_TRANSP 0
#define LV_COLOR_MIX_ROUND_OFS (LV_COLOR_DEPTH == 32 ? 0 : 128)
#define LV_COLOR_CHROMA_KEY    lv_color_hex(0x00ff00)

// =============================================================================
// MEMORY SETTINGS
// =============================================================================

#define LV_MEM_CUSTOM        0
#define LV_MEM_SIZE          (64U * 1024U)          // 64 KB LVGL heap.
#define LV_MEM_ADR           0                       // 0 = let LVGL allocate.
#define LV_MEM_AUTO_DEFRAG   1
#define LV_MEM_BUF_MAX_NUM   16
#define LV_MEMCPY_MEMSET_STD 0

// =============================================================================
// HAL SETTINGS
// =============================================================================

#define LV_DISP_DEF_REFR_PERIOD  30     // ms between display refreshes.
#define LV_INDEV_DEF_READ_PERIOD 30     // ms between input reads.
#define LV_TICK_CUSTOM           0      // We drive lv_tick_inc from our own task.
#define LV_DPI_DEF               130    // Physical ~130 DPI on the 2.1" round.

// =============================================================================
// FEATURE CONFIG — drawing
// =============================================================================

#define LV_DRAW_COMPLEX             1
#define LV_SHADOW_CACHE_SIZE        0
#define LV_CIRCLE_CACHE_SIZE        4
#define LV_LAYER_SIMPLE_BUF_SIZE    (24 * 1024)
#define LV_LAYER_SIMPLE_FALLBACK_BUF_SIZE (3 * 1024)
#define LV_IMG_CACHE_DEF_SIZE       0
#define LV_GRADIENT_MAX_STOPS       2
#define LV_GRAD_CACHE_DEF_SIZE      0
#define LV_DITHER_GRADIENT          0
#define LV_DISP_ROT_MAX_BUF         (10 * 1024)

// GPU backends — none on ESP32-S3.
#define LV_USE_GPU_ARM2D            0
#define LV_USE_GPU_STM32_DMA2D      0
#define LV_USE_GPU_RA6M3_G2D        0
#define LV_USE_GPU_SWM341_DMA2D     0
#define LV_USE_GPU_NXP_PXP          0
#define LV_USE_GPU_NXP_PXP_AUTO_INIT 0
#define LV_USE_GPU_NXP_VG_LITE      0
#define LV_USE_GPU_SDL              0

// =============================================================================
// LOGGING
// =============================================================================

#define LV_USE_LOG            0
#if LV_USE_LOG
    #define LV_LOG_LEVEL         LV_LOG_LEVEL_WARN
    #define LV_LOG_PRINTF        0
    #define LV_LOG_TRACE_MEM     1
    #define LV_LOG_TRACE_TIMER   1
    #define LV_LOG_TRACE_INDEV   1
    #define LV_LOG_TRACE_DISP_REFR 1
    #define LV_LOG_TRACE_EVENT   1
    #define LV_LOG_TRACE_OBJ_CREATE 1
    #define LV_LOG_TRACE_LAYOUT  1
    #define LV_LOG_TRACE_ANIM    1
#endif

// =============================================================================
// ASSERTS
// =============================================================================

#define LV_USE_ASSERT_NULL         1
#define LV_USE_ASSERT_MALLOC       1
#define LV_USE_ASSERT_STYLE        0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ          0
#define LV_ASSERT_HANDLER_INCLUDE  <stdint.h>
#define LV_ASSERT_HANDLER          while(1);

// =============================================================================
// OTHER FEATURES
// =============================================================================

#define LV_USE_PERF_MONITOR    0
#define LV_USE_MEM_MONITOR     0
#define LV_USE_REFR_DEBUG      0
#define LV_SPRINTF_CUSTOM      0
#define LV_SPRINTF_USE_FLOAT   0
#define LV_USE_USER_DATA       1
#define LV_ENABLE_GC           0

// =============================================================================
// COMPILER SETTINGS
// =============================================================================

#define LV_BIG_ENDIAN_SYSTEM      0
#define LV_ATTRIBUTE_TICK_INC
#define LV_ATTRIBUTE_TIMER_HANDLER
#define LV_ATTRIBUTE_FLUSH_READY
#define LV_ATTRIBUTE_MEM_ALIGN_SIZE 1
#define LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_LARGE_CONST
#define LV_ATTRIBUTE_LARGE_RAM_ARRAY
#define LV_ATTRIBUTE_FAST_MEM
#define LV_ATTRIBUTE_DMA
#define LV_EXPORT_CONST_INT(int_value) struct _silence_gcc_warning

// =============================================================================
// FONTS
// =============================================================================

#define LV_FONT_MONTSERRAT_8  0
#define LV_FONT_MONTSERRAT_10 0
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 0
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 0
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_26 0
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_30 0
#define LV_FONT_MONTSERRAT_32 0
#define LV_FONT_MONTSERRAT_34 0
#define LV_FONT_MONTSERRAT_36 0
#define LV_FONT_MONTSERRAT_38 0
#define LV_FONT_MONTSERRAT_40 0
#define LV_FONT_MONTSERRAT_42 0
#define LV_FONT_MONTSERRAT_44 0
#define LV_FONT_MONTSERRAT_46 0
#define LV_FONT_MONTSERRAT_48 1

#define LV_FONT_MONTSERRAT_12_SUBPX    0
#define LV_FONT_MONTSERRAT_28_COMPRESSED 0
#define LV_FONT_DEJAVU_16_PERSIAN_HEBREW 0
#define LV_FONT_SIMSUN_16_CJK          0

#define LV_FONT_UNSCII_8   0
#define LV_FONT_UNSCII_16  0

#define LV_FONT_CUSTOM_DECLARE
#define LV_FONT_DEFAULT    &lv_font_montserrat_20
#define LV_FONT_FMT_TXT_LARGE      0
#define LV_USE_FONT_COMPRESSED     0
#define LV_USE_FONT_SUBPX          0

// =============================================================================
// TEXT / BIDI
// =============================================================================

#define LV_TXT_ENC         LV_TXT_ENC_UTF8
#define LV_TXT_BREAK_CHARS " ,.;:-_"
#define LV_TXT_LINE_BREAK_LONG_LEN 0
#define LV_TXT_LINE_BREAK_LONG_PRE_MIN_LEN 3
#define LV_TXT_LINE_BREAK_LONG_POST_MIN_LEN 3
#define LV_TXT_COLOR_CMD  "#"
#define LV_USE_BIDI       0
#define LV_USE_ARABIC_PERSIAN_CHARS 0

// =============================================================================
// WIDGETS
// =============================================================================

#define LV_USE_ARC        1
#define LV_USE_BAR        1
#define LV_USE_BTN        1
#define LV_USE_BTNMATRIX  1
#define LV_USE_CANVAS     1
#define LV_USE_CHECKBOX   1
#define LV_USE_DROPDOWN   1
#define LV_USE_IMG        1
#define LV_USE_LABEL      1
#if LV_USE_LABEL
    #define LV_LABEL_TEXT_SELECTION 1
    #define LV_LABEL_LONG_TXT_HINT  1
#endif
#define LV_USE_LINE       1
#define LV_USE_ROLLER     1
#if LV_USE_ROLLER
    #define LV_ROLLER_INF_PAGES 7
#endif
#define LV_USE_SLIDER     1
#define LV_USE_SWITCH     1
#define LV_USE_TEXTAREA   1
#if LV_USE_TEXTAREA
    #define LV_TEXTAREA_DEF_PWD_SHOW_TIME 1500
#endif
#define LV_USE_TABLE      1

// --- Extra widgets ---
#define LV_USE_ANIMIMG    1
#define LV_USE_CALENDAR   1
#if LV_USE_CALENDAR
    #define LV_CALENDAR_WEEK_STARTS_MONDAY 0
    #if LV_CALENDAR_WEEK_STARTS_MONDAY
        #define LV_CALENDAR_DEFAULT_DAY_NAMES {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"}
    #else
        #define LV_CALENDAR_DEFAULT_DAY_NAMES {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"}
    #endif
    #define LV_CALENDAR_DEFAULT_MONTH_NAMES {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"}
    #define LV_USE_CALENDAR_HEADER_ARROW 1
    #define LV_USE_CALENDAR_HEADER_DROPDOWN 1
#endif
#define LV_USE_CHART      1
#define LV_USE_COLORWHEEL 1
#define LV_USE_IMGBTN     1
#define LV_USE_KEYBOARD   1
#define LV_USE_LED        1
#define LV_USE_LIST       1
#define LV_USE_MENU       1
#define LV_USE_METER      1
#define LV_USE_MSGBOX     1
#define LV_USE_SPAN       1
#if LV_USE_SPAN
    #define LV_SPAN_SNIPPET_STACK_SIZE 64
#endif
#define LV_USE_SPINBOX    1
#define LV_USE_SPINNER    1
#define LV_USE_TABVIEW    1
#define LV_USE_TILEVIEW   1
#define LV_USE_WIN        1

// =============================================================================
// THEMES
// =============================================================================

#define LV_USE_THEME_DEFAULT 1
#if LV_USE_THEME_DEFAULT
    #define LV_THEME_DEFAULT_DARK 1
    #define LV_THEME_DEFAULT_GROW 1
    #define LV_THEME_DEFAULT_TRANSITION_TIME 80
#endif
#define LV_USE_THEME_BASIC 1
#define LV_USE_THEME_MONO  1

// =============================================================================
// LAYOUTS
// =============================================================================

#define LV_USE_FLEX 1
#define LV_USE_GRID 1

// =============================================================================
// FILE / OTHER (unused on this board)
// =============================================================================

#define LV_USE_FS_STDIO  0
#define LV_USE_FS_POSIX  0
#define LV_USE_FS_WIN32  0
#define LV_USE_FS_FATFS  0
#define LV_USE_PNG       0
#define LV_USE_BMP       0
#define LV_USE_SJPG      0
#define LV_USE_GIF       0
#define LV_USE_QRCODE    0
#define LV_USE_FREETYPE  0
#define LV_USE_RLOTTIE   0
#define LV_USE_FFMPEG    0

#define LV_USE_SNAPSHOT  0
#define LV_USE_MONKEY    0
#define LV_USE_GRIDNAV   0
#define LV_USE_FRAGMENT  0
#define LV_USE_IMGFONT   0
#define LV_USE_MSG       0
#define LV_USE_IME_PINYIN 0

// =============================================================================
// EXAMPLES / DEMOS — off
// =============================================================================

#define LV_BUILD_EXAMPLES 0

#endif // LV_CONF_H
