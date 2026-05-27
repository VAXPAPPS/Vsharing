project('vsharing', 'c',
  version: '0.1.0',
  default_options: ['warning_level=2', 'c_std=gnu11']
)

gtk_dep = dependency('gtk4')
gio_dep = dependency('gio-2.0')
avahi_client_dep = dependency('avahi-client', required: false)
avahi_glib_dep = dependency('avahi-glib', required: false)
qrencode_dep = dependency('libqrencode', required: false)
soup_dep = dependency('libsoup-3.0', required: false)

# --- UI Application ---
vsharing_ui_sources = [
  'src/ui/main.c',
  'src/ui/window.c'
]

executable('vsharing-ui',
  vsharing_ui_sources,
  dependencies: [gtk_dep, avahi_client_dep, avahi_glib_dep, qrencode_dep],
  install: true
)

# --- Background Daemon ---
vsharing_daemon_sources = [
  'src/daemon/main.c',
  'src/daemon/discovery.c',
  'src/daemon/server.c'
]

executable('vsharingd',
  vsharing_daemon_sources,
  dependencies: [gio_dep, avahi_client_dep, avahi_glib_dep, soup_dep],
  install: true
)
