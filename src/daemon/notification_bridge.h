/*
 * notification_bridge.h — جسر إشعارات الهاتف ← الحاسوب
 *
 * الوظيفة:
 *   - استقبال إشعارات الهاتف عبر VLink (VLINK_NOTIF_SHOW / DISMISS)
 *   - عرضها على سطح مكتب Vaxp عبر D-Bus (org.freedesktop.Notifications)
 *   - استقبال إجراءات المستخدم (رد، إلغاء) وإرسالها للهاتف عبر VLINK_NOTIF_ACTION
 *
 * الاستخدام:
 *   notif_bridge_init();           // في main()
 *   notif_bridge_show(&notif);     // عند استقبال VLINK_NOTIF_SHOW
 *   notif_bridge_dismiss(id);      // عند استقبال VLINK_NOTIF_DISMISS
 *   notif_bridge_cleanup();        // عند الإغلاق
 */

#pragma once

#include <gio/gio.h>
#include "vlink_protocol.h"

/* ============================================================
 * هيكل بيانات الإشعار الداخلي (أبسط من VLinkNotifPayload)
 * ============================================================ */
typedef struct {
    uint64_t notif_id;          /* معرّف الإشعار الأصلي من الهاتف */
    uint32_t desktop_notif_id;  /* معرّف إشعار D-Bus على الحاسوب */
    char     app_name[64];
    char     title[128];
    char     body[512];
    uint8_t  category;          /* 0=msg, 1=call, 2=alarm, 3=media, 4=sys */
    uint8_t  priority;
    char     actions[4][64];    /* نصوص الإجراءات */
    uint8_t  action_count;
} NotifRecord;

/* ============================================================
 * نوع Callback لإرسال VLINK_NOTIF_ACTION للهاتف
 * يُستدعى عند نقر المستخدم على إجراء في الإشعار
 * ============================================================ */
typedef void (*NotifActionCallback)(uint64_t notif_id,
                                    uint8_t  action_index,
                                    const char *action_text,
                                    gpointer   user_data);

/* ============================================================
 * الواجهة العامة
 * ============================================================ */

/**
 * notif_bridge_init:
 * @action_cb: دالة تُستدعى عند تنفيذ إجراء على إشعار
 * @user_data: بيانات إضافية تُمرر لـ action_cb
 *
 * يُهيئ جسر الإشعارات ويتصل بـ D-Bus.
 * يجب استدعاؤه مرة واحدة في main() قبل أي عملية أخرى.
 */
void notif_bridge_init(NotifActionCallback action_cb, gpointer user_data);

/**
 * notif_bridge_show:
 * @payload: بيانات الإشعار المستلم من الهاتف عبر VLink
 *
 * يعرض الإشعار على سطح مكتب Vaxp عبر D-Bus Notifications.
 * يحفظ سجل الإشعار داخلياً لربط الإجراءات لاحقاً.
 */
void notif_bridge_show(const VLinkNotifPayload *payload);

/**
 * notif_bridge_dismiss:
 * @notif_id: معرّف الإشعار المراد إلغاؤه (من الهاتف)
 *
 * يلغي إشعاراً موجوداً على الحاسوب عبر D-Bus CloseNotification.
 */
void notif_bridge_dismiss(uint64_t notif_id);

/**
 * notif_bridge_get_count:
 * يُعيد عدد الإشعارات النشطة حالياً.
 */
guint notif_bridge_get_count(void);

/**
 * notif_bridge_cleanup:
 * يُحرر جميع الموارد ويغلق اتصال D-Bus.
 * يجب استدعاؤه عند إيقاف الـ daemon.
 */
void notif_bridge_cleanup(void);
