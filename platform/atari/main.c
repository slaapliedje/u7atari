/* Process control: main(), the program runner and exit paths.
 *
 * TOS has no threads, so a program runs on the caller's stack. plat_run_program sets a
 * jump point and plat_exit returns to it without unwinding, as a DOS EXE's exit did.
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mint/cookie.h>
#include <mint/osbind.h>

#include "plat.h"
#include "backend.h"

machine_type atari_machine = MACHINE_OTHER;

static jmp_buf *program_jump;
static int16_t program_code;
static char *self_name = "ULTIMA7.PRG";

static void shutdown_all(void)
{
	sound_shutdown();
	input_shutdown();
	video_shutdown();
	timer_shutdown();
}

static void end_process(int code)
{
	shutdown_all();
	fflush(NULL);
	exit(code);
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
	exit(1);
}

void plat_log(const char *text)
{
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

static const char *program_name;

/* Takes the backend's own options out of the arguments:
 *   --data <folder>     the game folder; U7_DATA is the default, then the current folder,
 *                       which the desktop sets to the program's own
 *   --program <name>    runs just that program ("u7", "intro", "mainmenu", "endgame")
 *                       instead of the launcher
 *   --line-in           Falcon: mixes the input jack into the sound output, for a MIDI synth
 *                       plugged in there */
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
		else
			argv[kept++] = argv[i];
	}
	*argc = kept;
	argv[kept] = NULL;
	return dir != NULL && dir[0] != '\0' ? dir : ".";
}

/* Started with no arguments (from the desktop, or Hatari's --auto), the program takes them
 * from ULTIMA7.ARG in its folder when there is one: words separated by blanks or lines.
 * Renaming the program to ULTIMA7.TTP asks for them instead. */
static char **arguments_from_file(int *argc, char **argv)
{
	static char text[512];
	static char *words[32];
	FILE *f = fopen("ULTIMA7.ARG", "r");
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
		plat_fatal("Ultima VII needs a TT030 or Falcon030.");
	files_set_root(take_options(&argc, argv));
	if (!files_check_data(problem, sizeof problem))
		plat_fatal(problem);

	/* The game owns the machine: video, IKBD, MFP timers and the sound DMA. */
	Super(0L);
	timer_init();
	video_init();
	input_init();
	sound_init();

#ifdef U7_DEBUG_LOG
	if (program_name != NULL && strcmp(program_name, "pcmtest") == 0) {
		pcm_test();
		end_process(0);
	}
#endif
	if (program_name != NULL)
		code = plat_run_program(program_name, (int16_t) (argc - 1), argv + 1);
	else
		code = Ultima7Main((int16_t) argc, argv);
	end_process(code);
	return code;
}
