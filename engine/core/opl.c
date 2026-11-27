/* An OPL2 (the AdLib's YM3812) of the engine's own (engine.h, e_opl_): enough of the chip for
 * the original's music as its driver plays it, in integers, as the Jaguar's DSP does the same
 * (jaguar/opl_dsp.s, to come): this is its reference.
 *
 * Nine channels of two operators. An operator: a phase stepping by its channel's frequency
 * (F-number << block) times its multiple; its wave (sine, half, absolute, quarter: register E0h
 * with waveform select on) read from a 1024-entry table at the phase (the carrier's moved by
 * the modulator's output, the modulator's by its own last two outputs, the channel's
 * feedback); times a gain from its attenuation in 0.1875 dB steps: its envelope, total level
 * (40h, 0.75 dB), key scaling (40h's top bits, by octave and F-number) and tremolo. The
 * envelope: attack (exponential), decay to the sustain level, sustain (held, or for a
 * percussive one decaying on at the release rate), release; each rate 4R plus the key's offset
 * (with key scale rate, all of it; else a quarter), stepped every 16 samples, the gain
 * gliding there across the block (a step at once clicks). Key-on restarts the phase and the
 * attack. Connection 1 adds the modulator to the output. Tremolo (1 dB at 3.7
 * Hz) and vibrato (7 cents at 6.1 Hz) where 20h asks. Not here: rhythm mode, the deeper tremolo
 * and vibrato, note select (the original's music uses none of them).
 */
#include <string.h>

#include "engine.h"

#define RATE E_OPL_RATE
#define ENV_BLOCK 16
#define CHANNELS 9

static const uint8_t slot_mod[CHANNELS] = { 0, 1, 2, 8, 9, 10, 16, 17, 18 };
static const uint8_t mult_x2[16] = { 1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30 };
/* key scaling at block 7 by the F-number's top 4 bits (0.1875 dB units: 0 to 21 dB), 3 dB less
 * an octave down */
static const uint8_t ksl_base[16] = { 0, 48, 64, 74, 80, 86, 90, 94, 96, 100, 102, 104, 106, 108, 110, 112 };

enum { OFF, ATTACK, DECAY, SUSTAIN, RELEASE };

static struct Op {
	uint8_t r20, r40, r60, r80, re0;
	uint8_t state;
	int32_t env;                /* attenuation, 0-511 << 16 */
	uint32_t phase;
	int16_t out[2];             /* its last two outputs (feedback) */
	int32_t g, dg;              /* its gain << 4 now, and a sample on across the block */
} ops[18];

static struct Channel {
	uint16_t fnum;
	uint8_t block, key, rc0;
} chans[CHANNELS];

static uint8_t wse;             /* 01h bit 5: the waveforms besides the sine */
static int16_t sine[1024];
static uint16_t gain32[32];     /* 65535 * 2^(-k/32) */
static int32_t decay_step[64], attack_keep[64];
static int env_left;
static uint32_t lfo_am, lfo_vib;
static int tables_ok;

/* integer sine for the table (a quarter's worth by a cubic-free series is not needed: the
 * table is made once, by the backend or here, in floating point avoided) */
static void make_tables(void)
{
	/* sine by the recurrence of a rotation (fixed point, 30 bits): exact enough for 13 bits */
	int64_t c = 1073741824, s = 0;
	const int64_t cs = 1073721611, sn = 6588387;    /* cos and sin of 2 pi / 1024, << 30 */
	for (int i = 0; i < 1024; i++) {
		sine[i] = (int16_t) ((s * 4095 + (s >= 0 ? 536870912 : -536870912)) >> 30);
		int64_t nc = (c * cs - s * sn) >> 30, ns = (s * cs + c * sn) >> 30;
		c = nc;
		s = ns;
	}
	/* 2^(-k/32) by repeated multiplication by 2^(-1/32) (<< 16) */
	uint32_t g = 65535;
	for (int k = 0; k < 32; k++) {
		gain32[k] = (uint16_t) g;
		g = (uint32_t) ((uint64_t) g * 64132 >> 16);
	}
	/* rates: decay and release over 96 dB (512 steps) in 39.28 s at rate 4 (R 1), half as long
	 * each R, a quarter shorter each step of the offset; attack from silence in 2.826 s at R 1,
	 * likewise, as a fall to a fraction of the attenuation left each block */
	for (int r = 0; r < 64; r++) {
		int R = r >> 2, f = r & 3;
		if (R == 0) {
			decay_step[r] = 0;
			attack_keep[r] = 65536;
			continue;
		}
		/* blocks for the whole decay: 39.28 s * RATE / 16 / 2^(R-1) / (1 + f/4) */
		uint64_t blocks = (uint64_t) 3928 * RATE / 100 / ENV_BLOCK * 4 / (uint64_t) (4 + f) >> (R - 1);
		decay_step[r] = blocks ? (int32_t) (((uint64_t) 512 << 16) / blocks) : 512 << 16;
		/* attack: keep a fraction k each block, so that 511 falls to 1 in its time: k^n = 1/511 */
		uint64_t ab = (uint64_t) 2826 * RATE / 1000 / ENV_BLOCK * 4 / (uint64_t) (4 + f) >> (R - 1);
		if (R >= 15 || ab == 0)
			attack_keep[r] = 0;
		else {
			/* k = 511^(-1/ab), by bisection on k^ab (16.16) */
			uint32_t lo = 0, hi = 65536;
			for (int it = 0; it < 20; it++) {
				uint32_t mid = (lo + hi) / 2, p = 65536;
				for (uint64_t n = ab, b = mid; n; n >>= 1) {
					if (n & 1)
						p = (uint32_t) ((uint64_t) p * b >> 16);
					b = b * b >> 16;
				}
				if (p * 511u > 65536u)
					hi = mid;
				else
					lo = mid;
			}
			attack_keep[r] = (int32_t) lo;
		}
	}
	tables_ok = 1;
}

void e_opl_reset(void)
{
	if (!tables_ok)
		make_tables();
	memset(ops, 0, sizeof ops);
	memset(chans, 0, sizeof chans);
	for (int k = 0; k < 18; k++) {
		ops[k].env = 511 << 16;
		ops[k].state = OFF;
	}
	wse = 0;
	env_left = ENV_BLOCK;
	lfo_am = lfo_vib = 0;
}

/* The slot (0-21, its register's low bits) as an operator (0-17: channel * 2, + 1 the carrier). */
static int op_of_slot(int s)
{
	for (int c = 0; c < CHANNELS; c++) {
		if (s == slot_mod[c])
			return c * 2;
		if (s == slot_mod[c] + 3)
			return c * 2 + 1;
	}
	return -1;
}

static void key(int c, int on)
{
	for (int k = c * 2; k <= c * 2 + 1; k++)
		if (on) {
			ops[k].state = ATTACK;
			ops[k].phase = 0;
		} else if (ops[k].state != OFF)
			ops[k].state = RELEASE;
}

void e_opl_write(int reg, int value)
{
	int o;

	reg &= 0xff;
	value &= 0xff;
	if (reg == 0x01)
		wse = (uint8_t) (value & 0x20);
	else if (reg >= 0x20 && reg < 0xa0 && (o = op_of_slot(reg & 0x1f)) >= 0) {
		switch (reg & 0xe0) {
		case 0x20: ops[o].r20 = (uint8_t) value; break;
		case 0x40: ops[o].r40 = (uint8_t) value; break;
		case 0x60: ops[o].r60 = (uint8_t) value; break;
		case 0x80: ops[o].r80 = (uint8_t) value; break;
		}
	} else if (reg >= 0xe0 && reg < 0xf6 && (o = op_of_slot(reg - 0xe0)) >= 0)
		ops[o].re0 = (uint8_t) value;
	else if (reg >= 0xa0 && reg <= 0xa8)
		chans[reg - 0xa0].fnum = (uint16_t) ((chans[reg - 0xa0].fnum & 0x300) | value);
	else if (reg >= 0xb0 && reg <= 0xb8) {
		struct Channel *ch = &chans[reg - 0xb0];
		int on = (value >> 5) & 1;
		ch->fnum = (uint16_t) ((ch->fnum & 0xff) | (value & 3) << 8);
		ch->block = (uint8_t) ((value >> 2) & 7);
		if (on != ch->key)
			key(reg - 0xb0, on);
		ch->key = (uint8_t) on;
	} else if (reg >= 0xc0 && reg <= 0xc8)
		chans[reg - 0xc0].rc0 = (uint8_t) value;
}

static int gain_of(const struct Op *op, const struct Channel *ch);

/* The envelopes a block on. */
static void envelopes(void)
{
	for (int c = 0; c < CHANNELS; c++) {
		const struct Channel *ch = &chans[c];
		int keycode = ch->block << 1 | (ch->fnum >> 9 & 1);
		for (int k = c * 2; k <= c * 2 + 1; k++) {
			struct Op *op = &ops[k];
			int rof = op->r20 & 0x10 ? keycode : keycode >> 2, R;
			switch (op->state) {
			case ATTACK:
				R = op->r60 >> 4;
				if (R == 0)
					break;
				if (4 * R + rof >= 60)
					op->env = 0;
				else
					op->env = (int32_t) ((int64_t) op->env * attack_keep[4 * R + rof > 63 ? 63 : 4 * R + rof] >> 16);
				if (op->env < (1 << 16)) {
					op->env = 0;
					op->state = DECAY;
				}
				break;
			case DECAY: {
				int32_t sl = (op->r80 >> 4) == 15 ? 496 << 16 : (op->r80 >> 4) * 16 << 16;
				R = op->r60 & 15;
				op->env += R ? decay_step[4 * R + rof > 63 ? 63 : 4 * R + rof] : 0;
				if (op->env >= sl) {
					op->env = sl;
					op->state = SUSTAIN;
				}
				break;
			}
			case SUSTAIN:
				if (op->r20 & 0x20)
					break;      /* (held) */
				/* fall through: percussive, on at the release rate */
			case RELEASE:
				R = op->r80 & 15;
				op->env += R ? decay_step[4 * R + rof > 63 ? 63 : 4 * R + rof] : 0;
				if (op->env >= 511 << 16) {
					op->env = 511 << 16;
					if (op->state == RELEASE)
						op->state = OFF;
				}
				break;
			}
		}
	}
	/* the gains each operator glides to by the block's end */
	for (int k = 0; k < 18; k++) {
		struct Op *op = &ops[k];
		int32_t to = op->state == OFF ? 0 : gain_of(op, &chans[k >> 1]) << 4;
		op->dg = (to - op->g) / ENV_BLOCK;
		op->g = to - op->dg * ENV_BLOCK;
	}
	/* the LFOs: tremolo 3.7 Hz, vibrato 6.1 Hz, as 16-bit phases a block on */
	lfo_am += (uint32_t) (65536ULL * 37 * ENV_BLOCK / 10 / RATE);
	lfo_vib += (uint32_t) (65536ULL * 61 * ENV_BLOCK / 10 / RATE);
}

static int gain_of(const struct Op *op, const struct Channel *ch)
{
	int att = op->env >> 16;
	att += (op->r40 & 63) * 4;
	if (op->r40 >> 6) {
		int k = ksl_base[ch->fnum >> 6] - 16 * (7 - ch->block);
		if (k > 0)      /* (the table at 3 dB an octave; then 1.5, then 6) */
			att += (op->r40 >> 6) == 1 ? k : (op->r40 >> 6) == 2 ? k >> 1 : k << 1;
	}
	if (op->r20 & 0x80) {
		/* tremolo: a triangle 0-5 steps (1 dB) */
		int t = (int) (lfo_am >> 8 & 255);
		att += (t < 128 ? t : 255 - t) * 5 >> 7;
	}
	if (att >= 512)
		return 0;
	return gain32[att & 31] >> (att >> 5);
}

static inline int wave(int w, uint32_t i)
{
	int s;

	i &= 1023;
	s = sine[i];
	switch (w) {
	case 1: return i < 512 ? s : 0;
	case 2: return s < 0 ? -s : s;
	case 3: return i & 256 ? 0 : (s < 0 ? -s : s);
	}
	return s;
}

static uint32_t step_of(const struct Op *op, const struct Channel *ch)
{
	/* (F-number << block) * multiple * 49716 Hz / 2^20, as a 2^32 phase at RATE */
	uint32_t f = (uint32_t) ch->fnum * mult_x2[op->r20 & 15] * 6372u << ch->block;
	if (op->r20 & 0x40) {
		/* vibrato: about 7 cents either way */
		int t = (int) (lfo_vib >> 8 & 255), tri = t < 64 ? t : t < 192 ? 128 - t : t - 256;
		f = (uint32_t) ((int32_t) f + (int32_t) (f >> 12) * tri / 4);
	}
	return f;
}

void e_opl_render(int16_t *out, int n)
{
	if (!tables_ok)
		make_tables();
	for (int i = 0; i < n; i++) {
		if (--env_left <= 0) {
			env_left = ENV_BLOCK;
			envelopes();
		}
		int32_t mix = 0;
		for (int c = 0; c < CHANNELS; c++) {
			const struct Channel *ch = &chans[c];
			struct Op *m = &ops[c * 2], *car = &ops[c * 2 + 1];
			if (m->state == OFF && car->state == OFF)
				continue;
			int fb = ch->rc0 >> 1 & 7;
			int32_t fbm = fb ? (m->out[0] + m->out[1]) >> (9 - fb) : 0;
			m->g += m->dg;
			car->g += car->dg;
			m->phase += step_of(m, ch);
			car->phase += step_of(car, ch);
			int mo = wave(wse ? m->re0 & 3 : 0, (m->phase >> 22) + (uint32_t) fbm) * (m->g >> 4) >> 16;
			m->out[1] = m->out[0];
			m->out[0] = (int16_t) mo;
			int co;
			if (ch->rc0 & 1) {
				co = wave(wse ? car->re0 & 3 : 0, car->phase >> 22) * (car->g >> 4) >> 16;
				mix += mo + co;
			} else {
				co = wave(wse ? car->re0 & 3 : 0, (car->phase >> 22) + (uint32_t) (mo >> 1)) * (car->g >> 4) >> 16;
				mix += co;
			}
		}
		out[i] = (int16_t) (mix > 32767 ? 32767 : mix < -32768 ? -32768 : mix);
	}
}
