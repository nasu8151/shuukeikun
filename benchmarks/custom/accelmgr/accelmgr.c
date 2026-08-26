/* accelmgr -- "accelerator-attendant" processing.
 *
 * This is the row of the target-domain table in CLAUDE.md that no existing
 * benchmark suite covers, and the one closest to the dsPIC-style system
 * this study is aimed at: the heavy signal processing lives in dedicated
 * logic, and the soft core is left with everything around it --
 *
 *   - programming the engine's control registers (small fields packed into
 *     wide control words),
 *   - building and enqueuing DMA descriptors (addresses, lengths, flags),
 *   - marshalling payloads into the DMA-visible buffers,
 *   - ring-buffer bookkeeping (head/tail, wrap masks, free space),
 *   - interrupt-time result retrieval and the light post-processing that
 *     follows it (scaling, thresholding, statistics).
 *
 * accel_tick() is a stand-in for the hardware, and is deliberately kept as
 * small as it can be while still retiring descriptors and handing back a
 * plausible result block. By construction that work never runs on the soft
 * core, so every instruction spent simulating it is contamination of the
 * histogram -- it is a handful per descriptor against the core's dozens,
 * which is the best this approach can do short of a second QEMU device
 * model.
 */

#include "harness.h"

#define RING_N       16
#define RING_MASK    (RING_N - 1)
#define QUEUE_DEPTH  6		/* descriptors the driver keeps in flight */

#define BUF_BYTES    256
#define RES_PER_DESC 8

#define OUT_N        32
#define OUT_MASK     (OUT_N - 1)

/* Frames submitted per run. Sized so the sample count lands near the
   middle of the existing corpus (~1.1M ALU results); analyze.py sums raw
   counts across benchmarks, so a workload far larger than its peers would
   skew the combined distribution. */
#define FRAMES       1536
#define NCHAN        4		/* logical accelerator channels */

/* ------------------------------------------------------------------ */
/* Engine register block.
 *
 * Real MMIO would sit at a fixed bus address. What this study measures is
 * the value stored, not the address it goes to, so a volatile array in RAM
 * produces exactly the distribution of interest -- and unlike a hard-coded
 * peripheral address it is portable to the host validation build.
 */

#define REG_CTRL     0
#define REG_CFG      1
#define REG_SRC      2
#define REG_DST      3
#define REG_LEN      4
#define REG_DESC     5
#define REG_STATUS   6
#define REG_IRQMASK  7
#define REG_DOORBELL 8
#define REG_COUNT    16

static volatile uint32_t accel_regs[REG_COUNT];

/* Control-word field layout. Packing 2- to 4-bit fields up into the top of
   a 32-bit word is what makes register programming look the way it does:
   the operands are tiny, the results are nearly full width. */
#define CFG_MODE(x)   (((uint32_t)(x) & 0xFu) << 28)
#define CFG_PRIO(x)   (((uint32_t)(x) & 0x7u) << 25)
#define CFG_BURST(x)  (((uint32_t)(x) & 0x3u) << 23)
#define CFG_IRQEN     (1u << 22)
#define CFG_CHAN(x)   (((uint32_t)(x) & 0xFu) << 18)
#define CFG_LEN(x)    ((uint32_t)(x) & 0xFFFFu)

#define CTRL_ENABLE   (1u << 0)
#define CTRL_RESET    (1u << 1)
#define CTRL_CHAIN    (1u << 2)

#define STAT_IRQ      (1u << 31)
#define STAT_BUSY     (1u << 30)
#define STAT_ERR      (1u << 29)

/* Descriptor flag layout. */
#define DESC_LEN(x)   ((uint32_t)(x) & 0xFFFFu)
#define DESC_CHAN(x)  (((uint32_t)(x) & 0xFu) << 16)
#define DESC_MODE(x)  (((uint32_t)(x) & 0x3u) << 20)
#define DESC_IRQ      (1u << 24)
#define DESC_DONE     (1u << 31)

typedef struct
{
  uint32_t src;			/* address handed to the engine */
  uint32_t dst;
  uint32_t next;		/* descriptor chaining */
  uint32_t len_flags;
  uint32_t status;
  uint32_t seq;
} desc_t;

static desc_t ring[RING_N];
static uint32_t ring_head, ring_tail;

/* Per-descriptor payload buffers: a descriptor pool owns its own staging
   area, so a descriptor still in flight is never overwritten. */
static uint8_t src_buf[RING_N][BUF_BYTES];
static uint16_t res_buf[RING_N][RES_PER_DESC];

/* Engine-side cursor, sequence number, and result-table position. */
static uint32_t hw_cursor, hw_inflight, hw_seq, hw_res;

/* Driver statistics, accumulated at interrupt time. */
static uint32_t stat_frames, stat_bytes, stat_results;
static uint32_t stat_sum, stat_min, stat_max;
static uint32_t stat_over, stat_full, stat_acc_mv;
static uint32_t cal_div;		/* runtime calibration divisor */

/* Report queue handed on to the application layer, and the summary that
   layer keeps. */
static uint32_t out_ring[OUT_N];
static uint32_t out_head, out_tail, out_dropped;
static uint32_t level_count[4];
static uint32_t chan_mag[NCHAN], chan_events[NCHAN];

/* Sensor samples to marshal, and the result magnitudes the engine hands
   back. Both table-driven for the same reason as in pidctl: a PRNG would
   inject full-width words the real workload never sees. */
static const uint16_t sample_tab[32] = {
  2048, 2312, 2570, 2811, 3020, 3187, 3300, 3352,
  3340, 3266, 3134, 2952, 2731, 2484, 2226, 1971,
  1734, 1527, 1361, 1244, 1181, 1174, 1223, 1324,
  1470, 1653, 1862, 2085, 2310, 2524, 2716, 2891
};

/* The engine hands back 12-bit magnitudes, one block of RES_PER_DESC per
   descriptor. Written as eight blocks whose peaks deliberately straddle
   the level thresholds below, so the threshold decision actually
   discriminates instead of answering "level 3" every time. */
static const uint16_t result_tab[64] = {
    12,   31,   58,   94,  120,  103,   71,   40,
   210,  880, 1940, 3080, 3600, 3310, 2440, 1290,
    95,  240,  470,  690,  800,  745,  560,  320,
   130,  420,  890, 1310, 1500, 1385, 1020,  580,
    18,   47,   88,  137,  180,  158,  112,   63,
   260, 1010, 2180, 3390, 3900, 3620, 2700, 1450,
   160,  530, 1130, 1720, 2000, 1840, 1360,  770,
    70,  185,  355,  520,  600,  555,  415,  235
};
#define RESULT_MASK  63

/* ------------------------------------------------------------------ */
/* Core-side work.                                                     */

/* Marshal a sensor frame into the DMA-visible staging buffer: a short
   header, then 12-bit samples packed big-endian into byte pairs, then a
   trailing checksum. Byte-level packing like this is a large part of what
   a driver actually spends its cycles on. */
static uint32_t
pack_frame (uint8_t *buf, uint32_t nsamp, uint32_t seq)
{
  uint32_t i, s, sum = 0;

  buf[0] = (uint8_t) (seq >> 8);
  buf[1] = (uint8_t) seq;
  buf[2] = (uint8_t) (nsamp >> 8);
  buf[3] = (uint8_t) nsamp;

  for (i = 0; i < nsamp; i++)
    {
      s = sample_tab[(seq + i) & 31];
      buf[4 + 2 * i] = (uint8_t) (s >> 8);
      buf[5 + 2 * i] = (uint8_t) (s & 0xFF);
      sum += s;
    }

  buf[4 + 2 * nsamp] = (uint8_t) (sum >> 8);
  buf[5 + 2 * nsamp] = (uint8_t) sum;

  return 6 + 2 * nsamp;
}

static uint32_t
ring_used (void)
{
  return (ring_head - ring_tail) & RING_MASK;
}

/* Build a descriptor, point the engine at it, and ring the doorbell. */
static int
submit (uint32_t chan, uint32_t nsamp, uint32_t seq)
{
  uint32_t slot, bytes;
  desc_t *d;

  if (ring_used () >= RING_MASK)
    {
      stat_full++;
      return 0;
    }

  slot = ring_head;
  d = &ring[slot];

  bytes = pack_frame (src_buf[slot], nsamp, seq);

  d->src = (uint32_t) (uintptr_t) &src_buf[slot][0];
  d->dst = (uint32_t) (uintptr_t) &res_buf[slot][0];
  d->next = (uint32_t) (uintptr_t) &ring[(slot + 1) & RING_MASK];
  d->len_flags = DESC_LEN (bytes) | DESC_CHAN (chan)
    | DESC_MODE (seq & 3) | DESC_IRQ;
  d->status = 0;
  d->seq = seq;

  accel_regs[REG_DESC] = d->next;
  accel_regs[REG_SRC] = d->src;
  accel_regs[REG_DST] = d->dst;
  accel_regs[REG_LEN] = bytes;
  accel_regs[REG_CFG] = CFG_MODE (seq & 0xF) | CFG_PRIO (chan & 7)
    | CFG_BURST (2) | CFG_IRQEN | CFG_CHAN (chan) | CFG_LEN (bytes);
  accel_regs[REG_DOORBELL] = 1u << chan;

  ring_head = (slot + 1) & RING_MASK;
  hw_inflight++;
  return 1;
}

/* Retire every finished descriptor and do the light post-processing the
   results need before the application layer sees them. */
static uint32_t
pop_done (void)
{
  uint32_t popped = 0;

  while (ring_used () != 0)
    {
      uint32_t slot = ring_tail;
      desc_t *d = &ring[slot];
      uint32_t bytes, chan, level, avg, mv, rep, i;
      uint32_t local_sum = 0, local_max = 0;

      if ((d->status & DESC_DONE) == 0)
	break;

      bytes = d->status & 0xFFFF;
      chan = (d->len_flags >> 16) & 0xF;

      for (i = 0; i < RES_PER_DESC; i++)
	{
	  uint32_t r = res_buf[slot][i];

	  local_sum += r;
	  if (r > local_max)
	    local_max = r;
	  if (r > stat_max)
	    stat_max = r;
	  if (r < stat_min)
	    stat_min = r;
	}

      stat_sum += local_sum;
      stat_results += RES_PER_DESC;
      stat_bytes += bytes;
      stat_frames++;

      /* Running average, then a conversion to engineering units. Both
         divisors are runtime values, so these stay real divides rather
         than collapsing into a multiply-and-shift. */
      avg = stat_sum / stat_results;
      mv = (local_max * 3300u) / cal_div;
      stat_acc_mv += mv;

      /* Threshold decision on the scaled peak. */
      if (mv > 2400)
	level = 3;
      else if (mv > 900)
	level = 2;
      else if (mv > 200)
	level = 1;
      else
	level = 0;
      if (level == 3)
	stat_over++;

      /* Pack a compact report for the application layer: channel, level,
         and the averaged magnitude in one word. */
      rep = (chan << 28) | (level << 26) | ((d->seq & 0x3F) << 20)
	| (avg & 0xFFFFF);

      if (((out_head + 1) & OUT_MASK) == out_tail)
	{
	  out_tail = (out_tail + 1) & OUT_MASK;	/* overwrite oldest */
	  out_dropped++;
	}
      out_ring[out_head] = rep;
      out_head = (out_head + 1) & OUT_MASK;

      /* Recalibrate occasionally, so cal_div keeps changing. */
      if ((stat_frames & 0x1F) == 0)
	cal_div = 3800 + (avg & 0x1FF);

      d->status = 0;
      ring_tail = (slot + 1) & RING_MASK;
      popped++;
    }

  return popped;
}

/* Application layer, running outside interrupt context: drain the report
   queue and fold each report into the running summary. Something has to
   consume the queue -- left undrained it just overflows, and the
   overwrite-oldest path is not where a real system spends its cycles. */
static void
drain_reports (void)
{
  while (out_tail != out_head)
    {
      uint32_t rep = out_ring[out_tail];
      uint32_t chan = (rep >> 28) & (NCHAN - 1);
      uint32_t level = (rep >> 26) & 3;

      level_count[level]++;
      chan_mag[chan] += rep & 0xFFFFF;
      if (level >= 2)
	chan_events[chan]++;

      out_tail = (out_tail + 1) & OUT_MASK;
    }
}

/* Interrupt-time entry: acknowledge the engine, then retire whatever has
   finished. Also called when the driver has to wait for queue space, which
   is how a small polled driver without a real ISR context ends up
   structured. */
static uint32_t
service_engine (void)
{
  uint32_t st = accel_regs[REG_STATUS];

  if (st & STAT_IRQ)
    accel_regs[REG_STATUS] = st & ~STAT_IRQ;

  return pop_done ();
}

/* ------------------------------------------------------------------ */
/* Hardware stand-in. Keep this as short as possible -- see file header. */

static void
accel_tick (void)
{
  desc_t *d;
  uint32_t slot, i;

  if (hw_inflight == 0)
    return;

  slot = hw_cursor;
  d = &ring[slot];

  for (i = 0; i < RES_PER_DESC; i++)
    res_buf[slot][i] = result_tab[(hw_res + i) & RESULT_MASK];

  d->status = DESC_DONE | (d->len_flags & 0xFFFF);

  hw_res = (hw_res + RES_PER_DESC) & RESULT_MASK;
  hw_seq++;
  hw_cursor = (slot + 1) & RING_MASK;
  hw_inflight--;
  accel_regs[REG_STATUS] |= STAT_IRQ | STAT_BUSY;
}

/* ------------------------------------------------------------------ */

void
bench_init (void)
{
  uint32_t i, j;

  for (i = 0; i < REG_COUNT; i++)
    accel_regs[i] = 0;

  for (i = 0; i < RING_N; i++)
    {
      ring[i].src = 0;
      ring[i].dst = 0;
      ring[i].next = 0;
      ring[i].len_flags = 0;
      ring[i].status = 0;
      ring[i].seq = 0;
      for (j = 0; j < RES_PER_DESC; j++)
	res_buf[i][j] = 0;
    }

  ring_head = ring_tail = 0;
  hw_cursor = hw_inflight = hw_seq = hw_res = 0;

  stat_frames = stat_bytes = stat_results = 0;
  stat_sum = 0;
  stat_min = 0xFFFF;
  stat_max = 0;
  stat_over = stat_full = stat_acc_mv = 0;
  cal_div = 4096;

  for (i = 0; i < OUT_N; i++)
    out_ring[i] = 0;
  out_head = out_tail = out_dropped = 0;

  for (i = 0; i < 4; i++)
    level_count[i] = 0;
  for (i = 0; i < NCHAN; i++)
    chan_mag[i] = chan_events[i] = 0;

  /* Bring the engine up: reset, then enable with descriptor chaining and
     all channel interrupts unmasked. */
  accel_regs[REG_CTRL] = CTRL_RESET;
  accel_regs[REG_CTRL] = CTRL_ENABLE | CTRL_CHAIN;
  accel_regs[REG_IRQMASK] = (1u << NCHAN) - 1u;
}

uint32_t
bench_run (void)
{
  uint32_t frame, chk, i;

  for (frame = 0; frame < FRAMES; frame++)
    {
      uint32_t chan = frame & (NCHAN - 1);
      uint32_t nsamp = 32 + ((frame * 13) & 63);

      /* Keep the engine a fixed number of descriptors deep. Servicing
         completions to make room is the core's job. */
      while (ring_used () >= QUEUE_DEPTH)
	{
	  accel_tick ();
	  service_engine ();
	}

      if (!submit (chan, nsamp, frame))
	{
	  accel_tick ();
	  service_engine ();
	  continue;
	}

      /* The engine runs asynchronously; the core picks completions up on
         the interrupt that follows. */
      if ((frame & 1) == 1)
	{
	  accel_tick ();
	  service_engine ();
	}

      /* Back at task level, hand the accumulated reports on. */
      if ((frame & 7) == 7)
	drain_reports ();
    }

  while (ring_used () != 0)
    {
      accel_tick ();
      service_engine ();
    }
  drain_reports ();

  /* Checksum: state only, and nothing address-derived -- pointer values
     differ between the ARM target and the host validation build. */
  chk = stat_frames;
  chk = chk * 1000003u + stat_bytes;
  chk = chk * 1000003u + stat_results;
  chk = chk * 1000003u + stat_sum;
  chk = chk * 1000003u + stat_min;
  chk = chk * 1000003u + stat_max;
  chk = chk * 1000003u + stat_over;
  chk = chk * 1000003u + stat_full;
  chk = chk * 1000003u + stat_acc_mv;
  chk = chk * 1000003u + cal_div;
  chk = chk * 1000003u + out_head;
  chk = chk * 1000003u + out_tail;
  chk = chk * 1000003u + out_dropped;
  chk = chk * 1000003u + hw_seq;

  for (i = 0; i < 4; i++)
    chk = chk * 1000003u + level_count[i];
  for (i = 0; i < NCHAN; i++)
    {
      chk = chk * 1000003u + chan_mag[i];
      chk = chk * 1000003u + chan_events[i];
    }
  for (i = 0; i < OUT_N; i++)
    chk = chk * 1000003u + out_ring[i];

  return chk;
}

uint32_t
bench_expected (void)
{
  return 0x1c76a897u;
}
