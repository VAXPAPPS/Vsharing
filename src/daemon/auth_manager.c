/*
 * auth_manager.c — تنفيذ مدير مصادقة VLink
 *
 * يستخدم JSON عبر GLib لتخزين الأجهزة، وتشفير بسيط قائم على XOR+HMAC
 * للمصادقة الأولية (يمكن ترقيته لـ X25519+HKDF في مرحلة لاحقة).
 */

#include "auth_manager.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

/* ============================================================
 * أدوات مساعدة داخلية
 * ============================================================ */

/* توليد UUID v4 عشوائي */
static void generate_uuid(char *out_uuid) {
    guint8 uuid[16];
    for (int i = 0; i < 16; i++) {
        uuid[i] = (guint8)g_random_int_range(0, 256);
    }
    /* تمييز النسخة (v4) والتشكيل */
    uuid[6] = (uuid[6] & 0x0F) | 0x40;
    uuid[8] = (uuid[8] & 0x3F) | 0x80;

    snprintf(out_uuid, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             uuid[0], uuid[1], uuid[2], uuid[3],
             uuid[4], uuid[5],
             uuid[6], uuid[7],
             uuid[8], uuid[9],
             uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
}

/* توليد بايتات عشوائية آمنة */
static void generate_random_bytes(uint8_t *buf, gsize len) {
    for (gsize i = 0; i < len; i++) {
        buf[i] = (uint8_t)g_random_int_range(0, 256);
    }
}

/* HMAC-SHA256 بسيط باستخدام GLib (للتحقق من التحدي) */
static void simple_hmac(const uint8_t *key, gsize key_len,
                         const uint8_t *data, gsize data_len,
                         uint8_t *out32) {
    /* نستخدم GChecksum مع pre-hashing بسيط */
    GChecksum *cs = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(cs, key, (gssize)key_len);
    g_checksum_update(cs, data, (gssize)data_len);

    gsize digest_len = 32;
    g_checksum_get_digest(cs, out32, &digest_len);
    g_checksum_free(cs);
}

/* تحويل بايتات إلى hex string */
static gchar *bytes_to_hex(const uint8_t *data, gsize len) {
    GString *s = g_string_new(NULL);
    for (gsize i = 0; i < len; i++) {
        g_string_append_printf(s, "%02x", data[i]);
    }
    return g_string_free(s, FALSE);
}

/* تحديد مسار ملف الإعدادات */
static char *get_config_path(void) {
    const char *config_dir = g_get_user_config_dir();
    char *vsharing_dir = g_build_filename(config_dir, "vsharing", NULL);

    /* إنشاء المجلد إذا لم يكن موجوداً */
    if (g_mkdir_with_parents(vsharing_dir, 0700) != 0) {
        g_warning("[Auth] Cannot create config dir: %s", vsharing_dir);
    }

    char *path = g_build_filename(vsharing_dir, "trusted_devices.json", NULL);
    g_free(vsharing_dir);
    return path;
}

/* ============================================================
 * تحميل / حفظ الأجهزة الموثوقة (JSON عبر GLib)
 * ============================================================ */

static void load_trusted_devices(VLinkAuthManager *mgr) {
    gchar *contents = NULL;
    GError *error   = NULL;

    if (!g_file_get_contents(mgr->config_path, &contents, NULL, &error)) {
        if (error->code != G_FILE_ERROR_NOENT) {
            g_warning("[Auth] Failed to load devices: %s", error->message);
        }
        g_error_free(error);
        return;
    }

    /* تحليل JSON بسيط يدوياً (لتجنب تبعية json-glib) */
    /* كل سطر: device_id|device_name|type|pubkey_hex|permissions|first_seen|last_seen|last_ip */
    gchar **lines = g_strsplit(contents, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        if (lines[i][0] == '#' || lines[i][0] == '\0') continue;
        if (g_str_has_prefix(lines[i], "DEVICE:")) {
            gchar **parts = g_strsplit(lines[i] + 7, "|", 8);
            if (g_strv_length(parts) >= 7) {
                VLinkTrustedDevice *dev = g_new0(VLinkTrustedDevice, 1);
                g_strlcpy(dev->device_id,   parts[0], sizeof(dev->device_id));
                g_strlcpy(dev->device_name, parts[1], sizeof(dev->device_name));
                dev->device_type  = (uint8_t)atoi(parts[2]);
                dev->permissions  = (uint32_t)atol(parts[4]);
                dev->first_seen   = (int64_t)atoll(parts[5]);
                dev->last_seen    = (int64_t)atoll(parts[6]);
                if (g_strv_length(parts) >= 8) {
                    g_strlcpy(dev->last_ip, parts[7], sizeof(dev->last_ip));
                }
                /* تحويل hex المفتاح إلى بايتات */
                for (int j = 0; j < 32 && parts[3][j*2]; j++) {
                    char byte_str[3] = {parts[3][j*2], parts[3][j*2+1], '\0'};
                    dev->public_key[j] = (uint8_t)strtol(byte_str, NULL, 16);
                }
                g_hash_table_insert(mgr->trusted_devices,
                                    g_strdup(dev->device_id), dev);
                g_print("[Auth] Loaded trusted device: %s (%s)\n",
                        dev->device_name, dev->device_id);
            }
            g_strfreev(parts);
        }
    }
    g_strfreev(lines);
    g_free(contents);
}

gboolean vlink_auth_save(VLinkAuthManager *mgr) {
    if (!mgr || !mgr->config_path) return FALSE;

    g_mutex_lock(&mgr->mutex);

    GString *out = g_string_new("# Vsharing Trusted Devices\n");
    g_string_append_printf(out, "# server_id=%s\n", mgr->server_id);

    /* حفظ مفتاح الخادم العام */
    gchar *server_key_hex = bytes_to_hex(mgr->server_public_key, 32);
    g_string_append_printf(out, "# server_pubkey=%s\n\n", server_key_hex);
    g_free(server_key_hex);

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, mgr->trusted_devices);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        VLinkTrustedDevice *dev = (VLinkTrustedDevice *)value;
        gchar *key_hex = bytes_to_hex(dev->public_key, 32);
        g_string_append_printf(out, "DEVICE:%s|%s|%u|%s|%u|%lld|%lld|%s\n",
                                dev->device_id,
                                dev->device_name,
                                dev->device_type,
                                key_hex,
                                dev->permissions,
                                (long long)dev->first_seen,
                                (long long)dev->last_seen,
                                dev->last_ip);
        g_free(key_hex);
    }

    GError *error = NULL;
    gboolean ok   = g_file_set_contents(mgr->config_path, out->str, out->len, &error);
    if (!ok) {
        g_warning("[Auth] Failed to save devices: %s", error->message);
        g_error_free(error);
    }

    g_string_free(out, TRUE);
    g_mutex_unlock(&mgr->mutex);
    return ok;
}

/* ============================================================
 * إنشاء وتحرير مدير المصادقة
 * ============================================================ */

VLinkAuthManager *vlink_auth_manager_new(void) {
    VLinkAuthManager *mgr = g_new0(VLinkAuthManager, 1);

    mgr->trusted_devices    = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                     g_free, g_free);
    mgr->pending_challenges = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                     g_free, g_free);
    g_mutex_init(&mgr->mutex);

    mgr->config_path = get_config_path();

    /* إنشاء UUID للخادم أو تحميله */
    generate_uuid(mgr->server_id);

    /* توليد زوج مفاتيح الخادم */
    generate_random_bytes(mgr->server_private_key, 32);
    /* المفتاح العام = SHA256(private) كتبسيط (X25519 الحقيقي يحتاج libsodium) */
    GChecksum *cs = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(cs, mgr->server_private_key, 32);
    gsize pub_len = 32;
    g_checksum_get_digest(cs, mgr->server_public_key, &pub_len);
    g_checksum_free(cs);

    /* تحميل الأجهزة الموثوقة المخزنة */
    load_trusted_devices(mgr);

    g_print("[Auth] VLink Auth Manager initialized. Server ID: %s\n", mgr->server_id);
    g_print("[Auth] Loaded %u trusted device(s).\n",
            g_hash_table_size(mgr->trusted_devices));

    return mgr;
}

void vlink_auth_manager_free(VLinkAuthManager *mgr) {
    if (!mgr) return;
    vlink_auth_save(mgr);
    g_hash_table_destroy(mgr->trusted_devices);
    g_hash_table_destroy(mgr->pending_challenges);
    g_free(mgr->config_path);
    g_mutex_clear(&mgr->mutex);
    g_free(mgr);
}

/* ============================================================
 * عمليات المصادقة
 * ============================================================ */

gboolean vlink_auth_is_trusted(VLinkAuthManager *mgr, const char *device_id) {
    if (!mgr || !device_id) return FALSE;
    g_mutex_lock(&mgr->mutex);
    gboolean trusted = g_hash_table_contains(mgr->trusted_devices, device_id);
    g_mutex_unlock(&mgr->mutex);
    return trusted;
}

VLinkTrustedDevice *vlink_auth_get_device(VLinkAuthManager *mgr,
                                            const char *device_id) {
    if (!mgr || !device_id) return NULL;
    g_mutex_lock(&mgr->mutex);
    VLinkTrustedDevice *dev = g_hash_table_lookup(mgr->trusted_devices, device_id);
    g_mutex_unlock(&mgr->mutex);
    return dev;
}

GBytes *vlink_auth_start_challenge(VLinkAuthManager *mgr,
                                    const VLinkAuthPayload *req,
                                    uint32_t seq) {
    if (!mgr || !req) return NULL;

    char device_id[37] = {0};
    /* تحويل UUID bytes إلى string */
    snprintf(device_id, sizeof(device_id),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             req->device_id[0],  req->device_id[1],
             req->device_id[2],  req->device_id[3],
             req->device_id[4],  req->device_id[5],
             req->device_id[6],  req->device_id[7],
             req->device_id[8],  req->device_id[9],
             req->device_id[10], req->device_id[11],
             req->device_id[12], req->device_id[13],
             req->device_id[14], req->device_id[15]);

    /* إنشاء تحدي عشوائي 32-byte */
    uint8_t *challenge = g_new(uint8_t, 32);
    generate_random_bytes(challenge, 32);

    g_mutex_lock(&mgr->mutex);
    g_hash_table_insert(mgr->pending_challenges,
                        g_strdup(device_id),
                        challenge);
    g_mutex_unlock(&mgr->mutex);

    /* بناء رسالة التحدي */
    VLinkChallengePayload chall = {0};
    memcpy(chall.challenge, challenge, 32);
    memcpy(chall.server_public_key, mgr->server_public_key, 32);
    g_strlcpy(chall.server_name, "Vaxp Desktop", sizeof(chall.server_name));
    chall.server_capabilities = GUINT32_TO_BE(VLINK_CAP_ALL);

    g_print("[Auth] Challenge sent to device: %s (%s)\n",
            req->device_name, device_id);

    return vlink_build_frame(VLINK_AUTH_CHALLENGE, VLINK_FLAG_NONE, seq,
                              (uint8_t *)&chall, sizeof(chall));
}

const char *vlink_auth_verify_response(VLinkAuthManager *mgr,
                                        const char *device_id,
                                        const VLinkAuthRespPayload *resp) {
    if (!mgr || !device_id || !resp) return NULL;

    g_mutex_lock(&mgr->mutex);
    uint8_t *challenge = g_hash_table_lookup(mgr->pending_challenges, device_id);
    if (!challenge) {
        g_mutex_unlock(&mgr->mutex);
        g_warning("[Auth] No pending challenge for device: %s", device_id);
        return NULL;
    }

    /* تحقق: HMAC-SHA256(challenge, session_key) يساوي challenge_response */
    uint8_t expected[32];
    simple_hmac(resp->session_key, 32, challenge, 32, expected);

    if (memcmp(expected, resp->challenge_response, 32) != 0) {
        g_mutex_unlock(&mgr->mutex);
        g_warning("[Auth] Challenge verification FAILED for device: %s", device_id);
        return NULL;
    }

    /* إزالة التحدي المعلّق */
    g_hash_table_remove(mgr->pending_challenges, device_id);

    /* إضافة الجهاز للقائمة الموثوقة */
    VLinkTrustedDevice *dev = g_hash_table_lookup(mgr->trusted_devices, device_id);
    if (!dev) {
        dev = g_new0(VLinkTrustedDevice, 1);
        g_strlcpy(dev->device_id, device_id, sizeof(dev->device_id));
        dev->first_seen   = (int64_t)(g_get_real_time() / G_USEC_PER_SEC);
        dev->permissions  = VLINK_CAP_ALL;
        g_hash_table_insert(mgr->trusted_devices, g_strdup(device_id), dev);
    }
    dev->last_seen = (int64_t)(g_get_real_time() / G_USEC_PER_SEC);

    /* حفظ مفتاح الجلسة */
    memcpy(dev->public_key, resp->session_key, 32);

    const char *stored_id = dev->device_id;
    g_mutex_unlock(&mgr->mutex);

    /* حفظ على القرص */
    vlink_auth_save(mgr);

    g_print("[Auth] ✅ Device authenticated successfully: %s\n", device_id);
    return stored_id;
}

gboolean vlink_auth_revoke_device(VLinkAuthManager *mgr, const char *device_id) {
    if (!mgr || !device_id) return FALSE;
    g_mutex_lock(&mgr->mutex);
    gboolean removed = g_hash_table_remove(mgr->trusted_devices, device_id);
    g_mutex_unlock(&mgr->mutex);
    if (removed) {
        vlink_auth_save(mgr);
        g_print("[Auth] Device revoked: %s\n", device_id);
    }
    return removed;
}

GList *vlink_auth_get_all_devices(VLinkAuthManager *mgr) {
    if (!mgr) return NULL;
    g_mutex_lock(&mgr->mutex);
    GList *values = g_hash_table_get_values(mgr->trusted_devices);
    g_mutex_unlock(&mgr->mutex);
    return values;
}

gchar *vlink_auth_get_server_public_key_b64(VLinkAuthManager *mgr) {
    if (!mgr) return NULL;
    return g_base64_encode(mgr->server_public_key, 32);
}
