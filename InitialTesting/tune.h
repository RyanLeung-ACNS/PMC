#ifndef _TUNE_H_
#define _TUNE_H_

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Step test for PI tuning (TUNE STEP, 0.12.7). Plain C with no
 * hardware calls, so tools/ctrl-test runs it on the PC against the
 * flow-line model; valve.c owns the motor, the mode and the faults and
 * calls tune_step() once a second, after the pressure read, in MANUAL.
 *
 *   1. Take up the drive's slack: move TUNE_PRELOAD_PCT the other way and
 *      back, so every step after this turns the drive the same way.
 *      Then wait until the pressure is steady (TUNE_STEADY_MBAR over
 *      TUNE_STEADY_S) and take that window's mean as the baseline.
 *   2. Move the valve by the step and record each second for
 *      TUNE_SETTLE_MIN_S, and on while the mean of the last
 *      TUNE_SETTLE_CMP_S s still differs from the mean of the
 *      TUNE_SETTLE_CMP_S s before them. If it moved less than TUNE_DYMIN_MBAR, step again
 *      by twice as much from there, up to TUNE_STEP_MAX_PCT.
 *   3. Fit first order plus dead time to that step by the two-point
 *      method: tau = 1.5 (t63 - t28), theta = t63 - tau, K = dp / dvalve.
 *   4. Step again by the same amount, the same way, and fit again (0.12.10:
 *      a step back reversed the drive, and its backlash, 2 % of travel or
 *      more on the bench, swallowed the step; Ryan 26 Oct). If the two fits
 *      disagree (K by more than TUNE_AGREE_K, tau by more than
 *      TUNE_AGREE_TAU), something else moved the pressure: no result.
 *   5. Suggest PI gains by the Lambda rule with lambda = tau:
 *      KP = tau / (K (tau + theta)), TI = tau. Nothing is applied.
 *
 * The test fails if the pressure leaves a step's baseline by more than
 * TUNE_WINDOW_MBAR, or a step does not settle in TUNE_SETTLE_MAX_S.
 * Background: notes/autotune-exploration in the project files.
 */

#define TUNE_STEP_DEF_PCT  1.0f    // first step, % of travel
#define TUNE_STEP_MAX_PCT  20.0f   // largest step after doubling, %
#define TUNE_PRELOAD_PCT   5.0f    // slack take-up move, % of travel
#define TUNE_STEADY_S      60      // steady window, s
#define TUNE_STEADY_MBAR   0.05f   // largest swing in that window, mbar
#define TUNE_WAIT_MAX_S    600     // longest wait for a steady pressure, s
#define TUNE_DYMIN_MBAR    0.20f   // smallest pressure change that is fitted, mbar
#define TUNE_WINDOW_MBAR   3.0f    // largest change in one step, from its baseline, mbar
#define TUNE_SETTLE_MIN_S  60      // shortest record of one step, s (Ryan, 26 Oct: was 150)
#define TUNE_SETTLE_CMP_S  15      // settled: the last this many s match the ones before
#define TUNE_SETTLE_MAX_S  600     // longest record of one step, s
#define TUNE_AGREE_K       0.25f   // the two fits' K may differ by this fraction
#define TUNE_AGREE_TAU     0.50f   // and tau by this

typedef enum {
    TUNE_IDLE,         // never run, or the result has been read
    TUNE_WAIT,         // taking up the slack, then waiting for a steady pressure
    TUNE_OUT,          // first step recorded
    TUNE_AGAIN,        // second step, the same way, recorded
    TUNE_DONE,         // result ready
    TUNE_FAILED        // stopped, why says what
} tune_phase_t;

typedef struct {
    float k;           // process gain, mbar per % of travel (signed)
    float theta;       // dead time, s
    float tau;         // time constant, s
} tune_fit_t;

typedef struct {
    tune_phase_t phase;
    int32_t pos_max;   // steps for 100 %
    int32_t pos0;      // where the test started, steps
    int32_t from;      // position before the step being recorded
    int32_t step;      // this step, steps (signed)
    float p_start;     // pressure at the start, mbar
    float y0;          // baseline of the step being recorded, mbar
    int n;             // readings in rec[]
    int16_t rec[TUNE_SETTLE_MAX_S];   // readings, 0.001 mbar from y0
    int waited;        // seconds in TUNE_WAIT
    float win[TUNE_STEADY_S];
    int win_n;
    int pre;           // slack take-up: 0 going away, 1 coming back, 2 done
    tune_fit_t fit[2]; // first step, second step
    float kp, ti;      // suggestion, %/mbar and min
    bool move;         // tune_step asks for a move to move_to
    int32_t move_to;
    char why[72];      // TUNE_FAILED: the reason
} tune_t;

void tune_init(tune_t *t);
// Starts a test from pos with a first step of step_pct (sign: + opens).
// NULL, or the reason it can't.
const char *tune_start(tune_t *t, int32_t pos, int32_t pos_max, float step_pct, float pv);
// Once a second, with a valid pressure; pos and moving from the motor.
void tune_step(tune_t *t, float pv, int32_t pos, bool moving);
void tune_fail(tune_t *t, const char *why);
bool tune_running(const tune_t *t);
// Seconds into the step being recorded (0 while waiting).
int tune_elapsed(const tune_t *t);

#endif // _TUNE_H_
