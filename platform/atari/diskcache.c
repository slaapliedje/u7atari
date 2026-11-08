/* More sector buffers for GEMDOS while the game runs, under plain TOS.
 *
 * GEMDOS keeps disk sectors in two short lists of buffers, one for the FAT and one for
 * folders and data (_bufl, $4B2): two buffers each in EmuTOS, and few in Atari's TOS. Each
 * cluster it hands out to a growing file comes from a search of the FAT from its start, and
 * each seek walks a file's clusters through it; with two buffers, every FAT sector read is
 * soon thrown out and read again. A new game writes 173 files, and on a fairly full disk that
 * was tens of sector reads for each cluster. Cache programs (CACHEnnn.PRG, HDDRIVER's own)
 * add buffers to these lists, and the BCB fields they set are part of the TOS API: 20 bytes,
 * the link at 0, the drive at 4 (-1: empty), the buffer at 16. GEMDOS takes an empty buffer
 * before reusing the least recently used one, so new ones go to work at once.
 *
 * They are taken out again when the game ends, after a full Fclose, which writes every dirty
 * buffer back (EmuTOS's ixclose and the DRI GEMDOS it comes from both do), so nothing is
 * left only in memory that is about to be freed. MiNT has a cache of its own and no use for
 * these lists, so there nothing is done.
 */
#include <mint/osbind.h>
#include <mint/sysvars.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "plat.h"
#include "backend.h"

/* buffers added to each list, at most, and the memory they may take in all */
#define LIST_BUFFERS 64
#define CACHE_MEMORY (1024 * INT32_C(1024))
/* when no AHDI-style driver says (pun_info): the largest logical sector TOS allows */
#define LARGEST_SECTOR 16384

typedef struct bcb {
	struct bcb *link;
	int16_t drive;              /* -1: holds nothing */
	int16_t private1[3];        /* GEMDOS's own: record, type, dirty, as each TOS lays them out */
	void *private2;             /* GEMDOS's own: the drive's media descriptor */
	uint8_t *buffer;
} bcb;

_Static_assert(sizeof(bcb) == 20, "a BCB is 20 bytes in the TOS API");

#define BUFFER_LISTS ((bcb **) 0x4b2)
#define PUN_INFO_POINTER (*(uint8_t **) 0x516)

extern int __mint;              /* MiNTLib: the MiNT version, 0 under plain TOS */

static uint8_t *memory;
static uint32_t memory_size;

/* The largest logical sector of any drive, from the hard disk driver's pun_info when it has
 * the AHDI 3 fields (EmuTOS fills them too). */
static uint32_t largest_sector(void)
{
	uint8_t *pun = PUN_INFO_POINTER;

	if (pun != NULL && *(uint32_t *) (pun + 82) == 0x41484449       /* 'AHDI' */
		&& *(uint8_t **) (pun + 86) == pun + 82 && *(uint16_t *) (pun + 90) >= 0x300) {
		uint16_t size = *(uint16_t *) (pun + 92);

		if (size >= 512 && size <= LARGEST_SECTOR)
			return size;
	}
	return LARGEST_SECTOR;
}

/* Called in supervisor mode. */
void disk_cache_init(void)
{
	uint32_t sector = largest_sector();
	uint32_t each = CACHE_MEMORY / 2 / (sizeof(bcb) + sector), count;
	bcb *blocks;
	uint8_t *buffers;

	if (__mint != 0 || BUFFER_LISTS[0] == NULL || BUFFER_LISTS[1] == NULL)
		return;
	if (each > LIST_BUFFERS)
		each = LIST_BUFFERS;
	count = 2 * each;
	memory_size = count * (sizeof(bcb) + sector);
	/* fast RAM if there is some: GEMDOS reads into data buffers anywhere already */
	memory = (uint8_t *) Mxalloc(memory_size, 3);
	if ((long) memory <= 0) {
		memory = NULL;
		return;
	}
	blocks = (bcb *) memory;
	buffers = memory + count * sizeof(bcb);
	for (uint32_t i = 0; i < count; i++) {
		memset(&blocks[i], 0, sizeof blocks[i]);
		blocks[i].drive = -1;
		blocks[i].buffer = buffers + i * sector;
		blocks[i].link = i + 1 == each || i + 1 == count ? NULL : &blocks[i + 1];
	}
	/* on the end of each list (TOS documents differ on which list is the FAT's; both get
	 * the same) */
	for (int list = 0; list < 2; list++) {
		bcb **end = &BUFFER_LISTS[list];

		while (*end != NULL)
			end = &(*end)->link;
		*end = &blocks[list * each];
	}
}

static bool ours(const bcb *b)
{
	return (const uint8_t *) b >= memory && (const uint8_t *) b < memory + memory_size;
}

/* Has GEMDOS write back its dirty buffers: a full close of a file does that. */
static void write_back(void)
{
	_DTA dta, *old = Fgetdta();
	char path[300];
	long found, file;

	Fsetdta(&dta);
	snprintf(path, sizeof path, "%s\\*.*", files_root());
	found = Fsfirst(path, 0);
	Fsetdta(old);
	if (found == 0) {
		snprintf(path, sizeof path, "%s\\%s", files_root(), dta.dta_name);
		if ((file = Fopen(path, 0)) >= 0)
			Fclose((int16_t) file);
	}
}

/* Called in supervisor mode, once the game's files are closed. */
void disk_cache_shutdown(void)
{
	if (memory == NULL)
		return;
	write_back();
	for (int list = 0; list < 2; list++) {
		bcb **at = &BUFFER_LISTS[list];

		while (*at != NULL) {
			if (ours(*at))
				*at = (*at)->link;
			else
				at = &(*at)->link;
		}
	}
	Mfree(memory);
	memory = NULL;
}
