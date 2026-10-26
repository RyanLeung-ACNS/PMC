#include <string.h>

#include "mcu.h"
#include "lvgl.h"   // lv_tick_get
#include "link.h"
#include "limit.h"
#include "pmc_config.h"

/*
 * Valve limit switch: one dry NC contact between PD13 (S2 PWM) and GND,
 * read with the MCU's internal pull-up. Contact closed pulls the pin low
 * (healthy, valve away from the limit); open (at the limit, or a broken
 * wire) lets it float high. PMC_LIMIT_OPEN_LEVEL flips this for an
 * inverting interface such as an Opto 2 Click.
 *
 * Treated as the closing stop (fail-safe):
 *  - a trip counts at once: the first OPEN sample makes the state OPEN;
 *  - only the release is debounced: CLOSED again needs 20 ms of closed
 *    samples, so a bouncing or intermittent contact stays OPEN;
 *  - until the first steady read after start-up the state is unknown and
 *    treated as OPEN;
 *  - limit_open_now() also reads the pin directly, so the step interrupt
 *    checks the contact itself before every closing step, not only the
 *    main-loop state (which a long screen redraw can delay).
 * With PMC_LIMIT_STOP 1 stepper3.c refuses and stops closing moves while
 * this reports open; with 0 the same state is only displayed.
 */
#define PMC_LIMIT_PORT      GPIOD
#define PMC_LIMIT_PIN       13
#define PMC_LIMIT_PIN_NAME  "PD13 (S2 PWM)"

#define SAMPLE_MS   5
#define STABLE_N    4          // 20 ms of closed samples before a release counts

static volatile limit_state_t state = LIMIT_UNKNOWN;   // read by the step ISR
static uint8_t same;
static uint32_t changes;
static uint32_t last_sample;

static bool read_open(void)
{
    bool high = (PMC_LIMIT_PORT->IDR >> PMC_LIMIT_PIN) & 1u;
    return high == (PMC_LIMIT_OPEN_LEVEL != 0);
}

void limit_init(void)
{
    if (PMC_LIMIT_PORT == GPIOD)
        RCC->AHB1ENR |= RCC_AHB1ENR_GPIODEN;
    else if (PMC_LIMIT_PORT == GPIOG)
        RCC->AHB1ENR |= RCC_AHB1ENR_GPIOGEN;
    (void)RCC->AHB1ENR;
    // Input (MODER 00), pull-up (PUPDR 01) or none (00).
    PMC_LIMIT_PORT->MODER &= ~(3UL << (2 * PMC_LIMIT_PIN));
    PMC_LIMIT_PORT->PUPDR = (PMC_LIMIT_PORT->PUPDR & ~(3UL << (2 * PMC_LIMIT_PIN))) |
                            ((PMC_LIMIT_PULLUP ? 1UL : 0UL) << (2 * PMC_LIMIT_PIN));
    same = 0;
    last_sample = lv_tick_get();
}

void limit_poll(void)
{
    if (lv_tick_get() - last_sample < SAMPLE_MS)
        return;
    last_sample = lv_tick_get();

    limit_state_t s;
    if (read_open()) {
        same = 0;                      // trip at once, restart the release count
        s = LIMIT_OPEN;
    } else {
        if (same < STABLE_N)
            same++;
        if (same < STABLE_N)
            return;                    // release not yet steady
        s = LIMIT_CLOSED;
    }
    if (s != state) {
        if (state != LIMIT_UNKNOWN)
            changes++;
        state = s;
        link_printf("LIM %s\r\n", limit_text());
    }
}

limit_state_t limit_state(void) { return state; }

bool limit_open_now(void)
{
    return state != LIMIT_CLOSED || read_open();
}

const char *limit_text(void)
{
    switch (state) {
    case LIMIT_CLOSED: return "free";
    case LIMIT_OPEN:   return "pressed";
    default:           return "unknown";
    }
}

bool limit_command(const char *line)
{
    if (strcmp(line, "LIM?") != 0)
        return false;
    bool high = (PMC_LIMIT_PORT->IDR >> PMC_LIMIT_PIN) & 1u;
    link_printf("LIM %s, pin %s %s, changes %lu, %s\r\nOK\r\n",
                limit_text(), PMC_LIMIT_PIN_NAME, high ? "high" : "low",
                (unsigned long)changes,
                PMC_LIMIT_STOP ? "closing moves blocked while pressed" : "display only (PMC_LIMIT_STOP 0)");
    return true;
}
