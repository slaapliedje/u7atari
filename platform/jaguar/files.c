/* The game's DOS files on the GameDrive's SD card, through its BIOS (gd.h).
 *
 * Names are the game's ("STATIC\\SHAPES.VGA"), resolved against the data folder on the card
 * (files_set_root); the card is FAT, so letter case does not matter. Listings, wildcards and the
 * made-up sound setup file follow platform/common/files.c, which the other backends use, so the
 * game sees the same files in the same order.
 *
 * The drive's BIOS can open, read, write, seek and list, but not delete, rename, truncate or make
 * a folder. So a removed file is one cut to nothing: a file of no bytes counts as not there (the
 * game never leaves an empty one). A rename is a copy, then a removal; a truncation copies the
 * part kept out and back. The card needs the folders the game uses (GAMEDAT) made beforehand.
 *
 * The drive keeps only a few files open (the libretro Virtual Jaguar's, 8), fewer than the game
 * does. So at most DRIVE_FILES are open on it from here, the one used longest ago closed to make
 * room; a game file not open on the drive is opened again at its place when next used. Two more
 * are left to stdio (syscalls.c).
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat.h"
#include "backend.h"
#include "game.h"
#include "gd.h"

#define PATH_SIZE 128
#define MAX_HANDLES 16
#define DRIVE_FILES 6
#define COPY_SIZE 4096
#define SCRATCH_NAME "U7CUT.$$$"

/* the drive's answer: a handle or 0 for success, or below 0 (as a signed word) */
#define FAILED(r) ((int16_t) (r) < 0)

typedef struct {
	bool open;
	bool live;                  /* open on the drive, as gd */
	uint16_t gd;
	uint16_t mode;              /* to open it again: never to make it again */
	uint32_t used;
	char *memory;               /* the made-up U7.CFG: read from here */
	int32_t pos, size;
	char path[PATH_SIZE];
} handle;

typedef struct {
	uint32_t size;
	uint16_t date, time;
	uint8_t attributes;
	char name[13];
} gd_info;

typedef struct {
	char name[13];
	uint32_t size;
} found_file;

typedef struct {
	found_file *files;
	uint32_t count;
	uint32_t next;
} find_list;

static char root[PATH_SIZE] = "U7";
static handle handles[MAX_HANDLES];
static int live_count;
static uint32_t use_clock;
static uint8_t copy_buffer[COPY_SIZE];

static void let_go(handle *h)
{
	if (h->live) {
		GD_FClose(h->gd);
		h->live = false;
		live_count--;
	}
}

/* Closes game files on the drive, those used longest ago first, until count more can open. */
static void make_room(int count)
{
	while (live_count + count > DRIVE_FILES) {
		handle *oldest = NULL;

		for (int i = 0; i < MAX_HANDLES; i++) {
			if (handles[i].live && (oldest == NULL || handles[i].used < oldest->used))
				oldest = &handles[i];
		}
		if (oldest == NULL)
			return;
		let_go(oldest);
	}
}

/* The file open on the drive, at its place: false when the drive will not have it. */
static bool use(handle *h)
{
	h->used = ++use_clock;
	if (h->memory != NULL || h->live)
		return true;
	make_room(1);
	if (FAILED(h->gd = GD_FOpen(h->path, h->mode)))
		return false;
	h->live = true;
	live_count++;
	if (h->pos > 0 && FAILED(GD_FSeek(h->gd, GD_FSEEK_SET, h->pos > h->size ? h->size : h->pos))) {
		let_go(h);
		return false;
	}
	return true;
}

const char *files_root(void)
{
	return root;
}

void files_set_root(const char *dir)
{
	snprintf(root, sizeof root, "%s", dir);
	while (root[0] != '\0' && (root[strlen(root) - 1] == '/' || root[strlen(root) - 1] == '\\'))
		root[strlen(root) - 1] = '\0';
}

/* The card path of a game name: the root, then the name with / for \, less any ".\". */
static bool resolve(const char *name, char path[PATH_SIZE])
{
	size_t n;

	while (name[0] == '.' && (name[1] == '\\' || name[1] == '/'))
		name += 2;
	if (strcmp(name, ".") == 0)
		name = "";
	n = (size_t) snprintf(path, PATH_SIZE, "%s%s%s", root, root[0] && name[0] ? "/" : "", name);
	if (n >= PATH_SIZE)
		return false;
	for (char *p = path; *p != '\0'; p++) {
		if (*p == '\\')
			*p = '/';
	}
	/* DOS reads "LINKDEP1." as LINKDEP1, with no extension; FAT does not: drop such dots */
	for (char *p = path, *out = path;; p++) {
		if (*p == '.' && (p[1] == '/' || p[1] == '\0') && p > path && p[-1] != '.'
			&& p[-1] != '/') {
			continue;
		}
		*out++ = *p;
		if (*p == '\0')
			break;
	}
	n = strlen(path);
	while (n > 0 && path[n - 1] == '/')
		path[--n] = '\0';
	return true;
}

enum { KIND_NONE, KIND_FILE, KIND_DIR };

static int path_kind(const char *path, uint32_t *size)
{
	gd_info info;
	uint16_t d;

	make_room(1);
	if (!FAILED(GD_FInfo(path, &info, GD_FINFO_SHORT_NAME))) {
		if (info.attributes & 0x10)
			return KIND_DIR;
		if (size != NULL)
			*size = info.size;
		return info.size > 0 ? KIND_FILE : KIND_NONE;
	}
	/* the root of the card, or a folder the drive does not describe: try to list it */
	make_room(1);
	if (FAILED(d = GD_DOpen(path)))
		return KIND_NONE;
	GD_DClose(d);
	return KIND_DIR;
}

static int16_t free_handle(void)
{
	for (int16_t i = 0; i < MAX_HANDLES; i++) {
		if (!handles[i].open)
			return i;
	}
	return -1;
}

static handle *get_handle(int16_t file)
{
	if (file < 0 || file >= MAX_HANDLES || !handles[file].open)
		return NULL;
	return &handles[file];
}

static int16_t open_path(const char *path, uint16_t mode, bool create)
{
	int16_t file = free_handle();
	handle *h;
	uint16_t gd;

	if (file < 0)
		return -1;
	make_room(1);
	if (FAILED(gd = GD_FOpen(path, mode)))
		return -1;
	h = &handles[file];
	memset(h, 0, sizeof *h);
	h->open = true;
	h->live = true;
	live_count++;
	h->gd = gd;
	h->mode = mode & (GD_FOPEN_READ | GD_FOPEN_WRITE);
	h->used = ++use_clock;
	h->size = create ? 0 : (int32_t) GD_FSize(gd);
	snprintf(h->path, sizeof h->path, "%s", path);
	return file;
}

/* ---- The sound setup: made up, as platform/common/files.c does ---- */

static bool is_sound_config(const char *name)
{
	return strcasecmp(name, GAME_SOUND_CONFIG) == 0
		|| (strncmp(name, ".\\", 2) == 0 && strcasecmp(name + 2, GAME_SOUND_CONFIG) == 0);
}

static int16_t open_sound_config(void)
{
	int16_t file = free_handle();
	handle *h;

	if (file < 0)
		return -1;
	h = &handles[file];
	memset(h, 0, sizeof *h);
	if ((h->memory = malloc(48)) == NULL)
		return -1;
	/* The card has no setup of its own: the speech line is the default one. */
	h->size = sprintf(h->memory, "%s\r\n%s\r\n", plat_midi_available() ? "r 330" : "a 388",
		GAME_DEFAULT_SPEECH);
	h->open = true;
	return file;
}

/* ---- Files ---- */

int16_t plat_file_open(const char *name, int16_t mode)
{
	char path[PATH_SIZE];

	if (is_sound_config(name))
		return open_sound_config();
	if (!resolve(name, path) || path_kind(path, NULL) != KIND_FILE)
		return -1;
	return open_path(path, (mode & PLAT_FILE_WRITE) ? GD_FOPEN_READ | GD_FOPEN_WRITE
		: GD_FOPEN_READ, false);
}

int16_t plat_file_create(const char *name)
{
	char path[PATH_SIZE];

	if (!resolve(name, path))
		return -1;
	return open_path(path, GD_FOPEN_READ | GD_FOPEN_WRITE | GD_FOPEN_CREATE_ALWAYS, true);
}

void plat_file_close(int16_t file)
{
	handle *h = get_handle(file);

	if (h == NULL)
		return;
	if (h->memory != NULL)
		free(h->memory);
	else
		let_go(h);
	h->open = false;
}

int32_t plat_file_read(int16_t file, void *buffer, int32_t count)
{
	handle *h = get_handle(file);

	if (h == NULL || count < 0)
		return -1;
	if (count > h->size - h->pos)
		count = h->size > h->pos ? h->size - h->pos : 0;
	if (count == 0)
		return 0;
	if (!use(h))
		return -1;
	if (h->memory != NULL)
		memcpy(buffer, h->memory + h->pos, (size_t) count);
	else if (FAILED(GD_FRead(h->gd, buffer, (uint32_t) count, GD_FREAD_CPU)))
		return -1;
	h->pos += count;
	return count;
}

int32_t plat_file_write(int16_t file, const void *buffer, int32_t count)
{
	handle *h = get_handle(file);

	if (h == NULL || h->memory != NULL || count < 0)
		return -1;
	if (count == 0)
		return 0;
	if (!use(h) || FAILED(GD_FWrite(h->gd, buffer, (uint32_t) count)))
		return -1;
	h->pos += count;
	if (h->pos > h->size)
		h->size = h->pos;
	return count;
}

int32_t plat_file_seek(int16_t file, int32_t offset, int16_t origin)
{
	handle *h = get_handle(file);
	int32_t at;

	if (h == NULL)
		return -1;
	at = origin == PLAT_SEEK_CUR ? h->pos + offset : origin == PLAT_SEEK_END ? h->size + offset
		: offset;
	if (at < 0)
		return -1;
	/* DOS seeks past the end; the drive is not asked to: the next write would leave a gap */
	if (h->live && FAILED(GD_FSeek(h->gd, GD_FSEEK_SET, at > h->size ? h->size : at)))
		return -1;
	h->pos = at;
	return at;
}

int32_t plat_file_length(int16_t file)
{
	handle *h = get_handle(file);

	return h == NULL ? -1 : h->size;
}

/* Copies count bytes from one open drive file to another, from where each stands. */
static bool copy_bytes(uint16_t from, uint16_t to, int32_t count)
{
	while (count > 0) {
		int32_t n = count < COPY_SIZE ? count : COPY_SIZE;

		if (FAILED(GD_FRead(from, copy_buffer, (uint32_t) n, GD_FREAD_CPU))
			|| FAILED(GD_FWrite(to, copy_buffer, (uint32_t) n)))
			return false;
		count -= n;
	}
	return true;
}

/* Copies the first count bytes of one file into another made anew. */
static bool copy_file(const char *from, const char *to, int32_t count)
{
	uint16_t in, out;
	bool ok;

	make_room(2);
	if (FAILED(in = GD_FOpen(from, GD_FOPEN_READ)))
		return false;
	if (FAILED(out = GD_FOpen(to, GD_FOPEN_WRITE | GD_FOPEN_CREATE_ALWAYS))) {
		GD_FClose(in);
		return false;
	}
	ok = copy_bytes(in, out, count);
	GD_FClose(in);
	GD_FClose(out);
	return ok;
}

static void make_empty(const char *path)
{
	uint16_t f;

	make_room(1);
	f = GD_FOpen(path, GD_FOPEN_WRITE | GD_FOPEN_CREATE_ALWAYS);

	if (!FAILED(f))
		GD_FClose(f);
}

int16_t plat_file_truncate(int16_t file)
{
	handle *h = get_handle(file);
	char scratch[PATH_SIZE];
	int32_t keep;

	if (h == NULL || h->memory != NULL)
		return 0;
	if (h->pos >= h->size)
		return 1;
	keep = h->pos;
	let_go(h);
	if (!resolve(SCRATCH_NAME, scratch) || !copy_file(h->path, scratch, keep)
		|| !copy_file(scratch, h->path, keep))
		return 0;
	make_empty(scratch);
	/* opened again, at the end, when next used */
	h->size = keep;
	return 1;
}

int16_t plat_file_exists(const char *name)
{
	char path[PATH_SIZE];

	return resolve(name, path) && path_kind(path, NULL) == KIND_FILE;
}

int16_t plat_dir_exists(const char *name)
{
	char path[PATH_SIZE];

	return resolve(name, path) && path_kind(path, NULL) == KIND_DIR;
}

/* The drive cannot make a folder: the card must have it already. As DOS, this fails when it does. */
int16_t plat_dir_create(const char *name)
{
	(void) name;
	return 0;
}

/* Nor remove one: its files are gone (cut to nothing) by now, which is what the game wants. */
int16_t plat_dir_remove(const char *name)
{
	return plat_dir_exists(name);
}

int16_t plat_file_remove(const char *name)
{
	char path[PATH_SIZE];

	if (!resolve(name, path) || path_kind(path, NULL) != KIND_FILE)
		return 0;
	make_empty(path);
	return 1;
}

int16_t plat_file_rename(const char *from, const char *to)
{
	char from_path[PATH_SIZE], to_path[PATH_SIZE];
	uint32_t size;

	if (!resolve(from, from_path) || !resolve(to, to_path)
		|| path_kind(from_path, &size) != KIND_FILE || path_kind(to_path, NULL) != KIND_NONE)
		return 0;
	if (!copy_file(from_path, to_path, (int32_t) size))
		return 0;
	make_empty(from_path);
	return 1;
}

uint32_t plat_disk_free(void)
{
	/* as platform/common/files.c gives under lockstep, and more than the game asks for */
	return UINT32_C(32) << 20;
}

/* ---- Listings: as platform/common/files.c ---- */

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

static int by_name(const void *a, const void *b)
{
	return strcmp(((const found_file *) a)->name, ((const found_file *) b)->name);
}

static find_list *list_matches(const char *dos_pattern)
{
	char dir_dos[PATH_SIZE], dir[PATH_SIZE], want[11], have[11];
	const char *wild = dos_pattern;
	find_list *list;
	gd_info info;
	uint16_t d;

	for (const char *p = dos_pattern; *p != '\0'; p++) {
		if (*p == '\\' || *p == '/')
			wild = p + 1;
	}
	snprintf(dir_dos, sizeof dir_dos, "%.*s", (int) (wild - dos_pattern), dos_pattern);
	if (!resolve(dir_dos, dir) || !dos_fields(wild, true, want))
		return NULL;
	make_room(1);
	if (FAILED(d = GD_DOpen(dir)))
		return NULL;
	list = calloc(1, sizeof *list);
	while (list != NULL && !FAILED(GD_DRead(d, &info, GD_FINFO_SHORT_NAME)) && info.name[0]) {
		found_file *f;

		/* folders, and files removed (cut to nothing), are not listed */
		if ((info.attributes & 0x10) || info.size == 0 || !dos_fields(info.name, false, have)
			|| !fields_match(want, have))
			continue;
		list->files = realloc(list->files, (list->count + 1) * sizeof *list->files);
		f = &list->files[list->count++];
		dos_name(have, f->name);
		f->size = info.size;
	}
	GD_DClose(d);
	if (list != NULL && list->count > 1)
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

/* ---- Start-up ---- */

void files_reset(void)
{
	for (int16_t i = 0; i < MAX_HANDLES; i++)
		plat_file_close(i);
}

bool files_check_data(char *message, size_t size)
{
	static const char *const needed[] = { "STATIC\\SHAPES.VGA", "STATIC\\U7CHUNKS",
		"STATIC\\USECODE", "STATIC\\U7MAP" };

	for (size_t i = 0; i < sizeof needed / sizeof *needed; i++) {
		if (!plat_file_exists(needed[i])) {
			snprintf(message, size, "%s not found in \"%s\" on the card.", needed[i], root);
			return false;
		}
	}
	if (!plat_dir_exists("GAMEDAT")) {
		snprintf(message, size, "The card needs a GAMEDAT folder in \"%s\".", root);
		return false;
	}
	message[0] = '\0';
	return true;
}
