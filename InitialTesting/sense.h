#ifndef _SENSE_H_
#define _SENSE_H_

#include <stdbool.h>

// Once a second from the main loop: read the boards, update the checks,
// print the status and DATA lines.
void sense_second(void);

// DATA ON / DATA OFF: start or pause the once-a-second DATA lines.
// Returns false if the line is not a DATA command.
bool sense_data_command(const char *line);

#endif // _SENSE_H_
