#include "window.h"
#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-glib/glib-watch.h>

static GtkWidget *global_radar_area = NULL;
static AvahiGLibPoll *ui_glib_poll = NULL;
static AvahiClient *ui_client = NULL;
static AvahiServiceBrowser *ui_browser = NULL;
static GList *discovered_devices = NULL;

static void add_device_to_radar(const char *device_name, int x, int y) {
    if (!global_radar_area) return;
    
    GtkWidget *node = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_add_css_class(node, "device-node");
    
    GtkWidget *icon = gtk_image_new_from_icon_name("computer-symbolic");
    gtk_widget_set_size_request(icon, 32, 32);
    
    GtkWidget *label = gtk_label_new(device_name);
    gtk_widget_add_css_class(label, "device-label");
    
    gtk_box_append(GTK_BOX(node), icon);
    gtk_box_append(GTK_BOX(node), label);
    
    gtk_fixed_put(GTK_FIXED(global_radar_area), node, x, y);
}

static void ui_browse_callback(AvahiServiceBrowser *b, AvahiIfIndex interface, AvahiProtocol protocol,
                               AvahiBrowserEvent event, const char *name, const char *type,
                               const char *domain, AvahiLookupResultFlags flags, void *userdata) {
    if (event == AVAHI_BROWSER_NEW) {
        // تجنب إضافة نفس الجهاز عدة مرات (بسبب واجهات الشبكة المتعددة)
        for (GList *l = discovered_devices; l != NULL; l = l->next) {
            if (g_strcmp0((char*)l->data, name) == 0) return; // الجهاز موجود بالفعل
        }
        discovered_devices = g_list_append(discovered_devices, g_strdup(name));
        
        g_print("📱 UI: ظهر جهاز جديد في الرادار: %s\n", name);
        // وضع الجهاز في إحداثيات عشوائية حول دائرة المركز
        int x = g_random_int_range(50, 650);
        int y = g_random_int_range(50, 400);
        // تفادي منطقة المنتصف
        if (x > 200 && x < 400 && y > 100 && y < 300) { x = 100; y = 100; }
        add_device_to_radar(name, x, y);
    }
}

static void ui_client_callback(AvahiClient *c, AvahiClientState state, void *userdata) {}

static void start_ui_discovery() {
    int error;
    ui_glib_poll = avahi_glib_poll_new(NULL, G_PRIORITY_DEFAULT);
    ui_client = avahi_client_new(avahi_glib_poll_get(ui_glib_poll), AVAHI_CLIENT_NO_FAIL, 
                                 ui_client_callback, NULL, &error);
    if (ui_client) {
        ui_browser = avahi_service_browser_new(ui_client, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
                                               "_vsharing._tcp", NULL, 0, ui_browse_callback, NULL);
    }
}

#include <qrencode.h>

static GdkTexture *create_qr_texture(const char *text) {
    QRcode *qr = QRcode_encodeString(text, 0, QR_ECLEVEL_L, QR_MODE_8, 1);
    if (!qr) return NULL;
    
    int scale = 6;
    int size = qr->width * scale;
    guchar *pixels = g_malloc(size * size * 4); // RGBA
    
    for (int y = 0; y < qr->width; y++) {
        for (int x = 0; x < qr->width; x++) {
            guchar color = (qr->data[y * qr->width + x] & 1) ? 0 : 255;
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    int offset = ((y * scale + sy) * size + (x * scale + sx)) * 4;
                    pixels[offset] = color;     // R
                    pixels[offset+1] = color;   // G
                    pixels[offset+2] = color;   // B
                    pixels[offset+3] = 255;     // A
                }
            }
        }
    }
    
    GBytes *bytes = g_bytes_new_take(pixels, size * size * 4);
    GdkTexture *texture = gdk_memory_texture_new(size, size, GDK_MEMORY_R8G8B8A8, bytes, size * 4);
    g_bytes_unref(bytes);
    QRcode_free(qr);
    
    return texture;
}

#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <netdb.h>

// دالة لجلب رقم الـ IP المحلي للحاسوب
static char* get_local_ip() {
    struct ifaddrs *ifaddr, *ifa;
    static char ip[NI_MAXHOST] = "127.0.0.1";
    
    if (getifaddrs(&ifaddr) == -1) return ip;
    
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        if (ifa->ifa_addr->sa_family == AF_INET) {
            if (strcmp(ifa->ifa_name, "lo") != 0) { // تخطي اتصال الـ localhost الداخلي
                strcpy(ip, inet_ntoa(((struct sockaddr_in *)ifa->ifa_addr)->sin_addr));
                break;
            }
        }
    }
    freeifaddrs(ifaddr);
    return ip;
}

static void show_qr_dialog(GtkWidget *widget, gpointer window) {
    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(window));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_title(GTK_WINDOW(dialog), "الاتصال بالهاتف");
    gtk_window_set_default_size(GTK_WINDOW(dialog), 350, 450);
    gtk_widget_add_css_class(dialog, "vsharing-window");
    
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
    gtk_widget_set_margin_top(box, 30);
    gtk_widget_set_margin_bottom(box, 30);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    
    // إنشاء الرابط الفعلي بناءً على الـ IP المحلي
    char *ip = get_local_ip();
    char url[256];
    snprintf(url, sizeof(url), "http://%s:5000", ip);
    
    GdkTexture *qr_tex = create_qr_texture(url);
    GtkWidget *qr_img = NULL;
    if (qr_tex) {
        qr_img = gtk_image_new_from_paintable(GDK_PAINTABLE(qr_tex));
        gtk_widget_set_size_request(qr_img, 200, 200);
        g_object_unref(qr_tex);
    } else {
        qr_img = gtk_label_new("خطأ في توليد QR Code - تأكد من توفر libqrencode");
    }
    
    GtkWidget *label = gtk_label_new("افتح كاميرا الهاتف وامسح الرمز\nلإرسال الملفات من المتصفح مباشرة");
    gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
    gtk_widget_add_css_class(label, "header-title");
    
    gtk_box_append(GTK_BOX(box), qr_img);
    gtk_box_append(GTK_BOX(box), label);
    
    gtk_window_set_child(GTK_WINDOW(dialog), box);
    gtk_window_present(GTK_WINDOW(dialog));
}

static GtkSwitch *global_clipboard_switch = NULL;
static char last_pc_copied_text[4096] = {0};
static char last_phone_received_text[4096] = {0};

static void on_clipboard_text_ready(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GdkClipboard *clipboard = GDK_CLIPBOARD(source_object);
    GError *error = NULL;
    char *text = gdk_clipboard_read_text_finish(clipboard, res, &error);
    if (text) {
        // إذا كان هذا النص هو نفس النص الذي استقبلناه للتو من الهاتف، نتجاهله لنمنع الحلقة المفرغة
        if (g_strcmp0(text, last_phone_received_text) == 0) {
            g_free(text);
            return;
        }
        
        if (g_strcmp0(text, last_pc_copied_text) != 0) {
            g_strlcpy(last_pc_copied_text, text, sizeof(last_pc_copied_text));
            g_file_set_contents("/tmp/vsharing_pc_clipboard.txt", text, -1, NULL);
            
            // الأهم: تفريغ ملف الهاتف والذاكرة لكي لا يرتد النص القديم للحاسوب
            g_file_set_contents("/tmp/vsharing_phone_clipboard.txt", "", -1, NULL);
            last_phone_received_text[0] = '\0';
            
            char preview[40] = {0};
            g_strlcpy(preview, text, sizeof(preview));
            g_print("📋 [مزامنة الحافظة] تم نسخ نص من الحاسوب وجاهز للهاتف: %s...\n", preview);
        }
        g_free(text);
    } else if (error) {
        // تجاهل أخطاء عدم توافق النص (تحدث في Wayland عند نسخ صور أو ملفات)
        g_error_free(error);
    }
}

static void on_clipboard_switch_state_set(GObject *gobject, GParamSpec *pspec, gpointer user_data) {
    if (gtk_switch_get_active(GTK_SWITCH(gobject))) {
        // بمجرد التفعيل، نقرأ الحافظة فوراً لتكون جاهزة
        GdkClipboard *cb = gdk_display_get_clipboard(gdk_display_get_default());
        gdk_clipboard_read_text_async(cb, NULL, (GAsyncReadyCallback)on_clipboard_text_ready, NULL);
    }
}

static void set_clipboard_forcefully(const char *text) {
    gint std_in;
    GError *err = NULL;
    gchar *argv_wl[] = {"wl-copy", NULL};
    if (g_spawn_async_with_pipes(NULL, argv_wl, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &std_in, NULL, NULL, &err)) {
        if (write(std_in, text, strlen(text)) == -1) {}
        close(std_in);
        return;
    }
    if (err) { g_error_free(err); err = NULL; }
    
    gchar *argv_xclip[] = {"xclip", "-selection", "clipboard", NULL};
    if (g_spawn_async_with_pipes(NULL, argv_xclip, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &std_in, NULL, NULL, &err)) {
        if (write(std_in, text, strlen(text)) == -1) {}
        close(std_in);
        return;
    }
    if (err) g_error_free(err);
}

static gboolean poll_phone_clipboard(gpointer data) {
    char *text = NULL;
    if (g_file_get_contents("/tmp/vsharing_phone_clipboard.txt", &text, NULL, NULL)) {
        if (text && strlen(text) > 0 && global_clipboard_switch && gtk_switch_get_active(global_clipboard_switch)) {
            if (g_strcmp0(text, last_phone_received_text) != 0) {
                g_strlcpy(last_phone_received_text, text, sizeof(last_phone_received_text));
                
                // مسح ذاكرة الحاسوب لكي يتم تسجيل أي نسخ قادم حتى لو كان مطابقاً
                last_pc_copied_text[0] = '\0';
                
                // الطريقة الأساسية عبر GTK
                GdkClipboard *cb = gdk_display_get_clipboard(gdk_display_get_default());
                gdk_clipboard_set_text(cb, text);
                
                // الطريقة الإجبارية التي تتجاوز قيود الأمان في Wayland/X11
                set_clipboard_forcefully(text);
                
                g_print("📥 [مزامنة الحافظة] تم استقبال نص جديد من الهاتف ووضعه بالحافظة!\n");
            }
        }
        g_free(text);
    }
    return G_SOURCE_CONTINUE;
}

static void on_clipboard_changed(GdkClipboard *clipboard, gpointer user_data) {
    // إذا كان خيار المزامنة مفعلاً، نقرأ النص الجديد
    if (global_clipboard_switch && gtk_switch_get_active(global_clipboard_switch)) {
        gdk_clipboard_read_text_async(clipboard, NULL, (GAsyncReadyCallback)on_clipboard_text_ready, NULL);
    }
}

// هذه الدالة تتعامل مع استقبال الملفات عبر السحب والإفلات
static void
on_drop (GtkDropTarget *target, const GValue *value, double x, double y, gpointer data)
{
    if (G_VALUE_HOLDS (value, GDK_TYPE_FILE_LIST)) {
        GSList *files = g_value_get_boxed (value);
        if (files != NULL) {
            // نأخذ أول ملف فقط للتبسيط
            GFile *file = files->data;
            char *path = g_file_get_path (file);
            g_print("📁 تم تجهيز الملف، الآن افتح هاتفك للاستلام: %s\n", path);
            
            // نكتب المسار في ملف مؤقت لكي يقرأه الخادم vsharingd
            g_file_set_contents("/tmp/vsharing_current_file.txt", path, -1, NULL);
            g_free (path);
        }
    }
}

void
vsharing_window_create (GtkApplication *app)
{
    GtkWidget *window = gtk_application_window_new (app);
    gtk_window_set_title (GTK_WINDOW (window), "Vsharing");
    gtk_window_set_default_size (GTK_WINDOW (window), 800, 600);
    
    // إزالة شريط العنوان التقليدي لاستخدام تصميم مخصص ونظيف
    // في GTK4 يمكننا الاعتماد على نافذة مسطحة وتطبيق ستايل من خلال CSS
    gtk_widget_add_css_class (window, "vsharing-window");

    // الحاوية الرئيسية
    GtkWidget *main_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child (GTK_WINDOW (window), main_box);

    // شريط علوي مخصص (Custom HeaderBar) لتجنب libadwaita
    GtkWidget *header = gtk_center_box_new ();
    gtk_widget_add_css_class (header, "custom-header");
    
    GtkWidget *title_label = gtk_label_new ("Vsharing");
    gtk_widget_add_css_class (title_label, "header-title");
    gtk_center_box_set_center_widget (GTK_CENTER_BOX (header), title_label);
    
    // حاوية للأزرار اليمنى
    GtkWidget *right_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    
    GtkWidget *pair_btn = gtk_button_new_with_label ("اقتران جهاز");
    gtk_widget_add_css_class (pair_btn, "flat-btn");
    g_signal_connect(pair_btn, "clicked", G_CALLBACK(show_qr_dialog), window);
    
    GtkWidget *settings_btn = gtk_button_new_from_icon_name ("applications-system-symbolic");
    gtk_widget_add_css_class (settings_btn, "flat-btn");
    
    gtk_box_append(GTK_BOX(right_box), pair_btn);
    gtk_box_append(GTK_BOX(right_box), settings_btn);
    
    gtk_center_box_set_end_widget (GTK_CENTER_BOX (header), right_box);
    
    gtk_box_append (GTK_BOX (main_box), header);

    // حاوية الرادار (المنطقة المركزية)
    GtkWidget *radar_area = gtk_fixed_new ();
    global_radar_area = radar_area;
    gtk_widget_set_vexpand (radar_area, TRUE);
    gtk_widget_set_hexpand (radar_area, TRUE);
    gtk_widget_add_css_class (radar_area, "radar-area");
    gtk_box_append (GTK_BOX (main_box), radar_area);

    // المنطقة المركزية (دائرة السحب والإفلات)
    GtkWidget *drop_zone = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_halign (drop_zone, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (drop_zone, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class (drop_zone, "drop-zone");

    GtkWidget *drop_icon = gtk_image_new_from_icon_name ("document-send-symbolic");
    gtk_widget_set_size_request (drop_icon, 64, 64);
    GtkWidget *drop_label = gtk_label_new ("اسحب وأفلت الملفات هنا\nأو انقر للبحث عن الأجهزة");
    gtk_label_set_justify (GTK_LABEL (drop_label), GTK_JUSTIFY_CENTER);
    
    gtk_box_append (GTK_BOX (drop_zone), drop_icon);
    gtk_box_append (GTK_BOX (drop_zone), drop_label);

    // وضع الدائرة في منتصف الرادار
    // سنقوم بتحديد موقعها باستخدام Fixed لاحقاً ديناميكياً، ولكن للتبسيط نضعها في المنتصف
    gtk_fixed_put (GTK_FIXED (radar_area), drop_zone, 300, 200);

    // تفعيل استقبال الملفات (Drag & Drop) على دائرة الإفلات
    GtkDropTarget *drop_target = gtk_drop_target_new (GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect (drop_target, "drop", G_CALLBACK (on_drop), NULL);
    gtk_widget_add_controller (drop_zone, GTK_EVENT_CONTROLLER (drop_target));

    // شريط سفلي (للحالة ومزامنة الحافظة)
    GtkWidget *bottom_bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class (bottom_bar, "bottom-bar");
    gtk_widget_set_margin_start (bottom_bar, 10);
    gtk_widget_set_margin_end (bottom_bar, 10);
    gtk_widget_set_margin_bottom (bottom_bar, 10);

    GtkWidget *clipboard_switch = gtk_switch_new ();
    global_clipboard_switch = GTK_SWITCH(clipboard_switch);
    GtkWidget *clipboard_label = gtk_label_new ("مزامنة الحافظة (Clipboard Sync)");
    
    // ربط مستمع لتغيرات الحافظة
    GdkClipboard *clipboard = gdk_display_get_clipboard(gdk_display_get_default());
    g_signal_connect(clipboard, "changed", G_CALLBACK(on_clipboard_changed), NULL);
    g_signal_connect(clipboard_switch, "notify::active", G_CALLBACK(on_clipboard_switch_state_set), NULL);
    
    // مراقبة نصوص الهاتف بشكل مستمر (Polling) لتفادي توقف GFileMonitor عند تغيير الـ Inode
    g_timeout_add_seconds(1, poll_phone_clipboard, NULL);
    
    gtk_box_append (GTK_BOX (bottom_bar), clipboard_switch);
    gtk_box_append (GTK_BOX (bottom_bar), clipboard_label);
    
    gtk_box_append (GTK_BOX (main_box), bottom_bar);

    // تشغيل البحث عن الأجهزة في الرادار
    start_ui_discovery();

    gtk_window_present (GTK_WINDOW (window));
}
