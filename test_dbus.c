#include <gio/gio.h>
#include <stdio.h>

int main() {
    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!bus) return 1;

    GVariantBuilder *actions_builder = g_variant_builder_new(G_VARIANT_TYPE("as"));
    GVariantBuilder *hints_builder = g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(hints_builder, "{sv}", "value", g_variant_new_int32(50));

    GVariant *result = g_dbus_connection_call_sync(
        bus,
        "org.freedesktop.Notifications",
        "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications",
        "Notify",
        g_variant_new("(susss@as@a{sv}i)",
                      "Vsharing",        // app_name
                      0,  // replaces_id
                      "document-send",   // app_icon
                      "Test",             // summary
                      "Progress 50%",              // body
                      g_variant_builder_end(actions_builder),
                      g_variant_builder_end(hints_builder),
                      0            // expire_timeout
        ),
        G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        &error
    );

    if (result) {
        printf("Success\n");
        g_variant_unref(result);
    } else if (error) {
        printf("Error: %s\n", error->message);
    }
    return 0;
}
