project('vsharing', 'c',
  version : '2.0.0',
  default_options : ['warning_level=2', 'c_std=gnu11']
)

fs = import('fs')

# ── التبعيات ──
gtk_dep      = dependency('gtk4')
gio_dep      = dependency('gio-2.0')
avahi_client = dependency('avahi-client', required: false)
avahi_glib   = dependency('avahi-glib',   required: false)
qrencode     = dependency('libqrencode',  required: false)
soup_dep     = dependency('libsoup-3.0',  required: false)
thread_dep   = dependency('threads')

# ── المرحلة 3: FFmpeg + X11 ──
avcodec_dep  = dependency('libavcodec',  required: true)
avutil_dep   = dependency('libavutil',   required: true)
avformat_dep = dependency('libavformat', required: false)
swscale_dep  = dependency('libswscale',  required: true)
x11_dep      = dependency('x11',         required: true)

# ── المرحلة 4: VFS Bridge ──
fuse3_dep    = dependency('fuse3',       required: true)

common_args = ['-DVLINK_VERSION_STR="3.0.0"']

# ══════════════════════════════════════════
# Daemon (vsharingd)
# ══════════════════════════════════════════
executable('vsharingd',
  files([
    'src/daemon/main.c',
    'src/daemon/discovery.c',
    'src/daemon/vlink_protocol.c',
    'src/daemon/vlink_server.c',
    'src/daemon/auth_manager.c',
    'src/daemon/clipboard_engine.c',
    'src/daemon/notification_bridge.c',   # ← المرحلة 2
    'src/daemon/input_controller.c',      # ← المرحلة 3
    'src/daemon/screen_mirror.c',         # ← المرحلة 3
    'src/daemon/vfs_bridge.c',            # ← المرحلة 4
  ]),
  dependencies : [
    gio_dep, avahi_client, avahi_glib, soup_dep, thread_dep,
    avcodec_dep, avutil_dep, swscale_dep, x11_dep,          # ← المرحلة 3
    fuse3_dep,                                              # ← المرحلة 4
  ],
  c_args       : common_args,
  install      : true,
)

# ══════════════════════════════════════════
# UI (vsharing-ui)
# ══════════════════════════════════════════
executable('vsharing-ui',
  files([
    'src/ui/main.c',
    'src/ui/window.c',
    'src/ui/screen_viewer.c',             # ← المرحلة 3
  ]),
  dependencies : [
    gtk_dep, gio_dep, avahi_client, avahi_glib, qrencode, thread_dep,
    avcodec_dep, avutil_dep, swscale_dep,                    # ← المرحلة 3
  ],
  c_args       : common_args,
  install      : true,
)


# ══════════════════════════════════════════
# Data
# ══════════════════════════════════════════
install_data('data/style.css',
  install_dir : get_option('datadir') / 'vsharing'
)

if fs.is_file('data/vsharing-send.desktop')
  install_data('data/vsharing-send.desktop',
    install_dir : get_option('datadir') / 'applications'
  )
endif

summary({
  'Version'     : meson.project_version(),
  'GTK4'        : gtk_dep.found(),
  'libsoup-3.0' : soup_dep.found(),
  'Avahi'       : avahi_client.found(),
  'libqrencode' : qrencode.found(),
}, section: 'VaxpLink Build Summary')
