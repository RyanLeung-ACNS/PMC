#include <string.h>

#include "lvgl.h"        // lv_tick_get, lv_strlcpy
#include "pmc_config.h"
#include "link.h"
#include "clicks.h"
#include "cal.h"
#include "stepper3.h"
#include "fixed.h"
#include "valve.h"
#include "trend.h"
#include "sense.h"

/*
 * Once a second from the main loop (was main_screen.c to 0.8.x): re-read
 * the boards, update the cal and valve fault checks. Outside LVGL, so a
 * slow redraw does not delay the reading or the fault checks.
 *
 * Engineering commands (PMC_ENG_COMMANDS, a bench build): DATA? one CSV
 * line of the last reading, DATA HDR? the column names:
 *   DATA,t_s,p_mbar,target_mbar,valve_pct,mode,alarm,ratio_mV_per_V,exc_mV,die_C,baro_mbar
 * t_s since start-up, 0.1 s. p_mbar and baro_mbar 0.01 mbar, target 0.1
 * mbar, valve 0.01 %. Empty: no value. alarm: the highest latched fault,
 * else the warning on the strip, else empty. SENSE? gives the live Power
 * Monitor and Druck readings. The remote set (remote.c) has PRES?, VALV?,
 * ALRM? and the rest.
 */
#if PMC_ENG_COMMANDS
#define DATA_COLUMNS "t_s,p_mbar,target_mbar,valve_pct,mode,alarm,ratio_mV_per_V,exc_mV,die_C,baro_mbar"

static void print_data_line(const clicks_state_t *s);

bool sense_data_command(const char *line)
{
    if (strcmp(line, "DATA HDR?") == 0)
        link_printf("DATA HDR %s\r\nOK\r\n", DATA_COLUMNS);
    else if (strcmp(line, "DATA?") == 0)
        print_data_line(clicks_state());
    else if (strcmp(line, "SENSE?") == 0)
        link_printf("%s\r\n%s\r\nOK\r\n", clicks_powermonitor_status(), clicks_druck_status());
    else
        return false;
    return true;
}

// The warning on the strip (W-1 question first), else W-3 controller hot, else "".
static const char *warning_tag(char *buf, size_t n)
{
    int32_t at, mc;
    if (valve_question(&at))
        return "W-1";
    const char *w = valve_warning();   // "W-4 Switch pressed ...": the tag is the first word
    if (w) {
        size_t k = strcspn(w, " ");
        if (k >= n)
            k = n - 1;
        memcpy(buf, w, k);
        buf[k] = 0;
        return buf;
    }
    return sense_hot(&mc) ? "W-3" : "";
}

// The highest latched fault, else the warning on the strip, else "".
static const char *alarm_tag(char *buf, size_t n)
{
    const char *tag, *text;
    if (valve_fault_first(&tag, &text))
        return tag;
    return warning_tag(buf, n);
}

static void print_data_line(const clicks_state_t *s)
{
    char p[16] = "", sp[16] = "", v[16] = "", r[16] = "", die[16], baro[16] = "", al[8];
    uint32_t ms = lv_tick_get();
    int32_t t;

    if (s->reading_ok) {
        fixed_fmt(p, sizeof p, fixed_round(s->p_mmbar, 1), 2);
        fixed_fmt(r, sizeof r, s->r_ppb, 6);
    }
    if (valve_target(&t))
        fixed_fmt(sp, sizeof sp, fixed_round(t, 2), 1);
    if (valve_reference() != VALVE_REF_NONE)
        fixed_fmt(v, sizeof v, fixed_round(valve_pct_milli(), 1), 2);
    if (s->baro_ok)
        fixed_fmt(baro, sizeof baro, fixed_round(s->baro_mmbar, 1), 2);
    link_printf("DATA,%lu.%lu,%s,%s,%s,%s,%s,%s,%ld,%s,%s\r\nOK\r\n",
                (unsigned long)(ms / 1000), (unsigned long)(ms % 1000 / 100), p, sp, v,
                valve_mode_name(), alarm_tag(al, sizeof al), r, (long)s->bus_mv,
                fixed_fmt(die, sizeof die, fixed_round(s->die_mc, 2), 1), baro);
}
#endif

static bool hot;
static int32_t hot_mc;

static void hot_check(const clicks_state_t *s)
{
    if (!s->pm_found)
        return;   // no reading: as it was
    hot_mc = s->die_mc;
    char t[16];
    if (!hot && s->die_mc >= SENSE_HOT_MC) {
        hot = true;
        valve_event("W-3", "Controller hot, %s C", fixed_fmt(t, sizeof t, s->die_mc / 100, 1));
    } else if (hot && s->die_mc < SENSE_COOL_MC) {
        hot = false;
        valve_event("W-3", "Controller cooled, %s C", fixed_fmt(t, sizeof t, s->die_mc / 100, 1));
    }
}

bool sense_hot(int32_t *die_mc)
{
    *die_mc = hot_mc;
    return hot;
}

void sense_second(void)
{
    clicks_poll();

    const clicks_state_t *s = clicks_state();
    cal_update(s);
    valve_sense(s);
    trend_second(s);
    hot_check(s);
}
