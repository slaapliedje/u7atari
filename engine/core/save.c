/* Saved games (engine.h, e_save_game, e_load_game): the engine's own, in U7/ENGINE/SAVEn.DAT.
 *
 * "U7E2", then each part's sections in turn: a tag, the size of its records and how many, then
 * the records as they are in memory (this machine's byte order: a save is the machine's own).
 * The parts: the world (the clock, the NPCs), the usecode (its flags, the party), the held items,
 * the kept world items. A load first goes through the whole file, each section's tag, record
 * size and count checked against this build's; only when all fit is anything read into place,
 * then each part does what follows (the NPCs begun again, the party following, the world's
 * files read again).
 */
#include <stdio.h>
#include <string.h>

#include "engine.h"

#define MAGIC "U7E2"           /* (2: the held items keep their slots) */
#define SECTIONS 8

typedef int (*Sections)(int k, struct ESection *s);
static const Sections parts[] = { e_world_section, e_uc_section, e_held_section, e_kept_section };
#define PARTS (int) (sizeof parts / sizeof parts[0])

static struct { char tag[4]; uint32_t count; } got[SECTIONS];
static int ngot;

static void name_of(int slot, char *name, size_t n)
{
	snprintf(name, n, "SAVE%d.DAT", slot);
}

int e_save_game(int slot)
{
	char name[16];
	struct ESection s;

	name_of(slot, name, sizeof name);
	if (!e_save_open(name, 1))
		return 0;
	e_save_io(MAGIC, 4);
	for (int p = 0; p < PARTS; p++)
		for (int k = 0; parts[p](k, &s); k++) {
			uint32_t head[2] = { s.size, s.count };
			e_save_io(s.tag, 4);
			e_save_io(head, sizeof head);
			e_save_io(s.data, s.size * s.count);
		}
	return e_save_close();
}

uint32_t e_save_count(const char *tag)
{
	for (int k = 0; k < ngot; k++)
		if (!memcmp(got[k].tag, tag, 4))
			return got[k].count;
	return 0;
}

/* One pass through the file: checked (apply 0), or read into place (1). */
static int pass(int apply)
{
	struct ESection s;
	char magic[4], tag[4];
	uint32_t at = 4, head[2];

	if (!e_save_seek(0) || !e_save_io(magic, 4) || memcmp(magic, MAGIC, 4))
		return 0;
	ngot = 0;
	for (int p = 0; p < PARTS; p++)
		for (int k = 0; parts[p](k, &s); k++) {
			if (!e_save_io(tag, 4) || !e_save_io(head, sizeof head) || memcmp(tag, s.tag, 4)
				|| head[0] != s.size || head[1] > s.most || ngot == SECTIONS)
				return 0;
			memcpy(got[ngot].tag, tag, 4);
			got[ngot++].count = head[1];
			at += 4 + sizeof head;
			if (apply) {
				if (!e_save_io(s.data, s.size * head[1]))
					return 0;
			}
			at += s.size * head[1];
			if (!apply && !e_save_seek(at))
				return 0;
		}
	return !apply ? (int32_t) at == e_save_length() : 1;
}

int e_load_game(int slot, uint32_t blanks)
{
	char name[16];

	name_of(slot, name, sizeof name);
	if (!e_save_open(name, 0))
		return 0;
	if (!pass(0)) {
		e_save_close();
		return 0;
	}
	int ok = pass(1);
	ok = e_save_close() && ok;
	if (!ok)
		return 0;               /* (it fitted, yet could not be read: the game as it is, mixed) */
	e_world_loaded(blanks);
	e_uc_loaded();
	e_held_loaded();
	e_kept_loaded();
	e_world_flush();
	return 1;
}

int e_save_exists(int slot)
{
	char name[16];

	name_of(slot, name, sizeof name);
	if (!e_save_open(name, 0))
		return 0;
	int ok = pass(0);
	e_save_close();
	return ok;
}

/* ---- the menu (the Jaguar's Pause): save in a slot, load one, go on. Up and down pick the
 * line, left and right the slot (1-3), A does it, B goes on. What came of it is said under. ---- */

#define MENU_SLOTS 3
static int menu_on, menu_line, menu_slot = 1;
static const char *menu_said;
static uint32_t menu_changes;

void e_menu_open(void)
{
	menu_on = 1;
	menu_line = 0;
	menu_said = NULL;
	menu_changes++;
}

int e_menu_active(void)
{
	return menu_on;
}

uint32_t e_menu_changes(void)
{
	return menu_changes;
}

int e_menu_key(int key, uint32_t blanks)
{
	menu_changes++;
	switch (key) {
	case E_KEY_UP:
		menu_line = (menu_line + 2) % 3;
		break;
	case E_KEY_DOWN:
		menu_line = (menu_line + 1) % 3;
		break;
	case E_KEY_LEFT:
		menu_slot = menu_slot > 1 ? menu_slot - 1 : MENU_SLOTS;
		break;
	case E_KEY_RIGHT:
		menu_slot = menu_slot < MENU_SLOTS ? menu_slot + 1 : 1;
		break;
	case E_KEY_BACK:
		menu_on = 0;
		break;
	case E_KEY_NEXT:
		if (menu_line == 2)
			menu_on = 0;
		else if (menu_line == 0)
			menu_said = e_save_game(menu_slot) ? "Saved." : "It could not be saved.";
		else if (!e_save_exists(menu_slot))
			menu_said = "Nothing is saved there.";
		else if (e_load_game(menu_slot, blanks)) {
			menu_on = 0;
			return 1;
		} else
			menu_said = "It could not be loaded.";
		break;
	}
	return 0;
}

void e_menu_draw(struct ESurface *frame)
{
	char line[3][32];
	int lh = e_line_height(), x = 100, y = 70;

	if (!menu_on)
		return;
	snprintf(line[0], sizeof line[0], "Save game %d", menu_slot);
	snprintf(line[1], sizeof line[1], "Load game %d", menu_slot);
	snprintf(line[2], sizeof line[2], "Continue");
	eb_clear(frame, (int16_t) (x - 12), (int16_t) (y - lh - 8), 136, (int16_t) (lh * 5 + 12));
	for (int k = 0; k < 3; k++) {
		e_draw_text(frame, (int16_t) x, (int16_t) (y + k * lh), line[k], 32);
		if (k == menu_line)
			e_draw_pointer(frame, 0, (int16_t) (x + e_text_width(line[k]) + 6), (int16_t) (y + k * lh - 2));
	}
	if (menu_said != NULL)
		e_draw_text(frame, (int16_t) x, (int16_t) (y + 3 * lh + 2), menu_said, 32);
}
