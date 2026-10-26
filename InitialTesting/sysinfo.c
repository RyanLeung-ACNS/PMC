#include <string.h>

#include "mcu.h"
#include "lvgl.h"   // lv_snprintf
#include "pmc_config.h"
#include "link.h"
#include "ethlink.h"
#include "sysinfo.h"

static const char *reason = "unknown";
static bool abnormal;
static char line[96];

void sysinfo_init(void)
{
    uint32_t csr = RCC->CSR;

    // A power-on also sets the pin and brown-out flags, so check the most
    // specific causes first. mikroBootloader runs before the application;
    // if it ever clears these flags the result is "unknown".
    if (csr & RCC_CSR_IWDGRSTF)       { reason = "watchdog"; abnormal = true; }
    else if (csr & RCC_CSR_WWDGRSTF)  { reason = "window watchdog"; abnormal = true; }
    else if (csr & RCC_CSR_LPWRRSTF)  { reason = "low-power"; abnormal = true; }
    else if (csr & RCC_CSR_SFTRSTF)   reason = "software";
    else if (csr & RCC_CSR_PORRSTF)   reason = "power-on";
    else if (csr & RCC_CSR_BORRSTF)   { reason = "brown-out"; abnormal = true; }
    else if (csr & RCC_CSR_PINRSTF)   reason = "reset pin";

    RCC->CSR |= RCC_CSR_RMVF;

    lv_snprintf(line, sizeof line, "PMC firmware %s, last reset: %s",
                sysinfo_version(), reason);
}

const char *sysinfo_version(void)
{
    return PMC_FW_VERSION " (" __DATE__ " " __TIME__ ")";
}

const char *sysinfo_reset_reason(void) { return reason; }
bool sysinfo_reset_abnormal(void)      { return abnormal; }
const char *sysinfo_line(void)         { return line; }

// *IDN? (0.12.2): the IEEE 488.2 identity query most instrument software
// sends first. One line, maker,model,serial,firmware, with the STM32's
// 96-bit unique ID as the serial; no echo and no OK, as the standard reply.
static bool idn_command(const char *cmd)
{
    if (strcmp(cmd, "*IDN?") != 0)
        return false;
#ifdef __wasm__
    static const uint8_t uid[12] = { 0 };   // the simulator: no chip ID to read
#else
    const volatile uint8_t *uid = (const volatile uint8_t *)0x1FFF7A10UL;
#endif
    char id[25];
    for (int i = 0; i < 12; i++)
        lv_snprintf(id + 2 * i, 3, "%02X", uid[i]);
    link_printf("PMC,MM4,%s,%s\r\n", id, PMC_FW_VERSION);
    return true;
}

bool sysinfo_command(const char *cmd)
{
    if (idn_command(cmd))
        return true;
    if (strcmp(cmd, "VER?") != 0)
        return false;
    link_printf("%s\r\n", line);
#if PMC_LINK_ETHERNET
    // For a DHCP reservation: the address IT ties the fixed IP to.
    link_printf("MAC %s, IP %s\r\n", ethlink_mac()[0] ? ethlink_mac() : "none",
                ethlink_ip()[0] ? ethlink_ip() : "none");
#endif
    link_printf("OK\r\n");
    return true;
}
