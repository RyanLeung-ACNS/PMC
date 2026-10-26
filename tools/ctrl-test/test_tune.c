/* Host test of tune.c (TUNE STEP): a synthetic first order plus dead time
 * process, where the right answer is known, then the flow-line model
 * (model.c) the way valve.c runs it: settled in AUTO by ctrl.c, MANUAL,
 * then the step test, then AUTO again with the suggested gains. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ctrl.h"
#include "model.h"
#include "tune.h"

static int fails;
#define CHECK(c, ...) do { bool ok_ = (c); printf("%s ", ok_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)
static bool near(double a, double b, double frac) { return fabs(a - b) <= frac * fabs(b); }

static tune_t T;

/* ---- synthetic process: y = y0 + K u through dead time and a lag ------- */
// slack: drive backlash, steps; the needle (pe) only follows once it is taken up.
typedef struct { double k, theta, tau, y, noise; int32_t pos, slack; double pe, hist[4000]; int t; } fopdt_sim_t;
static fopdt_sim_t S;
static double rnd(void) { static uint32_t a = 1; a = a * 1664525u + 1013904223u; return (double)(a >> 8) / 16777216.0 - 0.5; }
static double sim_pv(int32_t pos_max)
{
    if (S.pos > S.pe + S.slack / 2.0) S.pe = S.pos - S.slack / 2.0;
    if (S.pos < S.pe - S.slack / 2.0) S.pe = S.pos + S.slack / 2.0;
    double u = 100.0 * S.pe / pos_max;
    S.hist[S.t % 4000] = u;
    int d = (int)S.theta;
    double ud = S.t >= d ? S.hist[(S.t - d) % 4000] : S.hist[0];
    S.y += (S.k * ud - S.y) / S.tau;
    S.t++;
    return 5.0 + S.y + S.noise * rnd();
}
// Runs the test on the synthetic process; k2 != 0 changes the gain after
// the first fit (something else moves the pressure).
static int32_t synth_slack;
static void run_synth(double k, double theta, double tau, double noise, float step, double k2)
{
    const int32_t pmax = 66048;
    memset(&S, 0, sizeof S);
    S.k = k; S.theta = theta; S.tau = tau; S.noise = noise; S.pos = 20000; S.pe = S.pos; S.slack = synth_slack;
    for (int i = 0; i < 4000; i++) S.hist[i] = 100.0 * S.pos / pmax;
    S.y = k * 100.0 * S.pos / pmax;
    const char *why = tune_start(&T, S.pos, pmax, step, (float)sim_pv(pmax));
    if (why) { printf("     refused: %s\n", why); return; }
    for (int s = 0; s < 8000 && tune_running(&T); s++) {
        float pv = (float)sim_pv(pmax);
        if (k2 != 0 && T.phase == TUNE_AGAIN) S.k = k2;
        tune_step(&T, pv, S.pos, false);
        if (T.move) { S.pos = T.move_to; T.move = false; }
    }
}

/* ---- the model, the way valve.c runs it -------------------------------- */
static ctrl_tune_t K = { 4.0f, 0.25f, 8, 1, 0.1f, 0.0f, 99072 };
#define FS 1034.0f
static ctrl_t C;
static bool autom;
static uint32_t ms(void) { return (uint32_t)llround(M.t * 1000); }

static void model_read(void)
{
    ctrl_track(&C, M.pos, true);
    if (autom) {
        ctrl_in_t in = { ms(), (float)M.pv, M.pos, M.moving, M.lim_active, FS };
        if (ctrl_step(&C, &K, &in) == CTRL_RUN && C.move &&
            model_start_move(C.move_to))
            ctrl_sent(&C);
        return;
    }
    ctrl_idle(&C);
    if (tune_running(&T))
        tune_step(&T, (float)M.pv, M.pos, M.moving);
    if (T.move) {   // valve.c: stepper3_goto at MOT RATE
        model_start_move(T.move_to);
        T.move = false;
    }
}
static void run_s(double s) { long n = lround(s / MODEL_DT); for (long i = 0; i < n; i++) if (model_tick()) model_read(); }
static int run_tune(void) { int s = 0; while ((tune_running(&T) || M.moving) && s < 6000) { run_s(1); s++; } return s; }

// Settled at sp in AUTO with the defaults, then MANUAL.
static void settle(float sp, int32_t pos_max)
{
    K = (ctrl_tune_t){ 4.0f, 0.25f, 8, 1, 0.1f, 0.0f, pos_max };
    model_init();
    ctrl_init(&C);
    tune_init(&T);
    autom = false;
    run_s(120);
    ctrl_set_sp(&C, sp, FS, ms());
    ctrl_start(&C, &K, ms(), (float)M.pv, M.pos);
    autom = true;
    run_s(1500);
    autom = false;
}

// AUTO with kp/ti from the settled point: seconds to the band after a
// 0.5 mbar target step, and the largest error after that.
static void after(float kp, float ti, float sp, double *t_band, double *worst)
{
    K.kp = kp; K.ti = ti;
    ctrl_start(&C, &K, ms(), (float)M.pv, M.pos);
    ctrl_set_sp(&C, sp, FS, ms());
    autom = true;
    double t0 = M.t;
    *t_band = -1; *worst = 0;
    for (long i = 0; i < 60000; i++) {
        if (!model_tick()) continue;
        model_read();
        double e = fabs(M.pv - sp);
        if (*t_band < 0 && e <= K.band) *t_band = M.t - t0;
        else if (*t_band >= 0 && M.t - t0 < 400 && e > *worst) *worst = e;
    }
    autom = false;
}

static void print_fit(void)
{
    printf("     first: K %.4f, theta %.1f s, tau %.1f s; second: K %.4f, theta %.1f s, tau %.1f s; KP %.2f TI %.2f%s%s\n",
           T.fit[0].k, T.fit[0].theta, T.fit[0].tau, T.fit[1].k, T.fit[1].theta, T.fit[1].tau, T.kp, T.ti,
           T.phase == TUNE_FAILED ? "; failed: " : "", T.phase == TUNE_FAILED ? T.why : "");
}

int main(void)
{
    printf("synthetic first order plus dead time\n");
    run_synth(0.05, 10, 30, 0.02, 1.0f, 0);
    print_fit();
    double kk = (T.fit[0].k + T.fit[1].k) / 2, tt = (T.fit[0].tau + T.fit[1].tau) / 2, th = (T.fit[0].theta + T.fit[1].theta) / 2;
    CHECK(T.phase == TUNE_DONE && near(kk, 0.05, 0.1) && near(tt, 30, 0.2) && fabs(th - 10) <= 4,
          "K 0.05 mbar/%%, theta 10 s, tau 30 s found (escalated to a %.0f %% step)", fabs(100.0 * T.step / 66048));
    CHECK(near(T.kp, 30 / (0.05 * 40), 0.25) && near(T.ti, 0.5, 0.2), "KP near 15 %%/mbar, TI near 0.5 min (lambda = tau)");
    CHECK(S.pos == T.pos0, "valve back where it started");

    // Backlash of 3 % of travel (the bench, 26 Oct: 2 % back gave nothing).
    synth_slack = 1981;
    run_synth(0.05, 10, 30, 0.02, 1.0f, 0);
    print_fit();
    kk = (T.fit[0].k + T.fit[1].k) / 2;
    CHECK(T.phase == TUNE_DONE && near(kk, 0.05, 0.1), "3 %% backlash: K %.3f found, both steps the same way", kk);
    run_synth(0.05, 10, 30, 0.02, -1.0f, 0);
    kk = (T.fit[0].k + T.fit[1].k) / 2;
    CHECK(T.phase == TUNE_DONE && near(kk, 0.05, 0.1), "3 %% backlash, closing steps: K %.3f found", kk);
    synth_slack = 0;

    run_synth(-0.3, 5, 20, 0.01, 1.0f, 0);
    CHECK(T.phase == TUNE_DONE && T.fit[0].k < 0 && T.fit[1].k < 0, "opening lowers the pressure: K negative (KDIR -1)");

    run_synth(0.05, 10, 30, 0.02, 1.0f, 0.02);
    print_fit();
    CHECK(T.phase == TUNE_FAILED && (strstr(T.why, "disagree") || strstr(T.why, "opposite")) && S.pos == T.pos0,
          "gain changes during the test: refused, valve back");

    run_synth(0.001, 10, 30, 0.02, 1.0f, 0);
    CHECK(T.phase == TUNE_FAILED && strstr(T.why, "only") && S.pos == T.pos0, "no usable response at 16 %%: %s", T.why);

    run_synth(0.5, 10, 30, 0.02, 10.0f, 0);
    CHECK(T.phase == TUNE_FAILED && strstr(T.why, "smaller step") && S.pos == T.pos0, "5 mbar change: %s", T.why);

    // A pressure that never settles: only the slack take-up moves (away
    // and back), then failed after the wait.
    tune_start(&T, 5000, 66048, 1.0f, 5);
    int moves = 0;
    int32_t last = 5000;
    for (int s = 0; s < 700 && (tune_running(&T) || T.move); s++) {
        if (T.move) { moves++; last = T.move_to; T.move = false; }
        tune_step(&T, 5.0f + 0.1f * sinf(s / 10.0f), last, false);
    }
    CHECK(T.phase == TUNE_FAILED && moves == 2 && last == 5000 && T.waited == TUNE_WAIT_MAX_S,
          "unsteady pressure: %s; only the take-up moved, back at the start", T.why);

    CHECK(tune_start(&T, 60000, 66048, 10.0f, 5) && tune_start(&T, 1000, 66048, -2.0f, 5) &&
          tune_start(&T, 1000, 66048, 30.0f, 5), "steps past 100 %%, past 0 %% or over 20 %% refused");

    printf("flow-line model\n");
    double tb, worst, tb0, worst0;
    float ops[] = { 3, 5, 7 };
    for (int i = 0; i < 3; i++) {
        settle(ops[i], 99072);
        int32_t p0 = M.pos;
        tune_start(&T, M.pos, K.pos_max, 1.0f, (float)M.pv);
        int s = run_tune();
        print_fit();
        CHECK(T.phase == TUNE_DONE && T.kp > 2 && T.kp < 5 && T.ti > 0.2f && T.ti < 0.6f && M.pos == p0,
              "%.0f mbar: done in %d s, KP %.2f TI %.2f (defaults 4.00 / 0.25), valve back", ops[i], s, T.kp, T.ti);
    }
    // Eight times the gain: the defaults swing, the suggestion holds.
    settle(5, 8 * 99072);
    after(4.0f, 0.25f, 5.5f, &tb0, &worst0);
    settle(5, 8 * 99072);
    tune_start(&T, M.pos, K.pos_max, 1.0f, (float)M.pv);
    run_tune();
    print_fit();
    // The defaults leave this one swinging; the test then waits for steady.
    CHECK(T.phase == TUNE_DONE, "gain x 8: done, KP %.2f TI %.2f", T.kp, T.ti);
    after(T.kp, T.ti, 5.5f, &tb, &worst);
    CHECK(worst < 0.2 && worst0 > 0.3, "gain x 8, 5 to 5.5 mbar: suggestion in band %.0f s, then within %.2f mbar; defaults %.2f",
          tb, worst, worst0);

    printf("%s, %d failed\n", fails ? "FAILED" : "all passed", fails);
    return fails != 0;
}
