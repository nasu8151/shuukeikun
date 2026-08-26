/* pidctl -- four-channel fixed-point PID control loop with threshold /
 * hysteresis supervision.
 *
 * This is the "control" row of the target-domain table in CLAUDE.md: PID,
 * state machines and threshold decisions, the main job left to the soft
 * core. A simple first-order plant model closes the loop so the controller
 * sees a realistic mix of regimes -- large errors right after a setpoint
 * step, small ones once it has settled -- instead of a single operating
 * point.
 *
 * Everything is integer. Signals are raw 12-bit ADC counts, gains are
 * Q8.8, and every product is sized to stay inside 32 bits on purpose: a
 * soft core of this class would have its fixed-point formats chosen so
 * that no 64-bit intermediate (and certainly no soft float) is ever
 * needed. Using float here would measure libgcc's soft-float internals
 * rather than the application's own arithmetic.
 */

#include "harness.h"

#define NCH        4
#define ADC_MAX    4095		/* 12-bit sensor */
#define PWM_MAX    1023		/* 10-bit actuator */
/* Control ticks per run. Sized so the sample count lands near the middle
   of the existing corpus (~1.1M ALU results); analyze.py sums raw counts
   across benchmarks, so a workload far larger than its peers would skew
   the combined distribution. The setpoint schedule cycles every 2048
   ticks, so this is five passes through it. */
#define TICKS      10000

/* Supervision thresholds, in ADC counts of absolute error. The WARN/ALARM
   pairs differ on the way up and on the way down: that gap is the
   hysteresis. */
#define WARN_HI    600
#define WARN_LO    400
#define ALARM_HI   1200
#define ALARM_LO   900
#define DEBOUNCE_N 3

enum
{ ST_OK = 0, ST_WARN = 1, ST_ALARM = 2 };

typedef struct
{
  /* controller */
  int32_t kp, ki, kd;		/* Q8.8 */
  int32_t integ;
  int32_t integ_max;		/* anti-windup clamp */
  int32_t prev_err;
  int32_t out;			/* 0..PWM_MAX */

  /* sensor calibration; the divisor is per-channel state rather than a
     literal, so this really does become a divide instruction instead of
     the multiply-and-shift a constant divisor would compile to */
  int32_t span;
  int32_t full_scale_mv;
  int32_t reading_mv;

  /* plant model */
  int32_t y;			/* measured value, 0..ADC_MAX */
  int32_t lag;			/* Q8.8 */
  int32_t gain;			/* Q8.8 */
  int32_t load;			/* disturbance, in counts */

  /* supervision */
  int32_t setpoint;
  uint32_t state;
  uint32_t debounce;
  uint32_t warn_count;
  uint32_t alarm_count;
  uint32_t sat_count;
} channel_t;

static channel_t ch[NCH];
static uint32_t tickno;

/* Setpoint schedule, as a supervisory layer would hand it down. The steps
   are what drive the loop through its transients. */
static const int16_t sp_profile[8] = {
  512, 2048, 2048, 3600, 1200, 1200, 300, 2800
};

/* Sensor noise and load disturbance. Deliberately table-driven: an
   xorshift-style PRNG would be the obvious choice, but it emits full-width
   32-bit words that no control loop ever actually handles, and they would
   show up in the histogram as a spurious lump at 32 bits. */
static const int8_t noise_tab[16] = {
  1, -2, 0, 3, -1, 2, -3, 1, 0, -1, 2, -2, 3, 0, -3, 1
};

static const int8_t load_tab[8] = {
  0, 12, 40, 8, -20, 60, 4, -8
};

void
bench_init (void)
{
  /* Tuned so that kp * (a full-scale error) lands near the actuator range:
     a setpoint step saturates the output briefly and then comes out of it,
     which is the behaviour worth measuring. Gains an order of magnitude
     higher leave the loop clipped most of the time, doing no arithmetic. */
  static const int16_t kp_tab[NCH] = { 0x0050, 0x0030, 0x0080, 0x0020 };
  static const int16_t ki_tab[NCH] = { 0x0004, 0x0008, 0x0002, 0x000C };
  static const int16_t kd_tab[NCH] = { 0x0018, 0x0008, 0x0030, 0x0000 };
  static const int16_t lag_tab[NCH] = { 0x0030, 0x0018, 0x0060, 0x000C };
  static const int16_t gain_tab[NCH] = { 0x0400, 0x0380, 0x0480, 0x0300 };
  uint32_t i;

  tickno = 0;
  for (i = 0; i < NCH; i++)
    {
      channel_t *c = &ch[i];

      c->kp = kp_tab[i];
      c->ki = ki_tab[i];
      c->kd = kd_tab[i];
      c->integ = 0;
      /* Anti-windup clamp, a few times the actuator range: high enough
         that the integrator can hold a steady-state offset, low enough
         that it actually engages during a step. */
      c->integ_max = 4 * PWM_MAX;
      c->prev_err = 0;
      c->out = 0;

      c->span = 4096 - (int32_t) i * 3;	/* per-unit calibration */
      c->full_scale_mv = 3300;
      c->reading_mv = 0;

      c->y = 0;
      c->lag = lag_tab[i];
      c->gain = gain_tab[i];
      c->load = 0;

      c->setpoint = sp_profile[0];
      c->state = ST_OK;
      c->debounce = 0;
      c->warn_count = 0;
      c->alarm_count = 0;
      c->sat_count = 0;
    }
}

/* One control tick for one channel: read, convert, PID, actuate, then
   supervise. */
static void
channel_tick (channel_t *c, uint32_t t)
{
  int32_t err, p, d, u, aerr, drive, target;

  /* Sensor path: raw counts to millivolts. Runtime divisor, see span. */
  c->reading_mv = (c->y * c->full_scale_mv) / c->span;

  /* PID. The >> 8 renormalises the Q8.8 gain back out of each product. */
  err = c->setpoint - c->y;
  p = (c->kp * err) >> 8;

  c->integ += (c->ki * err) >> 8;
  if (c->integ > c->integ_max)
    c->integ = c->integ_max;
  else if (c->integ < -c->integ_max)
    c->integ = -c->integ_max;

  d = (c->kd * (err - c->prev_err)) >> 8;
  c->prev_err = err;

  u = p + c->integ + d;
  if (u < 0)
    {
      u = 0;
      c->sat_count++;
    }
  else if (u > PWM_MAX)
    {
      u = PWM_MAX;
      c->sat_count++;
    }
  c->out = u;

  /* Plant: first-order lag towards the commanded level, offset by the load
     disturbance, with a little sensor noise on top. */
  drive = (u * c->gain) >> 8;
  target = drive - c->load;
  c->y += ((target - c->y) * c->lag) >> 8;
  c->y += noise_tab[t & 15];
  if (c->y < 0)
    c->y = 0;
  else if (c->y > ADC_MAX)
    c->y = ADC_MAX;

  /* Threshold supervision: hysteresis on the level, plus a debounce
     counter so a single noisy sample cannot flip the state. */
  aerr = err < 0 ? -err : err;
  switch (c->state)
    {
    case ST_OK:
      if (aerr > WARN_HI)
	{
	  if (++c->debounce >= DEBOUNCE_N)
	    {
	      c->state = ST_WARN;
	      c->debounce = 0;
	      c->warn_count++;
	    }
	}
      else
	c->debounce = 0;
      break;

    case ST_WARN:
      if (aerr > ALARM_HI)
	{
	  if (++c->debounce >= DEBOUNCE_N)
	    {
	      c->state = ST_ALARM;
	      c->debounce = 0;
	      c->alarm_count++;
	    }
	}
      else if (aerr < WARN_LO)
	{
	  if (++c->debounce >= DEBOUNCE_N)
	    {
	      c->state = ST_OK;
	      c->debounce = 0;
	    }
	}
      else
	c->debounce = 0;
      break;

    default:			/* ST_ALARM */
      if (aerr < ALARM_LO)
	{
	  if (++c->debounce >= DEBOUNCE_N)
	    {
	      c->state = ST_WARN;
	      c->debounce = 0;
	    }
	}
      else
	c->debounce = 0;
      break;
    }
}

uint32_t
bench_run (void)
{
  uint32_t t, i, chk;

  for (t = 0; t < TICKS; t++)
    {
      /* New setpoint and load every 256 ticks, staggered per channel. */
      if ((t & 0xFF) == 0)
	{
	  for (i = 0; i < NCH; i++)
	    {
	      ch[i].setpoint = sp_profile[((t >> 8) + i) & 7];
	      ch[i].load = load_tab[((t >> 8) + i) & 7];
	    }
	}

      for (i = 0; i < NCH; i++)
	channel_tick (&ch[i], t + i);

      tickno++;
    }

  /* Checksum the final state, once. Folding it in per tick would inject
     the mixing constant's own wide products into exactly the classes this
     benchmark is meant to characterise. */
  chk = tickno;
  for (i = 0; i < NCH; i++)
    {
      const channel_t *c = &ch[i];

      chk = chk * 1000003u + (uint32_t) c->y;
      chk = chk * 1000003u + (uint32_t) c->out;
      chk = chk * 1000003u + (uint32_t) c->integ;
      chk = chk * 1000003u + (uint32_t) c->reading_mv;
      chk = chk * 1000003u + c->state;
      chk = chk * 1000003u + c->warn_count;
      chk = chk * 1000003u + c->alarm_count;
      chk = chk * 1000003u + c->sat_count;
    }
  return chk;
}

uint32_t
bench_expected (void)
{
  return 0xb5e38be9u;
}
