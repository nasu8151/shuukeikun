/* Minimal Embench board support for bare-metal mps2-an385 (Cortex-M3). We
 * don't care about wall-clock timing accuracy here -- this build only
 * exists to exercise the bitwidth QEMU plugin against real benchmark code,
 * not to produce an official Embench score. */

void initialise_board(void) { }
void start_trigger(void) { }
void stop_trigger(void) { }
