#include <gtk/gtk.h>
#include "window.h"

static void
on_activate (GtkApplication *app, gpointer user_data)
{
    // تحميل ستايل مخصص (نعتمد على CSS الخاص بـ GTK بدلاً من libadwaita)
    GtkCssProvider *provider = gtk_css_provider_new ();
    gtk_css_provider_load_from_path (provider, "data/style.css");
    gtk_style_context_add_provider_for_display (
        gdk_display_get_default (),
        GTK_STYLE_PROVIDER (provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );
    g_object_unref (provider);

    // إنشاء النافذة الرئيسية
    vsharing_window_create (app);
}

int
main (int argc, char **argv)
{
    GtkApplication *app;
    int status;

    // استخدام org.venom.vsharing ليتوافق مع بيئتك (Venom/Vsharing)
    app = gtk_application_new ("org.venom.vsharing.ui", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);

    status = g_application_run (G_APPLICATION (app), argc, argv);
    g_object_unref (app);

    return status;
}
