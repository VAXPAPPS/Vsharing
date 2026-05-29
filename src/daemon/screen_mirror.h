/*
 * screen_mirror.h — التقاط شاشة الحاسوب وبثها للهاتف
 *
 * يلتقط الشاشة عبر X11/XShm أو Wayland (pipewire)،
 * يُشفّر الإطارات بـ H.264 عبر FFmpeg libavcodec،
 * ويُرسلها للهاتف عبر UDP على المنفذ 5001.
 *
 * التبعيات: libavcodec, libavutil, libavformat, libswscale, x11
 */

#pragma once

#include <glib.h>
#include <stdint.h>

/* ============================================================
 * إعدادات البث الافتراضية
 * ============================================================ */
#define SCREEN_DEFAULT_FPS      15
#define SCREEN_DEFAULT_BITRATE  (1500 * 1000)  /* 1.5 Mbps */
#define SCREEN_DEFAULT_WIDTH    1280
#define SCREEN_DEFAULT_HEIGHT   720
#define SCREEN_UDP_PORT         5001

/* ============================================================
 * الواجهة العامة
 * ============================================================ */

/**
 * screen_mirror_start:
 * @client_ip:   عنوان IP الهاتف المُستقبِل
 * @fps:         معدل الإطارات (10, 15, 30)
 * @bitrate:     معدل البت بالـ bits/s
 * @crypto_key:  مفتاح التشفير (32 بايت) أو NULL للتعطيل (المرحلة 5)
 * يبدأ thread منفصلاً يلتقط الشاشة ويُشفّرها ويُرسلها.
 * يُعيد TRUE عند النجاح.
 */
gboolean screen_mirror_start(const char *client_ip, int fps, int bitrate, const uint8_t *crypto_key);

/**
 * screen_mirror_stop:
 * يوقف البث بشكل نظيف وينتظر انتهاء الـ thread.
 */
void screen_mirror_stop(void);

/**
 * screen_mirror_is_running:
 * يُعيد TRUE إذا كان البث نشطاً.
 */
gboolean screen_mirror_is_running(void);

/**
 * screen_mirror_get_stats:
 * يُعبئ إحصائيات البث (إطارات/ثانية، bytes مُرسَلة).
 */
void screen_mirror_get_stats(uint32_t *frames_sent, uint64_t *bytes_sent);

/**
 * screen_mirror_request_keyframe:
 * يطلب إعادة إرسال إطار مرجعي كامل (I-frame) في الدورة القادمة.
 */
void screen_mirror_request_keyframe(void);
