#include <math.h>
#include <stdio.h>
#include <string.h>

#include "tune.h"

// tune.h has the method. Positions are in steps, pressures in mbar,
// recorded readings in 0.001 mbar from the step's baseline.

static float pct_of(const tune_t *t, int32_t steps) { return 100.0f * (float)steps / (float)t->pos_max; }

// v to dec decimals (0 to 3), without printf's float support.
static const char *fx(char *buf, size_t n, float v, int dec)
{
    long scale = dec == 0 ? 1 : dec == 1 ? 10 : dec == 2 ? 100 : 1000;
    long q = lroundf(fabsf(v) * (float)scale);
    if (dec == 0)
        snprintf(buf, n, "%s%ld", v < 0 && q ? "-" : "", q);
    else
        snprintf(buf, n, "%s%ld.%0*ld", v < 0 && q ? "-" : "", q / scale, dec, q % scale);
    return buf;
}

void tune_init(tune_t *t)
{
    memset(t, 0, sizeof *t);
}

bool tune_running(const tune_t *t)
{
    return t->phase == TUNE_WAIT || t->phase == TUNE_OUT || t->phase == TUNE_BACK;
}

int tune_elapsed(const tune_t *t)
{
    return t->phase == TUNE_WAIT ? t->waited : t->n;
}

void tune_fail(tune_t *t, const char *why)
{
    t->phase = TUNE_FAILED;
    snprintf(t->why, sizeof t->why, "%s", why);
}

// A failure found here also takes the valve back to where it started.
static void fail_back(tune_t *t, const char *why)
{
    tune_fail(t, why);
    t->move = true;
    t->move_to = t->pos0;
}

const char *tune_start(tune_t *t, int32_t pos, int32_t pos_max, float step_pct, float pv)
{
    if (pos_max <= 0)
        return "valve travel not set";
    if (!(fabsf(step_pct) >= 0.1f && fabsf(step_pct) <= TUNE_STEP_MAX_PCT))
        return "step must be 0.1 to 20 %";
    int32_t step = (int32_t)lroundf(step_pct / 100.0f * (float)pos_max);
    if (pos + step < 0 || pos + step > pos_max)
        return step > 0 ? "step would pass 100 %, use a negative step" : "step would pass 0 %, use a positive step";
    tune_init(t);
    t->phase = TUNE_WAIT;
    t->pos_max = pos_max;
    t->pos0 = t->from = pos;
    t->step = step;
    t->p_start = pv;
    return NULL;
}

static void begin_step(tune_t *t)
{
    t->n = 0;
    t->move = true;
    t->move_to = t->from + t->step;
}

static float rec_mean(const tune_t *t, int from, int count)
{
    float s = 0;
    for (int i = from; i < from + count; i++)
        s += t->rec[i];
    return s / (float)count;
}

// Two-point fit of the step just recorded; dy is its settled change, mbar.
static bool fit_step(const tune_t *t, float dy, tune_fit_t *f)
{
    int t28 = -1, t63 = -1;
    for (int i = 2; i < t->n - 2 && t63 < 0; i++) {
        float frac = rec_mean(t, i - 2, 5) / 1000.0f / dy;   // 5 s centred mean
        if (t28 < 0 && frac >= 0.283f)
            t28 = i;
        if (frac >= 0.632f)
            t63 = i;
    }
    if (t28 < 0 || t63 < 0)
        return false;
    f->tau = 1.5f * (float)(t63 - t28);
    if (f->tau < 1.0f)
        f->tau = 1.0f;
    f->theta = (float)t63 - f->tau;
    if (f->theta < 0.0f)
        f->theta = 0.0f;
    f->k = dy / pct_of(t, t->step);
    return true;
}

static void result(tune_t *t)
{
    const tune_fit_t *a = &t->fit[0], *b = &t->fit[1];
    char why[72], x[4][12];
    if ((a->k > 0) != (b->k > 0)) {
        fail_back(t, "the two steps moved the pressure opposite ways");
        return;
    }
    float k = (a->k + b->k) / 2, tau = (a->tau + b->tau) / 2, theta = (a->theta + b->theta) / 2;
    if (fabsf(a->k - b->k) > TUNE_AGREE_K * fabsf(k) || fabsf(a->tau - b->tau) > TUNE_AGREE_TAU * tau) {
        snprintf(why, sizeof why, "the two steps disagree (K %s and %s, tau %s and %s s)", fx(x[0], 12, a->k, 3),
                 fx(x[1], 12, b->k, 3), fx(x[2], 12, a->tau, 0), fx(x[3], 12, b->tau, 0));
        fail_back(t, why);
        return;
    }
    float kp = tau / (fabsf(k) * (tau + theta)), ti = tau / 60.0f;
    t->kp = kp < 0.01f ? 0.01f : kp > 100.0f ? 100.0f : kp;
    t->ti = ti < 0.01f ? 0.01f : ti > 100.0f ? 100.0f : ti;
    t->phase = TUNE_DONE;
    t->move = true;
    t->move_to = t->pos0;
}

// The step has settled with change dy, mbar.
static void step_done(tune_t *t, float dy)
{
    char why[72], x[2][12];
    if (t->phase == TUNE_OUT && fabsf(dy) < TUNE_DYMIN_MBAR) {
        // Too small to fit: twice the step, from here.
        int32_t next = 2 * t->step, at = t->from + t->step;
        if (fabsf(pct_of(t, next)) > TUNE_STEP_MAX_PCT + 0.01f) {
            snprintf(why, sizeof why, "pressure moved only %s mbar with a %s %% step", fx(x[0], 12, dy, 2),
                     fx(x[1], 12, pct_of(t, t->step), 1));
            fail_back(t, why);
            return;
        }
        if (at + next < 0 || at + next > t->pos_max) {
            fail_back(t, next > 0 ? "a larger step would pass 100 %" : "a larger step would pass 0 %");
            return;
        }
        t->y0 += dy;
        t->from = at;
        t->step = next;
        begin_step(t);
        return;
    }
    tune_fit_t *f = &t->fit[t->phase == TUNE_OUT ? 0 : 1];
    if (fabsf(dy) < TUNE_DYMIN_MBAR / 2 || !fit_step(t, dy, f)) {
        snprintf(why, sizeof why, "no clear response to the step %s (%s mbar)",
                 t->phase == TUNE_OUT ? "out" : "back", fx(x[0], 12, dy, 2));
        fail_back(t, why);
        return;
    }
    if (t->phase == TUNE_BACK) {
        result(t);
        return;
    }
    t->phase = TUNE_BACK;
    t->y0 += dy;
    t->from += t->step;
    t->step = -t->step;
    begin_step(t);
}

void tune_step(tune_t *t, float pv, int32_t pos, bool moving)
{
    char why[72], x[12];
    if (!tune_running(t))
        return;
    if (t->phase == TUNE_WAIT) {
        t->waited++;
        if (moving) {
            t->win_n = 0;
        } else {
            if (t->win_n == TUNE_STEADY_S) {
                memmove(t->win, t->win + 1, sizeof t->win - sizeof t->win[0]);
                t->win_n--;
            }
            t->win[t->win_n++] = pv;
        }
        if (t->win_n == TUNE_STEADY_S) {
            float lo = t->win[0], hi = lo, s = 0;
            for (int i = 0; i < TUNE_STEADY_S; i++) {
                lo = t->win[i] < lo ? t->win[i] : lo;
                hi = t->win[i] > hi ? t->win[i] : hi;
                s += t->win[i];
            }
            if (hi - lo <= TUNE_STEADY_MBAR) {
                t->y0 = t->p_start = s / TUNE_STEADY_S;
                t->from = t->pos0 = pos;
                t->phase = TUNE_OUT;
                begin_step(t);
                return;
            }
        }
        if (t->waited >= TUNE_WAIT_MAX_S) {
            snprintf(why, sizeof why, "pressure not steady to %s mbar over %d s", fx(x, sizeof x, TUNE_STEADY_MBAR, 2),
                     TUNE_STEADY_S);
            tune_fail(t, why);   // nothing has moved yet
        }
        return;
    }
    if (fabsf(pv - t->p_start) > TUNE_WINDOW_MBAR) {
        snprintf(why, sizeof why, "pressure moved over %s mbar from the start, use a smaller step",
                 fx(x, sizeof x, TUNE_WINDOW_MBAR, 1));
        fail_back(t, why);
        return;
    }
    float d = (pv - t->y0) * 1000.0f;
    t->rec[t->n++] = (int16_t)(d > 32767 ? 32767 : d < -32767 ? -32767 : d);
    if (t->n >= TUNE_SETTLE_MIN_S && !moving) {
        float a = rec_mean(t, t->n - 30, 30), b = rec_mean(t, t->n - 150, 30);
        float tol = 0.02f * fabsf(a) > 20.0f ? 0.02f * fabsf(a) : 20.0f;   // 2 %, at least 0.02 mbar
        if (fabsf(a - b) <= tol) {
            step_done(t, a / 1000.0f);
            return;
        }
    }
    if (t->n >= TUNE_SETTLE_MAX_S) {
        snprintf(why, sizeof why, "step did not settle in %d s", TUNE_SETTLE_MAX_S);
        fail_back(t, why);
    }
}
