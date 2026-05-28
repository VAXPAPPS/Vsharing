/*
 * vlink_server.h — خادم VLink v2
 *
 * يُدير اتصالات WebSocket مع الأجهزة، يوزع الرسائل،
 * ويوفر نظام بث للحافظة والإشعارات والملفات.
 */

#pragma once

#include <glib.h>
#include "vlink_protocol.h"
#include "auth_manager.h"
#include "clipboard_engine.h"

/* ============================================================
 * سياق خادم VLink الرئيسي
 * ============================================================ */
typedef struct {
    /* إدارة الاتصالات */
    gpointer           soup_server;       /* SoupServer* */
    GPtrArray         *devices;           /* VLinkDevice* - الأجهزة المتصلة */
    GMutex             devices_mutex;

    /* الوحدات الفرعية */
    VLinkAuthManager  *auth;
    ClipboardEngine   *clipboard;

    /* الحالة */
    gboolean           running;
    int                ctrl_port;

    /* إحصائيات */
    uint64_t           total_bytes_sent;
    uint64_t           total_bytes_recv;
    uint32_t           total_connections;
} VLinkServer;

/* Callback للأحداث الكبيرة */
typedef void (*VLinkServerEventCallback)(VLinkServer *server,
                                          VLinkDevice *device,
                                          const char  *event_type,
                                          gpointer     user_data);

/* ============================================================
 * دوال الخادم
 * ============================================================ */

/**
 * vlink_server_new:
 * يُنشئ خادم VLink جديد.
 */
VLinkServer *vlink_server_new(int ctrl_port);

/**
 * vlink_server_start:
 * يبدأ الاستماع على المنافذ.
 */
gboolean vlink_server_start(VLinkServer *server);

/**
 * vlink_server_stop:
 * يوقف الخادم بشكل نظيف.
 */
void vlink_server_stop(VLinkServer *server);

/**
 * vlink_server_free:
 * يحرر جميع الموارد.
 */
void vlink_server_free(VLinkServer *server);

/**
 * vlink_server_broadcast:
 * يرسل رسالة لجميع الأجهزة المصادق عليها.
 */
void vlink_server_broadcast(VLinkServer *server, GBytes *frame);

/**
 * vlink_server_send_to_device:
 * يرسل رسالة لجهاز محدد.
 */
gboolean vlink_server_send_to_device(VLinkServer *server,
                                      const char  *device_id,
                                      GBytes      *frame);

/**
 * vlink_server_get_device_count:
 * يُعيد عدد الأجهزة المتصلة والمصادق عليها.
 */
guint vlink_server_get_device_count(VLinkServer *server);

/**
 * vlink_server_get_qr_url:
 * يُعيد رابط QR للاتصال بالخادم.
 * يجب تحرير النص المُعاد بـ g_free.
 */
gchar *vlink_server_get_qr_url(VLinkServer *server);
