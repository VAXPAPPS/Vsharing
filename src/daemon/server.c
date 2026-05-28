#include "server.h"
#include <libsoup/soup.h>
#include <stdio.h>
#include <gio/gio.h>
#include <unistd.h>

static SoupServer *server = NULL;
static guint32 current_notif_id = 0;

static void update_progress_notification(const char *filename, int percent, int is_download) {
    char title[128];
    char body[256];
    
    if (is_download) {
        snprintf(title, sizeof(title), "إرسال إلى الهاتف");
        if (percent < 100) {
            snprintf(body, sizeof(body), "يتم إرسال: %s\nالتقدم: %d%%", filename, percent);
        }
    } else {
        snprintf(title, sizeof(title), "استقبال من الهاتف");
        if (percent < 100) {
            snprintf(body, sizeof(body), "يتم استلام: %s\nالتقدم: %d%%", filename, percent);
        }
    }
    
    if (percent >= 100) {
        snprintf(body, sizeof(body), "✅ اكتمل النقل: %s", filename);
    }

    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!bus) {
        if (error) g_error_free(error);
        return;
    }

    GVariantBuilder *actions_builder = g_variant_builder_new(G_VARIANT_TYPE("as"));
    
    GVariantBuilder *hints_builder = g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));
    if (percent >= 0 && percent <= 100) {
        g_variant_builder_add(hints_builder, "{sv}", "value", g_variant_new_int32(percent));
    }
    
    int timeout = (percent >= 100) ? 3000 : 0;

    GVariant *result = g_dbus_connection_call_sync(
        bus,
        "org.freedesktop.Notifications",
        "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications",
        "Notify",
        g_variant_new("(susss@as@a{sv}i)",
                      "Vsharing",
                      current_notif_id,
                      "document-send",
                      title,
                      body,
                      g_variant_builder_end(actions_builder),
                      g_variant_builder_end(hints_builder),
                      timeout
        ),
        G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        &error
    );

    if (result) {
        g_variant_get(result, "(u)", &current_notif_id);
        g_variant_unref(result);
    } else if (error) {
        g_printerr("⚠️ فشل إرسال إشعار النظام (D-Bus): %s\n", error->message);
        g_error_free(error);
    }
    
    g_object_unref(bus);
    
    if (percent >= 100) {
        current_notif_id = 0;
    }
}

static const char *HTML_PAGE = 
"<!DOCTYPE html><html lang='ar' dir='rtl'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'><title>Vsharing - مشاركة سريعة</title><style>"
"body{font-family:system-ui,sans-serif;background:#1e1e2e;color:#cdd6f4;text-align:center;margin:0;padding:20px;}"
".container{max-width:400px;margin:50px auto;background:#313244;padding:30px;border-radius:20px;box-shadow:0 10px 30px rgba(0,0,0,0.5);}"
"h1{color:#89b4fa;margin-bottom:5px;font-size:2em;}p{color:#a6adc8;margin-bottom:40px;font-size:1.1em;}"
".btn{background:#89b4fa;color:#11111b;border:none;padding:15px 30px;font-size:1.2em;border-radius:50px;font-weight:bold;cursor:pointer;transition:all 0.2s;display:inline-block;width:80%;text-decoration:none;margin-bottom:15px;}"
".btn:active{transform:scale(0.95);}.btn-recv{background:#a6e3a1;display:none;}input[type=file]{display:none;}"
".icon{font-size:4em;margin-bottom:20px;}#status{margin-top:15px;font-weight:bold;color:#f9e2af;}"
"</style></head><body><div class='container'><div class='icon'>🚀</div><h1>Vsharing</h1><p>أنت متصل بالحاسوب</p>"
"<label class='btn' for='fileInput'>إرسال ملف للحاسوب</label><input type='file' id='fileInput'>"
"<div id='status'></div>"
"<a href='#' class='btn btn-recv' id='recvBtn'>⬇️ استلام ملف من الحاسوب</a>"
"<div style='background:#1e1e2e;padding:15px;border-radius:15px;margin-top:20px;'>"
"<h3 style='margin-top:0;'>مزامنة النصوص الفورية 📋</h3>"
"<p style='margin:5px 0;color:#a6adc8;font-size:0.9em;text-align:right;'>نص الحاسوب (يُحدث تلقائياً):</p>"
"<textarea id='pcClip' readonly placeholder='في انتظار نسخ نص من الحاسوب...' style='width:90%;height:60px;border-radius:10px;padding:10px;background:#313244;color:#cdd6f4;border:1px solid #45475a;margin-bottom:10px;'></textarea><br>"
"<p style='margin:5px 0;color:#a6adc8;font-size:0.9em;text-align:right;'>نص الهاتف (يُرسل تلقائياً):</p>"
"<textarea id='phoneClip' placeholder='اكتب أو انسخ هنا للإرسال...' style='width:90%;height:60px;border-radius:10px;padding:10px;background:#313244;color:#a6e3a1;border:1px solid #45475a;' oninput='autoSendText()'></textarea>"
"</div></div><script>"
"function sendFile(){"
"let file=document.getElementById('fileInput').files[0];if(!file)return;"
"document.getElementById('status').innerText='⏳ جاري الإرسال...';"
"fetch('/progress?p=0&name='+encodeURIComponent(file.name)+'&type=up');"
"let xhr=new XMLHttpRequest();xhr.open('POST','/upload?name='+encodeURIComponent(file.name),true);"
"xhr.upload.onprogress=function(e){if(e.lengthComputable){let percent=Math.round((e.loaded/e.total)*100);fetch('/progress?p='+percent+'&name='+encodeURIComponent(file.name)+'&type=up');document.getElementById('status').innerText='⏳ الإرسال: '+percent+'%';}};"
"xhr.onload=function(){if(xhr.status===200){document.getElementById('status').innerText='✅ تم الإرسال بنجاح!';fetch('/progress?p=100&name='+encodeURIComponent(file.name)+'&type=up');}else{document.getElementById('status').innerText='❌ فشل الإرسال';}};"
"xhr.send(file);}"
"document.getElementById('fileInput').addEventListener('change',sendFile);"
"function downloadFile(fname){"
"document.getElementById('recvBtn').style.display='none';document.getElementById('status').innerText='⏳ جاري الاستلام...';"
"fetch('/progress?p=0&name='+encodeURIComponent(fname)+'&type=down');"
"let xhr=new XMLHttpRequest();xhr.open('GET','/download',true);xhr.responseType='blob';"
"xhr.onprogress=function(e){if(e.lengthComputable){let percent=Math.round((e.loaded/e.total)*100);fetch('/progress?p='+percent+'&name='+encodeURIComponent(fname)+'&type=down');document.getElementById('status').innerText='⏳ الاستلام: '+percent+'%';}};"
"xhr.onload=function(){if(xhr.status===200){let a=document.createElement('a');a.href=window.URL.createObjectURL(xhr.response);a.download=fname;a.click();document.getElementById('status').innerText='✅ اكتمل الاستلام!';fetch('/progress?p=100&name='+encodeURIComponent(fname)+'&type=down');}};"
"xhr.send();}"
"let lastPhoneText=''; let lastPcText='';"
"let ws = new WebSocket('ws://' + location.host + '/ws');"
"ws.onmessage = function(e) {"
"    if (e.data.startsWith('clip:')) {"
"        let text = e.data.substring(5);"
"        if (text !== lastPcText) {"
"            lastPcText = text; document.getElementById('pcClip').value = text;"
"            if (navigator.clipboard) navigator.clipboard.writeText(text).catch(err=>{});"
"        }"
"    } else if (e.data.startsWith('file:')) {"
"        let fname = e.data.substring(5); let btn = document.getElementById('recvBtn');"
"        if (fname !== 'none') { btn.style.display = 'inline-block'; btn.innerText = '⬇️ استلام ' + fname; btn.onclick = function() { downloadFile(fname); }; }"
"        else { btn.style.display = 'none'; }"
"    }"
"};"
"function autoSendText() {"
"    let txt = document.getElementById('phoneClip').value;"
"    if (txt !== lastPhoneText) {"
"        lastPhoneText = txt; if (ws.readyState === 1) { ws.send('clip:' + txt); }"
"    }"
"}</script></body></html>";

static GPtrArray *ws_clients = NULL;
static char last_pc_clip[4096] = {0};
static char last_file[4096] = {0};

static void ws_broadcast(const char *msg) {
    if (!ws_clients) return;
    for (guint i = 0; i < ws_clients->len; i++) {
        SoupWebsocketConnection *conn = g_ptr_array_index(ws_clients, i);
        if (soup_websocket_connection_get_state(conn) == SOUP_WEBSOCKET_STATE_OPEN) {
            soup_websocket_connection_send_text(conn, msg);
        }
    }
}

static void on_ws_message(SoupWebsocketConnection *conn, gint type, GBytes *message, gpointer user_data) {
    if (type == SOUP_WEBSOCKET_DATA_TEXT) {
        gsize size;
        const gchar *data = g_bytes_get_data(message, &size);
        char *str = g_strndup(data, size);
        if (g_str_has_prefix(str, "clip:")) {
            g_file_set_contents("/tmp/vsharing_phone_clipboard.txt", str + 5, -1, NULL);
        }
        g_free(str);
    }
}

static void on_ws_closed(SoupWebsocketConnection *conn, gpointer user_data) {
    g_ptr_array_remove(ws_clients, conn);
    g_object_unref(conn);
}

static void on_websocket_connection(SoupServer *server, SoupServerMessage *msg, const char *path, SoupWebsocketConnection *connection, gpointer user_data) {
    if (!ws_clients) ws_clients = g_ptr_array_new();
    g_object_ref(connection);
    g_ptr_array_add(ws_clients, connection);
    
    g_signal_connect(connection, "message", G_CALLBACK(on_ws_message), NULL);
    g_signal_connect(connection, "closed", G_CALLBACK(on_ws_closed), NULL);
    
    // مزامنة مبدئية عند الاتصال
    if (last_pc_clip[0] != '\0') {
        char *resp = g_strdup_printf("clip:%s", last_pc_clip);
        soup_websocket_connection_send_text(connection, resp);
        g_free(resp);
    }
    if (last_file[0] != '\0') {
        char *resp = g_strdup_printf("file:%s", last_file);
        soup_websocket_connection_send_text(connection, resp);
        g_free(resp);
    }
}

static gboolean poll_tmp_files(gpointer data) {
    char *text = NULL;
    if (g_file_get_contents("/tmp/vsharing_pc_clipboard.txt", &text, NULL, NULL)) {
        if (g_strcmp0(text, last_pc_clip) != 0) {
            g_strlcpy(last_pc_clip, text, sizeof(last_pc_clip));
            char *msg = g_strdup_printf("clip:%s", text);
            ws_broadcast(msg);
            g_free(msg);
        }
        g_free(text);
    }
    
    char *file_path = NULL;
    if (g_file_get_contents("/tmp/vsharing_current_file.txt", &file_path, NULL, NULL)) {
        char *basename = g_path_get_basename(file_path);
        if (g_strcmp0(basename, last_file) != 0) {
            g_strlcpy(last_file, basename, sizeof(last_file));
            char *msg = g_strdup_printf("file:%s", basename);
            ws_broadcast(msg);
            g_free(msg);
        }
        g_free(basename);
        g_free(file_path);
    } else {
        if (last_file[0] != '\0') {
            last_file[0] = '\0';
            ws_broadcast("file:none");
        }
    }
    return G_SOURCE_CONTINUE;
}
static void server_callback(SoupServer *server, SoupServerMessage *msg, const char *path, GHashTable *query, gpointer user_data) {
    if (g_strcmp0(path, "/") == 0) {
        if (soup_server_message_get_method(msg) == SOUP_METHOD_GET) {
            SoupMessageBody *body = soup_server_message_get_response_body(msg);
            soup_message_body_append(body, SOUP_MEMORY_STATIC, HTML_PAGE, strlen(HTML_PAGE));
            soup_server_message_set_status(msg, SOUP_STATUS_OK, NULL);
        }
    } else if (g_strcmp0(path, "/progress") == 0) {
        GUri *uri = soup_server_message_get_uri(msg);
        const char *query_str = g_uri_get_query(uri);
        if (query_str) {
            GHashTable *q = g_uri_parse_params(query_str, -1, "&", G_URI_PARAMS_NONE, NULL);
            if (q) {
                const char *p_str = g_hash_table_lookup(q, "p");
                const char *name = g_hash_table_lookup(q, "name");
                const char *type = g_hash_table_lookup(q, "type");
                
                if (p_str && name) {
                    int p = atoi(p_str);
                    int is_down = (g_strcmp0(type, "down") == 0);
                    update_progress_notification(name, p, is_down);
                }
                g_hash_table_unref(q);
            }
        }
        soup_server_message_set_status(msg, SOUP_STATUS_OK, NULL);
    } else if (g_strcmp0(path, "/download") == 0) {
        char *file_path = NULL;
        if (g_file_get_contents("/tmp/vsharing_current_file.txt", &file_path, NULL, NULL)) {
            char *content;
            gsize length;
            if (g_file_get_contents(file_path, &content, &length, NULL)) {
                SoupMessageBody *body = soup_server_message_get_response_body(msg);
                soup_message_body_append(body, SOUP_MEMORY_TAKE, content, length);
                
                char *basename = g_path_get_basename(file_path);
                char disp[256];
                snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", basename);
                soup_message_headers_append(soup_server_message_get_response_headers(msg), "Content-Disposition", disp);
                g_free(basename);
                
                soup_server_message_set_status(msg, SOUP_STATUS_OK, NULL);
                unlink("/tmp/vsharing_current_file.txt");
            } else {
                soup_server_message_set_status(msg, SOUP_STATUS_NOT_FOUND, NULL);
            }
            g_free(file_path);
        } else {
            soup_server_message_set_status(msg, SOUP_STATUS_NOT_FOUND, NULL);
        }
    } else if (g_strcmp0(path, "/upload") == 0) {
        if (soup_server_message_get_method(msg) == SOUP_METHOD_POST) {
            GUri *uri = soup_server_message_get_uri(msg);
            const char *query_str = g_uri_get_query(uri);
            char *filename = "vsharing_received_file";
            if (query_str && g_str_has_prefix(query_str, "name=")) {
                filename = g_uri_unescape_string(query_str + 5, NULL);
            }
            
            // في Libsoup 3.0 نستخدم soup_message_body_flatten
            SoupMessageBody *req_body = soup_server_message_get_request_body(msg);
            GBytes *body_bytes = soup_message_body_flatten(req_body);
            gsize size;
            gconstpointer data = g_bytes_get_data(body_bytes, &size);
            
            char *save_path = g_build_filename(g_get_home_dir(), "Downloads", filename, NULL);
            g_file_set_contents(save_path, data, size, NULL);
            g_print("📥 تم استلام الملف وحفظه بنجاح: %s\n", save_path);
            
            if (filename != (char*)"vsharing_received_file") g_free(filename);
            g_free(save_path);
            g_bytes_unref(body_bytes);
            
            soup_server_message_set_status(msg, SOUP_STATUS_OK, NULL);
        }
    } else {
        soup_server_message_set_status(msg, SOUP_STATUS_NOT_FOUND, NULL);
    }
}

void vsharing_server_start(int port) {
    GError *error = NULL;
    server = soup_server_new(NULL, NULL);
    
    soup_server_add_handler(server, NULL, server_callback, NULL, NULL);
    soup_server_add_websocket_handler(server, "/ws", NULL, NULL, on_websocket_connection, NULL, NULL);
    
    // فحص دوري لنقل المتغيرات إلى قنوات الـ WebSocket
    g_timeout_add(500, poll_tmp_files, NULL);
    
    if (!soup_server_listen_all(server, port, 0, &error)) {
        g_printerr("⚠️ فشل تشغيل خادم Vsharing Web: %s\n", error->message);
        g_error_free(error);
        return;
    }
    g_print("🚀 خادم Vsharing Web جاهز لاستقبال الهواتف المحمولة على المنفذ %d...\n", port);
}

void vsharing_server_stop(void) {
    if (server) {
        soup_server_disconnect(server);
        g_object_unref(server);
    }
}
