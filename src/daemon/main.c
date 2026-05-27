#include <gio/gio.h>
#include <stdio.h>
#include "discovery.h"
#include "server.h"

int main(int argc, char **argv) {
    // هذه الخدمة ستعمل في الخلفية للبحث عن الأجهزة (mDNS) وإدارة البلوتوث والشبكة
    g_print("Vsharing Daemon Started...\n");
    
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    
    // تشغيل خادم TCP لاستقبال الملفات
    vsharing_server_start(5000);
    
    // تشغيل نظام اكتشاف الأجهزة (Avahi/mDNS)
    vsharing_discovery_start();
    
    // TODO: إضافة مراقب الحافظة (Clipboard Monitor)
    
    g_main_loop_run(loop);
    
    vsharing_discovery_stop();
    vsharing_server_stop();
    return 0;
}
