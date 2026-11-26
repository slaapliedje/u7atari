/* The Jaguar's start: as the PC lockstep reference's main (platform/host/main.c), with the
 * GameDrive's BIOS for files and the log, and the arguments from a file on the card.
 *
 * The card holds the game in U7 (SI for Serpent Isle): STATIC, an empty GAMEDAT, and
 * ULTIMA7.ARG (SERPENT.ARG), one line of arguments as the Atari's: --script <file> plays a
 * test script (TEST.SCR), --program <name> runs that program instead of the menu, and the rest
 * go to the game; --lockstep runs it on the PC reference's virtual clock, for comparing a run
 * with the PC's (else it runs in real time, played with joypad 1); --bench is --lockstep without
 * the checksums, to time a run that does the same whatever the speed. The log (every plat_log line, the lockstep checksums among them) goes out
 * through GD_DebugString: the drive's USB serial port, or the libretro Virtual Jaguar's stderr. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat.h"
#include "backend.h"
#include "game.h"
#include "gd.h"

#ifdef U7_SERPENT_ISLE
#define CARD_FOLDER "SI"
#else
#define CARD_FOLDER "U7"
#endif
#define MAX_ARGS 16

static uint32_t gd_bios[1024];
extern bool jag_bench;                  /* jaguar.c */
static bool gd_ready;
static jmp_buf *program_jump;
static int16_t program_code;
static char *self_name = GAME_PRG;
static bool machine_taken;

static void halt(void)
{
	__asm__ volatile ("move.w #0x2700, %%sr" ::: "cc");
	for (;;)
		;
}

static void shutdown_all(void)
{
	if (!machine_taken)
		return;
	machine_taken = false;
	sound_shutdown();
	input_shutdown();
	video_shutdown();
	timer_shutdown();
}

/* --profile: the pc and the top of the stack at every 64th vertical blank (startup.S), saved to
 * PCPROF.BIN on the card at the end: records of 33 big-endian longs, for tools/jag-profile.py. */
#define PROFILE_SAMPLES 1024
#define PROFILE_RECORD 132
extern uint8_t *volatile jag_prof_next;
extern uint8_t *volatile jag_prof_end;
extern volatile uint32_t jag_prof_mask;
static uint8_t *profile_buffer;

static void profile_save(void)
{
	uint8_t *end = jag_prof_next;
	int16_t f;

	if (profile_buffer == NULL)
		return;
	jag_prof_next = NULL;
	if ((f = plat_file_create("PCPROF.BIN")) >= 0) {
		plat_file_write(f, profile_buffer, (int32_t) (end - profile_buffer));
		plat_file_close(f);
	}
}

static void end_process(int code)
{
	char text[32];

	profile_save();

	snprintf(text, sizeof text, "exit %d\r\n", code);
	plat_log(text);
	if (lockstep)
		lockstep_end();
	shutdown_all();
	halt();
}

void backend_quit(int code)
{
	end_process(code);
}

void plat_exit(int16_t code)
{
	if (program_jump != NULL) {
		timer_drop_sound_lock();
		program_code = (int16_t) (code & 0xff);
		longjmp(*program_jump, 1);
	}
	end_process(code);
}

void plat_fatal(const char *message)
{
	shutdown_all();
	plat_log("u7: ");
	plat_log(message);
	plat_log("\r\n");
	halt();
}

/* From startup.S, for a bus or address error, an illegal instruction and the like: the
 * registers (d1 already changed) and where it happened, to the log, then a stop. */
void jag_crash(int32_t long_frame, const uint16_t *frame)
{
	static const char *const names[] = { "", "", "bus error", "address error",
		"illegal instruction", "division by zero", "CHK", "TRAPV", "privilege violation",
		"trace", "line A", "line F" };
	const uint32_t *regs = (const uint32_t *) (const void *) frame - 15;
	uint16_t vector = frame[0];
	uint32_t pc, address = 0;
	uint16_t sr;
	char text[96];

	if (long_frame) {
		address = (uint32_t) frame[2] << 16 | frame[3];
		sr = frame[5];
		pc = (uint32_t) frame[6] << 16 | frame[7];
	} else {
		sr = frame[1];
		pc = (uint32_t) frame[2] << 16 | frame[3];
	}
	snprintf(text, sizeof text, "crash: %s at pc %08lx, address %08lx, sr %04x\r\n",
		vector < 12 ? names[vector] : "?", (unsigned long) pc, (unsigned long) address, sr);
	plat_log(text);
	for (int i = 0; i < 15; i += 4) {
		snprintf(text, sizeof text, "%c%d-%d: %08lx %08lx %08lx %08lx\r\n", i < 8 ? 'd' : 'a',
			i & 7, (i & 7) + 3, (unsigned long) regs[i], (unsigned long) regs[i + 1],
			(unsigned long) regs[i + 2], i + 3 < 15 ? (unsigned long) regs[i + 3] : 0UL);
		plat_log(text);
	}
	/* the frame, then the stack above it: return addresses into the cartridge are $80xxxx */
	for (int i = 0; i < 256 && (uint32_t) (frame + i) < 0x200000; i += 8) {
		const uint16_t *w = frame + i;

		snprintf(text, sizeof text, "+%02d: %04x%04x %04x%04x %04x%04x %04x%04x\r\n", 2 * i,
			w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
		plat_log(text);
	}
	halt();
}

/* A line at a time: the drive's log takes text up to a NUL. */
void plat_log(const char *text)
{
	if (gd_ready)
		GD_DebugString(text);
}

int16_t plat_run_program(const char *name, int16_t argc, char **argv)
{
	jmp_buf jump, *outer = program_jump;
	char **args = calloc((size_t) argc + 2, sizeof (char *));

	args[0] = self_name;
	memcpy(args + 1, argv, (size_t) argc * sizeof *argv);

	input_reset();
	video_reset();
	sound_reset();
	files_reset();
	console_reset();
	ResetEnvironment();

	program_jump = &jump;
	if (setjmp(jump) == 0)
		program_code = ProgramMain(name, (int16_t) (argc + 1), args);
	program_jump = outer;
	free(args);
	return program_code;
}

/* The argument file's words, in argv from argv[1]; returns argc. \ooo in a word is a byte, as
 * the scripts' #@args lines write the cheat password's 0xFF. */
static int read_arguments(char **argv)
{
	static char line[256];
	char *p = line;
	int argc = 1;
	int16_t f = plat_file_open(GAME_ARG_FILE, PLAT_FILE_READ);
	int32_t n;

	if (f < 0)
		return argc;
	n = plat_file_read(f, line, sizeof line - 1);
	plat_file_close(f);
	line[n > 0 ? n : 0] = '\0';
	while (*p != '\0' && argc < MAX_ARGS - 1) {
		char *out;

		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
			p++;
		if (*p == '\0')
			break;
		argv[argc++] = out = p;
		while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
			if (p[0] == '\\' && p[1] >= '0' && p[1] <= '7' && p[2] >= '0' && p[2] <= '7'
				&& p[3] >= '0' && p[3] <= '7') {
				*out++ = (char) ((p[1] - '0') << 6 | (p[2] - '0') << 3 | (p[3] - '0'));
				p += 4;
			} else {
				*out++ = *p++;
			}
		}
		if (*p != '\0')
			p++;
		*out = '\0';
	}
	argv[argc] = NULL;
	return argc;
}

void jag_main(void)
{
	static char *argv[MAX_ARGS];
	const char *program_name = NULL, *script_path = NULL;
	bool use_lockstep = false;
	char problem[160];
	int argc, kept = 1;
	int16_t code;

	gd_ready = GD_Install(gd_bios) == 0;
	plat_log(GAME_NAME " on the Jaguar\r\n");
	files_set_root(CARD_FOLDER);
	if (!files_check_data(problem, sizeof problem))
		plat_fatal(problem);

	argc = read_arguments(argv);
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--program") == 0 && i + 1 < argc)
			program_name = argv[++i];
		else if (strcmp(argv[i], "--script") == 0 && i + 1 < argc)
			script_path = argv[++i];
		else if (strcmp(argv[i], "--gm") == 0)
			sound_gm = true;
		else if (strcmp(argv[i], "--lockstep") == 0)
			use_lockstep = true;
		else if (strcmp(argv[i], "--profile") == 0) {
			profile_buffer = malloc(PROFILE_SAMPLES * PROFILE_RECORD);
			if (profile_buffer != NULL) {
				jag_prof_mask = 63;
				jag_prof_end = profile_buffer + PROFILE_SAMPLES * PROFILE_RECORD;
				jag_prof_next = profile_buffer;
			}
		} else if (strcmp(argv[i], "--bench") == 0) {
			use_lockstep = true;
			jag_bench = true;
		}
		else if (strcmp(argv[i], "--natfeats") == 0)
			;
		else
			argv[kept++] = argv[i];
	}
	argc = kept;
	argv[argc] = NULL;
	argv[0] = self_name;

	if (use_lockstep)
		lockstep_start();
	machine_taken = true;
	timer_init();
	video_init();
	input_init();
	sound_init();

	if (script_path != NULL)
		script_load(script_path);
	if (program_name != NULL)
		code = plat_run_program(program_name, (int16_t) (argc - 1), argv + 1);
	else
		code = GAME_MAIN((int16_t) argc, argv);
	end_process(code);
}
