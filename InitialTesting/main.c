#include <string.h>
/**
 * @file main.c
 * @brief Main source file for the InitialTesting LVGL Designer application.
 *
 * This example demonstrates usage of the LVGL graphics library
 * for creating and running GUI applications with display and
 * touch support on embedded devices.
 */

#ifdef PREINIT_SUPPORTED
#include "preinit.h"
#endif

#ifdef __GNUC__
#include "delays.h"
#endif

#include "display_lvgl.h"
#include "lv_port_indev.h"
#include "1ms_Timer.h"
#include "pmc_config.h"
#include "link.h"
#include "sysinfo.h"
#include "rtclock.h"
#include "clicks.h"
#include "cal.h"
#include "stepper3.h"
#include "valve.h"
#include "cfg.h"
#include "ui.h"
#include "limit.h"
#include "supply.h"
#include "sense.h"
#include "valvepos.h"
#include "trend.h"
#include "baro.h"
#include "remote.h"

/*
 * PERF?: how responsive the main loop is, since the last PERF?. Touch is
 * read inside lv_timer_handler, so a long handler or loop pass is input lag.
 */
extern uint32_t disp_flush_ms, disp_flush_px;
static uint32_t perf_loops, perf_gap_max, perf_handler_max, perf_last_end;
#if PMC_ENG_COMMANDS
static uint32_t perf_since;
#endif

static void perf_note(uint32_t t0)
{
    uint32_t now = lv_tick_get();
    uint32_t handler = now - t0;
    if (handler > perf_handler_max) perf_handler_max = handler;
    if (perf_last_end && now - perf_last_end > perf_gap_max) perf_gap_max = now - perf_last_end;
    perf_last_end = now;
    perf_loops++;
}

#if PMC_ENG_COMMANDS
// PERF? (engineering, remote.c).
bool perf_command(const char *line)
{
    if (strcmp(line, "PERF?") != 0)
        return false;
    uint32_t span = lv_tick_elaps(perf_since);
    lv_mem_monitor_t mem;
    lv_mem_monitor(&mem);
    link_printf("PERF over %lu ms: loops=%lu longest_loop_ms=%lu longest_lvgl_ms=%lu "
                "flush_ms=%lu flush_px=%lu\r\n",
                (unsigned long)span, (unsigned long)perf_loops, (unsigned long)perf_gap_max,
                (unsigned long)perf_handler_max, (unsigned long)disp_flush_ms,
                (unsigned long)disp_flush_px);
    // LVGL heap: peak is the most ever used since boot, not since the last PERF?.
    link_printf("HEAP total=%lu used=%lu peak=%lu biggest_free=%lu frag_pct=%u\r\nOK\r\n",
                (unsigned long)mem.total_size, (unsigned long)(mem.total_size - mem.free_size),
                (unsigned long)mem.max_used, (unsigned long)mem.free_biggest_size,
                (unsigned)mem.frag_pct);
    perf_since = lv_tick_get();
    perf_loops = perf_gap_max = perf_handler_max = 0;
    disp_flush_ms = disp_flush_px = 0;
    return true;
}
#endif

/**
 * @brief Initializes board peripherals for display, touch, and LVGL timing.
 */
void board_init()
{
    lv_init();
    // A second LVGL heap pool in main SRAM: the 64 KB in CCM ran out with the
    // 0.6.0 Service screens built and the Valve page keypad open (0.8.1).
#if !PMC_LINK_ETHERNET
    // Not in the Ethernet build (0.11.30): with it, .bss ran to within about
    // 3 KB of the top of SRAM, which left no room for the stack and the
    // network buffers (malloc'd), and the screen stayed blank at boot. That
    // build has the 64 KB in CCM only; the host previews peak at about 55 KB
    // with 64-bit pointers, and the board read 33 KB on 14 Oct.
    static uint8_t lv_pool2[24 * 1024] __attribute__((aligned(8)));
    lv_mem_add_pool(lv_pool2, sizeof lv_pool2);
#endif
    lv_port_disp_init();
    lv_port_indev_init();

    // Configure 1ms timer interrupt for LVGL tick updates.
    timerInit();
}

// Waits ms on the start-up screen, its bar moving on every 30 ms.
static void splash_wait(int ms)
{
    for (int t = 0; t < ms; t += 30) {
        Delay_ms(30);
        ui_splash_tick();
    }
}

/**
 * @brief Initializes the LVGL application and loads the main screen.
 */
void application_init()
{
    // Why the last reset happened (before anything else touches RCC).
    sysinfo_init();
    rtclock_init();

    // Initialize board peripherals and LVGL drivers.
    board_init();
    // The start-up screen until Home is built (0.13.3): from 0.13.13 the
    // logo, a bar and each check's result under it as it is made, paced so
    // it can be read (Ryan, 28 Oct; the board is ready in about a second).
    // No delay in the simulator.
    ui_splash();
    ui_splash_line(!sysinfo_reset_abnormal(), "Last reset", "%s", sysinfo_reset_reason());

    // Druck calibration from the on-board serial flash (nominal if none stored).
    cal_init();

    // Click boards on the shield. Stepper coils off; the motor only moves
    // when the valve is moved (screen or link).
    clicks_init();
    limit_init();
    supply_init();   // motor 12 V on PA3
    valve_init();   // NOT HOMED: nothing moves until homed or moved on the screen
    splash_wait(200);
    clicks_poll();   // the excitation once the Boost 10 has settled
    const clicks_state_t *cs = clicks_state();
    char v[24];
    ui_splash_line(cs->pm_found, "Power monitor", cs->pm_found ? "INA228 found" : "INA228 not answering");
    splash_wait(180);
    ui_splash_line(!cs->boost_tripped && cs->bus_mv >= 9500 && cs->bus_mv <= 10500, "Druck excitation",
                   cs->boost_tripped ? "tripped" : "%s V", ui_fmt2(v, sizeof v, cs->bus_mv));
    splash_wait(180);
    ui_splash_line(cs->reading_ok, "Pressure", cs->reading_ok ? "%s mbar" : cs->druck_absent ? "no Druck signal" : "no reading",
                   ui_fmt1(v, sizeof v, cs->p_mmbar));
    // Saved motor, valve and cal settings (factory defaults if none). Nothing moves.
    cfg_init();
    splash_wait(180);
    {
        uint32_t rev;
        const char *at;
        char d[24];
        if (!cfg_flash_ok())
            ui_splash_line(false, "Settings", "serial flash not found, defaults");
        else if (cfg_rev(CFG_FROM_SAVED, &rev, &at))
            ui_splash_line(true, "Settings", "rev %lu, saved %s", (unsigned long)rev, at && at[0] ? ui_date(d, sizeof d, at) : "");
        else
            ui_splash_line(true, "Settings", "factory defaults");
    }
    // The valve position saved at the last standstill: offered on the
    // screen (Use saved / Calibrate), never used by itself.
    valvepos_init();
    splash_wait(180);
    {
        int32_t q;
        if (valvepos_offer(&q)) {
            int32_t pm = valve_pos_max();
            ui_fmt2(v, sizeof v, ((int64_t)q * 100000 + (q >= 0 ? pm / 2 : -pm / 2)) / pm);
            ui_splash_line(true, "Valve position", "last saved %s %%, not homed", v);
        } else {
            ui_splash_line(true, "Valve position", "not homed");
        }
    }
    splash_wait(180);
#if PMC_LINK_ETHERNET
    ui_splash_line(true, "Network", "Ethernet, remote control %s", cfg_remote() ? "On" : "Off");
#else
    ui_splash_line(true, "Network", "USB COM port");
#endif
    splash_wait(300);
    ui_splash_done();
    splash_wait(500);

    // The frame and Home. The 0.6.0 motor screen is built the first time
    // Menu > Motor opens it, which keeps its LVGL heap free until then.
    ui_init();

    // Draw the screen once first, so a link start-up fault cannot leave it blank.
    lv_timer_handler();

    // Text link to the PC (pmc_config.h): USB COM port (sets the 48 MHz USB
    // clock) or Ethernet (sets SYSCLK 150 MHz and the 50 MHz RMII clock).
    link_init();
}

/**
 * @brief Independent watchdog: resets the MCU if the main loop stops for
 * more than ~3-4 s (LSI 32 kHz nominal, 17-47 kHz; /64, reload 2000).
 * Covers hangs such as LV_ASSERT's while(1), HardFault and unbounded
 * hardware waits. Started after init; a software-started IWDG is stopped
 * by the reset it causes, so mikroBootloader is unaffected.
 */
static void watchdog_start(void)
{
    IWDG->KR = 0x5555;      // unlock PR/RLR
    IWDG->PR = 4;           // /64
    IWDG->RLR = 2000;
    IWDG->KR = 0xAAAA;      // reload
    IWDG->KR = 0xCCCC;      // start
}

static inline void watchdog_kick(void)
{
    IWDG->KR = 0xAAAA;
}

/**
 * @brief Application entry point.
 *
 * Initializes the MCU and LVGL environment, then enters
 * the main event loop for GUI processing.
 */
int main(void)
{
    /* Do not remove this line — it ensures correct MCU initialization. */
#ifdef PREINIT_SUPPORTED
    preinit();
#endif

    // Initialize the application.
    application_init();
    watchdog_start();

    ////////////////////////// LVGL timing routine (DO NOT REMOVE) //////////////////////////
    char line[LINK_LINE_MAX + 1];
    uint32_t sense_ms = lv_tick_get();
    while (1)
    {
        link_task();
        touch_poll();
        rtclock_poll();
        limit_poll();
        supply_poll();
        stepper3_poll();
        valve_task();
        valvepos_task();
        trend_task();
        if (lv_tick_elaps(sense_ms) >= 1000) {
            sense_ms += 1000;
            if (lv_tick_elaps(sense_ms) >= 1000)   // fell behind: no catch-up burst
                sense_ms = lv_tick_get();
            sense_second();
        }
        // The remote command set (remote.c): no echo, one reply line.
        if (link_getline(line, sizeof line))
            remote_line(line);
        uint32_t t0 = lv_tick_get();
        lv_timer_handler();
        perf_note(t0);
        watchdog_kick();
        Delay_ms(1);
    }
    ////////////////////////////////////////////////////////////////////////////////////////

    return 0;
}

/**
 * @brief 1ms interrupt routine for the LVGL tick (and the CycloneTCP tick).
 *
 * Touch is no longer polled here; see touchpad_read() in lv_port_indev.c.
 */
static volatile uint32_t msCount = 0;

#if PMC_LINK_ETHERNET
extern volatile uint32_t systemTicks;   // CycloneTCP time base, 1 ms
#endif

INTERRUPT_ROUTINE
{
    msCount++;
#if PMC_LINK_ETHERNET
    systemTicks++;
#endif

    if (5 == msCount) {
        msCount = 0;
        lv_tick_inc(5);
    }

    CLEAR_FLAG;
}
