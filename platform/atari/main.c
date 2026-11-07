/* Process control: main(), the program runner and exit paths.
 *
 * TOS has no threads, so a program runs on the caller's stack. plat_run_program sets a
 * jump point and plat_exit returns to it without unwinding, as a DOS EXE's exit did.
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mint/basepage.h>
#include <mint/cookie.h>
#include <mint/osbind.h>

#include "plat.h"
#include "backend.h"
#include "game.h"

machine_type atari_machine = MACHINE_OTHER;

/* MiNTLib's stack size; its default is 64 KB. The Black Gate's endgame needs over 64 KB (it
 * overran into MiNTLib's variables and crashed in getenv), so take plenty: the PC's is 8 MB. */
long _stksize = 256L * 1024;

#ifdef U7_DEBUG_LOG
/* Hatari's NatFeats (--natfeats, under Hatari's own --natfeats on only: the opcodes are illegal
 * instructions anywhere else): the log goes to Hatari's stderr, and leaving the game quits
 * Hatari. nf_id is opcode $7300 and nf_call $7301, with C arguments on the stack. */
__asm__(".text\n.even\n_nf_id:\n\t.dc.w 0x7300\n\trts\n_nf_call:\n\t.dc.w 0x7301\n\trts\n");
extern long nf_id(const char *name) __asm__("_nf_id");
extern long nf_call(long id, ...) __asm__("_nf_call");
static long nf_stderr, nf_shutdown;
#endif

static jmp_buf *program_jump;
static int16_t program_code;
static char *self_name = GAME_PRG;
static bool machine_taken;     /* in supervisor mode, setting up or owning the hardware */

/* Replaces MiNTLib's check, run from its startup code, that stops a program built for the
 * 68020-60 libraries on a machine without a 68881/68882. The game is built with -msoft-float,
 * and the library's floating point is only in printf's %f, which it does not use; main()
 * checks the machine. */
void _checkcpu(void)
{
}

static void shutdown_all(void)
{
	/* before that, the hardware is untouched, and user mode cannot reach it */
	if (!machine_taken)
		return;
	machine_taken = false;
	sound_shutdown();
	input_shutdown();
	video_shutdown();
	timer_shutdown();
}

#ifdef U7_DEBUG_LOG
static long nf_quit(void)
{
	return nf_call(nf_shutdown);
}
#endif

static void end_process(int code)
{
#ifdef U7_DEBUG_LOG
	char text[32];

	snprintf(text, sizeof text, "exit %d\r\n", code);
	plat_log(text);
	if (lockstep)
		lockstep_end();
#endif
	shutdown_all();
	video_fps_report();
	fflush(NULL);
#ifdef U7_DEBUG_LOG
	if (nf_shutdown != 0)
		Supexec(nf_quit);
#endif
	/* Not exit(): the game's static objects hold what its programs left behind, and destroying
	 * them crashes (MainMenu's device list, after a program has ended). */
	_exit(code);
}

void backend_quit(int code)
{
	end_process(code);
}

void plat_exit(int16_t code)
{
	if (program_jump != NULL) {
		/* As a DOS exit: the program's stack is never unwound, nor its locks let go. */
		timer_drop_sound_lock();
		program_code = (int16_t) (code & 0xff);
		longjmp(*program_jump, 1);
	}
	end_process(code);
}

/* Console text for TOS, which needs \r before each \n. */
static void console_text(const char *text)
{
	for (; *text != '\0'; text++) {
		if (*text == '\n')
			Cconout('\r');
		Cconout(*text);
	}
}

void plat_fatal(const char *message)
{
	shutdown_all();
	console_text("u7: ");
	console_text(message);
	console_text("\n\nPress any key.\n");
	Cnecin();
	fflush(NULL);
	_exit(1);
}

void plat_log(const char *text)
{
#ifdef U7_DEBUG_LOG
	if (nf_stderr != 0) {
		nf_call(nf_stderr, text);
		return;
	}
#endif
	console_text(text);
}

int16_t plat_run_program(const char *name, int16_t argc, char **argv)
{
	jmp_buf jump, *outer = program_jump;
	char **args = calloc((size_t) argc + 2, sizeof (char *));

	/* The program sees this executable as its argv[0], as a DOS program saw its EXE. */
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

static void detect_machine(void)
{
	long mch = 0;

	if (Getcookie(C__MCH, &mch) != C_FOUND)
		mch = 0;
	switch (mch >> 16) {
	case 2: atari_machine = MACHINE_TT; break;
	case 3: atari_machine = MACHINE_FALCON; break;
	default: atari_machine = MACHINE_OTHER; break;
	}
}

/* The game's arena (LINEAR_MEMORY_SIZE, set in CMakeLists.txt) and its other blocks, about
 * 1.3 MB more, come from fast RAM when there is enough, else from ST RAM, which is slower: on a
 * Falcon it has a 16-bit bus shared with the video. A Falcon's accelerator board may have fast
 * RAM that TOS has not been told of until the MAPROM program from github.com/agranlund/tftools
 * runs (the TF53x and DFB1X boards both use it; the DFB1X's package calls it MAPROM_C.PRG).
 * Booting with Control held skips the AUTO folder, so when fast RAM is missing the game runs
 * MAPROM from there itself (it stays resident, as after a normal boot), then runs a second copy
 * of the game, whose code TOS can now load into fast RAM. */
#define MEMORY_NEEDED (LINEAR_MEMORY_SIZE + (UINT32_C(2) << 20))

static long free_fast_ram(void)
{
	return (long) Mxalloc(-1L, 1);
}

static long free_st_ram(void)
{
	return (long) Mxalloc(-1L, 0);
}

static long read_boot_drive(void)
{
	return *(volatile int16_t *) 0x446;   /* _bootdev */
}

/* Runs the first \AUTO\MAPROM*.PRG on the boot drive; false if there is none. */
static bool run_maprom(void)
{
	char path[160];
	_DTA dta, *old_dta = Fgetdta();
	int drive = (int) Supexec(read_boot_drive);
	long found;

	/* A DTA of our own: the default one is the command line in the basepage, which
	 * check_memory() passes on, and the search would fill it with "MAPROM??PRG" (? = --version). */
	Fsetdta(&dta);
	snprintf(path, sizeof path, "%c:\\AUTO\\MAPROM*.PRG", 'A' + drive);
	found = Fsfirst(path, 0);
	Fsetdta(old_dta);
	if (found != 0)
		return false;
	snprintf(path, sizeof path, "%c:\\AUTO\\%s", 'A' + drive, dta.dta_name);
	console_text("u7: no fast RAM; running ");
	console_text(path);
	console_text("\n");
	Pexec(0, path, "\0", NULL);
	return true;
}

static void check_memory(void)
{
	char text[200];
	long st;

	if (free_fast_ram() >= (long) MEMORY_NEEDED)
		return;
	if (run_maprom() && free_fast_ram() >= (long) MEMORY_NEEDED) {
		/* the same command line; a game run from the desktop has the game's folder current */
		long code = Pexec(0, GAME_PRG, _base->p_cmdlin, NULL);
		if (code < 0)
			plat_fatal("Could not run " GAME_PRG " again after MAPROM.");
		exit((int) code);
	}
	st = free_st_ram();
	if (st >= (long) MEMORY_NEEDED)
		return;
	snprintf(text, sizeof text, GAME_NAME " needs %lu KB of free memory, and TOS has %ld KB.",
		(unsigned long) (MEMORY_NEEDED >> 10), (st > free_fast_ram() ? st : free_fast_ram()) >> 10);
	plat_fatal(text);
}

static const char *program_name;
#ifdef U7_DEBUG_LOG
static const char *script_path;
#endif

/* Takes the backend's own options out of the arguments:
 *   --data <folder>     the game folder; U7_DATA is the default, then the current folder,
 *                       which the desktop sets to the program's own
 *   --program <name>    runs just that program ("u7" or "si", "intro", "mainmenu",
 *                       "endgame") instead of the launcher
 *   --line-in           Falcon: mixes the input jack into the sound output, for a MIDI synth
 *                       plugged in there
 *   --gm                music and effects for a General MIDI synth instead of an MT-32
 *   --no-card           the TT's own video even with an ATW800/2 graphics card in the machine
 *   --fps               counts the frames shown, and writes them to U7FPS.TXT in the current
 *                       folder on leaving
 *   --script <file>     debug builds: scripted keys and mouse (script.c)
 *   --lockstep          debug builds: the virtual clock and checksums of lockstep.c
 *   --natfeats          debug builds, under Hatari with --natfeats on: log to Hatari's stderr,
 *                       and quit Hatari on leaving */
static const char *take_options(int *argc, char **argv)
{
	const char *dir = getenv("U7_DATA");
	int kept = 1;

	for (int i = 1; i < *argc; i++) {
		if (strcmp(argv[i], "--data") == 0 && i + 1 < *argc)
			dir = argv[++i];
		else if (strcmp(argv[i], "--program") == 0 && i + 1 < *argc)
			program_name = argv[++i];
		else if (strcmp(argv[i], "--line-in") == 0)
			sound_line_in = true;
		else if (strcmp(argv[i], "--gm") == 0)
			sound_gm = true;
		else if (strcmp(argv[i], "--no-card") == 0)
			video_no_card = true;
		else if (strcmp(argv[i], "--fps") == 0)
			video_fps = true;
#ifdef U7_DEBUG_LOG
		else if (strcmp(argv[i], "--script") == 0 && i + 1 < *argc)
			script_path = argv[++i];
		else if (strcmp(argv[i], "--lockstep") == 0)
			lockstep = true;
		else if (strcmp(argv[i], "--natfeats") == 0) {
			nf_stderr = nf_id("NF_STDERR");
			nf_shutdown = nf_id("NF_SHUTDOWN");
		}
#endif
		else
			argv[kept++] = argv[i];
	}
	*argc = kept;
	argv[kept] = NULL;
	return dir != NULL && dir[0] != '\0' ? dir : ".";
}

/* Started with no arguments (from the desktop, or Hatari's --auto), the program takes them
 * from ULTIMA7.ARG (SERPENT.ARG) in its folder when there is one: words separated by blanks or
 * lines. Renaming the program to .TTP asks for them instead. */
static char **arguments_from_file(int *argc, char **argv)
{
	static char text[512];
	static char *words[32];
	FILE *f = fopen(GAME_ARG_FILE, "r");
	size_t length;
	int count = 1;

	if (f == NULL)
		return argv;
	length = fread(text, 1, sizeof text - 1, f);
	fclose(f);
	text[length] = '\0';
	words[0] = argv[0];
	for (char *w = strtok(text, " \t\r\n"); w != NULL && count < 31; w = strtok(NULL, " \t\r\n"))
		words[count++] = w;
	words[count] = NULL;
	*argc = count;
	return words;
}

#ifdef U7_DEBUG_LOG
void save_test(void);     /* tests/save_test.cpp */

/* --program pcmtest: 6 s of the intro's speech (STATIC\INTROSND.DAT from its first sample)
 * through the speech path, queued steadily in the intro's 1298-byte halves, then 1 s of
 * silence; for recording under Hatari and comparing with the file. */
static void pcm_test(void)
{
	static uint8_t speech[6 * 10752];
	int32_t queued = 0, have = 0;
	uint32_t until;
	FILE *f = fopen("STATIC\\INTROSND.DAT", "rb");

	if (f != NULL) {
		fseek(f, 32, SEEK_SET);
		have = (int32_t) fread(speech, 1, sizeof speech, f);
		fclose(f);
	}
	plat_log("pcmtest\r\n");
	plat_pcm_start(10752);
	until = plat_milliseconds() + 7000;
	while ((int32_t) (until - plat_milliseconds()) > 0) {
		if (queued < have && plat_pcm_pending() < 2 * 1298)
			queued += plat_pcm_queue(speech + queued, have - queued < 1298 ? have - queued : 1298);
		plat_pump();
	}
	plat_log("pcmtest done\r\n");
}
#endif

int main(int argc, char **argv)
{
	char problem[1024];
	int16_t code;

	if (argc <= 1)
		argv = arguments_from_file(&argc, argv);
	detect_machine();
	if (atari_machine == MACHINE_OTHER)
		plat_fatal(GAME_NAME " needs a TT030 or Falcon030.");
	check_memory();
	files_set_root(take_options(&argc, argv));
	if (!files_check_data(problem, sizeof problem))
		plat_fatal(problem);

#ifdef U7_DEBUG_LOG
	if (lockstep) {
		lockstep_start();
		argv[0] = self_name;    /* as the PC reference has it */
	}
#endif
	/* The game owns the machine: video, IKBD, MFP timers and the sound DMA. */
	Super(0L);
	machine_taken = true;
	timer_init();
	video_init();
	input_init();
	sound_init();

#ifdef U7_DEBUG_LOG
	if (script_path != NULL)
		script_load(script_path);
	if (program_name != NULL && strcmp(program_name, "pcmtest") == 0) {
		pcm_test();
		end_process(0);
	}
	if (program_name != NULL && strcmp(program_name, "savetest") == 0) {
		save_test();
		end_process(0);
	}
#endif
	if (program_name != NULL)
		code = plat_run_program(program_name, (int16_t) (argc - 1), argv + 1);
	else
		code = GAME_MAIN((int16_t) argc, argv);
	end_process(code);
	return code;
}
