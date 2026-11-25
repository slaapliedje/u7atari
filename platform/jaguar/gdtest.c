/* A cartridge that checks the GameDrive's file calls: it writes U7TEST.TXT on the SD card,
 * reads it back, and reports each step with GD_DebugString (the drive's USB log; the libretro
 * Virtual Jaguar prints it as "[JGD] debug: ..."). Built as the jaguar_gdtest target. */
#include <stdint.h>
#include <string.h>
#include "gd.h"

static uint32_t gd_bios[1024];
static char line[96];

static void put_hex(char *out, uint32_t value)
{
	int i;

	for (i = 7; i >= 0; i--) {
		out[i] = "0123456789abcdef"[value & 15];
		value >>= 4;
	}
	out[8] = 0;
}

static void report(const char *what, uint32_t value)
{
	size_t n = strlen(what);

	memcpy(line, what, n);
	line[n] = ' ';
	put_hex(line + n + 1, value);
	line[n + 9] = '\n';
	line[n + 10] = 0;
	GD_DebugString(line);
}

/* startup.S's exception handler: here only said, and stopped */
void jag_crash(int32_t long_frame, const uint16_t *frame)
{
	(void) long_frame;
	report("crash, vector", frame[0]);
	for (;;)
		;
}

void jag_main(void)
{
	static const char text[] = "Ultima VII on the Jaguar: a GameDrive file test.\n";
	static char back[64];
	uint16_t f, r;

	r = GD_Install(gd_bios);
	report("install", r);
	report("hardware", GD_HWVersion());
	f = GD_FOpen("U7TEST.TXT", GD_FOPEN_WRITE | GD_FOPEN_CREATE_ALWAYS);
	report("open for writing", f);
	report("write", GD_FWrite(f, text, sizeof text - 1));
	report("close", GD_FClose(f));
	f = GD_FOpen("U7TEST.TXT", GD_FOPEN_READ);
	report("open for reading", f);
	report("size", GD_FSize(f));
	memset(back, 0, sizeof back);
	report("read", GD_FRead(f, back, sizeof text - 1, GD_FREAD_CPU));
	report("tell", GD_FTell(f));
	report("close", GD_FClose(f));
	report(memcmp(back, text, sizeof text - 1) == 0 ? "the same: yes" : "the same: NO", 0);
	/* as plat_file_create and stdio's "wb" open: read and write, made anew */
	f = GD_FOpen("U7TEST2.TXT", GD_FOPEN_READ | GD_FOPEN_WRITE | GD_FOPEN_CREATE_ALWAYS);
	report("open read+write, new", f);
	report("write", GD_FWrite(f, text, sizeof text - 1));
	report("close", GD_FClose(f));
	f = GD_FOpen("SUB/U7TEST3.TXT", GD_FOPEN_WRITE | GD_FOPEN_CREATE_ALWAYS);
	report("open in a folder", f);
	report("close", GD_FClose(f));
	GD_DebugString("done\n");
	for (;;)
		;
}
