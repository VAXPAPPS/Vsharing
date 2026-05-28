/*
 * screen_viewer.h — عارض شاشة الهاتف في واجهة GTK4
 *
 * يستقبل إطارات H.264 عبر UDP من الهاتف،
 * يفك تشفيرها عبر FFmpeg libavcodec،
 * ويعرضها في GtkDrawingArea.
 */

#pragma once

#include <gtk/gtk.h>
#include <stdint.h>

/* ============================================================
 * الواجهة العامة
 * ============================================================ */

/**
 * screen_viewer_new:
 * يُنشئ GtkWidget جديداً (GtkDrawingArea) لعرض شاشة الهاتف.
 */
GtkWidget *screen_viewer_new(void);

/**
 * screen_viewer_start:
 * @udp_port: المنفذ المحلي للاستماع (افتراضياً 5002)
 * يبدأ thread الاستقبال وفك التشفير.
 * يُعيد TRUE عند النجاح.
 */
gboolean screen_viewer_start(int udp_port);

/**
 * screen_viewer_stop:
 * يوقف thread الاستقبال ويُحرر الموارد.
 */
void screen_viewer_stop(void);

/**
 * screen_viewer_is_active:
 * يُعيد TRUE إذا كان المُستعرِض نشطاً.
 */
gboolean screen_viewer_is_active(void);

/**
 * screen_viewer_request_keyframe:
 * يُشعر الهاتف بطلب I-frame (عبر callback يُسجَّل خارجياً).
 */
void screen_viewer_request_keyframe(void);

/**
 * screen_viewer_set_keyframe_callback:
 * @cb: دالة تُستدعى عند الحاجة لإطار مرجعي
 */
typedef void (*KeyframeRequestCb)(void);
void screen_viewer_set_keyframe_callback(KeyframeRequestCb cb);
