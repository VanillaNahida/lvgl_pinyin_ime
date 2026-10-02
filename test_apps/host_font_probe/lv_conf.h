/**
 * LVGL configuration for test_apps/host_font_probe.
 *
 * Mirrors the firmware's relevant choices (16 bit colour, snapshot enabled,
 * small built-in fonts) so a host render is representative.
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16

/* Optional widgets the IME uses. */
#define LV_USE_LABEL 1
#define LV_USE_BUTTON 1
#define LV_USE_TEXTAREA 1
#define LV_USE_OBJ 1

/* Layouts: the IME builds a flex column. */
#define LV_USE_FLEX 1
#define LV_USE_GRID 1

/* Rendering to a PNG-ish raw dump for inspection. */
#define LV_USE_SNAPSHOT 1

/* No filesystem: the font and dictionary sources are stubbed on the host. */
#define LV_USE_FS_STDIO 0
#define LV_USE_FS_POSIX 0
#define LV_USE_FS_WIN32 0
#define LV_USE_FS_MEMFS 0

/* The IME supplies its own fonts; only the small montserrat is needed for the
 * stub text. */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

#define LV_MEM_SIZE (4U * 1024U * 1024U)

#define LV_USE_OS LV_OS_NONE

#endif /* LV_CONF_H */

