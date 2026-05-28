/*
 * vfs_bridge.h — جسر نظام الملفات الافتراضي (FUSE3)
 *
 * يُتيح تصفّح ملفات الهاتف من سطح مكتب Vaxp كما لو كانت
 * ملفات محلية تحت: ~/.vsharing/devices/<device_name>/
 *
 * المعمارية:
 * ┌─────────────────────────────────────────────────────────┐
 * │              Linux Kernel (FUSE Module)                  │
 * │         fuse_operations callbacks (thread-pool)          │
 * ├─────────────────────────────────────────────────────────┤
 * │                  vfs_bridge.c                            │
 * │  ┌─────────────┐  ┌──────────────┐  ┌────────────────┐ │
 * │  │ FUSE ops    │  │ Pending Reqs │  │ Attr/Dir Cache │ │
 * │  │ (getattr,   │  │ Hash Table   │  │ (LRU, TTL)     │ │
 * │  │  readdir,   │  │ + GMutex     │  │                │ │
 * │  │  read, ...) │  │ + GCond      │  │                │ │
 * │  └──────┬──────┘  └──────┬───────┘  └────────────────┘ │
 * │         │               │                               │
 * │  VLink RPC (req_id correlation)                         │
 * ├─────────────────────────────────────────────────────────┤
 * │             vlink_server.c (WebSocket)                   │
 * ├─────────────────────────────────────────────────────────┤
 * │                  Phone App                               │
 * └─────────────────────────────────────────────────────────┘
 *
 * آلية الطلبات:
 *  1. kernel يستدعي vfs_getattr("/Photos/img.jpg")
 *  2. vfs_bridge ينشئ طلباً: req_id=42, op=VFS_OP_STAT, path
 *  3. يُرسل عبر WebSocket (vlink_server_broadcast_frame)
 *  4. ينتظر على GCond مع timeout=8s
 *  5. vlink_server يستقبل الرد ويُنبّه vfs_bridge
 *  6. vfs_bridge يُحلل الرد ويُعيده لـ FUSE
 *
 * الـ Cache:
 *  - attr cache : TTL=2s لكل stat لمنع طلبات متكررة
 *  - dir cache  : TTL=5s لمحتوى المجلدات
 *  - LRU eviction عند تجاوز VFS_CACHE_MAX_ENTRIES
 *
 * الـ Threading:
 *  - FUSE يعمل في thread منفصل (مع thread-pool داخلي)
 *  - GLib main loop يُشغّل vlink_server callbacks
 *  - الـ pending table محمي بـ GMutex
 *  - كل طلب له GMutex/GCond خاص لانتظاره
 *
 * التبعيات: fuse3, glib-2.0
 */

#pragma once

#include "vlink_protocol.h"
#include <glib.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

/* ============================================================
 * الثوابت
 * ============================================================ */
#define VFS_MOUNT_BASE_DIR       "/.vsharing/devices"   /* نسبة لـ $HOME */
#define VFS_MOUNT_OPTIONS        "default_permissions,allow_other,nonempty"
#define VFS_FUSE_MAX_READAHEAD   (128 * 1024)           /* 128 KB */
#define VFS_MAX_CONCURRENT_OPS   64                      /* حد الطلبات المتزامنة */

/* ============================================================
 * نموذج حالة جهاز مُوصَّل
 * ============================================================ */
typedef struct _VfsMountedDevice VfsMountedDevice;

struct _VfsMountedDevice {
    char          device_id[37];       /* UUID string لتحديد الجهاز */
    char          device_name[64];     /* اسم قابل للقراءة */
    char          mount_path[PATH_MAX];/* المسار الكامل لنقطة التركيب */

    /* إحصائيات */
    uint64_t      requests_sent;
    uint64_t      requests_ok;
    uint64_t      requests_failed;
    uint64_t      requests_timeout;
    uint64_t      cache_hits;
    uint64_t      cache_misses;
    uint64_t      bytes_read;
    uint64_t      bytes_written;
    int64_t       mounted_at;          /* timestamp بالميلي ثانية */

    /* حالة FUSE */
    gpointer      fuse_handle;         /* struct fuse* */
    GThread      *fuse_thread;         /* thread يُشغّل fuse_loop_mt */
    volatile gboolean active;
};

/* ============================================================
 * واجهة إرسال إطارات VFS (يُسجَّل خارجياً من vlink_server)
 * ============================================================ */
typedef gboolean (*VfsSendFrameCb)(GBytes *frame, gpointer user_data);

/* ============================================================
 * الواجهة العامة — دورة حياة الـ Bridge
 * ============================================================ */

/**
 * vfs_bridge_init:
 * @send_cb:    دالة الإرسال عبر WebSocket (من vlink_server)
 * @user_data:  بيانات مُستخدِم تُمرَّر للـ callback
 *
 * يهيّئ نظام VFS Bridge: جدول الطلبات المعلقة، الـ cache،
 * ومجلد الجذر ~/.vsharing/devices/.
 * يجب استدعاؤها مرة واحدة عند بدء الـ daemon.
 */
gboolean vfs_bridge_init(VfsSendFrameCb send_cb, gpointer user_data);

/**
 * vfs_bridge_mount_device:
 * @device_id:   UUID الجهاز
 * @device_name: اسم الجهاز (يُستخدم كاسم مجلد)
 *
 * ينشئ مجلد التركيب ويبدأ FUSE mount في thread منفصل.
 * يُعيد المسار الكامل لنقطة التركيب أو NULL عند الفشل.
 * المستدعي يملك النتيجة ويحررها بـ g_free().
 */
char *vfs_bridge_mount_device(const char *device_id, const char *device_name);

/**
 * vfs_bridge_unmount_device:
 * @device_id: UUID الجهاز المراد فصله
 *
 * يُلغي التركيب بشكل نظيف، يُنبّه جميع الطلبات المعلقة
 * بخطأ ENODEV، ثم يُوقف thread الـ FUSE.
 */
void vfs_bridge_unmount_device(const char *device_id);

/**
 * vfs_bridge_unmount_all:
 * يُلغي جميع عمليات التركيب النشطة.
 * يُستدعى عند إيقاف الـ daemon.
 */
void vfs_bridge_unmount_all(void);

/**
 * vfs_bridge_cleanup:
 * يُحرر جميع موارد VFS Bridge.
 * يجب استدعاؤها بعد vfs_bridge_unmount_all().
 */
void vfs_bridge_cleanup(void);

/* ============================================================
 * الواجهة العامة — معالجة الاستجابات من الهاتف
 * ============================================================ */

/**
 * vfs_bridge_on_response:
 * @msg_type:  نوع رسالة VLink (VLINK_FILE_LIST_RESP أو VLINK_FILE_READ_RESP)
 * @payload:   بيانات الاستجابة الخام
 * @len:       حجم البيانات بالبايت
 *
 * تُستدعى من vlink_server عند استقبال رد VFS من الهاتف.
 * تُحلّل الرأس، تجد الطلب المعلق بـ req_id، وتُنبّه thread الانتظار.
 * آمنة للاستدعاء من أي thread.
 */
void vfs_bridge_on_response(uint8_t msg_type,
                             const uint8_t *payload, uint32_t len);

/* ============================================================
 * الواجهة العامة — الاستعلام والإحصاء
 * ============================================================ */

/**
 * vfs_bridge_get_mounted_devices:
 * يُعيد قائمة بأسماء الأجهزة المُركَّبة حالياً.
 * المستدعي يملك القائمة ويحررها بـ g_list_free_full(list, g_free).
 */
GList *vfs_bridge_get_mounted_devices(void);

/**
 * vfs_bridge_get_device_stats:
 * @device_id:  UUID الجهاز
 * @out_device: سيُعبَّأ بنسخة من حالة الجهاز
 * يُعيد TRUE عند النجاح.
 */
gboolean vfs_bridge_get_device_stats(const char *device_id,
                                      VfsMountedDevice *out_device);

/**
 * vfs_bridge_is_device_mounted:
 * يُعيد TRUE إذا كان الجهاز مُركَّباً حالياً.
 */
gboolean vfs_bridge_is_device_mounted(const char *device_id);

/**
 * vfs_bridge_invalidate_cache:
 * @device_id: UUID الجهاز (NULL = كل الأجهزة)
 * @path:      المسار المحدد (NULL = كل المسارات)
 *
 * يُلغي صلاحية cache الخصائص والمجلدات للمسار المحدد.
 * يُستدعى بعد عمليات الكتابة/الحذف لضمان اتساق البيانات.
 */
void vfs_bridge_invalidate_cache(const char *device_id, const char *path);
