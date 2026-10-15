#include <stddef.h>
#include <string.h>

#include "lvgl.h"        // lv_tick_get
#include "link.h"
#include "extflash.h"
#include "cal.h"
#include "stepper3.h"
#include "valve.h"
#include "cfg.h"

/* --------------------------------------------------------------------------
 * Record. Little-endian, fixed layout, CRC32 (IEEE) over everything before
 * the crc field. One per 4 KB sector, two sectors used in turn, the higher
 * rev is the newest. Below the old cal record at 0x7FF000.
 * ------------------------------------------------------------------------ */
#define CFG_ADDR_A   0x7FD000UL
#define CFG_ADDR_B   0x7FE000UL
#define CFG_MAGIC    0x47464350UL   // "PCFG"
#define CFG_VERSION  1

enum { M_RATE, M_START, M_ACCEL, M_MODE, M_HOLD, M_DIR, M_ORDER, M_N };
static const char *const mot_name[M_N] = { "RATE", "START", "ACCEL", "MODE", "HOLD", "DIR", "ORDER" };

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t size;               // sizeof(cfg_rec_t)
    uint32_t rev;                // 1 for the first save, +1 each save
    int32_t mot[M_N];            // ORDER as four digits, e.g. 213 for 0213
    int32_t valve[VALVE_N_SETTINGS];
    uint32_t has_cal;            // 0: nominal cal
    cal_rec_t cal;
    uint32_t crc;
} cfg_rec_t;

static const uint32_t slot_addr[2] = { CFG_ADDR_A, CFG_ADDR_B };
static cfg_rec_t slot[2];        // as read from each sector
static const char *slot_why[2];  // NULL if valid, else why not
static int newest = -1;          // slot with the newest valid record, -1 none
static int loaded = -1;          // slot loaded at start-up, -1 none
static uint32_t loaded_rev;
static cfg_rec_t factory;        // start-up values before any record is loaded
static bool flash_ok;

static const char *const EMPTY = "empty";

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

static uint32_t rec_crc(const cfg_rec_t *r)
{
    return crc32(r, offsetof(cfg_rec_t, crc));
}

static char slot_letter(int i) { return i == 0 ? 'A' : 'B'; }

static void order_digits(int32_t packed, uint8_t o[4])
{
    o[0] = packed / 1000 % 10; o[1] = packed / 100 % 10; o[2] = packed / 10 % 10; o[3] = packed % 10;
}

// The live settings.
static void capture(cfg_rec_t *r)
{
    uint8_t o[4];
    memset(r, 0, sizeof *r);
    r->magic = CFG_MAGIC;
    r->version = CFG_VERSION;
    r->size = sizeof *r;
    r->mot[M_RATE] = (int32_t)stepper3_rate();
    r->mot[M_START] = (int32_t)stepper3_start_rate();
    r->mot[M_ACCEL] = (int32_t)stepper3_accel();
    r->mot[M_MODE] = stepper3_mode();
    r->mot[M_HOLD] = stepper3_hold();
    r->mot[M_DIR] = stepper3_reverse();
    stepper3_order(o);
    r->mot[M_ORDER] = o[0] * 1000 + o[1] * 100 + o[2] * 10 + o[3];
    valve_settings(r->valve);
    r->has_cal = !cal_is_default();
    if (r->has_cal)
        cal_get_record(&r->cal);
}

// NULL if every value can be applied, else the first that cannot.
static const char *values_why(const cfg_rec_t *r)
{
    const int32_t *m = r->mot;
    uint8_t o[4], seen = 0;
    if (m[M_RATE] < STEPPER3_RATE_MIN || m[M_RATE] > STEPPER3_RATE_MAX) return "MOT RATE";
    if (m[M_START] < STEPPER3_RATE_MIN || m[M_START] > STEPPER3_RATE_MAX) return "MOT START";
    if (m[M_ACCEL] < STEPPER3_ACCEL_MIN || m[M_ACCEL] > STEPPER3_ACCEL_MAX) return "MOT ACCEL";
    if (m[M_MODE] < STEPPER3_WAVE || m[M_MODE] > STEPPER3_HALF) return "MOT MODE";
    if (m[M_HOLD] < 0 || m[M_HOLD] > 1) return "MOT HOLD";
    if (m[M_DIR] < 0 || m[M_DIR] > 1) return "MOT DIR";
    if (m[M_ORDER] < 0 || m[M_ORDER] > 3333) return "MOT ORDER";
    order_digits(m[M_ORDER], o);
    for (int i = 0; i < 4; i++) {
        if (o[i] > 3 || (seen & (1u << o[i])))
            return "MOT ORDER";
        seen |= 1u << o[i];
    }
    if (valve_settings_check(r->valve))
        return valve_settings_check(r->valve);
    if (r->has_cal > 1 || (r->has_cal && !cal_record_ok(&r->cal)))
        return "CAL";
    return NULL;
}

// NULL if r is a usable record, else why not.
static const char *rec_why(const cfg_rec_t *r)
{
    if (r->magic == 0xFFFFFFFFUL)
        return EMPTY;
    if (r->magic != CFG_MAGIC || r->version != CFG_VERSION || r->size != sizeof *r)
        return "not a settings record of this version";
    if (r->crc != rec_crc(r))
        return "CRC mismatch";
    if (values_why(r))
        return "value out of range";
    return NULL;
}

static void scan(void)
{
    newest = -1;
    for (int i = 0; i < 2; i++) {
        extflash_read(slot_addr[i], &slot[i], sizeof slot[i]);
        slot_why[i] = rec_why(&slot[i]);
        if (!slot_why[i] && (newest < 0 || slot[i].rev > slot[newest].rev))
            newest = i;
    }
}

// What the live settings count as unsaved against.
static const cfg_rec_t *base(void)
{
    return newest >= 0 ? &slot[newest] : &factory;
}

static bool cal_same(const cfg_rec_t *a, const cfg_rec_t *b)
{
    return a->has_cal == b->has_cal &&
           (!a->has_cal || memcmp(&a->cal, &b->cal, sizeof a->cal) == 0);
}

// Counts (and with print, lists) the settings in live differing from b.
static int diff(const cfg_rec_t *live, const cfg_rec_t *b, bool print)
{
    int n = 0;
    for (int i = 0; i < M_N; i++) {
        if (live->mot[i] == b->mot[i])
            continue;
        n++;
        if (!print)
            continue;
        if (i == M_MODE)
            link_printf("CFG DIFF MOT MODE saved %s, now %s\r\n",
                        stepper3_mode_name((stepper3_mode_t)b->mot[i]),
                        stepper3_mode_name((stepper3_mode_t)live->mot[i]));
        else if (i == M_ORDER)
            link_printf("CFG DIFF MOT ORDER saved %04ld, now %04ld\r\n", (long)b->mot[i], (long)live->mot[i]);
        else
            link_printf("CFG DIFF MOT %s saved %ld, now %ld\r\n", mot_name[i], (long)b->mot[i], (long)live->mot[i]);
    }
    for (int i = 0; i < VALVE_N_SETTINGS; i++) {
        if (live->valve[i] == b->valve[i])
            continue;
        n++;
        if (print)
            link_printf("CFG DIFF VALVE %s saved %ld, now %ld\r\n", valve_setting_name(i),
                        (long)b->valve[i], (long)live->valve[i]);
    }
    if (!cal_same(live, b)) {
        n++;
        if (print)
            link_printf("CFG DIFF CAL saved %s, now %s%s\r\n", b->has_cal ? b->cal.id : "nominal",
                        live->has_cal ? live->cal.id : "nominal",
                        b->has_cal && live->has_cal && strcmp(b->cal.id, live->cal.id) == 0
                            ? " (new points)" : "");
    }
    return n;
}

int cfg_unsaved(void)
{
    static cfg_rec_t live;
    capture(&live);
    return diff(&live, base(), false);
}

// The cal counts as saved when it matches the newest saved record.
static void sync_cal_saved(void)
{
    static cfg_rec_t live;
    capture(&live);
    cal_set_saved(newest >= 0 ? cal_same(&live, &slot[newest]) : cal_saved());
}

// All or nothing: checked first, applied only when idle. False if not applied.
static bool apply(const cfg_rec_t *r, bool with_cal)
{
    uint8_t o[4];
    if (values_why(r) || valve_mode() == VALVE_CALIBRATING || stepper3_busy())
        return false;
    stepper3_set_rate((uint32_t)r->mot[M_RATE]);
    stepper3_set_start((uint32_t)r->mot[M_START]);
    stepper3_set_accel((uint32_t)r->mot[M_ACCEL]);
    stepper3_set_mode((stepper3_mode_t)r->mot[M_MODE]);
    stepper3_set_hold(r->mot[M_HOLD] != 0);
    stepper3_set_reverse(r->mot[M_DIR] != 0);
    order_digits(r->mot[M_ORDER], o);
    stepper3_set_order(o);
    valve_set_settings(r->valve);
    if (with_cal) {
        if (r->has_cal)
            cal_set_record(&r->cal, false);
        else
            cal_set_default(false);
    }
    sync_cal_saved();
    return true;
}

static bool verify(uint32_t addr, const void *buf, size_t len)
{
    uint8_t b[64];
    const uint8_t *p = buf;
    for (size_t o = 0; o < len; o += sizeof b) {
        size_t n = len - o < sizeof b ? len - o : sizeof b;
        extflash_read(addr + o, b, n);
        if (memcmp(b, p + o, n) != 0)
            return false;
    }
    return true;
}

/* --------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------ */
void cfg_init(void)
{
    loaded = -1;
    capture(&factory);
    flash_ok = extflash_init();
    if (!flash_ok) {
        valve_event("CFG", "Serial flash not found, factory defaults");
        return;
    }
    scan();
    for (int i = 0; i < 2; i++)
        if (slot_why[i] && slot_why[i] != EMPTY)
            valve_event("CFG", "Record in %c not used: %s", slot_letter(i), slot_why[i]);
    if (newest < 0) {
        valve_event("CFG", "No saved settings, factory defaults%s",
                    cal_from_old_record() ? ", cal from the old record" : "");
        return;
    }
    apply(&slot[newest], true);   // idle at start-up: cannot be refused
    loaded = newest;
    loaded_rev = slot[newest].rev;
    valve_event("CFG", "Settings rev %lu loaded from %c", (unsigned long)slot[newest].rev,
                slot_letter(newest));
}

bool cfg_save(void)
{
    static cfg_rec_t rec;   // static: keeps 450 B off the stack
    const char *why;

    if (!flash_ok) {
        link_printf("ERR serial flash not found, nothing saved\r\n");
        return false;
    }
    capture(&rec);
    if ((why = values_why(&rec)) != NULL) {
        link_printf("ERR %s out of range, nothing saved\r\n", why);
        return false;
    }
    rec.rev = newest >= 0 ? slot[newest].rev + 1 : 1;
    rec.crc = rec_crc(&rec);
    int t = newest == 0 ? 1 : 0;
    uint32_t t0 = lv_tick_get();
    bool ok = extflash_erase_4k(slot_addr[t]) &&
              extflash_write(slot_addr[t], &rec, sizeof rec) &&
              verify(slot_addr[t], &rec, sizeof rec);
    uint32_t ms = lv_tick_elaps(t0);
    if (!ok) {
        scan();
        valve_event("CFG", "Save failed in %c, nothing saved", slot_letter(t));
        if (newest >= 0)
            link_printf("ERR flash write or read-back failed in %c, nothing saved, rev %lu in %c kept\r\n",
                        slot_letter(t), (unsigned long)slot[newest].rev, slot_letter(newest));
        else
            link_printf("ERR flash write or read-back failed in %c, nothing saved\r\n", slot_letter(t));
        return false;
    }
    slot[t] = rec;
    slot_why[t] = NULL;
    newest = t;
    cal_set_saved(true);
    valve_event("CFG", "Settings saved, rev %lu in %c", (unsigned long)rec.rev, slot_letter(t));
    link_printf("OK saved rev %lu in %c (%lu ms)\r\n", (unsigned long)rec.rev, slot_letter(t),
                (unsigned long)ms);
    return true;
}

static void show(void)
{
    link_printf("CFG flash %s\r\n", flash_ok ? "ok" : "not found");
    for (int i = 0; flash_ok && i < 2; i++) {
        if (slot_why[i])
            link_printf("CFG %c: %s\r\n", slot_letter(i), slot_why[i]);
        else
            link_printf("CFG %c: rev %lu ok%s\r\n", slot_letter(i), (unsigned long)slot[i].rev,
                        i == newest ? " (newest)" : "");
    }
    if (loaded >= 0)
        link_printf("CFG start-up: rev %lu loaded from %c\r\n", (unsigned long)loaded_rev,
                    slot_letter(loaded));
    else
        link_printf("CFG start-up: factory defaults, no saved record\r\n");
    int n = cfg_unsaved();
    if (n)
        link_printf("CFG unsaved: %d setting%s from %s, CFG DIFF? to list\r\n", n,
                    n == 1 ? " differs" : "s differ",
                    newest >= 0 ? "the newest save" : "the start-up values");
    else
        link_printf("CFG unsaved: none\r\n");
    link_printf("OK\r\n");
}

static void load(const char *what)
{
    const cfg_rec_t *src;
    bool with_cal = true;
    int other = newest < 0 ? -1 : 1 - newest;

    if (strcmp(what, "LAST") == 0) {
        if (newest < 0) { link_printf("ERR nothing saved\r\n"); return; }
        src = &slot[newest];
    } else if (strcmp(what, "PREVIOUS") == 0) {
        if (other < 0 || slot_why[other]) { link_printf("ERR no previous record\r\n"); return; }
        src = &slot[other];
    } else if (strcmp(what, "DEFAULTS") == 0) {
        src = &factory;
        with_cal = false;
    } else {
        link_printf("ERR expected: CFG LOAD LAST|PREVIOUS|DEFAULTS\r\n");
        return;
    }
    if (valve_mode() == VALVE_CALIBRATING || stepper3_busy()) {
        link_printf("ERR busy, wait for the move to end or CTRL STOP\r\n");
        return;
    }
    if (!apply(src, with_cal)) {
        link_printf("ERR record not applied\r\n");
        return;
    }
    if (src == &factory)
        valve_event("CFG", "Factory motor and valve settings loaded (cal kept)");
    else
        valve_event("CFG", "Rev %lu loaded from %c", (unsigned long)src->rev, slot_letter((int)(src - slot)));
    int n = cfg_unsaved();
    link_printf("OK loaded, %d unsaved%s\r\n", n, n ? ": CFG SAVE to keep" : "");
}

static void test(const char *what)
{
    if (!flash_ok) { link_printf("ERR serial flash not found\r\n"); return; }
    if (strcmp(what, "CORRUPT") == 0) {
        if (newest < 0) { link_printf("ERR nothing saved\r\n"); return; }
        static const uint8_t zero[4];
        int was = newest;
        // Programming clears bits without an erase: the CRC no longer matches.
        extflash_write(slot_addr[was] + offsetof(cfg_rec_t, crc), zero, sizeof zero);
        scan();
        valve_event("CFG", "Test: record in %c spoiled", slot_letter(was));
        link_printf("OK record in %c spoiled (%s), restart to check the start-up\r\n", slot_letter(was),
                    slot_why[was] ? slot_why[was] : "still valid?");
    } else if (strcmp(what, "ERASE") == 0) {
        bool ok = extflash_erase_4k(CFG_ADDR_A) && extflash_erase_4k(CFG_ADDR_B);
        scan();
        valve_event("CFG", "Test: both settings sectors erased");
        link_printf(ok ? "OK both erased, restart for factory defaults (old cal record kept)\r\n"
                       : "ERR erase timed out\r\n");
    } else {
        link_printf("ERR expected: CFG TEST CORRUPT|ERASE\r\n");
    }
}

bool cfg_command(const char *line)
{
    if (strncmp(line, "CFG", 3) != 0 || (line[3] != ' ' && line[3] != '?' && line[3] != '\0'))
        return false;
    const char *a = line + 3;

    if (strcmp(a, "?") == 0) {
        show();
    } else if (strcmp(a, " DIFF?") == 0) {
        static cfg_rec_t live;
        capture(&live);
        int n = diff(&live, base(), true);
        link_printf("OK %d difference%s from %s\r\n", n, n == 1 ? "" : "s",
                    newest >= 0 ? "the newest save" : "the start-up values");
    } else if (strcmp(a, " SAVE") == 0) {
        cfg_save();
    } else if (strncmp(a, " LOAD ", 6) == 0) {
        load(a + 6);
    } else if (strncmp(a, " TEST ", 6) == 0) {
        test(a + 6);
    } else {
        link_printf("ERR unknown CFG command\r\n");
    }
    return true;
}
