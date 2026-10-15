/*
 * Host render of the new screens (ui.c, ui_home.c, ui_valve.c): Home, Menu,
 * the keypad and dialogs, and the stage 5 pages, against fake readings
 * (fake_hw.c) and a fake valve layer below. Writes one 480 x 272 frame as raw RGB565 and
 * checks the LVGL heap comes back after opening and leaving screens.
 *
 *   ./ui_new <state> <out.raw>
 *   state: notcal | manual | calib | moving | nodruck | stop | neg | menu | keypad | confirm
 *          | valve | valvenc | limithit | calpage | faults | detail | diag | help
 */
#ifndef TVX
#define TVX 444
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "clicks.h"
#include "stepper3.h"
#include "valve.h"
#include "ui.h"

#define W 480
#define H 272
#ifndef POOL2_KB
#define POOL2_KB 24
#endif

void fake_hw_set_scenario(const char *name);

static uint16_t fb[W * H];
static uint8_t draw_buf[W * H * 2] __attribute__((aligned(4)));

/* Fake valve layer ------------------------------------------------------- */
static valve_mode_t v_mode = VALVE_NOT_CAL;
static bool v_cal, v_f01, v_f12;
static int v_step = -1;
static int32_t v_pct10, v_target10;
static bool moving;

valve_mode_t valve_mode(void) { return v_mode; }
const char *valve_mode_name(void)
{
    return v_mode == VALVE_CALIBRATING ? "CALIBRATING" : v_mode == VALVE_MANUAL ? "MANUAL" : "NOT CALIBRATED";
}
bool valve_calibrated(void) { return v_cal; }
int32_t valve_pct10(void) { return v_pct10; }
int32_t valve_target_pct10(void) { return moving ? v_target10 : v_pct10; }
int valve_cal_step(void) { return v_step; }
uint32_t valve_cal_budget_s(void) { return 300; }
bool valve_calibrate(const char **why) { (void)why; v_mode = VALVE_CALIBRATING; v_step = 1; return true; }
bool valve_goto_pct10(int32_t p, const char **why) { (void)why; v_target10 = p; moving = true; return true; }
void valve_stop(void) { moving = false; }
bool valve_stop_latches(void) { return false; }
int valve_fault_count(void) { return v_f01 + v_f12; }
bool valve_fault_first(const char **tag, const char **text)
{
    if (v_f01) { *tag = "F-01"; *text = "No Druck signal"; return true; }
    if (v_f12) { *tag = "F-12"; *text = "Operator STOP"; return true; }
    return false;
}
bool valve_fault_is(const char *tag)
{
    return (v_f01 && !strcmp(tag, "F-01")) || (v_f12 && !strcmp(tag, "F-12"));
}
int valve_reset(char *left, size_t n) { int a = valve_fault_count(); v_f12 = false; if (n) strcpy(left, v_f01 ? "F-01" : ""); return a; }
static bool v_q;
bool valve_question(int32_t *p) { if (v_q && p) *p = 187; return v_q; }
bool valve_answer(bool z, const char **why) { (void)z; (void)why; v_q = false; return true; }
bool valve_fault_get(int i, valve_fault_t *f)
{
    const char *tags[2] = { "F-01", "F-12" }, *texts[2] = { "No Druck signal", "Operator STOP" };
    int k = 0;
    for (int j = 0; j < 2; j++) {
        if (!(j ? v_f12 : v_f01)) continue;
        if (k++ != i) continue;
        f->tag = tags[j]; f->text = texts[j]; f->age_ms = j ? 64000 : 312000;
        f->mode = VALVE_MANUAL; f->can_reset = j == 1;
        return true;
    }
    return false;
}
static const char *const ev_tag[] = { "F-12", "W-01", "REF", "REF", "F-14" };
static const char *const ev_text[] = { "Operator stop at count 33880", "Point set as 0 %, count 0",
    "Calibrated in 15 s, position 0", "Calibrating started at count 1203", "Power-up (event only), valve not calibrated" };
int valve_event_count(void) { return 5; }
uint32_t valve_event_total(void) { return 5; }
bool valve_event_get(int i, uint32_t *age, const char **tag, const char **text)
{
    if (i < 0 || i >= 5) return false;
    *age = 64000 + i * 61000; *tag = ev_tag[i]; *text = ev_text[i];
    return true;
}
bool valve_last_cal(uint32_t *age, uint32_t *took) { if (!v_cal) return false; *age = 186000; *took = 15; return true; }
bool valve_beyond_switch(int32_t *s) { *s = 0; return false; }
bool limit_open_now(void) { return v_cal && v_pct10 == 0; }
const char *rtclock_iso(void) { return "2026-10-15 14:32:05"; }
const char *valve_last_cal_iso(void) { return "2026-10-05 14:28:59"; }
bool stepper3_last_move(int32_t *from, uint32_t *ms) { if (!v_cal) return false; *from = 0; *ms = 41300; return true; }
void stepper3_stop_ramped(void) {}
#ifndef REAL_OLD
void show_main_screen(void) {}
void init_screens(void) {}
#endif
bool stepper3_busy(void);

/* Render ------------------------------------------------------------------ */
static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    const uint16_t *src = (const uint16_t *)px;
    for (int32_t y = a->y1; y <= a->y2; y++)
        for (int32_t x = a->x1; x <= a->x2; x++)
            fb[y * W + x] = *src++;
    lv_display_flush_ready(d);
}

static lv_point_t touch_point;
static bool touch_down;
static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point = touch_point;
    data->state = touch_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void run(int ms)
{
    for (int t = 0; t < ms; t += 20) { lv_tick_inc(20); lv_timer_handler(); }
}

static void tap(int32_t x, int32_t y)
{
    touch_point.x = x;
    touch_point.y = y;
    touch_down = true;
    run(100);
    touch_down = false;
    run(100);
}

static void drag(int32_t x, int32_t y0, int32_t y1)
{
    touch_point.x = x;
    touch_point.y = y0;
    touch_down = true;
    run(60);
    for (int i = 1; i <= 10; i++) {
        touch_point.y = y0 + (y1 - y0) * i / 10;
        run(20);
    }
    touch_down = false;
    run(600);
}

static size_t heap_used(void)
{
    lv_mem_monitor_t m;
    lv_mem_monitor(&m);
    return m.total_size - m.free_size;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <state> out.raw\n", argv[0]);
        return 2;
    }
    const char *st = argv[1];
    fake_hw_set_scenario(!strcmp(st, "nodruck") ? "nodruck" : !strcmp(st, "neg") ? "neg" : "cal");
    bool page = !strcmp(st, "valve") || !strcmp(st, "valvenc") || !strcmp(st, "limithit") ||
                !strcmp(st, "calpage") || !strcmp(st, "faults") || !strcmp(st, "detail") || !strcmp(st, "diag") ||
                !strcmp(st, "help") || !strcmp(st, "help2") || !strcmp(st, "help3") || !strcmp(st, "back") || !strcmp(st, "svc");
    if (!strcmp(st, "manual") || !strcmp(st, "moving") || !strcmp(st, "stop") || !strcmp(st, "keypad") ||
        !strcmp(st, "neg") || (page && strcmp(st, "valvenc"))) {
        v_mode = VALVE_MANUAL; v_cal = true; v_pct10 = 342;
    }
    if (!strcmp(st, "moving")) { moving = true; v_target10 = 500; }
    if (!strcmp(st, "calib")) { v_mode = VALVE_CALIBRATING; v_step = 2; }
    if (!strcmp(st, "nodruck")) v_f01 = true;
    if (!strcmp(st, "faults") || !strcmp(st, "detail")) { v_f01 = true; v_f12 = true; }

    lv_init();
    static uint8_t pool2[POOL2_KB * 1024] __attribute__((aligned(8)));   // 24 KB as main.c board_init; 64-bit pointers need more
    lv_mem_add_pool(pool2, sizeof pool2);
    lv_display_t *d = lv_display_create(W, H);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, draw_buf, NULL, sizeof draw_buf, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(d, flush_cb);
    lv_indev_t *in = lv_indev_create();
    lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in, touch_read);

    ui_init();
    run(1200);

    // Heap check: Menu open and back ten times.
    size_t before = heap_used();
    for (int i = 0; i < 10; i++) {
        tap(415, 184);          // Menu
        tap(60, 20);            // Back
    }
    tap(300, 184);              // Go to box (opens the keypad in MANUAL)
    tap(60, 20);                // Back closes it
    for (int i = 0; i < 5; i++) {
        tap(415, 128); tap(60, 20);                             // Valve
        tap(415, 184); tap(350, 72); tap(60, 20); tap(60, 20);  // Faults and log
        tap(415, 184); tap(120, 128); tap(60, 20); tap(60, 20); // Calibrate valve
        tap(415, 184); tap(120, 184); tap(60, 20); tap(60, 20); // Diagnostics
    }
    run(1200);
    size_t after = heap_used();
    lv_mem_monitor_t mm;
    lv_mem_monitor(&mm);
    fprintf(stderr, "%s: heap used %zu B, after the round trips %zu B, peak %zu B\n", st, before, after,
            (size_t)mm.max_used);
    if (after > before + 64) {
        fprintf(stderr, "heap grew\n");
        return 1;
    }

    if (!strcmp(st, "stop")) tap(415, 240);
    if (!strcmp(st, "menu")) tap(415, 184);
    if (!strcmp(st, "keypad")) { tap(300, 184); tap(41, 184); tap(111, 240); tap(251, 240); tap(111, 184); }
    if (!strcmp(st, "confirm")) tap(415, 72);
    if (!strcmp(st, "valve") || !strcmp(st, "valvenc") || !strcmp(st, "limithit")) tap(415, 128);
    if (!strcmp(st, "limithit")) v_q = true;
    if (!strcmp(st, "svc")) {
        size_t a = heap_used();
        tap(415, 184); tap(300, 240);     // Menu, Service (0.6.0 screens built)
        run(1200);
        size_t b = heap_used();
        ui_home(); run(600);
        size_t c = heap_used();
        tap(415, 128); run(600);           // Valve page
        size_t d = heap_used();
        tap(60, 184); run(600);            // Go to: keypad
        size_t f = heap_used();
        lv_mem_monitor_t m; lv_mem_monitor(&m);
        fprintf(stderr, "svc: home %zu, on service %zu, back home %zu, valve %zu, keypad %zu, peak %zu, total %zu\n",
                a, b, c, d, f, (size_t)m.max_used, (size_t)m.total_size);
    }
    if (!strcmp(st, "back")) { tap(415, 184); tap(120, 184); tap(60, 20); }   // Diagnostics, Back: Menu
    if (!strcmp(st, "help") || !strcmp(st, "help2") || !strcmp(st, "help3")) { tap(415, 128); tap(415, 184); }
    if (!strcmp(st, "help2")) tap(TVX, 172);
    if (!strcmp(st, "help3")) { tap(TVX, 172); tap(TVX, 172); tap(TVX, 172); tap(TVX, 172); }
    if (!strcmp(st, "faults") || !strcmp(st, "detail")) { tap(415, 184); tap(350, 72); }
    if (!strcmp(st, "detail")) tap(150, 72);
    if (!strcmp(st, "calpage")) { tap(415, 184); tap(120, 128); }
    if (!strcmp(st, "diag")) { tap(415, 184); tap(120, 184); }
    run(1200);

    FILE *f = fopen(argv[2], "wb");
    if (!f) { perror(argv[2]); return 1; }
    fwrite(fb, sizeof fb, 1, f);
    fclose(f);
    return 0;
}
