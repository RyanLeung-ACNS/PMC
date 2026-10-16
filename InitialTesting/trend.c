#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "link.h"
#include "valve.h"
#include "trend.h"

/*
 * Two rings: fine[] holds the last 600 one-second samples, coarse[] the
 * last 360 ten-second means (an hour). 9.6 KB of RAM. The newest sample
 * of each is at index (count - 1) % N.
 */
static trend_pt_t fine[TREND_FINE_N], coarse[TREND_COARSE_N];
static uint32_t fine_n, coarse_n;
static struct {
    int64_t p, v;
    int np, nv;
    int32_t sp;
} acc;

static int32_t mean(int64_t sum, int n)
{
    if (!n)
        return TREND_NONE;
    return (int32_t)(sum >= 0 ? (sum + n / 2) / n : (sum - n / 2) / n);
}

void trend_second(const clicks_state_t *s)
{
    if (s->pm_diag)
        return;
    trend_pt_t q;
    bool ok = s->pm_found && s->reading_ok && !s->boost_tripped;
    q.p = ok ? s->p_mmbar : TREND_NONE;
    q.sp = TREND_NONE;   // AUTO comes in stage 8
    if (valve_calibrated() && valve_mode() != VALVE_CALIBRATING) {
        int32_t v = (valve_pct_milli() + 5) / 10;
        q.v = v < 0 ? 0 : v > 10000 ? 10000 : v;
    } else {
        q.v = TREND_NONE;
    }
    fine[fine_n % TREND_FINE_N] = q;
    fine_n++;

    if (q.p != TREND_NONE) { acc.p += q.p; acc.np++; }
    if (q.v != TREND_NONE) { acc.v += q.v; acc.nv++; }
    acc.sp = q.sp;
    if (fine_n % 10 == 0) {
        trend_pt_t c = { mean(acc.p, acc.np), acc.sp, mean(acc.v, acc.nv) };
        coarse[coarse_n % TREND_COARSE_N] = c;
        coarse_n++;
        memset(&acc, 0, sizeof acc);
    }
}

uint32_t trend_seconds(void) { return fine_n; }

bool trend_fine(uint32_t age_s, trend_pt_t *o)
{
    if (age_s >= fine_n || age_s >= TREND_FINE_N)
        return false;
    *o = fine[(fine_n - 1 - age_s) % TREND_FINE_N];
    return true;
}

bool trend_coarse(uint32_t age_s, trend_pt_t *o)
{
    // The newest coarse sample covers the 10 s before the last multiple of
    // 10; the seconds since are only in fine[].
    uint32_t r = fine_n % 10;
    if (age_s < r)
        return trend_fine(age_s, o);
    uint32_t k = (age_s - r) / 10;
    if (k >= coarse_n || k >= TREND_COARSE_N)
        return false;
    *o = coarse[(coarse_n - 1 - k) % TREND_COARSE_N];
    return true;
}

static void put_fixed(char *buf, size_t n, int32_t v, int dec)
{
    if (v == TREND_NONE) {
        lv_strlcpy(buf, "-", n);
        return;
    }
    int32_t div = dec == 3 ? 1000 : 100;
    int32_t a = abs(v);
    lv_snprintf(buf, n, dec == 3 ? "%s%ld.%03ld" : "%s%ld.%02ld", v < 0 ? "-" : "",
                (long)(a / div), (long)(a % div));
}

bool trend_command(const char *line)
{
    if (strcmp(line, "TREND?") == 0) {
        uint32_t f = fine_n < TREND_FINE_N ? fine_n : TREND_FINE_N;
        uint32_t c = coarse_n < TREND_COARSE_N ? coarse_n : TREND_COARSE_N;
        link_printf("TREND %lu s since start-up, %lu x 1 s and %lu x 10 s kept (10 min and 1 h)\r\n",
                    (unsigned long)fine_n, (unsigned long)f, (unsigned long)c);
        link_printf("OK\r\n");
        return true;
    }
    if (strncmp(line, "TREND DUMP", 10) == 0) {
        char *end;
        long s = strtol(line + 10, &end, 10);
        if (end == line + 10 || *end || s < 1 || s > TREND_FINE_N) {
            link_printf("ERR expected: TREND DUMP <1-%d>\r\n", TREND_FINE_N);
            return true;
        }
        link_printf("TREND age_s, pressure mbar, target mbar, valve %%\r\n");
        for (long a = s - 1; a >= 0; a--) {
            trend_pt_t q;
            if (!trend_fine((uint32_t)a, &q))
                continue;
            char p[20], sp[20], v[20];
            put_fixed(p, sizeof p, q.p, 3);
            put_fixed(sp, sizeof sp, q.sp, 3);
            put_fixed(v, sizeof v, q.v, 2);
            link_printf("TREND -%ld, %s, %s, %s\r\n", a, p, sp, v);
        }
        link_printf("OK\r\n");
        return true;
    }
    return false;
}
