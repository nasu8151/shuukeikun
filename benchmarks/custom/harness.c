/* Bare-metal entry point shared by the custom/ benchmarks.
 *
 * One fixed workload, then idle -- the same shape as Embench's
 * support/main.c, and for a reason that matters to the analysis:
 * analyze.py sums raw counts across benchmarks, so a benchmark that kept
 * working for the whole RUNTIME window instead of finishing would
 * contribute two orders of magnitude more samples than the Embench ones
 * and dominate the combined distribution on its own. The workload sizes in
 * each benchmark (TICKS, FRAMES) are chosen to land in the same range as
 * the rest of the corpus.
 *
 * Idling after the run costs nothing: the spin below is a branch, and the
 * plugin only records ALU results and stores.
 */

#include "harness.h"

/* volatile: keeps the whole computation live, and gives a debugger
 * something to look at. */
volatile uint32_t g_bench_check;
volatile uint32_t g_bench_ok;

int
main (void)
{
  uint32_t chk;

  bench_init ();
  chk = bench_run ();

  /* The bare-metal build has no console (run_all.sh passes -serial none),
     so this check exists to be caught by a debugger or by an unexpectedly
     small CSV. host_test.c is where a mismatch gets diagnosed. */
  g_bench_check = chk;
  g_bench_ok = (chk == bench_expected ());

  for (;;)
    ;
}
