#include <string.h>

#include "mcu.h"
#include "lvgl.h"   // lv_tick_get
#include "link.h"
#include "supply.h"
#include "pmc_config.h"

/*
 * Motor supply on PA3 (S1 and S2 share it as RX; Stepper 3 in S1 does not
 * use it, and nothing else on the board does). ADC1 polled: one conversion
 * of IN3 and one of VREFINT (IN17) every 5 ms, 480 cycles each (the module's
 * source is about 6 kohm). VDDA comes from VREFINT and its factory value, so
 * the 3.3 V regulator's tolerance does not count.
 * Moving average of 8 (40 ms) for the reading; OFF also trips on two raw
 * samples in a row below the threshold (10 ms), so a supply lost during a
 * move stops it within a few steps.
 */
#define SAMPLE_MS   5
#define AVG_N       8
#define DIVIDER     5          // (30k + 7.5k) / 7.5k
#define VREFINT_CAL (*(const volatile uint16_t *)0x1FFF7A2AUL)   // at VDDA 3.3 V, 30 C

static int32_t buf[AVG_N];
static int n_buf, i_buf;
static int32_t avg_mv;
static supply_state_t state = PMC_MOTOR_SENSE ? SUPPLY_OFF : SUPPLY_OK;
static uint8_t low_raw;        // raw samples in a row below SUPPLY_OFF_MV
static uint32_t last_sample;
static uint16_t raw_in, raw_ref;
static int32_t vdda_mv = 3300;

bool supply_fitted(void) { return PMC_MOTOR_SENSE != 0; }

#if PMC_MOTOR_SENSE
static uint16_t convert(uint32_t ch)
{
    ADC1->SQR3 = ch;
    ADC1->SR = 0;
    ADC1->CR2 |= ADC_CR2_SWSTART;
    for (uint32_t n = 0; !(ADC1->SR & ADC_SR_EOC); n++)
        if (n > 100000)
            return 0;   // never seen; reads as off
    return (uint16_t)ADC1->DR;
}
#endif

void supply_init(void)
{
#if PMC_MOTOR_SENSE
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;
    (void)RCC->APB2ENR;
    GPIOA->MODER |= 3UL << (2 * 3);                   // PA3 analog
    GPIOA->PUPDR &= ~(3UL << (2 * 3));
    // PCLK2 / 4: 21 MHz at 84 MHz (USB build), 18.75 MHz at 75 MHz (Ethernet).
    ADC->CCR = (ADC->CCR & ~ADC_CCR_ADCPRE) | ADC_CCR_ADCPRE_0 | ADC_CCR_TSVREFE;
    ADC1->CR1 = 0;                                    // 12 bit, no scan
    ADC1->CR2 = 0;                                    // single, right aligned
    ADC1->SMPR2 = (ADC1->SMPR2 & ~ADC_SMPR2_SMP3) | (7UL << ADC_SMPR2_SMP3_Pos);
    ADC1->SMPR1 = (ADC1->SMPR1 & ~ADC_SMPR1_SMP17) | (7UL << ADC_SMPR1_SMP17_Pos);
    ADC1->SQR1 = 0;                                   // one conversion
    ADC1->CR2 |= ADC_CR2_ADON;
    for (volatile uint32_t d = 0; d < 1000; d++) {}   // tSTAB, a few us
#endif
    last_sample = lv_tick_get();
}

static supply_state_t judge(int32_t mv, supply_state_t was)
{
    // Hysteresis: leaving a state needs SUPPLY_HYST_MV past its threshold.
    int32_t h = SUPPLY_HYST_MV;
    if (mv < SUPPLY_OFF_MV) return SUPPLY_OFF;
    if (was == SUPPLY_OFF && mv < SUPPLY_OFF_MV + h) return SUPPLY_OFF;
    if (mv < SUPPLY_LOW_MV) return SUPPLY_LOW;
    if (was == SUPPLY_LOW && mv < SUPPLY_LOW_MV + h) return SUPPLY_LOW;
    if (mv > SUPPLY_HIGH_MV) return SUPPLY_HIGH;
    if (was == SUPPLY_HIGH && mv > SUPPLY_HIGH_MV - h) return SUPPLY_HIGH;
    return SUPPLY_OK;
}

void supply_poll(void)
{
#if PMC_MOTOR_SENSE
    if (lv_tick_get() - last_sample < SAMPLE_MS)
        return;
    last_sample = lv_tick_get();
    raw_ref = convert(17);
    raw_in = convert(3);
    if (raw_ref > 0)
        vdda_mv = (int32_t)(3300UL * VREFINT_CAL / raw_ref);
    int32_t mv = (int32_t)((int64_t)raw_in * vdda_mv * DIVIDER / 4095);

    buf[i_buf] = mv;
    i_buf = (i_buf + 1) % AVG_N;
    if (n_buf < AVG_N)
        n_buf++;
    int32_t sum = 0;
    for (int i = 0; i < n_buf; i++)
        sum += buf[i];
    avg_mv = sum / n_buf;

    low_raw = mv < SUPPLY_OFF_MV ? (uint8_t)(low_raw < 255 ? low_raw + 1 : 255) : 0;
    supply_state_t s = low_raw >= 2 ? SUPPLY_OFF : n_buf < AVG_N ? state : judge(avg_mv, state);
    state = s;   // a change is logged by valve.c (SUP events)
#endif
}

int32_t supply_mv(void) { return avg_mv; }
supply_state_t supply_state(void) { return state; }
bool supply_off(void) { return state == SUPPLY_OFF; }

#if PMC_ENG_COMMANDS
bool supply_command(const char *line)
{
    if (strcmp(line, "SUP?") != 0)
        return false;
    static const char *const name[4] = { "ok", "LOW", "HIGH", "OFF" };
    if (!PMC_MOTOR_SENSE)
        link_printf("SUP not fitted (PMC_MOTOR_SENSE 0)\r\n");
    else
        link_printf("SUP %ld mV %s, raw %u, VREFINT raw %u cal %u, VDDA %ld mV, off <%d low <%d high >%d mV\r\n",
                    (long)avg_mv, name[state], raw_in, raw_ref, VREFINT_CAL, (long)vdda_mv,
                    SUPPLY_OFF_MV, SUPPLY_LOW_MV, SUPPLY_HIGH_MV);
    link_printf("OK\r\n");
    return true;
}
#endif
