/* newlib's system calls on the Jaguar (this newlib calls them without the underscore): the heap between .bss and the stack (jaguar.ld), files
 * on the GameDrive's card for stdio (the test script, read with fopen), and stdout and stderr to
 * plat_log. The game's own files go through files.c. */
#include <errno.h>
#include <stddef.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "plat.h"
#include "backend.h"
#include "gd.h"

#define FIRST_FILE 3
#define MAX_FILES 2                     /* the drive keeps 8 open; files.c takes 6 */
#define FAILED(r) ((int16_t) (r) < 0)
/* The flags libc's fopen passes to open: newlib's own (_FCREAT, _FTRUNC), not the O_CREAT and
 * O_TRUNC this toolchain's fcntl.h gives (4 and 0x10), which its libc was not built with. */
#define LIBC_CREAT 0x0200
#define LIBC_TRUNC 0x0400

extern char __heap_start[], __heap_end[];
static char *heap_top = __heap_start;

void *sbrk(ptrdiff_t increment)
{
	char *old = heap_top;

	if (increment > __heap_end - heap_top || increment < __heap_start - heap_top) {
		errno = ENOMEM;
		return (void *) -1;
	}
	heap_top += increment;
	return old;
}

/* how much of the heap is taken, for the script's mem step */
uint32_t jag_heap_used(void)
{
	return (uint32_t) (heap_top - __heap_start);
}

uint32_t jag_heap_size(void)
{
	return (uint32_t) (__heap_end - __heap_start);
}

static struct {
	bool open;
	uint16_t gd;
} files[MAX_FILES];

int open(const char *name, int flags, ...)
{
	char path[160];
	uint16_t gd_mode = GD_FOPEN_READ;
	const char *dir = files_root();

	if ((flags & 3) != 0)
		gd_mode |= GD_FOPEN_WRITE;
	if (flags & LIBC_CREAT)
		gd_mode |= (flags & LIBC_TRUNC) ? GD_FOPEN_CREATE_ALWAYS : GD_FOPEN_OPEN_ALWAYS;
	snprintf(path, sizeof path, "%s%s%s", dir, dir[0] ? "/" : "", name);
	for (int i = 0; i < MAX_FILES; i++) {
		if (!files[i].open) {
			uint16_t gd = GD_FOpen(path, gd_mode);

			if (FAILED(gd))
				break;
			files[i].open = true;
			files[i].gd = gd;
			return FIRST_FILE + i;
		}
	}
	errno = ENOENT;
	return -1;
}

static int file_index(int fd)
{
	int i = fd - FIRST_FILE;

	return i >= 0 && i < MAX_FILES && files[i].open ? i : -1;
}

_READ_WRITE_RETURN_TYPE read(int fd, void *buffer, size_t count)
{
	int i = file_index(fd);
	uint32_t at, size;

	if (i < 0) {
		errno = EBADF;
		return -1;
	}
	at = GD_FTell(files[i].gd);
	size = GD_FSize(files[i].gd);
	if (count > size - at)
		count = at < size ? size - at : 0;
	if (count > 0 && FAILED(GD_FRead(files[i].gd, buffer, count, GD_FREAD_CPU)))
		return -1;
	return (int) count;
}

_READ_WRITE_RETURN_TYPE write(int fd, const void *buffer, size_t count)
{
	int i = file_index(fd);

	if (fd == 1 || fd == 2) {
		char text[129];
		const char *p = buffer;
		size_t left = count;

		while (left > 0) {
			size_t n = left < sizeof text - 1 ? left : sizeof text - 1;

			memcpy(text, p, n);
			text[n] = '\0';
			plat_log(text);
			p += n;
			left -= n;
		}
		return (int) count;
	}
	if (i < 0) {
		errno = EBADF;
		return -1;
	}
	if (count > 0 && FAILED(GD_FWrite(files[i].gd, buffer, count)))
		return -1;
	return (int) count;
}

off_t lseek(int fd, off_t offset, int whence)
{
	int i = file_index(fd);
	int32_t at;

	if (i < 0) {
		errno = EBADF;
		return -1;
	}
	at = whence == SEEK_CUR ? (int32_t) GD_FTell(files[i].gd) + offset
		: whence == SEEK_END ? (int32_t) GD_FSize(files[i].gd) + offset : offset;
	if (at < 0 || FAILED(GD_FSeek(files[i].gd, GD_FSEEK_SET, at)))
		return -1;
	return at;
}

int close(int fd)
{
	int i = file_index(fd);

	if (i < 0)
		return fd >= 0 && fd < FIRST_FILE ? 0 : -1;
	GD_FClose(files[i].gd);
	files[i].open = false;
	return 0;
}

int fstat(int fd, struct stat *st)
{
	memset(st, 0, sizeof *st);
	st->st_mode = fd < FIRST_FILE ? S_IFCHR : S_IFREG;
	return 0;
}

int isatty(int fd)
{
	return fd < FIRST_FILE;
}

int getpid(void)
{
	return 1;
}

int kill(int pid, int signal)
{
	(void) pid;
	(void) signal;
	errno = EINVAL;
	return -1;
}

void _exit(int code)
{
	backend_quit(code);
	for (;;)
		;
}

/* C++ statics register their destructors here; the game never runs them (as on the Atari:
 * destroying them crashes), so nothing is kept. */
void *__dso_handle = &__dso_handle;

int __cxa_atexit(void (*destructor)(void *), void *object, void *dso)
{
	(void) destructor;
	(void) object;
	(void) dso;
	return 0;
}
