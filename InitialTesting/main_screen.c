#include <stdarg.h>
#include <string.h>

#include "main_screen.h"
#include "link.h"
#include "clicks.h"
#include "cal.h"
#include "sysinfo.h"
#include "rtclock.h"
#include "uncert.h"
#include "pmc_config.h"
#include "motor_screen.h"
#include "limit.h"
#include "stepper3.h"
#include "fixed.h"
#include "valve.h"

lvgl_main_screen_ui_t lvgl_main_screen_ui;

/*
 * Screen layout, 480 x 272:
 *   y   0-30   header: PMC, clock (RTC), Druck ADC (Power Monitor) / excitation (Boost 10) / link
 *   y  38-130  pressure card: calibrated value (48 px), unit, ratio, sensor / fault line;
 *              on the right the 1 min pressure history line and the pressure bar
 *   y 138-252  Chip readings / Calibration panels (2 x 2 fields), tab buttons
 *              along the bottom; Chip readings shown at start-up. The third
 *              button, Motor, opens the motor screen (motor_screen.c).
 *   y 256-270  firmware and reset cause, separate from fault messages
 * Pressure and faults remain visible while switching between detail tabs.
 * Excitation is fixed at 10.00 V nominal (clicks_init); the screen only reads it.
 * Full per-board status lines go to COM3 only, when they change.
 */
#define COL_HEADER   lv_color_make(31, 41, 51)
#define COL_CARD     lv_color_white()
#define COL_TEXT     lv_color_make(31, 41, 51)
#define COL_MUTED    lv_color_make(84, 99, 116)
#define COL_OK       lv_color_make(46, 160, 67)
#define COL_FAULT    lv_color_make(214, 48, 49)
#define COL_IDLE     lv_color_make(150, 158, 166)
#define COL_WARN     lv_color_make(214, 130, 0)

static lv_obj_t *chip_pm, *chip_boost, *chip_lim, *chip_usb;
static lv_obj_t *clock_label;
static lv_obj_t *pressure_label;
static lv_obj_t *pressure_sub;
static lv_obj_t *pressure_card;
static lv_font_t pressure_font;
static lv_obj_t *detail_cal, *detail_raw, *tab_cal, *tab_raw;
static lv_obj_t *val_signal, *val_exc, *val_die, *val_pg;
static lv_obj_t *pressure_unc;
static lv_obj_t *cal_table, *cal_vac, *cal_air, *cal_kind, *cal_store;

/*
 * Pressure history line and pressure bar, as the simulator's pressure tile
 * (simulator/index.html, homeTile, drawSpark; STYLE-GUIDE.md section 6), in
 * the right of the pressure card:
 *   history line 112 x 32, one pen, no axes, the last 1 min at one point a
 *   second, a dot on the newest point. The scale never clips: linear over
 *   the window's readings, at least 1 mbar tall; log when the readings span
 *   more than a decade (pump-down). No reading (shown as ----) leaves a gap.
 *   pressure bar 8 px, log 0.1 to 1000 mbar, filled to the reading, lines at
 *   each decade, labels 0.1, 10 and 1000 centred on their lines, a margin of
 *   half the width of "1000" at each end, a 3 x 14 marker. Empty with no
 *   reading.
 * The LVGL heap is nearly full, so both are one object that draws itself
 * (side_draw_event); the points are kept here as pixel rows.
 */
#define SIDE_X        336    // left of the history line and pressure bar, in the card
#define SIDE_CAP_W    44     // "1 min", left of the history line
#define HIST_POINTS   60
#define HIST_EVERY_S  1
#define HIST_W        112
#define HIST_H        32
#define HIST_PAD      2      // top and bottom, as the simulator
#define HIST_PAD_R    3      // room for the dot on the newest point
#define PBAR_W        112
#define PBAR_Y        38     // top of the bar; the marker is 3 px above and below
#define PBAR_PAD      15     // half the width of "1000" at 12 px
#define SIDE_H        62     // down to the bottom of the bar labels
#define COL_PEN       lv_color_make(45, 88, 119)     // #2D5877
#define COL_SPARK_BG  lv_color_make(243, 246, 248)   // #F3F6F8
#define COL_PAGE      lv_color_make(232, 238, 244)
#define COL_PFILL     lv_color_make(123, 135, 148)   // #7B8794
#define COL_PTICK     lv_color_make(163, 175, 187)   // #A3AFBB

static int32_t hist_mmbar[HIST_POINTS];   // oldest first, in 0.001 mbar
static bool hist_ok[HIST_POINTS];
static int8_t hist_y[HIST_POINTS];        // pixel row in the line, -1 = gap
static int hist_tick;                     // status ticks since the last point

static lv_obj_t *side;
static int32_t pbar_x = -1;               // fill width in px, -1 = no reading

#define STATUS_LINES 6
static const char *last_line[STATUS_LINES];
static char line_copy[STATUS_LINES][192];

/* Reuse the existing glyph bitmaps, with tabular advances for the readout.
 * Right alignment alone cannot stop proportional digits moving the decimal.
 * Disabling kerning also keeps neighbouring digit pairs from changing width. */
static bool pressure_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *dsc,
                               uint32_t letter, uint32_t next)
{
    (void)font;
    (void)next;
    if (!lv_font_montserrat_48.get_glyph_dsc(&lv_font_montserrat_48, dsc, letter, 0))
        return false;
    if ((letter >= '0' && letter <= '9') || letter == '-') {
        dsc->adv_w = 32;
        dsc->ofs_x = (32 - (int32_t)dsc->box_w) / 2;
    }
    return true;
}

static lv_obj_t *make_card(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, COL_CARD, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 6, 0);
    lv_obj_set_style_pad_all(c, 6, 0);
    return c;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, lv_color_t col)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, col, 0);
    return l;
}

static void detail_tab_event(lv_event_t *e)
{
    lv_obj_t *target = lv_event_get_target_obj(e);
    if (lv_obj_has_state(target, LV_STATE_CHECKED))
        return;                                     // already showing
    bool calibration = target == tab_cal;
    lv_obj_t *shown = calibration ? detail_cal : detail_raw;
    lv_obj_t *hidden = calibration ? detail_raw : detail_cal;
    lv_obj_remove_flag(shown, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(hidden, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_state(calibration ? tab_cal : tab_raw, LV_STATE_CHECKED);
    lv_obj_remove_state(calibration ? tab_raw : tab_cal, LV_STATE_CHECKED);
}

static void motor_button_event(lv_event_t *e)
{
    (void)e;
    motor_screen_show();
}

static lv_obj_t *make_tab(lv_obj_t *parent, int32_t x, int32_t width, const char *text)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_pos(button, x, 74);
    lv_obj_set_size(button, width, 28);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_radius(button, 4, 0);
    lv_obj_set_style_bg_color(button, lv_color_make(235, 240, 245), 0);
    lv_obj_set_style_text_color(button, COL_MUTED, 0);
    // Selected: same grey with a dark outline and dark text. The unselected
    // border matches the fill so the label does not shift. Slightly darker
    // while pressed, for feedback.
    lv_obj_set_style_border_width(button, 2, 0);
    lv_obj_set_style_border_color(button, lv_color_make(235, 240, 245), 0);
    lv_obj_set_style_border_color(button, COL_HEADER, LV_STATE_CHECKED);
    lv_obj_set_style_text_color(button, COL_TEXT, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(button, lv_color_make(218, 225, 232), LV_STATE_PRESSED);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    // On press, not release, so the tab changes as soon as it is touched.
    lv_obj_add_event_cb(button, detail_tab_event, LV_EVENT_PRESSED, NULL);
    return button;
}

static lv_obj_t *make_detail_panel(lv_obj_t *parent)
{
    lv_obj_t *panel = make_card(parent, 0, 0, 448, 68);
    lv_obj_set_style_pad_all(panel, 0, 0);
    return panel;
}

// Two columns, with a generous gutter and separate caption/value lines.
static lv_obj_t *make_field(lv_obj_t *parent, int32_t x, int32_t y, const char *caption)
{
    lv_obj_t *cap = make_label(parent, caption, COL_MUTED);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(cap, x, y);
    lv_obj_t *v = make_label(parent, "--", COL_TEXT);
    lv_obj_set_pos(v, x, y + 14);
    lv_obj_set_width(v, 204);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    return v;
}

/*
 * Updates that change nothing must not redraw: setting a label's text or a
 * style, even to the same value, invalidates the object, and the SSD1963 is
 * written pixel by pixel. The status timer refreshes every field each
 * second, so these only touch the object when the value differs.
 */
static void set_text(lv_obj_t *l, const char *text)
{
    if (strcmp(lv_label_get_text(l), text) != 0)
        lv_label_set_text(l, text);
}

static void set_text_fmt(lv_obj_t *l, const char *fmt, ...)
{
    char t[96];
    va_list ap;
    va_start(ap, fmt);
    lv_vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    set_text(l, t);
}

static void set_color(lv_obj_t *o, lv_color_t c)
{
    if (!lv_color_eq(lv_obj_get_style_text_color(o, LV_PART_MAIN), c))
        lv_obj_set_style_text_color(o, c, 0);
}

static void set_hidden(lv_obj_t *o, bool hidden)
{
    if (hidden != lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
        if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

// Fixed point with 2 decimals, rounded, and the unit: v in units of 10^-dec.
static void fmt_fixed2(char *buf, size_t n, int64_t v, int dec, const char *unit)
{
    char t[24];
    lv_snprintf(buf, n, "%s %s", fixed_fmt(t, sizeof t, fixed_round(v, dec - 2), 2), unit);
}

static void set_fixed2(lv_obj_t *l, int64_t v, int dec, const char *unit)
{
    char t[32];
    fmt_fixed2(t, sizeof t, v, dec, unit);
    set_text(l, t);
}

static void set_chip(lv_obj_t *chip, const char *text, lv_color_t col)
{
    set_text(chip, text);
    set_color(chip, col);
}

/*
 * main_screen_sense(), from the main loop once a second: re-read the boards,
 * update the cal, uncertainty and valve fault checks, print any changed
 * status line and the DATA line. Run outside LVGL, so a slow screen redraw
 * does not delay the reading or the fault checks.
 *
 * Once a second, one CSV line on COM3 for logging (tools/logger):
 *   DATA,t_ms,p_mbar,signal_uV,exc_mV,ratio_mV_per_V,die_C,set_mV,ok
 * p_mbar and ratio are empty when there is no valid reading (ok = 0).
 * Lines not starting with DATA are status text.
 */
// DATA lines once a second; DATA OFF pauses them and the live status lines.
// Off at boot; DATA ON (the logger sends it) turns them on.
static bool data_on = false;

bool main_screen_data_command(const char *line)
{
    if (strcmp(line, "DATA OFF") == 0)
        data_on = false;
    else if (strcmp(line, "DATA ON") == 0)
        data_on = true;
    else
        return false;
    link_printf("OK data %s\r\n", data_on ? "on" : "off");
    return true;
}

static void print_data_line(const clicks_state_t *s)
{
    char p[16] = "", r[16] = "", sig[16], die[16];

    if (s->reading_ok) {
        fixed_fmt(p, sizeof p, s->p_mmbar, 3);
        fixed_fmt(r, sizeof r, s->r_ppb, 6);
    }
    link_printf("DATA,%lu,%s,%s,%ld,%s,%s,%ld,%d\r\n",
                      (unsigned long)lv_tick_get(), p,
                      fixed_fmt(sig, sizeof sig, s->shunt_nv / 100, 1),   // 0.1 uV
                      (long)s->bus_mv, r, fixed_fmt(die, sizeof die, s->die_mc, 3),
                      (long)s->boost_set_mv, s->reading_ok ? 1 : 0);
}

static void print_changed_lines(void)
{
    const char *line[STATUS_LINES] = { sysinfo_line(), clicks_stepper3_status(), clicks_boost10_status(),
                                       clicks_powermonitor_status(), clicks_druck_status(), valve_status() };

    // A PC that connects later still gets every line once.
    bool resend = link_take_new_client();

    // The Power Monitor and Druck lines carry live readings and change every
    // second, so DATA OFF holds them back too.
    const bool live[STATUS_LINES] = { false, false, false, true, true, false };

    for (int i = 0; i < STATUS_LINES; i++) {
        if (!line[i][0] || (live[i] && !data_on))
            continue;
        if (resend || last_line[i] == NULL || strcmp(line_copy[i], line[i]) != 0) {
            lv_strlcpy(line_copy[i], line[i], sizeof line_copy[i]);
            last_line[i] = line_copy[i];
            link_printf("%s\r\n", line[i]);
        }
    }
}

/*
 * Valve limit switch chip, display only (does not stop the motor). Its own
 * 50 ms timer: the 1 s status timer made a change take up to a second to
 * show. Unchanged state redraws nothing.
 */
static void limit_timer_cb(lv_timer_t *t)
{
    (void)t;
    limit_state_t lim = limit_state();
    if (stepper3_override()) {
        set_chip(chip_lim, "Limit OVR", COL_WARN);
        return;
    }
    set_chip(chip_lim, lim == LIMIT_CLOSED ? "Limit OK" : lim == LIMIT_OPEN ? "Limit OPEN" : "Limit --",
             lim == LIMIT_CLOSED ? COL_OK : lim == LIMIT_OPEN ? COL_FAULT : COL_IDLE);
}

// log2(v) in 1/256 units, v > 0. Integer only, for the history line's log scale.
static int32_t log2_q8(uint32_t v)
{
    int32_t n = 31;
    while (!(v & 0x80000000u)) {
        v <<= 1;
        n--;
    }
    uint64_t m = v;                       // mantissa, 1.0 to 2.0 as Q31
    for (int i = 0; i < 8; i++) {
        m = (m * m) >> 31;
        n <<= 1;
        if (m >= (1ull << 32)) {
            m >>= 1;
            n |= 1;
        }
    }
    return n;
}

/* Scales the points to pixel rows and redraws the line. Only when a point
 * is added, so a steady screen does not redraw. */
static void hist_scale(void)
{
    int32_t lo = INT32_MAX, hi = INT32_MIN, plo = INT32_MAX;
    int last = -1;
    for (int i = 0; i < HIST_POINTS; i++) {
        if (!hist_ok[i])
            continue;
        int32_t v = hist_mmbar[i];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
        int32_t c = v > 1 ? v : 1;               // 0.001 mbar or below counts as 0.001
        if (c < plo) plo = c;
        last = i;
    }

    // Log when the readings span more than a decade. A reading at or below
    // 0.001 mbar (0, or a little negative from drift) counts as 0.001 mbar,
    // for the bottom of the scale too, so a rise from 0 stays in the box.
    bool use_log = last >= 0 && plo != INT32_MAX && (int64_t)hi > 10 * (int64_t)plo;
    int64_t l0 = 0, l1 = 1;
    if (use_log) {
        l0 = log2_q8((uint32_t)plo);
        l1 = log2_q8((uint32_t)hi);
    } else if (last >= 0) {
        if ((int64_t)hi - lo < 1000) {           // at least 1 mbar tall, centred
            int64_t c = ((int64_t)lo + hi) / 2;
            lo = (int32_t)(c - 500);
            hi = (int32_t)(c + 500);
        } else {
            int32_t pad = (int32_t)(((int64_t)hi - lo) * 8 / 100);
            lo -= pad;
            hi += pad;
        }
    }

    const int32_t ch = HIST_H - 2 * HIST_PAD - 1;
    for (int i = 0; i < HIST_POINTS; i++) {
        if (!hist_ok[i]) {
            hist_y[i] = -1;
            continue;
        }
        int64_t f;   // 0 to 1000 up the line
        if (use_log) {
            int32_t v = hist_mmbar[i] > 1 ? hist_mmbar[i] : 1;
            f = (log2_q8((uint32_t)v) - l0) * 1000 / (l1 - l0);
        } else {
            f = ((int64_t)hist_mmbar[i] - lo) * 1000 / ((int64_t)hi - lo);
        }
        int32_t y = HIST_PAD + ch - (int32_t)(f * ch / 1000);
        hist_y[i] = (int8_t)(y < 0 ? 0 : y > HIST_H - 1 ? HIST_H - 1 : y);
    }

    lv_area_t a;
    lv_obj_get_coords(side, &a);
    a.x1 += SIDE_CAP_W;
    a.y2 = a.y1 + HIST_H - 1;
    lv_obj_invalidate_area(side, &a);
}

// From the status timer, once a second: one point every HIST_EVERY_S, the
// first at start-up. A reading not shown on screen is a gap.
static void hist_sample(const clicks_state_t *s)
{
    if (hist_tick > 0 && hist_tick < HIST_EVERY_S) {
        hist_tick++;
        return;
    }
    hist_tick = 1;
    memmove(hist_mmbar, hist_mmbar + 1, (HIST_POINTS - 1) * sizeof hist_mmbar[0]);
    memmove(hist_ok, hist_ok + 1, (HIST_POINTS - 1) * sizeof hist_ok[0]);
    hist_ok[HIST_POINTS - 1] = s->reading_ok && !s->boost_tripped;
    hist_mmbar[HIST_POINTS - 1] = s->p_mmbar;
    hist_scale();
}

// History line, x of point i from the line's left edge.
static int32_t hist_x(int i)
{
    return (HIST_W - 1 - HIST_PAD_R) * i / (HIST_POINTS - 1);
}

// Pressure bar, x of a decade line: k = 0 for 0.1 mbar to 4 for 1000 mbar.
static int32_t pbar_decade(int k)
{
    return PBAR_PAD + k * (PBAR_W - 2 * PBAR_PAD) / 4;
}

/* Draws "1 min", the history line, and the pressure bar. Each draw call is
 * done before the next is made (no OS), so the LVGL heap holds one at a time. */
static void side_draw_event(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(side, &a);
    lv_draw_label_dsc_t text;
    lv_draw_label_dsc_init(&text);
    text.font = &lv_font_montserrat_12;
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);

    // History line.
    text.color = COL_MUTED;
    text.text = "1 min";
    text.align = LV_TEXT_ALIGN_RIGHT;
    lv_area_t cap = { a.x1, a.y1, a.x1 + SIDE_CAP_W - 5, a.y1 + 15 };
    lv_draw_label(layer, &text, &cap);
    int32_t hx = a.x1 + SIDE_CAP_W, hy = a.y1;
    lv_area_t box = { hx, hy, hx + HIST_W - 1, hy + HIST_H - 1 };
    r.bg_color = COL_SPARK_BG;
    lv_draw_rect(layer, &r, &box);
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = COL_PEN;
    line.width = 2;
    line.round_start = 1;
    line.round_end = 1;
    int last = -1;
    for (int i = 0; i < HIST_POINTS; i++) {
        if (hist_y[i] < 0)
            continue;
        if (last >= 0 && last == i - 1) {
            line.p1.x = hx + hist_x(last);
            line.p1.y = hy + hist_y[last];
            line.p2.x = hx + hist_x(i);
            line.p2.y = hy + hist_y[i];
            lv_draw_line(layer, &line);
        }
        last = i;
    }
    if (last >= 0) {                              // the dot on the newest point
        int32_t x = hx + hist_x(last), y = hy + hist_y[last];
        lv_area_t dot = { x - 2, y - 2, x + 2, y + 2 };
        r.bg_color = COL_PEN;
        r.radius = LV_RADIUS_CIRCLE;
        lv_draw_rect(layer, &r, &dot);
        r.radius = 0;
    }

    // Pressure bar.
    int32_t bx = a.x1 + SIDE_CAP_W, y1 = a.y1 + PBAR_Y, y2 = y1 + 7;
    lv_area_t bar = { bx, y1, bx + PBAR_W - 1, y2 };
    r.bg_color = COL_PAGE;
    lv_draw_rect(layer, &r, &bar);
    if (pbar_x > 0) {
        bar.x2 = bx + pbar_x - 1;
        r.bg_color = COL_PFILL;
        lv_draw_rect(layer, &r, &bar);
    }
    r.bg_color = COL_PTICK;                       // a line at each decade
    for (int k = 0; k <= 4; k++) {
        int32_t x = bx + pbar_decade(k);
        lv_area_t tick = { x, y1, x, y2 };
        lv_draw_rect(layer, &r, &tick);
    }
    static const char *const labels[3] = { "0.1", "10", "1000" };
    text.align = LV_TEXT_ALIGN_CENTER;
    for (int k = 0; k < 3; k++) {
        int32_t x = bx + pbar_decade(2 * k);
        lv_area_t l = { x - 20, y2 + 2, x + 20, y2 + 16 };
        text.text = labels[k];
        lv_draw_label(layer, &text, &l);
    }
    if (pbar_x >= 0) {
        int32_t x = pbar_x - 1 < 0 ? 0 : pbar_x - 1 > PBAR_W - 3 ? PBAR_W - 3 : pbar_x - 1;
        lv_area_t mark = { bx + x, y1 - 3, bx + x + 2, y1 + 10 };
        r.bg_color = COL_TEXT;
        lv_draw_rect(layer, &r, &mark);
    }
}

/* Pressure bar, from the status timer: log 0.1 to 1000 mbar between the
 * margins. Redrawn only when the fill moves a pixel. */
static void pbar_update(const clicks_state_t *s)
{
    int32_t x = -1;
    if (s->reading_ok && !s->boost_tripped) {
        const int32_t l_lo = log2_q8(100), l_hi = log2_q8(1000000);   // 0.1, 1000 mbar
        int32_t l = log2_q8(s->p_mmbar > 1 ? (uint32_t)s->p_mmbar : 1);
        x = pbar_decade(0) + (l - l_lo) * (pbar_decade(4) - pbar_decade(0)) / (l_hi - l_lo);
        x = x < 0 ? 0 : x > PBAR_W ? PBAR_W : x;
    }
    if (x != pbar_x) {
        pbar_x = x;
        lv_area_t a;
        lv_obj_get_coords(side, &a);
        a.x1 += SIDE_CAP_W;
        a.y1 += PBAR_Y - 3;
        a.y2 = a.y1 + 13;
        lv_obj_invalidate_area(side, &a);
    }
}

/**
 * @brief Every second: refresh the screen from the last reading. The boards
 * are read by main_screen_sense() in the main loop, not here.
 */
void main_screen_sense(void)
{
    clicks_poll();
    print_changed_lines();

    const clicks_state_t *s = clicks_state();
    uncert_update(s);
    cal_update(s);
    valve_sense(s);
    if (s->pm_found && data_on && !s->pm_diag)
        print_data_line(s);
}

static void status_timer_cb(lv_timer_t *t)
{
    (void)t;
    const clicks_state_t *s = clicks_state();

    // Header status.
    set_chip(chip_pm, s->pm_found ? "ADC OK" : "ADC OFF", s->pm_found ? COL_OK : COL_FAULT);
    if (s->boost_tripped)
        set_chip(chip_boost, "10 V TRIP", COL_FAULT);
    else
        set_chip(chip_boost, s->boost_pg ? "10 V OK" : "10 V OFF", s->boost_pg ? COL_OK : COL_FAULT);
    set_chip(chip_usb, link_chip_text(), link_chip_ok() ? COL_OK : COL_IDLE);
    set_text(clock_label, rtclock_screen_text());
    set_color(clock_label, rtclock_valid() ? lv_color_white() : COL_IDLE);

    // Pressure.
    set_text(pressure_label, s->reading_ok && !s->boost_tripped ? clicks_druck_value() : "----");
    lv_color_t edge = !s->reading_ok || s->boost_tripped ? COL_FAULT : s->cal_nominal ? COL_WARN : COL_OK;
    if (!lv_color_eq(lv_obj_get_style_border_color(pressure_card, LV_PART_MAIN), edge))
        lv_obj_set_style_border_color(pressure_card, edge, 0);
    if (!s->pm_found)
        set_text(pressure_sub, "Druck ADC (Power Monitor) not responding");
    else if (s->boost_tripped)
        set_text_fmt(pressure_sub, "10 V supply tripped at %ld mV, reset to clear", (long)s->boost_trip_mv);
    else if (s->druck_absent)
        set_text(pressure_sub, "No Druck signal, check the sensor is connected");
    else if (!s->reading_ok)
        set_text(pressure_sub, "Excitation below 7 V, check VBUS wiring");
    else if (s->cal_nominal)
        set_text(pressure_sub, "Druck 15 psia, NOT calibrated (nominal)");
    else
        set_text(pressure_sub, "Druck 15 psia, calibrated");
    set_color(pressure_sub,
                                (!s->pm_found || s->boost_tripped || !s->reading_ok) ? COL_FAULT :
                                s->cal_nominal ? COL_WARN : COL_MUTED);
    hist_sample(s);
    pbar_update(s);

    // Readouts, 2 decimals (the DATA lines keep full resolution).
    if (s->pm_found) {
        // Signal with the ratio it gives (mV/V, the sensor's own quantity,
        // to check against the cert); the ratio needs a valid excitation.
        char sig[24], rat[24];
        fmt_fixed2(sig, sizeof sig, s->shunt_nv, 6, "mV");
        if (s->reading_ok) {
            fmt_fixed2(rat, sizeof rat, s->r_ppb, 6, "mV/V");
            set_text_fmt(val_signal, "%s  (%s)", sig, rat);
        } else {
            set_text(val_signal, sig);
        }
        set_fixed2(val_exc, s->bus_mv, 3, "V");
        set_fixed2(val_die, s->die_mc, 3, "C");
    } else {
        set_text(val_signal, "--");
        set_text(val_exc, "--");
        set_text(val_die, "--");
    }
    bool unc_warn;
    set_text(pressure_unc, uncert_screen_text(&unc_warn));
    set_color(pressure_unc, unc_warn ? COL_WARN : COL_MUTED);

    if (s->boost_tripped) {
        set_text(val_pg, "tripped");
        set_color(val_pg, COL_FAULT);
    } else {
        set_text(val_pg, s->boost_pg ? "regulating" : "not regulating");
        set_color(val_pg, s->boost_pg ? COL_TEXT : COL_FAULT);
    }

    // Calibration: the bench two-point (CAL VAC, CAL AIR), each point as
    // its pressure and the ratio measured there.
    bool nominal = cal_is_default();
    set_text(cal_table, PMC_DRUCK_SERIAL);
    int32_t rv, pv, ra, pa;
    bool bench = cal_bench(&rv, &pv, &ra, &pa);
    int left = cal_capture_left();
    if (left) {
        set_text_fmt(cal_kind, "capturing, %d s left", left);
        set_color(cal_kind, COL_WARN);
    } else if (cal_old_version()) {
        set_text(cal_kind, "old cal not loaded, redo");
        set_color(cal_kind, COL_WARN);
    } else {
        set_text(cal_kind, nominal ? "nominal, not calibrated" : bench ? "bench two-point" : "entered table");
        set_color(cal_kind, bench ? COL_TEXT : COL_WARN);
    }
    if (bench) {
        char p[24], r[24];
        fmt_fixed2(p, sizeof p, pv, 3, "mbar");
        fixed_fmt(r, sizeof r, fixed_round(rv, 2), 4);
        set_text_fmt(cal_vac, "%s, %s mV/V", p, r);
        fmt_fixed2(p, sizeof p, pa, 3, "mbar");
        fixed_fmt(r, sizeof r, fixed_round(ra, 2), 4);
        set_text_fmt(cal_air, "%s, %s mV/V", p, r);
    } else {
        set_text(cal_vac, "--");
        set_text(cal_air, "--");
    }

    // Only a warning: nothing is shown once the table is saved. The button
    // text moves left to make room for it.
    bool unsaved = !(nominal || cal_saved());
    set_hidden(cal_store, !unsaved);
    lv_obj_t *cal_text = lv_obj_get_child(tab_cal, 0);
    if (lv_obj_get_style_translate_x(cal_text, LV_PART_MAIN) != (unsaved ? -34 : 0))
        lv_obj_set_style_translate_x(cal_text, unsaved ? -34 : 0, 0);
}

void init_main_screen()
{
    init_main_screen_ui(&lvgl_main_screen_ui);
    lv_obj_t *scr = lvgl_main_screen_ui.main_screen;

    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_make(232, 238, 244), 0);

    // The designer's test switch is no longer used.
    lv_obj_add_flag(lvgl_main_screen_ui.switch_0, LV_OBJ_FLAG_HIDDEN);

    // Header.
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_size(hdr, 480, 30);
    lv_obj_set_style_bg_color(hdr, COL_HEADER, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_hor(hdr, 10, 0);
    lv_obj_set_style_pad_ver(hdr, 0, 0);

    lv_obj_t *title = make_label(hdr, "PMC", lv_color_white());
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);
    clock_label = make_label(hdr, "time not set", COL_IDLE);
    lv_obj_set_style_text_font(clock_label, &lv_font_montserrat_12, 0);
    lv_obj_set_width(clock_label, 90);
    lv_label_set_long_mode(clock_label, LV_LABEL_LONG_CLIP);
    lv_obj_align(clock_label, LV_ALIGN_LEFT_MID, 46, 0);
    // Status words, right-aligned row; green = ok, red = fault, grey = idle.
    lv_obj_t *chips = lv_obj_create(hdr);
    lv_obj_remove_flag(chips, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(chips, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(chips, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chips, 0, 0);
    lv_obj_set_style_pad_all(chips, 0, 0);
    lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(chips, 8, 0);
    lv_obj_set_style_text_font(chips, &lv_font_montserrat_12, 0);
    lv_obj_align(chips, LV_ALIGN_RIGHT_MID, 0, 0);
    chip_pm = make_label(chips, "PM", COL_IDLE);
    chip_boost = make_label(chips, "BOOST", COL_IDLE);
    chip_lim = make_label(chips, "Limit --", COL_IDLE);
    chip_usb = make_label(chips, link_chip_text(), COL_IDLE);
    lv_obj_set_width(chip_pm, 52);
    lv_obj_set_width(chip_boost, 70);
    lv_obj_set_width(chip_lim, 80);
    lv_obj_set_width(chip_usb, 90);
    lv_label_set_long_mode(chip_lim, LV_LABEL_LONG_CLIP);
    lv_label_set_long_mode(chip_usb, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(chip_usb, LV_TEXT_ALIGN_RIGHT, 0);

    // Pressure card.
    lv_obj_t *pc = make_card(scr, 10, 38, 460, 92);
    pressure_card = pc;
    lv_obj_set_style_border_width(pc, 2, 0);
    lv_obj_set_style_border_side(pc, LV_BORDER_SIDE_LEFT, 0);
    pressure_font = lv_font_montserrat_48;
    pressure_font.get_glyph_dsc = pressure_glyph_dsc;
    pressure_font.kerning = LV_FONT_KERNING_NONE;
    pressure_label = make_label(pc, "----", COL_TEXT);
    lv_obj_set_style_text_font(pressure_label, &pressure_font, 0);
    lv_obj_set_style_text_align(pressure_label, LV_TEXT_ALIGN_RIGHT, 0);
    // Tabular digits and right alignment fix the decimal position; value and
    // unit sit at the left, the history line and pressure bar at the right.
    // The label box starts left of the card; the widest reading (1034.00)
    // still starts inside it.
    lv_obj_set_width(pressure_label, 250);
    lv_obj_align(pressure_label, LV_ALIGN_TOP_LEFT, -40, -4);
    lv_obj_t *unit = make_label(pc, "mB", COL_MUTED);   // mbar abs, as the simulator
    // Unit sits on the digits' baseline (the label box includes descender room).
    lv_obj_align_to(unit, pressure_label, LV_ALIGN_OUT_RIGHT_BOTTOM, 12, -7);
    // Uncertainty of the reading (uncert.c), under the unit.
    pressure_unc = make_label(pc, "", COL_MUTED);
    lv_obj_align_to(pressure_unc, unit, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 4);
#if !PMC_SHOW_UNCERTAINTY
    lv_obj_add_flag(pressure_unc, LV_OBJ_FLAG_HIDDEN);   // UNC? still works
#endif
    pressure_sub = make_label(pc, "", COL_MUTED);
    lv_obj_set_width(pressure_sub, SIDE_X - 8);   // clear of the pressure bar
    lv_label_set_long_mode(pressure_sub, LV_LABEL_LONG_DOT);
    lv_obj_align(pressure_sub, LV_ALIGN_BOTTOM_LEFT, 4, 0);

    // Pressure history line (last 1 min) and pressure bar, drawn by side_draw_event.
    memset(hist_y, -1, sizeof hist_y);
    side = lv_obj_create(pc);
    lv_obj_remove_style_all(side);
    lv_obj_remove_flag(side, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(side, SIDE_X - SIDE_CAP_W, 0);
    lv_obj_set_size(side, SIDE_CAP_W + HIST_W, SIDE_H);
    lv_obj_add_event_cb(side, side_draw_event, LV_EVENT_DRAW_MAIN, NULL);

    // Dedicated footer: firmware/reset information cannot obscure a fault.
    lv_obj_t *ver = make_label(scr, "v" PMC_FW_VERSION, COL_MUTED);
    lv_obj_set_style_text_font(ver, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(ver, 12, 256);
    if (sysinfo_reset_abnormal()) {
        lv_obj_t *reset = make_label(scr, "", COL_FAULT);
        lv_obj_set_style_text_font(reset, &lv_font_montserrat_12, 0);
        lv_label_set_text_fmt(reset, "Last reset: %s", sysinfo_reset_reason());
        lv_obj_set_width(reset, 330);
        lv_label_set_long_mode(reset, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(reset, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(reset, 138, 256);
    }

    // One spacious detail area; both sets of values still update every second.
    lv_obj_t *details = make_card(scr, 10, 138, 460, 114);
    tab_raw = make_tab(details, 0, 140, "Chip readings");
    tab_cal = make_tab(details, 146, 200, "Calibration");
    // Same look as the tabs, but it opens the motor screen.
    lv_obj_t *motor = make_tab(details, 352, 96, "Motor " LV_SYMBOL_RIGHT);
    lv_obj_remove_event_cb(motor, detail_tab_event);
    lv_obj_add_event_cb(motor, motor_button_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_state(tab_raw, LV_STATE_CHECKED);
    // Unsaved warning on the Calibration button, so it shows on either tab.
    cal_store = make_label(tab_cal, "not saved", COL_WARN);
    lv_obj_set_style_text_font(cal_store, &lv_font_montserrat_12, 0);
    lv_obj_align(cal_store, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_add_flag(cal_store, LV_OBJ_FLAG_HIDDEN);

    detail_cal = make_detail_panel(details);
    lv_obj_add_flag(detail_cal, LV_OBJ_FLAG_HIDDEN);
    cal_table = make_field(detail_cal, 6, 0, "Druck serial number");
    cal_kind = make_field(detail_cal, 236, 0, "Calibration");
    cal_vac = make_field(detail_cal, 6, 36, "Vacuum point (CAL VAC)");
    cal_air = make_field(detail_cal, 236, 36, "Air point (CAL AIR)");

    detail_raw = make_detail_panel(details);
    val_signal = make_field(detail_raw, 6, 0, "INA228 signal (ratio)");
    val_exc = make_field(detail_raw, 236, 0, "INA228 excitation (measured)");
    val_die = make_field(detail_raw, 6, 36, "INA228 temperature");
    val_pg = make_field(detail_raw, 236, 36, "Boost 10 supply");

    lv_timer_create(status_timer_cb, 1000, NULL);
    lv_timer_create(limit_timer_cb, 50, NULL);
    motor_screen_init(scr);
}

void show_main_screen()
{
    lv_screen_load(lvgl_main_screen_ui.main_screen);
}
