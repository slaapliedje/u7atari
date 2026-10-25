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
		program_code = (int16_t) (code & 0xff);
		longjmp(*program_jump, 1);
	}
	end_process(code);
}

void plat_fatal(const char *message)
{
	shutdown_all();
	fprintf(stderr, "u7: %s\r\n", message);
	fflush(NULL);
	exit(1);
}

void plat_log(const char *text)
{
	fputs(text, stderr);
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

int main(int argc, char **argv)
{
	int16_t code;

	detect_machine();
	if (atari_machine == MACHINE_OTHER) {
		fputs("Ultima VII needs a TT030 or Falcon030.\r\n", stderr);
		return 1;
	}
	/* The game owns the machine: video, IKBD, MFP timers and the sound DMA. */
	Super(0L);
	files_init(NULL);
	timer_init();
	video_init();
	input_init();
	sound_init();

	code = Ultima7Main((int16_t) argc, argv);
	end_process(code);
	return code;
}
