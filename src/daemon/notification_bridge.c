#include "notification_bridge.h"
#include <gio/gio.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ============================================================
 * ثوابت
 * ============================================================ */
#define NOTIF_BUS_NAME    "org.freedesktop.Notifications"
#define NOTIF_OBJ_PATH    "/org/freedesktop/Notifications"
#define NOTIF_IFACE       "org.freedesktop.Notifications"
#define MAX_ACTIVE_NOTIFS 64

/* أيقونات D-Bus لكل فئة إشعار */
static const char *NOTIF_ICONS[] = {
    "message-new-instant",   /* 0 = msg  */
    "call-start-symbolic",   /* 1 = call */
    "appointment-soon",      /* 2 = alarm */
    "media-playback-start",  /* 3 = media */
    "dialog-information",    /* 4 = sys  */
};
#define NOTIF_ICON_COUNT 5

/* ============================================================
 * حالة داخلية
 * ============================================================ */
static GDBusConnection     *g_dbus     = NULL;
static NotifActionCallback  g_action_cb = NULL;
static gpointer             g_action_data = NULL;

/* جدول الإشعارات النشطة */
static NotifRecord g_notifs[MAX_ACTIVE_NOTIFS];
static guint       g_notif_count = 0;

/* guint subscription_id للإشارات D-Bus */
static guint g_signal_sub = 0;

/* ============================================================
 * دوال مساعدة داخلية
 * ============================================================ */

/* إيجاد سجل إشعار بمعرّف الهاتف */
static NotifRecord *find_by_phone_id(uint64_t notif_id) {
    for (guint i = 0; i < g_notif_count; i++) {
        if (g_notifs[i].notif_id == notif_id)
            return &g_notifs[i];
    }
    return NULL;
}

/* إيجاد سجل إشعار بمعرّف D-Bus */
static NotifRecord *find_by_desktop_id(uint32_t desktop_id) {
    for (guint i = 0; i < g_notif_count; i++) {
        if (g_notifs[i].desktop_notif_id == desktop_id)
            return &g_notifs[i];
    }
    return NULL;
}

/* حذف سجل إشعار من الجدول */
static void remove_notif_record(NotifRecord *rec) {
    guint idx = (guint)(rec - g_notifs);
    if (idx >= g_notif_count) return;
    /* نقل آخر عنصر مكان المحذوف */
    if (idx < g_notif_count - 1)
        g_notifs[idx] = g_notifs[g_notif_count - 1];
    g_notif_count--;
}

/* ============================================================
 * مستمع إشارات D-Bus (NotificationClosed, ActionInvoked)
 * ============================================================ */
static void on_dbus_signal(GDBusConnection  *conn,
                            const gchar      *sender_name,
                            const gchar      *object_path,
                            const gchar      *interface_name,
                            const gchar      *signal_name,
                            GVariant         *parameters,
                            gpointer          user_data)
{
    (void)conn; (void)sender_name; (void)object_path;
    (void)interface_name; (void)user_data;

    if (g_strcmp0(signal_name, "ActionInvoked") == 0) {
        /* ActionInvoked(uint32 id, string action_key) */
        guint32     desktop_id = 0;
        const char *action_key = NULL;
        g_variant_get(parameters, "(us)", &desktop_id, &action_key);

        NotifRecord *rec = find_by_desktop_id(desktop_id);
        if (!rec || !action_key) return;

        /* نحوّل action_key ("0","1","2","3") إلى index */
        uint8_t idx = (uint8_t)atoi(action_key);
        const char *action_text = (idx < rec->action_count) ?
                                   rec->actions[idx] : action_key;

        g_print("[NotifBridge] 🔔 Action '%s' on notif %" G_GUINT64_FORMAT "\n",
                action_text, rec->notif_id);

        /* استدعاء الـ callback ليرسل VLINK_NOTIF_ACTION للهاتف */
        if (g_action_cb)
            g_action_cb(rec->notif_id, idx, action_text, g_action_data);

    } else if (g_strcmp0(signal_name, "NotificationClosed") == 0) {
        /* NotificationClosed(uint32 id, uint32 reason) */
        guint32 desktop_id = 0, reason = 0;
        g_variant_get(parameters, "(uu)", &desktop_id, &reason);

        NotifRecord *rec = find_by_desktop_id(desktop_id);
        if (rec) {
            g_print("[NotifBridge] ❌ Notif closed (reason=%u): %" G_GUINT64_FORMAT "\n",
                    reason, rec->notif_id);
            remove_notif_record(rec);
        }
    }
}

/* ============================================================
 * الواجهة العامة
 * ============================================================ */

void notif_bridge_init(NotifActionCallback action_cb, gpointer user_data) {
    if (g_dbus) return; /* لا تعيد التهيئة */

    g_action_cb   = action_cb;
    g_action_data = user_data;
    g_notif_count = 0;

    GError *err = NULL;
    g_dbus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!g_dbus) {
        g_printerr("[NotifBridge] ❌ Failed to connect to D-Bus: %s\n",
                   err ? err->message : "unknown");
        if (err) g_error_free(err);
        return;
    }

    /* اشتراك في إشارات D-Bus Notifications */
    g_signal_sub = g_dbus_connection_signal_subscribe(
        g_dbus,
        NOTIF_BUS_NAME,       /* sender */
        NOTIF_IFACE,          /* interface */
        NULL,                  /* member = all */
        NOTIF_OBJ_PATH,       /* object path */
        NULL,                  /* arg0 filter */
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_dbus_signal,
        NULL, NULL
    );

    g_print("[NotifBridge] ✅ Initialized. D-Bus connected, listening for actions.\n");
}

void notif_bridge_show(const VLinkNotifPayload *payload) {
    if (!payload || !g_dbus) return;

    /* تحقق من وجود إشعار بنفس المعرّف (تحديث) */
    NotifRecord *existing = find_by_phone_id(payload->notif_id);
    uint32_t replace_id = existing ? existing->desktop_notif_id : 0;

    /* الأيقونة */
    uint8_t cat = (payload->category < NOTIF_ICON_COUNT) ? payload->category : 4;
    const char *icon = NOTIF_ICONS[cat];

    /* بناء قائمة الإجراءات لـ D-Bus: ["0","نص0","1","نص1"...] */
    GVariantBuilder actions_builder;
    g_variant_builder_init(&actions_builder, G_VARIANT_TYPE("as"));
    for (uint8_t i = 0; i < payload->action_count && i < 4; i++) {
        char idx_str[4];
        snprintf(idx_str, sizeof(idx_str), "%u", i);
        g_variant_builder_add(&actions_builder, "s", idx_str);
        g_variant_builder_add(&actions_builder, "s", payload->actions[i]);
    }
    /* دائماً أضف "إغلاق" كإجراء افتراضي */
    g_variant_builder_add(&actions_builder, "s", "close");
    g_variant_builder_add(&actions_builder, "s", "إغلاق");

    /* بناء الـ hints */
    GVariantBuilder hints_builder;
    g_variant_builder_init(&hints_builder, G_VARIANT_TYPE("a{sv}"));

    /* urgency: 0=low, 1=normal, 2=critical */
    uint8_t urgency = (payload->priority >= 3) ? 2 :
                      (payload->priority >= 1) ? 1 : 0;
    g_variant_builder_add(&hints_builder, "{sv}",
                          "urgency", g_variant_new_byte(urgency));

    /* category hint */
    const char *cat_hint[] = {"im", "call", "alarm", "media", "device"};
    g_variant_builder_add(&hints_builder, "{sv}",
                          "category", g_variant_new_string(cat_hint[cat]));

    /* مدة الإشعار: المكالمات لا تنتهي تلقائياً (−1)، البقية 8 ثوانٍ */
    gint32 timeout_ms = (payload->category == 1) ? -1 : 8000;

    /* نص الجسم مع اسم التطبيق */
    char full_body[600];
    if (payload->app_name[0])
        snprintf(full_body, sizeof(full_body), "<b>%s</b>\n%s",
                 payload->app_name, payload->body);
    else
        g_strlcpy(full_body, payload->body, sizeof(full_body));

    GError *err = NULL;
    GVariant *result = g_dbus_connection_call_sync(
        g_dbus,
        NOTIF_BUS_NAME,
        NOTIF_OBJ_PATH,
        NOTIF_IFACE,
        "Notify",
        g_variant_new("(susssasa{sv}i)",
                      "VaxpLink",          /* app_name */
                      (guint32)replace_id, /* replaces_id */
                      icon,               /* app_icon */
                      payload->title,     /* summary */
                      full_body,          /* body */
                      &actions_builder,
                      &hints_builder,
                      timeout_ms),
        G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1, NULL, &err
    );

    if (!result) {
        g_printerr("[NotifBridge] ❌ Notify call failed: %s\n",
                   err ? err->message : "unknown");
        if (err) g_error_free(err);
        return;
    }

    guint32 desktop_id = 0;
    g_variant_get(result, "(u)", &desktop_id);
    g_variant_unref(result);

    /* حفظ السجل */
    if (existing) {
        /* تحديث سجل موجود */
        existing->desktop_notif_id = desktop_id;
        g_strlcpy(existing->title,    payload->title,    sizeof(existing->title));
        g_strlcpy(existing->body,     payload->body,     sizeof(existing->body));
        existing->action_count = payload->action_count;
        for (int i = 0; i < payload->action_count && i < 4; i++)
            g_strlcpy(existing->actions[i], payload->actions[i], 64);
    } else if (g_notif_count < MAX_ACTIVE_NOTIFS) {
        /* إنشاء سجل جديد */
        NotifRecord *rec = &g_notifs[g_notif_count++];
        rec->notif_id        = payload->notif_id;
        rec->desktop_notif_id= desktop_id;
        rec->category        = payload->category;
        rec->priority        = payload->priority;
        rec->action_count    = payload->action_count;
        g_strlcpy(rec->app_name, payload->app_name, sizeof(rec->app_name));
        g_strlcpy(rec->title,    payload->title,    sizeof(rec->title));
        g_strlcpy(rec->body,     payload->body,     sizeof(rec->body));
        for (int i = 0; i < payload->action_count && i < 4; i++)
            g_strlcpy(rec->actions[i], payload->actions[i], 64);
    }

    g_print("[NotifBridge] 📱 Shown: [%s] %s — %s (desktop_id=%u)\n",
            payload->app_name, payload->title, payload->body, desktop_id);
}

void notif_bridge_dismiss(uint64_t notif_id) {
    if (!g_dbus) return;

    NotifRecord *rec = find_by_phone_id(notif_id);
    if (!rec) {
        g_print("[NotifBridge] dismiss: notif %" G_GUINT64_FORMAT " not found\n", notif_id);
        return;
    }

    GError *err = NULL;
    GVariant *result = g_dbus_connection_call_sync(
        g_dbus,
        NOTIF_BUS_NAME,
        NOTIF_OBJ_PATH,
        NOTIF_IFACE,
        "CloseNotification",
        g_variant_new("(u)", (guint32)rec->desktop_notif_id),
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        -1, NULL, &err
    );

    if (result) g_variant_unref(result);
    if (err)    g_error_free(err);

    g_print("[NotifBridge] 🗑️  Dismissed notif %" G_GUINT64_FORMAT "\n", notif_id);
    remove_notif_record(rec);
}

guint notif_bridge_get_count(void) {
    return g_notif_count;
}

void notif_bridge_cleanup(void) {
    if (!g_dbus) return;

    /* إلغاء اشتراك الإشارات */
    if (g_signal_sub) {
        g_dbus_connection_signal_unsubscribe(g_dbus, g_signal_sub);
        g_signal_sub = 0;
    }

    g_object_unref(g_dbus);
    g_dbus = NULL;
    g_notif_count = 0;
    g_print("[NotifBridge] 🧹 Cleaned up.\n");
}
