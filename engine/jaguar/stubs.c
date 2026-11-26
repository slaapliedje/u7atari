/* The C library's system calls, for the engine test: nothing of them is used but snprintf, which
 * pulls in stdio. No heap: the engine works in a store of its own (main.c). */
#include <errno.h>
#include <stddef.h>
#include <sys/stat.h>

void *sbrk(ptrdiff_t n) { (void) n; errno = ENOMEM; return (void *) -1; }
void _exit(int code) { (void) code; for (;;) ; }
int kill(int pid, int sig) { (void) pid; (void) sig; errno = EINVAL; return -1; }
int getpid(void) { return 1; }
int write(int f, const void *p, size_t n) { (void) f; (void) p; return (int) n; }
int read(int f, void *p, size_t n) { (void) f; (void) p; (void) n; return 0; }
int close(int f) { (void) f; return -1; }
int fstat(int f, struct stat *s) { (void) f; s->st_mode = S_IFCHR; return 0; }
int isatty(int f) { (void) f; return 1; }
long lseek(int f, long o, int w) { (void) f; (void) o; (void) w; return -1; }
