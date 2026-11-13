/* The game's DOS files, found in the data directory whatever their letter case.
 *
 * Follows upstream's matoya backend (platform/matoya/files.c) on MiNTLib's POSIX layer, which
 * works under plain TOS and under MiNT. Names are matched against the directory only on a
 * case-sensitive MiNT file system; TOS FAT drives are 8.3 and case-insensitive already.
 *
 * Under plain TOS, where loading a game is mostly waiting on GEMDOS, the hot paths call GEMDOS
 * themselves: MiNTLib's stat, open and readdir each make several GEMDOS calls for one, and
 * turn every file date into a time_t (over a millisecond on a 68030).
 *
 * The game's data and saves stay little-endian; the bytes pass through unchanged.
 *
 * TT-RAM: the game's buffers are in fast RAM, which the ACSI/SCSI DMA cannot reach. The hard
 * disk driver bounces such transfers through ST RAM (_FRB cookie); HDDRIVER and the other
 * current drivers set this up.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __MINT__
#include <mint/mintbind.h>
#include <mint/osbind.h>
#include <mint/ostruct.h>
#else
#include <sys/statvfs.h>
#endif

#include "plat.h"
#include "backend.h"
#include "game.h"

#define PATH_SIZE 256
#define NAME_SIZE 64
#define MAX_HANDLES 32
/* GEMDOS calls are slow; read and write through bigger blocks than stdio's default. */
#define STREAM_BUFFER 8192

enum { LAST_NONE, LAST_READ, LAST_WRITE };

/* A file is read through two windows of its own, each WINDOW_SIZE bytes from where a read
 * first missed them, the one used least recently refilled: so a file's index and its data,
 * read by turns, both stay at hand. A seek only moves the position; the system's file is moved
 * only to fill a window, or for a read too big for one. On TOS a seek costs a walk along the
 * file's clusters (from its start, when going back), and stdio's own buffer goes at each one.
 * The game opens most files to read and write, as DOS programs did, and writes to few: a
 * handle leaves its windows at its first write. */
#define WINDOW_SIZE 8192
/* A big file may be open up to READERS times, each at its own place in it: a seek forward
 * walks the clusters from where the system's file stands, and one back walks them from the
 * start, so the reader standing nearest before a read is moved to it. The shapes, read
 * mostly in order with now and then one from far on, keep their place this way (SHAPES.VGA
 * is 3600 clusters long). */
#define READERS 4
#define READERS_FROM (256 * INT32_C(1024))     /* a read this far in may open another */

typedef struct {
	int32_t start, length;
	uint32_t used;
} window;

/* Saving a game reads back every file in GAMEDAT, and loading one writes them all: each open
 * or create is a search of that folder of nearly 200 entries, a slow one under GEMDOS on a real
 * disk. So files up to KEPT_LARGEST bytes are kept in memory once read or written whole, up to
 * KEPT_MEMORY in all, the one used least recently going first, and while KEPT_SPARE bytes of the
 * system's memory stay free; GAMEDAT takes about 400 KB. A file is opened from memory when kept;
 * a write to it forgets it, as do removing it and making it again. */
#define KEPT_LARGEST (64 * INT32_C(1024))
#define KEPT_MEMORY (1024 * INT32_C(1024))
#define KEPT_SPARE (512 * INT32_C(1024))
/* A file created is built in memory, up to BUILT_LARGEST bytes, and written whole when it is
 * closed: the game writes a saved game a piece at a time, going back to its index between
 * them, and each seek back has GEMDOS walk the file's clusters from its start. */
#define BUILT_LARGEST (1024 * INT32_C(1024))

typedef struct kept_file {
	struct kept_file *next;
	uint32_t used;
	uint32_t hash;
	int32_t size;
	char *path;
	uint8_t data[];             /* the file, then its path */
} kept_file;

static kept_file *kept;
static int32_t kept_memory;
static uint32_t kept_clock;

/* A handle is an open file, read and written through stdio; or one not yet written, read
 * through windows (window_data set); or under TOS one written, through GEMDOS itself with a
 * buffer of its own (write_data set: MiNTLib's fopen makes a dozen GEMDOS calls, and
 * looks up the file's date, where Fcreate is one); or a block of memory: a kept file's copy,
 * which goes to the file itself at the first write, or the made-up U7.CFG, only read. A file
 * created is also built in memory (built set) until it is closed or grows too big. */
typedef struct {
	FILE *file;
	int last;
	char *memory;
	int32_t memory_size;
	int32_t memory_pos;
	uint8_t *built;
	int32_t built_size, built_room, built_pos;
	uint8_t *window_data;       /* both windows' bytes */
	window windows[2];
	uint32_t window_clock;
	long raw;                   /* the system's handle (written files) */
	int32_t raw_pos;            /* where it stands; -1 when not known */
	long readers[READERS];      /* the system's handles (windows) */
	int32_t reader_pos[READERS];    /* where each stands; -1 when not known */
	uint32_t reader_used[READERS];
	int reader_count;
	int32_t pos;                /* where the next read starts */
	int32_t size;               /* -1 until needed */
	bool writable;              /* opened to write too */
	uint8_t *write_data;        /* bytes written, write_count of them, not yet given to GEMDOS */
	int32_t write_count;
	char path[PATH_SIZE];       /* to open it again for stdio */
} handle;

typedef struct {
	char name[13];
	uint32_t size;
} found_file;

typedef struct {
	found_file *files;
	uint32_t count;
	uint32_t next;
} find_list;

static char root[PATH_SIZE] = ".";
static handle handles[MAX_HANDLES + 1];

const char *files_root(void)
{
	return root;
}

void files_set_root(const char *dir)
{
	size_t n;

	snprintf(root, sizeof root, "%s", dir);
	n = strlen(root);
	/* Keep "C:\" whole; strip any other trailing separator. */
	while (n > 1 && (root[n - 1] == '/' || root[n - 1] == '\\') && !(n == 3 && root[1] == ':'))
		root[--n] = '\0';
}

static bool is_separator(char c)
{
	return c == '/' || c == '\\';
}

enum { KIND_NONE, KIND_FILE, KIND_DIR, KIND_OTHER };

#ifdef __MINT__
extern int __mint;          /* MiNTLib: the MiNT version, 0 under plain TOS */

/* path for GEMDOS itself, with TOS's separators */
static bool dos_path(const char *path, char dos[PATH_SIZE])
{
	size_t n = strlen(path);

	if (n == 0 || n >= PATH_SIZE)
		return false;
	for (size_t i = 0; i <= n; i++)
		dos[i] = path[i] == '/' ? '\\' : path[i];
	return true;
}

/* path's kind from GEMDOS Fsfirst, under plain TOS; -1 when Fsfirst can't say (a drive's root,
 * "." and "..", which have no entry of their own to find, or a wildcard). MiNTLib's stat does
 * the same search and then turns the entry's DOS date into a time_t through mktime, which takes
 * over a millisecond on a 68030: loading a game asks after a file several hundred times. */
static int tos_path_kind(const char *path, uint32_t *size)
{
	char dos[PATH_SIZE];
	const char *name = path;
	_DTA dta, *old;
	long found;

	if (!dos_path(path, dos) || strpbrk(path, "*?") != NULL)
		return -1;
	for (const char *p = path; *p != '\0'; p++) {
		if (is_separator(*p) || *p == ':')
			name = p + 1;
	}
	if (*name == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
		return -1;
	old = Fgetdta();
	Fsetdta(&dta);
	found = Fsfirst(dos, FA_RDONLY | FA_HIDDEN | FA_SYSTEM | FA_DIR);
	Fsetdta(old);
	if (found != 0)
		return KIND_NONE;
	if (size != NULL)
		*size = (uint32_t) dta.dta_size;
	return (dta.dta_attribute & FA_DIR) ? KIND_DIR : KIND_FILE;
}
#endif

/* What path is, and a file's size in *size when size isn't NULL. */
static int path_kind(const char *path, uint32_t *size)
{
	struct stat st;

#ifdef __MINT__
	if (__mint == 0) {
		int kind = tos_path_kind(path, size);

		if (kind >= 0)
			return kind;
	}
#endif
	if (stat(path, &st) != 0)
		return KIND_NONE;
	if (size != NULL)
		*size = (uint32_t) st.st_size;
	return S_ISREG(st.st_mode) ? KIND_FILE : S_ISDIR(st.st_mode) ? KIND_DIR : KIND_OTHER;
}

static bool exists(const char *path)
{
	return path_kind(path, NULL) != KIND_NONE;
}

static bool is_file(const char *path)
{
	return path_kind(path, NULL) == KIND_FILE;
}

static bool is_dir(const char *path)
{
	return path_kind(path, NULL) == KIND_DIR;
}

static bool join(char *path, size_t size, const char *dir, const char *name)
{
	size_t n = strlen(dir);
	int written = snprintf(path, size, "%s%s%s", dir,
		n > 0 && is_separator(dir[n - 1]) ? "" : "/", name);

	return written > 0 && (size_t) written < size;
}

/* Whether names in dir must match in case. TOS file systems never do; under MiNT, ext2 and the
 * like do (Dpathconf says). The last answer is kept, as most names are in one or two folders. */
static bool case_sensitive(const char *dir)
{
#ifdef __MINT__
	static char last_dir[PATH_SIZE];
	static bool last_answer;

	if (__mint == 0)
		return false;
	if (strcmp(dir, last_dir) != 0) {
		snprintf(last_dir, sizeof last_dir, "%s", dir);
		last_answer = Dpathconf(dir, 6 /* DP_CASE */) == 0 /* DP_CASESENS */;
	}
	return last_answer;
#else
	(void) dir;
	return true;
#endif
}

/* Finds the name on disk of one entry of dir, ignoring case, on a file system where case
 * matters. */
static bool real_name(const char *dir, const char *name, char *real)
{
	char path[PATH_SIZE];
	DIR *d;
	struct dirent *entry;
	bool found = false;

	if (join(path, sizeof path, dir, name) && exists(path)) {
		snprintf(real, NAME_SIZE, "%s", name);
		return true;
	}
	if ((d = opendir(dir)) == NULL)
		return false;
	while (!found && (entry = readdir(d)) != NULL) {
		if (strcasecmp(entry->d_name, name) == 0) {
			/* Same length as name, which fits. */
			memcpy(real, entry->d_name, strlen(name) + 1);
			found = true;
		}
	}
	closedir(d);
	return found;
}

/* Turns a DOS name into a host path. With create set, a missing last part is allowed and
 * spelt in upper case. Where case doesn't matter (every TOS drive) the name is the path as it
 * stands, whether or not it exists: what uses the path finds that out, and each look on the
 * way costs a search of the folder, a slow one on a real disk. */
static bool resolve(const char *dos, bool create, char *path)
{
	const char *p = dos;
	char part[NAME_SIZE];
	char real[NAME_SIZE];

	snprintf(path, PATH_SIZE, "%s", root);
	if (isalpha((unsigned char) p[0]) && p[1] == ':')
		p += 2;
	for (;;) {
		size_t n;
		bool last;

		while (is_separator(*p))
			p++;
		if (*p == '\0')
			return true;
		for (n = 0; p[n] != '\0' && !is_separator(p[n]); n++)
			;
		if (n >= sizeof part)
			return false;
		memcpy(part, p, n);
		part[n] = '\0';
		p += n;
		last = true;
		for (const char *q = p; *q != '\0'; q++) {
			if (!is_separator(*q))
				last = false;
		}

		if (strcmp(part, ".") == 0)
			continue;
		if (strcmp(part, "..") != 0) {
			/* "NAME." is NAME with no extension. */
			while (n > 0 && part[n - 1] == '.')
				part[--n] = '\0';
			if (n == 0)
				continue;
		}
		if (!case_sensitive(path)) {
			for (size_t i = 0; i <= n; i++)
				real[i] = create && last ? (char) toupper((unsigned char) part[i]) : part[i];
		} else if (!real_name(path, part, real)) {
			if (!create || !last)
				return false;
			for (size_t i = 0; i <= n; i++)
				real[i] = (char) toupper((unsigned char) part[i]);
		}
		size_t length = strlen(path);
		bool slash = length > 0 && !is_separator(path[length - 1]);

		if (length + slash + strlen(real) >= PATH_SIZE)
			return false;
		if (slash)
			strcat(path, "/");
		strcat(path, real);
	}
}

/* ---- Handles ---- */

static void raw_close(long raw);
static bool write_out(handle *h);

static bool in_use(const handle *h)
{
	return h->file != NULL || h->memory != NULL || h->window_data != NULL || h->write_data != NULL;
}

static void keep(const char *path, const void *data, int32_t size);
static void forget(const char *path);
static int32_t write_through(handle *h, const void *buffer, int32_t count);

static void release(handle *h)
{
	uint8_t *built = h->built;
	int32_t built_size = h->built_size;
	bool written = true;

	if (built != NULL) {
		h->built = NULL;
		written = write_through(h, built, built_size) == built_size;
	}
	if (h->file != NULL && fclose(h->file) != 0)
		written = false;
	if (h->window_data != NULL) {
		for (int i = 0; i < h->reader_count; i++)
			raw_close(h->readers[i]);
		free(h->window_data);
	}
	if (h->write_data != NULL) {
		written = write_out(h) && written;
		raw_close(h->raw);
		free(h->write_data);
	}
	/* a file written: kept as it now is, else not at all (it may have been read and kept
	 * while it was being written) */
	if (built != NULL && written)
		keep(h->path, built, built_size);
	else if (h->file != NULL || h->write_data != NULL)
		forget(h->path);
	free(built);
	free(h->memory);
	memset(h, 0, sizeof *h);
}

/* Closes whatever the last program left open. */
void files_reset(void)
{
	for (int i = 1; i <= MAX_HANDLES; i++)
		release(&handles[i]);
}

static int16_t free_handle(void)
{
	for (int16_t i = 1; i <= MAX_HANDLES; i++) {
		if (!in_use(&handles[i]))
			return i;
	}
	return -1;
}

static int16_t add_file(FILE *file)
{
	int16_t i;

	if (file == NULL)
		return -1;
	if ((i = free_handle()) < 0) {
		fclose(file);
		return -1;
	}
	setvbuf(file, NULL, _IOFBF, STREAM_BUFFER);
	handles[i].file = file;
	handles[i].last = LAST_NONE;
	return i;
}

static int16_t add_memory(char *memory, int32_t size)
{
	int16_t i = free_handle();

	if (i < 0) {
		free(memory);
		return -1;
	}
	handles[i].memory = memory;
	handles[i].memory_size = size;
	handles[i].memory_pos = 0;
	return i;
}

static handle *get_handle(int16_t file)
{
	if (file < 1 || file > MAX_HANDLES)
		return NULL;
	if (!in_use(&handles[file]))
		return NULL;
	return &handles[file];
}

/* ---- Files kept in memory ---- */

/* Paths compared without regard to case, as DOS does; MiNTLib's toupper is slow. */
static char ascii_upper(char c)
{
	return c >= 'a' && c <= 'z' ? (char) (c - 'a' + 'A') : c;
}

static uint32_t path_hash(const char *path)
{
	uint32_t hash = UINT32_C(2166136261);

	for (; *path != '\0'; path++)
		hash = (hash ^ (uint8_t) ascii_upper(*path)) * UINT32_C(16777619);
	return hash;
}

static bool same_path(const char *a, const char *b)
{
	while (*a != '\0' && ascii_upper(*a) == ascii_upper(*b))
		a++, b++;
	return ascii_upper(*a) == ascii_upper(*b);
}

static kept_file **kept_link(const char *path)
{
	uint32_t hash = path_hash(path);
	kept_file **k = &kept;

	while (*k != NULL && ((*k)->hash != hash || !same_path((*k)->path, path)))
		k = &(*k)->next;
	return k;
}

static void forget(const char *path)
{
	kept_file **k = kept_link(path), *f = *k;

	if (f != NULL) {
		*k = f->next;
		kept_memory -= f->size;
		free(f);
	}
}

static void forget_all(void)
{
	while (kept != NULL)
		forget(kept->path);
}

/* Whether size bytes more may be kept, once those used least recently have gone. */
static bool room_to_keep(int32_t size)
{
	if (size < 0 || size > KEPT_LARGEST)
		return false;
	while (kept != NULL && kept_memory + size > KEPT_MEMORY) {
		kept_file *oldest = kept;

		for (kept_file *f = kept->next; f != NULL; f = f->next) {
			if (f->used < oldest->used)
				oldest = f;
		}
		forget(oldest->path);
	}
#ifdef __MINT__
	if (Malloc(-1L) < KEPT_SPARE + size)
		return false;
#endif
	return true;
}

static void keep(const char *path, const void *data, int32_t size)
{
	kept_file *f;

	forget(path);
	if (!room_to_keep(size) || (f = malloc(sizeof *f + (size_t) size + strlen(path) + 1)) == NULL)
		return;
	memcpy(f->data, data, (size_t) size);
	f->path = (char *) f->data + size;
	strcpy(f->path, path);
	f->size = size;
	f->hash = path_hash(path);
	f->used = ++kept_clock;
	f->next = kept;
	kept = f;
	kept_memory += size;
}

/* Opens a kept file's copy: the handle; -1 when there is no room for it; -2 when the file
 * isn't kept. */
static int16_t open_kept(const char *path, bool writable)
{
	kept_file *f = *kept_link(path);
	char *copy;
	int16_t i;

	if (f == NULL || (copy = malloc(f->size > 0 ? (size_t) f->size : 1)) == NULL)
		return -2;
	memcpy(copy, f->data, (size_t) f->size);
	f->used = ++kept_clock;
	if ((i = add_memory(copy, f->size)) >= 0) {
		handles[i].writable = writable;
		snprintf(handles[i].path, sizeof handles[i].path, "%s", path);
	}
	return i;
}

/* ---- Files created: built in memory ---- */

static int32_t build_write(handle *h, const void *data, int32_t count)
{
	int32_t end = h->built_pos + count;

	if (end > BUILT_LARGEST)
		return -1;
	if (end > h->built_room) {
		int32_t room = end > 2 * h->built_room ? end : 2 * h->built_room;
		uint8_t *more = realloc(h->built, (size_t) room);

		if (more == NULL)
			return -1;
		h->built = more;
		h->built_room = room;
	}
	/* a seek past the end leaves a gap, filled with zeros as DOS does */
	if (h->built_pos > h->built_size)
		memset(h->built + h->built_size, 0, (size_t) (h->built_pos - h->built_size));
	memcpy(h->built + h->built_pos, data, (size_t) count);
	h->built_pos = end;
	if (end > h->built_size)
		h->built_size = end;
	return count;
}

static int32_t seek_through(handle *h, int32_t at);

/* Gives a file built so far to the file itself, to go on there. False if it can't be written. */
static bool unbuild(handle *h)
{
	uint8_t *data = h->built;
	bool ok;

	h->built = NULL;
	ok = write_through(h, data, h->built_size) == h->built_size
		&& seek_through(h, h->built_pos) == h->built_pos;
	free(data);
	return ok;
}

/* The handle's stream, ready for a read or a write. C needs a seek between the two. */
static FILE *use_stream(handle *h, int op)
{
	if (op != LAST_NONE && h->last != LAST_NONE && h->last != op)
		fseek(h->file, 0, SEEK_CUR);
	h->last = op;
	return h->file;
}

/* ---- Files only read: through windows ---- */

/* The system's own calls: GEMDOS's under TOS, where MiNTLib's open and fstat would look up the
 * file's date (a mktime each); POSIX's elsewhere. */
static long raw_open(const char *path)
{
#ifdef __MINT__
	if (__mint == 0) {
		char dos[PATH_SIZE];

		return dos_path(path, dos) ? Fopen(dos, 0) : -1;
	}
#endif
	return open(path, O_RDONLY);
}

static int32_t raw_read(long raw, void *buffer, int32_t count)
{
#ifdef __MINT__
	if (__mint == 0)
		return (int32_t) Fread((int16_t) raw, count, buffer);
#endif
	return (int32_t) read((int) raw, buffer, (size_t) count);
}

static int32_t raw_seek(long raw, int32_t offset, int whence)
{
#ifdef __MINT__
	if (__mint == 0)
		return (int32_t) Fseek(offset, (int16_t) raw, (int16_t) whence);
#endif
	return (int32_t) lseek((int) raw, offset, whence);
}

static int32_t raw_write(long raw, const void *buffer, int32_t count)
{
#ifdef __MINT__
	if (__mint == 0)
		return (int32_t) Fwrite((int16_t) raw, count, buffer);
#endif
	return (int32_t) write((int) raw, buffer, (size_t) count);
}

static void raw_close(long raw)
{
#ifdef __MINT__
	if (__mint == 0) {
		Fclose((int16_t) raw);
		return;
	}
#endif
	close((int) raw);
}

/* Opens path to read through windows: the handle; -1 when there is no such file; -2 when
 * there is no room for the windows, to open it as any other. */
static int16_t open_windowed(const char *path, bool writable)
{
	int16_t i = free_handle();
	handle *h;
	long raw;

	if (i < 0)
		return -2;
	if ((raw = raw_open(path)) < 0)
		return -1;
	h = &handles[i];
	memset(h, 0, sizeof *h);
	if ((h->window_data = malloc(2 * WINDOW_SIZE)) == NULL) {
		raw_close(raw);
		return -2;
	}
	h->readers[0] = raw;
	h->reader_count = 1;
	h->size = -1;
	h->writable = writable;
	snprintf(h->path, sizeof h->path, "%s", path);
	return i;
}

static int32_t tos_seek(handle *h, int32_t offset, int whence);

/* Turns a handle read through windows into one to write, where it stands: through GEMDOS
 * under TOS, else stdio. False if the file won't open again. */
static bool leave_windows(handle *h)
{
	FILE *file;

	forget(h->path);
	for (int i = 0; i < h->reader_count; i++)
		raw_close(h->readers[i]);
	h->reader_count = 0;
#ifdef __MINT__
	if (__mint == 0) {
		char dos[PATH_SIZE];

		/* the windows' memory becomes the write buffer */
		h->write_data = h->window_data;
		h->window_data = NULL;
		h->write_count = 0;
		h->raw = -1;
		if (dos_path(h->path, dos) && (h->raw = Fopen(dos, 2)) >= 0) {
			h->raw_pos = 0;
			if (tos_seek(h, h->pos, SEEK_SET) == h->pos)
				return true;
			Fclose((int16_t) h->raw);
		}
		/* stdio, then */
		h->window_data = h->write_data;
		h->write_data = NULL;
	}
#endif
	free(h->window_data);
	h->window_data = NULL;
	if ((file = fopen(h->path, "r+b")) == NULL || fseek(file, h->pos, SEEK_SET) != 0) {
		if (file != NULL)
			fclose(file);
		memset(h, 0, sizeof *h);
		return false;
	}
	setvbuf(file, NULL, _IOFBF, STREAM_BUFFER);
	h->file = file;
	h->last = LAST_NONE;
	return true;
}

/* A reader moved to at: the one standing nearest before it, else a new one when at is far
 * enough in, else the one used least recently. Its index, or -1. */
static int reader_at(handle *h, int32_t at)
{
	int best = -1;

	for (int i = 0; i < h->reader_count; i++) {
		if (h->reader_pos[i] >= 0 && h->reader_pos[i] <= at
			&& (best < 0 || h->reader_pos[i] > h->reader_pos[best]))
			best = i;
	}
	if (best < 0 && at >= READERS_FROM && h->reader_count < READERS) {
		long raw = raw_open(h->path);

		if (raw >= 0) {
			best = h->reader_count++;
			h->readers[best] = raw;
			h->reader_pos[best] = 0;
		}
	}
	if (best < 0) {
		best = 0;
		for (int i = 1; i < h->reader_count; i++) {
			if (h->reader_used[i] < h->reader_used[best])
				best = i;
		}
	}
	h->reader_used[best] = ++h->window_clock;
	if (h->reader_pos[best] != at && raw_seek(h->readers[best], at, SEEK_SET) != at) {
		h->reader_pos[best] = -1;
		return -1;
	}
	h->reader_pos[best] = at;
	return best;
}

/* Reads count bytes from at into buffer: how many, or -1. */
static int32_t reader_read(handle *h, int32_t at, void *buffer, int32_t count)
{
	int r = reader_at(h, at);
	int32_t n;

	if (r < 0)
		return -1;
	if ((n = raw_read(h->readers[r], buffer, count)) < 0) {
		h->reader_pos[r] = -1;
		return -1;
	}
	h->reader_pos[r] += n;
	return n;
}

static uint8_t *window_bytes(handle *h, const window *w)
{
	return h->window_data + (w - h->windows) * WINDOW_SIZE;
}

static int32_t window_read(handle *h, uint8_t *out, int32_t count)
{
	int32_t done = 0;

	while (done < count) {
		window *w = NULL;
		int32_t n;

		for (int k = 0; k < 2; k++) {
			window *c = &h->windows[k];

			if (h->pos >= c->start && h->pos < c->start + c->length)
				w = c;
		}
		if (w == NULL && count - done >= WINDOW_SIZE) {
			/* too big for a window: straight from the file */
			if ((n = reader_read(h, h->pos, out + done, count - done)) < 0)
				return done > 0 ? done : -1;
			h->pos += n;
			return done + n;
		}
		if (w == NULL) {
			/* refill the window used least recently, from the sector the read starts in */
			int32_t start = h->pos & ~(int32_t) 511;

			w = h->windows[0].used <= h->windows[1].used ? &h->windows[0] : &h->windows[1];
			w->length = 0;
			if ((n = reader_read(h, start, window_bytes(h, w), WINDOW_SIZE)) < 0)
				return done > 0 ? done : -1;
			w->start = start;
			w->length = n;
			if (h->pos >= start + n)
				break;      /* past the end of the file */
		}
		w->used = ++h->window_clock;
		n = w->start + w->length - h->pos;
		if (n > count - done)
			n = count - done;
		memcpy(out + done, window_bytes(h, w) + (h->pos - w->start), (size_t) n);
		h->pos += n;
		done += n;
	}
	return done;
}

/* The file's size, found the first time it is asked for. */
static int32_t window_file_size(handle *h)
{
	if (h->size < 0) {
		h->size = raw_seek(h->readers[0], 0, SEEK_END);
		h->reader_pos[0] = h->size;
	}
	return h->size;
}

/* A file just opened, small enough to keep: read whole and kept, and the handle turned into a
 * copy in memory. */
static void keep_whole(handle *h)
{
	int32_t size = window_file_size(h);
	char *data;

	if (!room_to_keep(size) || (data = malloc(size > 0 ? (size_t) size : 1)) == NULL)
		return;
	if (reader_read(h, 0, data, size) != size) {
		free(data);
		return;
	}
	keep(h->path, data, size);
	for (int i = 0; i < h->reader_count; i++)
		raw_close(h->readers[i]);
	h->reader_count = 0;
	free(h->window_data);
	h->window_data = NULL;
	h->memory = data;
	h->memory_size = size;
	h->memory_pos = 0;
}

/* Turns a kept file's copy into the file itself, where it stands, to write. False if the file
 * won't open. */
static bool leave_memory(handle *h)
{
	int32_t pos = h->memory_pos;

	free(h->memory);
	h->memory = NULL;
	if ((h->window_data = malloc(2 * WINDOW_SIZE)) == NULL) {
		forget(h->path);
		memset(h, 0, sizeof *h);
		return false;
	}
	h->pos = pos;
	return leave_windows(h);
}

/* ---- Files written, under TOS ---- */

#ifdef __MINT__
/* Creates path under TOS: the handle; -1 when it can't be made; -2 when there is no room for a
 * buffer, to make it with stdio. */
static int16_t create_tos(const char *path)
{
	char dos[PATH_SIZE];
	int16_t i = free_handle();
	handle *h;
	long raw;

	if (i < 0)
		return -2;
	if (!dos_path(path, dos) || (raw = Fcreate(dos, 0)) < 0)
		return -1;
	h = &handles[i];
	memset(h, 0, sizeof *h);
	if ((h->write_data = malloc(STREAM_BUFFER)) == NULL) {
		Fclose((int16_t) raw);
		return -2;
	}
	h->raw = raw;
	snprintf(h->path, sizeof h->path, "%s", path);
	return i;
}
#endif

/* Gives what is waiting to GEMDOS. */
static bool write_out(handle *h)
{
#ifdef __MINT__
	if (h->write_count > 0) {
		long put = Fwrite((int16_t) h->raw, h->write_count, h->write_data);

		if (put > 0)
			h->raw_pos += (int32_t) put;
		if (put != h->write_count) {
			h->write_count = 0;
			return false;
		}
		h->write_count = 0;
	}
#else
	(void) h;
#endif
	return true;
}

static int32_t tos_write(handle *h, const void *buffer, int32_t count)
{
#ifdef __MINT__
	if (count >= STREAM_BUFFER) {
		long put;

		if (!write_out(h))
			return -1;
		put = Fwrite((int16_t) h->raw, count, buffer);
		if (put < 0)
			return -1;
		h->raw_pos += (int32_t) put;
		return (int32_t) put;
	}
	if (h->write_count + count > STREAM_BUFFER && !write_out(h))
		return -1;
	memcpy(h->write_data + h->write_count, buffer, (size_t) count);
	h->write_count += count;
	return count;
#else
	(void) h, (void) buffer, (void) count;
	return -1;
#endif
}

static int32_t tos_read(handle *h, void *buffer, int32_t count)
{
	int32_t got;

	if (!write_out(h) || (got = raw_read(h->raw, buffer, count)) < 0)
		return -1;
	h->raw_pos += got;
	return got;
}

static int32_t tos_seek(handle *h, int32_t offset, int whence)
{
	int32_t at;

	if (!write_out(h))
		return -1;
	if (whence == SEEK_CUR) {
		offset += h->raw_pos;
		whence = SEEK_SET;
	}
	if (whence == SEEK_SET && offset == h->raw_pos)
		return offset;
	if (whence == SEEK_SET && offset < 0)
		return -1;
	if ((at = raw_seek(h->raw, offset, whence)) >= 0) {
		h->raw_pos = at;
		return at;
	}
	/* GEMDOS won't go past the end, where stdio (and DOS) would: fill up to there with zeros */
	if (whence == SEEK_SET && (at = raw_seek(h->raw, 0, SEEK_END)) >= 0 && at < offset) {
		static const uint8_t zeros[512];

		h->raw_pos = at;
		while (h->raw_pos < offset) {
			int32_t n = offset - h->raw_pos < (int32_t) sizeof zeros ? offset - h->raw_pos
				: (int32_t) sizeof zeros;

			if (raw_write(h->raw, zeros, n) != n)
				return -1;
			h->raw_pos += n;
		}
		return offset;
	}
	return -1;
}

static int32_t tos_length(handle *h)
{
	int32_t end;

	if (!write_out(h) || (end = raw_seek(h->raw, 0, SEEK_END)) < 0)
		return -1;
	raw_seek(h->raw, h->raw_pos, SEEK_SET);
	return end;
}

/* U7.CFG (SERPENT.CFG) is the sound setup the DOS installer wrote: line 1 the music card, line 2
 * the speech card's port, IRQ and DMA (and in Serpent Isle an 'o' for the Sound Blaster Pro). Music here is always the MT-32 on the MIDI port, and speech always
 * has an output, so the game reads a setup to match: "r 330", and a speech line when the file
 * has none. The rest is the file's own; the file is never changed.
 *
 * Upstream builds this in a tmpfile; TOS may have no writable temporary folder, so it lives
 * in memory instead. */
static bool is_sound_config(const char *name)
{
	return strcasecmp(name, GAME_SOUND_CONFIG) == 0
		|| (strncmp(name, ".\\", 2) == 0 && strcasecmp(name + 2, GAME_SOUND_CONFIG) == 0);
}

static int16_t open_sound_config(const char *name)
{
	char path[PATH_SIZE], music[81] = "p", speech[81] = "", copy[81];
	int tokens = 0;
	char *text;
	FILE *f;

	if (resolve(name, false, path) && is_file(path) && (f = fopen(path, "rb")) != NULL) {
		if (fgets(music, sizeof music, f) == NULL || fgets(speech, sizeof speech, f) == NULL)
			speech[0] = '\0';
		fclose(f);
		music[strcspn(music, "\r\n")] = '\0';
		speech[strcspn(speech, "\r\n")] = '\0';
	}
	/* the game turns speech on only with all three of port, IRQ and DMA */
	snprintf(copy, sizeof copy, "%s", speech);
	for (char *t = strtok(copy, " \t"); t != NULL; t = strtok(NULL, " \t"))
		tokens++;
	snprintf(music, sizeof music, plat_midi_available() ? "r 330" : "a 388");
	if (tokens < 3)
		snprintf(speech, sizeof speech, GAME_DEFAULT_SPEECH);

	if ((text = malloc(sizeof music + sizeof speech + 4)) == NULL)
		return -1;
	return add_memory(text, (int32_t) sprintf(text, "%s\r\n%s\r\n", music, speech));
}

int16_t plat_file_open(const char *name, int16_t mode)
{
	char path[PATH_SIZE];
	int16_t file;

	if (is_sound_config(name))
		return open_sound_config(name);
	if (!resolve(name, false, path))
		return -1;
	if ((file = open_kept(path, (mode & PLAT_FILE_WRITE) != 0)) != -2)
		return file;
#ifdef __MINT__
	/* GEMDOS's Fopen finds only files, so no look first */
	if (__mint != 0 && !is_file(path))
		return -1;
#else
	if (!is_file(path))
		return -1;
#endif
	if ((file = open_windowed(path, (mode & PLAT_FILE_WRITE) != 0)) != -2) {
		if (file >= 0)
			keep_whole(&handles[file]);
		return file;
	}
	if (!is_file(path))
		return -1;
	return add_file(fopen(path, (mode & PLAT_FILE_WRITE) ? "r+b" : "rb"));
}

int16_t plat_file_create(const char *name)
{
	char path[PATH_SIZE];
	int16_t file = -2;

	/* fopen fails on a folder: no need to look first */
	if (!resolve(name, true, path))
		return -1;
	forget(path);
#ifdef __MINT__
	if (__mint == 0)
		file = create_tos(path);
#endif
	if (file == -2)
		file = add_file(fopen(path, "w+b"));
	if (file >= 0) {
		handle *h = &handles[file];

		snprintf(h->path, sizeof h->path, "%s", path);
		h->built_room = 4096;
		h->built = malloc((size_t) h->built_room);
	}
	return file;
}

void plat_file_close(int16_t file)
{
	handle *h = get_handle(file);

	if (h != NULL)
		release(h);
}

/* A write or a seek on the file itself, of a handle written to. */
static int32_t write_through(handle *h, const void *buffer, int32_t count)
{
	size_t n;

	if (h->write_data != NULL)
		return tos_write(h, buffer, count);
	n = fwrite(buffer, 1, (size_t) count, use_stream(h, LAST_WRITE));
	return n == 0 && count > 0 ? -1 : (int32_t) n;
}

static int32_t seek_through(handle *h, int32_t at)
{
	FILE *f;

	if (h->write_data != NULL)
		return tos_seek(h, at, SEEK_SET);
	f = use_stream(h, LAST_NONE);
	if (fseek(f, at, SEEK_SET) != 0)
		return -1;
	return (int32_t) ftell(f);
}

/* Reads from memory: a kept file's copy, or a file being built. */
static int32_t memory_read(const uint8_t *memory, int32_t size, int32_t *pos, void *buffer,
	int32_t count)
{
	if (*pos >= size)
		return 0;
	if (count > size - *pos)
		count = size - *pos;
	memcpy(buffer, memory + *pos, (size_t) count);
	*pos += count;
	return count;
}

static int32_t memory_seek(int32_t size, int32_t *pos, int32_t offset, int whence)
{
	int32_t base = whence == SEEK_CUR ? *pos : whence == SEEK_END ? size : 0;

	if (base + offset < 0)
		return -1;
	*pos = base + offset;
	return *pos;
}

int32_t plat_file_read(int16_t file, void *buffer, int32_t count)
{
	handle *h = get_handle(file);
	FILE *f;
	size_t n;

	if (h == NULL || count < 0)
		return -1;
	if (h->memory != NULL)
		return memory_read((uint8_t *) h->memory, h->memory_size, &h->memory_pos, buffer, count);
	if (h->built != NULL)
		return memory_read(h->built, h->built_size, &h->built_pos, buffer, count);
	if (h->window_data != NULL)
		return window_read(h, buffer, count);
	if (h->write_data != NULL)
		return tos_read(h, buffer, count);
	f = use_stream(h, LAST_READ);
	n = fread(buffer, 1, (size_t) count, f);
	return n == 0 && ferror(f) ? -1 : (int32_t) n;
}

int32_t plat_file_write(int16_t file, const void *buffer, int32_t count)
{
	handle *h = get_handle(file);

	if (h == NULL || count < 0)
		return -1;
	if (h->memory != NULL && (!h->writable || !leave_memory(h)))
		return -1;
	if (h->built != NULL) {
		if (build_write(h, buffer, count) == count)
			return count;
		if (!unbuild(h))
			return -1;
	}
	if (h->window_data != NULL && (!h->writable || !leave_windows(h)))
		return -1;
	return write_through(h, buffer, count);
}

int32_t plat_file_seek(int16_t file, int32_t offset, int16_t origin)
{
	handle *h = get_handle(file);
	int whence = origin == PLAT_SEEK_CUR ? SEEK_CUR : origin == PLAT_SEEK_END ? SEEK_END : SEEK_SET;
	FILE *f;

	if (h == NULL)
		return -1;
	if (h->memory != NULL)
		return memory_seek(h->memory_size, &h->memory_pos, offset, whence);
	if (h->built != NULL)
		return memory_seek(h->built_size, &h->built_pos, offset, whence);
	if (h->window_data != NULL) {
		int32_t base = whence == SEEK_CUR ? h->pos : whence == SEEK_END ? window_file_size(h) : 0;

		if (base < 0 || base + offset < 0)
			return -1;
		h->pos = base + offset;
		return h->pos;
	}
	if (h->write_data != NULL)
		return tos_seek(h, offset, whence);
	f = use_stream(h, LAST_NONE);
	if (fseek(f, offset, whence) != 0)
		return -1;
	return (int32_t) ftell(f);
}

int32_t plat_file_length(int16_t file)
{
	handle *h = get_handle(file);
	FILE *f;
	long position, length;

	if (h == NULL)
		return -1;
	if (h->memory != NULL)
		return h->memory_size;
	if (h->built != NULL)
		return h->built_size;
	if (h->window_data != NULL)
		return window_file_size(h);
	if (h->write_data != NULL)
		return tos_length(h);
	f = use_stream(h, LAST_NONE);
	position = ftell(f);
	fseek(f, 0, SEEK_END);
	length = ftell(f);
	fseek(f, position, SEEK_SET);
	return (int32_t) length;
}

/* GEMDOS has no truncate; MiNTLib's ftruncate needs MiNT (Fcntl FTRUNCATE) and fails under
 * plain TOS. The game does not call this at present (TruncateAndClose has no callers). */
int16_t plat_file_truncate(int16_t file)
{
	handle *h = get_handle(file);
	FILE *f;
	long position;

	if (h == NULL)
		return 0;
	if (h->memory != NULL && (!h->writable || !leave_memory(h)))
		return 0;
	if (h->built != NULL && !unbuild(h))
		return 0;
	if (h->window_data != NULL && (!h->writable || !leave_windows(h)))
		return 0;
	if (h->write_data != NULL)
		return 0;   /* GEMDOS can't */
	f = use_stream(h, LAST_NONE);
	if (fflush(f) != 0 || (position = ftell(f)) < 0)
		return 0;
	return ftruncate(fileno(f), position) == 0;
}

/* ---- Names ---- */

int16_t plat_file_exists(const char *name)
{
	char path[PATH_SIZE];

	return resolve(name, false, path) && is_file(path);
}

int16_t plat_dir_exists(const char *name)
{
	char path[PATH_SIZE];

	return resolve(name, false, path) && is_dir(path);
}

int16_t plat_dir_create(const char *name)
{
	char path[PATH_SIZE];

	if (!resolve(name, true, path) || exists(path))
		return 0;
	return mkdir(path, 0777) == 0;
}

int16_t plat_dir_remove(const char *name)
{
	char path[PATH_SIZE];

	forget_all();
	return resolve(name, false, path) && is_dir(path) && rmdir(path) == 0;
}

int16_t plat_file_remove(const char *name)
{
	char path[PATH_SIZE];

	if (!resolve(name, false, path))
		return 0;
	forget(path);
	return is_file(path) && remove(path) == 0;
}

int16_t plat_file_rename(const char *from, const char *to)
{
	char from_path[PATH_SIZE];
	char to_path[PATH_SIZE];
	kept_file *f;

	/* DOS refused to rename over an existing file. */
	if (!resolve(from, false, from_path) || !resolve(to, true, to_path) || exists(to_path))
		return 0;
	if (rename(from_path, to_path) != 0)
		return 0;
	/* kept under the new name (keep may let the old go first) */
	if ((f = *kept_link(from_path)) != NULL) {
		int32_t size = f->size;
		void *data = malloc(size > 0 ? (size_t) size : 1);

		if (data != NULL)
			memcpy(data, f->data, (size_t) size);
		forget(from_path);
		if (data != NULL) {
			keep(to_path, data, size);
			free(data);
		}
	}
	return 1;
}

/* Free bytes on the data directory's drive, from GEMDOS Dfree; a fixed 32 MB in lockstep, the
 * same on every host. */
uint32_t plat_disk_free(void)
{
#ifdef U7_DEBUG_LOG
	if (lockstep)
		return UINT32_C(32) << 20;
#endif
#ifdef __MINT__
	_DISKINFO info;
	int16_t drive = 0;  /* the current drive */
	uint64_t bytes;

	if (isalpha((unsigned char) root[0]) && root[1] == ':')
		drive = (int16_t) (toupper((unsigned char) root[0]) - 'A' + 1);
	if (Dfree(&info, drive) != 0)
		return 0;
	bytes = (uint64_t) info.b_free * info.b_secsiz * info.b_clsiz;
#else
	struct statvfs info;
	uint64_t bytes;

	if (statvfs(root, &info) != 0)
		return 0;
	bytes = (uint64_t) info.f_bavail * info.f_frsize;
#endif
	/* Callers may hold this in a signed long. */
	return bytes > INT32_MAX ? INT32_MAX : (uint32_t) bytes;
}

/* ---- Listings ---- */

/* Splits a name into DOS's blank-padded 8 and 3 character fields; false if it has no
 * 8.3 form. In a pattern, '*' fills the rest of its field with '?'. */
static bool dos_fields(const char *name, bool pattern, char fields[11])
{
	const char *dot = strchr(name, '.');
	size_t base = dot ? (size_t) (dot - name) : strlen(name);
	size_t ext = dot ? strlen(dot + 1) : 0;

	if (base > 8 || ext > 3 || (dot && strchr(dot + 1, '.')))
		return false;
	if (base == 0 && !pattern)
		return false;
	memset(fields, ' ', 11);
	for (int field = 0; field < 2; field++) {
		const char *src = field == 0 ? name : dot ? dot + 1 : "";
		size_t length = field == 0 ? base : ext;
		char *dst = fields + (field == 0 ? 0 : 8);
		size_t width = field == 0 ? 8 : 3;

		for (size_t i = 0; i < length; i++) {
			if (pattern && src[i] == '*') {
				memset(dst + i, '?', width - i);
				break;
			}
			dst[i] = (char) toupper((unsigned char) src[i]);
		}
	}
	return true;
}

static bool fields_match(const char pattern[11], const char name[11])
{
	for (int i = 0; i < 11; i++) {
		if (pattern[i] != '?' && pattern[i] != name[i])
			return false;
	}
	return true;
}

static void dos_name(const char fields[11], char out[13])
{
	int n = 0;

	for (int i = 0; i < 8 && fields[i] != ' '; i++)
		out[n++] = fields[i];
	if (fields[8] != ' ') {
		out[n++] = '.';
		for (int i = 8; i < 11 && fields[i] != ' '; i++)
			out[n++] = fields[i];
	}
	out[n] = '\0';
}

static void add_found(find_list *list, const char fields[11], uint32_t size)
{
	found_file *f;

	list->files = realloc(list->files, (list->count + 1) * sizeof *list->files);
	f = &list->files[list->count++];
	dos_name(fields, f->name);
	f->size = size;
}

static int by_name(const void *a, const void *b)
{
	return strcmp(((const found_file *) a)->name, ((const found_file *) b)->name);
}

/* Every file in the pattern's directory whose 8.3 name matches, listed up front so
 * deleting files during the listing is safe, and sorted by name, so every host lists them in
 * the same order. */
static find_list *list_matches(const char *dos_pattern)
{
	char dir_dos[PATH_SIZE];
	char dir[PATH_SIZE];
	char want[11], have[11];
	const char *wild = dos_pattern;
	find_list *list;
	DIR *d;
	struct dirent *entry;

	for (const char *p = dos_pattern; *p != '\0'; p++) {
		if (is_separator(*p))
			wild = p + 1;
	}
	snprintf(dir_dos, sizeof dir_dos, "%.*s", (int) (wild - dos_pattern), dos_pattern);
	if (!resolve(dir_dos, false, dir) || !is_dir(dir) || !dos_fields(wild, true, want))
		return NULL;

	list = calloc(1, sizeof *list);
#ifdef __MINT__
	if (__mint == 0) {
		/* GEMDOS's own search gives each file's size, where readdir would need a stat (a
		 * search of the folder) for each */
		char pattern[PATH_SIZE], dos[PATH_SIZE];
		_DTA dta, *old = Fgetdta();

		Fsetdta(&dta);
		if (join(pattern, sizeof pattern, dir, "*.*") && dos_path(pattern, dos)) {
			for (long r = Fsfirst(dos, FA_RDONLY | FA_HIDDEN | FA_SYSTEM); r == 0; r = Fsnext()) {
				if (dos_fields(dta.dta_name, false, have) && fields_match(want, have))
					add_found(list, have, (uint32_t) dta.dta_size);
			}
		}
		Fsetdta(old);
		d = NULL;
	} else
#endif
	d = opendir(dir);
	while (d != NULL && (entry = readdir(d)) != NULL) {
		char path[PATH_SIZE];
		uint32_t size;

		if (!dos_fields(entry->d_name, false, have) || !fields_match(want, have))
			continue;
		if (join(path, sizeof path, dir, entry->d_name) && path_kind(path, &size) == KIND_FILE)
			add_found(list, have, size);
	}
	if (d != NULL)
		closedir(d);
	if (list->count > 1)
		qsort(list->files, list->count, sizeof *list->files, by_name);
	return list;
}

void plat_find_close(plat_find *find)
{
	find_list *list = find->state;

	if (list != NULL) {
		free(list->files);
		free(list);
		find->state = NULL;
	}
}

int16_t plat_find_next(plat_find *find)
{
	find_list *list = find->state;

	if (list == NULL)
		return 0;
	if (list->next >= list->count) {
		plat_find_close(find);
		return 0;
	}
	/* As DOS findnext: a shorter name leaves the end of the last one after its NUL. */
	/* the whole field: saved games store all 13 bytes, so those after the name must not vary */
	memset(find->name, 0, sizeof find->name);
	strcpy(find->name, list->files[list->next].name);
	find->size = list->files[list->next].size;
	list->next++;
	return 1;
}

int16_t plat_find_first(const char *pattern, plat_find *find)
{
	find->state = list_matches(pattern);
	return plat_find_next(find);
}

/* ---- Start-up check ---- */

/* STATIC files every game needs. The game stops with its own message when one is missing, but
 * a wrong folder then reads 'File "STATIC\linkdep1." not found!', and an empty file can show as
 * an unrelated error (an empty SHAPES.VGA reads "Out of voodoo memory"). */
static const char *const required_files[] = {
#ifdef U7_SERPENT_ISLE
	"AMMO.DAT", "ARMOR.DAT", "EQUIP.DAT", "FACES.VGA", "FONTS.VGA", "GUMPS.VGA", "INITGAME.DAT",
	"LINKDEP1", "LINKDEP2", "MAINSHP.FLX", "MONSTERS.DAT", "OCCLUDE.DAT", "PALETTES.FLX",
	"PAPERDOL.VGA", "POINTERS.SHP", "READY.DAT", "SCHEDULE.DAT", "SHAPES.VGA", "SHPDIMS.DAT",
	"SPRITES.VGA", "TEXT.FLX", "TFA.DAT", "U7CHUNKS", "U7MAP", "USECODE", "WEAPONS.DAT",
	"WGTVOL.DAT", "WIHH.DAT", "XFORM.TBL",
#else
	"AMMO.DAT", "ARMOR.DAT", "ENDSHAPE.FLX", "EQUIP.DAT", "FACES.VGA", "FONTS.VGA", "GUMPS.VGA",
	"INITGAME.DAT", "LINKDEP1", "LINKDEP2", "MONSTERS.DAT", "OCCLUDE.DAT", "PALETTES.FLX",
	"POINTERS.SHP", "READY.DAT", "SCHEDULE.DAT", "SHAPES.VGA", "SHPDIMS.DAT", "SPRITES.VGA",
	"TEXT.FLX", "TFA.DAT", "U7CHUNKS", "U7MAP", "USECODE", "WEAPONS.DAT", "WGTVOL.DAT",
	"WIHH.DAT", "XFORM.TBL",
#endif
};

static void append(char *message, size_t size, const char *text)
{
	size_t n = strlen(message);

	if (n < size)
		snprintf(message + n, size - n, "%s", text);
}

#define WHERE_TO_PUT_GAME "Run " GAME_PRG " from the " GAME_NAME " folder, or point it at the " \
	"folder with --data <folder> or the U7_DATA variable."

bool files_check_data(char *message, size_t size)
{
	char missing[512] = "", empty[512] = "";
	int missing_count = 0, empty_count = 0;

	message[0] = '\0';
	if (!is_dir(root)) {
		snprintf(message, size, "The game data folder \"%s\" does not exist.\n\n%s", root,
			WHERE_TO_PUT_GAME);
		return false;
	}
	if (!plat_dir_exists("STATIC")) {
		snprintf(message, size, "\"%s\" is not " GAME_A_NAME " folder: it has no STATIC folder.\n\n%s",
			root, WHERE_TO_PUT_GAME);
		return false;
	}
	for (size_t i = 0; i < sizeof required_files / sizeof *required_files; i++) {
		char name[64];
		int16_t file;
		int32_t length;

		snprintf(name, sizeof name, "STATIC\\%s", required_files[i]);
		file = plat_file_open(name, PLAT_FILE_READ);
		if (file < 0) {
			append(missing, sizeof missing, missing_count++ ? ", " : "");
			append(missing, sizeof missing, required_files[i]);
			continue;
		}
		length = plat_file_length(file);
		plat_file_close(file);
		if (length <= 0) {
			append(empty, sizeof empty, empty_count++ ? ", " : "");
			append(empty, sizeof empty, required_files[i]);
		}
	}
	if (missing_count == 0 && empty_count == 0)
		return true;

	snprintf(message, size, "The " GAME_NAME " installation in \"%s\" is incomplete.\n", root);
	if (missing_count) {
		append(message, size, "\nMissing from STATIC: ");
		append(message, size, missing);
		append(message, size, "\n");
	}
	if (empty_count) {
		append(message, size, "\nEmpty in STATIC: ");
		append(message, size, empty);
		append(message, size, "\n");
	}
	append(message, size, "\nCopy these files from a complete installation.");
	return false;
}
