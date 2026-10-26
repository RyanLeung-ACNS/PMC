#ifndef _BUZZER_H_
#define _BUZZER_H_

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief On-board piezo buzzer, PB8, driven by TIM10 CH1 PWM (AF3) through
 * the board's pre-biased transistor. Pitch is the PWM frequency, loudness
 * the duty cycle (Mikromedia 4 manual, 6.1).
 *
 * From 0.12.14 the events play them (buzzer_event): a new fault, a new
 * warning, a refusal on the screen, AUTO started or ended by the operator,
 * homing done. The volume and which sounds are on are settings, live and
 * kept by CFG SAVE (cfg.c).
 *
 *   BUZZ?                  sounds, which are on, volume, what is playing
 *   BUZZ <name>            play one (even if it is off)
 *   BUZZ ALL               play every sound once, in turn
 *   BUZZ STOP              silence
 *   BUZZ VOL <0-100>       loudness, 0 mutes everything
 *   BUZZ ON|OFF <name>     which sounds buzzer_event() lets through
 *   BUZZ TONE <Hz> [ms]    plain tone, 200-8000 Hz, default 300 ms
 */
typedef enum {
    BUZZ_CLICK,       // key press
    BUZZ_OK,          // command accepted
    BUZZ_REFUSED,     // command refused
    BUZZ_WARN,        // new warning W-x
    BUZZ_FAULT,       // new fault F-x, plays once (no repeat: Ryan, 26 Oct)
    BUZZ_AUTO_ON,     // AUTO started
    BUZZ_AUTO_OFF,    // AUTO stopped by the operator
    BUZZ_HOMED,       // valve homing finished
    BUZZ_COUNT
} buzz_t;

// Defaults: every sound on but CLICK and OK, volume 50.
#define BUZZ_DEF_VOL   50
#define BUZZ_DEF_MASK  ((1u << BUZZ_COUNT) - 1 - (1u << BUZZ_CLICK) - (1u << BUZZ_OK))
#define BUZZ_MASK_ALL  ((1u << BUZZ_COUNT) - 1)

void buzzer_init(void);
void buzzer_tick_1ms(void);          // from the 1 ms SysTick interrupt
void buzzer_event(buzz_t s);         // plays if that sound is on and outranks what is playing
void buzzer_play(buzz_t s);          // plays now, on or off
void buzzer_silence(void);
bool buzzer_busy(void);
bool buzzer_command(const char *line);
// Settings (cfg.c saves them): volume 0 to 100, and bit n on lets sound n play.
uint32_t buzzer_vol(void);
bool buzzer_set_vol(uint32_t v);
uint32_t buzzer_mask(void);
bool buzzer_set_mask(uint32_t m);

#endif // _BUZZER_H_
