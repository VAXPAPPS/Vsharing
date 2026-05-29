/*
 * auth_manager.h — إدارة مصادقة أجهزة VLink
 *
 * يدير الأجهزة الموثوقة، جلسات الاتصال، وتخزين المفاتيح.
 * يحمي ضد الاتصالات غير المصرح بها بنظام QR + challenge.
 */

#pragma once

#include <glib.h>
#include <stdint.h>
#include "vlink_protocol.h"

/* ============================================================
 * هيكل الجهاز الموثوق
 * ============================================================ */
typedef struct {
    char     device_id[37];        /* UUID string بصيغة xxxxxxxx-xxxx-... */
    char     device_name[64];
    uint8_t  device_type;          /* 0=Android, 1=iOS, 2=Linux */
    uint8_t  public_key[32];       /* X25519 Public Key */
    uint32_t permissions;          /* VLinkCapability bitmask */
    int64_t  first_seen;           /* Unix timestamp */
    int64_t  last_seen;
    char     last_ip[46];
} VLinkTrustedDevice;

/* ============================================================
 * سياق مدير المصادقة
 * ============================================================ */
typedef struct {
    /* قاموس الأجهزة الموثوقة: device_id → VLinkTrustedDevice* */
    GHashTable  *trusted_devices;

    /* مسار ملف التخزين */
    char        *config_path;      /* ~/.config/vsharing/trusted_devices.json */

    /* المفتاح الخاص للخادم */
    uint8_t      server_private_key[32];
    uint8_t      server_public_key[32];
    char         server_id[37];    /* UUID الخادم */

    /* تحديات معلّقة: device_id → challenge bytes */
    GHashTable  *pending_challenges;

    /* mutex للوصول المتزامن */
    GMutex       mutex;
} VLinkAuthManager;

/* ============================================================
 * دوال مدير المصادقة
 * ============================================================ */

/**
 * vlink_auth_manager_new:
 * يُنشئ مدير مصادقة جديد ويحمّل الأجهزة المخزنة.
 */
VLinkAuthManager *vlink_auth_manager_new(void);

/**
 * vlink_auth_manager_free:
 * يُحرر جميع موارد مدير المصادقة.
 */
void vlink_auth_manager_free(VLinkAuthManager *mgr);

/**
 * vlink_auth_is_trusted:
 * يتحقق إذا كان جهاز بـ device_id موثوقاً.
 */
gboolean vlink_auth_is_trusted(VLinkAuthManager *mgr, const char *device_id);

/**
 * vlink_auth_get_device:
 * يُعيد بيانات جهاز موثوق أو NULL.
 */
VLinkTrustedDevice *vlink_auth_get_device(VLinkAuthManager *mgr,
                                            const char *device_id);

/**
 * vlink_auth_start_challenge:
 * يُنشئ تحدي عشوائي لجهاز يطلب المصادقة.
 * يُعيد إطار VLinkChallenge جاهزاً للإرسال.
 */
GBytes *vlink_auth_start_challenge(VLinkAuthManager *mgr,
                                    const VLinkAuthPayload *req,
                                    uint32_t seq);

/**
 * vlink_auth_verify_response:
 * يتحقق من رد الجهاز على التحدي.
 * عند النجاح: يضيف الجهاز للقائمة الموثوقة ويحفظ.
 * @returns: device_id أو NULL عند الفشل.
 */
const char *vlink_auth_verify_response(VLinkAuthManager *mgr,
                                        const char *device_id,
                                        const VLinkAuthRespPayload *resp);

/**
 * vlink_auth_revoke_device:
 * يُلغي ثقة جهاز ويحذفه من القائمة.
 */
gboolean vlink_auth_revoke_device(VLinkAuthManager *mgr, const char *device_id);

/**
 * vlink_auth_save:
 * يحفظ قائمة الأجهزة الموثوقة على القرص.
 */
gboolean vlink_auth_save(VLinkAuthManager *mgr);

/**
 * vlink_auth_get_all_devices:
 * يُعيد قائمة بجميع الأجهزة الموثوقة (يجب تحرير القائمة بـ g_list_free).
 */
GList *vlink_auth_get_all_devices(VLinkAuthManager *mgr);

/**
 * vlink_auth_get_server_public_key_b64:
 * يُعيد المفتاح العام للخادم بصيغة Base64 (للعرض في QR).
 * يجب تحرير النص المُعاد بـ g_free.
 */
gchar *vlink_auth_get_server_public_key_b64(VLinkAuthManager *mgr);

/**
 * vlink_auth_ensure_tls_cert:
 * يتأكد من وجود شهادة TLS ذاتية التوقيع في مسار إعدادات Vsharing،
 * أو يُنشئها باستخدام openssl إذا لم تكن موجودة.
 * يُعيد مسارات الشهادة والمفتاح الخاص في out_cert_path و out_key_path.
 * يجب تحرير المسارات باستخدام g_free.
 */
gboolean vlink_auth_ensure_tls_cert(char **out_cert_path, char **out_key_path);

