#include <string.h>

#include "lvgl.h"
#include "motor_screen.h"
#include "stepper3.h"
#include "link.h"
#include "limit.h"
#include "valve.h"
#include "ui.h"

/*
 * Settings > Motor override (0.11.32: option B, Ryan 22 Oct). A framework
 * screen: the standard header, status strip and STOP; every MOT setting and
 * move from the touchscreen, and the screen follows MOT commands on the link.
 * Body y 48-208, five 28 px rows (y 48, 81, 114, 147, 180):
 *   left  x10 w226  position card (two rows), step size 1 to 10k,
 *                   Close / Open, Zero / Hold / Coils off
 *   right x244 w226 Mode WAVE/FULL/HALF, Run, Start and Accel [-] value [+],
 *                   Order / Dir
 * The strip reads the motor state and the limit line (a latched fault, an
 * armed override or a message over it, as on every screen).
 * Moves are relative, by the selected step size, in steps of the current
 * mode (half steps in HALF). Settings are refused while moving, as on the
 * link. Buttons act on press. Each action is echoed on the link as
 * "MOT screen: ..." so a terminal log shows what was done.
 */
#define TRACK lv_color_hex(0xC3CDD7)

static const int32_t R[5] = { 48, 81, 114, 147, 180 };
#define H 28

static struct {
    lv_obj_t *pos, *last;
    lv_obj_t *size[5], *mode[3];
    lv_obj_t *rate, *start, *accel, *adj[6];
    lv_obj_t *close, *open, *zero, *hold, *off, *order, *dir;
} m;
static int size_sel = 2;

static const int32_t step_sizes[5] = { 1, 10, 100, 1000, 10000 };
static const char *const step_names[5] = { "1", "10", "100", "1k", "10k" };

// +/- walk these lists; a value set over the link snaps to the nearest entry.
static const uint32_t rates[] = { 16, 25, 50, 75, 100, 125, 150, 200, 250, 300, 400, 500, 600, 800, 1000 };
static const uint32_t accels[] = { 25, 50, 100, 200, 400, 800, 1600, 3200, 6400 };

// The three distinct coil orders for a four-phase unipolar motor; every other
// order is one of these turned or reversed.
static const uint8_t orders[3][4] = { { 0, 1, 2, 3 }, { 0, 2, 1, 3 }, { 0, 1, 3, 2 } };

static intptr_t event_arg(lv_event_t *e)
{
    return (intptr_t)lv_event_get_user_data(e);
}

static size_t nearest(const uint32_t *list, size_t n, uint32_t v)
{
    size_t best = 0;
    for (size_t i = 1; i < n; i++) {
        uint32_t d = list[i] > v ? list[i] - v : v - list[i];
        uint32_t db = list[best] > v ? list[best] - v : v - list[best];
        if (d < db)
            best = i;
    }
    return best;
}

// Next list entry above (dir > 0) or below the current value.
static uint32_t step_list(const uint32_t *list, size_t n, uint32_t v, int dir)
{
    size_t i = nearest(list, n, v);
    if (dir > 0 && list[i] <= v && i + 1 < n)
        i++;
    else if (dir < 0 && list[i] >= v && i > 0)
        i--;
    return list[i];
}

static int order_index(void)
{
    uint8_t o[4];
    stepper3_order(o);
    for (int i = 0; i < 3; i++)
        if (memcmp(o, orders[i], 4) == 0)
            return i;
    return -1;
}

static void motor_fast(void);

/* --------------------------------------------------------------------------
 * Events (on press; a greyed button does nothing)
 * ------------------------------------------------------------------------ */
static void size_event(lv_event_t *e)
{
    size_sel = (int)event_arg(e);
    motor_fast();
}

static void move_event(lv_event_t *e)
{
    if (ui_btn_disabled(e))
        return;
    int32_t steps = step_sizes[size_sel] * (int32_t)event_arg(e);
    const char *why = stepper3_locked();
    if (!why && stepper3_move(steps)) {
        link_printf("MOT screen: move %ld\r\n", (long)steps);
    } else {
        if (!why)
            why = stepper3_refusal();
        link_printf("MOT screen: move refused, %s\r\n", why);
        ui_msg(why);
    }
    motor_fast();
}

static void mode_event(lv_event_t *e)
{
    if (stepper3_busy())
        return;
    stepper3_mode_t md = (stepper3_mode_t)event_arg(e);
    if (stepper3_set_mode(md))
        link_printf("MOT screen: mode %s\r\n", stepper3_mode_name(md));
    motor_fast();
}

enum { SET_RATE, SET_START, SET_ACCEL };

static void adjust_event(lv_event_t *e)
{
    if (ui_btn_disabled(e))
        return;
    intptr_t a = event_arg(e);
    int dir = a & 1 ? 1 : -1;
    switch (a >> 1) {
    case SET_RATE: {
        uint32_t v = step_list(rates, (sizeof rates / sizeof rates[0]), stepper3_rate(), dir);
        if (stepper3_set_rate(v))
            link_printf("MOT screen: rate %lu\r\n", (unsigned long)v);
        break;
    }
    case SET_START: {
        uint32_t v = step_list(rates, (sizeof rates / sizeof rates[0]), stepper3_start_rate(), dir);
        if (stepper3_set_start(v))
            link_printf("MOT screen: start %lu\r\n", (unsigned long)v);
        break;
    }
    default: {
        uint32_t v = step_list(accels, (sizeof accels / sizeof accels[0]), stepper3_accel(), dir);
        if (stepper3_set_accel(v))
            link_printf("MOT screen: accel %lu\r\n", (unsigned long)v);
        break;
    }
    }
    motor_fast();
}

static void order_event(lv_event_t *e)
{
    if (ui_btn_disabled(e))
        return;
    int i = (order_index() + 1) % 3;
    if (stepper3_set_order(orders[i]))
        link_printf("MOT screen: order %d%d%d%d\r\n",
                    orders[i][0], orders[i][1], orders[i][2], orders[i][3]);
    motor_fast();
}

static void dir_event(lv_event_t *e)
{
    if (ui_btn_disabled(e))
        return;
    if (stepper3_set_reverse(!stepper3_reverse()))
        link_printf("MOT screen: dir %d\r\n", stepper3_reverse() ? 1 : 0);
    motor_fast();
}

static void hold_event(lv_event_t *e)
{
    if (ui_btn_disabled(e))
        return;
    if (stepper3_set_hold(!stepper3_hold()))
        link_printf("MOT screen: hold %d\r\n", stepper3_hold() ? 1 : 0);
    motor_fast();
}

static void zero_event(lv_event_t *e)
{
    if (ui_btn_disabled(e))
        return;
    if (!stepper3_busy() && !stepper3_locked()) {
        stepper3_zero();
        link_printf("MOT screen: zero\r\n");
    }
    motor_fast();
}

static void off_event(lv_event_t *e)
{
    (void)e;
    stepper3_off();
    link_printf("MOT screen: coils off at %ld\r\n", (long)stepper3_position());
    motor_fast();
}

/* --------------------------------------------------------------------------
 * Layout
 * ------------------------------------------------------------------------ */
static lv_obj_t *btn(lv_obj_t *p, int32_t x, int32_t y, int32_t w, const char *text, lv_event_cb_t cb,
                     intptr_t arg)
{
    lv_obj_t *b = ui_btn(p, x, y, w, H, UI_SECONDARY, text, NULL, NULL);
    lv_obj_add_event_cb(b, cb, LV_EVENT_PRESSED, (void *)arg);
    return b;
}

// Segmented choice as Valve setup's tabs: grey track, the chosen segment
// raised white (seg_show).
static void seg(lv_obj_t *p, int32_t x, int32_t y, int32_t w, int n, const char *const *labels,
                lv_obj_t **out, lv_event_cb_t cb)
{
    lv_obj_t *t = ui_card(p, x, y, w, H);
    lv_obj_set_style_bg_color(t, TRACK, 0);
    lv_obj_set_style_border_width(t, 0, 0);
    lv_obj_set_style_radius(t, 6, 0);
    const int32_t pad = 3, iw = (w - 2 * pad) / n;
    for (int i = 0; i < n; i++) {
        lv_obj_t *b = ui_card(t, pad + i * iw, pad, iw, H - 2 * pad);
        lv_obj_set_style_radius(b, 4, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, cb, LV_EVENT_PRESSED, (void *)(intptr_t)i);
        lv_obj_t *l = ui_label(b, 0, 0, iw, &ui_font_16, UI_MUTED, LV_TEXT_ALIGN_CENTER, labels[i]);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
        out[i] = b;
    }
}

static void seg_show(lv_obj_t *b, bool sel, bool on)
{
    lv_opa_t o = sel ? LV_OPA_COVER : LV_OPA_TRANSP;
    if (lv_obj_get_style_bg_opa(b, 0) != o)
        lv_obj_set_style_bg_opa(b, o, 0);
    ui_set_color(lv_obj_get_child(b, 0), !on ? UI_IDLE : sel ? UI_TEXT : UI_MUTED);
}

// [-] value card [+]; returns the value label.
static lv_obj_t *adjust_row(lv_obj_t *p, int32_t x, int32_t y, int32_t w, int which)
{
    const int32_t bw = 32;
    m.adj[which * 2] = btn(p, x, y, bw, LV_SYMBOL_MINUS, adjust_event, which << 1);
    lv_obj_t *c = ui_card(p, x + bw + 4, y, w - 2 * bw - 8, H);
    lv_obj_t *v = ui_label(c, 0, 0, w - 2 * bw - 8, &ui_font_16, UI_TEXT, LV_TEXT_ALIGN_CENTER, "");
    lv_obj_align(v, LV_ALIGN_CENTER, 0, 0);
    m.adj[which * 2 + 1] = btn(p, x + w - bw, y, bw, LV_SYMBOL_PLUS, adjust_event, (which << 1) | 1);
    return v;
}

static void motor_build(lv_obj_t *scr)
{
    memset(&m, 0, sizeof m);

    // Left: position (two rows; room for -2147483648, Ryan 22 Oct) and motion.
    lv_obj_t *pc = ui_card(scr, 10, R[0], 226, R[1] + H - R[0]);
    ui_label(pc, 10, 5, 206, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, "Steps (+ open)");
    m.last = ui_label(pc, 10, 5, 206, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_RIGHT, "");
    m.pos = ui_label(pc, 0, 0, 210, &ui_num_26, UI_TEXT, LV_TEXT_ALIGN_RIGHT, "0");
    lv_obj_align(m.pos, LV_ALIGN_BOTTOM_RIGHT, -8, -4);
    seg(scr, 10, R[2], 226, 5, step_names, m.size, size_event);
    m.close = btn(scr, 10, R[3], 111, LV_SYMBOL_LEFT " Close", move_event, -1);
    m.open = btn(scr, 125, R[3], 111, "Open " LV_SYMBOL_RIGHT, move_event, 1);
    m.zero = btn(scr, 10, R[4], 72, "Zero", zero_event, 0);
    m.hold = btn(scr, 86, R[4], 72, "", hold_event, 0);
    m.off = btn(scr, 162, R[4], 74, "Coils off", off_event, 0);

    // Right: drive settings.
    const int32_t X = 244, LW = 48, CW = 226 - LW;
    static const char *const caps[4] = { "Mode", "Run", "Start", "Accel" };
    for (int i = 0; i < 4; i++)
        ui_label(scr, X, R[i] + 6, LW, &ui_font_16, UI_MUTED, LV_TEXT_ALIGN_LEFT, caps[i]);
    static const char *modes[3];
    for (int i = 0; i < 3; i++)
        modes[i] = stepper3_mode_name((stepper3_mode_t)i);
    seg(scr, X + LW, R[0], CW, 3, modes, m.mode, mode_event);
    m.rate = adjust_row(scr, X + LW, R[1], CW, SET_RATE);
    m.start = adjust_row(scr, X + LW, R[2], CW, SET_START);
    m.accel = adjust_row(scr, X + LW, R[3], CW, SET_ACCEL);
    m.order = btn(scr, X, R[4], 111, "", order_event, 0);
    m.dir = btn(scr, X + 115, R[4], 111, "", dir_event, 0);
    motor_fast();
}

/* --------------------------------------------------------------------------
 * Refresh: fast() at 10 Hz so the position follows a move; unchanged
 * values redraw nothing.
 * ------------------------------------------------------------------------ */
static void motor_fast(void)
{
    char t[48];
    bool busy = stepper3_busy();

    lv_snprintf(t, sizeof t, "%ld", (long)stepper3_position());
    ui_set_text(m.pos, t);
    stepper3_stop_t s = stepper3_last_stop();
    lv_snprintf(t, sizeof t, "Last move: %s", stepper3_stop_name(s));
    ui_set_text(m.last, t);
    ui_set_color(m.last, s == STEPPER3_STOP_LIMIT ? UI_FAULT : UI_MUTED);

    for (int i = 0; i < 5; i++)
        seg_show(m.size[i], i == size_sel, true);
    for (int i = 0; i < 3; i++)
        seg_show(m.mode[i], (int)stepper3_mode() == i, !busy);
    // Close is refused while the limit stop reads open; Open always works.
    ui_btn_enable(m.close, !busy && !stepper3_limit_open());
    ui_btn_enable(m.open, !busy);
    ui_btn_enable(m.zero, !busy && !stepper3_locked());

    lv_snprintf(t, sizeof t, "%lu sps", (unsigned long)stepper3_rate());
    ui_set_text(m.rate, t);
    lv_snprintf(t, sizeof t, "%lu sps", (unsigned long)stepper3_start_rate());
    ui_set_text(m.start, t);
    lv_snprintf(t, sizeof t, "%lu sps/s", (unsigned long)stepper3_accel());
    ui_set_text(m.accel, t);
    for (int i = 0; i < 6; i++)
        ui_btn_enable(m.adj[i], !busy);

    uint8_t o[4];
    stepper3_order(o);
    lv_snprintf(t, sizeof t, "Order %d%d%d%d", o[0], o[1], o[2], o[3]);
    ui_btn_text(m.order, t, NULL);
    ui_btn_text(m.dir, stepper3_reverse() ? "Dir reversed" : "Dir normal", NULL);
    ui_btn_text(m.hold, stepper3_hold() ? "Hold on" : "Hold off", NULL);
    ui_btn_enable(m.order, !busy);
    ui_btn_enable(m.dir, !busy);
    ui_btn_enable(m.hold, !busy);
}

// The strip: the motor state, then the limit switch and whether closing is
// allowed. A latched fault, an armed override or a message shows over it.
static void motor_refresh(void)
{
    char t[96];
    int8_t d = stepper3_direction();
    const char *st = d > 0 ? "Opening" : d < 0 ? "Closing"
                   : stepper3_energised() ? "Idle, coils on" : "Idle, coils off";
    limit_state_t lim = limit_state();
    const char *ls;
    bool warn = false;
    if (!stepper3_limit_fitted())
        ls = lim == LIMIT_CLOSED ? "Limit closed, stop off" : lim == LIMIT_OPEN ? "Limit OPEN, stop off"
                                                                                : "Limit not read yet";
    else if (lim == LIMIT_CLOSED)
        ls = "Limit closed, closing allowed";
    else {
        ls = lim == LIMIT_OPEN ? "Limit OPEN, closing blocked" : "Limit not read yet, closing blocked";
        warn = true;
    }
    lv_snprintf(t, sizeof t, "%s. %s", st, ls);
    // The status line's icons (arrows, valve symbol), without its sparkline.
    ui_strip_set(warn ? UI_STRIP_WARN : UI_STRIP_INFO, d > 0 ? LV_SYMBOL_UP : d < 0 ? LV_SYMBOL_DOWN : UI_ICON_VALVE, t);
    motor_fast();
}

const ui_screen_t ui_scr_motor = { "Motor override", motor_build, motor_refresh, motor_fast };
