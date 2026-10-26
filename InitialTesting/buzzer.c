#include <stdlib.h>
#include <string.h>

#include "mcu.h"
#include "link.h"
#include "buzzer.h"

/* --------------------------------------------------------------------------
 * Hardware
 *
 * Piezo buzzer on PB8 through a pre-biased transistor (high = sounding).
 * PB8 is not a mikroBUS socket pin and not an Ethernet RMII pin. TIM10
 * (APB2, one channel) drives it as CH1 on AF3; nothing else uses TIM10.
 * TIM4 CH3 is the other choice for PB8, but TIM4 CH2 is also on PD13 (the
 * limit switch) and CH3/CH4 on the RGB LED, so TIM10 keeps clear of them.
 *
 * The counter runs at about 4 MHz: ARR = 500 at 8 kHz, 20000 at 200 Hz.
 * Its input clock is read back from RCC at each sound, because the
 * Ethernet build changes SYSCLK (168 -> 150 MHz) after start-up.
 *
 * Loudness: duty = 50 % x (VOL / 100)^2, so VOL 50 is 12.5 % duty. A piezo
 * is loudest near its resonance (part not known, often 2.7 to 4 kHz): BUZZ
 * TONE finds it on the bench.
 *
 * Timing: buzzer_tick_1ms() runs from the 1 ms SysTick interrupt, so note
 * lengths do not stretch while the screen redraws. The main loop only
 * leaves a request (req) for the tick to pick up.
 * ------------------------------------------------------------------------ */

#define BUZ_PIN       8          // PB8
#define BUZ_AF        3          // TIM10_CH1
#define CNT_HZ        4000000UL
#define TONE_MIN_HZ   200
#define TONE_MAX_HZ   8000
#define TONE_MAX_MS   5000
#define DEMO_GAP_MS   700

#define OC1M_FORCE_LOW  (4UL << TIM_CCMR1_OC1M_Pos)
#define OC1M_PWM1       (6UL << TIM_CCMR1_OC1M_Pos)

typedef struct {
    uint16_t hz;    // 0 = rest
    uint16_t ms;    // 0 ends the sound
} step_t;

typedef struct {
    const char *name;
    const char *what;
    const step_t *steps;
    uint8_t rank;      // buzzer_event() does not cut off a higher rank
    bool on;           // default for buzzer_event(): abnormal events only
} sound_t;

/* --------------------------------------------------------------------------
 * Sounds: the "industrial" set Ryan picked on 26 Oct from the browser player
 * (8-bit and RPG-menu shapes kept plain). Fixed pitches in the 2 to 5 kHz
 * range. A short chirp a minor third above starts some notes. Nothing
 * repeats: FAULT plays once.
 * ------------------------------------------------------------------------ */
static const step_t s_click[]    = { { 3136, 8 }, { 0, 0 } };
static const step_t s_ok[]       = { { 2093, 40 }, { 0, 25 }, { 3136, 70 }, { 0, 0 } };
static const step_t s_refused[]  = { { 1568, 60 }, { 1480, 120 }, { 0, 0 } };   // G down to F sharp
static const step_t s_warn[]     = { { 3136, 15 }, { 2637, 135 }, { 0, 100 },
                                     { 3136, 15 }, { 2637, 135 }, { 0, 100 },
                                     { 3136, 15 }, { 2637, 135 }, { 0, 0 } };
static const step_t s_fault[]    = { { 3520, 15 }, { 3136, 95 }, { 0, 60 },
                                     { 3520, 15 }, { 3136, 95 }, { 0, 60 },
                                     { 3520, 15 }, { 3136, 95 }, { 0, 250 },
                                     { 3520, 15 }, { 3136, 95 }, { 0, 60 },
                                     { 3520, 15 }, { 3136, 95 }, { 0, 60 },
                                     { 3520, 15 }, { 3136, 95 }, { 0, 0 } };
static const step_t s_auto_on[]  = { { 2491, 12 }, { 2093, 38 }, { 0, 30 },
                                     { 3732, 12 }, { 3136, 38 }, { 0, 30 },
                                     { 4981, 12 }, { 4186, 108 }, { 0, 0 } };
static const step_t s_auto_off[] = { { 4981, 12 }, { 4186, 38 }, { 0, 30 },
                                     { 3732, 12 }, { 3136, 38 }, { 0, 30 },
                                     { 2491, 12 }, { 2093, 108 }, { 0, 0 } };
static const step_t s_homed[]    = { { 4186, 80 }, { 0, 60 }, { 4186, 80 }, { 0, 0 } };

static sound_t sounds[BUZZ_COUNT] = {
    [BUZZ_CLICK]    = { "CLICK",   "key press, 8 ms tick",                         s_click,    0, false },
    [BUZZ_OK]       = { "OK",      "accepted, two steps up",                       s_ok,       1, false },
    [BUZZ_REFUSED]  = { "REFUSED", "refused, G down to F sharp",                   s_refused,  2, true  },
    [BUZZ_WARN]     = { "WARN",    "new warning W-x, three chirped pulses",        s_warn,     3, true  },
    [BUZZ_FAULT]    = { "FAULT",   "new fault F-x, two sets of three higher pulses", s_fault,  4, true  },
    [BUZZ_AUTO_ON]  = { "AUTOON",  "AUTO started, beep boop ding up",              s_auto_on,  1, false },
    [BUZZ_AUTO_OFF] = { "AUTOOFF", "AUTO stopped, ding boop beep down",            s_auto_off, 1, false },
    [BUZZ_HOMED]    = { "HOMED",   "valve homing finished, two short high pulses", s_homed,    1, false },
};

static const step_t demo_gap[] = { { 0, DEMO_GAP_MS }, { 0, 0 } };

/* --------------------------------------------------------------------------
 * State. req is written by the main loop and taken by the tick; the rest
 * belongs to the tick.
 * ------------------------------------------------------------------------ */
#define REQ_NONE   0
#define REQ_SOUND  1             // + buzz_t
#define REQ_TONE   (REQ_SOUND + BUZZ_COUNT)
#define REQ_ALL    (REQ_TONE + 1)
#define REQ_STOP   (REQ_TONE + 2)
#define CUR_IDLE   (-1)
#define CUR_TONE   BUZZ_COUNT

static volatile uint8_t req;
static volatile uint16_t req_hz, req_ms;
static volatile uint8_t vol = 50;
static volatile int8_t cur = CUR_IDLE;
static volatile int8_t demo = -1;      // sound BUZZ ALL is on, -1 when not

static const step_t *seq;
static uint8_t seq_i;
static uint16_t left_ms;
static step_t tone_steps[2];
static uint16_t psc;
static uint32_t cnt_hz = CNT_HZ;

static uint32_t apb2_timer_hz(void)
{
    static const uint16_t ahb_div[16] = { 1, 1, 1, 1, 1, 1, 1, 1, 2, 4, 8, 16, 64, 128, 256, 512 };
    static const uint8_t apb_div[8] = { 1, 1, 1, 1, 2, 4, 8, 16 };
    uint32_t cfgr = RCC->CFGR;
    uint32_t sys;

    switch (cfgr & RCC_CFGR_SWS) {
    case RCC_CFGR_SWS_HSI:
        sys = 16000000UL;
        break;
    case RCC_CFGR_SWS_HSE:
        sys = 25000000UL;          // board crystal
        break;
    default: {
        uint32_t pll = RCC->PLLCFGR;
        uint64_t in = (pll & RCC_PLLCFGR_PLLSRC) ? 25000000ULL : 16000000ULL;
        uint32_t m = (pll & RCC_PLLCFGR_PLLM_Msk) >> RCC_PLLCFGR_PLLM_Pos;
        uint32_t n = (pll & RCC_PLLCFGR_PLLN_Msk) >> RCC_PLLCFGR_PLLN_Pos;
        uint32_t p = (((pll & RCC_PLLCFGR_PLLP_Msk) >> RCC_PLLCFGR_PLLP_Pos) + 1) * 2;
        sys = (uint32_t)(in * n / m / p);
        break;
    }
    }
    uint32_t hclk = sys / ahb_div[(cfgr & RCC_CFGR_HPRE_Msk) >> RCC_CFGR_HPRE_Pos];
    uint32_t d2 = apb_div[(cfgr & RCC_CFGR_PPRE2_Msk) >> RCC_CFGR_PPRE2_Pos];
    // Timers on APB2 run at twice PCLK2 when the APB2 divider is not 1.
    return d2 == 1 ? hclk : hclk / d2 * 2;
}

static void read_clock(void)
{
    uint32_t t = apb2_timer_hz();
    uint32_t div = (t + CNT_HZ / 2) / CNT_HZ;
    if (div < 1)
        div = 1;
    psc = (uint16_t)(div - 1);
    cnt_hz = t / div;
}

static void set_tone(uint16_t hz)
{
    uint32_t v = vol;
    if (hz == 0 || v == 0) {
        TIM10->CCMR1 = OC1M_FORCE_LOW;     // pin low at once, transistor off
        TIM10->CR1 = 0;
        return;
    }
    uint32_t arr = cnt_hz / hz;
    uint32_t ccr = arr * v * v / 20000;
    if (ccr == 0)
        ccr = 1;
    TIM10->CR1 = 0;
    TIM10->PSC = psc;
    TIM10->ARR = arr - 1;
    TIM10->CCR1 = ccr;
    TIM10->CCMR1 = OC1M_PWM1 | TIM_CCMR1_OC1PE;
    TIM10->EGR = TIM_EGR_UG;               // load PSC, ARR and CCR1 now
    TIM10->CR1 = TIM_CR1_ARPE | TIM_CR1_CEN;
}

static void stop_all(void)
{
    set_tone(0);
    seq = NULL;
    cur = CUR_IDLE;
    demo = -1;
}

static void end_of_seq(void);

static void play_step(void)
{
    const step_t *st = &seq[seq_i];
    if (st->ms == 0) {
        end_of_seq();
        return;
    }
    set_tone(st->hz);
    left_ms = st->ms;
}

static void start_seq(const step_t *steps)
{
    seq = steps;
    seq_i = 0;
    play_step();
}

static void start_sound(int8_t i)
{
    cur = i;
    start_seq(sounds[i].steps);
}

static void end_of_seq(void)
{
    if (demo >= 0) {
        if (seq == demo_gap) {
            start_sound(demo);
            return;
        }
        if (++demo < BUZZ_COUNT) {
            cur = demo;
            start_seq(demo_gap);
            return;
        }
    }
    stop_all();
}

static void take(uint8_t r)
{
    read_clock();
    if (r == REQ_STOP) {
        stop_all();
    } else if (r == REQ_ALL) {
        demo = 0;
        start_sound(0);
    } else if (r == REQ_TONE) {
        demo = -1;
        cur = CUR_TONE;
        tone_steps[0].hz = req_hz;
        tone_steps[0].ms = req_ms;
        tone_steps[1].hz = 0;
        tone_steps[1].ms = 0;
        start_seq(tone_steps);
    } else if (r >= REQ_SOUND && r < REQ_SOUND + BUZZ_COUNT) {
        demo = -1;
        start_sound((int8_t)(r - REQ_SOUND));
    }
}

void buzzer_tick_1ms(void)
{
    uint8_t r = req;
    if (r != REQ_NONE) {
        req = REQ_NONE;
        take(r);
    }
    if (seq && left_ms && --left_ms == 0) {
        seq_i++;
        play_step();
    }
}

/* --------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------ */
void buzzer_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    RCC->APB2ENR |= RCC_APB2ENR_TIM10EN;
    (void)RCC->APB2ENR;
    TIM10->CR1 = 0;
    TIM10->CCER = 0;
    TIM10->CCMR1 = OC1M_FORCE_LOW;
    TIM10->CCER = TIM_CCER_CC1E;           // active high
    TIM10->CCR1 = 0;

    GPIOB->OTYPER &= ~(1UL << BUZ_PIN);
    GPIOB->PUPDR &= ~(3UL << (2 * BUZ_PIN));
    GPIOB->OSPEEDR &= ~(3UL << (2 * BUZ_PIN));
    GPIOB->AFR[BUZ_PIN >> 3] = (GPIOB->AFR[BUZ_PIN >> 3] & ~(0xFUL << (4 * (BUZ_PIN & 7)))) |
                               ((uint32_t)BUZ_AF << (4 * (BUZ_PIN & 7)));
    GPIOB->MODER = (GPIOB->MODER & ~(3UL << (2 * BUZ_PIN))) | (2UL << (2 * BUZ_PIN));
}

static uint8_t rank_of(int8_t c)
{
    return (c >= 0 && c < BUZZ_COUNT) ? sounds[c].rank : 0;
}

void buzzer_event(buzz_t s)
{
    if ((unsigned)s >= BUZZ_COUNT || !sounds[s].on || vol == 0)
        return;
    uint8_t r = req;
    if (r >= REQ_SOUND && r < REQ_SOUND + BUZZ_COUNT && sounds[r - REQ_SOUND].rank > sounds[s].rank)
        return;
    if (cur != CUR_IDLE && rank_of(cur) > sounds[s].rank)
        return;
    req = (uint8_t)(REQ_SOUND + s);
}

void buzzer_play(buzz_t s)
{
    if ((unsigned)s < BUZZ_COUNT)
        req = (uint8_t)(REQ_SOUND + s);
}

void buzzer_silence(void)
{
    req = REQ_STOP;
}

bool buzzer_busy(void)
{
    return cur != CUR_IDLE || req != REQ_NONE;
}

/* --------------------------------------------------------------------------
 * Commands
 * ------------------------------------------------------------------------ */
static int find_sound(const char *name)
{
    for (int i = 0; i < BUZZ_COUNT; i++)
        if (strcmp(name, sounds[i].name) == 0)
            return i;
    return -1;
}

static bool parse_long(const char *s, long *out)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || (*end != '\0' && *end != ' '))
        return false;
    *out = v;
    return true;
}

static void list_sounds(void)
{
    int8_t c = cur;
    const char *now = c == CUR_IDLE ? "nothing" : c == CUR_TONE ? "TONE" : sounds[c].name;
    link_printf("BUZZ vol %u%s, playing %s%s\r\n", vol, vol == 0 ? " (muted)" : "", now,
                demo >= 0 ? " (BUZZ ALL)" : "");
    for (int i = 0; i < BUZZ_COUNT; i++)
        link_printf("  %-8s %-3s  %s\r\n", sounds[i].name, sounds[i].on ? "on" : "off", sounds[i].what);
    link_printf("on/off: which sounds events may play (no events wired yet); not saved\r\n");
}

bool buzzer_command(const char *line)
{
    if (strcmp(line, "BUZZ?") == 0) {
        list_sounds();
        return true;
    }
    if (strncmp(line, "BUZZ ", 5) != 0)
        return false;
    const char *arg = line + 5;
    long v, ms;

    if (strcmp(arg, "STOP") == 0) {
        buzzer_silence();
        link_printf("OK\r\n");
    } else if (strcmp(arg, "ALL") == 0) {
        req = REQ_ALL;
        link_printf("OK %d sounds, %d ms apart, BUZZ STOP ends it\r\n", BUZZ_COUNT, DEMO_GAP_MS);
    } else if (strncmp(arg, "VOL ", 4) == 0) {
        if (!parse_long(arg + 4, &v) || v < 0 || v > 100) {
            link_printf("ERR expected: BUZZ VOL <0-100>\r\n");
        } else {
            vol = (uint8_t)v;
            link_printf("OK vol %ld, duty %ld.%ld %%, not saved\r\n", v, v * v / 200, (v * v / 20) % 10);
        }
    } else if (strncmp(arg, "ON ", 3) == 0 || strncmp(arg, "OFF ", 4) == 0) {
        bool on = arg[1] == 'N';
        const char *name = arg + (on ? 3 : 4);
        int i = find_sound(name);
        if (strcmp(name, "ALL") == 0) {
            for (i = 0; i < BUZZ_COUNT; i++)
                sounds[i].on = on;
            link_printf("OK all sounds %s, not saved\r\n", on ? "on" : "off");
        } else if (i < 0) {
            link_printf("ERR unknown sound, BUZZ? lists them\r\n");
        } else {
            sounds[i].on = on;
            link_printf("OK %s %s, not saved\r\n", sounds[i].name, on ? "on" : "off");
        }
    } else if (strncmp(arg, "TONE ", 5) == 0) {
        const char *p = arg + 5;
        ms = 300;
        bool good = parse_long(p, &v) && v >= TONE_MIN_HZ && v <= TONE_MAX_HZ;
        const char *sp = strchr(p, ' ');
        if (good && sp)
            good = parse_long(sp + 1, &ms) && ms >= 1 && ms <= TONE_MAX_MS;
        if (!good) {
            link_printf("ERR expected: BUZZ TONE <%d-%d Hz> [1-%d ms]\r\n", TONE_MIN_HZ, TONE_MAX_HZ, TONE_MAX_MS);
        } else {
            req_hz = (uint16_t)v;
            req_ms = (uint16_t)ms;
            req = REQ_TONE;
            link_printf("OK %ld Hz %ld ms at vol %u\r\n", v, ms, vol);
        }
    } else {
        int i = find_sound(arg);
        if (i < 0) {
            link_printf("ERR expected: BUZZ <sound>|ALL|STOP|VOL|ON|OFF|TONE, BUZZ? lists the sounds\r\n");
        } else {
            buzzer_play((buzz_t)i);
            link_printf("OK %s%s\r\n", sounds[i].name, vol == 0 ? " (vol 0: silent)" : "");
        }
    }
    return true;
}
