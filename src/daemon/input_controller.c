#include "input_controller.h"
#include <gio/gio.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/uinput.h>
#include <linux/input-event-codes.h>

/* ============================================================
 * حالة داخلية
 * ============================================================ */
static int g_uinput_touch = -1;  /* fd جهاز اللمس */
static int g_uinput_kbd   = -1;  /* fd لوحة المفاتيح */

static int g_screen_w = 1920;
static int g_screen_h = 1080;

/* ─── دالة مساعدة: إرسال حدث uinput ─── */
static void emit(int fd, uint16_t type, uint16_t code, int32_t value) {
    struct input_event ev = {0};
    ev.type  = type;
    ev.code  = code;
    ev.value = value;
    if (write(fd, &ev, sizeof(ev)) < 0) {}
}

/* ─── إنشاء جهاز اللمس الافتراضي ─── */
static gboolean create_touch_device(void) {
    g_uinput_touch = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (g_uinput_touch < 0) {
        g_printerr("[InputCtrl] ❌ Cannot open /dev/uinput for touch: add user to 'input' group\n");
        return FALSE;
    }

    /* تفعيل أحداث اللمس */
    ioctl(g_uinput_touch, UI_SET_EVBIT,  EV_ABS);
    ioctl(g_uinput_touch, UI_SET_EVBIT,  EV_KEY);
    ioctl(g_uinput_touch, UI_SET_EVBIT,  EV_SYN);
    ioctl(g_uinput_touch, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(g_uinput_touch, UI_SET_KEYBIT, BTN_TOOL_FINGER);

    /* محاور اللمس المتعدد */
    ioctl(g_uinput_touch, UI_SET_ABSBIT, ABS_MT_SLOT);
    ioctl(g_uinput_touch, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
    ioctl(g_uinput_touch, UI_SET_ABSBIT, ABS_MT_POSITION_X);
    ioctl(g_uinput_touch, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
    ioctl(g_uinput_touch, UI_SET_ABSBIT, ABS_MT_PRESSURE);
    ioctl(g_uinput_touch, UI_SET_ABSBIT, ABS_X);
    ioctl(g_uinput_touch, UI_SET_ABSBIT, ABS_Y);

    struct uinput_user_dev uidev = {0};
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "VaxpLink Virtual Touch");
    uidev.id.bustype = BUS_VIRTUAL;
    uidev.id.vendor  = 0x1234;
    uidev.id.product = 0x5678;
    uidev.id.version = 1;

    /* نطاق المحاور */
    uidev.absmin[ABS_MT_SLOT]       = 0;
    uidev.absmax[ABS_MT_SLOT]       = 9;
    uidev.absmin[ABS_MT_TRACKING_ID]= 0;
    uidev.absmax[ABS_MT_TRACKING_ID]= 65535;
    uidev.absmin[ABS_MT_POSITION_X] = 0;
    uidev.absmax[ABS_MT_POSITION_X] = g_screen_w - 1;
    uidev.absmin[ABS_MT_POSITION_Y] = 0;
    uidev.absmax[ABS_MT_POSITION_Y] = g_screen_h - 1;
    uidev.absmin[ABS_MT_PRESSURE]   = 0;
    uidev.absmax[ABS_MT_PRESSURE]   = 255;
    uidev.absmin[ABS_X]             = 0;
    uidev.absmax[ABS_X]             = g_screen_w - 1;
    uidev.absmin[ABS_Y]             = 0;
    uidev.absmax[ABS_Y]             = g_screen_h - 1;

    if (write(g_uinput_touch, &uidev, sizeof(uidev)) < 0) {
        g_printerr("[InputCtrl] ❌ Failed to configure touch device\n");
        close(g_uinput_touch);
        g_uinput_touch = -1;
        return FALSE;
    }

    if (ioctl(g_uinput_touch, UI_DEV_CREATE) < 0) {
        g_printerr("[InputCtrl] ❌ UI_DEV_CREATE failed for touch\n");
        close(g_uinput_touch);
        g_uinput_touch = -1;
        return FALSE;
    }

    g_print("[InputCtrl] ✅ Virtual touch device created\n");
    return TRUE;
}

/* ─── إنشاء لوحة المفاتيح الافتراضية ─── */
static gboolean create_keyboard_device(void) {
    g_uinput_kbd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (g_uinput_kbd < 0) return FALSE;

    ioctl(g_uinput_kbd, UI_SET_EVBIT, EV_KEY);
    ioctl(g_uinput_kbd, UI_SET_EVBIT, EV_SYN);

    /* تفعيل جميع مفاتيح الكيبورد الشائعة */
    for (int k = KEY_ESC; k <= KEY_DELETE; k++)
        ioctl(g_uinput_kbd, UI_SET_KEYBIT, k);
    for (int k = KEY_HOME; k <= KEY_PAGEDOWN; k++)
        ioctl(g_uinput_kbd, UI_SET_KEYBIT, k);

    struct uinput_user_dev uidev = {0};
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "VaxpLink Virtual Keyboard");
    uidev.id.bustype = BUS_VIRTUAL;
    uidev.id.vendor  = 0x1234;
    uidev.id.product = 0x5679;
    uidev.id.version = 1;

    if (write(g_uinput_kbd, &uidev, sizeof(uidev)) < 0 ||
        ioctl(g_uinput_kbd,  UI_DEV_CREATE) < 0) {
        close(g_uinput_kbd);
        g_uinput_kbd = -1;
        return FALSE;
    }

    g_print("[InputCtrl] ✅ Virtual keyboard device created\n");
    return TRUE;
}

/* ============================================================
 * الواجهة العامة
 * ============================================================ */

gboolean input_ctrl_is_available(void) {
    return (access("/dev/uinput", W_OK) == 0);
}

gboolean input_ctrl_init(void) {
    if (!input_ctrl_is_available()) {
        g_printerr("[InputCtrl] ⚠️  /dev/uinput not writable — remote input disabled\n");
        g_printerr("[InputCtrl]    Fix: sudo usermod -aG input $USER && reboot\n");
        return FALSE;
    }
    gboolean t = create_touch_device();
    gboolean k = create_keyboard_device();
    return t || k;  /* نجاح جزئي مقبول */
}

void input_ctrl_inject_touch(float x_norm, float y_norm,
                              uint8_t action, uint8_t pointer_id,
                              int screen_w, int screen_h)
{
    if (g_uinput_touch < 0) return;

    int px = (int)(x_norm * (float)(screen_w  > 0 ? screen_w  : g_screen_w));
    int py = (int)(y_norm * (float)(screen_h  > 0 ? screen_h  : g_screen_h));

    /* SLOT → TRACKING_ID */
    emit(g_uinput_touch, EV_ABS, ABS_MT_SLOT, pointer_id);

    if (action == 1) {
        /* UP: tracking id = -1 لإنهاء اللمسة */
        emit(g_uinput_touch, EV_ABS, ABS_MT_TRACKING_ID, -1);
        emit(g_uinput_touch, EV_KEY, BTN_TOUCH, 0);
    } else {
        /* DOWN أو MOVE */
        emit(g_uinput_touch, EV_ABS, ABS_MT_TRACKING_ID, pointer_id + 1);
        emit(g_uinput_touch, EV_ABS, ABS_MT_POSITION_X,  px);
        emit(g_uinput_touch, EV_ABS, ABS_MT_POSITION_Y,  py);
        emit(g_uinput_touch, EV_ABS, ABS_MT_PRESSURE,    200);
        emit(g_uinput_touch, EV_ABS, ABS_X, px);
        emit(g_uinput_touch, EV_ABS, ABS_Y, py);
        if (action == 0)
            emit(g_uinput_touch, EV_KEY, BTN_TOUCH, 1);
    }
    emit(g_uinput_touch, EV_SYN, SYN_REPORT, 0);
}

void input_ctrl_inject_key(uint32_t keycode, uint8_t action, uint8_t modifiers) {
    if (g_uinput_kbd < 0) return;

    /* المعدّلات أولاً */
    if (modifiers & 0x01) emit(g_uinput_kbd, EV_KEY, KEY_LEFTCTRL,  action == 0 ? 1 : 0);
    if (modifiers & 0x02) emit(g_uinput_kbd, EV_KEY, KEY_LEFTSHIFT, action == 0 ? 1 : 0);
    if (modifiers & 0x04) emit(g_uinput_kbd, EV_KEY, KEY_LEFTALT,   action == 0 ? 1 : 0);

    emit(g_uinput_kbd, EV_KEY, (uint16_t)keycode, action == 0 ? 1 : 0);
    emit(g_uinput_kbd, EV_SYN, SYN_REPORT, 0);
}

void input_ctrl_inject_scroll(int32_t dx, int32_t dy) {
    if (g_uinput_touch < 0) return;
    /* نُرسل scroll عبر REL_WHEEL */
    emit(g_uinput_touch, EV_REL, REL_WHEEL,  -dy);
    emit(g_uinput_touch, EV_REL, REL_HWHEEL,  dx);
    emit(g_uinput_touch, EV_SYN, SYN_REPORT,   0);
}

void input_ctrl_cleanup(void) {
    if (g_uinput_touch >= 0) {
        ioctl(g_uinput_touch, UI_DEV_DESTROY);
        close(g_uinput_touch);
        g_uinput_touch = -1;
    }
    if (g_uinput_kbd >= 0) {
        ioctl(g_uinput_kbd, UI_DEV_DESTROY);
        close(g_uinput_kbd);
        g_uinput_kbd = -1;
    }
    g_print("[InputCtrl] 🧹 Virtual devices destroyed\n");
}
