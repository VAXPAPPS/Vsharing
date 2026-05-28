/*
 * clipboard_engine.c — محرك مزامنة الحافظة عالي الأداء
 *
 * يستخدم GFileMonitor لمراقبة ملف مشترك مع الـ UI بدلاً من Polling.
 * التأخير: < 20ms بدلاً من 500ms.
 */

#include "clipboard_engine.h"
#include <gio/gio.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

/* إعلانات أمامية لدوال الـ callback */
static void on_pc_clip_file_changed(GFileMonitor *monitor, GFile *file,
                                     GFile *other_file, GFileMonitorEvent ev_type,
                                     gpointer user_data);
static void on_phone_clip_file_changed(GFileMonitor *monitor, GFile *file,
                                        GFile *other_file, GFileMonitorEvent ev_type,
                                        gpointer user_data);

#define VSHARING_PC_CLIP_FILE    "/tmp/vsharing_pc_clipboard.txt"
#define VSHARING_PHONE_CLIP_FILE "/tmp/vsharing_phone_clipboard.txt"
#define CLIP_DEBOUNCE_US         50000  /* 50ms debounce لمنع التكرار */

/* ============================================================
 * أدوات مساعدة داخلية
 * ============================================================ */

/* التحقق من توفر أداة في PATH */
static gboolean check_tool(const char *name) {
    gchar *path = g_find_program_in_path(name);
    if (path) {
        g_free(path);
        return TRUE;
    }
    return FALSE;
}

/* تطبيق النص على الحافظة عبر wl-copy أو xclip */
static gboolean set_clipboard_native(ClipboardEngine *engine, const char *text) {
    GError *err = NULL;
    gint std_in = -1;
    gsize text_len = strlen(text);

    /* محاولة wl-copy أولاً (Wayland) */
    if (engine->has_wl_copy) {
        gchar *argv[] = {"wl-copy", NULL};
        if (g_spawn_async_with_pipes(NULL, argv, NULL,
                                     G_SPAWN_SEARCH_PATH,
                                     NULL, NULL, NULL,
                                     &std_in, NULL, NULL, &err)) {
            if (write(std_in, text, text_len) == -1) {}
            close(std_in);
            return TRUE;
        }
        if (err) { g_error_free(err); err = NULL; }
    }

    /* محاولة xclip (X11) */
    if (engine->has_xclip) {
        gchar *argv[] = {"xclip", "-selection", "clipboard", NULL};
        if (g_spawn_async_with_pipes(NULL, argv, NULL,
                                     G_SPAWN_SEARCH_PATH,
                                     NULL, NULL, NULL,
                                     &std_in, NULL, NULL, &err)) {
            if (write(std_in, text, text_len) == -1) {}
            close(std_in);
            return TRUE;
        }
        if (err) { g_error_free(err); err = NULL; }
    }

    /* محاولة xdotool type (آخر خيار) */
    if (engine->has_xdotool) {
        gchar *argv[] = {"xdotool", "type", "--clearmodifiers", "--", (gchar*)text, NULL};
        g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
        return TRUE;
    }

    g_warning("[Clipboard] No clipboard tool found (wl-copy/xclip/xdotool)");
    return FALSE;
}

/* قراءة النص من الحافظة عبر wl-paste أو xclip */
static gchar *get_clipboard_native(ClipboardEngine *engine) {
    gchar *output = NULL;
    gint exit_status = 0;
    GError *err = NULL;

    if (engine->has_wl_copy) {
        gchar *argv[] = {"wl-paste", "--no-newline", NULL};
        if (g_spawn_sync(NULL, argv, NULL,
                         G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
                         NULL, NULL, &output, NULL, &exit_status, &err)) {
            if (exit_status == 0 && output && strlen(output) > 0) {
                return output;
            }
        }
        g_free(output); output = NULL;
        if (err) { g_error_free(err); err = NULL; }
    }

    if (engine->has_xclip) {
        gchar *argv[] = {"xclip", "-selection", "clipboard", "-o", NULL};
        if (g_spawn_sync(NULL, argv, NULL,
                         G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
                         NULL, NULL, &output, NULL, &exit_status, &err)) {
            if (exit_status == 0 && output && strlen(output) > 0) {
                return output;
            }
        }
        g_free(output); output = NULL;
        if (err) { g_error_free(err); err = NULL; }
    }

    return NULL;
}

/* ============================================================
 * مراقبة ملف الحافظة بـ GFileMonitor (بدلاً من Polling)
 * ============================================================ */
static void on_pc_clip_file_changed(GFileMonitor *monitor, GFile *file,
                                     GFile *other_file, GFileMonitorEvent ev_type,
                                     gpointer user_data) {
    (void)monitor; (void)file; (void)other_file;
    if (ev_type != G_FILE_MONITOR_EVENT_CHANGED &&
        ev_type != G_FILE_MONITOR_EVENT_CREATED) return;

    ClipboardEngine *engine = (ClipboardEngine *)user_data;
    if (!engine->enabled || engine->is_setting_clipboard) return;

    /* قراءة الملف */
    gchar *text = NULL;
    if (!g_file_get_contents(VSHARING_PC_CLIP_FILE, &text, NULL, NULL)) return;
    if (!text || strlen(text) == 0) { g_free(text); return; }

    clipboard_engine_on_pc_clipboard_changed(engine, text);
    g_free(text);
}

static void on_phone_clip_file_changed(GFileMonitor *monitor, GFile *file,
                                        GFile *other_file, GFileMonitorEvent ev_type,
                                        gpointer user_data) {
    (void)monitor; (void)file; (void)other_file;
    if (ev_type != G_FILE_MONITOR_EVENT_CHANGED &&
        ev_type != G_FILE_MONITOR_EVENT_CREATED) return;

    ClipboardEngine *engine = (ClipboardEngine *)user_data;
    if (!engine->enabled) return;

    gchar *text = NULL;
    if (!g_file_get_contents(VSHARING_PHONE_CLIP_FILE, &text, NULL, NULL)) return;
    if (!text || strlen(text) == 0) { g_free(text); return; }

    /* تجاهل التكرار */
    if (g_strcmp0(text, engine->last_text) == 0) {
        g_free(text);
        return;
    }

    /* Debounce: تجاهل التغييرات المتسارعة */
    int64_t now = g_get_monotonic_time();
    if (now - engine->last_change_time < CLIP_DEBOUNCE_US) {
        g_free(text);
        return;
    }

    g_free(engine->last_text);
    engine->last_text        = g_strdup(text);
    engine->last_change_time = now;

    /* تطبيق على الحافظة */
    engine->is_setting_clipboard = TRUE;
    set_clipboard_native(engine, text);
    engine->is_setting_clipboard = FALSE;

    /* إطلاق الـ callback */
    if (engine->on_changed) {
        ClipboardContent content = {
            .type         = CLIP_TYPE_TEXT,
            .text         = text,
            .timestamp_us = g_get_real_time(),
        };
        engine->on_changed(&content, engine->user_data);
    }

    g_print("[Clipboard] 📥 Phone → PC: %.60s%s\n",
            text, strlen(text) > 60 ? "..." : "");
    g_free(text);
}

/* ============================================================
 * إنشاء وتحرير المحرك
 * ============================================================ */

ClipboardEngine *clipboard_engine_new(ClipboardChangedCallback on_changed,
                                       gpointer user_data) {
    ClipboardEngine *engine = g_new0(ClipboardEngine, 1);
    engine->on_changed   = on_changed;
    engine->user_data    = user_data;
    engine->enabled      = FALSE;
    engine->has_wl_copy  = check_tool("wl-copy");
    engine->has_xclip    = check_tool("xclip");
    engine->has_xdotool  = check_tool("xdotool");

    g_print("[Clipboard] Tools detected: wl-copy=%s xclip=%s xdotool=%s\n",
            engine->has_wl_copy ? "✓" : "✗",
            engine->has_xclip   ? "✓" : "✗",
            engine->has_xdotool ? "✓" : "✗");

    /* مراقبة ملف حافظة الحاسوب */
    GFile *pc_file = g_file_new_for_path(VSHARING_PC_CLIP_FILE);
    GFileMonitor *pc_monitor = g_file_monitor_file(pc_file,
                                                     G_FILE_MONITOR_NONE,
                                                     NULL, NULL);
    if (pc_monitor) {
        g_signal_connect(pc_monitor, "changed",
                         G_CALLBACK(on_pc_clip_file_changed), engine);
        g_print("[Clipboard] Watching PC clipboard file (event-driven).\n");
    }
    g_object_unref(pc_file);

    /* مراقبة ملف حافظة الهاتف */
    GFile *phone_file = g_file_new_for_path(VSHARING_PHONE_CLIP_FILE);
    GFileMonitor *phone_monitor = g_file_monitor_file(phone_file,
                                                        G_FILE_MONITOR_NONE,
                                                        NULL, NULL);
    if (phone_monitor) {
        g_signal_connect(phone_monitor, "changed",
                         G_CALLBACK(on_phone_clip_file_changed), engine);
        g_print("[Clipboard] Watching Phone clipboard file (event-driven).\n");
    }
    g_object_unref(phone_file);

    return engine;
}

void clipboard_engine_free(ClipboardEngine *engine) {
    if (!engine) return;
    g_free(engine->last_text);
    g_free(engine);
}

void clipboard_engine_set_enabled(ClipboardEngine *engine, gboolean enabled) {
    if (!engine) return;
    engine->enabled = enabled;
    g_print("[Clipboard] Sync %s.\n", enabled ? "ENABLED" : "DISABLED");
}

gboolean clipboard_engine_set_text(ClipboardEngine *engine, const char *text) {
    if (!engine || !text) return FALSE;

    /* منع الحلقة */
    if (g_strcmp0(text, engine->last_text) == 0) return TRUE;

    engine->is_setting_clipboard = TRUE;

    /* تحديث الحالة الداخلية */
    g_free(engine->last_text);
    engine->last_text        = g_strdup(text);
    engine->last_change_time = g_get_monotonic_time();

    /* تطبيق على الحافظة */
    gboolean ok = set_clipboard_native(engine, text);

    /* كتابة الملف المشترك */
    g_file_set_contents(VSHARING_PHONE_CLIP_FILE, text, -1, NULL);

    engine->is_setting_clipboard = FALSE;

    g_print("[Clipboard] 📤 External → PC: %.60s%s\n",
            text, strlen(text) > 60 ? "..." : "");
    return ok;
}

gchar *clipboard_engine_get_text(ClipboardEngine *engine) {
    if (!engine) return NULL;
    return get_clipboard_native(engine);
}

void clipboard_engine_on_pc_clipboard_changed(ClipboardEngine *engine,
                                               const char *new_text) {
    if (!engine || !new_text || !engine->enabled) return;
    if (engine->is_setting_clipboard) return;
    if (g_strcmp0(new_text, engine->last_text) == 0) return;

    int64_t now = g_get_monotonic_time();
    if (now - engine->last_change_time < CLIP_DEBOUNCE_US) return;

    g_free(engine->last_text);
    engine->last_text        = g_strdup(new_text);
    engine->last_change_time = now;

    /* كتابة الملف لكي يرسله vsharingd للهاتف */
    g_file_set_contents(VSHARING_PC_CLIP_FILE, new_text, -1, NULL);

    if (engine->on_changed) {
        ClipboardContent content = {
            .type         = CLIP_TYPE_TEXT,
            .text         = (char *)new_text,
            .timestamp_us = g_get_real_time(),
        };
        engine->on_changed(&content, engine->user_data);
    }

    g_print("[Clipboard] 📤 PC → Phone: %.60s%s\n",
            new_text, strlen(new_text) > 60 ? "..." : "");
}

void clipboard_content_free(ClipboardContent *content) {
    if (!content) return;
    if (content->type == CLIP_TYPE_TEXT) {
        g_free(content->text);
    } else if (content->type == CLIP_TYPE_IMAGE) {
        g_free(content->image.data);
    } else if (content->type == CLIP_TYPE_FILES) {
        g_list_free_full(content->file_paths, g_free);
    }
}
