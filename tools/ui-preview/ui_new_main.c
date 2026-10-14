/*
 * Host render of the stage 4 screens (ui.c, ui_home.c): Home, Menu, the
 * keypad and the confirm dialog, against fake readings (fake_hw.c) and a
 * fake valve layer below. Writes one 480 x 272 frame as raw RGB565 and
 * checks the LVGL heap comes back after opening and leaving screens.
 *
 *   ./ui_new <state> <out.raw>
 *   state: notcal | manual | calib | moving | nodruck | stop | neg | menu | keypad | confirm
 */
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
void valve_stop(void) { v_f12 = true; moving = false; }
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
bool valve_question(int32_t *p) { (void)p; return false; }
void show_main_screen(void) {}
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
    if (!strcmp(st, "manual") || !strcmp(st, "moving") || !strcmp(st, "stop") || !strcmp(st, "keypad") ||
        !strcmp(st, "neg")) {
        v_mode = VALVE_MANUAL; v_cal = true; v_pct10 = 342;
    }
    if (!strcmp(st, "moving")) { moving = true; v_target10 = 500; }
    if (!strcmp(st, "calib")) { v_mode = VALVE_CALIBRATING; v_step = 2; }
    if (!strcmp(st, "nodruck")) v_f01 = true;

    lv_init();
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
    run(1200);
    size_t after = heap_used();
    fprintf(stderr, "%s: heap used %zu B, after 10 x Menu and a keypad %zu B\n", st, before, after);
    if (after > before + 64) {
        fprintf(stderr, "heap grew\n");
        return 1;
    }

    if (!strcmp(st, "stop")) tap(415, 240);
    if (!strcmp(st, "menu")) tap(415, 184);
    if (!strcmp(st, "keypad")) { tap(300, 184); tap(41, 184); tap(111, 240); tap(251, 240); tap(111, 184); }
    if (!strcmp(st, "confirm")) tap(415, 72);
    run(1200);

    FILE *f = fopen(argv[2], "wb");
    if (!f) { perror(argv[2]); return 1; }
    fwrite(fb, sizeof fb, 1, f);
    fclose(f);
    return 0;
}
