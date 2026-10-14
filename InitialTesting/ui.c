#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "pmc_config.h"
#include "stepper3.h"
#include "valve.h"
#include "main_screen.h"
#include "ui.h"

/* --------------------------------------------------------------------------
 * Fonts: tabular digits. Every digit takes the width of the widest, so a
 * right-aligned reading does not shift as it changes.
 * ------------------------------------------------------------------------ */
lv_font_t ui_num_16, ui_num_26;

static bool num_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *dsc, uint32_t letter, uint32_t next)
{
    const lv_font_t *base = font->user_data;
    if (!base->get_glyph_dsc(base, dsc, letter, next))
        return false;
    if (letter >= '0' && letter <= '9') {
        int32_t widest = 0;
        lv_font_glyph_dsc_t d;
        for (uint32_t c = '0'; c <= '9'; c++)
            if (base->get_glyph_dsc(base, &d, c, 0) && d.adv_w > widest)
                widest = d.adv_w;
        dsc->ofs_x += (widest - dsc->adv_w) / 2;
        dsc->adv_w = widest;
    }
    return true;
}

static void num_font(lv_font_t *f, const lv_font_t *base)
{
    *f = *base;
    f->get_glyph_dsc = num_glyph_dsc;
    f->kerning = LV_FONT_KERNING_NONE;
    f->user_data = (void *)base;
}

/* --------------------------------------------------------------------------
 * Styles (STYLE-GUIDE.md section 6)
 * ------------------------------------------------------------------------ */
#define UI_EDGE    lv_color_hex(0xAEB9C4)
#define UI_PRESS   lv_color_hex(0xE3E9EF)
#define UI_DIS_BG  lv_color_hex(0xE9EEF2)
#define UI_DIS_TX  lv_color_hex(0x9AA4AE)
#define UI_CHEV    lv_color_hex(0x7B8794)

static lv_style_t st_btn, st_btn_pr, st_btn_dis, st_act, st_act_pr, st_act_small,
                  st_red, st_red_pr, st_dark, st_dark_pr, st_card;
#define ST_DIS LV_STATE_USER_1   // greyed, still clickable so it can say why

static void styles_init(void)
{
    lv_style_init(&st_btn);
    lv_style_set_bg_opa(&st_btn, LV_OPA_COVER);
    lv_style_set_bg_color(&st_btn, UI_CARD);
    lv_style_set_radius(&st_btn, 6);
    lv_style_set_border_side(&st_btn, LV_BORDER_SIDE_BOTTOM);   // the raised edge
    lv_style_set_border_width(&st_btn, 2);
    lv_style_set_border_color(&st_btn, UI_EDGE);
    lv_style_set_text_color(&st_btn, UI_TEXT);
    lv_style_set_text_font(&st_btn, &ui_font_16);

    lv_style_init(&st_btn_pr);
    lv_style_set_bg_color(&st_btn_pr, UI_PRESS);
    lv_style_set_border_width(&st_btn_pr, 0);

    lv_style_init(&st_btn_dis);
    lv_style_set_bg_color(&st_btn_dis, UI_DIS_BG);
    lv_style_set_text_color(&st_btn_dis, UI_DIS_TX);
    lv_style_set_border_width(&st_btn_dis, 0);

    lv_style_init(&st_act);
    lv_style_set_bg_color(&st_act, UI_BLUE);
    lv_style_set_text_color(&st_act, lv_color_white());
    lv_style_set_border_width(&st_act, 0);
    lv_style_init(&st_act_pr);
    lv_style_set_bg_color(&st_act_pr, lv_color_hex(0x24475F));
    lv_style_init(&st_act_small);
    lv_style_set_text_color(&st_act_small, lv_color_hex(0xD5E2EC));

    lv_style_init(&st_red);
    lv_style_set_bg_color(&st_red, UI_FAULT);
    lv_style_set_text_color(&st_red, lv_color_white());
    lv_style_set_border_width(&st_red, 0);
    lv_style_init(&st_red_pr);
    lv_style_set_bg_color(&st_red_pr, lv_color_hex(0xAA1E1E));

    lv_style_init(&st_dark);
    lv_style_set_bg_color(&st_dark, UI_TEXT);
    lv_style_set_text_color(&st_dark, lv_color_white());
    lv_style_set_radius(&st_dark, 0);
    lv_style_set_border_width(&st_dark, 0);
    lv_style_init(&st_dark_pr);
    lv_style_set_bg_color(&st_dark_pr, lv_color_hex(0x3C4856));

    lv_style_init(&st_card);
    lv_style_set_bg_opa(&st_card, LV_OPA_COVER);
    lv_style_set_bg_color(&st_card, UI_CARD);
    lv_style_set_radius(&st_card, 6);
    lv_style_set_border_color(&st_card, UI_FAULT);   // alarm edge, width 0 until set
    lv_style_set_border_width(&st_card, 0);
}

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t *ui_card(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *o = plain(parent);
    lv_obj_add_style(o, &st_card, 0);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    return o;
}

lv_obj_t *ui_label(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, const lv_font_t *font,
                   lv_color_t color, lv_text_align_t align, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    if (w > 0) {
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    }
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_align(l, align, 0);
    lv_label_set_text(l, text);
    return l;
}

void ui_set_text(lv_obj_t *l, const char *text)
{
    if (strcmp(lv_label_get_text(l), text) != 0)
        lv_label_set_text(l, text);
}

void ui_set_color(lv_obj_t *o, lv_color_t c)
{
    if (!lv_color_eq(lv_obj_get_style_text_color(o, LV_PART_MAIN), c))
        lv_obj_set_style_text_color(o, c, 0);
}

const char *ui_fmt1(char *buf, size_t n, int64_t milli)
{
    bool neg = milli < 0;
    int64_t t = ((neg ? -milli : milli) + 50) / 100;
    lv_snprintf(buf, n, "%s%ld.%ld", neg && t ? "-" : "", (long)(t / 10), (long)(t % 10));
    return buf;
}

/* --------------------------------------------------------------------------
 * Buttons. Child 0 is the main label, child 1 the 12 px state line, child 2
 * the chevron (nav only).
 * ------------------------------------------------------------------------ */
static void btn_layout(lv_obj_t *b)
{
    lv_obj_t *main = lv_obj_get_child(b, 0), *small = lv_obj_get_child(b, 1);
    bool two = !lv_obj_has_flag(small, LV_OBJ_FLAG_HIDDEN);
    bool nav = lv_obj_get_child_count(b) > 2;
    lv_align_t al = nav ? LV_ALIGN_LEFT_MID : LV_ALIGN_CENTER;
    int32_t x = nav ? 12 : 0;
    lv_obj_align(main, al, x, two ? -8 : 0);
    lv_obj_align(small, al, x, 10);
}

lv_obj_t *ui_btn(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                 ui_btn_kind_t kind, const char *text, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_add_style(b, &st_btn, 0);
    lv_obj_add_style(b, &st_btn_pr, LV_STATE_PRESSED);
    if (kind == UI_PRIMARY) {
        lv_obj_add_style(b, &st_act, 0);
        lv_obj_add_style(b, &st_act_pr, LV_STATE_PRESSED);
    } else if (kind == UI_RED) {
        lv_obj_add_style(b, &st_red, 0);
        lv_obj_add_style(b, &st_red_pr, LV_STATE_PRESSED);
    } else if (kind == UI_DARK) {
        lv_obj_add_style(b, &st_dark, 0);
        lv_obj_add_style(b, &st_dark_pr, LV_STATE_PRESSED);
    }
    lv_obj_add_style(b, &st_btn_dis, ST_DIS);

    lv_obj_t *main = lv_label_create(b);
    lv_obj_set_width(main, w - (kind == UI_NAV ? 36 : 8));
    lv_obj_set_style_text_align(main, kind == UI_NAV || kind == UI_DARK ? LV_TEXT_ALIGN_LEFT
                                                                         : LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(main, text);
    lv_obj_t *small = lv_label_create(b);
    lv_obj_set_width(small, w - (kind == UI_NAV ? 36 : 8));
    lv_obj_set_style_text_align(small, lv_obj_get_style_text_align(main, 0), 0);
    lv_obj_set_style_text_font(small, &ui_font_12, 0);
    lv_obj_set_style_text_color(small, UI_MUTED, 0);
    if (kind == UI_PRIMARY)
        lv_obj_add_style(small, &st_act_small, 0);
    lv_label_set_text(small, "");
    lv_obj_add_flag(small, LV_OBJ_FLAG_HIDDEN);
    if (kind == UI_NAV) {
        lv_obj_t *chev = lv_label_create(b);
        lv_obj_set_style_text_font(chev, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(chev, UI_CHEV, 0);
        lv_label_set_text(chev, LV_SYMBOL_RIGHT);
        lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -8, 0);
    }
    btn_layout(b);
    if (cb)
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    return b;
}

void ui_btn_text(lv_obj_t *b, const char *text, const char *small)
{
    lv_obj_t *m = lv_obj_get_child(b, 0), *s = lv_obj_get_child(b, 1);
    bool had = !lv_obj_has_flag(s, LV_OBJ_FLAG_HIDDEN), has = small && *small;
    ui_set_text(m, text);
    ui_set_text(s, has ? small : "");
    if (had != has) {
        if (has) lv_obj_remove_flag(s, LV_OBJ_FLAG_HIDDEN);
        else     lv_obj_add_flag(s, LV_OBJ_FLAG_HIDDEN);
        btn_layout(b);
    }
}

void ui_btn_enable(lv_obj_t *b, bool on)
{
    if (on == !lv_obj_has_state(b, ST_DIS))
        return;
    if (on) lv_obj_remove_state(b, ST_DIS);
    else    lv_obj_add_state(b, ST_DIS);
}

bool ui_btn_disabled(lv_event_t *e)
{
    return lv_obj_has_state(lv_event_get_current_target(e), ST_DIS);
}

/* --------------------------------------------------------------------------
 * Frame on lv_layer_top(): header, status strip, STOP
 * ------------------------------------------------------------------------ */
static lv_obj_t *frame, *hdr, *back_btn, *pmc_lbl, *title_lbl, *mode_lbl, *mode_dot, *fault_rule;
static lv_obj_t *strip, *strip_dot, *strip_icon, *strip_text, *strip_ok, *strip_act;
static lv_obj_t *stop_btn;

static lv_obj_t *home_scr, *cur_scr, *overlay;
static const ui_screen_t *cur = &ui_scr_home;
static bool service;

static char msg_text[96];
static uint32_t msg_until;

static ui_strip_kind_t line_kind;
static lv_color_t line_dot;
static char line_text[96];
static lv_event_cb_t act_cb;
static int32_t strip_w = 343;

static void back_event(lv_event_t *e)
{
    (void)e;
    if (overlay)
        ui_overlay_close();
    else if (cur != &ui_scr_home)
        ui_home();
}

static void stop_event(lv_event_t *e)
{
    (void)e;
    ui_overlay_close();
    valve_stop();
}

static void ok_event(lv_event_t *e)
{
    (void)e;
    msg_until = 0;
}

static void act_event(lv_event_t *e)
{
    if (act_cb)
        act_cb(e);
}

static void build_frame(void)
{
    frame = plain(lv_layer_top());
    lv_obj_set_size(frame, 480, 272);
    lv_obj_add_flag(frame, LV_OBJ_FLAG_EVENT_BUBBLE);

    hdr = plain(frame);
    lv_obj_set_size(hdr, 480, 40);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(hdr, UI_TEXT, 0);
    lv_obj_add_flag(hdr, LV_OBJ_FLAG_CLICKABLE);    // touches on the header stop here
    fault_rule = plain(hdr);
    lv_obj_set_pos(fault_rule, 0, 36);
    lv_obj_set_size(fault_rule, 480, 4);
    lv_obj_set_style_bg_opa(fault_rule, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(fault_rule, UI_FAULT, 0);
    lv_obj_add_flag(fault_rule, LV_OBJ_FLAG_HIDDEN);
    back_btn = ui_btn(hdr, 0, 0, 119, 40, UI_DARK, LV_SYMBOL_LEFT " Back", back_event, NULL);
    lv_obj_align(lv_obj_get_child(back_btn, 0), LV_ALIGN_LEFT_MID, 4, 0);
    pmc_lbl = ui_label(hdr, 10, 11, 109, &ui_font_16, lv_color_white(), LV_TEXT_ALIGN_LEFT, "PMC");
    title_lbl = ui_label(hdr, 127, 11, 166, &ui_font_16, lv_color_white(), LV_TEXT_ALIGN_LEFT, "");
    lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_DOT);
    mode_lbl = ui_label(hdr, 296, 11, 158, &ui_font_16, lv_color_white(), LV_TEXT_ALIGN_RIGHT, "");
    mode_dot = plain(hdr);
    lv_obj_set_pos(mode_dot, 460, 15);
    lv_obj_set_size(mode_dot, 10, 10);
    lv_obj_set_style_radius(mode_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(mode_dot, lv_color_hex(0xC3CDD7), 0);
    lv_obj_set_style_bg_color(mode_dot, UI_FAULT, 0);

    strip = plain(frame);
    lv_obj_set_pos(strip, 10, 216);
    lv_obj_set_size(strip, 343, 48);
    lv_obj_add_style(strip, &st_card, 0);
    lv_obj_add_flag(strip, LV_OBJ_FLAG_CLICKABLE);
    strip_dot = plain(strip);
    lv_obj_set_pos(strip_dot, 12, 20);
    lv_obj_set_size(strip_dot, 8, 8);
    lv_obj_set_style_radius(strip_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(strip_dot, LV_OPA_COVER, 0);
    strip_icon = ui_label(strip, 6, 15, 0, &lv_font_montserrat_14, lv_color_white(), LV_TEXT_ALIGN_LEFT,
                          LV_SYMBOL_WARNING);
    strip_text = ui_label(strip, 28, 0, 303, &ui_font_12, UI_TEXT, LV_TEXT_ALIGN_LEFT, "");
    lv_label_set_long_mode(strip_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(strip_text, 4, 0);
    strip_ok = ui_btn(strip, 279, 4, 60, 40, UI_SECONDARY, "OK", ok_event, NULL);
    strip_act = ui_btn(strip, 239, 4, 100, 40, UI_PRIMARY, "", act_event, NULL);

    stop_btn = ui_btn(frame, 361, 216, 109, 48, UI_RED, "STOP", NULL, NULL);
    lv_obj_set_style_text_font(lv_obj_get_child(stop_btn, 0), &ui_font_26, 0);
    lv_obj_add_event_cb(stop_btn, stop_event, LV_EVENT_PRESSED, NULL);   // on press, not release
}

static void show(lv_obj_t *o, bool on)
{
    if (on == !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN))
        return;
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

void ui_msg(const char *text)
{
    lv_strlcpy(msg_text, text, sizeof msg_text);
    msg_until = lv_tick_get() + 5000;
    if (!msg_until)
        msg_until = 1;
}

void ui_strip_set(ui_strip_kind_t kind, lv_color_t dot, const char *text)
{
    line_kind = kind;
    line_dot = dot;
    lv_strlcpy(line_text, text, sizeof line_text);
}

void ui_strip_width(int32_t w) { strip_w = w; }

void ui_strip_action(const char *text, const char *small, lv_event_cb_t cb)
{
    act_cb = text ? cb : NULL;
    if (text)
        ui_btn_text(strip_act, text, small);
}

static void strip_draw(ui_strip_kind_t kind, lv_color_t dot, const char *text, bool ok, bool act)
{
    static const uint32_t bg[] = { 0xFFFFFF, 0xE0E0E0, 0x3F3F3F, 0x000000 };
    lv_color_t b = lv_color_hex(bg[kind]);
    if (!lv_color_eq(lv_obj_get_style_bg_color(strip, 0), b))
        lv_obj_set_style_bg_color(strip, b, 0);
    int32_t bw = kind == UI_STRIP_FAULT ? 3 : kind == UI_STRIP_PROMPT ? 1 : 0;
    lv_color_t bc = kind == UI_STRIP_FAULT ? UI_FAULT : UI_WARN;
    if (lv_obj_get_style_border_width(strip, 0) != bw || !lv_color_eq(lv_obj_get_style_border_color(strip, 0), bc)) {
        lv_obj_set_style_border_width(strip, bw, 0);
        lv_obj_set_style_pad_all(strip, -bw, 0);    // children keep their place
        lv_obj_set_style_border_color(strip, bc, 0);
    }
    show(strip_dot, kind == UI_STRIP_INFO);
    if (kind == UI_STRIP_INFO && !lv_color_eq(lv_obj_get_style_bg_color(strip_dot, 0), dot))
        lv_obj_set_style_bg_color(strip_dot, dot, 0);
    show(strip_icon, kind >= UI_STRIP_WARN);
    ui_set_color(strip_icon, kind == UI_STRIP_FAULT ? UI_FAULT : lv_color_white());
    ui_set_color(strip_text, kind >= UI_STRIP_WARN ? lv_color_white()
                             : kind == UI_STRIP_PROMPT ? lv_color_black() : UI_TEXT);
    int32_t x = kind == UI_STRIP_PROMPT ? 12 : 28;
    int32_t w = (act ? strip_w - 104 : ok ? strip_w - 64 : strip_w - 4) - x - 4;
    if (lv_obj_get_width(strip) != strip_w) {
        lv_obj_set_width(strip, strip_w);
        lv_obj_set_x(strip_ok, strip_w - 64);
        lv_obj_set_x(strip_act, strip_w - 104);
    }
    if (lv_obj_get_x(strip_text) != x || lv_obj_get_width(strip_text) != w) {
        lv_obj_set_x(strip_text, x);
        lv_obj_set_width(strip_text, w);
    }
    ui_set_text(strip_text, text);
    lv_obj_align(strip_text, LV_ALIGN_LEFT_MID, x, 0);
    show(strip_ok, ok);
    show(strip_act, act);
}

static void frame_refresh(void)
{
    const char *tag, *text;
    int nf = valve_fault_count();
    bool f12 = valve_fault_is("F-12");
    valve_mode_t m = valve_mode();

    // Header.
    bool home = cur == &ui_scr_home;
    show(back_btn, !home || overlay);
    show(pmc_lbl, home && !overlay);
    show(title_lbl, !home);
    if (!home)
        ui_set_text(title_lbl, cur->title);
    ui_set_text(mode_lbl, f12 ? "STOPPED" : nf ? "FAULT" : valve_mode_name());
    show(fault_rule, nf > 0);
    // Dot: red with a fault, filled grey in AUTO (not built), else a ring.
    lv_opa_t opa = nf ? LV_OPA_COVER : LV_OPA_TRANSP;
    if (lv_obj_get_style_bg_opa(mode_dot, 0) != opa) {
        lv_obj_set_style_bg_opa(mode_dot, opa, 0);
        lv_obj_set_style_border_width(mode_dot, nf ? 0 : 2, 0);
    }

    // The screen's own line and Home's action, then the frame's priorities.
    act_cb = NULL;
    if (cur->refresh)
        cur->refresh();
    show(strip, !overlay);
    if (msg_until && (int32_t)(lv_tick_get() - msg_until) >= 0)
        msg_until = 0;
    char buf[96];
    if (msg_until) {
        strip_draw(UI_STRIP_WARN, UI_IDLE, msg_text, true, false);
    } else if (valve_fault_first(&tag, &text)) {
        if (f12)
            lv_strlcpy(buf, "Stopped. Valve held, not closed.", sizeof buf);
        else if (nf > 1)
            lv_snprintf(buf, sizeof buf, "%s %s (+%d)", tag, text, nf - 1);
        else
            lv_snprintf(buf, sizeof buf, "%s %s", tag, text);
        strip_draw(UI_STRIP_FAULT, UI_IDLE, buf, false, act_cb != NULL);
    } else if (stepper3_override()) {
        strip_draw(UI_STRIP_WARN, UI_IDLE, "Limit override on", false, act_cb != NULL);
    } else {
        strip_draw(line_kind, line_dot, line_text, false, act_cb != NULL);
    }
}

static void refresh_timer(lv_timer_t *t)
{
    (void)t;
    if (service) {
        // Back on Home once the 0.6.0 screens are left (their Back goes to
        // the old main screen; its "Home" button calls ui_home()).
        return;
    }
    frame_refresh();
}

/* --------------------------------------------------------------------------
 * Screens
 * ------------------------------------------------------------------------ */
static lv_obj_t *new_screen(void)
{
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_remove_style_all(s);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s, UI_BG, 0);
    lv_obj_set_style_text_font(s, &ui_font_16, 0);
    lv_obj_set_style_text_color(s, UI_TEXT, 0);
    return s;
}

void ui_open(const ui_screen_t *s)
{
    ui_overlay_close();
    lv_obj_t *old = cur_scr;
    lv_obj_t *scr = new_screen();
    cur = s;
    cur_scr = scr;
    strip_w = 343;
    s->build(scr);
    lv_screen_load(scr);
    if (old != home_scr)
        lv_obj_delete(old);
    msg_until = 0;
    frame_refresh();
}

void ui_home(void)
{
    ui_overlay_close();
    lv_obj_t *old = cur_scr;
    cur = &ui_scr_home;
    cur_scr = home_scr;
    strip_w = 343;
    lv_screen_load(home_scr);
    if (service) {
        service = false;
        show(frame, true);
    } else if (old != home_scr) {
        lv_obj_delete(old);
    }
    frame_refresh();
}

void ui_service(void)
{
    ui_overlay_close();
    show(frame, false);
    service = true;
    show_main_screen();
    if (cur_scr != home_scr)
        lv_obj_delete(cur_scr);
    cur = &ui_scr_home;
    cur_scr = home_scr;
}

bool ui_on_service(void) { return service; }

/* --------------------------------------------------------------------------
 * Overlays: keypad and confirm, over 0,40 480x232 (STOP stays on top)
 * ------------------------------------------------------------------------ */
bool ui_overlay_open(void) { return overlay != NULL; }

void ui_overlay_close(void)
{
    if (overlay) {
        lv_obj_delete(overlay);
        overlay = NULL;
    }
}

static lv_obj_t *overlay_new(void)
{
    ui_overlay_close();
    overlay = plain(cur_scr);
    lv_obj_set_pos(overlay, 0, 40);
    lv_obj_set_size(overlay, 480, 232);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);   // nothing under it is touched
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(overlay, UI_BG, 0);
    return overlay;
}

// Keypad.
static struct {
    char entry[10];
    int32_t min10, max10;
    ui_keypad_set_t on_set;
    lv_obj_t *value, *title, *info, *err;
} kp;

static void kp_show_err(const char *why)
{
    show(kp.title, why == NULL);
    show(kp.info, why == NULL);
    show(kp.err, why != NULL);
    if (why)
        ui_set_text(kp.err, why);
}

static bool kp_parse(int32_t *v10)
{
    const char *s = kp.entry;
    int32_t whole = 0, frac = 0;
    bool neg = *s == '-';
    if (neg) s++;
    if (!*s)
        return false;
    while (*s >= '0' && *s <= '9')
        whole = whole * 10 + (*s++ - '0');
    if (*s == '.') {
        s++;
        if (*s >= '0' && *s <= '9')
            frac = *s - '0';
    }
    *v10 = (neg ? -1 : 1) * (whole * 10 + frac);
    return true;
}

static void kp_key(lv_event_t *e)
{
    const char *k = lv_event_get_user_data(e);
    size_t n = strlen(kp.entry);
    char *dot = strchr(kp.entry, '.');
    kp_show_err(NULL);
    if (strcmp(k, "<") == 0) {
        if (n) kp.entry[n - 1] = '\0';
    } else if (strcmp(k, "set") == 0) {
        int32_t v10;
        const char *why = NULL;
        if (!kp_parse(&v10)) {
            ui_overlay_close();
            return;
        }
        if (v10 < kp.min10 || v10 > kp.max10) {
            char a[16], b[16], t[48];
            lv_snprintf(t, sizeof t, "Outside %s to %s", ui_fmt1(a, sizeof a, kp.min10 * 100),
                        ui_fmt1(b, sizeof b, kp.max10 * 100));
            kp_show_err(t);
            return;
        }
        if (!kp.on_set(v10, &why)) {
            kp_show_err(why ? why : "Refused");
            return;
        }
        ui_overlay_close();
        return;
    } else if (n < sizeof kp.entry - 1) {
        if (*k == '.') {
            if (dot) return;
            if (!n) { strcpy(kp.entry, "0"); n = 1; }
        } else if (dot && strlen(dot) > 1) {
            return;                           // one decimal
        } else if (n == 1 && kp.entry[0] == '0') {
            n = 0;                            // no leading zero
        }
        kp.entry[n] = *k;
        kp.entry[n + 1] = '\0';
    }
    ui_set_text(kp.value, kp.entry);
}

void ui_keypad(const char *title, const char *info, int32_t min10, int32_t max10,
               const char *side_label, const char *side_value, ui_keypad_set_t on_set)
{
    lv_obj_t *o = overlay_new();
    memset(&kp, 0, sizeof kp);
    kp.min10 = min10;
    kp.max10 = max10;
    kp.on_set = on_set;

    lv_obj_t *card = ui_card(o, 10, 8, 343, 48);
    kp.title = ui_label(card, 10, 6, 180, &ui_font_12, UI_TEXT, LV_TEXT_ALIGN_LEFT, title);
    kp.info = ui_label(card, 10, 24, 180, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, info);
    kp.err = ui_label(card, 10, 6, 176, &ui_font_12, UI_FAULT, LV_TEXT_ALIGN_LEFT, "");
    lv_label_set_long_mode(kp.err, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(kp.err, LV_OBJ_FLAG_HIDDEN);
    kp.value = ui_label(card, 190, 10, 143, &ui_num_26, UI_TEXT, LV_TEXT_ALIGN_RIGHT, "");

    static const char *const keys[3][4] = { { "7", "8", "9", "<" }, { "4", "5", "6", "0" }, { "1", "2", "3", "." } };
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++) {
            const char *k = keys[r][c];
            bool bs = *k == '<';
            lv_obj_t *b = ui_btn(o, 10 + c * 70, 64 + r * 56, bs ? 132 : 62, 48, UI_SECONDARY,
                                 bs ? LV_SYMBOL_BACKSPACE : k, kp_key, (void *)k);
            if (bs)
                lv_obj_set_style_text_font(lv_obj_get_child(b, 0), &lv_font_montserrat_14, 0);
        }
    ui_btn(o, 290, 120, 62, 104, UI_PRIMARY, "Set", kp_key, (void *)"set");

    lv_obj_t *side = ui_card(o, 361, 8, 109, 160);
    ui_label(side, 10, 52, 89, &ui_font_12, UI_MUTED, LV_TEXT_ALIGN_LEFT, side_label);
    ui_label(side, 4, 72, 97, &ui_num_26, UI_TEXT, LV_TEXT_ALIGN_RIGHT, side_value);
    frame_refresh();
}

// Confirm.
static void (*confirm_ok)(void);

static void confirm_event(lv_event_t *e)
{
    bool ok = lv_event_get_user_data(e) != NULL;
    void (*f)(void) = confirm_ok;
    ui_overlay_close();
    if (ok && f)
        f();
    frame_refresh();
}

void ui_confirm(const char *text, const char *ok, void (*on_ok)(void))
{
    lv_obj_t *o = overlay_new();
    lv_obj_set_style_bg_color(o, lv_color_make(40, 50, 60), 0);
    lv_obj_set_style_bg_opa(o, 140, 0);
    confirm_ok = on_ok;
    lv_obj_t *box = ui_card(o, 70, 0, 340, 100);
    lv_obj_set_style_radius(box, 8, 0);
    lv_obj_t *t = ui_label(box, 16, 14, 308, &ui_font_16, UI_TEXT, LV_TEXT_ALIGN_LEFT, text);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_update_layout(t);
    int32_t th = lv_obj_get_height(t);
    if (th > 84) th = 84;
    int32_t by = 14 + th + 10, h = by + 60;
    lv_obj_set_height(t, th);
    lv_obj_set_size(box, 340, h);
    lv_obj_set_y(box, (168 - h) / 2);
    ui_btn(box, 16, by, 150, 48, UI_SECONDARY, "Cancel", confirm_event, NULL);
    ui_btn(box, 174, by, 150, 48, UI_PRIMARY, ok, confirm_event, (void *)1);
    frame_refresh();
}

/* --------------------------------------------------------------------------
 * Start-up
 * ------------------------------------------------------------------------ */
void ui_init(void)
{
    num_font(&ui_num_16, &ui_font_16);
    num_font(&ui_num_26, &ui_font_26);
    styles_init();
    home_scr = new_screen();
    cur_scr = home_scr;
    ui_scr_home.build(home_scr);
    build_frame();
    lv_screen_load(home_scr);
    frame_refresh();
    lv_timer_create(refresh_timer, 500, NULL);
}
