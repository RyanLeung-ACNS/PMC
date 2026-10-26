/* Host stand-in for buzzer.c (no TIM10 here): keeps the volume and the
 * on/off mask, so cfg.c saves and loads them, and counts the events. Used by
 * tools/cfg-test and tools/ui-preview. */
#include "buzzer.h"

static uint32_t vol = BUZZ_DEF_VOL, mask = BUZZ_DEF_MASK;
int buzzer_stub_events[BUZZ_COUNT];

void buzzer_init(void) {}
void buzzer_tick_1ms(void) {}
void buzzer_event(buzz_t s) { if ((unsigned)s < BUZZ_COUNT && (mask >> s & 1u) && vol) buzzer_stub_events[s]++; }
void buzzer_play(buzz_t s) { (void)s; }
void buzzer_silence(void) {}
bool buzzer_busy(void) { return false; }
bool buzzer_command(const char *line) { (void)line; return false; }
uint32_t buzzer_vol(void) { return vol; }
bool buzzer_set_vol(uint32_t v) { if (v > 100) return false; vol = v; return true; }
uint32_t buzzer_mask(void) { return mask; }
bool buzzer_set_mask(uint32_t m) { if (m & ~BUZZ_MASK_ALL) return false; mask = m; return true; }
