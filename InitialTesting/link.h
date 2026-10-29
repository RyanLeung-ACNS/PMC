#ifndef _LINK_H_
#define _LINK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pmc_config.h"

/**
 * @brief Text link to the PC: the USB COM port or Ethernet (TCP), chosen by
 * PMC_LINK_ETHERNET in pmc_config.h. Same DATA lines and CAL commands
 * either way.
 */

void link_init(void);

// Call often from the main loop.
void link_task(void);

/*
 * Output (0.13.0). The board never sends a line by itself: the remote
 * command set (remote.c) sends its reply lines with link_write. link_printf
 * is for the engineering commands of a bench build (PMC_ENG_COMMANDS 1) and
 * sends only while one of them runs (link_eng_open); in the normal build it
 * sends nothing, so status changes, events and saves from the screen never
 * reach the link (the event log keeps them, EVNT?).
 */
void link_write(const char *s);
void link_printf(const char *fmt, ...);
#if PMC_ENG_COMMANDS
void link_eng_open(bool open);
#endif

// "ERR <why>" with the first letter in upper case: the refusal texts are
// shared with the screen, and the link's ERR lines are in sentence case.
static inline void link_err(const char *why)
{
    if (!why || !why[0]) {
        link_printf("ERR\r\n");
        return;
    }
    char c = why[0] >= 'a' && why[0] <= 'z' ? (char)(why[0] - 'a' + 'A') : why[0];
    link_printf("ERR %c%s\r\n", c, why + 1);
}
// Bytes link_printf can send now without dropping or waiting. Bulk replies
// (TREND DUMP) send a line only when this is enough.
size_t link_tx_room(void);

// Non-blocking: true with one complete line (no CR/LF) in buf.
bool link_getline(char *buf, size_t n);


// Header chip: short text and whether the link is usable / in use.
const char *link_chip_text(void);
bool link_chip_ok(void);

// One-line state for the status lines.
const char *link_status(void);

/*
 * Line assembly, shared by the USB and Ethernet code. CR, LF or CRLF end a
 * line (terminals differ) and empty lines are ignored. A line of up to
 * LINK_LINE_MAX characters is taken; a longer one comes out as
 * LINK_LINE_LONG (ERR 103), so nothing cut short is run.
 */
#define LINK_LINE_MAX  96
#define LINK_LINE_LONG "\x7f"
typedef struct {
    char text[LINK_LINE_MAX + 1];
    size_t len;
    bool over;           // characters were dropped
    uint32_t last_ms;    // lv_tick_get() at the last character that was not CR/LF
} link_line_t;

// Adds one received character. True with the line (no CR/LF) in out when it
// ends one; out needs LINK_LINE_MAX + 1 bytes.
bool link_line_feed(link_line_t *l, char c, char *out, size_t n);
// Ends a pending line once nothing has arrived for idle_ms. True with the line in out.
bool link_line_idle(link_line_t *l, uint32_t idle_ms, char *out, size_t n);

#endif // _LINK_H_
