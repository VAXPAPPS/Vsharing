/*
 * clipboard_engine.h — محرك مزامنة الحافظة عالي الأداء
 *
 * يحل محل نظام الـ Polling البطيء بنظام مبني على Events.
 * يدعم: نص، صور، قوائم ملفات.
 * تأخير < 20ms بدلاً من 500ms.
 */

#pragma once

#include <glib.h>
#include <stdint.h>

/* أنواع محتوى الحافظة */
typedef enum {
    CLIP_TYPE_NONE   = 0,
    CLIP_TYPE_TEXT   = 1,
    CLIP_TYPE_IMAGE  = 2,
    CLIP_TYPE_FILES  = 3,
} ClipboardContentType;

/* محتوى الحافظة */
typedef struct {
    ClipboardContentType type;
    union {
        char     *text;          /* لـ CLIP_TYPE_TEXT */
        struct {                 /* لـ CLIP_TYPE_IMAGE */
            uint8_t *data;
            gsize    len;
            char     format[16]; /* "png", "jpeg", ... */
        } image;
        GList    *file_paths;    /* لـ CLIP_TYPE_FILES: قائمة char* */
    };
    int64_t timestamp_us;        /* وقت النسخ بالميكروثانية */
} ClipboardContent;

/* Callback عند تغير الحافظة */
typedef void (*ClipboardChangedCallback)(const ClipboardContent *content,
                                          gpointer user_data);

/* سياق محرك الحافظة */
typedef struct {
    ClipboardChangedCallback on_changed;
    gpointer                 user_data;

    /* آخر محتوى لمنع ping-pong */
    char    *last_text;
    int64_t  last_change_time;

    /* علامة مصدر التغيير لمنع الحلقة */
    gboolean is_setting_clipboard;
    gboolean enabled;

    /* مسارات أدوات الحافظة */
    gboolean has_wl_copy;
    gboolean has_xclip;
    gboolean has_xdotool;
} ClipboardEngine;

/* ============================================================
 * دوال محرك الحافظة
 * ============================================================ */

/**
 * clipboard_engine_new:
 * يُنشئ محرك حافظة جديد.
 * @on_changed: callback يُستدعى عند كل تغيير حقيقي
 */
ClipboardEngine *clipboard_engine_new(ClipboardChangedCallback on_changed,
                                       gpointer user_data);

/**
 * clipboard_engine_free:
 * يحرر جميع موارد المحرك.
 */
void clipboard_engine_free(ClipboardEngine *engine);

/**
 * clipboard_engine_set_enabled:
 * تفعيل/إيقاف المزامنة.
 */
void clipboard_engine_set_enabled(ClipboardEngine *engine, gboolean enabled);

/**
 * clipboard_engine_set_text:
 * يضع نصاً في الحافظة من خارج النظام (من الهاتف مثلاً).
 * يمنع تشغيل الـ callback لهذا التغيير.
 */
gboolean clipboard_engine_set_text(ClipboardEngine *engine, const char *text);

/**
 * clipboard_engine_get_text:
 * يقرأ النص الحالي في الحافظة.
 * يجب تحرير النص المُعاد بـ g_free.
 */
gchar *clipboard_engine_get_text(ClipboardEngine *engine);

/**
 * clipboard_engine_on_pc_clipboard_changed:
 * يجب استدعاؤه من on_clipboard_changed في الـ UI عند تغيير الحافظة.
 */
void clipboard_engine_on_pc_clipboard_changed(ClipboardEngine *engine,
                                               const char *new_text);

/**
 * clipboard_content_free:
 * يحرر محتوى الحافظة.
 */
void clipboard_content_free(ClipboardContent *content);
