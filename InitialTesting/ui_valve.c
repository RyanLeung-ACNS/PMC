#include <string.h>

#include "lvgl.h"
#include "pmc_config.h"
#include "clicks.h"
#include "limit.h"
#include "stepper3.h"
#include "valve.h"
#include "ui.h"

/* --------------------------------------------------------------------------
 * Operator screens for MANUAL (stage 5, simulator v63): Valve, Calibrate
 * valve, Faults and log, Diagnostics. The closed-switch question is in
 * ui.c, on every screen.
 * ------------------------------------------------------------------------ */

static const char *why_not_manual(void)
{
    if (valve_fault_count()) return "Reset the fault first";
    if (valve_mode() == VALVE_CALIBRATING) return "Calibrating: STOP to cancel";
    if (valve_mode() != VALVE_MANUAL) return "Calibrate the valve first";
    return NULL;
}

static void reset_cb(lv_event_t *e)
{
    (void)e;
    char left[40], t[64];
    valve_reset(left, sizeof left);
    if (left[0]) {
        lv_snprintf(t, sizeof t, "Not reset, cause still present: %s", left);
        ui_msg(t);
    }
}

static const char *pressure_text(char *buf, size_t n, bool *ok)
{
    const clicks_state_t *s = clicks_state();
    *ok = s->pm_found && s->reading_ok && !s->boost_tripped;
    return *ok ? ui_fmt1(buf, n, s->p_mmbar) : "----";
}

static const char *pct_text(char *buf, size_t n, int32_t pct10)
{
    return ui_fmt1(buf, n, (int64_t)pct10 * 100);
}

static void right(lv_obj_t *l)
{
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
}

/* --------------------------------------------------------------------------
 * Valve page
 * ------------------------------------------------------------------------ */
static struct {
    lv_obj_t *p, *v, *go, *fill, *btn[8];
    bool jog;                 // a Close or Open hold is moving the valve
} vp;

static const char *const valve_help =
    "Close and Open move the valve while held and stop when let go. "
    "-5 % and +5 % step from where the valve is going. Go to opens a keypad "
    "for a position, 0 closed to 100 open, one decimal. Fully close and Fully "
    "open ask first.\n\n"
    "The valve moves only in MANUAL with no fault. The closed switch stops "
    "any closing move; the valve does not close past it. If it is pressed "
    "during a move, the controller asks whether that point is 0 %.\n\n"
    "STOP stops the valve at once and holds it where it is.";

static bool vp_goto(int32_t pct10)
{
    const char *why = why_not_manual();
    if (!why && pct10 < valve_pct10() && limit_open_now())
        why = "At seat, cannot close";
    if (!why && !valve_goto_pct10(pct10 < 0 ? 0 : pct10 > 1000 ? 1000 : pct10, &why))
        why = "At seat, cannot close";
    if (why)
        ui_msg(why);
    return why == NULL;
}

static void jog_press(lv_event_t *e)
{
    bool open = lv_event_get_user_data(e) != NULL;
    vp.jog = vp_goto(open ? 1000 : 0);
}

static void jog_release(lv_event_t *e)
{
    (void)e;
    if (vp.jog)
        stepper3_stop_ramped();
    vp.jog = false;
}

static void step_cb(lv_event_t *e)
{
    int32_t d = (int32_t)(intptr_t)lv_event_get_user_data(e);
    vp_goto((stepper3_busy() ? valve_target_pct10() : valve_pct10()) + d);
}

static bool goto_set(int32_t v10, const char **why)
{
    if (!valve_goto_pct10(v10, why)) {
        *why = why_not_manual();
        if (!*why)
            *why = "At seat, cannot close";
        return false;
    }
    return true;
}

static void goto_cb(lv_event_t *e)
{
    (void)e;
    const char *why = why_not_manual();
    if (why) {
        ui_msg(why);
        return;
    }
    char v[16];
    ui_keypad("Go to, %", "0 closed, 100 open", 0, 1000, "Valve, %",
              pct_text(v, sizeof v, valve_pct10()), goto_set);
}

static void full_close(void) { vp_goto(0); }
static void full_open(void)  { vp_goto(1000); }

static void full_cb(lv_event_t *e)
{
    bool open = lv_event_get_user_data(e) != NULL;
    const char *why = why_not_manual();
    if (!why && !open && limit_open_now())
        why = "At seat, cannot close";
    if (why)
        ui_msg(why);
    else if (open)
        ui_confirm("Open fully, to 100 %?", "Open fully", full_open);
    else
        ui_confirm("Close fully, to 0 %?", "Close fully", full_close);
}

static void help_cb(lv_event_t *e)
{
    ui_text_view(lv_event_get_user_data(e) == valve_help ? "Valve" : "Calibrate valve",
                 lv_event_get_user_data(e));
}

static void valve_build(lv_obj_t *scr)
{
    memset(&vp, 0, sizeof vp);
    vp.p = ui_info(scr, 10, 48, 148, 48, "Pressure, mB");
    vp.v = ui_info(scr, 166, 48, 148, 48, "Valve, %");
    vp.go = ui_info(scr, 322, 48, 148, 48, "Go to, %");
    right(vp.p);
    right(vp.v);
    right(vp.go);
    lv_obj_t *card = lv_obj_get_parent(vp.v);
    lv_obj_t *track = ui_card(card, 10, 42, 128, 3);
    lv_obj_set_style_radius(track, 0, 0);
    lv_obj_set_style_bg_color(track, UI_TRACK, 0);
    vp.fill = ui_card(track, 0, 0, 0, 3);
    lv_obj_set_style_radius(vp.fill, 0, 0);
    lv_obj_set_style_bg_color(vp.fill, UI_FILL, 0);

    static const char *const row2[4] = { "Close", "-5 %", "+5 %", "Open" };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = ui_btn(scr, 10 + i * 118, 104, 106, 48, UI_SECONDARY, row2[i],
                             i == 1 || i == 2 ? step_cb : NULL, (void *)(intptr_t)(i == 1 ? -50 : 50));
        if (i == 0 || i == 3) {
            void *open = (void *)(intptr_t)(i == 3);
            lv_obj_add_event_cb(b, jog_press, LV_EVENT_PRESSED, open);
            lv_obj_add_event_cb(b, jog_release, LV_EVENT_RELEASED, NULL);
            lv_obj_add_event_cb(b, jog_release, LV_EVENT_PRESS_LOST, NULL);
            ui_btn_text(b, row2[i], "hold");
        }
        vp.btn[i] = b;
    }
    vp.btn[4] = ui_btn(scr, 10, 160, 106, 48, UI_PRIMARY, "Go to", goto_cb, NULL);
    vp.btn[5] = ui_btn(scr, 128, 160, 106, 48, UI_SECONDARY, "Fully close", full_cb, NULL);
    vp.btn[6] = ui_btn(scr, 246, 160, 106, 48, UI_SECONDARY, "Fully open", full_cb, (void *)1);
    ui_btn(scr, 364, 160, 106, 48, UI_NAV, "Help", help_cb, (void *)valve_help);
}

static void valve_refresh(void)
{
    char buf[16];
    bool ok;
    ui_set_text(vp.p, pressure_text(buf, sizeof buf, &ok));
    ui_set_color(vp.p, ok ? UI_TEXT : UI_FAULT);
    bool cal = valve_calibrated() && valve_mode() != VALVE_CALIBRATING;
    ui_set_text(vp.v, cal ? pct_text(buf, sizeof buf, valve_pct10()) : "----");
    ui_set_color(vp.v, cal ? UI_TEXT : UI_IDLE);
    int32_t p = valve_pct10();
    p = !cal || p < 0 ? 0 : p > 1000 ? 1000 : p;
    if (lv_obj_get_width(vp.fill) != p * 128 / 1000)
        lv_obj_set_width(vp.fill, p * 128 / 1000);
    bool moving = cal && stepper3_busy();
    ui_set_text(vp.go, moving ? pct_text(buf, sizeof buf, valve_target_pct10()) : "--");
    ui_set_color(vp.go, moving ? UI_TEXT : UI_IDLE);
    bool on = why_not_manual() == NULL;
    for (int i = 0; i < 7; i++)
        ui_btn_enable(vp.btn[i], on);
    if (vp.jog && !stepper3_busy())
        vp.jog = false;
    ui_status_line();
}

const ui_screen_t ui_scr_valve = { "Valve", valve_build, valve_refresh };

/* --------------------------------------------------------------------------
 * Calibrate valve
 * ------------------------------------------------------------------------ */
static struct {
    lv_obj_t *state, *v, *last, *seat, *start;
} cp;

static const char *const calib_help =
    "Calibration finds 0 % from the closed switch. If the switch is pressed at "
    "the start, the valve first moves off it. Then three steps: 1 closes fast "
    "until the switch opens, 2 backs off a short way, 3 closes slowly to the "
    "switch. That point is 0 %, and the valve is in MANUAL.\n\n"
    "It takes up to a few minutes from fully open. STOP cancels it and the "
    "valve is then not calibrated. If the switch is not found (F-07) or stays "
    "pressed after moving off it (F-09), check the switch and its wiring, "
    "reset the fault and calibrate again.";

static void start_cb(lv_event_t *e)
{
    (void)e;
    if (valve_mode() == VALVE_CALIBRATING)
        ui_msg("Calibrating: STOP to cancel");
    else if (valve_fault_count())
        ui_msg("Reset the fault first");
    else
        ui_calibrate_ask();
}

static void calib_build(lv_obj_t *scr)
{
    cp.state = ui_info(scr, 10, 48, 226, 48, "Valve calibration");
    cp.v = ui_info(scr, 244, 48, 109, 48, "Valve, %");
    right(cp.v);
    cp.last = ui_info(scr, 10, 104, 226, 48, "Last calibrated");
    cp.seat = ui_info(scr, 244, 104, 109, 48, "Seat switch");
    ui_btn(scr, 361, 48, 109, 48, UI_NAV, "Help", help_cb, (void *)calib_help);
    cp.start = ui_btn(scr, 361, 104, 109, 48, UI_PRIMARY, "Start calibration", start_cb, NULL);
}

static void calib_refresh(void)
{
    char buf[40], w[16];
    bool calib = valve_mode() == VALVE_CALIBRATING;
    int step = valve_cal_step();
    if (calib) {
        lv_snprintf(buf, sizeof buf, "Calibrating, step %d of 3", step < 1 ? 1 : step);
        ui_set_text(cp.state, step == 0 ? "Calibrating, moving off" : buf);
    } else {
        ui_set_text(cp.state, valve_calibrated() ? "Calibrated" : "Not calibrated");
    }
    bool cal = valve_calibrated() && !calib;
    ui_set_text(cp.v, cal ? pct_text(buf, sizeof buf, valve_pct10()) : "----");
    ui_set_color(cp.v, cal ? UI_TEXT : UI_IDLE);
    uint32_t age, took;
    if (valve_last_cal(&age, &took)) {
        lv_snprintf(buf, sizeof buf, "%s, took %lu s", ui_when(w, sizeof w, age), (unsigned long)took);
        ui_set_text(cp.last, buf);
    } else {
        ui_set_text(cp.last, "Not since power-up");
    }
    ui_set_text(cp.seat, limit_open_now() ? "At seat" : "Off seat");
    ui_btn_text(cp.start, calib ? "Calibrating" : "Start calibration", NULL);
    ui_btn_enable(cp.start, !calib && !valve_fault_count());
    ui_status_line();
}

const ui_screen_t ui_scr_calib = { "Calibrate valve", calib_build, calib_refresh };

/* --------------------------------------------------------------------------
 * Faults and log
 * ------------------------------------------------------------------------ */
#define LOG_ROWS 40
static struct {
    lv_obj_t *list, *none, *reset, *log;
    int nf;                   // rows built
    char sig[64];             // tags shown, to rebuild on change
    uint32_t log_total;
    int detail;               // fault shown in the detail, -1 none
    lv_obj_t *d_title, *d_when, *d_text[4], *d_next;
} fp;
static char log_text[LOG_ROWS * 112];

typedef struct {
    const char *tag, *happened, *controller, *check, *reset;
} fault_help_t;

static const fault_help_t fault_help[] = {
    { "F-01", "The Druck reading is missing or outside 0.005-11 mV/V.",
      "Stopped the valve and holds it. Pressure shows ----.",
      "The Druck cable and plug, and the Power Monitor Click.",
      "The reading is back." },
    { "F-03", "The Druck excitation left 9.5-10.5 V, or the Boost 10 tripped.",
      "Stopped the valve and holds it.",
      "The Boost 10 and the 5 V supply. A trip needs a power cycle.",
      "The excitation is back in the window." },
    { "F-07", "Calibrating, the valve closed 1.1 x full travel without the closed switch.",
      "Stopped. The valve is not calibrated.",
      "The switch and its wire, and that the valve turns.",
      "Any time. Then calibrate again." },
    { "F-09", "The closed switch stayed pressed after the valve moved off it.",
      "Stopped. The valve is not calibrated.",
      "The switch is not stuck (ice) and its wire is not broken.",
      "Any time. Then calibrate again." },
    { "F-13", "A move took longer than its distance at the set rate, plus 2 s.",
      "Stopped the valve and holds it. Calibration kept.",
      "The valve turns freely and the motor 12 V supply.",
      "Any time." },
    { "F-12", "STOP was pressed, or CTRL STOP sent.",
      "Stopped the valve and holds it, not closed.",
      "Why it was stopped.",
      "Any time." },
};

static const fault_help_t *help_for(const char *tag)
{
    for (size_t i = 0; i < sizeof fault_help / sizeof fault_help[0]; i++)
        if (strcmp(fault_help[i].tag, tag) == 0)
            return &fault_help[i];
    return &fault_help[0];
}

static const char *mode_word(valve_mode_t m)
{
    return m == VALVE_CALIBRATING ? "CALIBRATING" : m == VALVE_MANUAL ? "MANUAL" : "NOT CALIBRATED";
}

static void detail_fill(void)
{
    valve_fault_t f;
    int n = valve_fault_count();
    if (fp.detail >= n)
        fp.detail = n - 1;
    if (fp.detail < 0 || !valve_fault_get(fp.detail, &f)) {
        ui_overlay_close();
        fp.detail = -1;
        return;
    }
    const fault_help_t *h = help_for(f.tag);
    char buf[160], w[16];
    lv_snprintf(buf, sizeof buf, "%s  %s", f.tag, f.text);
    ui_set_text(fp.d_title, buf);
    lv_snprintf(buf, sizeof buf, "%s, %s", ui_when(w, sizeof w, f.age_ms), mode_word(f.mode));
    ui_set_text(fp.d_when, buf);
    ui_set_text(fp.d_text[0], h->happened);
    ui_set_text(fp.d_text[1], h->controller);
    ui_set_text(fp.d_text[2], h->check);
    lv_snprintf(buf, sizeof buf, "%s%s", h->reset, f.can_reset ? " It can be reset now." : " Not yet.");
    ui_set_text(fp.d_text[3], buf);
    lv_snprintf(buf, sizeof buf, "%d of %d", fp.detail + 1, n);
    ui_btn_text(fp.d_next, "Next fault", buf);
    ui_btn_enable(fp.d_next, n > 1);
}

static void d_next_cb(lv_event_t *e)
{
    (void)e;
    int n = valve_fault_count();
    if (n > 1) {
        fp.detail = (fp.detail + 1) % n;
        detail_fill();
    }
}

static void d_reset_cb(lv_event_t *e)
{
    reset_cb(e);
    if (valve_fault_count())
        detail_fill();
    else {
        ui_overlay_close();
        fp.detail = -1;
    }
}

static void d_close_cb(lv_event_t *e)
{
    (void)e;
    ui_overlay_close();
    fp.detail = -1;
}

static void detail_open(int i)
{
    lv_obj_t *o = ui_overlay();
    fp.detail = i;
    lv_obj_t *c = ui_card(o, 10, 8, 460, 160);
    fp.d_title = ui_label(c, 10, 6, 260, &ui_font_16, UI_FAULT, LV_TEXT_ALIGN_LEFT, "");
    fp.d_when = ui_label(c, 270, 8, 180, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_RIGHT, "");
    static const char *const caps[4] = { "Happened", "Controller", "Check", "Reset when" };
    for (int k = 0; k < 4; k++) {
        ui_label(c, 10, 30 + k * 32, 84, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, caps[k]);
        fp.d_text[k] = ui_label(c, 98, 30 + k * 32, 352, &ui_font_12, UI_TEXT, LV_TEXT_ALIGN_LEFT, "");
        lv_label_set_long_mode(fp.d_text[k], LV_LABEL_LONG_WRAP);
    }
    fp.d_next = ui_btn(o, 10, 176, 109, 48, UI_SECONDARY, "Next fault", d_next_cb, NULL);
    ui_btn(o, 127, 176, 109, 48, UI_PRIMARY, "Reset fault", d_reset_cb, NULL);
    ui_btn(o, 244, 176, 109, 48, UI_SECONDARY, "Close", d_close_cb, NULL);
    detail_fill();
}

static void row_cb(lv_event_t *e)
{
    detail_open((int)(intptr_t)lv_event_get_user_data(e));
}

static void faults_reset_cb(lv_event_t *e)
{
    if (ui_btn_disabled(e)) {
        ui_msg("No fault to reset");
        return;
    }
    reset_cb(e);
}

static void faults_build(lv_obj_t *scr)
{
    memset(&fp, 0, sizeof fp);
    fp.detail = -1;
    fp.nf = -1;
    fp.list = ui_card(scr, 10, 48, 343, 48);
    lv_obj_add_flag(fp.list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(fp.list, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(fp.list, LV_SCROLL_SNAP_START);
    fp.reset = ui_btn(scr, 361, 48, 109, 48, UI_PRIMARY, "Reset fault", faults_reset_cb, NULL);

    lv_obj_t *ev = ui_card(scr, 10, 104, 460, 104);
    ui_label(ev, 10, 4, 300, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, "Events, newest first");
    lv_obj_t *box = lv_obj_create(ev);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, 10, 22);
    lv_obj_set_size(box, 446, 80);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    fp.log = ui_label(box, 0, 0, 436, &ui_font_12, UI_TEXT, LV_TEXT_ALIGN_LEFT, "");
    lv_label_set_long_mode(fp.log, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(fp.log, 2, 0);
    fp.log_total = (uint32_t)-1;
}

static void faults_rows(void)
{
    char sig[64] = "";
    valve_fault_t f;
    int n = valve_fault_count();
    for (int i = 0; i < n && valve_fault_get(i, &f); i++) {
        size_t len = strlen(sig);
        lv_snprintf(sig + len, sizeof sig - len, "%s ", f.tag);
    }
    if (n == fp.nf && strcmp(sig, fp.sig) == 0)
        return;
    fp.nf = n;
    lv_strlcpy(fp.sig, sig, sizeof fp.sig);
    lv_obj_clean(fp.list);
    if (!n) {
        ui_label(fp.list, 10, 14, 300, &ui_font_16, UI_IDLE, LV_TEXT_ALIGN_LEFT, "No faults");
        return;
    }
    for (int i = 0; i < n && valve_fault_get(i, &f); i++) {
        lv_obj_t *r = lv_obj_create(fp.list);
        lv_obj_remove_style_all(r);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(r, 0, i * 48);
        lv_obj_set_size(r, 343, 48);
        lv_obj_add_flag(r, LV_OBJ_FLAG_SCROLL_ON_FOCUS | LV_OBJ_FLAG_SNAPPABLE);
        lv_obj_add_event_cb(r, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        char t[48], w[16];
        lv_snprintf(t, sizeof t, "%s  %s", f.tag, f.text);
        ui_label(r, 10, 5, 300, &ui_font_16, UI_FAULT, LV_TEXT_ALIGN_LEFT, t);
        lv_snprintf(t, sizeof t, "%s  %s", ui_when(w, sizeof w, f.age_ms), mode_word(f.mode));
        ui_label(r, 10, 27, 300, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, t);
        ui_label(r, 318, 14, 0, &lv_font_montserrat_14, UI_IDLE, LV_TEXT_ALIGN_LEFT, LV_SYMBOL_RIGHT);
    }
}

static void faults_log(void)
{
    uint32_t total = valve_event_total();
    if (total == fp.log_total)
        return;
    fp.log_total = total;
    size_t len = 0;
    log_text[0] = '\0';
    for (int i = 0; i < LOG_ROWS; i++) {
        uint32_t age;
        const char *tag, *text;
        char w[16];
        if (!valve_event_get(i, &age, &tag, &text))
            break;
        lv_snprintf(log_text + len, sizeof log_text - len, "%s%s  %s  %s", i ? "\n" : "",
                    ui_when(w, sizeof w, age), tag, text);
        len += strlen(log_text + len);
    }
    lv_label_set_text_static(fp.log, log_text);
}

static void faults_refresh(void)
{
    faults_rows();
    faults_log();
    ui_btn_enable(fp.reset, valve_fault_count() > 0);
    if (fp.detail >= 0 && ui_overlay_open())
        detail_fill();
    else
        fp.detail = -1;
    char t[48];
    int n = valve_fault_count();
    lv_snprintf(t, sizeof t, n ? "%d latched. Tap one for detail." : "No faults latched", n);
    ui_strip_set(UI_STRIP_INFO, UI_MUTED, t);
}

const ui_screen_t ui_scr_faults = { "Faults and log", faults_build, faults_refresh };

/* --------------------------------------------------------------------------
 * Diagnostics
 * ------------------------------------------------------------------------ */
static struct {
    lv_obj_t *v[9];
} dp;

// Valve motor row: position, limit switch and steps past it, at 50 Hz so
// the switch shows as the hardware reads it (limit.c samples every 5 ms).
static void diag_motor(lv_timer_t *t)
{
    (void)t;
    char buf[16];
    lv_snprintf(buf, sizeof buf, "%ld", (long)stepper3_position());
    ui_set_text(dp.v[3], buf);
    bool pressed = limit_open_now();
    ui_set_text(dp.v[4], pressed ? "Pressed" : "Not pressed");
    int32_t beyond;
    if (pressed && valve_beyond_switch(&beyond))
        lv_snprintf(buf, sizeof buf, "%ld", (long)beyond);
    else
        lv_strlcpy(buf, "--", sizeof buf);
    ui_set_text(dp.v[5], buf);
}

static void diag_deleted(lv_event_t *e)
{
    lv_timer_delete(lv_event_get_user_data(e));
}

static void diag_build(lv_obj_t *scr)
{
    static const char *const rows[3] = { "Pressure sensor", "Valve motor", "Last move" };
    static const char *const caps[9] = { "Signal, mV", "Supply, V", "Before cal, mB",
                                         "Position, steps", "Limit switch", "Beyond limit",
                                         "Result", "Moved, steps", "Took, s" };
    static const int32_t x[3] = { 98, 225, 351 };
    for (int r = 0; r < 3; r++) {
        lv_obj_t *l = ui_label(scr, 10, 48 + r * 56, 80, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, rows[r]);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, 10, 48 + r * 56 + 9);
        for (int c = 0; c < 3; c++)
            dp.v[r * 3 + c] = ui_info(scr, x[c], 48 + r * 56, 119, 48, caps[r * 3 + c]);
    }
    lv_timer_t *t = lv_timer_create(diag_motor, 20, NULL);
    lv_obj_add_event_cb(scr, diag_deleted, LV_EVENT_DELETE, t);
    diag_motor(t);
}

static void diag_refresh(void)
{
    const clicks_state_t *s = clicks_state();
    char buf[48];
    bool sig = s->pm_found && !s->pm_diag;
    int32_t uv = s->shunt_nv / 1000;    // Druck output, 0.001 mV
    int32_t au = uv < 0 ? -uv : uv;
    lv_snprintf(buf, sizeof buf, "%s%ld.%03ld", uv < 0 ? "-" : "", (long)(au / 1000), (long)(au % 1000));
    ui_set_text(dp.v[0], sig ? buf : "----");
    lv_snprintf(buf, sizeof buf, "%ld.%02ld", (long)(s->bus_mv / 1000), (long)(s->bus_mv % 1000 / 10));
    ui_set_text(dp.v[1], sig ? buf : "----");
    ui_set_text(dp.v[2], sig ? ui_fmt1(buf, sizeof buf, (int64_t)s->r_ppb * 1034214 / 10000000) : "----");

    int32_t from;
    uint32_t ms;
    bool any = stepper3_last_move(&from, &ms);
    const char *res = "None yet";
    if (stepper3_busy() && any) res = "Moving";
    else switch (stepper3_last_stop()) {
        case STEPPER3_STOP_DONE:    res = "At target"; break;
        case STEPPER3_STOP_LIMIT:   res = "At limit"; break;
        case STEPPER3_STOP_CMD:     res = "Stopped"; break;
        case STEPPER3_STOP_TIMEOUT: res = "Timed out"; break;
        default: break;
    }
    ui_set_text(dp.v[6], res);
    lv_snprintf(buf, sizeof buf, "%ld", (long)(stepper3_position() - from));
    ui_set_text(dp.v[7], any ? buf : "--");
    ui_set_text(dp.v[8], any ? ui_fmt1(buf, sizeof buf, ms) : "--");

    char t[16];
    lv_snprintf(buf, sizeof buf, "Firmware %s. Board %s \xC2\xB0" "C. Link %s.", PMC_FW_VERSION,
                ui_fmt1(t, sizeof t, s->die_mc), PMC_LINK_ETHERNET ? "Ethernet" : "USB");
    ui_strip_set(UI_STRIP_INFO, UI_MUTED, buf);
}

const ui_screen_t ui_scr_diag = { "Diagnostics", diag_build, diag_refresh };
