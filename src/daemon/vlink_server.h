#pragma once
#include <glib.h>

void   vlink_server_start          (int port);
void   vlink_server_stop           (void);
gchar *vlink_server_get_qr_url     (int port);
guint  vlink_server_get_client_count(void);
