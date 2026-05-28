/*
 * vlink_protocol.c — تنفيذ بروتوكول VLink v2
 */

#include "vlink_protocol.h"
#include <string.h>
#include <stdlib.h>

/* ============================================================
 * CRC32 Table (IEEE 802.3)
 * ============================================================ */
static uint32_t crc32_table[256];
static gboolean crc32_initialized = FALSE;

static void crc32_init_table(void) {
    if (crc32_initialized) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            crc = (crc & 1) ? (0xEDB88320 ^ (crc >> 1)) : (crc >> 1);
        }
        crc32_table[i] = crc;
    }
    crc32_initialized = TRUE;
}

uint32_t vlink_crc32(const uint8_t *data, gsize len) {
    crc32_init_table();
    uint32_t crc = 0xFFFFFFFF;
    for (gsize i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

/* ============================================================
 * بناء إطار VLink
 * ============================================================ */
GBytes *vlink_build_frame(VLinkMsgType type, VLinkFlags flags,
                           uint32_t seq, const uint8_t *payload,
                           uint32_t payload_len) {
    /* تحقق من الحجم الأقصى */
    if (payload_len > VLINK_MAX_PAYLOAD) {
        g_warning("[VLink] payload too large: %u bytes", payload_len);
        return NULL;
    }

    gsize total_size = sizeof(VLinkFrame) + payload_len;
    uint8_t *buf = g_malloc(total_size);
    if (!buf) return NULL;

    VLinkFrame *frame = (VLinkFrame *)buf;
    memcpy(frame->magic, VLINK_MAGIC, 4);
    frame->version     = VLINK_VERSION;
    frame->type        = (uint8_t)type;
    frame->flags       = GUINT16_TO_BE(flags);
    frame->payload_len = GUINT32_TO_BE(payload_len);
    frame->seq         = GUINT32_TO_BE(seq);

    if (payload && payload_len > 0) {
        memcpy(buf + sizeof(VLinkFrame), payload, payload_len);
    }

    /* نحسب CRC32 على الإطار كاملاً (مع صفر في مكان الـ checksum) */
    frame->checksum = 0;
    uint32_t crc = vlink_crc32(buf, total_size);
    frame->checksum = GUINT32_TO_BE(crc);

    return g_bytes_new_take(buf, total_size);
}

/* ============================================================
 * تحليل إطار VLink مستلم
 * ============================================================ */
gboolean vlink_parse_frame(const uint8_t *data, gsize len,
                            VLinkFrame *out_header,
                            const uint8_t **out_payload) {
    if (!data || len < sizeof(VLinkFrame)) {
        g_warning("[VLink] frame too short: %zu bytes", len);
        return FALSE;
    }

    const VLinkFrame *frame = (const VLinkFrame *)data;

    /* التحقق من السحر */
    if (memcmp(frame->magic, VLINK_MAGIC, 4) != 0) {
        g_warning("[VLink] invalid magic bytes");
        return FALSE;
    }

    /* التحقق من الإصدار */
    if (frame->version != VLINK_VERSION) {
        g_warning("[VLink] version mismatch: got %u, expected %u",
                  frame->version, VLINK_VERSION);
        return FALSE;
    }

    uint32_t payload_len = GUINT32_FROM_BE(frame->payload_len);

    /* التحقق من حجم البيانات الكلي */
    if (len < sizeof(VLinkFrame) + payload_len) {
        g_warning("[VLink] truncated payload: expected %u + %zu bytes, got %zu",
                  payload_len, sizeof(VLinkFrame), len);
        return FALSE;
    }

    /* التحقق من CRC32 */
    uint32_t received_crc = GUINT32_FROM_BE(frame->checksum);
    uint8_t *verify_buf   = g_memdup2(data, sizeof(VLinkFrame) + payload_len);
    ((VLinkFrame *)verify_buf)->checksum = 0;
    uint32_t computed_crc = vlink_crc32(verify_buf, sizeof(VLinkFrame) + payload_len);
    g_free(verify_buf);

    if (received_crc != computed_crc) {
        g_warning("[VLink] CRC mismatch: received 0x%08X, computed 0x%08X",
                  received_crc, computed_crc);
        return FALSE;
    }

    /* نقل البيانات للمخرجات */
    memcpy(out_header, frame, sizeof(VLinkFrame));
    out_header->flags       = GUINT16_FROM_BE(frame->flags);
    out_header->payload_len = payload_len;
    out_header->seq         = GUINT32_FROM_BE(frame->seq);
    out_header->checksum    = received_crc;

    if (out_payload) {
        *out_payload = (payload_len > 0) ? (data + sizeof(VLinkFrame)) : NULL;
    }

    return TRUE;
}

/* ============================================================
 * دوال بناء الرسائل الجاهزة
 * ============================================================ */

GBytes *vlink_make_ping(uint32_t seq) {
    uint64_t ts = (uint64_t)g_get_real_time();
    ts = GUINT64_TO_BE(ts);
    return vlink_build_frame(VLINK_PING, VLINK_FLAG_PRIORITY, seq,
                              (uint8_t *)&ts, sizeof(ts));
}

GBytes *vlink_make_pong(uint32_t seq) {
    uint64_t ts = (uint64_t)g_get_real_time();
    ts = GUINT64_TO_BE(ts);
    return vlink_build_frame(VLINK_PONG, VLINK_FLAG_PRIORITY, seq,
                              (uint8_t *)&ts, sizeof(ts));
}

GBytes *vlink_make_clipboard_text(uint32_t seq, const char *text) {
    if (!text) return NULL;
    gsize len = strlen(text);
    if (len == 0) return NULL;
    return vlink_build_frame(VLINK_CLIPBOARD_TEXT, VLINK_FLAG_NONE, seq,
                              (const uint8_t *)text, (uint32_t)len);
}

GBytes *vlink_make_notif(uint32_t seq, const VLinkNotifPayload *notif) {
    if (!notif) return NULL;
    return vlink_build_frame(VLINK_NOTIF_SHOW, VLINK_FLAG_NONE, seq,
                              (const uint8_t *)notif, sizeof(VLinkNotifPayload));
}

GBytes *vlink_make_auth_req(uint32_t seq, const VLinkAuthPayload *auth) {
    if (!auth) return NULL;
    return vlink_build_frame(VLINK_AUTH_REQ, VLINK_FLAG_NONE, seq,
                              (const uint8_t *)auth, sizeof(VLinkAuthPayload));
}

GBytes *vlink_make_caps(uint32_t seq, uint32_t caps) {
    uint32_t caps_be = GUINT32_TO_BE(caps);
    return vlink_build_frame(VLINK_CAPS, VLINK_FLAG_NONE, seq,
                              (uint8_t *)&caps_be, sizeof(caps_be));
}

GBytes *vlink_make_error(uint32_t seq, uint8_t code, const char *message) {
    char buf[256];
    buf[0] = code;
    if (message) {
        g_strlcpy(buf + 1, message, sizeof(buf) - 1);
    } else {
        buf[1] = '\0';
    }
    gsize len = 1 + (message ? strlen(message) : 0);
    return vlink_build_frame(VLINK_ERROR, VLINK_FLAG_NONE, seq,
                              (uint8_t *)buf, (uint32_t)len);
}

/* ============================================================
 * المرحلة 4: بُناة إطارات VFS RPC
 *
 * تنسيق حمولة VLINK_FILE_LIST_REQ (VFS Request):
 *   [VfsRpcHeader 10 bytes][op_payload bytes][path bytes]
 *
 * تنسيق حمولة VLINK_FILE_LIST_RESP (VFS Response):
 *   [VfsRpcRespHeader 12 bytes][resp_data bytes]
 * ============================================================ */

/**
 * vlink_make_vfs_request:
 * @req_id:        معرّف فريد يُستخدم لربط الطلب بالاستجابة
 * @op:            كود العملية (VfsOpCode)
 * @op_payload:    بيانات إضافية خاصة بالعملية (يمكن أن تكون NULL)
 * @op_payload_len: حجم op_payload بالبايت
 * @path:          مسار الملف/المجلد الهدف (يمكن أن يكون NULL لعمليات مثل STATFS)
 *
 * يُعيد GBytes* يحتوي على إطار VLink كامل، أو NULL عند الخطأ.
 * المستدعي مسؤول عن استدعاء g_bytes_unref() على النتيجة.
 */
GBytes *vlink_make_vfs_request(uint64_t req_id, VfsOpCode op,
                                const uint8_t *op_payload,
                                uint32_t op_payload_len,
                                const char *path)
{
    uint32_t path_len = path ? (uint32_t)strlen(path) : 0;

    /* احسب الحجم الإجمالي: header + op_payload + path */
    uint32_t total = sizeof(VfsRpcHeader) + op_payload_len + path_len;

    if (total > VLINK_MAX_PAYLOAD) {
        g_warning("[VFS] Request payload too large: %u bytes (max=%u)",
                  total, VLINK_MAX_PAYLOAD);
        return NULL;
    }

    uint8_t *buf = g_malloc0(total);
    if (!buf) return NULL;

    /* رأس الطلب */
    VfsRpcHeader *hdr = (VfsRpcHeader *)buf;
    hdr->req_id = GUINT64_TO_BE(req_id);
    hdr->op     = (uint8_t)op;
    hdr->flags  = 0;

    uint8_t *cursor = buf + sizeof(VfsRpcHeader);

    /* البيانات الخاصة بالعملية */
    if (op_payload && op_payload_len > 0) {
        memcpy(cursor, op_payload, op_payload_len);
        cursor += op_payload_len;
    }

    /* المسار */
    if (path && path_len > 0) {
        memcpy(cursor, path, path_len);
    }

    GBytes *frame = vlink_build_frame(
        VLINK_FILE_LIST_REQ,   /* نستخدمه كـ VFS_RPC_REQUEST */
        VLINK_FLAG_PRIORITY,   /* أولوية عالية لضمان سرعة الاستجابة */
        0,                     /* seq = 0 (req_id يُغني عنه هنا) */
        buf, total
    );
    g_free(buf);
    return frame;
}

/**
 * vlink_make_vfs_response:
 * @req_id:      نفس req_id من الطلب الأصلي
 * @error:       رمز الخطأ (0 = نجاح، سالب = VfsErrorCode)
 * @resp_data:   بيانات الاستجابة الخاصة بالعملية (يمكن أن تكون NULL)
 * @resp_data_len: حجم resp_data بالبايت
 *
 * يُعيد إطار VLink كاملاً لإرسال الاستجابة للجهاز.
 */
GBytes *vlink_make_vfs_response(uint64_t req_id, int32_t error,
                                 const uint8_t *resp_data,
                                 uint32_t resp_data_len)
{
    uint32_t total = sizeof(VfsRpcRespHeader) + resp_data_len;

    if (total > VLINK_MAX_PAYLOAD) {
        g_warning("[VFS] Response payload too large: %u bytes", total);
        return NULL;
    }

    uint8_t *buf = g_malloc0(total);
    if (!buf) return NULL;

    VfsRpcRespHeader *hdr = (VfsRpcRespHeader *)buf;
    hdr->req_id = GUINT64_TO_BE(req_id);
    hdr->error  = (int32_t)GUINT32_TO_BE((uint32_t)error);

    if (resp_data && resp_data_len > 0)
        memcpy(buf + sizeof(VfsRpcRespHeader), resp_data, resp_data_len);

    GBytes *frame = vlink_build_frame(
        VLINK_FILE_LIST_RESP,  /* VFS_RPC_RESPONSE */
        VLINK_FLAG_NONE,
        0,
        buf, total
    );
    g_free(buf);
    return frame;
}

