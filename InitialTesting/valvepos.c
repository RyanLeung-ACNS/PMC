#include <stddef.h>
#include <string.h>

#include "lvgl.h"        // lv_tick_get
#include "link.h"
#include "extflash.h"
#include "fixed.h"
#include "stepper3.h"
#include "valve.h"
#include "valvepos.h"

#define VP_BASE      0x7EC000UL
#define VP_SECTORS   16
#define VP_SECTOR    4096UL
#define VP_PER       (VP_SECTOR / sizeof(vp_entry_t))
#define VP_SAFE      0x45464153UL   // "SAFE"
#define VP_UNSAFE    0UL
#define STILL_MS     2000

typedef struct {
    uint32_t seq;        // +1 each entry, never 0xFFFFFFFF
    int32_t count;
    uint32_t state;      // VP_SAFE or VP_UNSAFE
    uint32_t crc;
} vp_entry_t;
_Static_assert(sizeof(vp_entry_t) == 16, "16-byte entries");

static bool ok;                 // flash found
static uint32_t sector, at;  // where the next entry goes
static vp_entry_t last;         // newest good entry (seq 0: none)
static bool offer;
static int32_t boot_count;
static uint32_t still_since;
static uint32_t writes;

static uint32_t crc32(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t c = 0xFFFFFFFFUL;
    while (len--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320UL & (0UL - (c & 1)));
    }
    return ~c;
}

static bool good(const vp_entry_t *e)
{
    return e->seq != 0xFFFFFFFFUL && e->seq != 0 && e->crc == crc32(e, offsetof(vp_entry_t, crc));
}

static uint32_t addr(uint32_t s, uint32_t i)
{
    return VP_BASE + s * VP_SECTOR + i * sizeof(vp_entry_t);
}

static void put(int32_t count, bool safe)
{
    if (at >= VP_PER) {
        sector = (sector + 1) % VP_SECTORS;
        at = 0;
    }
    if (at == 0 && !extflash_erase_4k(addr(sector, 0)))
        return;
    vp_entry_t e = { last.seq + 1, count, safe ? VP_SAFE : VP_UNSAFE, 0 };
    if (e.seq == 0xFFFFFFFFUL)
        e.seq = 1;
    e.crc = crc32(&e, offsetof(vp_entry_t, crc));
    extflash_write(addr(sector, at), &e, sizeof e);
    at++;
    last = e;   // even if the write failed: the next change tries again
    writes++;
}

void valvepos_init(void)
{
    vp_entry_t e;
    ok = extflash_init();
    if (!ok)
        return;
    // The sector whose first entry is newest holds the newest entry.
    int best = -1;
    uint32_t best_seq = 0;
    for (uint32_t s = 0; s < VP_SECTORS; s++) {
        extflash_read(addr(s, 0), &e, sizeof e);
        if (good(&e) && (best < 0 || e.seq > best_seq)) {
            best = (int)s;
            best_seq = e.seq;
        }
    }
    sector = best < 0 ? 0 : (uint32_t)best;
    at = 0;
    if (best >= 0) {
        for (uint32_t i = 0; i < VP_PER; i++) {
            extflash_read(addr(sector, i), &e, sizeof e);
            if (e.seq == 0xFFFFFFFFUL)
                break;
            at = i + 1;       // a torn entry is skipped, not written over
            if (good(&e) && e.seq >= last.seq)
                last = e;
        }
    }
    if (last.seq && last.state == VP_SAFE) {
        offer = true;
        boot_count = last.count;
        char p[16];
        valve_event("REF", "Saved position on offer: count %ld (%s %%), use it or calibrate",
                    (long)boot_count, fixed_fmt(p, sizeof p,
                    fixed_round((int64_t)boot_count * 10000 / valve_pos_max(), 1), 1));
    }
}

void valvepos_task(void)
{
    if (!ok)
        return;
    bool moving = stepper3_busy() || valve_mode() == VALVE_CALIBRATING;
    bool cal = valve_calibrated();
    int32_t pos = stepper3_position();
    if (moving)
        still_since = 0;
    else if (!still_since)
        still_since = lv_tick_get() | 1;
    if (offer) {
        // Kept, untouched, until the operator answers or something moves.
        if (!moving && !cal)
            return;
        offer = false;
    }
    bool safe = last.seq && last.state == VP_SAFE;
    if (safe && (moving || !cal || pos != last.count))
        put(pos, false);
    else if (!safe && cal && !moving && still_since && lv_tick_elaps(still_since) >= STILL_MS)
        put(pos, true);
}

bool valvepos_offer(int32_t *count)
{
    if (offer)
        *count = boot_count;
    return offer;
}

bool valvepos_use(const char **why)
{
    if (!offer) { *why = "no saved position on offer"; return false; }
    if (!valve_restore(boot_count, why))
        return false;
    offer = false;
    return true;
}

void valvepos_decline(void)
{
    offer = false;
}

bool valvepos_command(const char *line)
{
    const char *why;
    if (strcmp(line, "VALVE SAVED?") == 0) {
        if (!ok)
            link_printf("VALVE SAVED serial flash not found\r\n");
        else if (!last.seq)
            link_printf("VALVE SAVED none\r\n");
        else
            link_printf("VALVE SAVED count %ld, %s, entry %lu of sector %lu, %lu writes since boot%s\r\n",
                        (long)last.count, last.state == VP_SAFE ? "safe (at standstill)" : "unsafe (moved since)",
                        (unsigned long)at, (unsigned long)sector, (unsigned long)writes,
                        offer ? ", on offer: VALVE RESTORE to use" : "");
        link_printf("OK\r\n");
    } else if (strcmp(line, "VALVE RESTORE") == 0) {
        if (valvepos_use(&why))
            link_printf("OK position %ld from the saved count\r\n", (long)boot_count);
        else
            link_printf("ERR %s\r\n", why);
    } else {
        return false;
    }
    return true;
}
