/* Video: the game draws a 320x200 chunky 8-bit frame; both machines show 256 colours only
 * as 8 interleaved bitplanes, so every present is a chunky-to-planar conversion into
 * ST RAM (the only RAM the shifter/VIDEL can fetch from).
 *
 *   TT030:    TT low, 320x480x8. Each game line is written twice (400 lines), letterboxed.
 *   Falcon:   VIDEL 320x200x8 with line doubling on VGA (Vsetmode), 320x200 on RGB/TV.
 *
 * Palette: 6-bit VGA triples. TT palette is 12-bit (4 bits/gun) at 0xFF8400; Falcon is
 * 18-bit (6 bits/gun, VGA-exact) at 0xFF9800.
 *
 * TODO: Mxalloc(ST-RAM) double-buffered screens, c2p.S (68030 move.l merge c2p), dirty
 * line tracking against the last frame, VBL-synced palette upload.
 */
#include "plat.h"
#include "backend.h"

void video_init(void) {}
void video_shutdown(void) {}
void video_reset(void) {}

void plat_video_present(const uint8_t *pixels)
{
	(void) pixels;
}

void plat_video_set_palette(int16_t first, int16_t count, const uint8_t *rgb)
{
	(void) first; (void) count; (void) rgb;
}

void plat_video_wait_retrace(void)
{
	/* TODO: wait for the next VBL (the game expects ~70 Hz; TT low and Falcon VGA run ~60). */
}

/* ---- Console text (8x8 font on a 40x25 grid, drawn into the frame) ---- */

void console_reset(void) {}

void plat_console_goto(int16_t x, int16_t y)
{
	(void) x; (void) y;
}

void plat_console_write(const char *text)
{
	/* TODO: render with vgafont8.h from the upstream matoya backend. */
	plat_log(text);
}
