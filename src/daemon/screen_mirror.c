#include "screen_mirror.h"
#include "vlink_protocol.h"
#include <gio/gio.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* FFmpeg */
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

/* X11 للالتقاط */
#include <X11/Xlib.h>
#include <X11/Xutil.h>

/* ============================================================
 * حالة داخلية
 * ============================================================ */
typedef struct {
    /* X11 */
    Display    *display;
    Window      root;
    int         x11_w, x11_h;

    /* FFmpeg */
    AVCodecContext  *enc_ctx;
    AVFrame         *frame_yuv;
    AVFrame         *frame_rgb;
    AVPacket        *pkt;
    struct SwsContext *sws;

    /* UDP */
    int              udp_sock;
    struct sockaddr_in dest_addr;

    /* إعدادات */
    int     fps;
    int     bitrate;
    char    client_ip[46];

    /* تحكم */
    volatile gboolean running;
    GThread  *thread;

    /* إحصائيات */
    uint32_t frames_sent;
    uint64_t bytes_sent;
    gboolean need_keyframe;
    uint32_t frame_id;

} ScreenMirrorCtx;

static ScreenMirrorCtx g_ctx = {0};
static GMutex          g_stats_mutex;

/* ============================================================
 * دوال التهيئة
 * ============================================================ */

static gboolean init_x11(void) {
    g_ctx.display = XOpenDisplay(NULL);
    if (!g_ctx.display) {
        g_printerr("[ScreenMirror] ❌ Cannot open X11 display. Is DISPLAY set?\n");
        return FALSE;
    }
    g_ctx.root  = DefaultRootWindow(g_ctx.display);
    XWindowAttributes wa;
    XGetWindowAttributes(g_ctx.display, g_ctx.root, &wa);
    g_ctx.x11_w = wa.width;
    g_ctx.x11_h = wa.height;
    g_print("[ScreenMirror] 🖥️  X11 display: %dx%d\n", g_ctx.x11_w, g_ctx.x11_h);
    return TRUE;
}

static gboolean init_encoder(void) {
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        g_printerr("[ScreenMirror] ❌ H.264 encoder not found\n");
        return FALSE;
    }

    g_ctx.enc_ctx = avcodec_alloc_context3(codec);
    g_ctx.enc_ctx->codec_id  = AV_CODEC_ID_H264;
    g_ctx.enc_ctx->bit_rate  = g_ctx.bitrate;
    g_ctx.enc_ctx->width     = SCREEN_DEFAULT_WIDTH;
    g_ctx.enc_ctx->height    = SCREEN_DEFAULT_HEIGHT;
    g_ctx.enc_ctx->time_base = (AVRational){1, g_ctx.fps};
    g_ctx.enc_ctx->framerate = (AVRational){g_ctx.fps, 1};
    g_ctx.enc_ctx->gop_size  = g_ctx.fps * 2;  /* I-frame كل ثانيتين */
    g_ctx.enc_ctx->max_b_frames = 0;
    g_ctx.enc_ctx->pix_fmt   = AV_PIX_FMT_YUV420P;

    /* إعدادات سرعة/زمن-استجابة */
    av_opt_set(g_ctx.enc_ctx->priv_data, "preset",  "ultrafast", 0);
    av_opt_set(g_ctx.enc_ctx->priv_data, "tune",    "zerolatency", 0);
    av_opt_set(g_ctx.enc_ctx->priv_data, "profile", "baseline", 0);

    if (avcodec_open2(g_ctx.enc_ctx, codec, NULL) < 0) {
        g_printerr("[ScreenMirror] ❌ Cannot open H.264 encoder\n");
        avcodec_free_context(&g_ctx.enc_ctx);
        return FALSE;
    }

    /* إطارات AVFrame */
    g_ctx.frame_yuv = av_frame_alloc();
    g_ctx.frame_yuv->format = AV_PIX_FMT_YUV420P;
    g_ctx.frame_yuv->width  = SCREEN_DEFAULT_WIDTH;
    g_ctx.frame_yuv->height = SCREEN_DEFAULT_HEIGHT;
    av_image_alloc(g_ctx.frame_yuv->data, g_ctx.frame_yuv->linesize,
                   SCREEN_DEFAULT_WIDTH, SCREEN_DEFAULT_HEIGHT,
                   AV_PIX_FMT_YUV420P, 32);

    g_ctx.frame_rgb = av_frame_alloc();
    g_ctx.frame_rgb->format = AV_PIX_FMT_BGR24;
    g_ctx.frame_rgb->width  = g_ctx.x11_w;
    g_ctx.frame_rgb->height = g_ctx.x11_h;

    /* SWS: تحويل BGR24 → YUV420P مع تغيير الحجم */
    g_ctx.sws = sws_getContext(
        g_ctx.x11_w, g_ctx.x11_h, AV_PIX_FMT_BGR24,
        SCREEN_DEFAULT_WIDTH, SCREEN_DEFAULT_HEIGHT, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, NULL, NULL, NULL
    );
    if (!g_ctx.sws) {
        g_printerr("[ScreenMirror] ❌ sws_getContext failed\n");
        return FALSE;
    }

    g_ctx.pkt = av_packet_alloc();
    g_print("[ScreenMirror] ✅ H.264 encoder ready (%dx%d @ %d fps)\n",
            SCREEN_DEFAULT_WIDTH, SCREEN_DEFAULT_HEIGHT, g_ctx.fps);
    return TRUE;
}

static gboolean init_udp(void) {
    g_ctx.udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_ctx.udp_sock < 0) {
        g_printerr("[ScreenMirror] ❌ Cannot create UDP socket\n");
        return FALSE;
    }

    memset(&g_ctx.dest_addr, 0, sizeof(g_ctx.dest_addr));
    g_ctx.dest_addr.sin_family      = AF_INET;
    g_ctx.dest_addr.sin_port        = htons(SCREEN_UDP_PORT);
    g_ctx.dest_addr.sin_addr.s_addr = inet_addr(g_ctx.client_ip);

    /* زيادة حجم مخزن الإرسال */
    int sndbuf = 4 * 1024 * 1024;
    setsockopt(g_ctx.udp_sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    g_print("[ScreenMirror] 📡 UDP stream → %s:%d\n", g_ctx.client_ip, SCREEN_UDP_PORT);
    return TRUE;
}

/* ============================================================
 * دالة الالتقاط والترميز والإرسال (تعمل في thread منفصل)
 * ============================================================ */

/* إرسال حزمة H.264 عبر UDP مع VLink header */
static void send_frame_packet(const uint8_t *data, int size, gboolean is_keyframe) {
    if (size <= 0 || g_ctx.udp_sock < 0) return;

    /* بناء VLinkScreenFrame header (16 bytes) */
    VLinkScreenFrame sf = {0};
    sf.frame_id     = g_ctx.frame_id++;
    sf.timestamp_ms = (uint32_t)(g_get_monotonic_time() / 1000);
    sf.is_keyframe  = is_keyframe ? 1 : 0;
    sf.data_len     = (uint32_t)size;

    /* نُجمع header + بيانات H.264 في مخزن واحد */
    int total = sizeof(sf) + size;
    uint8_t *buf = g_malloc(total);
    memcpy(buf, &sf, sizeof(sf));
    memcpy(buf + sizeof(sf), data, size);

    /* نُجزّئ إذا تجاوز حد UDP (65507 bytes) */
    int max_chunk = 60000;
    int offset    = 0;
    while (offset < total) {
        int chunk = MIN(total - offset, max_chunk);
        sendto(g_ctx.udp_sock, buf + offset, chunk, 0,
               (struct sockaddr *)&g_ctx.dest_addr,
               sizeof(g_ctx.dest_addr));
        offset += chunk;
    }

    g_mutex_lock(&g_stats_mutex);
    g_ctx.frames_sent++;
    g_ctx.bytes_sent += total;
    g_mutex_unlock(&g_stats_mutex);

    g_free(buf);
}

static gpointer mirror_thread_func(gpointer data) {
    (void)data;
    int64_t frame_pts = 0;
    int64_t frame_interval_us = G_USEC_PER_SEC / g_ctx.fps;

    g_print("[ScreenMirror] 🎬 Streaming started\n");

    while (g_ctx.running) {
        int64_t t0 = g_get_monotonic_time();

        /* ── التقاط الشاشة عبر X11 ── */
        XImage *img = XGetImage(g_ctx.display, g_ctx.root,
                                0, 0, g_ctx.x11_w, g_ctx.x11_h,
                                AllPlanes, ZPixmap);
        if (!img) {
            g_usleep(frame_interval_us);
            continue;
        }

        /* ربط بيانات XImage بـ frame_rgb */
        uint8_t *rgb_data[4]  = { (uint8_t *)img->data, NULL, NULL, NULL };
        int      rgb_lines[4] = { img->bytes_per_line, 0, 0, 0 };

        /* اختيار تنسيق البيكسل بحسب عمق الصورة */
        enum AVPixelFormat src_fmt = AV_PIX_FMT_BGR24;
        if (img->bits_per_pixel == 32)
            src_fmt = AV_PIX_FMT_BGR0;

        /* تحديث SWS إذا تغيّر التنسيق */
        if (g_ctx.sws) {
            /* نُعيد البناء فقط عند الضرورة */
        }
        struct SwsContext *sws_cur = sws_getContext(
            g_ctx.x11_w, g_ctx.x11_h, src_fmt,
            SCREEN_DEFAULT_WIDTH, SCREEN_DEFAULT_HEIGHT, AV_PIX_FMT_YUV420P,
            SWS_BILINEAR, NULL, NULL, NULL
        );

        /* تحويل + تغيير الحجم */
        if (sws_cur) {
            sws_scale(sws_cur,
                      (const uint8_t *const *)rgb_data, rgb_lines,
                      0, g_ctx.x11_h,
                      g_ctx.frame_yuv->data, g_ctx.frame_yuv->linesize);
            sws_freeContext(sws_cur);
        }

        XDestroyImage(img);

        /* ── الترميز ── */
        g_ctx.frame_yuv->pts = frame_pts++;

        /* طلب I-frame إذا طُلب */
        if (g_ctx.need_keyframe) {
            g_ctx.frame_yuv->pict_type = AV_PICTURE_TYPE_I;
            g_ctx.need_keyframe = FALSE;
        } else {
            g_ctx.frame_yuv->pict_type = AV_PICTURE_TYPE_NONE;
        }

        int ret = avcodec_send_frame(g_ctx.enc_ctx, g_ctx.frame_yuv);
        if (ret < 0) { g_usleep(frame_interval_us); continue; }

        while (avcodec_receive_packet(g_ctx.enc_ctx, g_ctx.pkt) == 0) {
            gboolean is_kf = (g_ctx.pkt->flags & AV_PKT_FLAG_KEY) != 0;
            send_frame_packet(g_ctx.pkt->data, g_ctx.pkt->size, is_kf);
            av_packet_unref(g_ctx.pkt);
        }

        /* تنظيم معدل الإطارات */
        int64_t elapsed = g_get_monotonic_time() - t0;
        int64_t sleep   = frame_interval_us - elapsed;
        if (sleep > 0) g_usleep(sleep);
    }

    g_print("[ScreenMirror] 🛑 Streaming stopped\n");
    return NULL;
}

/* ============================================================
 * الواجهة العامة
 * ============================================================ */

gboolean screen_mirror_start(const char *client_ip, int fps, int bitrate) {
    if (g_ctx.running) {
        g_print("[ScreenMirror] ⚠️  Already running\n");
        return TRUE;
    }

    memset(&g_ctx, 0, sizeof(g_ctx));
    g_mutex_init(&g_stats_mutex);
    g_ctx.udp_sock = -1;

    g_strlcpy(g_ctx.client_ip, client_ip, sizeof(g_ctx.client_ip));
    g_ctx.fps     = (fps     > 0) ? fps     : SCREEN_DEFAULT_FPS;
    g_ctx.bitrate = (bitrate > 0) ? bitrate : SCREEN_DEFAULT_BITRATE;

    if (!init_x11()     ||
        !init_encoder() ||
        !init_udp()) {
        screen_mirror_stop();
        return FALSE;
    }

    g_ctx.running    = TRUE;
    g_ctx.need_keyframe = TRUE;
    g_ctx.thread = g_thread_new("screen-mirror", mirror_thread_func, NULL);

    g_print("[ScreenMirror] 🚀 Streaming to %s:%d @ %dfps / %dkbps\n",
            client_ip, SCREEN_UDP_PORT, g_ctx.fps, bitrate / 1000);
    return TRUE;
}

void screen_mirror_stop(void) {
    if (g_ctx.running) {
        g_ctx.running = FALSE;
        if (g_ctx.thread) {
            g_thread_join(g_ctx.thread);
            g_ctx.thread = NULL;
        }
    }

    /* تنظيف FFmpeg */
    if (g_ctx.enc_ctx) {
        avcodec_send_frame(g_ctx.enc_ctx, NULL);  /* flush */
        avcodec_free_context(&g_ctx.enc_ctx);
    }
    if (g_ctx.frame_yuv) {
        av_freep(&g_ctx.frame_yuv->data[0]);
        av_frame_free(&g_ctx.frame_yuv);
    }
    if (g_ctx.frame_rgb) av_frame_free(&g_ctx.frame_rgb);
    if (g_ctx.pkt)       av_packet_free(&g_ctx.pkt);

    /* تنظيف X11 */
    if (g_ctx.display) {
        XCloseDisplay(g_ctx.display);
        g_ctx.display = NULL;
    }

    /* تنظيف UDP */
    if (g_ctx.udp_sock >= 0) {
        close(g_ctx.udp_sock);
        g_ctx.udp_sock = -1;
    }

    g_mutex_clear(&g_stats_mutex);
    g_print("[ScreenMirror] 🧹 Resources freed\n");
}

gboolean screen_mirror_is_running(void) {
    return g_ctx.running;
}

void screen_mirror_get_stats(uint32_t *frames_sent, uint64_t *bytes_sent) {
    g_mutex_lock(&g_stats_mutex);
    if (frames_sent) *frames_sent = g_ctx.frames_sent;
    if (bytes_sent)  *bytes_sent  = g_ctx.bytes_sent;
    g_mutex_unlock(&g_stats_mutex);
}

void screen_mirror_request_keyframe(void) {
    g_ctx.need_keyframe = TRUE;
}
