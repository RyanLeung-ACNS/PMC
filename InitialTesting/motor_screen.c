#include <string.h>

#include "lvgl.h"
#include "motor_screen.h"
#include "stepper3.h"
#include "link.h"
#include "limit.h"
#include "valve.h"
#include "ui.h"

/*
 * Motor override (Settings > Motor override) and Motor drive, on the screen
 * framework since 0.9.17 (Ryan, 16 Oct: tidy it and match the styling).
 * The 0.6.0 motor screen's functions, on the 10/127/244/361 grid:
 *   Motor override  position, motor state, step size, Close, Open, limit
 *                   switch, last move, Set zero, Coils off, Drive settings
 *   Motor drive     run, start and accel speeds, step mode, coil order,
 *                   direction, hold
 * Moves are relative, by the step size, in steps of the current mode. Drive
 * settings apply at once (not drafts) and are refused while moving, as on
 * the link; Save settings keeps them. Each action is echoed on the link as
 * "MOT screen: ..." so a terminal log shows what was done.
 */
#define RH 48
static const int32_t RY[3] = { 48, 104, 160 };
static const int32_t CX[4] = { 10, 127, 244, 361 };

static const int32_t step_sizes[5] = { 1, 10, 100, 1000, 10000 };
static const char *const step_names[5] = { "1", "10", "100", "1000", "10000" };
static int size_sel = 2;

// The three distinct coil orders for a four-phase unipolar motor; every other
// order is one of these turned or reversed.
static const uint8_t orders[3][4] = { { 0, 1, 2, 3 }, { 0, 2, 1, 3 }, { 0, 1, 3, 2 } };

// Tappable card: caption, value and a chevron, as the Settings fields.
static lv_obj_t *field(lv_obj_t *scr, int32_t x, int32_t y, int32_t w, const char *cap,
                       lv_event_cb_t cb, void *user)
{
    lv_obj_t *c = ui_card(scr, x, y, w, RH);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, UI_TICK, 0);
    lv_obj_set_style_bg_color(c, UI_TRACK, LV_STATE_PRESSED);
    lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, user);
    ui_label(c, 10, 5, w - 30, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, cap);
    lv_obj_t *v = ui_label(c, 10, 22, w - 30, &ui_font_16, UI_TEXT, LV_TEXT_ALIGN_LEFT, "");
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    ui_label(c, w - 20, 15, 0, &lv_font_montserrat_14, UI_IDLE, LV_TEXT_ALIGN_LEFT, LV_SYMBOL_RIGHT);
    return v;
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

// Why a move or setting is refused now, or NULL.
static const char *why_busy(void)
{
    if (stepper3_busy())
        return "Wait for the motor to stop";
    return stepper3_locked();
}

static void help_cb(lv_event_t *e)
{
    ui_text_view(lv_event_get_user_data(e) ? "Motor drive" : "Motor override",
                 lv_event_get_user_data(e) ? "Run speed: steps/s while moving. Start speed: steps/s at the "
                 "start and end of a move. Acceleration: the ramp between them.\n"
                 "Step mode: WAVE one coil, FULL two coils (full torque), HALF half-size steps; "
                 "changing it converts the count.\n"
                 "Coil order: the three distinct orders of a four-phase motor; try another if it "
                 "buzzes without turning.\n"
                 "Direction and Idle hold as MOT DIR and MOT HOLD.\n"
                 "These apply at once; Save settings keeps them."
                 : "Moves the motor directly by steps, + is open. Pick the step size, then Close or Open.\n"
                 "Close is refused while the limit switch is pressed (Limit override on Settings "
                 "allows one move past it).\n"
                 "Set zero makes this the 0 count: the valve calibration then counts from here.\n"
                 "Coils off stops at once and switches the coils off.\n"
                 "STOP stops a move where it is.");
}

/* --------------------------------------------------------------------------
 * Motor override
 * ------------------------------------------------------------------------ */
static struct {
    lv_obj_t *pos, *state, *size, *mv[2], *lim, *last, *zero, *off, *drive;
} mo;

static void size_pick(int i) { size_sel = i; }

static void size_cb(lv_event_t *e)
{
    (void)e;
    ui_pick("Step size, steps", step_names, 5, size_sel, size_pick);
}

static void move_cb(lv_event_t *e)
{
    int dir = lv_event_get_user_data(e) ? 1 : -1;
    int32_t steps = step_sizes[size_sel] * dir;
    const char *why = why_busy();
    if (!why && dir < 0 && stepper3_limit_open() && !stepper3_override())
        why = "Limit switch pressed, cannot close";
    if (why) {
        ui_msg(why);
        if (stepper3_locked())
            link_printf("MOT screen: move refused, %s\r\n", stepper3_locked());
    } else if (stepper3_move(steps)) {
        link_printf("MOT screen: move %ld\r\n", (long)steps);
    }
}

static void zero_go(void)
{
    const char *why = why_busy();
    if (why) {
        ui_msg(why);
        return;
    }
    stepper3_zero();
    link_printf("MOT screen: zero\r\n");
}

static void zero_cb(lv_event_t *e)
{
    if (ui_btn_disabled(e)) {
        ui_msg(why_busy() ? why_busy() : "Not now");
        return;
    }
    ui_confirm("Set the count to 0 here? The valve calibration then counts from this point.",
               "Set zero", zero_go);
}

static void off_cb(lv_event_t *e)
{
    (void)e;
    stepper3_off();
    link_printf("MOT screen: coils off at %ld\r\n", (long)stepper3_position());
}

static void drive_cb(lv_event_t *e)
{
    (void)e;
    ui_open(&ui_scr_motor_drive);
}

static void mo_build(lv_obj_t *scr)
{
    mo.pos = ui_info(scr, CX[0], RY[0], 226, RH, "Position, steps (+ open)");
    mo.state = ui_info(scr, CX[2], RY[0], 109, RH, "Motor");
    ui_btn(scr, CX[3], RY[0], 109, RH, UI_NAV, "Help", help_cb, NULL);

    mo.size = field(scr, CX[0], RY[1], 109, "Step size", size_cb, NULL);
    mo.mv[0] = ui_btn(scr, CX[1], RY[1], 109, RH, UI_SECONDARY, "Close", move_cb, NULL);
    mo.mv[1] = ui_btn(scr, CX[2], RY[1], 109, RH, UI_SECONDARY, "Open", move_cb, (void *)1);
    mo.drive = ui_btn(scr, CX[3], RY[1], 109, RH, UI_NAV, "Drive", drive_cb, NULL);

    mo.lim = ui_info(scr, CX[0], RY[2], 109, RH, "Limit switch");
    mo.last = ui_info(scr, CX[1], RY[2], 109, RH, "Last move");
    mo.zero = ui_btn(scr, CX[2], RY[2], 109, RH, UI_SECONDARY, "Set zero", zero_cb, NULL);
    mo.off = ui_btn(scr, CX[3], RY[2], 109, RH, UI_SECONDARY, "Coils off", off_cb, NULL);
}

// Position and motor state at 10 Hz, so they follow a move.
static void mo_fast(void)
{
    char t[24];
    lv_snprintf(t, sizeof t, "%ld", (long)stepper3_position());
    ui_set_text(mo.pos, t);
    int8_t d = stepper3_direction();
    ui_set_text(mo.state, d > 0 ? "Opening" : d < 0 ? "Closing" : "Idle");
    ui_set_text(mo.lim, limit_open_now() ? "Pressed" : "Released");
}

static void mo_refresh(void)
{
    static const char *const last[5] = { "None", "Done", "Stopped", "At switch", "F-13" };
    char t[24];
    mo_fast();
    ui_set_text(mo.size, step_names[size_sel]);
    stepper3_stop_t s = stepper3_last_stop();
    ui_set_text(mo.last, last[s <= STEPPER3_STOP_TIMEOUT ? s : 0]);
    bool busy = why_busy() != NULL;
    lv_snprintf(t, sizeof t, "%s steps", step_names[size_sel]);
    ui_btn_text(mo.mv[0], "Close", t);
    ui_btn_text(mo.mv[1], "Open", t);
    ui_btn_enable(mo.mv[0], !busy && (!stepper3_limit_open() || stepper3_override()));
    ui_btn_enable(mo.mv[1], !busy);
    ui_btn_enable(mo.zero, !busy);
    ui_btn_text(mo.off, "Coils off", stepper3_energised() ? "Coils are on" : "Coils are off");
    lv_snprintf(t, sizeof t, "%s, %lu/s", stepper3_mode_name(stepper3_mode()), (unsigned long)stepper3_rate());
    ui_btn_text(mo.drive, "Drive", t);

    // Faults first (with Reset fault); else what the limit switch allows.
    limit_state_t lim = limit_state();
    if (valve_fault_count())
        ui_status_line();
    else if (!stepper3_limit_fitted())
        ui_strip_set(UI_STRIP_INFO, UI_MUTED, "Limit switch not a stop (display only)");
    else if (lim == LIMIT_OPEN)
        ui_strip_set(UI_STRIP_PROMPT, UI_IDLE, "Limit switch pressed: Close blocked (at the seat or wire broken)");
    else if (lim == LIMIT_UNKNOWN)
        ui_strip_set(UI_STRIP_PROMPT, UI_IDLE, "Limit switch not read yet: Close blocked");
    else if (stepper3_busy())
        ui_strip_set(UI_STRIP_INFO, UI_MUTED, stepper3_direction() > 0 ? "Opening by steps" : "Closing by steps");
    else
        ui_strip_set(UI_STRIP_INFO, UI_MUTED, "Direct motor control by steps");
}

const ui_screen_t ui_scr_motor = { "Motor override", mo_build, mo_refresh, mo_fast };

/* --------------------------------------------------------------------------
 * Motor drive
 * ------------------------------------------------------------------------ */
static struct {
    lv_obj_t *rate, *start, *accel, *mode, *order, *dir, *hold;
} md;

enum { SET_RATE, SET_START, SET_ACCEL };

static bool rate_set(int32_t v, const char **why)
{
    if ((*why = why_busy()) != NULL)
        return false;
    if (!stepper3_set_rate((uint32_t)v)) {
        *why = "Not set";
        return false;
    }
    link_printf("MOT screen: rate %ld\r\n", (long)v);
    return true;
}

static bool start_set(int32_t v, const char **why)
{
    if ((*why = why_busy()) != NULL)
        return false;
    if ((uint32_t)v > stepper3_rate()) {
        *why = "Start speed must not exceed Run speed";
        return false;
    }
    if (!stepper3_set_start((uint32_t)v)) {
        *why = "Not set";
        return false;
    }
    link_printf("MOT screen: start %ld\r\n", (long)v);
    return true;
}

static bool accel_set(int32_t v, const char **why)
{
    if ((*why = why_busy()) != NULL)
        return false;
    if (!stepper3_set_accel((uint32_t)v)) {
        *why = "Not set";
        return false;
    }
    link_printf("MOT screen: accel %ld\r\n", (long)v);
    return true;
}

static void speed_cb(lv_event_t *e)
{
    static const char *const title[3] = { "Run speed, steps/s", "Start speed, steps/s", "Acceleration, steps/s/s" };
    static const char *const info[3] = { "Speed while moving", "Speed at the start and end of a move",
                                         "Ramp between start and run" };
    static bool (*const set[3])(int32_t, const char **) = { rate_set, start_set, accel_set };
    static char now[16];
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (why_busy()) {
        ui_msg(why_busy());
        return;
    }
    uint32_t v = k == SET_RATE ? stepper3_rate() : k == SET_START ? stepper3_start_rate() : stepper3_accel();
    lv_snprintf(now, sizeof now, "%lu", (unsigned long)v);
    ui_kp_t kp = {
        .title = title[k], .info = info[k], .dec = 0,
        .min = k == SET_ACCEL ? STEPPER3_ACCEL_MIN : STEPPER3_RATE_MIN,
        .max = k == SET_ACCEL ? STEPPER3_ACCEL_MAX : STEPPER3_RATE_MAX,
        .side_label = { "In use", NULL }, .side_text = { now, NULL },
        .on_set = set[k],
    };
    ui_keypad_ex(&kp);
}

static void mode_pick(int i)
{
    const char *why = why_busy();
    if (why)
        ui_msg(why);
    else if (stepper3_set_mode((stepper3_mode_t)i))
        link_printf("MOT screen: mode %s\r\n", stepper3_mode_name((stepper3_mode_t)i));
}

static void mode_cb(lv_event_t *e)
{
    (void)e;
    static const char *const names[3] = { "WAVE, one coil", "FULL, two coils", "HALF, half steps" };
    ui_pick("Step mode", names, 3, (int)stepper3_mode(), mode_pick);
}

static void order_pick(int i)
{
    const char *why = why_busy();
    if (why)
        ui_msg(why);
    else if (stepper3_set_order(orders[i]))
        link_printf("MOT screen: order %d%d%d%d\r\n", orders[i][0], orders[i][1], orders[i][2], orders[i][3]);
}

static void order_cb(lv_event_t *e)
{
    (void)e;
    static const char *const names[3] = { "0123", "0213", "0132" };
    ui_pick("Coil order", names, 3, order_index(), order_pick);
}

static void dir_pick(int i)
{
    const char *why = why_busy();
    if (why)
        ui_msg(why);
    else if (stepper3_set_reverse(i == 1))
        link_printf("MOT screen: dir %d\r\n", i);
}

static void dir_cb(lv_event_t *e)
{
    (void)e;
    static const char *const names[2] = { "Normal", "Reversed" };
    ui_pick("Direction", names, 2, stepper3_reverse() ? 1 : 0, dir_pick);
}

static void hold_pick(int i)
{
    const char *why = why_busy();
    if (why)
        ui_msg(why);
    else if (stepper3_set_hold(i == 1))
        link_printf("MOT screen: hold %d\r\n", i);
}

static void hold_cb(lv_event_t *e)
{
    (void)e;
    static const char *const names[2] = { "Off, coils off when idle", "On, coils hold when idle" };
    ui_pick("Idle hold", names, 2, stepper3_hold() ? 1 : 0, hold_pick);
}

static void md_build(lv_obj_t *scr)
{
    md.rate = field(scr, CX[0], RY[0], 109, "Run, steps/s", speed_cb, (void *)SET_RATE);
    md.start = field(scr, CX[1], RY[0], 109, "Start, steps/s", speed_cb, (void *)SET_START);
    md.accel = field(scr, CX[2], RY[0], 109, "Acceleration", speed_cb, (void *)SET_ACCEL);
    ui_btn(scr, CX[3], RY[0], 109, RH, UI_NAV, "Help", help_cb, (void *)1);
    md.mode = field(scr, CX[0], RY[1], 109, "Step mode", mode_cb, NULL);
    md.order = field(scr, CX[1], RY[1], 109, "Coil order", order_cb, NULL);
    md.dir = field(scr, CX[2], RY[1], 109, "Direction", dir_cb, NULL);
    md.hold = field(scr, CX[0], RY[2], 109, "Idle hold", hold_cb, NULL);
}

static void md_refresh(void)
{
    char t[16];
    lv_snprintf(t, sizeof t, "%lu", (unsigned long)stepper3_rate());
    ui_set_text(md.rate, t);
    lv_snprintf(t, sizeof t, "%lu", (unsigned long)stepper3_start_rate());
    ui_set_text(md.start, t);
    lv_snprintf(t, sizeof t, "%lu", (unsigned long)stepper3_accel());
    ui_set_text(md.accel, t);
    ui_set_text(md.mode, stepper3_mode_name(stepper3_mode()));
    uint8_t o[4];
    stepper3_order(o);
    lv_snprintf(t, sizeof t, "%d%d%d%d", o[0], o[1], o[2], o[3]);
    ui_set_text(md.order, t);
    ui_set_text(md.dir, stepper3_reverse() ? "Reversed" : "Normal");
    ui_set_text(md.hold, stepper3_hold() ? "On" : "Off");
    if (valve_fault_count())
        ui_status_line();
    else
        ui_strip_set(UI_STRIP_INFO, UI_MUTED, "Applies at once; Save settings keeps it");
}

const ui_screen_t ui_scr_motor_drive = { "Motor drive", md_build, md_refresh, NULL };
