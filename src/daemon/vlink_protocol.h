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

/* ============================================================
 * ── المرحلة 4: VFS Bridge (نظام الملفات الافتراضي) ──
 *
 * يستخدم VLINK_FILE_LIST_REQ  (0x36) كـ "VFS_RPC_REQUEST"
 * ويستخدم VLINK_FILE_LIST_RESP (0x37) كـ "VFS_RPC_RESPONSE"
 * يستخدم VLINK_FILE_READ_REQ  (0x38) كـ "VFS_WRITE_REQUEST"
 * يستخدم VLINK_FILE_READ_RESP (0x39) كـ "VFS_WRITE_RESPONSE"
 *
 * بروتوكول VFS RPC ثنائي الاتجاه:
 *   PC  →  Phone : VLINK_FILE_LIST_REQ  + VfsRpcRequest
 *   Phone → PC   : VLINK_FILE_LIST_RESP + VfsRpcResponse
 *   PC  →  Phone : VLINK_FILE_READ_REQ  + VfsWriteRequest
 *   Phone → PC   : VLINK_FILE_READ_RESP + VfsWriteResponse
 *
 * كل عملية تحمل req_id فريداً لربط الطلب بالاستجابة
 * ============================================================ */

/* ──────────────────────────────────────
 * أكواد عمليات VFS
 * ────────────────────────────────────── */
typedef enum {
    VFS_OP_STAT      = 0x01,   /* الحصول على معلومات ملف/مجلد */
    VFS_OP_READDIR   = 0x02,   /* قراءة محتوى مجلد */
    VFS_OP_OPEN      = 0x03,   /* فتح ملف */
    VFS_OP_READ      = 0x04,   /* قراءة بيانات من ملف */
    VFS_OP_WRITE     = 0x05,   /* كتابة بيانات في ملف */
    VFS_OP_CREATE    = 0x06,   /* إنشاء ملف جديد */
    VFS_OP_UNLINK    = 0x07,   /* حذف ملف */
    VFS_OP_MKDIR     = 0x08,   /* إنشاء مجلد */
    VFS_OP_RMDIR     = 0x09,   /* حذف مجلد */
    VFS_OP_RENAME    = 0x0A,   /* إعادة تسمية */
    VFS_OP_STATFS    = 0x0B,   /* معلومات نظام الملفات */
    VFS_OP_TRUNCATE  = 0x0C,   /* تغيير حجم ملف */
    VFS_OP_CLOSE     = 0x0D,   /* إغلاق ملف */
} VfsOpCode;

/* ──────────────────────────────────────
 * أكواد أخطاء VFS (مُعيَّرة على errno)
 * ────────────────────────────────────── */
typedef enum {
    VFS_OK          =  0,    /* نجاح */
    VFS_ENOENT      = -2,    /* الملف/المجلد غير موجود */
    VFS_EACCES      = -13,   /* لا صلاحية للوصول */
    VFS_EEXIST      = -17,   /* الملف موجود مسبقاً */
    VFS_ENOTDIR     = -20,   /* ليس مجلداً */
    VFS_EISDIR      = -21,   /* هو مجلد */
    VFS_EINVAL      = -22,   /* معامل غير صالح */
    VFS_ENOSPC      = -28,   /* لا مساحة كافية */
    VFS_EROFS       = -30,   /* نظام ملفات للقراءة فقط */
    VFS_ENAMETOOLONG= -36,   /* اسم الملف طويل جداً */
    VFS_ENOTEMPTY   = -39,   /* المجلد غير فارغ */
    VFS_ENODEV      = -19,   /* الجهاز غير متصل */
    VFS_ETIMEOUT    = -110,  /* انتهت المهلة الزمنية */
    VFS_EIO         = -5,    /* خطأ إدخال/إخراج */
} VfsErrorCode;

/* ──────────────────────────────────────
 * هيكل رأس طلب VFS RPC (8 + 2 bytes = 10 bytes)
 * ────────────────────────────────────── */
#pragma pack(push, 1)
typedef struct {
    uint64_t req_id;    /* معرّف فريد للطلب (لربطه بالاستجابة) */
    uint8_t  op;        /* VfsOpCode */
    uint8_t  flags;     /* أعلام العملية (0 = عادي) */
    /* يتبع: بيانات العملية (op-specific) */
} VfsRpcHeader;

/* هيكل رأس استجابة VFS RPC (8 + 4 bytes = 12 bytes) */
typedef struct {
    uint64_t req_id;    /* نفس req_id من الطلب */
    int32_t  error;     /* VfsErrorCode (0 = نجاح) */
    /* يتبع: بيانات الاستجابة (op-specific) */
} VfsRpcRespHeader;

/* ──────────────────────────────────────
 * بنية معلومات الملف (STAT)
 * ────────────────────────────────────── */
typedef struct {
    uint64_t  file_size;      /* حجم الملف بالبايت */
    uint32_t  mode;           /* نوع الملف + صلاحيات Unix */
    uint32_t  uid;            /* معرّف المالك */
    uint32_t  gid;            /* معرّف المجموعة */
    uint32_t  nlink;          /* عدد الروابط الصلبة */
    int64_t   atime_sec;      /* آخر وصول (ثواني Unix) */
    int64_t   mtime_sec;      /* آخر تعديل */
    int64_t   ctime_sec;      /* آخر تغيير للبيانات الوصفية */
    uint32_t  blksize;        /* حجم الكتلة المفضل */
    uint64_t  blocks;         /* عدد الكتل المخصصة (512-byte) */
} VfsStatInfo;

/* ──────────────────────────────────────
 * إدخال دليل واحد في READDIR
 * ────────────────────────────────────── */
typedef struct {
    uint64_t  file_size;      /* حجم الملف */
    uint32_t  mode;           /* النوع والصلاحيات */
    int64_t   mtime_sec;      /* تاريخ التعديل */
    uint16_t  name_len;       /* طول اسم الملف بالبايت */
    uint8_t   file_type;      /* 0=Unknown 1=Regular 2=Dir 3=Link 4=Other */
    /* يتبع: name_len bytes من اسم الملف (بدون null terminator) */
} VfsDirEntry;

/* ──────────────────────────────────────
 * معاملات عملية READ
 * ────────────────────────────────────── */
typedef struct {
    uint64_t  offset;         /* البداية بالبايت */
    uint32_t  length;         /* عدد البايتات المطلوب */
    uint32_t  path_len;       /* طول المسار */
    /* يتبع: path_len bytes من المسار */
} VfsReadParams;

/* ──────────────────────────────────────
 * معاملات عملية WRITE
 * ────────────────────────────────────── */
typedef struct {
    uint64_t  offset;         /* موضع الكتابة */
    uint32_t  data_len;       /* حجم البيانات للكتابة */
    uint32_t  path_len;       /* طول المسار */
    /* يتبع: path_len bytes مسار + data_len bytes بيانات */
} VfsWriteParams;

/* ──────────────────────────────────────
 * معاملات عملية RENAME
 * ────────────────────────────────────── */
typedef struct {
    uint32_t  old_path_len;   /* طول المسار القديم */
    uint32_t  new_path_len;   /* طول المسار الجديد */
    uint32_t  flags;          /* أعلام RENAME_* */
    /* يتبع: old_path_len bytes + new_path_len bytes */
} VfsRenameParams;

/* ──────────────────────────────────────
 * معلومات نظام الملفات (STATFS)
 * ────────────────────────────────────── */
typedef struct {
    uint64_t  total_bytes;    /* إجمالي سعة التخزين */
    uint64_t  free_bytes;     /* المساحة الحرة */
    uint64_t  avail_bytes;    /* المساحة المتاحة للمستخدم */
    uint32_t  total_files;    /* إجمالي inodes */
    uint32_t  free_files;     /* inodes الحرة */
    uint32_t  block_size;     /* حجم الكتلة */
    uint32_t  name_max;       /* أقصى طول لاسم ملف */
} VfsStatfsData;

/* ──────────────────────────────────────
 * معاملات CREATE / MKDIR
 * ────────────────────────────────────── */
typedef struct {
    uint32_t  mode;           /* صلاحيات Unix (e.g. 0644) */
    uint32_t  path_len;       /* طول المسار */
    /* يتبع: path_len bytes من المسار */
} VfsCreateParams;

/* ──────────────────────────────────────
 * استجابة عملية READ
 * ────────────────────────────────────── */
typedef struct {
    uint32_t  bytes_read;     /* البايتات المُعادة فعلياً */
    /* يتبع: bytes_read bytes من البيانات */
} VfsReadResponse;

/* ──────────────────────────────────────
 * استجابة عملية WRITE
 * ────────────────────────────────────── */
typedef struct {
    uint32_t  bytes_written;  /* البايتات المكتوبة فعلياً */
} VfsWriteResponse;

#pragma pack(pop)

/* ──────────────────────────────────────
 * أقصى حجم مسموح به لمسار VFS
 * ────────────────────────────────────── */
#define VFS_MAX_PATH      4096
#define VFS_MAX_NAME      255
#define VFS_REQUEST_TIMEOUT_MS  8000   /* 8 ثوان timeout لكل طلب VFS */
#define VFS_DIR_CACHE_TTL_MS    5000   /* صلاحية cache المجلدات (5 ثوان) */
#define VFS_ATTR_CACHE_TTL_MS   2000   /* صلاحية cache الخصائص (2 ثانية) */
#define VFS_CACHE_MAX_ENTRIES   512    /* حد LRU لـ cache */

/* فئات أنواع الملفات (تُستخدم في VfsDirEntry.file_type) */
#define VFS_FT_UNKNOWN    0
#define VFS_FT_REGULAR    1
#define VFS_FT_DIRECTORY  2
#define VFS_FT_SYMLINK    3
#define VFS_FT_OTHER      4

/* بناء إطار VFS RPC للإرسال */
GBytes *vlink_make_vfs_request(uint64_t req_id, VfsOpCode op,
                                const uint8_t *op_payload,
                                uint32_t op_payload_len,
                                const char *path);
GBytes *vlink_make_vfs_response(uint64_t req_id, int32_t error,
                                 const uint8_t *resp_data,
                                 uint32_t resp_data_len);

