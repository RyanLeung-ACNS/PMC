#ifndef _LIMIT_H_
#define _LIMIT_H_

#include <stdbool.h>

/**
 * @brief Valve limit switch input (pin and polarity in pmc_config.h).
 * Debounced in the main loop; each change is sent on the link as
 * "LIM closed" or "LIM OPEN". Display only: it does not stop the motor.
 *   LIM?   state, raw pin level and number of changes since start-up
 */
typedef enum {
    LIMIT_UNKNOWN,   // not yet read steadily (first 20 ms)
    LIMIT_CLOSED,    // contact closed: healthy, valve away from the limit
    LIMIT_OPEN       // contact open: at the limit, or a broken wire
} limit_state_t;

void limit_init(void);
void limit_poll(void);           // main loop; samples every 5 ms
limit_state_t limit_state(void);
const char *limit_text(void);    // "closed", "OPEN" or "unknown"
bool limit_command(const char *line);

#endif // _LIMIT_H_
