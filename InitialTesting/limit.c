#include <string.h>

#include "mcu.h"
#include "lvgl.h"   // lv_tick_get
#include "link.h"
#include "limit.h"
#include "pmc_config.h"

/*
 * Valve limit switch: one dry NC contact between the PMC_LIMIT_INPUT pin and GND,
 * read with the MCU's internal pull-up. Contact closed pulls the pin low
 * (healthy, valve away from the limit); open (at the limit, or a broken
 * wire) lets it float high. PMC_LIMIT_OPEN_LEVEL flips this for an
 * inverting interface such as an Opto 2 Click.
 *
 * Display only: nothing here stops the stepper (LIMIT_FITTED in stepper3.c
 * stays 0).
 */
#if PMC_LIMIT_INPUT == 1
#define PMC_LIMIT_PORT      GPIOG
#define PMC_LIMIT_PIN       1
#define PMC_LIMIT_PIN_NAME  "PG1 (S2 INT)"
#elif PMC_LIMIT_INPUT == 2
#define PMC_LIMIT_PORT      GPIOD
#define PMC_LIMIT_PIN       13
#define PMC_LIMIT_PIN_NAME  "PD13 (S2 PWM)"
#else
#error "PMC_LIMIT_INPUT must be 1 (PG1) or 2 (PD13)"
#endif

#if PMC_STEPPER3_SOCKET == 2
#error "The limit input is on S2, which also holds the Stepper 3 Click"
#endif

#define SAMPLE_MS   5
#define STABLE_N    4          // 20 ms of equal samples before a change counts

static limit_state_t state = LIMIT_UNKNOWN;
static bool last_raw;
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
    last_raw = read_open();
    same = 0;
    last_sample = lv_tick_get();
}

void limit_poll(void)
{
    if (lv_tick_get() - last_sample < SAMPLE_MS)
        return;
    last_sample = lv_tick_get();

    bool raw = read_open();
    if (raw != last_raw) {
        last_raw = raw;
        same = 0;
        return;
    }
    if (same < STABLE_N)
        same++;
    if (same < STABLE_N)
        return;

    limit_state_t s = raw ? LIMIT_OPEN : LIMIT_CLOSED;
    if (s != state) {
        if (state != LIMIT_UNKNOWN)
            changes++;
        state = s;
        link_printf("LIM %s\r\n", limit_text());
    }
}

limit_state_t limit_state(void) { return state; }

const char *limit_text(void)
{
    switch (state) {
    case LIMIT_CLOSED: return "closed";
    case LIMIT_OPEN:   return "OPEN";
    default:           return "unknown";
    }
}

bool limit_command(const char *line)
{
    if (strcmp(line, "LIM?") != 0)
        return false;
    bool high = (PMC_LIMIT_PORT->IDR >> PMC_LIMIT_PIN) & 1u;
    link_printf("LIM %s, pin %s %s, changes %lu, display only (not used to stop the motor)\r\nOK\r\n",
                limit_text(), PMC_LIMIT_PIN_NAME, high ? "high" : "low",
                (unsigned long)changes);
    return true;
}
