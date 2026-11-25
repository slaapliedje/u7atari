/* The Jaguar's video and sound. The screen is the game's 320x200 frame in an 8-bit bitmap
 * object, its palette in the CLUT. Sound is not played yet: MIDI goes nowhere (with --lockstep
 * it is checksummed, as the PC reference's), and the game is told there is no synth.
 *
 * The object list (as jagwrestle's startup.s): two branches past the bitmap when the beam is
 * above or below the display, the bitmap, and a stop. The Object Processor changes the
 * bitmap's first phrase as it draws it; jag_vbl (startup.S) puts it back every frame. */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "plat.h"
#include "backend.h"
#include "gm.h"

#define REG16(a) (*(volatile uint16_t *) (a))
#define REG32(a) (*(volatile uint32_t *) (a))
#define OLP 0xF00020
#define VMODE 0xF00028
#define BORD1 0xF0002A
#define HDB1 0xF00038
#define HDB2 0xF0003A
#define HDE 0xF0003C
#define VDB 0xF00046
#define VDE 0xF00048
#define VI 0xF0004E
#define BG 0xF00058
#define INT1 0xF000E0
#define CLUT 0xF00400
#define CONFIG 0xF14002
#define LEVEL0 0x100

#define WIDTH PLAT_SCREEN_WIDTH
#define HEIGHT PLAT_SCREEN_HEIGHT
#define PHRASES (WIDTH / 8)

extern void jag_vbl(void);
extern volatile uint32_t jag_ticks;
extern volatile uint32_t *jag_vbl_object;
extern uint32_t jag_vbl_phrase[2];
uint32_t jag_heap_used(void);         /* syscalls.c */
uint32_t jag_heap_size(void);

#ifdef U7_DEBUG_LOG
#define IF_LOCKSTEP(call) do { if (lockstep) call; } while (0)
#else
#define IF_LOCKSTEP(call) do { } while (0)
#endif

/* ---- Video ---- */

static uint32_t object_list[12] __attribute__((aligned(16)));
static uint8_t screen[WIDTH * HEIGHT] __attribute__((aligned(16)));
static bool active;
static const uint8_t *last_pixels;
static uint8_t palette[256 * 3];

/* Branch object: to link when the beam's half-line compares with ypos as condition says. */
static void branch(uint32_t *object, uint32_t link, uint16_t ypos, uint32_t condition)
{
	object[0] = link >> 11;
	object[1] = link << 21 | condition << 14 | (uint32_t) ypos << 3 | 3;
}

void video_init(void)
{
	bool ntsc = (REG16(CONFIG) & 0x10) != 0;
	uint16_t width = ntsc ? 1409 : 1381, hmid = ntsc ? 823 : 843;
	uint16_t height = ntsc ? 241 : 287, vmid = ntsc ? 266 : 322;
	uint16_t hde = (uint16_t) ((width / 2 - 1) | 0x400), hdb = (uint16_t) (hmid - width / 2 + 4);
	uint16_t vdb = (uint16_t) (vmid - height), vde = (uint16_t) (vmid + height);
	uint32_t stop = (uint32_t) &object_list[8], data = (uint32_t) screen;
	uint16_t ypos = (uint16_t) ((height - HEIGHT + vdb) & ~1);
	uint16_t xpos = (uint16_t) ((width / 4 - WIDTH) / 2);

	REG16(HDB1) = hdb;
	REG16(HDB2) = hdb;
	REG16(HDE) = hde;
	REG16(VDB) = vdb;
	REG16(VDE) = 0xFFFF;
	REG32(BORD1) = 0;
	REG16(BG) = 0;

	/* past the bitmap outside the display: the conditions and lines as jagwrestle's */
	branch(&object_list[0], stop, vde, 2);
	branch(&object_list[2], stop, vdb, 1);
	/* the bitmap: 8 bits a pixel through the CLUT, a phrase on from each line to the next */
	object_list[4] = data << 8 | stop >> 11;
	object_list[5] = stop << 21 | (uint32_t) HEIGHT << 14 | (uint32_t) ypos << 3;
	object_list[6] = PHRASES >> 4;
	object_list[7] = (uint32_t) PHRASES << 28 | (uint32_t) PHRASES << 18 | 1 << 15 | 3 << 12 | xpos;
	object_list[8] = 0;
	object_list[9] = 4 | 8;                      /* stop, with interrupts */
	jag_vbl_phrase[0] = object_list[4];
	jag_vbl_phrase[1] = object_list[5];
	jag_vbl_object = &object_list[4];

	/* through a pointer GCC cannot follow: it takes a store this low for a null one */
	volatile uint32_t *level0 = (volatile uint32_t *) LEVEL0;
	__asm__ ("" : "+a" (level0));
	*level0 = (uint32_t) jag_vbl;
	REG16(VI) = vde | 1;
	REG16(INT1) = 1;                             /* video interrupts */
	REG16(VMODE) = 0;
	REG32(OLP) = (uint32_t) object_list << 16 | (uint32_t) object_list >> 16;
	REG16(VMODE) = 0x6C7;                        /* RGB16 CLUT, 320 wide, on */
	__asm__ volatile ("andi.w #0xF8FF, %%sr" ::: "cc");
	active = true;
}

void video_shutdown(void) { active = false; }

void video_reset(void)
{
	static const uint8_t black[256 * 3];

	last_pixels = NULL;
	if (active)
		plat_video_set_palette(0, 256, black);
}

/* The game's 6-bit VGA colours as the CLUT's RGB16: red 5 bits, blue 5, green 6. */
void plat_video_set_palette(int16_t first, int16_t count, const uint8_t *rgb)
{
	volatile uint16_t *clut = (volatile uint16_t *) CLUT;

	if (!active)
		return;
	if (first < 0) {
		rgb -= 3 * first;
		count += first;
		first = 0;
	}
	if (first + count > 256)
		count = 256 - first;
	if (count <= 0)
		return;
	memcpy(palette + 3 * first, rgb, (size_t) count * 3);
	for (int16_t i = 0; i < count; i++) {
		const uint8_t *c = rgb + 3 * i;

		clut[first + i] = (uint16_t) ((c[0] & 0x3e) << 10 | (c[2] & 0x3e) << 5 | (c[1] & 0x3f));
	}
	IF_LOCKSTEP(lockstep_palette(first, count, rgb));
}

#ifdef U7_DEBUG_LOG
/* frames presented, and the blank counted at the last report */
static uint32_t presents, report_presents, report_at;
#endif

void plat_video_present(const uint8_t *pixels)
{
	last_pixels = pixels;
	memcpy(screen, pixels, sizeof screen);
	IF_LOCKSTEP(lockstep_frame(pixels, 'P'));
#ifdef U7_DEBUG_LOG
	/* every 10 s of real time: the frame rate, to the log */
	presents++;
	if (!lockstep && timer_real_milliseconds() - report_at >= 10000) {
		char text[64];
		uint32_t now = timer_real_milliseconds();
		uint32_t tenths = (presents - report_presents) * 10000 / (now - report_at);

		snprintf(text, sizeof text, "fps %lu.%lu (%lu frames at %lu s)\r\n",
			(unsigned long) tenths / 10, (unsigned long) tenths % 10, (unsigned long) presents,
			(unsigned long) now / 1000);
		plat_log(text);
		report_presents = presents;
		report_at = now;
	}
#endif
}

void plat_video_copy_frame(uint8_t *dst, const uint8_t *src) { memmove(dst, src, WIDTH * HEIGHT); }
void plat_video_wait_retrace(void)
{
	uint32_t ticks;

#ifdef U7_DEBUG_LOG
	if (lockstep) {
		lockstep_wait_retrace();
		return;
	}
#endif
	ticks = jag_ticks;
	while (jag_ticks == ticks)
		;
}
void video_refresh(void) {}
void video_refresh_wanted(void) {}
void video_rows_wanted(int16_t top, int16_t bottom) { (void) top; (void) bottom; }
void video_pump(void) {}
/* The game draws its own pointer, as on the PC. */
int16_t video_pointer(void) { return 0; }
void video_pointer_shape(const uint8_t *pixels, int16_t width, int16_t height, int16_t hot_x,
	int16_t hot_y) { (void) pixels; (void) width; (void) height; (void) hot_x; (void) hot_y; }
void video_pointer_show(int16_t visible) { (void) visible; }
uint8_t *video_frame(void) { return (uint8_t *) last_pixels; }
/* Serpent Isle's debug screens: not shown yet. */
void video_show_text(const uint16_t *cells, int16_t cursor_x, int16_t cursor_y)
{
	(void) cells; (void) cursor_x; (void) cursor_y;
}
void video_show_game(void) {}

/* The script's "shot": the frame as a PPM on the card, as the PC's. */
void video_shot(const char *name)
{
	static uint8_t row[WIDTH * 3];
	FILE *f;

	if (last_pixels == NULL)
		return;
	if ((f = fopen(name, "wb")) == NULL) {
		char text[96];

		snprintf(text, sizeof text, "shot: cannot make %s (errno %d, heap %lu K of %lu K)\r\n",
			name, errno, (unsigned long) (jag_heap_used() >> 10),
			(unsigned long) (jag_heap_size() >> 10));
		plat_log(text);
		return;
	}
	fprintf(f, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
	for (int y = 0; y < HEIGHT; y++) {
		for (int x = 0; x < WIDTH; x++)
			for (int c = 0; c < 3; c++) {
				uint8_t v = palette[3 * last_pixels[y * WIDTH + x] + c] & 0x3f;

				row[3 * x + c] = (uint8_t) (v << 2 | v >> 4);
			}
		fwrite(row, 1, sizeof row, f);
	}
	fclose(f);
}

/* ---- Sound: not played yet; checksummed under --lockstep, as the PC reference's ---- */

bool sound_line_in;
bool sound_gm;
uint32_t sound_midi_bytes;
uint32_t sound_pcm_samples;

static void midi_out(const uint8_t *message, int16_t length)
{
	plat_sound_lock();
	sound_midi_bytes += (uint32_t) length;
	IF_LOCKSTEP(lockstep_midi(message, length));
	plat_sound_unlock();
}

static void midi_all_off(void)
{
	for (uint8_t channel = 0; channel < 16; channel++) {
		const uint8_t messages[3][3] = {
			{ 0xb0 | channel, 0x40, 0 },
			{ 0xb0 | channel, 0x7b, 0 },
			{ 0xb0 | channel, 0x79, 0 },
		};

		for (int i = 0; i < 3; i++)
			midi_out(messages[i], 3);
	}
}

void sound_init(void)
{
	midi_all_off();
	if (sound_gm)
		gm_reset(midi_out);
}

void sound_shutdown(void)
{
	plat_sound_tick_set(NULL);
	midi_all_off();
}

void sound_reset(void)
{
	plat_sound_tick_set(NULL);
	midi_all_off();
	plat_pcm_stop();
}

void sound_service(void) {}
/* As the PC reference under --lockstep; else no synth, until the DSP plays the music. */
int16_t plat_midi_available(void)
{
#ifdef U7_DEBUG_LOG
	return lockstep;
#else
	return 0;
#endif
}

void plat_midi_send(const uint8_t *message, int16_t length)
{
	if (sound_gm) {
		plat_sound_lock();
		gm_feed(message, length, midi_out);
		plat_sound_unlock();
	} else {
		midi_out(message, length);
	}
}

bool plat_opl_available(void) { return false; }
void plat_opl_write(uint16_t port, uint8_t value) { (void) port; (void) value; }
uint8_t plat_opl_read(uint16_t port) { (void) port; return 0xff; }

void plat_pcm_start(uint32_t rate)
{
	char text[48];

	snprintf(text, sizeof text, "pcm start %lu Hz\r\n", (unsigned long) rate);
	plat_log(text);
	IF_LOCKSTEP(lockstep_pcm_start(rate));
}

/* Speech: taken and dropped at once, when not checksummed. */
int32_t plat_pcm_queue(const uint8_t *samples, int32_t count)
{
#ifdef U7_DEBUG_LOG
	if (lockstep)
		return lockstep_pcm_queue(samples, count);
#endif
	(void) samples;
	return count;
}

int32_t plat_pcm_pending(void)
{
#ifdef U7_DEBUG_LOG
	if (lockstep)
		return lockstep_pcm_pending();
#endif
	return 0;
}

void plat_pcm_stop(void) { IF_LOCKSTEP(lockstep_pcm_stop()); }

void memory_report(char *text, size_t size)
{
	snprintf(text, size, "heap %lu K in use of %lu K", (unsigned long) (jag_heap_used() >> 10),
		(unsigned long) (jag_heap_size() >> 10));
}
