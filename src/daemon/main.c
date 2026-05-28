/*
 * vsharingd — VLink Daemon v2
 * العصب الرئيسي لنظام Vaxp البيئي
 */

#include <gio/gio.h>
#include <stdio.h>
#include <signal.h>
#include "discovery.h"
#include "vlink_server.h"
#include "vlink_protocol.h"
#include "auth_manager.h"
#include "clipboard_engine.h"

#define VLINK_CTRL_PORT 5000

static GMainLoop        *g_loop     = NULL;
static VLinkAuthManager *g_auth     = NULL;
static ClipboardEngine  *g_clip_eng = NULL;

/* ── مزامنة الحافظة: عند تغيّرها من الهاتف، نرسل إشعاراً للـ UI ── */
static void on_clipboard_changed(const ClipboardContent *content, gpointer data) {
    (void)data;
    if (!content || content->type != CLIP_TYPE_TEXT || !content->text) return;
    g_print("[Daemon] Clipboard synced (%.40s...)\n", content->text);
    /* سيُستلَم في vsharing-ui عبر GFileMonitor على /tmp/vsharing_phone_clipboard.txt */
}

/* ── معالجة إشارات الإيقاف النظيف ── */
static void on_signal(int signum) {
    (void)signum;
    g_print("\n[Daemon] Shutting down gracefully...\n");
    if (g_loop && g_main_loop_is_running(g_loop))
        g_main_loop_quit(g_loop);
}

/* ── دالة بث QR URL عند تشغيل الـ daemon ── */
static void print_connection_info(void) {
    gchar *url = vlink_server_get_qr_url(VLINK_CTRL_PORT);
    g_print("┌─────────────────────────────────────────┐\n");
    g_print("│  VaxpLink Daemon v2.0                   │\n");
    g_print("│  Protocol: VLink/2 + WebSocket           │\n");
    g_print("├─────────────────────────────────────────┤\n");
    g_print("│  Mobile URL : %-27s│\n", url);
    g_print("│  Control Port : %-25d│\n", VLINK_CTRL_PORT);
    g_print("│  Stream Port  : %-25d│\n", VLINK_CTRL_PORT + 1);
    g_print("└─────────────────────────────────────────┘\n");
    g_free(url);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    /* إشارات الإيقاف */
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP,  on_signal);

    g_print("[Daemon] Initializing VLink Daemon v2...\n");

    /* تهيئة حلقة الأحداث */
    g_loop = g_main_loop_new(NULL, FALSE);

    /* تهيئة مدير المصادقة */
    g_auth = vlink_auth_manager_new();

    /* تهيئة محرك الحافظة (Event-driven, <20ms) */
    g_clip_eng = clipboard_engine_new(on_clipboard_changed, NULL);
    clipboard_engine_set_enabled(g_clip_eng, TRUE);

    /* تشغيل خادم VLink (يهيّئ جسر الإشعارات داخلياً — المرحلة 2) */
    vlink_server_start(VLINK_CTRL_PORT);

    /* تشغيل اكتشاف الأجهزة mDNS */
    vsharing_discovery_start();

    /* طباعة معلومات الاتصال */
    print_connection_info();

    g_print("[Daemon] ✅ All systems operational. Running...\n");

    /* حلقة الأحداث الرئيسية */
    g_main_loop_run(g_loop);

    /* إيقاف نظيف */
    g_print("[Daemon] Stopping all services...\n");
    vsharing_discovery_stop();
    vlink_server_stop();  /* يوقف جسر الإشعارات داخلياً */
    clipboard_engine_free(g_clip_eng);
    vlink_auth_manager_free(g_auth);
    g_main_loop_unref(g_loop);

    g_print("[Daemon] Goodbye. 👋\n");
    return 0;
}
