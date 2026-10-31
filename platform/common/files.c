/* The game's DOS files, found in the data directory whatever their letter case.
 *
 * Follows upstream's matoya backend (platform/matoya/files.c) on MiNTLib's POSIX layer, which
 * works under plain TOS and under MiNT. TOS FAT drives are 8.3 and case-insensitive already,
 * so most lookups succeed on the first stat; the directory scan only matters on a
 * case-sensitive MiNT file system.
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
#include <sys/stat.h>
#include <unistd.h>

#ifdef __MINT__
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

/* A handle is an open file, or a read-only block of memory (the made-up U7.CFG). */
typedef struct {
	FILE *file;
	int last;
	char *memory;
	int32_t memory_size;
	int32_t memory_pos;
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

static bool exists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0;
}

static bool is_file(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool is_dir(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool join(char *path, size_t size, const char *dir, const char *name)
{
	size_t n = strlen(dir);
	int written = snprintf(path, size, "%s%s%s", dir,
		n > 0 && is_separator(dir[n - 1]) ? "" : "/", name);

	return written > 0 && (size_t) written < size;
}

/* Finds the name on disk of one entry of dir, ignoring case. */
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
 * spelt in upper case. */
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
		if (!real_name(path, part, real)) {
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

static void release(handle *h)
{
	if (h->file != NULL)
		fclose(h->file);
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
		if (handles[i].file == NULL && handles[i].memory == NULL)
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
	if (handles[file].file == NULL && handles[file].memory == NULL)
		return NULL;
	return &handles[file];
}

/* The handle's stream, ready for a read or a write. C needs a seek between the two. */
static FILE *use_stream(handle *h, int op)
{
	if (op != LAST_NONE && h->last != LAST_NONE && h->last != op)
		fseek(h->file, 0, SEEK_CUR);
	h->last = op;
	return h->file;
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

	if (is_sound_config(name))
		return open_sound_config(name);
	if (!resolve(name, false, path) || !is_file(path))
		return -1;
	return add_file(fopen(path, (mode & PLAT_FILE_WRITE) ? "r+b" : "rb"));
}

int16_t plat_file_create(const char *name)
{
	char path[PATH_SIZE];

	if (!resolve(name, true, path) || is_dir(path))
		return -1;
	return add_file(fopen(path, "w+b"));
}

void plat_file_close(int16_t file)
{
	handle *h = get_handle(file);

	if (h != NULL)
		release(h);
}

int32_t plat_file_read(int16_t file, void *buffer, int32_t count)
{
	handle *h = get_handle(file);
	FILE *f;
	size_t n;

	if (h == NULL || count < 0)
		return -1;
	if (h->memory != NULL) {
		if (count > h->memory_size - h->memory_pos)
			count = h->memory_size - h->memory_pos;
		memcpy(buffer, h->memory + h->memory_pos, (size_t) count);
		h->memory_pos += count;
		return count;
	}
	f = use_stream(h, LAST_READ);
	n = fread(buffer, 1, (size_t) count, f);
	return n == 0 && ferror(f) ? -1 : (int32_t) n;
}

int32_t plat_file_write(int16_t file, const void *buffer, int32_t count)
{
	handle *h = get_handle(file);
	size_t n;

	if (h == NULL || h->memory != NULL || count < 0)
		return -1;
	n = fwrite(buffer, 1, (size_t) count, use_stream(h, LAST_WRITE));
	return n == 0 && count > 0 ? -1 : (int32_t) n;
}

int32_t plat_file_seek(int16_t file, int32_t offset, int16_t origin)
{
	handle *h = get_handle(file);
	int whence = origin == PLAT_SEEK_CUR ? SEEK_CUR : origin == PLAT_SEEK_END ? SEEK_END : SEEK_SET;
	FILE *f;

	if (h == NULL)
		return -1;
	if (h->memory != NULL) {
		int32_t base = whence == SEEK_CUR ? h->memory_pos : whence == SEEK_END ? h->memory_size : 0;

		if (base + offset < 0)
			return -1;
		h->memory_pos = base + offset;
		return h->memory_pos;
	}
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

	if (h == NULL || h->memory != NULL)
		return 0;
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

	return resolve(name, false, path) && is_dir(path) && rmdir(path) == 0;
}

int16_t plat_file_remove(const char *name)
{
	char path[PATH_SIZE];

	return resolve(name, false, path) && is_file(path) && remove(path) == 0;
}

int16_t plat_file_rename(const char *from, const char *to)
{
	char from_path[PATH_SIZE];
	char to_path[PATH_SIZE];

	/* DOS refused to rename over an existing file. */
	if (!resolve(from, false, from_path) || !resolve(to, true, to_path) || exists(to_path))
		return 0;
	return rename(from_path, to_path) == 0;
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
	d = opendir(dir);
	while (d != NULL && (entry = readdir(d)) != NULL) {
		char path[PATH_SIZE];
		struct stat st;

		if (!dos_fields(entry->d_name, false, have) || !fields_match(want, have))
			continue;
		if (join(path, sizeof path, dir, entry->d_name) && stat(path, &st) == 0
			&& S_ISREG(st.st_mode))
			add_found(list, have, (uint32_t) st.st_size);
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
