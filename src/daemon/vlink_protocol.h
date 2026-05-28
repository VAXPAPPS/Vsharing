/*
 * vlink_protocol.h — VLink Binary Protocol v2
 * العصب الرئيسي لنظام Vaxp البيئي
 *
 * بروتوكول ثنائي فائق السرعة يحل محل البروتوكول النصي القديم.
 * يدعم قناتين مستقلتين:
 *   - Control Channel (WebSocket/TLS): الأوامر، الإشعارات، الحافظة
 *   - Stream Channel (UDP/RTP): بث الشاشة، الصوت
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <glib.h>

/* ============================================================
 * ثوابت البروتوكول
 * ============================================================ */
#define VLINK_MAGIC         "VLNK"
#define VLINK_VERSION       0x02
#define VLINK_CTRL_PORT     5000
#define VLINK_STREAM_PORT   5001
#define VLINK_MAX_PAYLOAD   (4 * 1024 * 1024)   /* 4 MB */
#define VLINK_CHUNK_SIZE    (64 * 1024)          /* 64 KB per file chunk */
#define VLINK_HEARTBEAT_MS  5000                 /* فحص دوري كل 5 ثوانٍ */

/* ============================================================
 * أنواع الرسائل
 * ============================================================ */
typedef enum {
    /* ── نبضات القلب ── */
    VLINK_PING              = 0x01,
    VLINK_PONG              = 0x02,

    /* ── الحافظة ── */
    VLINK_CLIPBOARD_TEXT    = 0x10,   /* نص عادي */
    VLINK_CLIPBOARD_IMAGE   = 0x11,   /* صورة PNG */
    VLINK_CLIPBOARD_FILES   = 0x12,   /* قائمة ملفات */

    /* ── الإشعارات ── */
    VLINK_NOTIF_SHOW        = 0x20,   /* إظهار إشعار */
    VLINK_NOTIF_DISMISS     = 0x21,   /* إلغاء إشعار */
    VLINK_NOTIF_ACTION      = 0x22,   /* تنفيذ إجراء على إشعار */
    VLINK_NOTIF_UPDATE      = 0x23,   /* تحديث إشعار (تقدم) */

    /* ── نقل الملفات ── */
    VLINK_FILE_OFFER        = 0x30,   /* عرض ملف للإرسال */
    VLINK_FILE_ACCEPT       = 0x31,   /* قبول الملف */
    VLINK_FILE_REJECT       = 0x32,   /* رفض الملف */
    VLINK_FILE_CHUNK        = 0x33,   /* جزء من الملف */
    VLINK_FILE_DONE         = 0x34,   /* اكتمل الملف */
    VLINK_FILE_CANCEL       = 0x35,   /* إلغاء النقل */
    VLINK_FILE_LIST_REQ     = 0x36,   /* طلب قائمة الملفات (VFS) */
    VLINK_FILE_LIST_RESP    = 0x37,   /* استجابة قائمة الملفات */
    VLINK_FILE_READ_REQ     = 0x38,   /* طلب قراءة ملف (VFS) */
    VLINK_FILE_READ_RESP    = 0x39,   /* استجابة البيانات */

    /* ── مشاركة الشاشة ── */
    VLINK_SCREEN_START      = 0x40,   /* بدء بث الشاشة */
    VLINK_SCREEN_STOP       = 0x41,   /* إيقاف البث */
    VLINK_SCREEN_CONFIG     = 0x42,   /* إعدادات البث (دقة، FPS، معدل) */
    VLINK_SCREEN_FRAME      = 0x43,   /* إطار فيديو (UDP Stream Channel) */
    VLINK_SCREEN_KEYFRAME   = 0x44,   /* طلب إطار مرجعي جديد */

    /* ── التحكم عن بُعد (Input) ── */
    VLINK_INPUT_TOUCH       = 0x50,   /* حدث لمس */
    VLINK_INPUT_KEY         = 0x51,   /* ضغطة مفتاح */
    VLINK_INPUT_SCROLL      = 0x52,   /* تمرير */
    VLINK_INPUT_GESTURE     = 0x53,   /* إيماءة (pinch, swipe) */

    /* ── المصادقة والتحكم ── */
    VLINK_AUTH_REQ          = 0x60,   /* طلب مصادقة */
    VLINK_AUTH_CHALLENGE    = 0x61,   /* تحدي التشفير */
    VLINK_AUTH_RESP         = 0x62,   /* رد التحدي */
    VLINK_AUTH_OK           = 0x63,   /* مصادقة ناجحة */
    VLINK_AUTH_FAIL         = 0x64,   /* مصادقة فاشلة */
    VLINK_DISCONNECT        = 0x65,   /* قطع الاتصال بشكل نظيف */
    VLINK_CAPS              = 0x66,   /* إعلان القدرات */
    VLINK_ERROR             = 0xFF,   /* رسالة خطأ */
} VLinkMsgType;

/* ============================================================
 * أعلام الإطار (Flags)
 * ============================================================ */
typedef enum {
    VLINK_FLAG_NONE         = 0x0000,
    VLINK_FLAG_COMPRESSED   = 0x0001,  /* البيانات مضغوطة بـ LZ4 */
    VLINK_FLAG_ENCRYPTED    = 0x0002,  /* البيانات مشفرة بـ AES-256-GCM */
    VLINK_FLAG_FRAGMENT     = 0x0004,  /* هذا الإطار جزء من رسالة أكبر */
    VLINK_FLAG_LAST_FRAG    = 0x0008,  /* آخر جزء */
    VLINK_FLAG_ACK_REQ      = 0x0010,  /* يتطلب تأكيداً */
    VLINK_FLAG_PRIORITY     = 0x0020,  /* أولوية عالية */
} VLinkFlags;

/* ============================================================
 * هيكل إطار VLink الرئيسي (Header = 20 bytes)
 * ============================================================ */
#pragma pack(push, 1)
typedef struct {
    uint8_t  magic[4];       /* "VLNK" */
    uint8_t  version;        /* 0x02 */
    uint8_t  type;           /* VLinkMsgType */
    uint16_t flags;          /* VLinkFlags */
    uint32_t payload_len;    /* حجم البيانات اللاحقة بالبايت */
    uint32_t seq;            /* رقم التسلسل للتأكيد */
    uint32_t checksum;       /* CRC32 على كامل الإطار */
    /* يتبع: uint8_t payload[payload_len]; */
} VLinkFrame;
#pragma pack(pop)

/* ============================================================
 * هياكل البيانات لكل نوع رسالة
 * ============================================================ */

/* --- المصادقة --- */
#pragma pack(push, 1)
typedef struct {
    uint8_t  device_id[16];       /* UUID v4 */
    uint8_t  public_key[32];      /* X25519 Public Key */
    uint8_t  device_type;         /* 0=Android, 1=iOS, 2=Linux, 3=Windows */
    char     device_name[64];
    uint32_t capabilities;        /* VLinkCapability bitmask */
    uint64_t timestamp;           /* Unix timestamp للحماية من Replay */
} VLinkAuthPayload;

typedef struct {
    uint8_t  challenge[32];       /* 32-byte random nonce */
    uint8_t  server_public_key[32];
    char     server_name[64];
    uint32_t server_capabilities;
} VLinkChallengePayload;

typedef struct {
    uint8_t  challenge_response[32]; /* HMAC-SHA256(challenge, shared_secret) */
    uint8_t  session_key[32];        /* مفتاح الجلسة المشترك */
} VLinkAuthRespPayload;
#pragma pack(pop)

/* --- القدرات --- */
typedef enum {
    VLINK_CAP_CLIPBOARD     = (1 << 0),
    VLINK_CAP_NOTIFICATIONS = (1 << 1),
    VLINK_CAP_FILE_TRANSFER = (1 << 2),
    VLINK_CAP_SCREEN_MIRROR = (1 << 3),
    VLINK_CAP_INPUT_CONTROL = (1 << 4),
    VLINK_CAP_VFS           = (1 << 5),
    VLINK_CAP_AUDIO         = (1 << 6),
    VLINK_CAP_CAMERA        = (1 << 7),
    VLINK_CAP_ALL           = 0x00FF,
} VLinkCapability;

/* --- نقل الملفات --- */
#pragma pack(push, 1)
typedef struct {
    uint64_t file_id;         /* معرّف فريد لعملية النقل */
    uint64_t total_size;      /* الحجم الكلي بالبايت */
    uint32_t total_chunks;    /* عدد الأجزاء */
    uint32_t chunk_size;      /* حجم كل جزء */
    char     filename[256];
    char     mime_type[64];
    uint8_t  hash_sha256[32]; /* للتحقق من سلامة الملف */
} VLinkFileOffer;

typedef struct {
    uint64_t file_id;
    uint32_t chunk_index;
    uint32_t chunk_len;
    /* يتبع: بيانات الجزء */
} VLinkFileChunk;
#pragma pack(pop)

/* --- الإشعارات --- */
#pragma pack(push, 1)
typedef struct {
    uint64_t notif_id;
    uint8_t  category;       /* 0=msg, 1=call, 2=alarm, 3=media, 4=sys */
    uint8_t  priority;       /* 0=low, 1=normal, 2=high, 3=urgent */
    uint32_t icon_size;      /* حجم الأيقونة PNG */
    char     app_name[64];
    char     title[128];
    char     body[512];
    char     actions[4][64]; /* حتى 4 إجراءات */
    uint8_t  action_count;
    /* يتبع اختيارياً: بيانات الأيقونة PNG */
} VLinkNotifPayload;
#pragma pack(pop)

/* --- مشاركة الشاشة --- */
#pragma pack(push, 1)
typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t  fps;           /* 10، 15، 30، 60 */
    uint8_t  codec;         /* 0=H.264، 1=VP9، 2=AV1 */
    uint32_t bitrate;       /* بالـ bits/s */
    uint8_t  use_hw_accel;
} VLinkScreenConfig;

typedef struct {
    uint32_t frame_id;
    uint32_t timestamp_ms;
    uint8_t  is_keyframe;
    uint32_t data_len;
    /* يتبع: بيانات الإطار المشفرة */
} VLinkScreenFrame;

/* --- أحداث الإدخال --- */
typedef struct {
    uint8_t  action;        /* 0=DOWN، 1=UP، 2=MOVE */
    uint8_t  pointer_id;
    float    x_normalized; /* 0.0 - 1.0 نسبة لعرض الشاشة */
    float    y_normalized;
    float    pressure;
} VLinkTouchEvent;

typedef struct {
    uint32_t keycode;       /* Linux evdev keycode */
    uint8_t  action;        /* 0=DOWN، 1=UP */
    uint8_t  modifiers;     /* Ctrl/Shift/Alt bitmask */
} VLinkKeyEvent;
#pragma pack(pop)

/* ============================================================
 * سياق الاتصال لجهاز موثوق
 * ============================================================ */
typedef struct _VLinkDevice VLinkDevice;

typedef void (*VLinkMsgCallback)(VLinkDevice *device, VLinkMsgType type,
                                  const uint8_t *payload, uint32_t len,
                                  gpointer user_data);

struct _VLinkDevice {
    char          device_id[37];    /* UUID string */
    char          device_name[64];
    char          remote_ip[46];    /* IPv4 أو IPv6 */
    uint8_t       device_type;
    uint32_t      capabilities;
    gboolean      authenticated;

    /* قناة التحكم (WebSocket) */
    gpointer      ctrl_conn;        /* SoupWebsocketConnection* */

    /* إحصائيات */
    uint32_t      bytes_sent;
    uint32_t      bytes_recv;
    uint64_t      connected_at;
    uint32_t      next_seq;

    /* Callback للأحداث */
    VLinkMsgCallback on_message;
    gpointer         user_data;
};

/* ============================================================
 * دوال البروتوكول
 * ============================================================ */

/* بناء إطار VLink ثنائي جاهز للإرسال */
GBytes *vlink_build_frame(VLinkMsgType type, VLinkFlags flags,
                           uint32_t seq, const uint8_t *payload,
                           uint32_t payload_len);

/* تحليل إطار VLink مستلم من البيانات الخام */
gboolean vlink_parse_frame(const uint8_t *data, gsize len,
                            VLinkFrame *out_header,
                            const uint8_t **out_payload);

/* حساب CRC32 للتحقق من سلامة البيانات */
uint32_t vlink_crc32(const uint8_t *data, gsize len);

/* بناء رسائل جاهزة لأنواع محددة */
GBytes *vlink_make_clipboard_text(uint32_t seq, const char *text);
GBytes *vlink_make_ping(uint32_t seq);
GBytes *vlink_make_pong(uint32_t seq);
GBytes *vlink_make_notif(uint32_t seq, const VLinkNotifPayload *notif);
GBytes *vlink_make_auth_req(uint32_t seq, const VLinkAuthPayload *auth);
GBytes *vlink_make_caps(uint32_t seq, uint32_t caps);
GBytes *vlink_make_error(uint32_t seq, uint8_t code, const char *message);
