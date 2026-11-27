/* An OPL2 (the AdLib's YM3812) of the engine's own (engine.h, e_opl_): enough of the chip for
 * the original's music as its driver plays it, in integers, step for step as the Jaguar's DSP
 * does it (jaguar/opl_dsp.s): this is its reference, and the host's.
 *
 * Nine channels of two operators. An operator: a phase stepping by its channel's frequency
 * (F-number << block) times its multiple; its wave (sine, half, absolute, quarter: register E0h
 * with waveform select on) read from a half sine at the phase (the carrier's moved by the
 * modulator's output, the modulator's by its own last two outputs, the channel's feedback);
 * times a gain from its attenuation in 0.1875 dB steps: its envelope, total level (40h, 0.75
 * dB), key scaling (40h's top bits, by octave and F-number) and tremolo. The envelope: attack
 * (exponential), decay to the sustain level, sustain (held, or for a percussive one decaying on
 * at the release rate), release; each rate 4R plus the key's offset (with key scale rate, all of
 * it; else a quarter). Key-on restarts the phase and the attack. Connection 1 adds the
 * modulator to the output. Tremolo (1 dB at 3.7 Hz) and vibrato (7 cents at 6.1 Hz) where 20h
 * asks. Not here: rhythm mode, the deeper tremolo and vibrato, note select (the original's music
 * uses none of them).
 *
 * It goes a block (E_OPL_BLOCK samples) at a time: at the block's start the stream's ticks due
 * (60 a second) are written, the envelopes go a step, and each operator's step, wave and the
 * gain it glides to across the block (a step at once would click) are set; then the samples.
 */
#include <string.h>

#include "engine.h"

#define RATE E_OPL_RATE
#define BLOCK E_OPL_BLOCK
#define CHANNELS 9

static const uint8_t slot_mod[CHANNELS] = { 0, 1, 2, 8, 9, 10, 16, 17, 18 };
static const uint8_t mult_x2[16] = { 1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30 };
/* key scaling at block 7 by the F-number's top 4 bits (0.1875 dB units: 0 to 21 dB), 3 dB less
 * an octave down */
static const uint8_t ksl_base[16] = { 0, 48, 64, 74, 80, 86, 90, 94, 96, 100, 102, 104, 106, 108, 110, 112 };
/* by waveform: the phase bits that silence it, and that turn it over */
static const uint16_t zero_mask[4] = { 0, 512, 0, 256 }, neg_mask[4] = { 512, 512, 0, 0 };

enum { OFF, ATTACK, DECAY, SUSTAIN, RELEASE };

static struct Op {
	uint32_t phase, step;
	int32_t g, dg;              /* its gain << 4 now, and a sample on across the block */
	uint32_t zmask, nmask;
	int32_t env;                /* attenuation, 0-511 << 16 */
	uint8_t state, r20, r40, r60, r80, re0;
	int32_t out0, out1;         /* (the modulator's) its last two outputs */
} ops[18];

static struct Channel {
	uint16_t fnum;
	uint8_t block, key, rc0, live;
	int32_t fbmul;              /* 2 ^ feedback, 0 for none */
	int32_t modmask;            /* -1: the modulator moves the carrier; 0: it is added */
} chans[CHANNELS];

static uint8_t wse;             /* 01h bit 5: the waveforms besides the sine */
static struct EOplTables tables;
static uint32_t lfo_am, lfo_vib;
static int tables_ok;

static void make_tables(void)
{
	/* half of a 1024-step sine: a quarter and one by the recurrence of a rotation (fixed point,
	 * 30 bits: exact enough for 13 bits), the rest its mirror */
	int64_t c = 1073741824, s = 0;
	const int64_t cs = 1073721611, sn = 6588387;    /* cos and sin of 2 pi / 1024, << 30 */
	for (int i = 0; i <= 256; i++) {
		tables.hsine[i] = tables.hsine[(512 - i) & 511] = (int32_t) ((s * 4095 + 536870912) >> 30);
		int64_t nc = (c * cs - s * sn) >> 30, ns = (s * cs + c * sn) >> 30;
		c = nc;
		s = ns;
	}
	tables.hsine[0] = 0;
	/* 2^(-k/32) by repeated multiplication by 2^(-1/32) (<< 16) */
	uint32_t g = 65535;
	for (int k = 0; k < 32; k++) {
		tables.gain32[k] = (int32_t) g;
		g = (uint32_t) ((uint64_t) g * 64132 >> 16);
	}
	/* rates: decay and release over 96 dB (512 steps) in 39.28 s at rate 4 (R 1), half as long
	 * each R, a quarter shorter each step of the offset; attack from silence in 2.826 s at R 1,
	 * likewise, as a fall to a fraction of the attenuation left each block */
	for (int r = 0; r < 64; r++) {
		int R = r >> 2, f = r & 3;
		if (R == 0) {
			tables.decay_step[r] = 0;
			tables.attack_keep[r] = 65535;
			continue;
		}
		/* blocks for the whole decay: 39.28 s * RATE / 16 / 2^(R-1) / (1 + f/4) */
		uint64_t blocks = (uint64_t) 3928 * RATE / 100 / BLOCK * 4 / (uint64_t) (4 + f) >> (R - 1);
		tables.decay_step[r] = blocks ? (int32_t) (((uint64_t) 512 << 16) / blocks) : 512 << 16;
		/* attack: keep a fraction k each block, so that 511 falls to 1 in its time: k^n = 1/511 */
		uint64_t ab = (uint64_t) 2826 * RATE / 1000 / BLOCK * 4 / (uint64_t) (4 + f) >> (R - 1);
		if (R >= 15 || ab == 0)
			tables.attack_keep[r] = 0;
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
			tables.attack_keep[r] = (int32_t) (lo > 65535 ? 65535 : lo);
		}
	}
	tables_ok = 1;
}

const struct EOplTables *e_opl_tables(void)
{
	if (!tables_ok)
		make_tables();
	return &tables;
}

void e_opl_reset(void)
{
	if (!tables_ok)
		make_tables();
	memset(ops, 0, sizeof ops);
	memset(chans, 0, sizeof chans);
	for (int k = 0; k < 18; k++)
		ops[k].env = 511 << 16;
	wse = 0;
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
		int c = reg - 0xb0, on = (value >> 5) & 1;
		struct Channel *ch = &chans[c];
		ch->fnum = (uint16_t) ((ch->fnum & 0xff) | (value & 3) << 8);
		ch->block = (uint8_t) ((value >> 2) & 7);
		if (on != ch->key)
			for (int k = c * 2; k <= c * 2 + 1; k++) {
				if (on) {
					ops[k].state = ATTACK;
					ops[k].phase = 0;
				} else if (ops[k].state != OFF)
					ops[k].state = RELEASE;
			}
		ch->key = (uint8_t) on;
	} else if (reg >= 0xc0 && reg <= 0xc8)
		chans[reg - 0xc0].rc0 = (uint8_t) value;
}

/* The ticks due: each, its writes (or a wait). Short of two bytes in the ring, the rest waits
 * for the next. */
static void ticks(struct EOplStream *s)
{
	s->frac += BLOCK * 60;
	while (s->frac >= RATE) {
		s->frac -= RATE;
		if (s->ended)
			continue;
		if (s->wait > 0) {
			s->wait--;
			continue;
		}
		while (((s->wr - s->rd) & s->mask) >= 2) {
			uint8_t b = s->ring[s->rd];
			if (b == 0xff) {
				s->rd = (s->rd + 1) & s->mask;
				break;
			}
			if (b == 0xfd) {
				s->rd = (s->rd + 1) & s->mask;
				s->ended = 1;
				break;
			}
			uint8_t v = s->ring[(s->rd + 1) & s->mask];
			s->rd = (s->rd + 2) & s->mask;
			if (b == 0xfe) {
				s->wait = v - 1u;
				break;
			}
			e_opl_write(b, v);
		}
	}
}

static int32_t gain_of(const struct Op *op, const struct Channel *ch)
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
	return tables.gain32[att & 31] >> (att >> 5);
}

/* The envelopes a block on, and each operator's step, wave and gain for the block. */
static void envelopes(void)
{
	for (int c = 0; c < CHANNELS; c++) {
		struct Channel *ch = &chans[c];
		int keycode = ch->block << 1 | (ch->fnum >> 9 & 1);
		for (int k = c * 2; k <= c * 2 + 1; k++) {
			struct Op *op = &ops[k];
			int rof = op->r20 & 0x10 ? keycode : keycode >> 2, R, r;
			switch (op->state) {
			case ATTACK:
				R = op->r60 >> 4;
				if (R == 0)
					break;
				r = 4 * R + rof;
				if (r >= 60)
					op->env = 0;
				else {
					int32_t keep = tables.attack_keep[r];
					op->env = (op->env >> 16) * keep + (int32_t) ((uint32_t) (op->env & 0xffff) * (uint32_t) keep >> 16);
				}
				if (op->env < (1 << 16)) {
					op->env = 0;
					op->state = DECAY;
				}
				break;
			case DECAY: {
				int32_t sl = (op->r80 >> 4) == 15 ? 496 << 16 : (op->r80 >> 4) * 16 << 16;
				R = op->r60 & 15;
				r = 4 * R + rof;
				if (R)
					op->env += tables.decay_step[r > 63 ? 63 : r];
				if (op->env >= sl) {
					op->env = sl;
					op->state = SUSTAIN;
				}
				break;
			}
			case SUSTAIN:
				if (op->r20 & 0x20)
					break;      /* (held); percussive, on at the release rate: */
				/* fall through */
			case RELEASE:
				R = op->r80 & 15;
				r = 4 * R + rof;
				if (R)
					op->env += tables.decay_step[r > 63 ? 63 : r];
				if (op->env >= 511 << 16) {
					op->env = 511 << 16;
					if (op->state == RELEASE)
						op->state = OFF;
				}
				break;
			}
			/* the gain it glides to by the block's end */
			int32_t to = op->state == OFF ? 0 : gain_of(op, ch) << 4, d = to - op->g;
			op->dg = d < 0 ? -(-d >> 4) : d >> 4;
			op->g = to - op->dg * BLOCK;
			/* its step: (F-number << block) * multiple * 49716 Hz / 2^20, as a 2^32 phase at RATE */
			int32_t base = (int32_t) (ch->fnum * mult_x2[op->r20 & 15] * 6372u);
			if (op->r20 & 0x40) {
				/* vibrato: about 7 cents either way */
				int t = (int) (lfo_vib >> 8 & 255), tri = t < 64 ? t : t < 192 ? 128 - t : t - 256;
				base += (base >> 13) * tri >> 1;
			}
			op->step = (uint32_t) base << ch->block;
			int w = wse ? op->re0 & 3 : 0;
			op->zmask = zero_mask[w];
			op->nmask = neg_mask[w];
		}
		ch->live = ops[c * 2].state != OFF || ops[c * 2 + 1].state != OFF;
		ch->fbmul = ch->rc0 >> 1 & 7 ? 1 << (ch->rc0 >> 1 & 7) : 0;
		ch->modmask = ch->rc0 & 1 ? 0 : -1;
	}
	/* the LFOs: tremolo 3.7 Hz, vibrato 6.1 Hz, as 16-bit phases a block on */
	lfo_am += (uint32_t) (65536ULL * 37 * BLOCK / 10 / RATE);
	lfo_vib += (uint32_t) (65536ULL * 61 * BLOCK / 10 / RATE);
}

/* An operator's wave at phase i (as the DSP: without branches) */
static inline int32_t wave(const struct Op *op, uint32_t i)
{
	int32_t s = tables.hsine[i & 511];
	int32_t neg = -(int32_t) ((i & op->nmask) != 0), keep = ((int32_t) (i & op->zmask) - 1) >> 31;
	return ((s ^ neg) - neg) & keep;
}

void e_opl_block(struct EOplStream *s, int16_t out[E_OPL_BLOCK])
{
	int32_t mix[BLOCK];

	if (!tables_ok)
		make_tables();
	if (s != NULL)
		ticks(s);
	envelopes();
	memset(mix, 0, sizeof mix);
	/* a channel at a time, its samples (as the DSP) */
	for (int c = 0; c < CHANNELS; c++) {
		struct Channel *ch = &chans[c];
		struct Op *m = &ops[c * 2], *car = &ops[c * 2 + 1];
		if (!ch->live)
			continue;
		for (int i = 0; i < BLOCK; i++) {
			m->phase += m->step;
			m->g += m->dg;
			int32_t fbm = (m->out0 + m->out1) * ch->fbmul >> 9;
			car->phase += car->step;
			car->g += car->dg;
			int32_t mo = wave(m, (m->phase >> 22) + (uint32_t) fbm) * (m->g >> 5) >> 15;
			m->out1 = m->out0;
			m->out0 = mo;
			int32_t moved = mo & ch->modmask;
			mix[i] += (wave(car, (car->phase >> 22) + (uint32_t) moved) * (car->g >> 5) >> 15) + (mo - moved);
		}
	}
	for (int i = 0; i < BLOCK; i++)
		out[i] = (int16_t) (mix[i] > 32767 ? 32767 : mix[i] < -32768 ? -32768 : mix[i]);
}
