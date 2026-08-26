/* Shared interface for the hand-written benchmarks in custom/.
 *
 * These exist because no off-the-shelf suite covers the parts of the target
 * domain we care about most: the fixed-point control work and the
 * accelerator-attendant bookkeeping a soft core is left holding once the
 * DSP-heavy work has moved into dedicated logic. See CLAUDE.md.
 *
 * Contract for a benchmark:
 *   bench_init()     resets all state, so that
 *   bench_run()      always performs the identical workload and returns a
 *                    checksum over the resulting state, and
 *   bench_expected() returns the value bench_run() must produce.
 *
 * The checksum must be free of anything address-derived (pointer values
 * differ between the ARM target and the host used to validate the logic --
 * see host_test.c).
 */
#ifndef CUSTOM_HARNESS_H
#define CUSTOM_HARNESS_H

#include <stdint.h>

void bench_init(void);
uint32_t bench_run(void);
uint32_t bench_expected(void);

#endif /* CUSTOM_HARNESS_H */
