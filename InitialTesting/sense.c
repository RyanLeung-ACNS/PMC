#include <string.h>

#include "lvgl.h"        // lv_tick_get, lv_strlcpy
#include "link.h"
#include "clicks.h"
#include "cal.h"
#include "sysinfo.h"
#include "fixed.h"
#include "valve.h"
#include "trend.h"
#include "sense.h"

/*
 * Once a second from the main loop (was main_screen.c to 0.8.x): re-read
 * the boards, update the cal and valve fault checks, print any
 * changed status line and the DATA line. Outside LVGL, so a slow redraw
 * does not delay the reading or the fault checks.
 *
 * With DATA ON, once a second, one CSV line for logging (tools/logger);
 * DATA? prints the column names (0.11.52):
 *   DATA,t_s,p_mbar,target_mbar,valve_pct,mode,alarm,ratio_mV_per_V,exc_mV,die_C,baro_mbar
 * t_s since start-up, 0.1 s. p_mbar and baro_mbar 0.01 mbar, target 0.1
 * mbar, valve 0.01 %. Empty: p_mbar and ratio with no valid reading,
 * target_mbar with none set, valve_pct before the valve is homed or its
 * position set, baro_mbar with no Pressure 21 reading. alarm: the highest
 * latched fault, else the warning on the strip, else empty.
 * Lines not starting with DATA are status text, each printed when its
 * state changes; SENSE? gives the live Power Monitor and Druck readings.
 */
#define DATA_COLUMNS "t_s,p_mbar,target_mbar,valve_pct,mode,alarm,ratio_mV_per_V,exc_mV,die_C,baro_mbar"
#define STATUS_LINES 6
static const char *last_line[STATUS_LINES];
static char line_copy[STATUS_LINES][192];

// DATA lines once a second. Off at boot; DATA ON (the logger sends it)
// turns them on, DATA OFF pauses them.
static bool data_on = false;

bool sense_data_command(const char *line)
{
    if (strcmp(line, "DATA?") == 0) {
        link_printf("DATA %s\r\nOK\r\n", DATA_COLUMNS);
        return true;
    }
    if (strcmp(line, "SENSE?") == 0) {
        link_printf("%s\r\n%s\r\nOK\r\n", clicks_powermonitor_status(), clicks_druck_status());
        return true;
    }
    if (strcmp(line, "DATA OFF") == 0)
        data_on = false;
    else if (strcmp(line, "DATA ON") == 0)
        data_on = true;
    else
        return false;
    link_printf("OK data %s\r\n", data_on ? "on" : "off");
    return true;
}

// The highest latched fault, else the warning on the strip, else "".
static const char *alarm_tag(char *buf, size_t n)
{
    const char *tag, *text;
    int32_t at, mc;
    if (valve_fault_first(&tag, &text))
        return tag;
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
    link_printf("DATA,%lu.%lu,%s,%s,%s,%s,%s,%s,%ld,%s,%s\r\n",
                (unsigned long)(ms / 1000), (unsigned long)(ms % 1000 / 100), p, sp, v,
                valve_mode_name(), alarm_tag(al, sizeof al), r, (long)s->bus_mv,
                fixed_fmt(die, sizeof die, fixed_round(s->die_mc, 2), 1), baro);
}

// The Power Monitor and Druck lines carry live readings. Only the words
// before the first number after the name are compared, so they print when
// their state changes (found, lost, not connected), not every second.
static size_t state_len(const char *s)
{
    const char *c = strstr(s, ": ");
    if (!c)
        return strlen(s);
    for (c += 2; *c && !(*c >= '0' && *c <= '9') && *c != '-'; c++)
        ;
    return (size_t)(c - s);
}

// The motor and valve lines: the same apart from the numbers (count,
// position, target), so they print when a move starts or ends, a homing
// stage or the switch changes, not as the count runs. A number is a word
// that starts with a digit or "-digit"; the 4 in F-4 is not one.
static bool is_num(const char *s, const char *at)
{
    if (at > s && at[-1] != ' ' && at[-1] != '(')
        return false;
    return (*at >= '0' && *at <= '9') || (*at == '-' && at[1] >= '0' && at[1] <= '9');
}

static const char *skip_num(const char *c)
{
    if (*c == '-')
        c++;
    while ((*c >= '0' && *c <= '9') || *c == '.')
        c++;
    return c;
}

static bool same_words(const char *a, const char *b)
{
    const char *x = a, *y = b;
    for (;;) {
        bool nx = is_num(a, x), ny = is_num(b, y);
        if (nx != ny)
            return false;
        if (nx) {
            x = skip_num(x);
            y = skip_num(y);
            continue;
        }
        if (*x != *y)
            return false;
        if (!*x)
            return true;
        x++;
        y++;
    }
}

typedef enum { SAME_ALL, SAME_STATE, SAME_WORDS } same_t;

static bool same_state(const char *a, const char *b, same_t how)
{
    if (how == SAME_WORDS)
        return same_words(a, b);
    if (how == SAME_ALL)
        return strcmp(a, b) == 0;
    size_t n = state_len(a);
    return n == state_len(b) && strncmp(a, b, n) == 0;
}

static void print_changed_lines(void)
{
    const char *line[STATUS_LINES] = { sysinfo_line(), clicks_stepper3_status(), clicks_boost10_status(),
                                       clicks_powermonitor_status(), clicks_druck_status(), valve_status() };

    // A PC that connects later still gets every line once.
    bool resend = link_take_new_client();

    static const same_t how[STATUS_LINES] = { SAME_ALL, SAME_WORDS, SAME_ALL, SAME_STATE, SAME_STATE, SAME_WORDS };
    // In AUTO the motor line changes every second: DATA carries the position.
    bool in_auto = valve_auto();

    for (int i = 0; i < STATUS_LINES; i++) {
        if (!line[i][0] || (i == 1 && in_auto))
            continue;
        if (resend || last_line[i] == NULL || !same_state(line_copy[i], line[i], how[i])) {
            lv_strlcpy(line_copy[i], line[i], sizeof line_copy[i]);
            last_line[i] = line_copy[i];
            link_printf("%s\r\n", line[i]);
        }
    }
}

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
    print_changed_lines();

    const clicks_state_t *s = clicks_state();
    cal_update(s);
    valve_sense(s);
    trend_second(s);
    hot_check(s);
    if (s->pm_found && data_on)
        print_data_line(s);
}
