/* Files through GEMDOS (via MiNTLib).
 *
 * The game asks for DOS names ("STATIC\\SHAPES.VGA") in any letter case; TOS file systems
 * are 8.3 and case-insensitive already, so names map almost directly onto the data
 * directory. All game data is little-endian and is read as-is; the game code converts.
 *
 * TODO: everything below.
 */
#include "plat.h"
#include "backend.h"

void files_init(const char *data_dir) { (void) data_dir; }
void files_reset(void) {}

int16_t plat_file_open(const char *name, int16_t mode) { (void) name; (void) mode; return -1; }
int16_t plat_file_create(const char *name) { (void) name; return -1; }
void plat_file_close(int16_t file) { (void) file; }
int32_t plat_file_read(int16_t file, void *buffer, int32_t count) { (void) file; (void) buffer; (void) count; return -1; }
int32_t plat_file_write(int16_t file, const void *buffer, int32_t count) { (void) file; (void) buffer; (void) count; return -1; }
int32_t plat_file_seek(int16_t file, int32_t offset, int16_t origin) { (void) file; (void) offset; (void) origin; return -1; }
int32_t plat_file_length(int16_t file) { (void) file; return -1; }
int16_t plat_file_truncate(int16_t file) { (void) file; return 0; }

int16_t plat_file_exists(const char *name) { (void) name; return 0; }
int16_t plat_dir_exists(const char *name) { (void) name; return 0; }
int16_t plat_dir_create(const char *name) { (void) name; return 0; }
int16_t plat_dir_remove(const char *name) { (void) name; return 0; }
int16_t plat_file_remove(const char *name) { (void) name; return 0; }
int16_t plat_file_rename(const char *from, const char *to) { (void) from; (void) to; return 0; }
uint32_t plat_disk_free(void) { return 0; }

int16_t plat_find_first(const char *pattern, plat_find *find) { (void) pattern; (void) find; return 0; }
int16_t plat_find_next(plat_find *find) { (void) find; return 0; }
void plat_find_close(plat_find *find) { (void) find; }
