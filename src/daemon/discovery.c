#include "discovery.h"
#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-client/publish.h>
#include <avahi-glib/glib-watch.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <stdio.h>

static AvahiGLibPoll *glib_poll = NULL;
static AvahiClient *client = NULL;
static AvahiEntryGroup *group = NULL;
static AvahiServiceBrowser *browser = NULL;

static void browse_callback(AvahiServiceBrowser *b, AvahiIfIndex interface, AvahiProtocol protocol,
                            AvahiBrowserEvent event, const char *name, const char *type,
                            const char *domain, AvahiLookupResultFlags flags, void *userdata) {
    if (event == AVAHI_BROWSER_NEW) {
        g_print("✅ جهاز جديد وجد على الشبكة (mDNS): %s\n", name);
        // لاحقاً: هنا نقوم بطلب عنوان IP للجهاز واسم المضيف (Resolve) للاتصال به
    } else if (event == AVAHI_BROWSER_REMOVE) {
        g_print("❌ جهاز غادر الشبكة: %s\n", name);
    }
}

static void client_callback(AvahiClient *c, AvahiClientState state, void *userdata) {
    if (state == AVAHI_CLIENT_S_RUNNING) {
        // إنشاء مجموعة الخدمات ونشر الخدمة الخاصة بنا لكي يكتشفنا الآخرون
        if (!group) {
            group = avahi_entry_group_new(c, NULL, NULL);
            // ننشر الخدمة على منفذ 5000 (منفذ مبدئي لخادم نقل الملفات)
            avahi_entry_group_add_service(group, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, 0,
                                          "Vsharing-Device", "_vsharing._tcp", NULL, NULL, 5000, NULL);
            avahi_entry_group_commit(group);
        }
    }
}

void vsharing_discovery_start(void) {
    int error;
    glib_poll = avahi_glib_poll_new(NULL, G_PRIORITY_DEFAULT);
    
    client = avahi_client_new(avahi_glib_poll_get(glib_poll), AVAHI_CLIENT_NO_FAIL, 
                              client_callback, NULL, &error);
                              
    if (!client) {
        g_printerr("⚠️ فشل في تشغيل Avahi: %s\n", avahi_strerror(error));
        return;
    }
    
    // تصفح الأجهزة الأخرى التي تبث نفس الخدمة (_vsharing._tcp)
    browser = avahi_service_browser_new(client, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
                                        "_vsharing._tcp", NULL, 0, browse_callback, NULL);
    
    g_print("🔍 Vsharing Discovery Started (Avahi mDNS)...\n");
}

void vsharing_discovery_stop(void) {
    if (browser) avahi_service_browser_free(browser);
    if (group) avahi_entry_group_free(group);
    if (client) avahi_client_free(client);
    if (glib_poll) avahi_glib_poll_free(glib_poll);
}
