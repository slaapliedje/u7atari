/* The PC lockstep reference: the same game code, built for the PC with no window or sound, always
 * in lockstep (common/lockstep.c), driven by a script. Its log is what the Atari build's is
 * compared with (tools/lockstep-compare.sh). It mirrors the Atari backend's behaviour wherever
 * the game could tell them apart: keep the two in step.
 *
 *   u7host [--program u7] [--gm] --data <folder> --script <file> [game arguments]
 *
 * The data folder becomes the current folder, as on the Atari, where the game runs from its own.
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "plat.h"
#include "backend.h"
#include "game.h"

static jmp_buf *program_jump;
static int16_t program_code;
static char *self_name = GAME_PRG;
static bool machine_taken;

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

static void end_process(int code)
{
	char text[32];

	snprintf(text, sizeof text, "exit %d\r\n", code);
	plat_log(text);
	lockstep_end();
	shutdown_all();
	fflush(NULL);
	/* As the Atari backend: destroying the game's static objects crashes. */
	_exit(code);
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
	fprintf(stdout, "u7: %s\n", message);
	fflush(NULL);
	_exit(1);
}

void plat_log(const char *text)
{
	fputs(text, stdout);
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

static const char *program_name;
static const char *script_path;

static const char *take_options(int *argc, char **argv)
{
	const char *dir = ".";
	int kept = 1;

	for (int i = 1; i < *argc; i++) {
		if (strcmp(argv[i], "--data") == 0 && i + 1 < *argc)
			dir = argv[++i];
		else if (strcmp(argv[i], "--program") == 0 && i + 1 < *argc)
			program_name = argv[++i];
		else if (strcmp(argv[i], "--gm") == 0)
			sound_gm = true;
		else if (strcmp(argv[i], "--script") == 0 && i + 1 < *argc)
			script_path = argv[++i];
		else if (strcmp(argv[i], "--lockstep") == 0)
			;
		else
			argv[kept++] = argv[i];
	}
	*argc = kept;
	argv[kept] = NULL;
	return dir;
}

int main(int argc, char **argv)
{
	char problem[1024];
	int16_t code;
	const char *dir;

	setvbuf(stdout, NULL, _IOLBF, 0);
	dir = take_options(&argc, argv);
	if (chdir(dir) != 0) {
		fprintf(stderr, "u7host: cannot enter %s\n", dir);
		return 1;
	}
	argv[0] = self_name;
	files_set_root(".");
	if (!files_check_data(problem, sizeof problem))
		plat_fatal(problem);

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
	return code;
}
