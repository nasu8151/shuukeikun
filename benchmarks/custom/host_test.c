/* Host-side validation for the custom/ benchmarks.
 *
 * The benchmarks are pure fixed-width integer code with no target
 * dependencies, so the same sources compiled for the host must produce the
 * same checksum as the Cortex-M3 build -- which is what makes it possible
 * to check them at all, given that the bare-metal ELF has no console
 * (run_all.sh passes -serial none). Anything address-derived is kept out
 * of the checksums for the same reason; see harness.h.
 *
 * Build and run one benchmark:
 *   gcc -Wall -Wextra -O2 -Icustom -o /tmp/pidctl_host \
 *       custom/pidctl/pidctl.c custom/host_test.c && /tmp/pidctl_host
 *
 * Or all of them at once: ./custom/check.sh
 *
 * Prints the checksum, so this is also how a new benchmark's
 * bench_expected() value gets established in the first place.
 */

#include <stdio.h>

#include "harness.h"

int
main (void)
{
  uint32_t got, again, want;

  bench_init ();
  got = bench_run ();

  bench_init ();
  again = bench_run ();

  want = bench_expected ();

  printf ("checksum 0x%08x, expected 0x%08x\n", got, want);

  if (again != got)
    {
      printf ("FAIL: not reproducible (second run gave 0x%08x)\n", again);
      return 2;
    }
  if (got != want)
    {
      printf ("FAIL: checksum mismatch\n");
      return 1;
    }

  printf ("OK\n");
  return 0;
}
