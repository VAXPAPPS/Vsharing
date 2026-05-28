#include "screen_viewer.h"
#include <gio/gio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* FFmpeg */
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

/* VLink frame header (يطابق VLinkScreenFrame في vlink_protocol.h) */
typedef struct {
    uint32_t frame_id;
    uint32_t timestamp_ms;
    uint8_t  is_keyframe;
    uint32_t data_len;
} __attribute__((packed)) ScreenFrameHdr;

#define VIEWER_UDP_PORT  5002
#define RECV_BUF_SIZE    (128 * 1024)

/* ============================================================
 * حالة داخلية
 * ============================================================ */
typedef struct {
    /* GTK */
    GtkWidget       *drawing_area;
    GdkTexture      *current_texture;  /* الإطار الحالي للعرض */
    GMutex           texture_mutex;

    /* FFmpeg decoder */
    AVCodecContext  *dec_ctx;
    AVFrame         *frame_yuv;
    AVFrame         *frame_rgb;
    AVPacket        *pkt;
    struct SwsContext *sws;
    int              out_w, out_h;

    /* UDP */
    int              udp_sock;
    uint8_t         *recv_buf;

    /* تجميع الإطارات المُجزَّأة */
    uint8_t         *assemble_buf;
    int              assemble_off;
    uint32_t         expected_frame_id;

    /* تحكم */
    volatile gboolean running;
    GThread          *thread;

    /* Callback */
    KeyframeRequestCb keyframe_cb;

    /* إحصائيات */
    uint32_t frames_decoded;
    uint32_t frames_dropped;

} ViewerCtx;

static ViewerCtx g_vctx = {0};

/* ============================================================
 * دالة العرض في GTK (تُستدعى من main thread)
 * ============================================================ */
static gboolean redraw_idle(gpointer data) {
    (void)data;
    if (g_vctx.drawing_area && gtk_widget_get_realized(g_vctx.drawing_area))
        gtk_widget_queue_draw(g_vctx.drawing_area);
    return G_SOURCE_REMOVE;
}

/* ── draw callback لـ GtkDrawingArea ── */
static void on_draw(GtkDrawingArea *da, cairo_t *cr,
                    int width, int height, gpointer data)
{
    (void)da; (void)data;

    /* خلفية داكنة */
    cairo_set_source_rgb(cr, 0.05, 0.05, 0.10);
    cairo_paint(cr);

    g_mutex_lock(&g_vctx.texture_mutex);
    GdkTexture *tex = g_vctx.current_texture
                      ? g_object_ref(g_vctx.current_texture) : NULL;
    g_mutex_unlock(&g_vctx.texture_mutex);

    if (!tex) {
        /* نص انتظار */
        cairo_set_source_rgba(cr, 0.5, 0.6, 0.9, 0.7);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 16);
        const char *msg = "في انتظار بث الهاتف...";
        cairo_text_extents_t ext;
        cairo_text_extents(cr, msg, &ext);
        cairo_move_to(cr,
            (width  - ext.width)  / 2.0,
            (height + ext.height) / 2.0);
        cairo_show_text(cr, msg);
        return;
    }

    /* رسم الإطار مع الحفاظ على نسبة العرض */
    int tex_w = gdk_texture_get_width(tex);
    int tex_h = gdk_texture_get_height(tex);

    double scale = MIN((double)width  / tex_w,
                       (double)height / tex_h);
    double draw_w = tex_w * scale;
    double draw_h = tex_h * scale;
    double ox = (width  - draw_w) / 2.0;
    double oy = (height - draw_h) / 2.0;

    cairo_save(cr);
    cairo_translate(cr, ox, oy);
    cairo_scale(cr, scale, scale);
    gdk_texture_download(tex,
        (guchar *)cairo_image_surface_get_data(
            cairo_get_target(cr)),
        (gsize)(cairo_image_surface_get_stride(cairo_get_target(cr))));
    /* نستخدم snapshot API بدلاً من cairo مباشرة */
    cairo_restore(cr);

    /* الطريقة الصحيحة: نرسم الـ texture كـ pixbuf */
    GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, tex_w, tex_h);
    if (pb) {
        gdk_texture_download(tex,
                             gdk_pixbuf_get_pixels(pb),
                             gdk_pixbuf_get_rowstride(pb));
        cairo_save(cr);
        cairo_translate(cr, ox, oy);
        cairo_scale(cr, scale, scale);
        gdk_cairo_set_source_pixbuf(cr, pb, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
        g_object_unref(pb);
    }

    g_object_unref(tex);
}

/* ============================================================
 * تهيئة FFmpeg decoder
 * ============================================================ */
static gboolean init_decoder(void) {
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        g_printerr("[ScreenViewer] ❌ H.264 decoder not found\n");
        return FALSE;
    }

    g_vctx.dec_ctx = avcodec_alloc_context3(codec);
    g_vctx.dec_ctx->flags  |= AV_CODEC_FLAG_LOW_DELAY;
    g_vctx.dec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;

    if (avcodec_open2(g_vctx.dec_ctx, codec, NULL) < 0) {
        g_printerr("[ScreenViewer] ❌ Cannot open H.264 decoder\n");
        avcodec_free_context(&g_vctx.dec_ctx);
        return FALSE;
    }

    g_vctx.frame_yuv = av_frame_alloc();
    g_vctx.frame_rgb = av_frame_alloc();
    g_vctx.pkt       = av_packet_alloc();

    g_print("[ScreenViewer] ✅ H.264 decoder ready\n");
    return TRUE;
}

/* ============================================================
 * فك تشفير إطار وتحديث الـ texture
 * ============================================================ */
static void decode_and_display(const uint8_t *data, int size) {
    g_vctx.pkt->data = (uint8_t *)data;
    g_vctx.pkt->size = size;

    if (avcodec_send_packet(g_vctx.dec_ctx, g_vctx.pkt) < 0) {
        g_vctx.frames_dropped++;
        return;
    }

    while (avcodec_receive_frame(g_vctx.dec_ctx, g_vctx.frame_yuv) == 0) {
        int w = g_vctx.frame_yuv->width;
        int h = g_vctx.frame_yuv->height;
        if (w <= 0 || h <= 0) continue;

        /* بناء SWS إذا تغيّر الحجم */
        if (!g_vctx.sws || g_vctx.out_w != w || g_vctx.out_h != h) {
            if (g_vctx.sws) sws_freeContext(g_vctx.sws);
            g_vctx.sws = sws_getContext(w, h, g_vctx.frame_yuv->format,
                                        w, h, AV_PIX_FMT_RGB24,
                                        SWS_BILINEAR, NULL, NULL, NULL);
            g_vctx.out_w = w;
            g_vctx.out_h = h;

            if (g_vctx.frame_rgb->data[0])
                av_freep(&g_vctx.frame_rgb->data[0]);
            av_image_alloc(g_vctx.frame_rgb->data, g_vctx.frame_rgb->linesize,
                           w, h, AV_PIX_FMT_RGB24, 32);
            g_vctx.frame_rgb->width  = w;
            g_vctx.frame_rgb->height = h;
        }

        sws_scale(g_vctx.sws,
                  (const uint8_t *const *)g_vctx.frame_yuv->data,
                  g_vctx.frame_yuv->linesize, 0, h,
                  g_vctx.frame_rgb->data, g_vctx.frame_rgb->linesize);

        /* بناء GdkTexture من بيانات RGB */
        GBytes *bytes = g_bytes_new(g_vctx.frame_rgb->data[0],
                                    (gsize)(w * h * 3));
        GdkTexture *tex = gdk_memory_texture_new(
            w, h, GDK_MEMORY_R8G8B8, bytes,
            (gsize)(g_vctx.frame_rgb->linesize[0]));
        g_bytes_unref(bytes);

        /* تبادل آمن مع mutex */
        g_mutex_lock(&g_vctx.texture_mutex);
        if (g_vctx.current_texture)
            g_object_unref(g_vctx.current_texture);
        g_vctx.current_texture = tex;
        g_mutex_unlock(&g_vctx.texture_mutex);

        g_vctx.frames_decoded++;
        g_idle_add(redraw_idle, NULL);
    }
}

/* ============================================================
 * Thread الاستقبال
 * ============================================================ */
static gpointer viewer_recv_thread(gpointer data) {
    (void)data;
    g_print("[ScreenViewer] 📥 Listening for H.264 stream on UDP:%d\n",
            VIEWER_UDP_PORT);

    while (g_vctx.running) {
        struct timeval tv = {0, 50000};  /* timeout 50ms */
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(g_vctx.udp_sock, &fds);

        int sel = select(g_vctx.udp_sock + 1, &fds, NULL, NULL, &tv);
        if (sel <= 0) continue;

        ssize_t n = recv(g_vctx.udp_sock, g_vctx.recv_buf,
                         RECV_BUF_SIZE, 0);
        if (n <= (ssize_t)sizeof(ScreenFrameHdr)) continue;

        /* تحليل الـ header */
        ScreenFrameHdr hdr;
        memcpy(&hdr, g_vctx.recv_buf, sizeof(hdr));
        uint8_t *payload = g_vctx.recv_buf + sizeof(hdr);
        int      plen    = (int)(n - sizeof(hdr));

        if (plen <= 0) continue;

        /* فك التشفير مباشرة (بدون تجميع - كل حزمة NAL مستقلة) */
        decode_and_display(payload, plen);
    }

    return NULL;
}

/* ============================================================
 * الواجهة العامة
 * ============================================================ */

GtkWidget *screen_viewer_new(void) {
    if (!g_vctx.drawing_area) {
        g_mutex_init(&g_vctx.texture_mutex);

        g_vctx.drawing_area = gtk_drawing_area_new();
        gtk_widget_set_size_request(g_vctx.drawing_area, 360, 640);
        gtk_widget_add_css_class(g_vctx.drawing_area, "screen-viewer");
        gtk_drawing_area_set_draw_func(
            GTK_DRAWING_AREA(g_vctx.drawing_area),
            on_draw, NULL, NULL);
    }
    return g_vctx.drawing_area;
}

gboolean screen_viewer_start(int udp_port) {
    if (g_vctx.running) return TRUE;

    if (udp_port <= 0) udp_port = VIEWER_UDP_PORT;

    if (!init_decoder()) return FALSE;

    /* إنشاء UDP socket */
    g_vctx.udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_vctx.udp_sock < 0) {
        g_printerr("[ScreenViewer] ❌ Cannot create UDP socket\n");
        return FALSE;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)udp_port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(g_vctx.udp_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        g_printerr("[ScreenViewer] ❌ Cannot bind UDP port %d\n", udp_port);
        close(g_vctx.udp_sock);
        g_vctx.udp_sock = -1;
        return FALSE;
    }

    g_vctx.recv_buf    = g_malloc(RECV_BUF_SIZE);
    g_vctx.running     = TRUE;
    g_vctx.thread = g_thread_new("screen-viewer", viewer_recv_thread, NULL);

    g_print("[ScreenViewer] 🎥 Started on UDP port %d\n", udp_port);
    return TRUE;
}

void screen_viewer_stop(void) {
    if (!g_vctx.running) return;
    g_vctx.running = FALSE;

    if (g_vctx.thread) {
        g_thread_join(g_vctx.thread);
        g_vctx.thread = NULL;
    }

    /* تنظيف FFmpeg */
    if (g_vctx.dec_ctx)  avcodec_free_context(&g_vctx.dec_ctx);
    if (g_vctx.frame_yuv) av_frame_free(&g_vctx.frame_yuv);
    if (g_vctx.frame_rgb) {
        if (g_vctx.frame_rgb->data[0]) av_freep(&g_vctx.frame_rgb->data[0]);
        av_frame_free(&g_vctx.frame_rgb);
    }
    if (g_vctx.pkt) av_packet_free(&g_vctx.pkt);
    if (g_vctx.sws) { sws_freeContext(g_vctx.sws); g_vctx.sws = NULL; }

    /* تنظيف UDP */
    if (g_vctx.udp_sock >= 0) {
        close(g_vctx.udp_sock);
        g_vctx.udp_sock = -1;
    }
    g_free(g_vctx.recv_buf);
    g_vctx.recv_buf = NULL;

    /* تنظيف GTK */
    g_mutex_lock(&g_vctx.texture_mutex);
    if (g_vctx.current_texture) {
        g_object_unref(g_vctx.current_texture);
        g_vctx.current_texture = NULL;
    }
    g_mutex_unlock(&g_vctx.texture_mutex);

    g_print("[ScreenViewer] 🧹 Stopped (decoded=%u, dropped=%u)\n",
            g_vctx.frames_decoded, g_vctx.frames_dropped);
}

gboolean screen_viewer_is_active(void) {
    return g_vctx.running;
}

void screen_viewer_request_keyframe(void) {
    if (g_vctx.keyframe_cb) g_vctx.keyframe_cb();
}

void screen_viewer_set_keyframe_callback(KeyframeRequestCb cb) {
    g_vctx.keyframe_cb = cb;
}
