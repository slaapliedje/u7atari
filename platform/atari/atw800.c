/* The ATW800/2 graphics card on the TT's VME bus, for its HDMI output (geekdot.com/atw800_2).
 *
 * The game programs the card itself, with or without xVDI loaded, in xVDI's lowest mode:
 * 640x400, 256 colours through a table of RGB565 words, one byte a pixel. The card has no line
 * or pixel doubling, so each game pixel is written as four. There is one screen, without the
 * flip the TT's own video has: two would need every change written twice over a slow bus.
 *
 * Detection and the mode follow set_adr() and set_fpga() in the ATW800/2 Programmer's Manual.
 * When a driver is loaded, what it set is put back on exit from the shadow it keeps in the
 * card's ATWInfo block, with the part of its screen the game wrote over; otherwise the card's
 * output is turned off.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat.h"
#include "backend.h"

#define WIDTH PLAT_SCREEN_WIDTH
#define HEIGHT PLAT_SCREEN_HEIGHT
#define CARD_WIDTH (2 * WIDTH)
#define CARD_BYTES (CARD_WIDTH * 2 * HEIGHT)

#define BASE_A UINT32_C(0xfea00000)       /* the two positions of the ADDR jumper */
#define BASE_C UINT32_C(0xfec00000)
#define MEGA UINT32_C(0x100000)
/* The top of the card's memory holds these, at 1 MB up on a 2 MB card and 3 MB on a 4 MB one. */
#define ATW_INFO 0xfdc00
#define LUT 0xff000
#define FPGA_ID (0xff200 + 24)
#define VTG 0xff800
#define ID_STRING UINT32_C(0x2063706d)    /* " cpm" */

/* VTG registers, words */
enum {
	VTG_CTRL, VTG_HFP, VTG_HSY, VTG_HBP, VTG_HDI, VTG_VFP, VTG_VSY, VTG_VBP, VTG_VDI,
	VTG_PLL_FBD, VTG_PLL_ID, VTG_PLL_OD, VTG_VMEM_LOW, VTG_VMEM_HIGH, VTG_RESERVED, VTG_MEM_REG,
	VTG_COUNT
};

/* xVDI's " 640 x  400   256  72Hz": a 32.4 MHz pixel clock, with the PLL values minus 1 */
static const uint16_t game_mode[VTG_VMEM_LOW] = {
	[VTG_HFP] = 16, [VTG_HSY] = 96, [VTG_HBP] = 48, [VTG_HDI] = CARD_WIDTH,
	[VTG_VFP] = 12, [VTG_VSY] = 2, [VTG_VBP] = 35, [VTG_VDI] = 2 * HEIGHT,
	[VTG_PLL_FBD] = 24 - 1, [VTG_PLL_ID] = 5 - 1, [VTG_PLL_OD] = 4 - 1,
};
#define GAME_CTRL 29      /* on, positive vsync, PLL values taken, 8 bits a pixel with the table */

/* The driver's record of itself in the card's memory */
typedef struct {
	uint32_t id_a, id_b, version, version_fpga, vdi_memstart, vdi_memsize;
	uint16_t vdi_vstride, vdi_bpp, vdi_width, vdi_height;
	uint16_t vtg[VTG_COUNT];
	uint16_t endmagic;
} atw_info;
_Static_assert(offsetof(atw_info, vtg) == 32 && offsetof(atw_info, endmagic) == 64, "manual, p. 6");

static volatile uint16_t *vtg;
static volatile uint16_t *lut;
static uint8_t *memory;
static uint16_t doubled[256];
static uint16_t old_lut[256];
static uint16_t old_vtg[VTG_MEM_REG];
static uint8_t *old_screen;       /* NULL when no driver had set the card up */

/* probe.S: reads a long, in supervisor mode; false on a bus error. */
bool probe_long(uint32_t address, uint32_t *value);

static bool has_id(uint32_t base, uint32_t extra)
{
	uint32_t value;

	return probe_long(base + extra + FPGA_ID, &value) && value == ID_STRING;
}

/* As set_adr(): the card's memory size register must be written before its id can be read at
 * the top of a 4 MB card. */
static bool find_card(uint32_t *base, uint32_t *extra)
{
	uint32_t value;

	*base = BASE_A;
	if (probe_long(BASE_A + MEGA + VTG, &value)) {
		*(volatile uint16_t *) (BASE_A + MEGA + VTG + 2 * VTG_MEM_REG) = 1;
		*extra = 3 * MEGA;
		if (has_id(BASE_A, *extra))
			return true;
		*extra = MEGA;
		return has_id(BASE_A, *extra);
	}
	*base = BASE_C;
	*extra = MEGA;
	if (!probe_long(BASE_C + MEGA + VTG, &value))
		return false;
	*(volatile uint16_t *) (BASE_C + MEGA + VTG + 2 * VTG_MEM_REG) = 1;
	return has_id(BASE_C, *extra);
}

static void set_registers(const uint16_t *values, uint16_t ctrl)
{
	vtg[VTG_CTRL] = 0;
	/* the PLL's output divider goes last of the PLL's, as in set_fpga() */
	for (int i = VTG_PLL_FBD; i <= VTG_PLL_OD; i++)
		vtg[i] = values[i];
	for (int i = VTG_HFP; i <= VTG_VDI; i++)
		vtg[i] = values[i];
	vtg[VTG_VMEM_LOW] = values[VTG_VMEM_LOW];
	vtg[VTG_VMEM_HIGH] = values[VTG_VMEM_HIGH];
	vtg[VTG_CTRL] = ctrl;
}

bool atw800_start(void)
{
	uint32_t base, extra;
	const atw_info *info;
	uint16_t registers[VTG_MEM_REG];

	if (!find_card(&base, &extra))
		return false;
	memory = (uint8_t *) base;
	vtg = (volatile uint16_t *) (base + extra + VTG);
	lut = (volatile uint16_t *) (base + extra + LUT);
	for (int i = 0; i < 256; i++) {
		doubled[i] = (uint16_t) (i << 8 | i);
		old_lut[i] = lut[i];
	}
	info = (const atw_info *) (base + extra + ATW_INFO);
	if (info->id_a == UINT32_C(0x41545738) && info->endmagic == 0x4711) {     /* "ATW8" */
		for (int i = 0; i < VTG_MEM_REG; i++)
			old_vtg[i] = info->vtg[i];
		old_screen = malloc(CARD_BYTES);
		if (old_screen == NULL)
			plat_fatal("Out of memory for the graphics card's screen.");
		memcpy(old_screen, memory, CARD_BYTES);
	}
	memset(memory, 0, CARD_BYTES);
	memset(registers, 0, sizeof registers);
	memcpy(registers, game_mode, sizeof game_mode);
	set_registers(registers, GAME_CTRL);
	return true;
}

void atw800_stop(void)
{
	if (old_screen == NULL) {
		vtg[VTG_CTRL] = 0;
	} else {
		memcpy(memory, old_screen, CARD_BYTES);
		free(old_screen);
		old_screen = NULL;
		set_registers(old_vtg, old_vtg[VTG_CTRL]);
	}
	for (int i = 0; i < 256; i++)
		lut[i] = old_lut[i];
}

static uint16_t lut_word(const uint8_t *rgb)
{
	return (uint16_t) ((rgb[0] & 0x3e) << 10 | (rgb[1] & 0x3f) << 5 | (rgb[2] & 0x3e) >> 1);
}

void atw800_palette(int16_t first, int16_t count, const uint8_t *rgb)
{
	for (int16_t i = 0; i < count; i++)
		lut[first + i] = lut_word(rgb + 3 * i);
}

/* Writes the groups of 16 pixels that differ from shadow, or all of them, each pixel as four. */
void atw800_frame(const uint8_t *pixels, uint8_t *shadow, bool full)
{
	volatile uint32_t *line = (volatile uint32_t *) memory;

	for (int y = 0; y < HEIGHT; y++, line += 2 * CARD_WIDTH / 4) {
		const uint32_t *in = (const uint32_t *) (const void *) (pixels + y * WIDTH);
		uint32_t *seen = (uint32_t *) (void *) (shadow + y * WIDTH);

		for (int group = 0; group < WIDTH / 16; group++, in += 4, seen += 4) {
			const uint8_t *p = (const uint8_t *) in;
			volatile uint32_t *out = line + 8 * group;

			if (!full && in[0] == seen[0] && in[1] == seen[1] && in[2] == seen[2] && in[3] == seen[3])
				continue;
			seen[0] = in[0];
			seen[1] = in[1];
			seen[2] = in[2];
			seen[3] = in[3];
			for (int i = 0; i < 8; i++, p += 2) {
				uint32_t wide = (uint32_t) doubled[p[0]] << 16 | doubled[p[1]];

				out[i] = wide;
				out[i + CARD_WIDTH / 4] = wide;
			}
		}
	}
}

#ifdef U7_DEBUG_LOG
/* For scripted tests: logs how far the card's screen and colours differ from what it should show. */
void atw800_check(const uint8_t *pixels, const uint8_t *colours)
{
	uint32_t wrong_pixels = 0, wrong_colours = 0;
	char text[64];

	for (int y = 0; y < 2 * HEIGHT; y++)
		for (int x = 0; x < CARD_WIDTH; x++) {
			uint8_t pixel = pixels[y / 2 * WIDTH + x / 2];

			if (memory[y * CARD_WIDTH + x] != pixel)
				wrong_pixels++;
			if (lut[pixel] != lut_word(colours + 3 * pixel))
				wrong_colours++;
		}
	snprintf(text, sizeof text, "card check: %lu pixels, %lu colours wrong\r\n",
		(unsigned long) wrong_pixels, (unsigned long) wrong_colours);
	plat_log(text);
}
#endif
