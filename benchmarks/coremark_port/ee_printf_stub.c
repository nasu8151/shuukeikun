#include <stdarg.h>

/* No output device is wired up for this bare-metal mps2-an385 build; we
 * only care about exercising the CoreMark workload under the bitwidth
 * plugin, not about the printed report. */
int ee_printf(const char *fmt, ...)
{
    (void)fmt;
    return 0;
}
