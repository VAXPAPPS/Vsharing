#include "vlink_server.h"
#include "notification_bridge.h"
#include "vlink_protocol.h"
#include <libsoup/soup.h>
#include <stdio.h>
#include <string.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

/* ── HTML واجهة الهاتف المحسّنة ── */
static const char *MOBILE_HTML =
"<!DOCTYPE html><html lang='ar' dir='rtl'><head>"
"<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1.0'>"
"<title>VaxpLink</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:system-ui,sans-serif;background:#0d0d1a;color:#e2e8f0;min-height:100vh}"
".app{max-width:440px;margin:0 auto;padding:16px}"
".header{display:flex;align-items:center;gap:12px;padding:20px 0;border-bottom:1px solid #1e2a4a}"
".logo{width:44px;height:44px;background:linear-gradient(135deg,#6366f1,#8b5cf6);border-radius:12px;display:flex;align-items:center;justify-content:center;font-size:22px}"
".header h1{font-size:1.3rem;font-weight:700;background:linear-gradient(90deg,#818cf8,#c084fc);-webkit-background-clip:text;-webkit-text-fill-color:transparent}"
".status-pill{display:inline-flex;align-items:center;gap:6px;background:#0f2;border-radius:20px;padding:4px 10px;font-size:.75rem;color:#0a0;margin-top:4px;background:rgba(34,197,94,.15);color:#4ade80}"
".dot{width:6px;height:6px;border-radius:50%;background:#4ade80;animation:pulse 2s infinite}"
"@keyframes pulse{0%,100%{opacity:1}50%{opacity:.4}}"
".card{background:#111827;border:1px solid #1e2a4a;border-radius:16px;padding:20px;margin:16px 0;transition:.3s}"
".card:hover{border-color:#4f46e5;box-shadow:0 0 20px rgba(99,102,241,.15)}"
".card-title{font-size:.8rem;color:#94a3b8;text-transform:uppercase;letter-spacing:.08em;margin-bottom:14px;display:flex;align-items:center;gap:6px}"
".btn{width:100%;padding:14px;border:none;border-radius:12px;font-size:1rem;font-weight:600;cursor:pointer;transition:.2s;margin-top:8px}"
".btn-primary{background:linear-gradient(135deg,#6366f1,#8b5cf6);color:#fff}"
".btn-primary:active{transform:scale(.97)}"
".btn-secondary{background:#1e2a4a;color:#94a3b8}"
"label.btn-primary{display:block;text-align:center;line-height:1.5}"
"input[type=file]{display:none}"
"textarea{width:100%;background:#0d0d1a;border:1px solid #1e2a4a;border-radius:10px;color:#e2e8f0;padding:12px;font-size:.9rem;resize:none;margin-top:8px;transition:.2s}"
"textarea:focus{outline:none;border-color:#6366f1}"
".progress-bar{width:100%;height:6px;background:#1e2a4a;border-radius:3px;overflow:hidden;margin-top:10px;display:none}"
".progress-fill{height:100%;background:linear-gradient(90deg,#6366f1,#c084fc);width:0%;transition:width .3s;border-radius:3px}"
".status-msg{font-size:.85rem;color:#94a3b8;margin-top:8px;min-height:20px}"
".status-msg.ok{color:#4ade80} .status-msg.err{color:#f87171}"
"</style></head><body><div class='app'>"
"<div class='header'><div class='logo'>⚡</div><div><h1>VaxpLink</h1>"
"<div class='status-pill'><span class='dot'></span> متصل بـ Vaxp</div></div></div>"

/* بطاقة نقل الملفات */
"<div class='card'><div class='card-title'>📁 نقل الملفات</div>"
"<label class='btn btn-primary' for='fi'>📤 إرسال ملف للحاسوب</label>"
"<input type='file' id='fi'><div class='progress-bar' id='pb'><div class='progress-fill' id='pf'></div></div>"
"<div class='status-msg' id='fst'></div>"
"<button class='btn btn-secondary' id='dlbtn' style='display:none'>⬇️ استلام ملف من الحاسوب</button>"
"</div>"

/* بطاقة الحافظة */
"<div class='card'><div class='card-title'>📋 مزامنة الحافظة</div>"
"<textarea id='pcClip' rows='3' readonly placeholder='نص الحاسوب يظهر هنا تلقائياً...'></textarea>"
"<textarea id='phClip' rows='3' placeholder='اكتب أو الصق هنا للإرسال...' oninput='sendClip()'></textarea>"
"<div class='status-msg' id='cst'></div>"
"</div></div>"

"<script>"
"let ws,lastPc='',lastPh='',debTimer;"

/* اتصال WebSocket */
"function connect(){"
"ws=new WebSocket('ws://'+location.host+'/ws');"
"ws.onopen=()=>{};"
"ws.onclose=()=>setTimeout(connect,3000);"
"ws.onmessage=e=>{"
"  let d=e.data;"
"  if(d.startsWith('clip:')){let t=d.slice(5);if(t!==lastPc){lastPc=t;document.getElementById('pcClip').value=t;if(navigator.clipboard)navigator.clipboard.writeText(t).catch(()=>{});}}"
"  else if(d.startsWith('file:')){let n=d.slice(5);let b=document.getElementById('dlbtn');"
"    if(n!=='none'){b.style.display='block';b.onclick=()=>dlFile(n);}else b.style.display='none';}"
"};"
"}connect();"

/* إرسال ملف */
"document.getElementById('fi').onchange=function(){"
"let f=this.files[0];if(!f)return;"
"let pb=document.getElementById('pb'),pf=document.getElementById('pf'),st=document.getElementById('fst');"
"pb.style.display='block';st.textContent='⏳ جاري الإرسال...';"
"let xhr=new XMLHttpRequest();xhr.open('POST','/upload?name='+encodeURIComponent(f.name));"
"xhr.upload.onprogress=e=>{if(e.lengthComputable){let p=Math.round(e.loaded/e.total*100);pf.style.width=p+'%';st.textContent='⏳ '+p+'%';fetch('/progress?p='+p+'&name='+encodeURIComponent(f.name)+'&type=up');}};"
"xhr.onload=()=>{if(xhr.status===200){st.className='status-msg ok';st.textContent='✅ تم الإرسال!';pf.style.width='100%';fetch('/progress?p=100&name='+encodeURIComponent(f.name)+'&type=up');}else{st.className='status-msg err';st.textContent='❌ فشل الإرسال';}};"
"xhr.send(f);};"

/* استلام ملف */
"function dlFile(n){"
"let st=document.getElementById('fst');st.textContent='⏳ جاري الاستلام...';"
"let xhr=new XMLHttpRequest();xhr.open('GET','/download');xhr.responseType='blob';"
"xhr.onload=()=>{if(xhr.status===200){let a=document.createElement('a');a.href=URL.createObjectURL(xhr.response);a.download=n;a.click();st.className='status-msg ok';st.textContent='✅ تم الاستلام!';}};"
"xhr.send();}"

/* مزامنة الحافظة */
"function sendClip(){"
"clearTimeout(debTimer);debTimer=setTimeout(()=>{"
"  let t=document.getElementById('phClip').value;"
"  if(t!==lastPh&&ws&&ws.readyState===1){lastPh=t;ws.send('clip:'+t);document.getElementById('cst').textContent='✅ تم الإرسال';}"
"},300);}"
"</script></body></html>";

/* ── أدوات الشبكة ── */
static char *get_local_ip(void) {
    struct ifaddrs *ifaddr, *ifa;
    static char ip[NI_MAXHOST] = "127.0.0.1";
    if (getifaddrs(&ifaddr) == -1) return ip;
    for (ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (strcmp(ifa->ifa_name, "lo") == 0) continue;
        strcpy(ip, inet_ntoa(((struct sockaddr_in *)ifa->ifa_addr)->sin_addr));
        break;
    }
    freeifaddrs(ifaddr);
    return ip;
}

/* ── إشعار D-Bus مع شريط تقدم ── */
static guint32 g_notif_id = 0;
static void send_progress_notif(const char *filename, int pct, gboolean is_upload) {
    char title[128], body[256];
    snprintf(title, sizeof(title), is_upload ? "إرسال إلى الهاتف" : "استلام من الهاتف");
    if (pct >= 100)
        snprintf(body, sizeof(body), "✅ اكتمل: %s", filename);
    else
        snprintf(body, sizeof(body), "%s\nالتقدم: %d%%", filename, pct);

    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!bus) return;
    GVariantBuilder *ab = g_variant_builder_new(G_VARIANT_TYPE("as"));
    GVariantBuilder *hb = g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));
    if (pct >= 0 && pct <= 100)
        g_variant_builder_add(hb, "{sv}", "value", g_variant_new_int32(pct));
    GVariant *r = g_dbus_connection_call_sync(bus,
        "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", "Notify",
        g_variant_new("(susss@as@a{sv}i)", "VaxpLink", g_notif_id,
                      "document-send", title, body,
                      g_variant_builder_end(ab), g_variant_builder_end(hb),
                      pct >= 100 ? 3000 : 0),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
    if (r) { g_variant_get(r, "(u)", &g_notif_id); g_variant_unref(r); }
    if (pct >= 100) g_notif_id = 0;
    g_object_unref(bus);
}

/* ── حالة مشتركة بين الـ callbacks ── */
static GPtrArray *g_clients      = NULL;
static char g_last_pc_clip[8192] = {0};
static char g_last_file[4096]    = {0};

/* ── بث رسالة نصية لجميع عملاء WebSocket ── */
static void ws_broadcast(const char *msg) {
    if (!g_clients) return;
    for (guint i = 0; i < g_clients->len; i++) {
        SoupWebsocketConnection *c = g_ptr_array_index(g_clients, i);
        if (soup_websocket_connection_get_state(c) == SOUP_WEBSOCKET_STATE_OPEN)
            soup_websocket_connection_send_text(c, msg);
    }
}

/* ── بث رسالة ثنائية VLink لجميع العملاء ── */
static void ws_broadcast_binary(GBytes *frame) {
    if (!g_clients || !frame) return;
    gsize sz;
    gconstpointer data = g_bytes_get_data(frame, &sz);
    for (guint i = 0; i < g_clients->len; i++) {
        SoupWebsocketConnection *c = g_ptr_array_index(g_clients, i);
        if (soup_websocket_connection_get_state(c) == SOUP_WEBSOCKET_STATE_OPEN)
            soup_websocket_connection_send_binary(c, data, sz);
    }
}

/* ── Callback: عند نقر المستخدم على إجراء إشعار → نرسل VLink للهاتف ── */
static void on_notif_action(uint64_t notif_id, uint8_t action_index,
                             const char *action_text, gpointer user_data)
{
    (void)user_data;

    /* بناء payload VLINK_NOTIF_ACTION يدوياً (notif_id + action_index) */
    uint8_t payload[16];
    memset(payload, 0, sizeof(payload));
    /* notif_id: 8 bytes little-endian */
    for (int i = 0; i < 8; i++)
        payload[i] = (uint8_t)((notif_id >> (i * 8)) & 0xFF);
    payload[8] = action_index;
    /* action_text: اختياري، ندرجه كـ null-terminated string (حتى 7 chars) */
    if (action_text)
        g_strlcpy((char *)payload + 9, action_text, 7);

    static uint32_t seq = 1000;
    GBytes *frame = vlink_build_frame(VLINK_NOTIF_ACTION,
                                      VLINK_FLAG_PRIORITY,
                                      seq++,
                                      payload, sizeof(payload));
    if (frame) {
        ws_broadcast_binary(frame);
        g_bytes_unref(frame);
        g_print("[VLink] 📤 Sent VLINK_NOTIF_ACTION (id=%.16" G_GUINT64_FORMAT ", action=%u)\n",
                notif_id, action_index);
    }
}

/* ── معالجة رسائل WebSocket الواردة ── */
static void on_ws_message(SoupWebsocketConnection *conn, gint type,
                           GBytes *message, gpointer user_data) {
    (void)conn; (void)user_data;
    gsize sz;
    const uint8_t *d = g_bytes_get_data(message, &sz);

    /* ═══ رسالة ثنائية VLink ═══ */
    if (type == SOUP_WEBSOCKET_DATA_BINARY) {
        VLinkFrame  hdr;
        const uint8_t *payload = NULL;
        if (!vlink_parse_frame(d, sz, &hdr, &payload)) {
            g_print("[VLink] ⚠️  Invalid binary frame (len=%zu)\n", sz);
            return;
        }

        switch ((VLinkMsgType)hdr.type) {

        /* ── الإشعارات ── */
        case VLINK_NOTIF_SHOW:
            if (hdr.payload_len >= sizeof(VLinkNotifPayload)) {
                const VLinkNotifPayload *np = (const VLinkNotifPayload *)payload;
                notif_bridge_show(np);
            } else {
                g_print("[VLink] ⚠️  NOTIF_SHOW: payload too small (%u bytes)\n",
                        hdr.payload_len);
            }
            break;

        case VLINK_NOTIF_DISMISS: {
            if (hdr.payload_len >= 8) {
                uint64_t notif_id = 0;
                for (int i = 0; i < 8; i++)
                    notif_id |= ((uint64_t)payload[i]) << (i * 8);
                notif_bridge_dismiss(notif_id);
            }
            break;
        }

        case VLINK_NOTIF_UPDATE:
            /* نعيد استخدام SHOW للتحديث (replace_id موجود في السجل) */
            if (hdr.payload_len >= sizeof(VLinkNotifPayload))
                notif_bridge_show((const VLinkNotifPayload *)payload);
            break;

        /* ── نبضات القلب ── */
        case VLINK_PING: {
            static uint32_t pong_seq = 5000;
            GBytes *pong = vlink_make_pong(pong_seq++);
            if (pong) {
                gsize ps;
                gconstpointer pd = g_bytes_get_data(pong, &ps);
                soup_websocket_connection_send_binary(conn, pd, ps);
                g_bytes_unref(pong);
            }
            break;
        }

        default:
            g_print("[VLink] 🔹 Binary msg type=0x%02X len=%u\n",
                    hdr.type, hdr.payload_len);
            break;
        }
        return;
    }

    /* ═══ رسالة نصية (Legacy HTTP WebSocket) ═══ */
    if (type != SOUP_WEBSOCKET_DATA_TEXT) return;
    char *str = g_strndup((const char *)d, sz);
    if (g_str_has_prefix(str, "clip:")) {
        const char *text = str + 5;
        g_file_set_contents("/tmp/vsharing_phone_clipboard.txt", text, -1, NULL);
        g_print("[VLink] 📥 Clipboard from device: %.60s\n", text);
    }
    g_free(str);
}

static void on_ws_closed(SoupWebsocketConnection *conn, gpointer user_data) {
    (void)user_data;
    g_ptr_array_remove(g_clients, conn);
    g_object_unref(conn);
    g_print("[VLink] Device disconnected. Active: %u\n", g_clients->len);
}

static void on_ws_connected(SoupServer *srv, SoupServerMessage *msg,
                              const char *path, SoupWebsocketConnection *conn,
                              gpointer user_data) {
    (void)srv; (void)msg; (void)path; (void)user_data;
    if (!g_clients) g_clients = g_ptr_array_new();
    g_object_ref(conn);
    g_ptr_array_add(g_clients, conn);
    g_signal_connect(conn, "message", G_CALLBACK(on_ws_message), NULL);
    g_signal_connect(conn, "closed",  G_CALLBACK(on_ws_closed),  NULL);
    /* مزامنة مبدئية */
    if (g_last_pc_clip[0]) {
        char *m = g_strdup_printf("clip:%s", g_last_pc_clip);
        soup_websocket_connection_send_text(conn, m);
        g_free(m);
    }
    if (g_last_file[0]) {
        char *m = g_strdup_printf("file:%s", g_last_file);
        soup_websocket_connection_send_text(conn, m);
        g_free(m);
    }
    g_print("[VLink] ✅ New device connected. Active: %u\n", g_clients->len);
}

/* ── فحص دوري لمتغيرات النظام وبثها ── */
static gboolean poll_shared_state(gpointer data) {
    (void)data;
    /* الحافظة */
    char *clip = NULL;
    if (g_file_get_contents("/tmp/vsharing_pc_clipboard.txt", &clip, NULL, NULL)) {
        if (g_strcmp0(clip, g_last_pc_clip) != 0) {
            g_strlcpy(g_last_pc_clip, clip, sizeof(g_last_pc_clip));
            char *m = g_strdup_printf("clip:%s", clip);
            ws_broadcast(m);
            g_free(m);
        }
        g_free(clip);
    }
    /* الملف الجاهز */
    char *fpath = NULL;
    if (g_file_get_contents("/tmp/vsharing_current_file.txt", &fpath, NULL, NULL)) {
        char *base = g_path_get_basename(g_strchomp(fpath));
        if (g_strcmp0(base, g_last_file) != 0) {
            g_strlcpy(g_last_file, base, sizeof(g_last_file));
            char *m = g_strdup_printf("file:%s", base);
            ws_broadcast(m);
            g_free(m);
        }
        g_free(base);
        g_free(fpath);
    } else if (g_last_file[0]) {
        g_last_file[0] = '\0';
        ws_broadcast("file:none");
    }
    return G_SOURCE_CONTINUE;
}

/* ── HTTP Router الرئيسي ── */
static void http_handler(SoupServer *srv, SoupServerMessage *msg,
                          const char *path, GHashTable *query,
                          gpointer user_data) {
    (void)srv; (void)query; (void)user_data;
    const char *method = soup_server_message_get_method(msg);

    /* GET / → واجهة الهاتف */
    if (g_strcmp0(path, "/") == 0 && g_strcmp0(method, "GET") == 0) {
        SoupMessageBody *body = soup_server_message_get_response_body(msg);
        soup_message_body_append(body, SOUP_MEMORY_STATIC,
                                 MOBILE_HTML, strlen(MOBILE_HTML));
        soup_server_message_set_status(msg, SOUP_STATUS_OK, NULL);
        return;
    }

    /* GET /progress?p=N&name=X&type=up|down */
    if (g_strcmp0(path, "/progress") == 0) {
        GUri *uri = soup_server_message_get_uri(msg);
        const char *qs = g_uri_get_query(uri);
        if (qs) {
            GHashTable *q = g_uri_parse_params(qs, -1, "&", G_URI_PARAMS_NONE, NULL);
            if (q) {
                const char *p    = g_hash_table_lookup(q, "p");
                const char *name = g_hash_table_lookup(q, "name");
                const char *type = g_hash_table_lookup(q, "type");
                if (p && name) {
                    gboolean up = (g_strcmp0(type, "up") == 0);
                    send_progress_notif(name, atoi(p), up);
                }
                g_hash_table_unref(q);
            }
        }
        soup_server_message_set_status(msg, SOUP_STATUS_OK, NULL);
        return;
    }

    /* POST /upload?name=X */
    if (g_strcmp0(path, "/upload") == 0 && g_strcmp0(method, "POST") == 0) {
        GUri *uri = soup_server_message_get_uri(msg);
        const char *qs = g_uri_get_query(uri);
        char *fname = g_strdup("vsharing_file");
        if (qs && g_str_has_prefix(qs, "name=")) {
            g_free(fname);
            fname = g_uri_unescape_string(qs + 5, NULL);
        }
        SoupMessageBody *req = soup_server_message_get_request_body(msg);
        GBytes *bytes = soup_message_body_flatten(req);
        gsize sz;
        gconstpointer data = g_bytes_get_data(bytes, &sz);
        char *save = g_build_filename(g_get_home_dir(), "Downloads", fname, NULL);
        GError *err = NULL;
        if (!g_file_set_contents(save, data, (gssize)sz, &err)) {
            g_printerr("[VLink] Save failed: %s\n", err ? err->message : "?");
            if (err) g_error_free(err);
            soup_server_message_set_status(msg, SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL);
        } else {
            g_print("[VLink] 📥 File saved: %s (%zu bytes)\n", save, sz);
            soup_server_message_set_status(msg, SOUP_STATUS_OK, NULL);
        }
        g_free(save); g_free(fname);
        g_bytes_unref(bytes);
        return;
    }

    /* GET /download */
    if (g_strcmp0(path, "/download") == 0 && g_strcmp0(method, "GET") == 0) {
        char *fpath = NULL;
        if (!g_file_get_contents("/tmp/vsharing_current_file.txt", &fpath, NULL, NULL)) {
            soup_server_message_set_status(msg, SOUP_STATUS_NOT_FOUND, NULL);
            return;
        }
        g_strchomp(fpath);
        char *content = NULL; gsize length;
        if (!g_file_get_contents(fpath, &content, &length, NULL)) {
            g_free(fpath);
            soup_server_message_set_status(msg, SOUP_STATUS_NOT_FOUND, NULL);
            return;
        }
        char *base = g_path_get_basename(fpath);
        char disp[512];
        snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", base);
        SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
        soup_message_headers_append(hdrs, "Content-Disposition", disp);
        soup_message_headers_append(hdrs, "Content-Type", "application/octet-stream");
        SoupMessageBody *body = soup_server_message_get_response_body(msg);
        soup_message_body_append(body, SOUP_MEMORY_TAKE, content, length);
        soup_server_message_set_status(msg, SOUP_STATUS_OK, NULL);
        g_remove("/tmp/vsharing_current_file.txt");
        g_free(base); g_free(fpath);
        return;
    }

    /* GET /api/status → JSON */
    if (g_strcmp0(path, "/api/status") == 0) {
        char *ip = get_local_ip();
        char *json = g_strdup_printf(
            "{\"name\":\"Vaxp Desktop\",\"ip\":\"%s\","
            "\"version\":\"2.0\",\"clients\":%u,"
            "\"clipboard\":%s,\"protocol\":\"VLink/2\"}",
            ip, g_clients ? g_clients->len : 0u,
            g_last_pc_clip[0] ? "true" : "false");
        SoupMessageBody *body = soup_server_message_get_response_body(msg);
        soup_message_body_append(body, SOUP_MEMORY_TAKE, json, strlen(json));
        SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
        soup_message_headers_append(hdrs, "Content-Type", "application/json");
        soup_server_message_set_status(msg, SOUP_STATUS_OK, NULL);
        return;
    }

    soup_server_message_set_status(msg, SOUP_STATUS_NOT_FOUND, NULL);
}

/* ── واجهة عامة ── */
static SoupServer *g_soup_server = NULL;

void vlink_server_start(int port) {
    GError *err = NULL;

    /* ── المرحلة 2: تهيئة جسر الإشعارات ── */
    notif_bridge_init(on_notif_action, NULL);

    g_soup_server = soup_server_new(NULL, NULL);
    soup_server_add_handler(g_soup_server, NULL, http_handler, NULL, NULL);
    soup_server_add_websocket_handler(g_soup_server, "/ws", NULL, NULL,
                                      on_ws_connected, NULL, NULL);
    /* فحص دوري كل 300ms */
    g_timeout_add(300, poll_shared_state, NULL);
    if (!soup_server_listen_all(g_soup_server, port, 0, &err)) {
        g_printerr("[VLink] ❌ Failed to start server: %s\n",
                   err ? err->message : "unknown");
        if (err) g_error_free(err);
        return;
    }
    char *ip = get_local_ip();
    g_print("[VLink] 🚀 Server ready on http://%s:%d\n", ip, port);
    g_print("[VLink] 📡 WebSocket on ws://%s:%d/ws\n", ip, port);
    g_print("[VLink] 🔔 Notification bridge active (Phase 2)\n");
}

void vlink_server_stop(void) {
    notif_bridge_cleanup();
    if (g_soup_server) {
        soup_server_disconnect(g_soup_server);
        g_object_unref(g_soup_server);
        g_soup_server = NULL;
    }
}

gchar *vlink_server_get_qr_url(int port) {
    char *ip = get_local_ip();
    return g_strdup_printf("http://%s:%d", ip, port);
}

guint vlink_server_get_client_count(void) {
    return g_clients ? g_clients->len : 0;
}
