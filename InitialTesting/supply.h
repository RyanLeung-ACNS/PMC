#ifndef _SUPPLY_H_
#define _SUPPLY_H_

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Motor 12 V supply sense (PMC_MOTOR_SENSE in pmc_config.h): a
 * 0-25 V divider module (30k / 7.5k, x5) from the motor supply to S1 RX,
 * PA3 (ADC1 IN3), read every 5 ms against the internal reference.
 * The board can run from USB with the motor supply off; PG15 DC_DETECT
 * only sees the board's own 12 V terminal.
 *   OFF   below 6.0 V: moves are refused; one lost during a move stops it
 *         and clears the homing (the count ran on without the valve)
 *   LOW   below 10.5 V: a warning, moves allowed
 *   HIGH  above 14.0 V: a warning, moves allowed
 *   SUP?  supply volts, state, raw ADC counts and the ADC reference
 */
typedef enum { SUPPLY_OK, SUPPLY_LOW, SUPPLY_HIGH, SUPPLY_OFF } supply_state_t;

#define SUPPLY_OFF_MV   6000
#define SUPPLY_LOW_MV   10500
#define SUPPLY_HIGH_MV  14000
#define SUPPLY_HYST_MV  200

void supply_init(void);
void supply_poll(void);              // main loop; samples every 5 ms
int32_t supply_mv(void);             // averaged over 40 ms
supply_state_t supply_state(void);   // SUPPLY_OK when not fitted
bool supply_off(void);               // moves refused
bool supply_fitted(void);
bool supply_command(const char *line);

#endif // _SUPPLY_H_
