/*
 * input_controller.h — حقن أحداث اللمس والكيبورد عبر uinput
 *
 * يستقبل أحداث الإدخال القادمة من الهاتف عبر VLINK_INPUT_TOUCH / KEY
 * ويُعيد حقنها في نظام Linux عبر /dev/uinput كأجهزة افتراضية.
 *
 * يتطلب: صلاحية قراءة/كتابة على /dev/uinput
 *   (أضف المستخدم لمجموعة input: sudo usermod -aG input $USER)
 */

#pragma once

#include <glib.h>
#include <stdint.h>

/* ============================================================
 * الواجهة العامة
 * ============================================================ */

/**
 * input_ctrl_init:
 * يفتح /dev/uinput ويُنشئ جهازَي افتراضيَّين:
 *   - جهاز لمس متعدد النقاط (Multi-touch)
 *   - لوحة مفاتيح افتراضية
 * يُعيد TRUE عند النجاح.
 */
gboolean input_ctrl_init(void);

/**
 * input_ctrl_inject_touch:
 * @x_norm: إحداثي X مُعيَّار (0.0 – 1.0)
 * @y_norm: إحداثي Y مُعيَّار (0.0 – 1.0)
 * @action: 0=DOWN, 1=UP, 2=MOVE
 * @pointer_id: معرّف نقطة اللمس (0-9)
 * @screen_w: عرض شاشة الحاسوب بالبيكسل
 * @screen_h: ارتفاع شاشة الحاسوب بالبيكسل
 */
void input_ctrl_inject_touch(float x_norm, float y_norm,
                              uint8_t action, uint8_t pointer_id,
                              int screen_w, int screen_h);

/**
 * input_ctrl_inject_key:
 * @keycode: كود المفتاح (Linux evdev)
 * @action:  0=DOWN, 1=UP
 * @modifiers: Ctrl=1, Shift=2, Alt=4
 */
void input_ctrl_inject_key(uint32_t keycode, uint8_t action, uint8_t modifiers);

/**
 * input_ctrl_inject_scroll:
 * @dx, @dy: كمية التمرير (وحدات نسبية)
 */
void input_ctrl_inject_scroll(int32_t dx, int32_t dy);

/**
 * input_ctrl_is_available:
 * يُعيد TRUE إذا كان /dev/uinput قابلاً للوصول.
 */
gboolean input_ctrl_is_available(void);

/**
 * input_ctrl_cleanup:
 * يُدمر الأجهزة الافتراضية ويغلق /dev/uinput.
 */
void input_ctrl_cleanup(void);
