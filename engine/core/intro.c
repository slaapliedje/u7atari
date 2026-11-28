/* The intro (the original's INTRO): its six scenes as the original plays them, from the pictures
 * made ahead (ENGINE/ENDSHAPE.FLX: the original's ENDSHAPE.FLX), its synth effects and speech
 * (ENGINE/INTRO.FLX), its songs (MUSIC.FLX's 64-66: INTROADM.DAT's 0-2), STATIC/INTROPAL.DAT and
 * the speech's sync track (MAINSHP.FLX's 15). Written as the original's is, a scene after the
 * other, each frame drawn whole and shown with its palette (eb_frame_show), the original's 60 Hz
 * ticks the blanks. Its fades and palette turns, a VGA retrace each there (70 Hz), are a blank
 * each here; its loops that ran as fast as the PC could, a blank a step.
 *
 * The original's keys are the pad's: B (its Esc) ends the intro; any other key cuts short what is
 * going on, as each scene's keys do there. */

#include <string.h>

#include "engine.h"

/* ENDSHAPE.FLX's pictures */
#define E_CURTAIN_L 0
#define E_CURTAIN_R 1
#define E_STONES 2          /* 2-5: the four quarters */
#define E_MAP 6
#define E_MONITOR 7         /* 7 top right, 8 bottom right, 9 top left, 10 bottom left */
#define E_DESK 11
#define E_FIST 12
#define E_TITLE 13
#define E_BUTTERFLY 14
#define E_PRESENTS 17
#define E_TITLE_BACK 18
#define E_NOISE 19
#define E_DOT 20
#define E_CAPTION 21        /* 21-24, 25, 26, 28 */
#define E_SMALL_TITLE 29
#define E_MOUTH 30
#define E_CROWN 31
#define E_EYES 32
#define E_SWIRL1 33
#define E_SWIRL2 34
#define E_FACE 35
#define PICTURES 40

/* INTROPAL.DAT's */
#define P_DESK 1
#define P_MONITOR 2
#define P_PRESENTS 3
#define P_TITLE 4
#define P_MOONGATE 5

/* INTRO.FLX's: the synth effects, then the speech */
enum { S_STATIC, S_POWER_OFF, S_APPEAR, S_PUNCH, S_MOONGATE, S_ENTER, S_VANISH, S_DOT, S_SPEECH };
#define SOUNDS 9
#define SPEECH_VOLUME 80        /* (under the Guardian's song, at most a little clipped) */

#define SONG_TITLE (64 + 0)
#define SONG_DESK (64 + 1)
#define SONG_GUARDIAN (64 + 2)

#define FADE 60
#define STATIC_COLOR 7

static struct { struct EPicture p; const uint8_t *room; uint8_t ok, tried; } pics[PICTURES];
static struct { const uint8_t *at; uint32_t n; int channel; } sounds[SOUNDS];
static uint8_t pal[768];        /* the palette loaded */
static uint8_t shown[768];      /* the palette shown */
static int quit;                /* B: the intro over */
static int key;                 /* the last key (not B) since cleared */
static uint32_t seed = 0x12345678;

static int rnd(int n)
{
	seed = seed * 1103515245u + 12345u;
	return (int) ((seed >> 16) % (uint32_t) n);
}

/* ---- the pictures ---- */

static int pic(int n)
{
	if (n < 0 || n >= PICTURES)
		return 0;
	if (!pics[n].tried) {
		uint32_t size;
		const uint8_t *e = e_flex_direct("ENGINE", "ENDSHAPE.FLX", n, &size);
		pics[n].tried = 1;
		if (e != NULL && size > E_PICTURE_HEAD) {
			e_picture_head(e, &pics[n].p);
			pics[n].room = e + E_PICTURE_HEAD;
			pics[n].ok = 1;
		}
	}
	return pics[n].ok;
}

static void draw(struct ESurface *f, int n, int fr, int x, int y)
{
	if (pic(n))
		e_picture_draw(f, pics[n].room, &pics[n].p, fr, (int16_t) x, (int16_t) y);
}

/* the frame's reach from the hot spot: right, left, above, below */
static int reach(int n, int fr, int16_t r[4])
{
	return pic(n) && e_picture_reach(pics[n].room, &pics[n].p, fr, r);
}

/* ---- pictures drawn into bitmaps (the scaled and turned ones), in the spare room ---- */

static uint8_t *room;
static uint32_t room_size, room_used, room_base;

struct Bitmap {
	uint8_t *px;                /* w by h, 0 clear */
	int16_t w, h, hx, hy;       /* its hot spot in it */
};

static void *take(uint32_t n)
{
	n = (n + 7) & ~7u;
	if (room == NULL || room_used + n > room_size)
		return NULL;
	void *p = room + room_used;
	room_used += n;
	return p;
}

/* Picture n's frame fr drawn into a bitmap of its own size (pixels it does not cover 0). */
static int bitmap(int n, int fr, struct Bitmap *b)
{
	int16_t r[4];
	b->px = NULL;
	if (!reach(n, fr, r))
		return 0;
	b->w = (int16_t) (r[0] + r[1] + 1);
	b->h = (int16_t) (r[2] + r[3] + 1);
	b->hx = r[1];
	b->hy = r[2];
	struct ESurface s;
	s.pixels = take((uint32_t) b->w * b->h);
	s.rows = take((uint32_t) b->h * 4);
	if (s.pixels == NULL || s.rows == NULL)
		return 0;
	s.pitch = s.w = (uint16_t) b->w;
	s.h = (uint16_t) b->h;
	e_surface_rows(&s);
	memset(s.pixels, 0, (size_t) b->w * b->h);
	int16_t clip[4] = { 0, 0, (int16_t) (b->w - 1), (int16_t) (b->h - 1) };
	e_picture_draw_clip(&s, pics[n].room, &pics[n].p, fr, b->hx, b->hy, clip);
	eb_finish();
	b->px = s.pixels;
	return 1;
}

/* A full-screen picture (a scene's background) drawn once into a frame of its own and copied by
 * the blitter after (redrawing its runs every frame was half the Guardian scene's time). */
static struct ESurface back;
static int back_n = -1;

static void background(struct ESurface *f, int n)
{
	if (back.pixels == NULL) {
		draw(f, n, 0, 0, 0);
		return;
	}
	if (back_n != n) {
		static const int16_t whole[4] = { 0, 0, E_VIEW_W - 1, E_VIEW_H - 1 };
		eb_clear(&back, 0, 0, E_VIEW_W, E_VIEW_H);
		if (pic(n))
			e_picture_draw_clip(&back, pics[n].room, &pics[n].p, 0, 0, 0, whole);
		eb_finish();
		back_n = n;
	}
	eb_copy(f, 0, 0, &back, 0, 0, E_VIEW_W, E_VIEW_H);
}

/* the static's random grey and black, read from a random place each line (as the original read
 * the PC's BIOS ROM) */
#define NOISE 8192
static uint8_t *noise;

/* A bitmap drawn with its hot spot at x, y, scaled by scale/256 about it (nearest), its 0 clear. */
static void blit(struct ESurface *f, const struct Bitmap *b, int x, int y, int scale)
{
	if (b->px == NULL || scale <= 0)
		return;
	eb_scaled(f, b->px, b->w, b->h, x - b->hx * scale / 256, y - b->hy * scale / 256, scale);
}

/* A bitmap turned by deg degrees (clockwise on the screen) and scaled by scale/256 about its hot
 * spot, into a new bitmap; sine and cosine from a small table (the angles the intro uses). */
static int turned(const struct Bitmap *b, int deg, int scale, struct Bitmap *out)
{
	/* sin, cos of 0..45 degrees in 1/4096 */
	static const int16_t sn[46] = { 0, 71, 143, 214, 286, 357, 428, 499, 570, 641, 711, 782, 852,
		921, 991, 1060, 1129, 1198, 1266, 1334, 1401, 1468, 1534, 1600, 1666, 1731, 1796, 1860,
		1923, 1986, 2048, 2110, 2171, 2231, 2290, 2349, 2408, 2465, 2522, 2578, 2633, 2687, 2741,
		2793, 2845, 2896 };
	if (b->px == NULL || deg < 0 || deg > 45)
		return 0;
	int s = sn[deg], c;
	/* cos from sin, by its square root (the table stops at 45) */
	{
		int64_t cc = (int64_t) 4096 * 4096 - (int64_t) s * s;
		int r = 4096;
		for (int k = 0; k < 20; k++)
			r = (int) ((r + cc / r) / 2);
		c = r;
	}
	/* the corners about the hot spot, turned and scaled: the new box */
	int minx = 0, maxx = 0, miny = 0, maxy = 0;
	for (int k = 0; k < 4; k++) {
		int px = (k & 1 ? b->w - 1 : 0) - b->hx, py = (k & 2 ? b->h - 1 : 0) - b->hy;
		int qx = (int) (((int64_t) px * c - (int64_t) py * s) * scale / 256 / 4096);
		int qy = (int) (((int64_t) px * s + (int64_t) py * c) * scale / 256 / 4096);
		if (k == 0 || qx < minx) minx = qx;
		if (k == 0 || qx > maxx) maxx = qx;
		if (k == 0 || qy < miny) miny = qy;
		if (k == 0 || qy > maxy) maxy = qy;
	}
	out->w = (int16_t) (maxx - minx + 1);
	out->h = (int16_t) (maxy - miny + 1);
	out->hx = (int16_t) -minx;
	out->hy = (int16_t) -miny;
	out->px = take((uint32_t) out->w * out->h);
	if (out->px == NULL)
		return 0;
	/* back from the screen to the bitmap (turned the other way, unscaled), stepped in 16.16: a
	 * pixel right adds (c, -s) / k, a row down (s, c) / k, k the scale */
	int32_t ax = (int32_t) ((int64_t) c * 4096 / scale), ay = (int32_t) ((int64_t) s * 4096 / scale);
	int32_t rx = -out->hx * ax - out->hy * ay + (b->hx << 16) + 32768;
	int32_t ry = out->hx * ay - out->hy * ax + (b->hy << 16) + 32768;
	for (int y = 0; y < out->h; y++, rx += ay, ry += ax) {
		int32_t px = rx, py = ry;
		uint8_t *o = out->px + y * out->w;
		for (int x = 0; x < out->w; x++, px += ax, py -= ay) {
			int ix = px >> 16, iy = py >> 16;
			o[x] = ix >= 0 && ix < b->w && iy >= 0 && iy < b->h ? b->px[iy * b->w + ix] : 0;
		}
	}
	return 1;
}

/* ---- the palette ---- */

static void load_palette(int n)
{
	if (!e_palette_read("STATIC", "INTROPAL.DAT", n, pal))
		memset(pal, 0, sizeof pal);
	pal[765] = pal[766] = pal[767] = 0;         /* E_BLACK */
}

static void black(void)
{
	memset(shown, 0, sizeof shown);
}

static void apply(void)
{
	memcpy(shown, pal, sizeof shown);
}

/* the colours first..last turned a place: toward first (first < last), else the other way; in
 * the loaded palette and the shown */
static void rotate(int first, int last)
{
	uint8_t keep[3];
	if (first < last) {
		memcpy(keep, pal + first * 3, 3);
		memmove(pal + first * 3, pal + (first + 1) * 3, (size_t) (last - first) * 3);
		memcpy(pal + last * 3, keep, 3);
	} else {
		memcpy(keep, pal + first * 3, 3);
		memmove(pal + (last + 1) * 3, pal + last * 3, (size_t) (first - last) * 3);
		memcpy(pal + last * 3, keep, 3);
	}
	int lo = first < last ? first : last, hi = first < last ? last : first;
	memcpy(shown + lo * 3, pal + lo * 3, (size_t) (hi - lo + 1) * 3);
}

/* ---- showing ---- */

/* The frame drawn shown, ticks blanks after the last (0: at once); a key cuts short (key set),
 * B ends all (quit). */
static void show(int ticks)
{
	e_music_service();
	int k = eb_frame_show(shown, ticks);
	if (k == E_KEY_BACK)
		quit = 1;
	else if (k != 0)
		key = 1;
}

/* the scene's own painting: each scene sets it */
static void (*paint)(struct ESurface *f);

static void repaint(int ticks)
{
	struct ESurface *f = eb_frame_next();
	eb_clear(f, 0, 0, E_VIEW_W, E_VIEW_H);
	if (paint != NULL)
		paint(f);
	eb_finish();
	show(ticks);
}

/* Waits ticks blanks (showing the frame again), or till a key. */
static void wait(int ticks)
{
	uint32_t until = eb_blanks() + (uint32_t) ticks;
	key = 0;
	while (!quit && !key && (int32_t) (until - eb_blanks()) > 0)
		repaint(1);
}

/* Fades from black (colours first..last; the rest as loaded) or to black, FADE steps a blank each;
 * a key ends it at its end. */
static void fade_from(int first, int last, int others_black)
{
	key = 0;
	for (int step = 1; step <= FADE && !quit; step++) {
		if (key)
			step = FADE;
		if (others_black)
			memset(shown, 0, sizeof shown);
		else
			memcpy(shown, pal, sizeof shown);
		for (int c = first * 3; c <= last * 3 + 2; c++)
			shown[c] = (uint8_t) (pal[c] * step / FADE);
		repaint(1);
	}
	key = 0;
}

static void fade_in(int first, int last)
{
	fade_from(first, last, 0);
}

static void fade_out(void)
{
	uint8_t from[768];
	memcpy(from, shown, sizeof from);
	key = 0;
	for (int step = FADE - 1; step >= 0 && !quit; step--) {
		if (key)
			step = 0;
		for (int c = 0; c < 768; c++)
			shown[c] = (uint8_t) (from[c] * step / FADE);
		repaint(1);
	}
	key = 0;
}

/* TV static: n frames of the scene with the rectangle grey and black at random (colour 7 or 0). */
static int static_x0, static_y0, static_x1 = E_VIEW_W, static_y1 = E_VIEW_H, static_black;

static void tv_static(int frames)
{
	for (int k = 0; k < frames && !quit; k++) {
		struct ESurface *f = eb_frame_next();
		eb_clear(f, 0, 0, E_VIEW_W, E_VIEW_H);
		if (paint != NULL)
			paint(f);
		eb_finish();
		int w = static_x1 - static_x0;
		for (int y = static_y0; y < static_y1; y++) {
			uint8_t *p = f->pixels + y * f->pitch + static_x0;
			if (static_black || noise == NULL)
				memset(p, 0, (size_t) w);
			else
				memcpy(p, noise + rnd(NOISE - w), (size_t) w);
		}
		show(1);
	}
}

/* ---- the sounds ---- */

static void sound(int n)
{
	if (n < 0 || n >= SOUNDS || sounds[n].at == NULL)
		return;
	if (sounds[n].channel >= 0)
		eb_sound_stop(sounds[n].channel);
	sounds[n].channel = eb_sound(sounds[n].at, sounds[n].n, n == S_SPEECH ? SPEECH_VOLUME : 127, 64);
}

static void hush(int n)
{
	if (n >= 0 && n < SOUNDS && sounds[n].channel >= 0) {
		eb_sound_stop(sounds[n].channel);
		sounds[n].channel = -1;
	}
}

static void hush_all(void)
{
	for (int n = 0; n < SOUNDS; n++)
		hush(n);
}

/* ---- scene 1: Lord British presents ---- */

static void paint_presents(struct ESurface *f)
{
	background(f, E_PRESENTS);
}

static void presents(void)
{
	load_palette(P_PRESENTS);
	black();
	paint = paint_presents;
	fade_in(0, 255);
	wait(60);
	fade_out();
}

/* ---- scene 2: the title, the butterfly ---- */

static const uint16_t fly_x[96] = { 6, 18, 30, 41, 52, 62, 70, 78, 86, 95, 104, 113, 122, 132, 139,
	146, 151, 155, 157, 158, 157, 155, 151, 146, 139, 132, 124, 116, 108, 102, 96, 93, 93, 93, 95, 99,
	109, 111, 118, 125, 132, 140, 148, 157, 164, 171, 178, 184, 190, 196, 203, 211, 219, 228, 237,
	246, 254, 259, 262, 264, 265, 265, 263, 260, 256, 251, 245, 239, 232, 226, 219, 212, 208, 206,
	206, 209, 212, 216, 220, 224, 227, 234, 231, 232, 233, 233, 233, 233, 234, 236, 239, 243, 247,
	250, 258, 265 };
static const uint8_t fly_y[96] = { 155, 153, 151, 150, 149, 148, 148, 148, 148, 149, 150, 150, 150,
	149, 147, 142, 137, 131, 125, 118, 110, 103, 98, 94, 92, 91, 91, 91, 92, 95, 99, 104, 110, 117,
	123, 127, 131, 134, 135, 135, 135, 135, 135, 134, 132, 129, 127, 123, 119, 115, 112, 109, 104,
	102, 101, 102, 109, 109, 114, 119, 125, 131, 138, 144, 149, 152, 156, 158, 159, 159, 158, 155,
	150, 144, 137, 130, 124, 118, 112, 105, 99, 93, 86, 80, 73, 66, 59, 53, 47, 42, 38, 35, 32, 29,
	26, 25 };
static int bfly_x, bfly_y, bfly_frame, bfly_shown, bfly_dir;

static void paint_title(struct ESurface *f)
{
	background(f, E_TITLE_BACK);
	draw(f, E_TITLE, 0, 160, 50);
	if (bfly_shown)
		draw(f, E_BUTTERFLY, bfly_frame, bfly_x, bfly_y);
}

static void flap(void)
{
	/* the wings: 4 times in 5 a frame on, bouncing 0 1 2 3 2 1 0 */
	if (rnd(5) == 0)
		return;
	if (bfly_frame == 3)
		bfly_dir = -1;
	else if (bfly_frame == 0)
		bfly_dir = 1;
	bfly_frame += bfly_dir;
}

static void title(void)
{
	load_palette(P_TITLE);
	black();
	paint = paint_title;
	bfly_shown = 0;
	bfly_frame = 0;
	bfly_dir = 1;
	bfly_x = fly_x[0];
	bfly_y = fly_y[0];
	e_music_song(SONG_TITLE);
	uint32_t start = eb_blanks();
	wait(230);
	if (!quit)
		fade_in(0, 255);
	/* the flight from tick 990, 3 steps a leg, a step a tick (paced by the clock from the song's
	 * start); a key stops it where it is */
	bfly_shown = 1;
	key = 0;
	while (!quit && !key && (int32_t) (eb_blanks() - (start + 990)) < 0)
		repaint(1);
	for (int leg = 0; leg < 95 && !quit && !key; leg++)
		for (int k = 1; k <= 3 && !quit && !key; k++) {
			flap();
			bfly_x = fly_x[leg] + (fly_x[leg + 1] - fly_x[leg]) * k / 3;
			bfly_y = fly_y[leg] + (fly_y[leg + 1] - fly_y[leg]) * k / 3;
			uint32_t due = start + 990 + (uint32_t) (leg * 3 + k);
			int32_t ahead = (int32_t) (due - eb_blanks());
			repaint(ahead > 0 ? ahead : 0);
		}
	/* the landing (a key skips it) */
	static const uint8_t land_frame[8] = { 3, 4, 3, 4, 3, 2, 1, 0 };
	static const uint8_t land_ticks[8] = { 10, 25, 15, 25, 25, 25, 25, 20 };
	key = 0;
	for (int k = 0; k < 8 && !quit && !key; k++) {
		bfly_frame = land_frame[k];
		wait(land_ticks[k]);
	}
	/* till the song ends (a key ends that) */
	key = 0;
	while (!quit && !key && !e_music_song_over())
		repaint(1);
	/* the flash into static */
	load_palette(P_MONITOR);
	e_music_song(E_MUSIC_NONE);
	black();
	sound(S_STATIC);
	paint = NULL;
	static_x0 = static_y0 = 0;
	static_x1 = E_VIEW_W;
	static_y1 = E_VIEW_H;
	static_black = 0;
	apply();
	tv_static(1);
}

/* ---- scene 3: static ---- */

static void paint_noise(struct ESurface *f)
{
	background(f, E_NOISE);
}

static void static_scene(void)
{
	load_palette(P_MONITOR);
	apply();
	paint = paint_noise;
	tv_static(20);
	hush(S_STATIC);
	repaint(0);
	wait(6);
	sound(S_STATIC);
	tv_static(10);
	hush(S_STATIC);
	repaint(0);
	wait(12);
	sound(S_STATIC);
	tv_static(5);
	hush(S_STATIC);
	repaint(0);
}

/* ---- scene 4: the Guardian ---- */

static int swirl, swirl_frame, face_frame, guardian, mouth_frame, eye_frame, brow, eye_state;

static void paint_guardian(struct ESurface *f)
{
	background(f, E_NOISE);
	if (swirl == 1 && swirl_frame <= 9) {
		draw(f, E_SWIRL1, 0, 118, 48);
		draw(f, E_SWIRL1, swirl_frame, 118, 48);
	} else if (swirl == 2 && swirl_frame >= 1) {
		draw(f, E_SWIRL2, 0, 147, 62);
		draw(f, E_SWIRL2, swirl_frame, 147, 62);
	}
	if (face_frame >= 0) {
		draw(f, E_FACE, 0, 160, 100);
		draw(f, E_FACE, face_frame, 160, 100);
	}
	if (guardian) {
		draw(f, E_MOUTH, 0, 160, 100);
		draw(f, E_MOUTH, mouth_frame, 160, 100);
		draw(f, E_CROWN, 0, 160, 52);
		draw(f, E_EYES, 0, 160, 89);
		draw(f, E_EYES, eye_frame, 160, 89);
	}
}

/* a cycle step: the monitor's colours 16-93 a place on (dir 'A' toward 16, 'B' toward 93) */
static void cycle(int dir, int ticks)
{
	if (dir == 'A')
		rotate(16, 93);
	else
		rotate(93, 16);
	repaint(ticks);
}

static void cycles(int n, int dir)
{
	key = 0;
	for (int k = 0; k < n && !quit && !key; k++)
		cycle(dir, 3);
}

static void set_eyes(void)
{
	eye_frame = 3 * brow + eye_state + 1;
}

static void guardian_scene(void)
{
	int skipped = 0;

	load_palette(P_MONITOR);
	apply();
	e_music_song(SONG_GUARDIAN);
	paint = paint_guardian;
	swirl = 0;
	face_frame = -1;
	guardian = 0;
	cycles(120, 'A');
	/* the swirls and the face forming: a key there skips the rest of the scene */
	key = 0;
	swirl = 1;
	swirl_frame = 9;
	sound(S_APPEAR);
	for (int step = 0; swirl_frame > 1 && !quit && !key; step++) {
		if (step % 2 == 0)
			swirl_frame--;
		cycle('A', 1);
	}
	hush(S_APPEAR);
	sound(S_VANISH);
	for (int step = 0; swirl_frame <= 9 && !quit && !key; step++) {
		if (step % 5 == 0)
			swirl_frame++;
		cycle('A', 1);
	}
	swirl = 0;
	hush(S_VANISH);
	skipped = key;
	if (!skipped) {
		cycles(50, 'A');
		key = 0;
		swirl = 2;
		swirl_frame = 1;
		sound(S_APPEAR);
		for (int step = 0; swirl_frame < 8 && !quit && !key; step++) {
			if (step % 5 == 0)
				swirl_frame++;
			cycle('A', 1);
		}
		hush(S_APPEAR);
		sound(S_VANISH);
		for (int step = 0; swirl_frame >= 1 && !quit && !key; step++) {
			if (step % 5 == 0)
				swirl_frame--;
			cycle('A', 1);
		}
		swirl = 0;
		hush(S_VANISH);
		skipped = key;
	}
	if (!skipped && !quit) {
		cycles(50, 'A');
		key = 0;
		face_frame = 0;
		sound(S_APPEAR);
		for (int step = 0; face_frame < 15 && !quit && !key; step++) {
			if (step % 5 == 0)
				face_frame++;
			cycle('A', 1);
		}
		hush(S_APPEAR);
		skipped = key;
	}
	if (!skipped && !quit) {
		/* the Guardian, his parts; before he speaks, his eyes */
		face_frame = -1;
		guardian = 1;
		mouth_frame = 1;
		brow = 2;
		eye_state = 0;
		eye_frame = 1;
		static const uint8_t group[5] = { 20, 5, 9, 10, 15 };
		static const int8_t eyes[5] = { 5, 4, 10, 4, -1 };
		key = 0;
		for (int g = 0; g < 5 && !quit && !key; g++) {
			for (int k = 0; k < group[g] && !quit && !key; k++) {
				rotate(93, 16);
				repaint(3);
			}
			if (eyes[g] >= 0)
				eye_frame = eyes[g];
		}
		/* the speech, his face moved by the sync track (a cue's time in ticks from the speech's
		 * start, then what: 1-3 brow 2-0, 4-6 eye state 0-2, 7 the bright eyes, 8 on a mouth);
		 * as the original's: a cue already past or due at once dropped */
		static uint8_t track[900];
		int32_t n = e_flex_read("STATIC", "MAINSHP.FLX", 15, track, sizeof track);
		int cues = n > 0 ? (int) n / 3 : 0, next = 1;
		key = 0;
		sound(S_SPEECH);
		uint32_t start = eb_blanks();
		uint32_t length = sounds[S_SPEECH].n * 60 / E_SOUND_RATE;
		while (!quit && !key) {
			uint32_t now = eb_blanks() - start;
			while (next < cues) {
				uint32_t t = track[next * 3] | (uint32_t) track[next * 3 + 1] << 8;
				if (t > now)
					break;
				int v = track[next * 3 + 2];
				if (v >= 1 && v <= 3) {
					brow = 3 - v;
					set_eyes();
				} else if (v >= 4 && v <= 6) {
					eye_state = v - 4;
					set_eyes();
				} else if (v == 7)
					eye_frame = 10;
				else if (v >= 8)
					mouth_frame = v - 8;
				/* the next one still to come: those due now, or behind, dropped */
				next++;
				while (next < cues && (uint32_t) (track[next * 3] | track[next * 3 + 1] << 8) <= t)
					next++;
			}
			if (now >= length && next >= cues)
				break;
			rotate(93, 16);
			repaint(1);
		}
		hush(S_SPEECH);
		skipped = key;
		guardian = 0;
	}
	if (!skipped && !quit) {
		/* the face going, then the end pause */
		key = 0;
		face_frame = 15;
		sound(S_VANISH);
		for (int step = 0; face_frame > 0 && !quit && !key; step++) {
			if (step % 5 == 0)
				face_frame--;
			cycle('A', 1);
		}
		face_frame = -1;
		hush(S_VANISH);
		repaint(0);
		cycles(40, 'A');
		/* off: static, then the dot */
		e_music_song(E_MUSIC_NONE);
		sound(S_STATIC);
		static_x0 = static_y0 = 0;
		static_x1 = E_VIEW_W;
		static_y1 = E_VIEW_H;
		static_black = 0;
		tv_static(20);
		hush(S_STATIC);
		paint = NULL;
		sound(S_POWER_OFF);
		sound(S_DOT);
		key = 0;
		for (int k = 0; k < 100 && !quit && !key; k++) {
			struct ESurface *f = eb_frame_next();
			eb_clear(f, 0, 0, E_VIEW_W, E_VIEW_H);
			draw(f, E_DOT, 0, 159 + rnd(3), 99 + rnd(3));
			eb_finish();
			show(1);
		}
	}
	e_music_song(E_MUSIC_NONE);
	hush(S_APPEAR);
	hush(S_VANISH);
	hush(S_SPEECH);
}

/* ---- scene 5: the desk ---- */

static struct Bitmap quarter[4], small_title, fist[4], dot;
static int q_x, q_y, q_scale, map_x, map_y, desk_y, title_x, title_y, title_shown;
static int dot_x, dot_y, dot_shown, fist_angle, caption, caption_y, desk_shown, map_shown;

static void paint_desk(struct ESurface *f)
{
	if (map_shown)
		draw(f, E_MAP, 0, map_x, map_y);
	for (int k = 0; k < 4; k++)
		blit(f, &quarter[k], q_x, q_y, q_scale);
	if (title_shown)
		blit(f, &small_title, title_x, title_y, 256);
	if (dot_shown)
		blit(f, &dot, dot_x + rnd(3) - 1, dot_y + rnd(3) - 1, 256);
	if (fist_angle > 0)
		blit(f, &fist[fist_angle / 11], 0, 150, 256);
	if (desk_shown)
		draw(f, E_DESK, 0, 319, desk_y);
	if (caption)
		draw(f, caption, 0, 160, caption_y);
}

/* the Orb's glow: its three groups of colours a place each */
static void orb(void)
{
	rotate(240, 244);
	rotate(245, 249);
	rotate(250, 254);
}

static void desk(void)
{
	room_used = room_base;
	for (int k = 0; k < 4; k++)
		bitmap(E_MONITOR + k, 0, &quarter[k]);
	bitmap(E_SMALL_TITLE, 0, &small_title);
	bitmap(E_DOT, 0, &dot);
	struct Bitmap up;
	fist[0].px = NULL;
	if (bitmap(E_FIST, 0, &up)) {
		turned(&up, 11, 200, &fist[1]);
		turned(&up, 22, 200, &fist[2]);
		turned(&up, 33, 200, &fist[3]);
	}
	load_palette(P_DESK);
	e_music_song(SONG_DESK);
	paint = paint_desk;
	map_shown = desk_shown = title_shown = 0;
	caption = 0;
	fist_angle = 0;
	/* 5.1 the pull back from the dot: the four quarters from 2x to 1x, 41 ticks */
	dot_shown = 1;
	dot_x = 160;
	dot_y = 104;
	q_scale = 0;
	repaint(0);
	apply();
	key = 0;
	for (int k = 1; k <= 41 && !quit && !key; k++) {
		q_x = 138 + (160 - 138) * k / 41;
		q_y = 144 + (100 - 144) * k / 41;
		q_scale = 512 - 256 * k / 41;
		dot_x = 160 + 10 * k / 41;
		dot_y = 104 - 24 * k / 41;
		repaint(1);
	}
	q_x = 160;
	q_y = 100;
	q_scale = 256;
	hush(S_DOT);
	dot_shown = 0;
	title_x = 171;
	title_y = 78;
	/* 5.2 the fist, three blows (a key ends them, the title shown) */
	key = 0;
	for (int blow = 0; blow < 3 && !quit && !key; blow++) {
		fist_angle = 33;
		repaint(1);
		if (blow > 0)
			hush(S_STATIC);
		sound(S_PUNCH);
		if (blow < 2) {
			title_shown = 1;
			repaint(1);
			title_shown = 0;
		}
		sound(S_STATIC);
		static_x0 = 92;
		static_y0 = 26;
		static_x1 = 252;
		static_y1 = 131;
		static_black = blow == 2;
		for (int a = 22; a >= 0 && !quit; a -= 11) {
			fist_angle = a;
			tv_static(1);
			repaint(0);
		}
	}
	fist_angle = 0;
	title_shown = 1;
	hush(S_STATIC);
	repaint(0);
	wait(60);
	/* 5.4 "Something is obviously amiss.": a key ends the whole scene */
	caption = E_CAPTION;
	caption_y = 150;
	wait(200);
	if (key || quit) {
		e_music_song(E_MUSIC_NONE);
		return;
	}
	/* 5.5 the pan left to the map */
	caption = 0;
	map_shown = 1;
	key = 0;
	for (int k = 1; k <= 101 && !quit && !key; k++) {
		int d = 194 * k / 101;
		q_x = 160 - d;
		map_x = 319 - d;
		map_y = -1;
		title_x = 171 - d;
		if (k == 40) {
			caption = E_CAPTION + 1;
			caption_y = 150;
		}
		repaint(1);
	}
	q_x = 160 - 194;
	map_x = 319 - 194;
	title_x = 171 - 194;
	caption = 0;
	/* (the left quarters gone) */
	quarter[2].px = quarter[3].px = NULL;
	wait(100);
	/* 5.7 the pan down to the Orb, its glow every second step */
	desk_shown = 1;
	caption = E_CAPTION + 2;
	caption_y = 10;
	key = 0;
	for (int k = 1; k <= 53 && !quit && !key; k++) {
		int d = 52 * k / 53;
		q_y = 100 - d;
		map_y = -1 - d;
		desk_y = 199 - d;
		title_y = 78 - d;
		if (k % 2 == 0)
			orb();
		repaint(1);
	}
	q_y = 48;
	map_y = -53;
	desk_y = 147;
	title_y = 26;
	/* 5.8 "It has opened gateways..." */
	caption = E_CAPTION + 3;
	for (int k = 0; k < 20 && !quit; k++) {
		orb();
		repaint(10);
	}
	fade_out();
	e_music_song(E_MUSIC_NONE);
}

/* ---- scene 6: the moongate ---- */

static struct Bitmap curtain[2], stone[4];
static int curtain_dx, curtain_scale, stone_dx, stone_dy, stone_scale;

static void paint_moongate(struct ESurface *f)
{
	static const int16_t sx[4] = { 160, 160, 160, 160 }, sy[4] = { 100, 100, 101, 101 };
	for (int k = 0; k < 4; k++)
		blit(f, &stone[k], sx[k] + stone_dx, sy[k] + stone_dy, stone_scale);
	blit(f, &curtain[0], 210 - curtain_dx, 0, curtain_scale);
	blit(f, &curtain[1], 110 + curtain_dx, 0, curtain_scale);
	if (caption)
		draw(f, caption, 0, 160, 150);
}

static void moongate(void)
{
	room_used = room_base;
	bitmap(E_CURTAIN_L, 0, &curtain[0]);
	bitmap(E_CURTAIN_R, 0, &curtain[1]);
	for (int k = 0; k < 4; k++)
		bitmap(E_STONES + k, 0, &stone[k]);
	load_palette(P_MOONGATE);
	black();
	sound(S_MOONGATE);
	paint = paint_moongate;
	curtain_dx = 0;
	curtain_scale = 256;
	stone_dx = stone_dy = 0;
	stone_scale = 256;
	caption = E_CAPTION + 4;
	/* 6.1 the caption alone (colour 11) */
	fade_from(11, 11, 1);
	wait(120);
	/* 6.3 the rest from black (0-15 at once; colour 11 stays) */
	fade_in(16, 255);
	/* 6.4 the glow */
	key = 0;
	for (int k = 0; k < 15 && !quit && !key; k++) {
		rotate(254, 240);
		repaint(5);
	}
	/* 6.5 the trees part */
	key = 0;
	for (int k = 1; k <= 82 && !quit && !key; k++) {
		curtain_dx = 240 * k / 82;
		curtain_scale = 256 + 160 * k / 82;
		caption = k >= 10 && k <= 36 ? E_CAPTION + 5 : k >= 42 ? E_CAPTION + 7 : 0;
		rotate(254, 240);
		repaint(1);
	}
	curtain_dx = 240;
	curtain_scale = 416;
	caption = 0;
	repaint(0);
	for (int k = 0; k < 40 && !quit; k++) {
		rotate(254, 240);
		repaint(5);
	}
	/* 6.7 into the gate */
	key = 0;
	for (int k = 1; k <= 12 && !quit && !key; k++) {
		stone_dx = 60 * k / 12;
		stone_dy = 150 * k / 12;
		stone_scale = 256 + 2048 * k / 12;
		rotate(254, 240);
		repaint(1);
	}
	hush(S_MOONGATE);
	sound(S_ENTER);
	paint = NULL;
	black();
	repaint(0);
	wait(60);
}

int e_intro_run(void)
{
	uint32_t size;

	quit = key = 0;
	paint = NULL;
	eb_intro_memory(1);
	for (int n = 0; n < PICTURES; n++)
		pics[n].tried = pics[n].ok = 0;
	for (int n = 0; n < SOUNDS; n++) {
		sounds[n].at = e_flex_direct("ENGINE", "INTRO.FLX", n, &size);
		sounds[n].n = sounds[n].at != NULL ? size / 2 : 0;
		sounds[n].channel = -1;
	}
	if (!pic(E_PRESENTS) || sounds[S_SPEECH].at == NULL) {
		eb_intro_memory(0);
		return -1;
	}
	room = e_spare_room(&room_size);
	room_used = 0;
	back.pixels = take(E_VIEW_W * E_VIEW_H);
	back.rows = take(E_VIEW_H * 4);
	if (back.pixels == NULL || back.rows == NULL)
		back.pixels = NULL;
	else {
		back.pitch = back.w = E_VIEW_W;
		back.h = E_VIEW_H;
		e_surface_rows(&back);
	}
	back_n = -1;
	noise = take(NOISE);
	for (int k = 0; noise != NULL && k < NOISE; k++)
		noise[k] = rnd(2) ? STATIC_COLOR : 0;
	room_base = room_used;
	eb_music_gain(E_MUSIC_GAIN_INTRO);
	presents();
	if (!quit)
		title();
	if (!quit)
		static_scene();
	if (!quit)
		guardian_scene();
	if (!quit)
		desk();
	if (!quit)
		moongate();
	hush_all();
	e_music_song(E_MUSIC_NONE);
	eb_music_gain(E_MUSIC_GAIN);
	black();
	paint = NULL;
	repaint(0);
	eb_intro_memory(0);
	return quit;
}
