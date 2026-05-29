# 🗺️ Vsharing → VaxpLink Ecosystem Hub
## وثيقة حالة التطوير — دليل الاستكمال في محادثة جديدة

> **آخر تحديث:** 2026-05-29  
> **الإصدار الحالي:** v4.0.0 (المرحلة 4 مكتملة ✅)  
> **الهدف النهائي:** تحويل Vsharing من تطبيق مشاركة بسيط إلى العصب الرئيسي لنظام Vaxp البيئي

---

## 📋 السياق الكامل للمشروع

### ما هو Vsharing؟
تطبيق C/GTK4 يعمل على Linux يتيح مشاركة الملفات ومزامنة الحافظة بين الحاسوب (Vaxp OS) والهاتف عبر الشبكة المحلية Wi-Fi. يتكون من:
- **`vsharingd`** — daemon يعمل في الخلفية (HTTP server + WebSocket + mDNS)
- **`vsharing-ui`** — واجهة GTK4 للمستخدم

### بنية المشروع الحالية
```
Vsharing/
├── meson.build              ← نظام البناء (v3.0.0)
├── DEVELOPMENT_STATUS.md
├── data/
│   ├── style.css            ← ✅ محدَّث (إشعارات + شاشة)
│   └── vsharing-send.desktop
└── src/
    ├── daemon/
    │   ├── main.c
    │   ├── discovery.{c,h}
    │   ├── vlink_protocol.{c,h}
    │   ├── vlink_server.{c,h}    ← ✅ محدَّث (SCREEN + INPUT handlers)
    │   ├── auth_manager.{c,h}
    │   ├── clipboard_engine.{c,h}
    │   ├── notification_bridge.{c,h} ← ✅ (مرحلة 2)
    │   ├── input_controller.{c,h}    ← ✅ (مرحلة 3)
    │   ├── screen_mirror.{c,h}       ← ✅ (مرحلة 3)
    │   └── vfs_bridge.{c,h}          ← ✅ جديد (مرحلة 4)
    └── ui/
        ├── main.c
        ├── window.{c,h}          ← ✅ محدَّث (زر مشاركة الشاشة)
        └── screen_viewer.{c,h}   ← ✅ جديد (مرحلة 3)
```

### أوامر البناء
```bash
cd /home/x/Work/Vsharing
meson setup build
meson compile -C build

# التشغيل:
./build/vsharingd          # الـ daemon
./build/vsharing-ui        # الواجهة
```

### التبعيات المطلوبة
- `gtk4`, `gio-2.0`, `libsoup-3.0`
- `avahi-client`, `avahi-glib`
- `libqrencode`
- `wl-clipboard` أو `xclip` (في الـ PATH)

---

## ✅ المرحلة 1 — بروتوكول VLink الأساسي (مكتملة 100%)

### ما تم إنجازه:

#### `vlink_protocol.h` + `vlink_protocol.c`
- بروتوكول ثنائي بـ Header 20-byte: `magic[4] + version + type + flags + payload_len + seq + checksum`
- **25+ نوع رسالة** مُعرَّف: Clipboard، Notifications، File Transfer، Screen Mirror، Input Control، Auth، VFS
- CRC32 للتحقق من سلامة البيانات
- دوال بناء رسائل جاهزة: `vlink_make_ping`, `vlink_make_clipboard_text`, `vlink_make_notif`...
- دعم الضغط (LZ4 flag) والتشفير (AES flag) والتجزئة (Fragment flags)

#### `auth_manager.h` + `auth_manager.c`
- UUID v4 للأجهزة
- نظام Challenge-Response (32-byte random nonce + HMAC-SHA256)
- تخزين الأجهزة الموثوقة في `~/.config/vsharing/trusted_devices.json`
- صلاحيات قابلة للضبط لكل جهاز (Clipboard/Notifications/Files/Screen/Input)
- إلغاء الثقة بجهاز

#### `clipboard_engine.h` + `clipboard_engine.c`
- **GFileMonitor** بدلاً من Polling → تأخير `500ms → <20ms`
- Debounce 50ms لمنع حلقة ping-pong
- اكتشاف تلقائي لـ `wl-copy`/`xclip`/`xdotool`
- يراقب ملفين: `/tmp/vsharing_pc_clipboard.txt` و `/tmp/vsharing_phone_clipboard.txt`

#### `vlink_server.h` + `vlink_server.c`
- HTTP + WebSocket على منفذ 5000
- واجهة ويب للهاتف محسّنة (تصميم dark modern)
- `/api/status` نقطة JSON للاستعلام عن حالة الخادم
- دعم رفع/تنزيل الملفات مع إشعارات D-Bus تتبع التقدم
- Broadcast لجميع العملاء المتصلين

#### `main.c` (محدَّث)
- Graceful shutdown على SIGINT/SIGTERM/SIGHUP
- تهيئة موحّدة لجميع المكونات
- طباعة معلومات الاتصال عند البدء

---

## ✅ المرحلة 2 — مزامنة الإشعارات الكاملة (مكتملة 100%)

### الهدف
إشعارات الهاتف (WhatsApp، مكالمات، تنبيهات) تظهر على شاشة الحاسوب. الإجراءات (رد، إلغاء) تنعكس على الهاتف.

### الملفات الجديدة المطلوبة

#### `src/daemon/notification_bridge.h` + `.c`
```c
// الوظيفة: استقبال إشعارات الهاتف عبر VLink وبثها عبر D-Bus للنظام
// + استقبال إجراءات المستخدم على الإشعار وإرسالها للهاتف

typedef struct {
    uint64_t notif_id;
    char     app_name[64];
    char     title[128];
    char     body[512];
    char     icon_data[8192]; // PNG base64
    uint8_t  category;        // 0=msg, 1=call, 2=alarm, 3=media
    uint8_t  priority;
    char     actions[4][64];  // "رد"، "إغلاق"...
    uint8_t  action_count;
} VLinkNotification;

// الدوال:
void notif_bridge_init(void);
void notif_bridge_show(const VLinkNotification *notif);
void notif_bridge_dismiss(uint64_t notif_id);
// Callback عند الضغط على action → يُرسل VLINK_NOTIF_ACTION للهاتف
```

#### تعديل `vlink_server.c`
- في `on_ws_message()`: معالجة `VLINK_NOTIF_SHOW` و `VLINK_NOTIF_DISMISS`
- إرسال `VLINK_NOTIF_ACTION` عند تفاعل المستخدم

#### تعديل `src/ui/window.c`
- إضافة قسم "مركز الإشعارات" في الواجهة
- عرض قائمة إشعارات الهاتف الحالية مع أزرار الإجراءات

### خطوات التنفيذ بالترتيب
1. أنشئ `src/daemon/notification_bridge.c` مع D-Bus Notify
2. أضف معالجة `VLINK_NOTIF_*` في `vlink_server.c` → `on_ws_message()`
3. أضف قسم الإشعارات في `src/ui/window.c`
4. حدّث `src/daemon/main.c` لاستدعاء `notif_bridge_init()`
5. حدّث `meson.build` بإضافة `src/daemon/notification_bridge.c`

---

## ✅ المرحلة 3 — مشاركة الشاشة والتحكم عن بُعد (مكتملة 100%)

### الهدف
- بث شاشة الهاتف على الحاسوب بتأخير < 50ms
- التحكم بالهاتف (لمس، كيبورد) من الحاسوب

### الملفات الجديدة المطلوبة

#### `src/daemon/screen_mirror.h` + `.c`
```c
// التقاط شاشة الحاسوب (wlr-screencopy أو XShm)
// ترميز H.264 عبر FFmpeg libavcodec
// إرسال عبر UDP/RTP على منفذ 5001

typedef struct {
    int width, height, fps;
    int bitrate;          // bits/s
    gboolean use_hw;      // VA-API / NVENC
    AVCodecContext *enc;
    int udp_sock;
} ScreenMirrorCtx;

void screen_mirror_start(const char *client_ip, int stream_port);
void screen_mirror_stop(void);
void screen_mirror_set_config(int fps, int bitrate);
```

#### `src/daemon/input_controller.h` + `.c`
```c
// حقن أحداث اللمس من الهاتف في uinput للحاسوب
#include <linux/uinput.h>

void input_ctrl_init(void);
void input_ctrl_inject_touch(float x_norm, float y_norm, uint8_t action);
void input_ctrl_inject_key(uint32_t keycode, uint8_t action);
void input_ctrl_cleanup(void);
```

#### `src/ui/screen_viewer.h` + `.c`
```c
// GtkGLArea أو GtkDrawingArea لعرض الإطارات المستلمة
// Thread منفصل لاستقبال UDP
// فك تشفير H.264 عبر FFmpeg libavcodec

GtkWidget *screen_viewer_new(void);
void screen_viewer_start(const char *server_ip, int port);
void screen_viewer_stop(void);
```

### التبعيات الإضافية للـ meson.build
```
ffmpeg_dep = dependency('libavcodec')
ffmpeg_avutil = dependency('libavutil')
```

### خطوات التنفيذ بالترتيب
1. أنشئ `src/daemon/input_controller.c` (أبسط — uinput فقط)
2. أنشئ `src/daemon/screen_mirror.c` (FFmpeg H.264 encoder)
3. أضف معالجة `VLINK_SCREEN_START/STOP/CONFIG` في `vlink_server.c`
4. أضف معالجة `VLINK_INPUT_TOUCH/KEY` في `vlink_server.c` → `input_ctrl_inject_*`
5. أنشئ `src/ui/screen_viewer.c` (GTK4 widget)
6. أضف زر "مشاركة الشاشة" في `src/ui/window.c`
7. حدّث `meson.build` بالتبعيات والملفات الجديدة

---

## ✅ المرحلة 4 — نظام الملفات الافتراضي VFS Bridge (مكتملة 100%)

### الهدف
ملفات الهاتف تظهر كمجلد محلي في `~/.vsharing/devices/<name>/` عبر FUSE.

### الملفات الجديدة المطلوبة

#### `src/daemon/vfs_bridge.h` + `.c`
```c
#include <fuse3/fuse.h>

// نقاط FUSE:
static int vfs_readdir(const char *path, void *buf, ...);
static int vfs_read(const char *path, char *buf, size_t size, off_t offset, ...);
static int vfs_write(const char *path, const char *buf, ...);
static int vfs_getattr(const char *path, struct stat *stbuf, ...);

// تنفيذ:
// كل عملية FUSE → إرسال VLINK_FILE_LIST_REQ أو VLINK_FILE_READ_REQ عبر VLink
// انتظار VLINK_FILE_LIST_RESP أو VLINK_FILE_READ_RESP
// إعادة البيانات لـ FUSE

void vfs_bridge_mount(const char *device_id, const char *mountpoint);
void vfs_bridge_unmount(const char *mountpoint);
```

### التبعيات الإضافية
```
fuse_dep = dependency('fuse3')
```

### خطوات التنفيذ بالترتيب
1. أضف معالجة `VLINK_FILE_LIST_REQ/RESP` و `VLINK_FILE_READ_REQ/RESP` في `vlink_server.c`
2. أنشئ `src/daemon/vfs_bridge.c` مع FUSE3
3. عند اتصال جهاز → `vfs_bridge_mount()` في `vlink_server.c → on_ws_connected()`
4. أضف متصفح ملفات بسيط في `src/ui/window.c` يعرض `~/.vsharing/devices/`
5. حدّث `meson.build`

---

## ⏳ المرحلة 5 — التشفير الكامل TLS + SRTP (موازية)

### الهدف
تشفير قناة التحكم بـ TLS 1.3 وقناة البث بـ DTLS/SRTP.

### ما يحتاج تعديلاً
- `vlink_server.c`: إضافة `SOUP_SERVER_TLS_CERTIFICATE` لـ SoupServer
- `screen_mirror.c`: استخدام SRTP بدلاً من RTP خام
- `auth_manager.c`: توليد شهادة TLS ذاتية عند أول تشغيل

### التبعيات الإضافية
```
gnutls_dep = dependency('gnutls')  # أو openssl
```

---

## 🔌 تكامل Vaxp Ecosystem (مستقبلي)

| التطبيق | كيفية التكامل |
|---|---|
| **AetherFiles** | يقرأ من `~/.vsharing/devices/` (FUSE mount من المرحلة 4) |
| **AetherNotif** | يستقبل إشعارات الهاتف عبر D-Bus (المرحلة 2) |
| **VCamera** | يُضاف `VLINK_CAP_CAMERA` لاستخدام كاميرا الهاتف كـ webcam |
| **SonicWave** | `VLINK_CAP_AUDIO` لمشاركة الصوت |

---

## 📌 تعليمات للمحادثة الجديدة

### الجملة الافتتاحية المقترحة:
```
هذا مشروع Vsharing في /home/x/Work/Vsharing
اقرأ ملف DEVELOPMENT_STATUS.md أولاً ثم نكمل من المرحلة 2
```

### أولويات الملفات للقراءة في المحادثة الجديدة:
1. `DEVELOPMENT_STATUS.md` ← هذا الملف
2. `src/daemon/vlink_protocol.h` ← لفهم أنواع الرسائل
3. `src/daemon/vlink_server.c` ← لفهم نقطة إضافة المعالجات
4. `src/daemon/main.c` ← لفهم تسلسل التهيئة
5. `meson.build` ← لإضافة الملفات الجديدة

### قاعدة مهمة
> كل مرحلة تبدأ بإنشاء ملفات الـ daemon أولاً (المعالجة)، ثم ملفات الـ UI (العرض)، ثم تحديث `main.c` و `meson.build` وبناء واختبار.

---

## 🔧 ملاحظات تقنية مهمة

### البروتوكول الحالي (مختلط)
الخادم حالياً يدعم **قناتين متوازيتين**:
1. **VLink Binary** — عبر WebSocket (`/ws`) للعملاء المتقدمين
2. **HTTP Legacy** — للهاتف عبر المتصفح (لا يحتاج تطبيق)

### ملفات IPC المشتركة (مؤقتة)
| الملف | الاستخدام |
|---|---|
| `/tmp/vsharing_pc_clipboard.txt` | الحاسوب → الهاتف |
| `/tmp/vsharing_phone_clipboard.txt` | الهاتف → الحاسوب |
| `/tmp/vsharing_current_file.txt` | مسار الملف الجاهز للتنزيل |

> **ملاحظة:** هذه الملفات ستُستبدل بـ IPC Socket في مرحلة لاحقة.

### اصطلاحات التسمية
- كل دوال الـ daemon تبدأ بـ `vlink_` أو باسم الوحدة (`notif_`, `screen_`, `vfs_`)
- الـ structs تنتهي بـ `Ctx` للسياق و`Payload` لبيانات البروتوكول
- كل ملف `.c` يبدأ بـ `#include` الخاص به ثم `<gio/gio.h>` ثم باقي المكتبات
